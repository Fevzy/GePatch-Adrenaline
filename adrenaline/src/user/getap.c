/*
  GE tap, Vita side: follows the GE lists the PSP submits (recorded by
  ge_tap.prx, see getap_shared.h) by reading them from PSP memory, and reports
  what a Vita-side renderer would have to process per frame. Step 1 of
  rendering PSP frames with the Vita GPU: it only reads and measures.

  PSP RAM is shared with the Vita side; VRAM is the GE's own memory and only
  the pages the PSP side forwards are visible here.
*/

#include <psp2/ctrl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/dmac.h>
#include <psp2/compat.h>
#include <stddef.h>
#include <string.h>

#include "main.h"
#include "gerender.h"
#include "../getap_shared.h"

#define TAP_LISTS 64
#define TAP_STACK 32
#define TAP_PAGE 0x1000
#define TAP_BUDGET 2000000 /* commands per walk, against garbage lists */
#define TAP_NOP_RUN 4096   /* that many NOPs in a row: not a list */
#define TAP_REPORT_US 5000000

#define GE_CMD_PRIM 0x04
#define GE_CMD_BEZIER 0x05
#define GE_CMD_SPLINE 0x06
#define GE_CMD_JUMP 0x08
#define GE_CMD_BJUMP 0x09
#define GE_CMD_CALL 0x0A
#define GE_CMD_RET 0x0B
#define GE_CMD_END 0x0C
#define GE_CMD_SIGNAL 0x0E
#define GE_CMD_FINISH 0x0F
#define GE_CMD_BASE 0x10
#define GE_CMD_OFFSETADDR 0x13
#define GE_CMD_ORIGIN 0x14

typedef struct {
	uint32_t qid, active, done, list;
	uint32_t pc, stall, offset, prev, prev_data;
	uint32_t sp, stack[TAP_STACK * 2];
} TapList;

typedef struct {
	uint32_t frames, lists, commands, prims, vertices, splines;
	uint32_t walk_us, inval_us, invals, budget_hit, bad_address, unknown_qid, ring_lost;
	uint32_t max_commands, max_walk_us, max_lag;
	uint32_t ends, stalls, nop_runs;
	uint32_t vram_commands, vram_misses, vram_dma_reads;
	uint32_t dmas, dma_bytes, dma_us;
} TapStats;

/* The first addresses a walk could not follow in a report window. */
#define TAP_SAMPLES 4
typedef struct {
	uint32_t list, pc, from, op, prev;
} TapSample;
static TapSample samples[TAP_SAMPLES];
static uint32_t sample_count;

static TapList tap_lists[TAP_LISTS];
static TapStats window, frame;
static uint32_t cached_page;
static volatile uint32_t *cached_ptr; /* Vita view of cached_page */
/* The first VRAM reads in a report window that found no copy. */
static uint32_t miss_samples[TAP_SAMPLES][3], miss_count;

/* BASE is a GE register: it carries over from list to list. */
static uint32_t ge_base;

/* VRAM forwarding (see getap_shared.h) */
static GetapRing *ring;
static uint32_t ring_addr;
static uint32_t vram_request[GETAP_VRAM_PAGES / 32], requests_dirty;
/* VRAM as the game DMAs into it, rebuilt from the same sources in RAM. */
static uint8_t *dma_vram;
static uint32_t dma_valid[GETAP_VRAM_PAGES / 32];

/* VRAM: 0x04000000, 2 MiB, mirrored up to 0x04800000. */
static inline int tapIsVram(uint32_t addr) {
	addr &= 0x0FFFFFFF;
	return addr >= 0x04000000 && addr < 0x04800000;
}

/* PSP RAM (0x08000000-0x0BFFFFFF) and VRAM. */
static inline int tapValid(uint32_t addr) {
	addr &= 0x0FFFFFFF;
	return ((addr >= 0x08000000 && addr < 0x0C000000) || tapIsVram(addr)) && !(addr & 3);
}

/* The Vita view of PSP memory, made current for reading. Never a plain
   invalidate (what ScePspemuConvertAddress does for aligned ranges): ScePspemu
   writes into PSP RAM through its cache too (files, save data), and dropping
   its dirty lines corrupts them. Write back, then invalidate. */
static const void *pspRead(uint32_t addr, uint32_t size) {
	uint32_t p = (uint32_t)ScePspemuConvertAddress(0x80000000 | addr, KERMIT_INPUT_MODE, 0);
	if (p < 0x10000)
		return NULL;
	(void)size; /* the view without cache maintenance reads PSP memory as it is (slow, uncached) */
	return (const void *)p;
}

/* Reading from PSP RAM: page by page, made current once per page per walk
   step that enters it. VRAM reads go to the page's forwarded copy; a
   page not forwarded yet is requested and reads as END, which ends the list
   for this frame. */
static inline uint32_t tapRead(uint32_t addr) {
	addr &= 0x0FFFFFFF;
	if (tapIsVram(addr)) {
		uint32_t offset = addr & 0x1FFFFF, page = offset / GETAP_VRAM_PAGE;
		if (dma_valid[page / 32] & (1u << (page % 32))) {
			frame.vram_dma_reads++;
			return *(uint32_t *)(dma_vram + (offset & ~3));
		}
		uint32_t slot = ring->vram_slot[page];
		if (!slot) {
			if (!(vram_request[page / 32] & (1u << (page % 32)))) {
				vram_request[page / 32] |= 1u << (page % 32);
				requests_dirty = 1;
			}
			frame.vram_misses++;
			if (miss_count < TAP_SAMPLES) {
				miss_samples[miss_count][0] = addr;
				miss_samples[miss_count][1] = ring->pool_full;
				miss_samples[miss_count][2] = ((vram_request[page / 32] >> (page % 32)) & 1) |
					(((ring->vram_request[page / 32] >> (page % 32)) & 1) << 1) | (ring->reserved[0] << 2);
				miss_count++;
			}
			return GE_CMD_END << 24;
		}
		addr = ring->pool + (slot - 1) * GETAP_VRAM_PAGE + (offset & (GETAP_VRAM_PAGE - 1));
	}
	uint32_t page = addr & ~(TAP_PAGE - 1);
	if (page != cached_page) {
		cached_ptr = (volatile uint32_t *)pspRead(page, TAP_PAGE);
		frame.invals++;
		cached_page = page;
	}
	if ((uint32_t)cached_ptr < 0x10000) /* no translation: read as NOPs */
		return 0;
	return cached_ptr[(addr - page) >> 2];
}

/* Copies out of PSP memory. sceDmacMemcpy was tried: its per-call cost made
   reading a heavy frame 3x slower than the CPU's own copy. */
static inline void pspCopy(void *dst, const void *from, uint32_t size) {
	memcpy(dst, from, size);
}

int tapReadBlock(uint32_t addr, void *dst, uint32_t size) {
	addr &= 0x0FFFFFFF;
	if (!size)
		return 1;
	if (addr >= 0x08000000 && addr + size <= 0x0C000000) {
		const void *from = pspRead(addr, size);
		if (!from)
			return 0;
		pspCopy(dst, from, size);
		return 1;
	}
	if (!tapIsVram(addr) || !ring)
		return 0;
	uint8_t *out = dst;
	while (size) { /* page by page: from DMA copies or forwarded pages */
		uint32_t offset = addr & 0x1FFFFF, page = offset / GETAP_VRAM_PAGE, in = offset % GETAP_VRAM_PAGE;
		uint32_t n = GETAP_VRAM_PAGE - in < size ? GETAP_VRAM_PAGE - in : size;
		if (dma_valid[page / 32] & (1u << (page % 32))) {
			memcpy(out, dma_vram + offset, n);
		} else if (ring->vram_slot[page]) {
			const void *from = pspRead(ring->pool + (ring->vram_slot[page] - 1) * GETAP_VRAM_PAGE + in, n);
			if (!from)
				return 0;
			pspCopy(out, from, n);
		} else {
			if (!(vram_request[page / 32] & (1u << (page % 32)))) {
				vram_request[page / 32] |= 1u << (page % 32);
				requests_dirty = 1;
			}
			return 0;
		}
		out += n;
		addr += n;
		size -= n;
	}
	return 1;
}

/* A DMA into VRAM the game made: the same bytes from its RAM source. */
static void tapDma(uint32_t dst, uint32_t src, uint32_t size) {
	uint32_t started = sceKernelGetProcessTimeLow();
	uint32_t offset = dst & 0x1FFFFF;
	if (!dma_vram || offset + size > 0x200000 || !size)
		return;
	src &= 0x0FFFFFFF;
	if (tapIsVram(src)) { /* VRAM to VRAM: only from what we have */
		uint32_t from = src & 0x1FFFFF;
		if (from + size > 0x200000)
			return;
		memmove(dma_vram + offset, dma_vram + from, size);
	} else if (src >= 0x08000000 && src + size <= 0x0C000000) {
		const void *from = pspRead(src, size);
		if (!from)
			return;
		memcpy(dma_vram + offset, from, size);
	} else {
		return;
	}
	for (uint32_t page = offset / GETAP_VRAM_PAGE; page <= (offset + size - 1) / GETAP_VRAM_PAGE; page++)
		dma_valid[page / 32] |= 1u << (page % 32);
	frame.dmas++;
	frame.dma_bytes += size;
	frame.dma_us += sceKernelGetProcessTimeLow() - started;
}

static TapList *tapFind(uint32_t qid, int create) {
	TapList *free_slot = NULL;
	for (int i = 0; i < TAP_LISTS; i++) {
		if (tap_lists[i].active && tap_lists[i].qid == qid)
			return &tap_lists[i];
		if (!tap_lists[i].active && !free_slot)
			free_slot = &tap_lists[i];
	}
	if (!create)
		return NULL;
	if (!free_slot) { /* all busy: recycle the first finished one */
		for (int i = 0; i < TAP_LISTS && !free_slot; i++)
			if (tap_lists[i].done)
				free_slot = &tap_lists[i];
		if (!free_slot)
			free_slot = &tap_lists[0];
	}
	return free_slot;
}

/* Follows one list from its cursor to its stall address or its end, the way
   the GE does: JUMP/CALL/RET relative to BASE and the offset, SIGNAL+END
   jumps, FINISH+END ends. */
static void tapWalk(TapList *l) {
	uint32_t budget = TAP_BUDGET, started = sceKernelGetProcessTimeLow();
	uint32_t commands = 0, from = 0, nops = 0;
	cached_page = 0xFFFFFFFF;
	while (!l->done && budget--) {
		if (l->stall && l->pc == l->stall) {
			frame.stalls++;
			break;
		}
		if (!tapValid(l->pc)) {
			if (sample_count < TAP_SAMPLES) {
				TapSample *t = &samples[sample_count++];
				t->list = l->list;
				t->pc = l->pc;
				t->from = from;
				t->op = from ? tapRead(from) : 0;
				t->prev = l->prev;
			}
			frame.bad_address++;
			l->done = 1;
			break;
		}
		if (tapIsVram(l->pc))
			frame.vram_commands++;
		uint32_t op = tapRead(l->pc), cmd = op >> 24, data = op & 0xFFFFFF;
		uint32_t next = l->pc + 4;
		commands++;
		grCommand(cmd, data, l->offset);
		if (op == 0) {
			if (++nops == TAP_NOP_RUN) {
				frame.nop_runs++;
				l->done = 1;
				break;
			}
		} else {
			nops = 0;
		}
		switch (cmd) {
			case GE_CMD_PRIM:
				frame.prims++;
				frame.vertices += data & 0xFFFF;
				break;
			case GE_CMD_BEZIER:
			case GE_CMD_SPLINE:
				frame.splines++;
				break;
			case GE_CMD_BASE:
				ge_base = (data << 8) & 0x0F000000;
				break;
			case GE_CMD_OFFSETADDR:
				l->offset = data << 8;
				break;
			case GE_CMD_ORIGIN:
				l->offset = l->pc;
				break;
			case GE_CMD_JUMP:
				next = ((ge_base | data) & 0x0FFFFFFC) + l->offset;
				break;
			case GE_CMD_BJUMP: /* bounding box result unknown here: not taken */
				break;
			case GE_CMD_CALL:
				if (l->sp < TAP_STACK) {
					l->stack[l->sp * 2] = next;
					l->stack[l->sp * 2 + 1] = l->offset;
					l->sp++;
					next = ((ge_base | data) & 0x0FFFFFFC) + l->offset;
				}
				break;
			case GE_CMD_RET:
				if (l->sp) {
					l->sp--;
					next = l->stack[l->sp * 2];
					l->offset = l->stack[l->sp * 2 + 1];
				}
				break;
			case GE_CMD_END:
				if (l->prev == GE_CMD_SIGNAL) {
					uint32_t behaviour = (l->prev_data >> 16) & 0xFF;
					uint32_t target = ((l->prev_data & 0xFFFF) << 16) | (data & 0xFFFF);
					if (behaviour == 0x10) {
						next = target & 0x0FFFFFFC;
					} else if (behaviour == 0x11 && l->sp < TAP_STACK) {
						l->stack[l->sp * 2] = next;
						l->stack[l->sp * 2 + 1] = l->offset;
						l->sp++;
						next = target & 0x0FFFFFFC;
					} else if (behaviour == 0x12 && l->sp) {
						l->sp--;
						next = l->stack[l->sp * 2];
						l->offset = l->stack[l->sp * 2 + 1];
					}
				} else {
					l->done = 1; /* after FINISH, and the GE stops on a bare END too */
					frame.ends++;
				}
				break;
		}
		l->prev = cmd;
		l->prev_data = data;
		from = l->pc;
		l->pc = next & 0x0FFFFFFF;
	}
	if (budget == (uint32_t)-1) {
		frame.budget_hit++;
		l->done = 1;
	}
	frame.commands += commands;
	frame.walk_us += sceKernelGetProcessTimeLow() - started;
}

static void tapStart(uint32_t qid, uint32_t list, uint32_t stall) {
	TapList *l = tapFind(qid, 1);
	memset(l, 0, sizeof(*l));
	l->qid = qid;
	l->active = 1;
	frame.lists++;
	l->pc = list & 0x0FFFFFFF;
	l->list = l->pc;
	l->stall = stall & 0x0FFFFFFF;
	tapWalk(l);
}

static void tapStall(uint32_t qid, uint32_t stall) {
	TapList *l = tapFind(qid, 0);
	if (!l) {
		frame.unknown_qid++;
		return;
	}
	l->stall = stall & 0x0FFFFFFF;
	tapWalk(l);
}

static void tapEndFrame(void) {
	if (frame.commands > window.max_commands)
		window.max_commands = frame.commands;
	if (frame.walk_us > window.max_walk_us)
		window.max_walk_us = frame.walk_us;
	window.frames++;
	window.lists += frame.lists;
	window.commands += frame.commands;
	window.prims += frame.prims;
	window.vertices += frame.vertices;
	window.splines += frame.splines;
	window.walk_us += frame.walk_us;
	window.inval_us += frame.inval_us;
	window.invals += frame.invals;
	window.budget_hit += frame.budget_hit;
	window.bad_address += frame.bad_address;
	window.unknown_qid += frame.unknown_qid;
	window.ends += frame.ends;
	window.stalls += frame.stalls;
	window.nop_runs += frame.nop_runs;
	window.vram_commands += frame.vram_commands;
	window.vram_misses += frame.vram_misses;
	window.vram_dma_reads += frame.vram_dma_reads;
	window.dmas += frame.dmas;
	window.dma_bytes += frame.dma_bytes;
	window.dma_us += frame.dma_us;
	memset(&frame, 0, sizeof(frame));
	/* lists of finished frames are gone; keep ones still open */
	for (int i = 0; i < TAP_LISTS; i++)
		if (tap_lists[i].done)
			tap_lists[i].active = 0;
}

static void tapReport(uint32_t elapsed_us) {
	static uint32_t last_copy_us, last_copied;
	uint32_t f = window.frames ? window.frames : 1, pages = 0, dma_pages = 0;
	uint32_t copy_us = ring->copy_us - last_copy_us, copied = ring->copied - last_copied;
	last_copy_us = ring->copy_us;
	last_copied = ring->copied;
	for (uint32_t i = 0; i < GETAP_VRAM_PAGES; i++)
		pages += ring->vram_slot[i] != 0;
	for (uint32_t i = 0; i < GETAP_VRAM_PAGES; i++) /* no libgcc here: no popcount */
		dma_pages += (dma_valid[i / 32] >> (i % 32)) & 1;
	sceClibPrintf("[gt] ms=%u frames=%u fps10=%u lists/f=%u cmds/f=%u max_cmds=%u prims/f=%u verts/f=%u splines=%u "
		"walk_us/f=%u max_walk_us=%u inval_us/f=%u invals/f=%u budget=%u bad=%u unknown_qid=%u ring_lost=%u max_lag=%u "
		"psp_lost=%u ends=%u stalls=%u nop_runs=%u vram_cmds/f=%u vram_misses=%u vram_pages=%u/%u pool_full=%u "
		"psp_copy_us/f=%u copied/f=%u dma_hooked=%u dmas=%u dma_kb=%u dma_us=%u dma_pages=%u vram_dma_reads/f=%u\n",
		elapsed_us / 1000, window.frames, (uint32_t)((uint64_t)window.frames * 10000000ull / elapsed_us),
		window.lists / f, window.commands / f, window.max_commands, window.prims / f, window.vertices / f,
		window.splines, window.walk_us / f, window.max_walk_us, window.inval_us / f, window.invals / f,
		window.budget_hit, window.bad_address, window.unknown_qid, window.ring_lost, window.max_lag,
		ring->lost, window.ends, window.stalls, window.nop_runs, window.vram_commands / f, window.vram_misses,
		pages, ring->pool_pages, ring->pool_full, copy_us / f, copied / f,
		ring->dma_hooked, window.dmas, window.dma_bytes >> 10, window.dma_us, dma_pages, window.vram_dma_reads / f);
	for (uint32_t i = 0; i < sample_count; i++)
		sceClibPrintf("[gt] bad list=%08x pc=%08x from=%08x op=%08x prev=%02x\n",
			samples[i].list, samples[i].pc, samples[i].from, samples[i].op, samples[i].prev);
	sample_count = 0;
	for (uint32_t i = 0; i < miss_count; i++)
		sceClibPrintf("[gt] vram miss addr=%08x pool_full=%u requested=%08x (bit0 ours, bit1 in ring, rest flips served)\n",
			miss_samples[i][0], miss_samples[i][1], miss_samples[i][2]);
	miss_count = 0;
	memset(&window, 0, sizeof(window));
}

/* New VRAM page requests go to the PSP side, which serves them on its next
   frame flip. */
static void tapSendRequests(void) {
	volatile uint32_t *out = (volatile uint32_t *)ScePspemuConvertAddress(
		0x80000000 | (ring_addr + offsetof(GetapRing, vram_request)), KERMIT_OUTPUT_MODE, sizeof(vram_request));
	for (uint32_t i = 0; i < GETAP_VRAM_PAGES / 32; i++)
		out[i] = vram_request[i];
	ScePspemuWritebackCache((void *)out, sizeof(vram_request));
	requests_dirty = 0;
}

int AdrenalineGeTap(SceSize args, void *argp) {
	uint32_t read = 0, attached = 0, report_start = 0;
	(void)args;
	(void)argp;
	/* no DMA copy of VRAM (dma_vram): SC and Tekken write VRAM with the CPU,
	   and ScePspemu needs the memory */
	while (1) {
		ring_addr = *(volatile uint32_t *)ScePspemuConvertAddress(GETAP_POINTER, KERMIT_INPUT_MODE, 4);
		if (!tapValid(ring_addr)) {
			if (attached)
				sceClibPrintf("[gt] detached: pointer %08x\n", ring_addr);
			attached = 0;
			sceKernelDelayThread(100 * 1000);
			continue;
		}
		/* header, requests and slots: what the PSP side updates besides events */
		ring = (GetapRing *)ScePspemuConvertAddress(0x80000000 | ring_addr, KERMIT_INPUT_MODE,
			offsetof(GetapRing, events));
		uint32_t write = ring->write;
		if (ring->magic != GETAP_MAGIC || ring->version != GETAP_VERSION || (attached && write < read)) {
			if (attached)
				sceClibPrintf("[gt] detached: magic %08x version %u write %u read %u\n", ring->magic, ring->version, write, read);
			attached = 0;
			sceKernelDelayThread(100 * 1000);
			continue;
		}
		if (!attached) { /* a new game: start at the current position */
			attached = 1;
			read = write;
			report_start = sceKernelGetProcessTimeLow();
			memset(tap_lists, 0, sizeof(tap_lists));
			memset(&window, 0, sizeof(window));
			memset(&frame, 0, sizeof(frame));
			memset(vram_request, 0, sizeof(vram_request));
			memset(dma_valid, 0, sizeof(dma_valid));
			if (dma_vram)
				memset(dma_vram, 0, 0x40000);
			ge_base = 0;
			static int renderer_started;
			if (!renderer_started) { /* only with ge_tap: its memory is ScePspemu's otherwise */
				grInit();
				renderer_started = 1;
			}
			grReset();
			sceClibPrintf("[gt] attached ring=%08x write=%u pool=%08x pages=%u dma_hooked=%u dma_vram=%08x\n", ring_addr, write, ring->pool, ring->pool_pages,
				ring->dma_hooked, (uint32_t)dma_vram);
		}
		if (write - read > GETAP_RING) {
			window.ring_lost += write - read - GETAP_RING;
			read = write - GETAP_RING;
			memset(tap_lists, 0, sizeof(tap_lists));
		}
		if (write - read > window.max_lag)
			window.max_lag = write - read;
		if (write != read) {
			ScePspemuConvertAddress(0x80000000 | (ring_addr + offsetof(GetapRing, events)), KERMIT_INPUT_MODE,
				sizeof(ring->events));
		}
		while (read != write) {
			GetapEvent *e = &ring->events[read & (GETAP_RING - 1)];
			switch (e->type) {
				case GETAP_EV_ENQUEUE:
				case GETAP_EV_ENQUEUE_HEAD:
					tapStart(e->a, e->b, e->c);
					break;
				case GETAP_EV_STALL:
					tapStall(e->a, e->b);
					break;
				case GETAP_EV_FRAME:
					tapEndFrame();
					grEndFrame(e->a);
					break;
				case GETAP_EV_DMA:
					tapDma(e->a, e->b, e->c);
					break;
			}
			read++;
		}
		if (requests_dirty)
			tapSendRequests();
		/* the PSP side waits for this when the game syncs; with the renderer
		   off ("Original") it is published far ahead so nothing waits */
		static uint32_t published;
		uint32_t publish = read;
		if (publish != published) {
			volatile uint32_t *out = (volatile uint32_t *)ScePspemuConvertAddress(
				0x80000000 | (ring_addr + offsetof(GetapRing, vita_read)), KERMIT_OUTPUT_MODE, 64);
			*out = publish;
			ScePspemuWritebackCache((void *)out, 64);
			published = publish;
		}
		uint32_t now = sceKernelGetProcessTimeLow();
		if (now - report_start >= TAP_REPORT_US) {
			tapReport(now - report_start);
			grReport();
			report_start = now;
		}
		/* R + SELECT: a snapshot to compare (pressed, not held) */
		static uint32_t held;
		SceCtrlData pad;
		uint32_t combo = 0;
		if (sceCtrlPeekBufferPositive(0, &pad, 1) > 0)
			combo = (pad.buttons & (SCE_CTRL_RTRIGGER | SCE_CTRL_SELECT)) == (SCE_CTRL_RTRIGGER | SCE_CTRL_SELECT);
		if (combo && !held)
			grDump();
		held = combo;
		sceKernelDelayThread(500);
	}
	return 0;
}
