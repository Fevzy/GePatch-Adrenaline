/* Vertices3 + Inset1: scoped through-vertex edits and a half-texel UV inset for
 * linear-filtered through draws, all retired at confirmed GE completion. */
#include <pspsdk.h>
#include <pspkernel.h>
#include <pspctrl.h>
#include <pspge.h>
#include <pspgu.h>
#include <psppower.h>
#include <pspsysmem_kernel.h>

#include <stdio.h>
#include <string.h>

#include <systemctrl.h>

#include "ge_constants.h"
#include "vertex_restore.h"
#include "uv_inset.h"
#include "guard_band.h"

PSP_MODULE_INFO("GePatch", 0x1007, 1, 23);

#define DRAW_NATIVE 0xABCDEF00

/* Adrenaline's side, if it is ours: its setting (GP_MODE_ADDR, anything but
 * 0), and what the frame shown is -- Adrenaline displays the display buffer
 * at this size and format and scales it. */
#define GP_MODE_ADDR 0xABCDEF40 /* written by the Vita side: a cache line of its own */
#define GP_FRAME_SIZE_ADDR 0xABCDEF24  /* width | height << 16 */
#define GP_FRAME_FORMAT_ADDR 0xABCDEF28 /* GE_FORMAT_* */
#define GP_FRAME_COUNT_ADDR 0xABCDEF2C  /* frames shown, for Adrenaline's FPS counter */
#define GP_FRAME_READY_ADDR 0xABCDEF30  /* frames left in VRAM for the Vita to copy itself (see presentDirect) */
#define GP_FRAME_TAKEN_ADDR 0xABCDEF80  /* the last of them it copied: the Vita's own cache line */
#define GP_VIDEO_TAKES_FRAMES 8u        /* GP_VIDEO_ADDR bit: the Vita copies frames from VRAM */
#define GP_VIDEO_TWO_BUFFERS 0x20u      /* GP_VIDEO_ADDR bit: the Vita shows the buffer DISPLAY_FRONT names */
#define GP_VIDEO_ADDR 0xABCDEF44        /* bit 0 ScePspemu decodes a movie (Adrenaline sees it, late
                                         * and with gaps), bit 3 GP_VIDEO_TAKES_FRAMES, bit 5
                                         * GP_VIDEO_TWO_BUFFERS */

/* The internal resolution, 960x544: twice the PSP's. PITCH is the GE stride,
 * a multiple of 64 pixels; the depth buffer after the draw buffer. (1.5x,
 * 960x544i, frame skipping: gone, 30.10 -- Tekken's fights ran at full speed
 * at 2x with none of them, the user.) */
#define WIDTH 960u
#define HEIGHT 544u
#define PITCH 960u
#define GP_DEPTH 0x04100000u
#define PIXELFORMAT GE_FORMAT_565
/* Nothing at Adrenaline's setting: an Adrenaline without our side (TheFloW's,
 * isage's), shown the way it shows GePatch's frame, 565 (gp_stock). */
static u32 gp_stock;

/* The game's video decoder is open or decoding: set by its hooks at once,
 * kept per frame by followMode. */
static u32 movie_playing;
/* A movie frame as drawn: a sprite of at least 256x256 texels from the game's
 * VRAM above our buffers' share (decoders write there), and no 3D. Seen from
 * its first frame; Adrenaline's decode signal came 1.9 s late in DW. */
static u32 frame_movie_draw, frame_3d_draw, frame_draws, movie_frames_ago = 0xffffu;
#define MOVIE_MAX_DRAWS 6u /* DW's movie: a clear and the picture (in two lists); its menus draw more */
#define MOVIE_LEAVE_FRAMES 8u /* a 30 fps movie draws every other display frame */
/* The registers games set once (depth buffer, strides, viewport, scissor)
 * are sent again with the next copy: at the first (the GE's offset ours from
 * then on), and when the movie depth buffer comes or goes. */
static u32 mode_changed = 1;
static void sendModeRegisters(void);

#define FAKE_VRAM 0x0A000000
#define DISPLAY_BUFFER 0x0A400000
/* Big 3D triangles near the camera the GE drops at 2x and keeps on the PSP
 * (its guard band, 4096 screen pixels): cut here (guardBand, below). */
#ifndef GP_GUARD
#define GP_GUARD 1
#endif
/* Two display buffers, with our Adrenaline (GP_VIDEO_TWO_BUFFERS): each frame
 * copied into the one not shown, and then, by the GE right after the copy,
 * its number written to DISPLAY_FRONT, which the Vita reads to pick the one
 * it shows. With one, the Vita's GPU read it while the copy wrote it: a fade
 * came out torn, lighter above and darker below a line (the user). */
#define DISPLAY_BUFFER2 0x0A500000
#define DISPLAY_FRONT 0x0A5FF000u        /* 0 or 1, the GE's (a cache line of its own) */
#define DISPLAY_FRONT_VALUES 0x0A5FF040u /* 0 and 1, 64 bytes apart: what it copies there */
#define VERTICES_BUFFER 0x0A600000
#define RENDER_LIST 0x0A800000
#define VRAM_DRAW_BUFFER_OFFSET 0x04000000
/* While a movie plays the depth buffer
 * is squeezed to a 64-pixel stride at the end of VRAM (rows overlap: a movie
 * draws no 3D), and the megabyte it leaves takes the movie's picture (see
 * useRealVramTexture): drawn from real VRAM it starts at once; read from RAM
 * a frame took 47 ms and the start lagged behind the sound. */
#define MOVIE_DEPTH 0x1EE000u        /* 543 * 128 + 1920 bytes fit before 2 MB */
#define MOVIE_DEPTH_STRIDE 64u
static u32 movie_depth;              /* set between frames by followMode */
#define VRAM_DEPTH_BUFFER_OFFSET (movie_depth ? 0x04000000u + MOVIE_DEPTH : GP_DEPTH)
#define DEPTH_PITCH (movie_depth ? MOVIE_DEPTH_STRIDE : PITCH)
#define VRAM_1KB 0x041ff000
/* GP_LOG 0 (the usual build): nothing written to the memory stick at all --
 * no statistics, no log thread, no frame dump. GP_LOG=1 for diagnosis. */
#ifndef GP_LOG
#define GP_LOG 0
#endif
/* the sublist cache measure ([gp-cache]): done, and costly in itself */
#ifndef GP_SUBMEASURE
#define GP_SUBMEASURE 0
#endif
#if GP_LOG
#define log(...) \
{ \
  char msg[256]; \
  sprintf(msg,__VA_ARGS__); \
  logmsg(msg); \
}
#else
#define log(...) do { } while (0)
#endif

static void writeLog(const char *text, unsigned size) {
  int k1 = pspSdkSetK1(0);
  SceUID fd = sceIoOpen("ms0:/ge_patch.txt", PSP_O_WRONLY | PSP_O_CREAT, 0777);
  if (fd >= 0) {
    sceIoLseek(fd, 0, PSP_SEEK_END);
    sceIoWrite(fd, text, size);
    sceIoClose(fd);
  }
  pspSdkSetK1(k1);
}

/* Lines go to a buffer that a thread of its own writes out each second: an
 * open, write and close on the memory stick per line, in the game's thread
 * at its frame flip, was a stall every 5 s (the statistics). Before the
 * thread runs, and if the buffer is full, a line is written at once. */
#define LOG_BUFFER 0x0A8D0000u /* upper RAM GePatch uses anyway */
/* The last hook the game entered, and when; and when it last showed a frame:
 * the log thread reports them while the game shows none (a hang). 1 stall
 * update, 2 enqueue, 3 enqueue head, 4 list sync, 5 draw sync, 6 frame. */
static volatile u32 gp_last_hook, gp_last_hook_ms, gp_last_frame_ms, gp_hook_count;
#define HOOK_ENTERED(id) do { gp_last_hook = (id); gp_last_hook_ms = sceKernelGetSystemTimeLow() / 1000u; ++gp_hook_count; \
                              if ((id) == 6) gp_last_frame_ms = gp_last_hook_ms; } while (0)
static unsigned ge_samples, ge_busy_samples; /* GE busy looks, by the log thread */
extern int (*_sceGeDrawSync)(int);
#define LOG_BUFFER_SIZE 0x8000u
static unsigned log_used, log_thread_running;
void logmsg(char *msg) {
  const unsigned size = strlen(msg);
  if (log_thread_running) {
    const int intr = sceKernelCpuSuspendIntr();
    const int fits = log_used + size <= LOG_BUFFER_SIZE;
    if (fits) {
      memcpy((char *)LOG_BUFFER + log_used, msg, size);
      log_used += size;
    }
    sceKernelCpuResumeIntr(intr);
    if (fits) return;
  }
  writeLog(msg, size);
}
/* (GP_LOG) the game's threads every 5 s: priority, CPU time they had, their
 * state -- does our parse, on the drawing thread, leave Tekken's other
 * threads (its game logic?) short of the CPU? */
#if GP_LOG
static SceUID render_thread; /* the thread enqueueing the game's lists */
static void logThreads(void) {
  static SceUID ids[48];
  static u32 last_run[48];
  SceUID now_ids[48];
  int count = 0, i, k;
  if (sceKernelGetThreadmanIdList(SCE_KERNEL_TMID_Thread, now_ids, 48, &count) < 0) return;
  if (count > 48) count = 48;
  for (i = 0; i < count; ++i) {
    SceKernelThreadInfo info;
    char line[160];
    u32 before = 0, run;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelReferThreadStatus(now_ids[i], &info) < 0 || !(info.attr & 0x80000000u)) continue; /* user threads */
    run = info.runClocks.low; /* microseconds */
    for (k = 0; k < 48; ++k)
      if (ids[k] == now_ids[i]) { before = last_run[k]; break; }
    if (k == 48)
      for (k = 0; k < 48; ++k)
        if (!ids[k]) { ids[k] = now_ids[i]; break; }
    if (k < 48) last_run[k] = run;
    sprintf(line, "[gp-thread] %s%s prio=%d run_ms=%u status=%d wait=%d\n", info.name,
            now_ids[i] == render_thread ? " (draws)" : "", info.currentPriority, (unsigned)((run - before) / 1000u),
            info.status, info.waitType);
    logmsg(line);
  }
}
#endif
static u32 *grp_top, *grp_stall; /* (the groups' own: below) */
static int logThread(SceSize args, void *argp) {
  char *const out = (char *)(LOG_BUFFER + LOG_BUFFER_SIZE); /* not in the kernel's scarce memory */
  (void)args; (void)argp;
  for (;;) {
    unsigned size, k;
    int intr;
    /* meanwhile: how often the GE is busy, 1000 looks a second (a game that
     * never waits for it shows nothing in ge_wait) */
    for (k = 0; k < 1000u; ++k) {
      sceKernelDelayThread(1000);
      ++ge_samples;
      if (_sceGeDrawSync && _sceGeDrawSync(1) != 0) ++ge_busy_samples;
    }
    {
      static unsigned beats;
      const u32 now = sceKernelGetSystemTimeLow() / 1000u;
      if (gp_last_frame_ms && now - gp_last_frame_ms > 1500u && beats < 60u) { /* no frame for 1.5 s */
        char line[160];
        ++beats;
        sprintf(line, "[gp-beat] no frame for %u ms: last hook %u, %u ms ago, %u hook calls, GE %s\n",
                (unsigned)(now - gp_last_frame_ms), (unsigned)gp_last_hook, (unsigned)(now - gp_last_hook_ms),
                (unsigned)gp_hook_count, _sceGeDrawSync && _sceGeDrawSync(1) != 0 ? "busy" : "idle");
        logmsg(line);
        if (beats <= 3u) { /* where the GE is (uOFW hardware.h), and the words there */
          const u32 pc = *(volatile u32 *)0xBD400108u & 0x0fffffffu;
          const volatile u32 *w = (const volatile u32 *)(0x40000000u | ((pc - 16u) & 0x0ffffffcu));
          sprintf(line, "[gp-beat] GE exec %08x pc %08x stall %08x radr %08x %08x oadr %08x; ours top %08x stall %08x\n",
                  (unsigned)*(volatile u32 *)0xBD400100u, (unsigned)pc, (unsigned)*(volatile u32 *)0xBD40010Cu,
                  (unsigned)*(volatile u32 *)0xBD400110u, (unsigned)*(volatile u32 *)0xBD400114u,
                  (unsigned)*(volatile u32 *)0xBD400120u, (unsigned)grp_top, (unsigned)grp_stall);
          logmsg(line);
          if (pc >= 0x08000000u && pc < 0x0c000000u) {
            sprintf(line, "[gp-beat] at pc-16: %08x %08x %08x %08x | %08x %08x %08x %08x\n", (unsigned)w[0], (unsigned)w[1],
                    (unsigned)w[2], (unsigned)w[3], (unsigned)w[4], (unsigned)w[5], (unsigned)w[6], (unsigned)w[7]);
            logmsg(line);
          }
        }
      }
    }
#if GP_LOG
    {
      static unsigned seconds;
      if (++seconds % 5u == 0u) logThreads();
    }
#endif
    intr = sceKernelCpuSuspendIntr();
    size = log_used;
    memcpy(out, (const char *)LOG_BUFFER, size);
    log_used = 0;
    sceKernelCpuResumeIntr(intr);
    if (size) writeLog(out, size);
  }
  return 0;
}
static void startLogThread(void) {
  const SceUID thread = sceKernelCreateThread("GePatchLog", logThread, 0x70, 0x1000, 0, NULL);
  if (thread >= 0 && sceKernelStartThread(thread, 0, NULL) >= 0) log_thread_running = 1;
}
int checkAddress(u32 addr) {
  addr &= 0x0fffffff;
  if (addr >= 0x08400000 && addr < 0x0A400000)
    return 0;
  if (addr >= 0x04000000 && addr < 0x04200000) {
    // log("vram address: %08x\n", addr);
    return -1;
  }
  log("invalid address: %08x\n", (unsigned)addr);
  return -1;
}
static const u8 tcsize[4] = { 0, 2, 4, 8 }, tcalign[4] = { 0, 1, 2, 4 };
static const u8 colsize[8] = { 0, 0, 0, 0, 2, 2, 2, 4 }, colalign[8] = { 0, 0, 0, 0, 2, 2, 2, 4 };
static const u8 nrmsize[4] = { 0, 3, 6, 12 }, nrmalign[4] = { 0, 1, 2, 4 };
static const u8 possize[4] = { 3, 3, 6, 12 }, posalign[4] = { 1, 1, 2, 4 };
static const u8 wtsize[4] = { 0, 1, 2, 4 }, wtalign[4] = { 0, 1, 2, 4 };
#define ALIGN(x, align) (((x) + ((align) - 1)) & ~((align) - 1))
void getVertexInfo(u32 op, u32 *vertex_size, u32 *pos_off, u32 *visit_off) {
  int tc = (op & GE_VTYPE_TC_MASK) >> GE_VTYPE_TC_SHIFT;
  int col = (op & GE_VTYPE_COL_MASK) >> GE_VTYPE_COL_SHIFT;
  int nrm = (op & GE_VTYPE_NRM_MASK) >> GE_VTYPE_NRM_SHIFT;
  int pos = (op & GE_VTYPE_POS_MASK) >> GE_VTYPE_POS_SHIFT;
  int weight = (op & GE_VTYPE_WEIGHT_MASK) >> GE_VTYPE_WEIGHT_SHIFT;
  int weightCount = ((op & GE_VTYPE_WEIGHTCOUNT_MASK) >> GE_VTYPE_WEIGHTCOUNT_SHIFT) + 1;
  int morphCount = ((op & GE_VTYPE_MORPHCOUNT_MASK) >> GE_VTYPE_MORPHCOUNT_SHIFT) + 1;
  u32 biggest = 0;
  u32 size = 0;
  u32 aligned_size = 0;
  // u8 weightoff = 0, tcoff = 0, coloff = 0, nrmoff = 0;
  u32 posoff = 0;
  u32 visitoff = 0;

  if (weight) {
    // size = ALIGN(size, wtalign[weight]);
    // weightoff = size;
    size += wtsize[weight] * weightCount;
    if (wtalign[weight] > biggest)
      biggest = wtalign[weight];
  }
  if (tc) {
    aligned_size = ALIGN(size, tcalign[tc]);
    if (!visitoff && aligned_size != size)
      visitoff = size;
    size = aligned_size;
    // tcoff = size;
    size += tcsize[tc];
    if (tcalign[tc] > biggest)
      biggest = tcalign[tc];
  }
  if (col) {
    aligned_size = ALIGN(size, colalign[col]);
    if (!visitoff && aligned_size != size)
      visitoff = size;
    size = aligned_size;
    // coloff = size;
    size += colsize[col];
    if (colalign[col] > biggest)
      biggest = colalign[col];
  }
  if (nrm) {
    aligned_size = ALIGN(size, nrmalign[nrm]);
    if (!visitoff && aligned_size != size)
      visitoff = size;
    size = aligned_size;
    // nrmoff = size;
    size += nrmsize[nrm];
    if (nrmalign[nrm] > biggest)
      biggest = nrmalign[nrm];
  }
  if (pos) {
    aligned_size = ALIGN(size, posalign[pos]);
    if (!visitoff && aligned_size != size)
      visitoff = size;
    size = aligned_size;
    posoff = size;
    size += possize[pos];
    if (posalign[pos] > biggest)
      biggest = posalign[pos];
  }

  aligned_size = ALIGN(size, biggest);
  if (!visitoff && aligned_size != size)
    visitoff = size;
  size = aligned_size;
  size *= morphCount;

  *vertex_size = size;
  *pos_off = posoff;
  *visit_off = visitoff;
}
/* getVertexInfo for the last VTYPE asked about: draws mostly share theirs */
static void vertexInfoCached(u32 vt, u32 *vertex_size, u32 *pos_off, u32 *visit_off) {
  /* a few types at once: Tekken's fight switches between them draw by draw */
  static u32 type[8] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u}, size[8], pos[8], visit[8];
  const unsigned k = (vt ^ (vt >> 9) ^ (vt >> 14)) & 7u;
  if (vt != type[k]) { type[k] = vt; getVertexInfo(vt, &size[k], &pos[k], &visit[k]); }
  *vertex_size = size[k]; *pos_off = pos[k]; *visit_off = visit[k];
}
void GetIndexBounds(const void *inds, int count, u32 vertType, u32 *indexLowerBound, u32 *indexUpperBound) {
  int i;
  u32 lowerBound = 0xFFFFFFFFu;
  u32 upperBound = 0;
  u32 idx = vertType & GE_VTYPE_IDX_MASK;
  if (idx == GE_VTYPE_IDX_8BIT) {
    const u8 *ind8 = (const u8 *)inds;
    for (i = 0; i < count; i++) {
      u8 value = ind8[i];
      if (value > upperBound)
        upperBound = value;
      if (value < lowerBound)
        lowerBound = value;
    }
  } else if (idx == GE_VTYPE_IDX_16BIT) {
    const u16 *ind16 = (const u16 *)inds;
    for (i = 0; i < count; i++) {
      u16 value = ind16[i];
      if (value > upperBound)
        upperBound = value;
      if (value < lowerBound)
        lowerBound = value;
    }
  } else if (idx == GE_VTYPE_IDX_32BIT) {
    const u32 *ind32 = (const u32 *)inds;
    for (i = 0; i < count; i++) {
      u32 value = ind32[i];
      if (value > upperBound)
        upperBound = value;
      if (value < lowerBound)
        lowerBound = value;
    }
  } else {
    lowerBound = 0;
    upperBound = count - 1;
  }
  *indexLowerBound = lowerBound;
  *indexUpperBound = upperBound;
}
typedef struct {
  u32 *list;
  u32 base;
  u32 offset;
  u32 commands, writes, draws_2d; /* at the CALL: what its sublist did (statistics) */
  u32 target, drops;               /* (GP_LOG: the sublist cache measure) */
} StackEntry;
/* Words of the outermost CALLed sublists, those GePatch changed nothing in
 * (no word written, no 2D draw) and the rest: what a cache of unchanged
 * sublists would save. */
static u32 perf_writes, perf_clean_words, perf_dirty_words, perf_clean_calls, perf_dirty_calls;
#if GP_LOG && GP_SUBMEASURE
/* What a sublist cache would save: each outermost CALL's words (matrix data
 * left out) hashed and compared with the last time at that address; "pure"
 * if it held nothing patched anew each frame (buffers, screen registers, 2D
 * draws, dropped draws, jumps out). */
static u32 sub_hash, sub_uncacheable, sub_same_calls, sub_same_words, sub_same_pure_calls, sub_same_pure_words,
    sub_pure_words, sub_lost;
static u8 sub_uncache[256];
#define SUB_TABLE 8192u
static u32 sub_addr[SUB_TABLE], sub_sum[SUB_TABLE];
static void subNote(u32 address, u32 words) {
  unsigned slot = (address >> 2) & (SUB_TABLE - 1u), n;
  for (n = 0; n < 16u; ++n, slot = (slot + 1u) & (SUB_TABLE - 1u))
    if (sub_addr[slot] == address || !sub_addr[slot]) break;
  if (n == 16u) { ++sub_lost; return; }
  if (!sub_uncacheable) sub_pure_words += words;
  if (sub_addr[slot] == address && sub_sum[slot] == sub_hash) {
    ++sub_same_calls; sub_same_words += words;
    if (!sub_uncacheable) { ++sub_same_pure_calls; sub_same_pure_words += words; }
  }
  sub_addr[slot] = address;
  sub_sum[slot] = sub_hash;
}
#endif

typedef struct {
  u32 ge_cmd[0x100];

  u32 texbufptr[8];
  u32 texbufwidth[8];
  u32 framebufptr;
  u32 framebufwidth;
  u32 *framebufwidth_addr;
  u32 framebuf;

  u32 base;
  u32 offset;
  u32 address;

  u32 index_addr;
  u32 vertex_addr;
  u32 vertex_type;

  u32 ignore_framebuf;
  u32 ignore_texture;

  u32 has_draws;

  StackEntry stack[64];
  u32 curr_stack;

  u32 framebuf_addr[16];
  u32 framebuf_count;

  /* The draw's own BASE/VADDR/VTYPE words (UV inset). since_draw: bit 0 BASE,
   * bit 1 VADDR, bit 2 VTYPE seen since the previous draw; base_users counts
   * commands that used BASE after the last BASE word. */
  u32 *base_word, *vaddr_word, *vtype_word;
  u32 vaddr_offset, since_draw, base_users;
  /* An inset waits until the next draw has set BASE, VADDR and VTYPE again
   * (seen: same bits, counted after the inset draw). Without that the GE would
   * carry our values into a draw that relies on them. */
  u32 pending, pending_seen, pending_cursor, pending_primitive;
  /* The draw being parsed continues the pending inset's vertex pointer and
   * must join its copy; if it cannot, the inset is abandoned after it. */
  u32 extend_open;
  /* BASE, VADDR, VTYPE of the inset draw, then BASE and VADDR of each draw of
   * a run that shares its VTYPE word (see insetJoinRun). */
#define INSET_RUN_DRAWS 32u
  uint32_t *pending_words[3 + 2 * INSET_RUN_DRAWS];
  uint32_t pending_values[3 + 2 * INSET_RUN_DRAWS];
  u32 pending_count, run_open;
} GeState;
static GeState state;
/* 295 KB, out of the kernel's memory: what this module takes there the game
 * cannot have (DW hung loading a battle once the module grew by 21 KB; 1 MB
 * there broke savedata). Plain CPU data: the upper RAM GePatch uses anyway. */
#define VERTEX_RESTORE_TABLE 0x0A810000u /* after RENDER_LIST's lists */
#define vertex_restore (*(GpVertexRestore *)VERTEX_RESTORE_TABLE)
/* (469 KB on the PSP: to 0x0A88282C; the host's, with 8-byte pointers, larger) */
typedef char vertex_restore_fits[sizeof(GpVertexRestore) <= 0x80000u || sizeof(void *) != 4u ? 1 : -1];
/* More of GePatch's tables out of the kernel's memory, after it (bss had grown
 * to 151 KB, 30.10; DW stopped loading a battle as it grew, 28.10 and 30.10):
 * the inset's state, the saved words, the groups' tables. Cleared at start. */
#define SAVED_SLOTS 1024u /* (saveWord's: below) */
#define GRP_WORDS 4096u /* (the groups': below) */
#define GRP_DONE 1024u
/* (sizes as compiled: the host tests' pointers are 8 bytes; 0x0A890000 on the PSP) */
#define UPPER_ROUND(n) (((u32)(n) + 0xffffu) & ~0xffffu)
#define UPPER_TABLES (VERTEX_RESTORE_TABLE + UPPER_ROUND(sizeof(GpVertexRestore)))
#define INSET_STATE UPPER_TABLES
#define SAVED_TABLES (INSET_STATE + UPPER_ROUND(sizeof(GpInset)))                /* SAVED_SLOTS pointers, 3 x SAVED_SLOTS words */
#define GROUP_TABLES (SAVED_TABLES + UPPER_ROUND(SAVED_SLOTS * (sizeof(void *) + 12u))) /* GRP_WORDS pointers, 2 x GRP_WORDS words, 2 x GRP_DONE pointers */
#define UPPER_TABLES_END (GROUP_TABLES + UPPER_ROUND(GRP_WORDS * (sizeof(void *) + 8u) + 2u * GRP_DONE * sizeof(void *)))
typedef char inset_fits[sizeof(GpInset) <= 0x10000u || sizeof(void *) != 4u ? 1 : -1];
typedef char upper_tables_fit[UPPER_TABLES_END <= 0x0A8D0000u || sizeof(void *) != 4u ? 1 : -1]; /* (LOG_BUFFER) */
#define inset (*(GpInset *)(uintptr_t)INSET_STATE)
static unsigned vertex_bounds_errors;
/* When the game last showed a frame (SetFrameBuf), microseconds, if it has. */
static u32 framebuf_set_at, framebuf_seen;
static int rendered_in_sync;
static unsigned int copies_queued, copy_errors;
/* Pixel format of the redirected draw buffer, from the game's last
 * FRAMEBUFPIXFORMAT; anything but 565 is converted for display. */
static u32 draw_format = PIXELFORMAT;
static int force_565; /* R held at boot: the old 565 path, for comparison */
/* SELECT held at boot: nothing is patched, the game runs as it does without
 * GePatch; only frames and GE waits are timed ([gp-perf] measure=1). */
static int measure_only;
/* TRIANGLE held at boot: the old whole-cache writeback+invalidate after every
 * parse, for comparison. Otherwise only what this plugin wrote is written
 * back: on the Vita the PSP and its GE are emulated, and a whole-cache
 * operation per list looks like it makes the emulator drop its texture cache
 * (Tekken and DW's movies: a handful of commands, 20-400 ms of GE time). */
static int legacy_cache;
static u32 wb_lo, wb_hi; /* pending writeback range */
static void flushWrites(void) {
  if (wb_hi) sceKernelDcacheWritebackRange((void *)wb_lo, wb_hi - wb_lo);
  wb_lo = wb_hi = 0;
}
/* Gaps up to 16 KiB are merged: writing back clean lines does nothing. */
static void noteWrite(const void *p, unsigned size);
/* Words changed in the game's lists (TEXLEVEL, TEXBUFWIDTH0), put back once
 * the GE is done (like the vertices): lists are parsed again, and a change
 * must not add up or be mistaken for the game's own value. */
#define saved_words ((u32 **)(uintptr_t)SAVED_TABLES)
#define saved_original ((u32 *)(uintptr_t)(SAVED_TABLES + SAVED_SLOTS * sizeof(void *)))
#define saved_value (saved_original + SAVED_SLOTS)
#define saved_used_slots (saved_original + 2u * SAVED_SLOTS)
static u32 saved_used, lod_full;
/* The game's own value of a word, changed or not. Every change is made from
 * it: a list parsed again before the GE finished (DW's movie: seconds) must
 * not see our value -- after a mode change it is the wrong one. */
static u32 savedOriginal(const u32 *word, u32 op) {
  unsigned slot = (unsigned)(((u32)word >> 2) & (SAVED_SLOTS - 1u));
  if (!saved_used) return op; /* nothing changed since the GE was last done */
  while (saved_words[slot]) {
    if (saved_words[slot] == word) {
      /* ours still there: the game's value is the saved one; else the game
       * wrote the word again (a new list where an old one was) */
      if (op != saved_value[slot]) { saved_original[slot] = saved_value[slot] = op; return op; }
      return saved_original[slot];
    }
    slot = (slot + 1u) & (SAVED_SLOTS - 1u);
  }
  return op;
}
/* Before changing a word to `value`: keeps the game's value (once) and ours
 * (what restoring checks for). 0 if full. */
static int saveWord(u32 *word, u32 original, u32 value) {
  unsigned slot = (unsigned)(((u32)word >> 2) & (SAVED_SLOTS - 1u));
  while (saved_words[slot]) {
    if (saved_words[slot] == word) { saved_value[slot] = value; return 1; }
    slot = (slot + 1u) & (SAVED_SLOTS - 1u);
  }
  if (value == original) return 1; /* back to the game's: nothing to keep */
  if (saved_used >= SAVED_SLOTS * 3u / 4u) { ++lod_full; return 0; }
  saved_words[slot] = word;
  saved_original[slot] = original;
  saved_value[slot] = value;
  saved_used_slots[saved_used++] = slot;
  return 1;
}
/* *word = value, from the game's original: kept for putting back. */
static void changeWord(u32 *word, u32 original, u32 value) {
  if (*word == value) return;
  saveWord(word, original, value); /* full: changed anyway, as before */
  *word = value;
}
static unsigned restoreSavedWords(void) {
  unsigned i, n = saved_used;
  for (i = 0; i < saved_used; ++i) {
    const unsigned slot = saved_used_slots[i];
    /* a word the game has written again since is its own */
    if (*saved_words[slot] == saved_value[slot]) {
      *saved_words[slot] = saved_original[slot];
      if (!legacy_cache) noteWrite(saved_words[slot], 4u);
    }
    saved_words[slot] = 0;
  }
  saved_used = 0;
  return n;
}
/* Auto mip level: the GE picks it from texels per screen pixel, and at 2x
 * there are twice the pixels -- one level finer than on the PSP. DW streams
 * the finer levels only for what is near: far ground came out grey until
 * approached. The bias (signed 4.4) puts it back. */
#define LOD_BIAS 16
static void patchTexLevel(u32 *word, u32 op) {
  int bias = (int)(signed char)((op >> 16) & 0xffu) + LOD_BIAS;
  if ((op & 3u) != GE_TEXLEVEL_MODE_AUTO) bias = (int)(signed char)((op >> 16) & 0xffu);
  if (bias > 127) bias = 127;
  changeWord(word, op, (op & 0xff00ffffu) | ((u32)(bias & 0xff) << 16));
}

/* Textures in the game's VRAM (fake VRAM: RAM at 0x0A000000) are read slowly
 * by the GE when they are not swizzled -- the reason PSP games keep movie
 * frames in VRAM: DW's movie, one 480x272 5551 sprite, took 26-47 ms a frame.
 * While the depth buffer is squeezed for a movie, such a texture is copied to
 * real VRAM, into the depth buffer's share, and drawn from there. A movie's
 * picture: above REAL_VRAM_FREE in the game's VRAM (past its own frame). */
#define REAL_VRAM_FREE 0xCC000u /* 512x272 8888 colour, then 16-bit depth */
#define VRAM_SIZE 0x200000u
static u32 currentTexture(void);
static u32 *tbw0_word, *tex0_word; /* this list's TEXBUFWIDTH0/TEXADDR0, to point at real VRAM */
/* Two places in real VRAM, a frame each: copying the next movie frame where
 * the GE still draws the last one from mixed their lines (black stripes). */
static u32 vram_slot;
static u32 vram_copied[8][2], vram_copied_count, perf_vram_copies, perf_vram_bytes;
/* A through-mode textured sprite of at least 512x256 texels from the game's
 * VRAM above our buffers' share: what a movie frame (decoded there) is drawn
 * with. Only these are copied to real VRAM: every VRAM texture of a 3D scene
 * was 26704 copies (400 MB) in 5 s. */
static int isMoviePicture(u32 prim) {
  u32 tex;
  if (((prim >> 16) & 7u) != 6u || (state.vertex_type & GE_VTYPE_THROUGH_MASK) != GE_VTYPE_THROUGH ||
      !(state.ge_cmd[GE_CMD_TEXTUREMAPENABLE] & 1u))
    return 0; /* 3D draws first: most of them */
  tex = currentTexture();
  return tex >= FAKE_VRAM + REAL_VRAM_FREE && tex < FAKE_VRAM + VRAM_SIZE &&
         (state.ge_cmd[GE_CMD_TEXSIZE0] & 15u) >= 9u && ((state.ge_cmd[GE_CMD_TEXSIZE0] >> 8) & 15u) >= 8u;
}
static void useRealVramTexture(void) {
  const u32 tex = currentTexture(), fmt = state.ge_cmd[GE_CMD_TEXFORMAT] & 15u;
  const u32 width = state.ge_cmd[GE_CMD_TEXBUFWIDTH0] & 0x7ffu;
  const u32 height = 1u << ((state.ge_cmd[GE_CMD_TEXSIZE0] >> 8) & 15u);
  static const u8 bits[8] = {16, 16, 16, 32, 4, 8, 16, 32};
  u32 offset, size, i;
  /* only while the depth buffer is squeezed for a movie (movie_depth), into
   * its share (a copy there with the full depth buffer was striped by the
   * sprite's own depth writes) */
  const u32 free_from = GP_DEPTH & 0xffffffu, free_to = MOVIE_DEPTH;
  u32 dest, slot_size;
  if (!movie_depth) return;
  if (tex < FAKE_VRAM || tex >= FAKE_VRAM + VRAM_SIZE) return;
  if (fmt > 7u || (state.ge_cmd[GE_CMD_TEXMODE] & 0x70000u)) return; /* DXT, mipmaps */
  /* both words of this list are pointed at the copy, or nothing is done */
  if (!tbw0_word || !tex0_word || (*tbw0_word >> 24) != GE_CMD_TEXBUFWIDTH0 || (*tex0_word >> 24) != GE_CMD_TEXADDR0) return;
  offset = tex - FAKE_VRAM;
  size = width * height * bits[fmt] / 8u;
  if (!size || offset + size > VRAM_SIZE) size = VRAM_SIZE - offset;
  slot_size = (size + 0x1fffu) & ~0x1fffu;
  if (free_from + slot_size > free_to) return;
  dest = free_from + (free_from + 2u * slot_size <= free_to ? vram_slot * slot_size : 0u);
  for (i = 0; i < vram_copied_count; ++i)
    if (vram_copied[i][0] == offset && vram_copied[i][1] == dest) break;
  if (i == vram_copied_count) { /* new in this frame: the game may have changed it */
    sceKernelDcacheWritebackInvalidateRange((void *)tex, size);
    memcpy((void *)(0x44000000u | dest), (const void *)tex, size); /* uncached VRAM */
    if (vram_copied_count < 8u) {
      vram_copied[vram_copied_count][0] = offset;
      vram_copied[vram_copied_count++][1] = dest;
    }
    ++perf_vram_copies;
    perf_vram_bytes += size;
  }
  {
    const u32 tbw = (GE_CMD_TEXBUFWIDTH0 << 24) | state.ge_cmd[GE_CMD_TEXBUFWIDTH0];
    const u32 addr = (GE_CMD_TEXADDR0 << 24) | state.ge_cmd[GE_CMD_TEXADDR0];
    const u32 real = VRAM_DRAW_BUFFER_OFFSET + dest;
    changeWord(tbw0_word, tbw, (tbw & 0xff00ffffu) | ((real >> 8) & 0x0f0000u));
    changeWord(tex0_word, addr, (GE_CMD_TEXADDR0 << 24) | (real & 0xffffffu));
    noteWrite(tbw0_word, 4u);
    noteWrite(tex0_word, 4u);
  }
}
#if GP_LOG
static int retro_on;
static void retroCheck(const void *p, unsigned size);
#endif
static void noteWrite(const void *p, unsigned size) {
#if GP_LOG
  if (retro_on) retroCheck(p, size);
#endif
  ++perf_writes;
  const u32 a = (u32)p & ~63u, e = ((u32)p + size + 63u) & ~63u;
  if (wb_hi && a <= wb_hi + 0x4000u && e + 0x4000u >= wb_lo) {
    if (a < wb_lo) wb_lo = a;
    if (e > wb_hi) wb_hi = e;
    return;
  }
  flushWrites();
  wb_lo = a; wb_hi = e;
}
/* Display frames left in which the stencil counts as in use. While it is, the
 * whole draw buffer is one 16-bit format with alpha (the stencil's bits); in
 * between -- movies, menus -- it is 565 and copied directly, which costs the
 * GE half as much as the conversion. One format for every target: a system
 * dialog drawing 565 into a 5551 buffer converted as 5551 came out purple. */
#define STENCIL_HOLD_FRAMES 120u
static u32 stencil_frames_left;
/* Frames left in which the game asked for 8888. Its 24-bit colour ends up in
 * a 16-bit buffer here (Adrenaline shows 565), and gradients band: the GE's
 * own ordered dither is switched on for it, as the PSP would for 16 bits. */
static u32 truecolor_frames_left;
/* ... and only for a game that itself asks for a format with alpha (5551 or
 * 4444) in that time. A game drawing 565 has no stencil on the PSP either:
 * turning it into 5551 only took a bit of green away (Tekken 6). */
static u32 alpha_frames_left, alpha_format = GE_FORMAT_5551;
static u32 chooseDrawFormat(u32 requested) {
  if (requested == GE_FORMAT_5551 || requested == GE_FORMAT_4444) {
    alpha_frames_left = STENCIL_HOLD_FRAMES;
    alpha_format = requested;
  }
  /* (an Adrenaline of others shows 565 only: no stencil bit then -- a game's
   * stencil shadows as on a PSP drawing 565) */
  if (force_565 || gp_stock || !stencil_frames_left || !alpha_frames_left) return GE_FORMAT_565;
  return alpha_format;
}
/* Targets and textures upstream GePatch drops, first of each kind: drawing
 * into a buffer whose stride is not 512/960, and sampling one it recorded. */
static u32 ignore_keys[16];
static unsigned ignore_count;
static void noteIgnore(unsigned what, u32 address, u32 detail) {
  const u32 key = (what << 30) ^ (address & 0x0fffffffu) ^ (detail << 20);
  unsigned i;
  for (i = 0; i < ignore_count; ++i)
    if (ignore_keys[i] == key) return;
  if (ignore_count == 16u) return;
  ignore_keys[ignore_count++] = key;
  log("[gp-ignore] %s address=%08x %s=%u fmt=%u\n", what ? "texture" : "framebuffer", (unsigned)address,
      what ? "texfmt" : "stride", (unsigned)detail, (unsigned)(state.ge_cmd[GE_CMD_FRAMEBUFPIXFORMAT] & 3u));
}

/* Lists enqueued since the last sync, for the slow-GE dump below. */
static u32 *recent_lists[4];
static unsigned recent_list_count, slow_dumped;

/* Textured linear draws the UV inset did not take, first of each kind. */
static u32 inset_skip_keys[16];
static unsigned inset_skip_count;
static void noteInsetSkip(unsigned reason, u32 primitive) {
  const u32 key = (reason << 28) ^ (state.vertex_type << 4) ^ (primitive >> 16) ^
                  ((state.ge_cmd[GE_CMD_TEXFORMAT] & 15u) << 24);
  unsigned i;
  for (i = 0; i < inset_skip_count; ++i)
    if (inset_skip_keys[i] == key) return;
  if (inset_skip_count == 16u) return;
  inset_skip_keys[inset_skip_count++] = key;
  log("[gp-inset-skip] reason=%u vt=%06x prim=%u count=%u texfmt=%u texsize=%06x tex=%06x/%06x mode=%06x\n",
      reason, (unsigned)state.vertex_type, (unsigned)(primitive >> 16), (unsigned)(primitive & 0xffffu),
      (unsigned)(state.ge_cmd[GE_CMD_TEXFORMAT] & 15u), (unsigned)state.ge_cmd[GE_CMD_TEXSIZE0],
      (unsigned)state.ge_cmd[GE_CMD_TEXADDR0], (unsigned)state.ge_cmd[GE_CMD_TEXBUFWIDTH0],
      (unsigned)state.ge_cmd[GE_CMD_TEXMODE]);
}
#define PRESENT_VERTICES (RENDER_LIST + 0x4000) /* after the copy list's commands */

/* The game set its own dither matrix (sceGuSetDither): emitDither keeps it. */
static u32 game_dither_set;
/* Per command: what the parser must do with it. Most commands of a 3D list
 * (material, light, matrix selection...) need nothing but the register copy;
 * they skip the switch and every check after it. CMD_HANDLED lists exactly
 * the switch's case labels in patchGeList -- keep the two in step (the host
 * test compares this fast path against the full path). */
enum { CMD_MATRIX = 1, CMD_HANDLED = 2, CMD_INSET = 8, CMD_KEEP = 16, CMD_SAVED = 32, CMD_HOT = 64 };
static u8 cmd_class[256];
static int force_full_parse; /* tests: every command through the full path */
static void buildCommandClasses(void) {
  static const u8 handled[] = {
    GE_CMD_BASE, GE_CMD_BEZIER, GE_CMD_BJUMP, GE_CMD_BOUNDINGBOX, GE_CMD_CALL, GE_CMD_END,
    GE_CMD_FRAMEBUFPIXFORMAT, GE_CMD_FRAMEBUFPTR, GE_CMD_FRAMEBUFWIDTH, GE_CMD_IADDR, GE_CMD_JUMP,
    GE_CMD_OFFSETADDR, GE_CMD_OFFSETX, GE_CMD_OFFSETY, GE_CMD_ORIGIN, GE_CMD_PRIM, GE_CMD_REGION2,
    GE_CMD_RET, GE_CMD_SCISSOR2, GE_CMD_SPLINE, GE_CMD_STENCILTESTENABLE,
    GE_CMD_TEXADDR0, GE_CMD_TEXADDR1, GE_CMD_TEXADDR2, GE_CMD_TEXADDR3,
    GE_CMD_TEXADDR4, GE_CMD_TEXADDR5, GE_CMD_TEXADDR6, GE_CMD_TEXADDR7,
    GE_CMD_TEXBUFWIDTH0, GE_CMD_TEXBUFWIDTH1, GE_CMD_TEXBUFWIDTH2, GE_CMD_TEXBUFWIDTH3,
    GE_CMD_TEXBUFWIDTH4, GE_CMD_TEXBUFWIDTH5, GE_CMD_TEXBUFWIDTH6, GE_CMD_TEXBUFWIDTH7,
    GE_CMD_VADDR, GE_CMD_VERTEXTYPE, GE_CMD_VIEWPORTXCENTER, GE_CMD_VIEWPORTXSCALE,
    GE_CMD_VIEWPORTYCENTER, GE_CMD_VIEWPORTYSCALE, GE_CMD_ZBUFPTR, GE_CMD_ZBUFWIDTH,
    GE_CMD_DITHERENABLE, GE_CMD_TEXLEVEL, GE_CMD_DITH0, GE_CMD_SCISSOR1, GE_CMD_REGION1,
    GE_CMD_NOP,
  };
  unsigned i;
  for (i = 0; i < sizeof(handled); ++i) cmd_class[handled[i]] |= CMD_HANDLED;
#if GP_LOG && GP_SUBMEASURE
  {
    static const u8 uncache[] = {
      GE_CMD_FRAMEBUFPTR, GE_CMD_FRAMEBUFWIDTH, GE_CMD_FRAMEBUFPIXFORMAT, GE_CMD_ZBUFPTR, GE_CMD_ZBUFWIDTH,
      GE_CMD_VIEWPORTXSCALE, GE_CMD_VIEWPORTYSCALE, GE_CMD_VIEWPORTXCENTER, GE_CMD_VIEWPORTYCENTER,
      GE_CMD_OFFSETX, GE_CMD_OFFSETY, GE_CMD_SCISSOR1, GE_CMD_SCISSOR2, GE_CMD_REGION1, GE_CMD_REGION2,
      GE_CMD_JUMP, GE_CMD_END, GE_CMD_FINISH, GE_CMD_SIGNAL, GE_CMD_ORIGIN, GE_CMD_TRANSFERSTART,
      GE_CMD_BEZIER, GE_CMD_SPLINE,
    };
    for (i = 0; i < sizeof(uncache); ++i) sub_uncache[uncache[i]] = 1;
  }
#endif
  /* words changed through changeWord: read as the game wrote them */
  cmd_class[GE_CMD_FRAMEBUFWIDTH] |= CMD_SAVED;
  cmd_class[GE_CMD_FRAMEBUFPIXFORMAT] |= CMD_SAVED;
  cmd_class[GE_CMD_TEXLEVEL] |= CMD_SAVED;
  /* a dropped draw, NOP until the GE is done. Upstream's NOP stayed: a
   * shared sublist (Tekken's clear) was dead from then on, every frame. */
  cmd_class[GE_CMD_NOP] |= CMD_SAVED;
  cmd_class[GE_CMD_TEXBUFWIDTH0] |= CMD_SAVED;
  cmd_class[GE_CMD_TEXADDR0] |= CMD_SAVED;
  cmd_class[GE_CMD_VIEWPORTXSCALE] |= CMD_SAVED; cmd_class[GE_CMD_VIEWPORTYSCALE] |= CMD_SAVED;
  cmd_class[GE_CMD_VIEWPORTXCENTER] |= CMD_SAVED; cmd_class[GE_CMD_VIEWPORTYCENTER] |= CMD_SAVED;
  cmd_class[GE_CMD_OFFSETX] |= CMD_SAVED; cmd_class[GE_CMD_OFFSETY] |= CMD_SAVED;
  cmd_class[GE_CMD_SCISSOR1] |= CMD_SAVED; cmd_class[GE_CMD_SCISSOR2] |= CMD_SAVED;
  cmd_class[GE_CMD_REGION1] |= CMD_SAVED; cmd_class[GE_CMD_REGION2] |= CMD_SAVED;
  /* (a CALL may be the guard band's, in a PRIM's place: savedOriginal) */
  if (GP_GUARD) cmd_class[GE_CMD_CALL] |= CMD_SAVED;
  cmd_class[GE_CMD_BONEMATRIXDATA] = cmd_class[GE_CMD_WORLDMATRIXDATA] = cmd_class[GE_CMD_VIEWMATRIXDATA] =
      cmd_class[GE_CMD_PROJMATRIXDATA] = cmd_class[GE_CMD_TGENMATRIXDATA] = CMD_MATRIX;
  cmd_class[GE_CMD_WORLDMATRIXNUMBER] = cmd_class[GE_CMD_VIEWMATRIXNUMBER] =
      cmd_class[GE_CMD_PROJMATRIXNUMBER] = CMD_MATRIX;
  /* Registers whose value the parser reads later (state.ge_cmd[...]). */
  {
    static const u8 kept[] = {
      GE_CMD_CLEARMODE, GE_CMD_CLUTFORMAT, GE_CMD_FRAMEBUFPIXFORMAT, GE_CMD_FRAMEBUFWIDTH,
      GE_CMD_SCISSOR1, GE_CMD_SCISSOR2, GE_CMD_TEXADDR0, GE_CMD_TEXBUFWIDTH0, GE_CMD_TEXFILTER,
      GE_CMD_TEXFORMAT, GE_CMD_TEXFUNC, GE_CMD_TEXMODE, GE_CMD_TEXSIZE0, GE_CMD_TEXTUREMAPENABLE,
      GE_CMD_TEXWRAP, GE_CMD_ZBUFPTR, GE_CMD_ZBUFWIDTH, GE_CMD_ZTEST, GE_CMD_ZTESTENABLE,
      GE_CMD_ZWRITEDISABLE, GE_CMD_ALPHABLENDENABLE, GE_CMD_BLENDMODE,
    };
    for (i = 0; i < sizeof(kept); ++i) cmd_class[kept[i]] |= CMD_KEEP;
  }
  /* the commands of a 3D scene, on their own short path (see patchGeList) */
  {
    static const u8 hot[] = {
      GE_CMD_BASE, GE_CMD_VADDR, GE_CMD_IADDR, GE_CMD_VERTEXTYPE, GE_CMD_OFFSETADDR, GE_CMD_ORIGIN,
      GE_CMD_BJUMP, GE_CMD_BOUNDINGBOX, GE_CMD_PRIM, GE_CMD_CALL, GE_CMD_RET, GE_CMD_JUMP, GE_CMD_NOP,
      /* (parseHot only: the generic short path leaves them to the full one) */
      GE_CMD_TEXADDR0, GE_CMD_TEXBUFWIDTH0, GE_CMD_TEXLEVEL, GE_CMD_STENCILTESTENABLE,
    };
    for (i = 0; i < sizeof(hot); ++i) cmd_class[hot[i]] |= CMD_HOT;
  }
  cmd_class[GE_CMD_BASE] |= CMD_INSET;
  cmd_class[GE_CMD_VADDR] |= CMD_INSET;
  cmd_class[GE_CMD_VERTEXTYPE] |= CMD_INSET;
}

/* Speed, one line every five seconds (always on in this build). */
static u32 perf_window_start, perf_parse_us, perf_sync_wait_us, perf_frames, perf_lists, perf_syncs, perf_commands;
/* where parse time goes: 2D vertex scaling, cache writeback, restoring after the GE */
static u32 perf_scale_us, perf_scaled_vertices, perf_2d_prims, perf_flush_us, perf_restore_us, perf_copy_us;
/* draws: 3D ones, and the ones dropped for their target or texture */
static u32 perf_3d_prims, perf_dropped_fb, perf_dropped_tex;
/* 3D draws checked against the guard band, those with a triangle dropped at
 * this scale that the PSP keeps, those not decided (behind the camera,
 * skinned...), and the time it took */
static u32 perf_guard_checked, perf_guard_dropped, perf_guard_other, perf_guard_us;
static GpMatrices matrices;
/* where parsing goes: matrix data, commands kept as they are, the rest
 * (commands - both) through the switch */
static u32 perf_matrix_words, perf_fast_words;
/* (GP_PROFILE) parseHot's share: its words and time, and the releases' */
static u32 prof_hot_cycles, prof_hot_words, prof_hot_calls, prof_rel_cycles, prof_rel_count;
static u32 prof_hot_cmd[256], prof_hot_stop[256], prof_bone_calls;

/* CPU cycles (COP0 Count) spent in the switch per command, and how many went
 * through it: where the full parse goes (control commands that leave the
 * loop are not counted) */
static u32 cmd_cycles[256], cmd_count[256];
/* GP_PROFILE: each parsed word's time by the way it went (0 matrix run, 1 not
 * handled, 2 handled: before, in and after its case), and the whole parse --
 * against parse_ms it gives the counter's rate */
static u32 sec_cycles[3], sec_mark, sec_last, parse_cycles;
static u32 pre_cycles, saved_cycles, inset_cycles; /* handled words before their case: all, savedOriginal, inset */
/* and each word's whole time, loop and all, by its command (word_cycles) */
static u32 word_cycles[256], word_count[256], sec_last_cmd;
#define SECTION(n, c) do { if (GP_PROFILE) { const u32 t_ = cpuCycles(); \
  sec_cycles[sec_last] += t_ - sec_mark; word_cycles[sec_last_cmd] += t_ - sec_mark; ++word_count[sec_last_cmd]; \
  sec_mark = t_; sec_last = (n); sec_last_cmd = (c); } } while (0)
static u32 prim_2d_cycles; /* the through draws' part of PRIM: scaling and inset */
static u32 prim_scale_cycles, prim_inset_cycles; /* ... of which scaling, and the UV inset */
/* GP_PROFILE 1: the [gp-cmd] profile; its counter reads cost ~0.1 s in 5 */
#ifndef GP_PROFILE
#define GP_PROFILE 0
#endif
static inline u32 cpuCycles(void) {
#if defined(__psp__) && GP_PROFILE
  u32 c;
  __asm__ volatile("mfc0 %0, $9" : "=r"(c));
  return c;
#else
  return 0;
#endif
}
#ifndef GP_TRACE
#define GP_TRACE 0
#endif
#if GP_TRACE
static unsigned int trace_enqueue_count, trace_sync_count, trace_display_count;
static void trace_state(const char *event, unsigned int count, unsigned int a, unsigned int b,
                        unsigned int c, unsigned int d) {
  if (count <= 8u || count % 120u == 0u)
    log("[gp-sync2] %s n=%u a=%08x b=%u c=%u d=%u copy=%u err=%u fb=%08x vtype=%06x ignore=%u/%u\n",
        event, count, a, b, c, d, copies_queued, copy_errors,
        (unsigned)state.framebuf, (unsigned)state.vertex_type,
        (unsigned)state.ignore_framebuf, (unsigned)state.ignore_texture);
}
#else
#define trace_state(...) ((void)0)
#endif


void resetGeState() {
  memset(&state, 0, sizeof(GeState));
  /* A new list (not a stall update: that one goes on with the state, and
   * its TEXBUFWIDTH0 can be in an earlier part) copies its VRAM textures
   * again: the game may have changed them, and a sync can be many lists
   * away (DW's movie: 20 copies in 88 frames, black until the GE was waited
   * for). */
  tbw0_word = tex0_word = NULL;
  vram_copied_count = 0;
}

static void insetAbort(void) {
  if (!state.pending) return;
  noteInsetSkip(9u, state.pending_primitive); /* next draw did not set its own words */
  gp_inset_rollback(&inset, state.pending_cursor);
  state.pending = 0;
}
/* A command that reads BASE: only safe once the game has set BASE again. */
static void insetBaseUser(void) {
  if (state.pending && !(state.pending_seen & 1u)) insetAbort();
  ++state.base_users;
}
static void insetNextDraw(void) {
  if (!state.pending) return;
  if ((state.pending_seen & 6u) == 6u &&
      gp_inset_commit(&inset, state.pending_words, state.pending_values, state.pending_count)) {
    unsigned k;
    for (k = 0; k < state.pending_count; ++k) noteWrite(state.pending_words[k], 4u);
    state.pending = 0;
  }
  else
    insetAbort();
}

int push(StackEntry *data) {
  if (state.curr_stack < (sizeof(state.stack) / sizeof(StackEntry))) {
    memcpy(&state.stack[state.curr_stack++], data, sizeof(StackEntry));
    return 0;
  }

  return -1;
}

StackEntry *pop() {
  if (state.curr_stack > 0)
    return &state.stack[--state.curr_stack];
  return NULL;
}
int findFramebuf(u32 framebuf) {
  int i;
  for (i = 0; i < state.framebuf_count; i++) {
    // Some textures point to within the framebuf.
    // If we want to be correct we should record the pixelsize, linewidth and height.
    if (framebuf >= state.framebuf_addr[i] && framebuf < (state.framebuf_addr[i] + 0x400)) {
      return i;
    }
  }

  return -1;
}

int insertFramebuf(u32 framebuf) {
  int res = findFramebuf(framebuf);
  if (res >= 0)
    return res;
  if (state.framebuf_count < (sizeof(state.framebuf_addr) / sizeof(u32))) {
    state.framebuf_addr[state.framebuf_count] = framebuf;
    return state.framebuf_count++;
  }

  return -1;
}

#if GP_TRACE
static unsigned pass_samples[4];
static void tracePassState(u32 primitive) {
  unsigned through = (state.vertex_type & GE_VTYPE_THROUGH_MASK) != 0;
  unsigned clear = (state.ge_cmd[GE_CMD_CLEARMODE] & 1u) != 0;
  unsigned format = state.ge_cmd[GE_CMD_TEXFORMAT] & 15u;
  unsigned texaddr = (state.ge_cmd[GE_CMD_TEXADDR0] & 0xFFFFF0u) |
                     ((state.ge_cmd[GE_CMD_TEXBUFWIDTH0] << 8) & 0x0F000000u);
  unsigned cls = clear ? 0u : !through ? 1u :
      (format >= 4u && format <= 7u && texaddr >= FAKE_VRAM && texaddr < DISPLAY_BUFFER) ? 2u : 3u;
  unsigned sample = pass_samples[cls];
  u32 stride, position, marker;
  if (sample >= 8u) return;
  pass_samples[cls] = ++sample;
  getVertexInfo(state.vertex_type, &stride, &position, &marker);
  log("[gp-pass3] c=%u n=%u prim=%u verts=%u fb=%08x fmt=%u z=%06x/%06x test=%06x/%06x zw=%u clear=%06x ig=%u/%u vt=%06x stride=%u pad=%u\n",
      cls, sample, (unsigned)(primitive >> 16), (unsigned)(primitive & 0xFFFFu),
      (unsigned)state.framebuf, (unsigned)(state.ge_cmd[GE_CMD_FRAMEBUFPIXFORMAT] & 3u),
      (unsigned)state.ge_cmd[GE_CMD_ZBUFPTR], (unsigned)state.ge_cmd[GE_CMD_ZBUFWIDTH],
      (unsigned)state.ge_cmd[GE_CMD_ZTESTENABLE], (unsigned)state.ge_cmd[GE_CMD_ZTEST],
      (unsigned)(state.ge_cmd[GE_CMD_ZWRITEDISABLE] & 1u),
      (unsigned)state.ge_cmd[GE_CMD_CLEARMODE], (unsigned)state.ignore_framebuf,
      (unsigned)state.ignore_texture, (unsigned)state.vertex_type, (unsigned)stride, (unsigned)marker);
  log("[gp-tex3] c=%u n=%u tex=%08x fmt=%u size=%06x bw=%06x tfx=%06x clut=%06x scissor=%06x/%06x\n",
      cls, sample, texaddr, format, (unsigned)state.ge_cmd[GE_CMD_TEXSIZE0],
      (unsigned)state.ge_cmd[GE_CMD_TEXBUFWIDTH0], (unsigned)state.ge_cmd[GE_CMD_TEXFUNC],
      (unsigned)state.ge_cmd[GE_CMD_CLUTFORMAT], (unsigned)state.ge_cmd[GE_CMD_SCISSOR1],
      (unsigned)state.ge_cmd[GE_CMD_SCISSOR2]);
}
#else
#define tracePassState(primitive) ((void)0)
#endif

void AdvanceVerts(int count, int vertex_size) {
  if ((state.vertex_type & GE_VTYPE_IDX_MASK) != GE_VTYPE_IDX_NONE) {
    int index_shift = ((state.vertex_type & GE_VTYPE_IDX_MASK) >> GE_VTYPE_IDX_SHIFT) - 1;
    state.index_addr += count << index_shift;
  } else {
    state.vertex_addr += count * vertex_size;
  }
}
static u32 currentTexture(void) {
  return ((state.ge_cmd[GE_CMD_TEXBUFWIDTH0] & 0x0f0000) << 8) | (state.ge_cmd[GE_CMD_TEXADDR0] & 0xffffff);
}

/* Builds the float-UV copy and prepares the three word values; they are only
 * written once the next draw proves it sets BASE, VADDR and VTYPE itself. */
static unsigned insetCandidate(u32 primitive, u32 stride, u32 position) {
  uint32_t *const words[3] = {(uint32_t *)state.base_word, (uint32_t *)state.vaddr_word,
                              (uint32_t *)state.vtype_word};
  GpInsetSource source;
  GpInsetVertex *first;
  unsigned cursor = inset.used_vertices, k, ours = 0;
  u32 address, low;
  for (k = 0; k < 3u; ++k) ours += gp_inset_value(&inset, words[k]) != 0u;
  if (ours) {
    /* The same list parsed again before completion: already redirected. */
    if (ours == 3u) { ++inset.reused; return 0; }
    ++inset.unsupported;
    return 6;
  }
  source.type = state.vertex_type;
  source.primitive = primitive;
  source.vertices = state.vertex_addr;
  source.stride = stride;
  source.position = position;
  source.texture = currentTexture();
  if (!gp_inset_build(&inset, &source, &first, 1)) return 5;
  address = (u32)first & 0x0fffffff;
  low = address & 0x00ffffffu;
  if (state.vaddr_offset > low) { gp_inset_rollback(&inset, cursor); ++inset.unsupported; return 7; }
  state.pending_words[0] = words[0];
  state.pending_words[1] = words[1];
  state.pending_words[2] = words[2];
  state.pending_values[0] = (GE_CMD_BASE << 24) | ((address >> 8) & 0x0f0000);
  state.pending_values[1] = (GE_CMD_VADDR << 24) | (low - state.vaddr_offset);
  state.pending_values[2] = (GE_CMD_VERTEXTYPE << 24) | GP_INSET_VTYPE;
  state.pending_count = 3;
  state.pending_cursor = cursor;
  state.pending_primitive = primitive;
  state.pending_seen = 0;
  state.pending = 1;
  return 0;
}

/* A draw that sets neither VADDR nor VTYPE continues where the inset draw's
 * vertices ended -- in our copy, with our layout (sceGuDrawArray with NULL
 * vertices). Its own vertices go right after, so the GE reads exactly them. */
static int insetExtend(u32 primitive, u32 stride, u32 position, int clamp) {
  GpInsetSource source;
  GpInsetVertex *first;
  source.type = state.vertex_type;
  source.primitive = primitive;
  source.vertices = state.vertex_addr;
  source.stride = stride;
  source.position = position;
  source.texture = currentTexture();
  return gp_inset_build(&inset, &source, &first, clamp) != 0u;
}

/* A draw with its own BASE and VADDR but no VTYPE of its own reads the inset
 * draw's VTYPE word -- our layout. Tekken draws its tiles this way: one VTYPE,
 * then BASE/VADDR/PRIM per tile. The whole run is redirected (each draw to its
 * own copy) or, if one draw cannot be, none of it. */
static int insetJoinRun(u32 primitive, u32 stride, u32 position, int clamp) {
  uint32_t *words[2] = {(uint32_t *)state.base_word, (uint32_t *)state.vaddr_word};
  GpInsetSource source;
  GpInsetVertex *first;
  u32 address, low;
  if (state.pending_count + 2u > 3u + 2u * INSET_RUN_DRAWS) return 0;
  if (gp_inset_value(&inset, words[0]) || gp_inset_value(&inset, words[1])) return 0;
  source.type = state.vertex_type;
  source.primitive = primitive;
  source.vertices = state.vertex_addr;
  source.stride = stride;
  source.position = position;
  source.texture = currentTexture();
  if (!gp_inset_build(&inset, &source, &first, clamp)) return 0;
  address = (u32)first & 0x0fffffff;
  low = address & 0x00ffffffu;
  if (state.vaddr_offset > low) return 0; /* rolled back with the run */
  state.pending_words[state.pending_count] = words[0];
  state.pending_values[state.pending_count++] = (GE_CMD_BASE << 24) | ((address >> 8) & 0x0f0000);
  state.pending_words[state.pending_count] = words[1];
  state.pending_values[state.pending_count++] = (GE_CMD_VADDR << 24) | (low - state.vaddr_offset);
  state.pending_seen = 0;
  return 1;
}

/* Once: the first large textured sprite batch (a tiled picture, like the
 * title movie), so its tile layout can be read from the log. */
static void dumpSpriteBatch(u32 primitive, u32 stride, u32 position) {
  static int dumped;
  GpInsetSource source;
  unsigned i, count = primitive & 0xffffu;
  if (dumped || (primitive >> 16) != 6u || count < 32u) return;
  dumped = 1;
  source.type = state.vertex_type; source.primitive = primitive;
  source.vertices = state.vertex_addr; source.stride = stride; source.position = position;
  log("[gp-sprites] count=%u vt=%06x tex=%06x/%06x size=%06x fmt=%u filter=%06x wrap=%06x func=%06x\n",
      count, (unsigned)state.vertex_type, (unsigned)state.ge_cmd[GE_CMD_TEXADDR0],
      (unsigned)state.ge_cmd[GE_CMD_TEXBUFWIDTH0], (unsigned)state.ge_cmd[GE_CMD_TEXSIZE0],
      (unsigned)(state.ge_cmd[GE_CMD_TEXFORMAT] & 15u), (unsigned)state.ge_cmd[GE_CMD_TEXFILTER],
      (unsigned)state.ge_cmd[GE_CMD_TEXWRAP], (unsigned)state.ge_cmd[GE_CMD_TEXFUNC]);
  for (i = 0; i + 1u < count && i < 40u; i += 2u) {
    GpInsetVertex a, b;
    if (!gp_inset_read(&source, i, &a) || !gp_inset_read(&source, i + 1u, &b)) break;
    log("[gp-sprites] %u xy=%d,%d-%d,%d uv=%d,%d-%d,%d\n", i / 2u, (int)a.x, (int)a.y, (int)b.x, (int)b.y,
        (int)a.u, (int)a.v, (int)b.u, (int)b.v);
  }
}

// TODO: when ignore_framebuf=1 or ignore_texture=1, dummy all non-control-flow instructions
/* A 3D draw with triangles the GE drops here and the PSP keeps (guard band):
 * its PRIM word becomes a CALL of a short list that draws it as it was, then
 * those triangles cut to the guard band (from GUARD_VERTICES), and puts back
 * VADDR/IADDR, VTYPE, BASE and OFFSETADDR as the draw left them. The list
 * lives in this module's memory: in the kernel's first megabytes, which a CALL
 * with the usual BASE (0x08) reaches. The PRIM word comes back with the rest
 * once the GE is done (changeWord). */
static u32 *emitCommand(u32 *p, unsigned cmd, u32 data);
#ifndef GP_GUARD_CALL
#define GP_GUARD_CALL 1
#endif
/* the straightened copy's pool from 0x0A700000 (256 KB), the cut vertices after it */
#define GUARD_VERTICES 0x0A740000u
#define GUARD_VERTEX_BYTES (RENDER_LIST - 0x20000u - GUARD_VERTICES)
#define GUARD_COMMANDS 3072u
/* The CALLed lists: in this module (the kernel's first megabytes, which a
 * CALL with the usual BASE 0x08 reaches). 28.10 the GE stopped at the first
 * such CALL, taken then for the GE not reading the kernel's memory -- it
 * reads its own lists there; more likely the CALL was a third level, which
 * the GE refuses (uOFW: two, 30.10): a draw two CALLs deep is not cut. */
static u32 guard_list_memory[GUARD_COMMANDS];
static u32 *const guard_commands = guard_list_memory;
static u32 perf_guard_deep;
static u32 guard_vertex_used, guard_command_used, perf_guard_cut, perf_guard_vertices, perf_guard_full;
/* The check cost up to 0.9 s in 5 of a DW battle (15 instead of 30 fps,
 * 28.10): to be made cheaper once the cut is seen to work. */
static void guardBand(u32 *word, u32 prim) {
  const u32 vt = state.vertex_type, count = prim & 0xffffu, type = (prim >> 16) & 7u;
  u32 vertex_size = 0, pos_off = 0, visit_off = 0, lower = 0, upper = count, index_size = 0;
  const void *indices = NULL;
  u32 started;
  GpGuardDraw draw;
  if (type < 3u || type > 5u || count < 3u) return;
  if ((vt & GE_VTYPE_WEIGHT_MASK) || (vt & GE_VTYPE_MORPHCOUNT_MASK)) { ++perf_guard_other; return; }
  started = sceKernelGetSystemTimeLow();
  getVertexInfo(vt, &vertex_size, &pos_off, &visit_off);
  index_size = (vt & GE_VTYPE_IDX_MASK) >> GE_VTYPE_IDX_SHIFT;
  if (index_size) {
    index_size = 1u << (index_size - 1u);
    if (state.index_addr < 0x08400000u || (uint64_t)state.index_addr + (uint64_t)count * index_size > 0x0A400000u) return;
    indices = (const void *)state.index_addr;
    GetIndexBounds(indices, count, vt, &lower, &upper);
    if (upper == 0xFFFFFFFFu) return;
    ++upper;
  }
  if (!vertex_size || state.vertex_addr < 0x08400000u ||
      (uint64_t)state.vertex_addr + (uint64_t)upper * vertex_size > 0x0A400000u) return;
  draw.vertices = (const uint8_t *)state.vertex_addr;
  draw.vtype = vt;
  draw.prim = type;
  draw.count = count;
  draw.indices = indices;
  draw.index_size = index_size;
  draw.limit_x = 2048.0f / (float)(WIDTH / 2);
  draw.limit_y = 2048.0f / (float)(HEIGHT / 2);
  draw.psp_x = draw.limit_x * (float)gp_scale_num / (float)gp_scale_den;
  draw.psp_y = draw.limit_y * (float)gp_scale_num_y / (float)gp_scale_den;
  ++perf_guard_checked;
  {
    const u32 out_size = gp_guard_out_size(vt);
    const u32 capacity = (GUARD_VERTEX_BYTES - guard_vertex_used) / out_size;
    const u32 list_phys = (u32)&guard_commands[guard_command_used] & 0x0fffffffu;
    const u32 rel = list_phys - state.base - state.offset;
    uint8_t *out = (uint8_t *)(GUARD_VERTICES + guard_vertex_used);
    u32 n, *p, vend, iend;
    if (state.curr_stack > 1u) ++perf_guard_deep; /* (our CALL would be a third level) */
    if (state.curr_stack > 1u || guard_command_used + 24u > GUARD_COMMANDS || rel >= 0x01000000u) {
      if (gp_guard_prim(&matrices, &draw) == GP_GUARD_DROPPED_HERE) { ++perf_guard_dropped; ++perf_guard_full; }
      perf_guard_us += sceKernelGetSystemTimeLow() - started;
      return;
    }
    n = gp_guard_cut(&matrices, &draw, out, capacity);
    if (n) {
      static unsigned traced;
      if (traced < 20u) { /* the first cuts, written before the GE sees them */
        ++traced;
        log("[gp-cut] %u word=%08x prim=%06x vt=%06x vaddr=%08x iaddr=%08x base=%08x offset=%08x n=%u list=%08x\n",
            traced, (unsigned)word, (unsigned)prim, (unsigned)vt, (unsigned)state.vertex_addr,
            (unsigned)state.index_addr, (unsigned)state.base, (unsigned)state.offset, (unsigned)n, (unsigned)list_phys);
      }
    }
    if (n) {
      const u32 vphys = (u32)out & 0x0fffffffu;
      ++perf_guard_dropped; ++perf_guard_cut; perf_guard_vertices += n;
      noteWrite(out, n * out_size);
      guard_vertex_used += (n * out_size + 15u) & ~15u;
      vend = index_size ? state.vertex_addr : state.vertex_addr + count * vertex_size;
      iend = state.index_addr + count * index_size;
      p = &guard_commands[guard_command_used];
      p = emitCommand(p, GE_CMD_PRIM, prim);                      /* the draw as it was */
      p = emitCommand(p, GE_CMD_OFFSETADDR, 0);
      p = emitCommand(p, GE_CMD_BASE, (vphys >> 8) & 0x0f0000u);
      p = emitCommand(p, GE_CMD_VADDR, vphys & 0xffffffu);
      p = emitCommand(p, GE_CMD_VERTEXTYPE, gp_guard_out_vtype(vt));
      p = emitCommand(p, GE_CMD_PRIM, (3u << 16) | n);            /* the cut triangles */
      p = emitCommand(p, GE_CMD_VERTEXTYPE, vt);
      p = emitCommand(p, GE_CMD_BASE, (vend >> 8) & 0x0f0000u);
      p = emitCommand(p, GE_CMD_VADDR, vend & 0xffffffu);
      if (index_size) {
        p = emitCommand(p, GE_CMD_BASE, (iend >> 8) & 0x0f0000u);
        p = emitCommand(p, GE_CMD_IADDR, iend & 0xffffffu);
      }
      p = emitCommand(p, GE_CMD_BASE, state.base >> 8);
      p = emitCommand(p, GE_CMD_OFFSETADDR, state.offset >> 8);
      p = emitCommand(p, GE_CMD_RET, 0);
      noteWrite(&guard_commands[guard_command_used], (u32)(p - &guard_commands[guard_command_used]) * 4u);
#if GP_GUARD_CALL /* 0: everything but the CALL (DW hung at its first 3D frame with it) */
      changeWord(word, (GE_CMD_PRIM << 24) | prim, (GE_CMD_CALL << 24) | (rel & 0xfffffcu));
#else
      (void)word;
#endif
      noteWrite(word, 4u);
      guard_command_used = (u32)(p - guard_commands + 15u) & ~15u;
    } else if (gp_guard_prim(&matrices, &draw) == GP_GUARD_DROPPED_HERE) {
      ++perf_guard_dropped; ++perf_guard_full;
    }
  }
  perf_guard_us += sceKernelGetSystemTimeLow() - started;
}

/* R + SELECT (Adrenaline's snapshot then): the next frame's targets,
 * transfers, textures and draws in the log -- how a game builds its frame.
 * Kept in memory and written after the frame: lists may be queued where no
 * file can be written (this game's were: none of 400 lines reached the log). */
static u32 dump_frame, dump_lines;
#define DUMP_LINES 3000u /* Tekken spent 400 on its HUD before the 3D */
/* draw: `look` is the blend mode (24 bits) + 1 << 24 blending, 2 << 24 depth
 * test, 4 << 24 no depth write */
typedef struct { u32 word, op, vt, look, fb, ignore, tex, repeat; } DumpLine;
static DumpLine *dump_buffer; /* in upper RAM: 3000 x 32 bytes, after the log's */
#define DUMP_BUFFER 0x0A8E0000u
static void dumpCommand(const u32 *word, u32 op) {
  const u32 cmd = op >> 24;
  DumpLine *d;
  u32 tex, ignore, look;
  /* Only what changes rarely: Tekken's fight sets texture, blend and depth
   * for each of its draws and did not fit; those go on the draw's line. */
  switch (cmd) {
    case GE_CMD_FRAMEBUFPTR: case GE_CMD_FRAMEBUFWIDTH: case GE_CMD_FRAMEBUFPIXFORMAT:
    case GE_CMD_TRANSFERSRC: case GE_CMD_TRANSFERSRCW: case GE_CMD_TRANSFERDST: case GE_CMD_TRANSFERDSTW:
    case GE_CMD_TRANSFERSRCPOS: case GE_CMD_TRANSFERDSTPOS: case GE_CMD_TRANSFERSIZE: case GE_CMD_TRANSFERSTART:
    case GE_CMD_CLEARMODE: case GE_CMD_SCISSOR1: case GE_CMD_SCISSOR2: case GE_CMD_REGION2:
    case GE_CMD_VIEWPORTXSCALE: case GE_CMD_OFFSETX: case GE_CMD_PRIM:
    case GE_CMD_VIEWPORTYSCALE: case GE_CMD_VIEWPORTXCENTER: case GE_CMD_VIEWPORTYCENTER: case GE_CMD_OFFSETY:
    case GE_CMD_REGION1: case GE_CMD_ZBUFPTR: case GE_CMD_ZBUFWIDTH:
    case GE_CMD_BEZIER: case GE_CMD_SPLINE: case GE_CMD_PATCHPRIMITIVE:
      break;
    default:
      return;
  }
  tex = (state.ge_cmd[GE_CMD_TEXTUREMAPENABLE] & 1u) ? currentTexture() : 0u;
  ignore = state.ignore_framebuf | state.ignore_texture << 1;
  look = (state.ge_cmd[GE_CMD_BLENDMODE] & 0xffffffu) | (state.ge_cmd[GE_CMD_ALPHABLENDENABLE] & 1u) << 24 |
         (state.ge_cmd[GE_CMD_ZTESTENABLE] & 1u) << 25 | (state.ge_cmd[GE_CMD_ZWRITEDISABLE] & 1u) << 26;
  if (cmd == GE_CMD_PRIM && dump_lines) { /* draws like the last one: counted on its line */
    DumpLine *last = &dump_buffer[dump_lines - 1];
    const int both_3d = !(last->vt & GE_VTYPE_THROUGH_MASK) && !(state.vertex_type & GE_VTYPE_THROUGH_MASK);
    if ((last->op >> 24) == GE_CMD_PRIM && last->fb == state.framebuf && last->ignore == ignore &&
        (both_3d || (last->vt == state.vertex_type && last->tex == tex && last->look == look &&
                     ((last->op >> 16) & 7u) == ((op >> 16) & 7u)))) { /* 3D draws: one line */
      ++last->repeat;
      return;
    }
  }
  if (dump_lines >= DUMP_LINES) return;
  d = &dump_buffer[dump_lines++];
  d->repeat = 0;
  d->word = (u32)word; d->op = op; d->vt = state.vertex_type; d->look = look;
  d->fb = state.framebuf; d->ignore = ignore; d->tex = tex;
}
static void writeDump(void) {
  unsigned i;
  for (i = 0; i < dump_lines; ++i) {
    const DumpLine *d = &dump_buffer[i];
    if ((d->op >> 24) == GE_CMD_PRIM) { /* log() is a block */
      log("[gp-frame] %08x PRIM type=%u count=%u vt=%06x look=%07x fb=%08x ignore=%u tex=%08x +%u\n", (unsigned)d->word,
          (unsigned)((d->op >> 16) & 7u), (unsigned)(d->op & 0xffffu), (unsigned)d->vt, (unsigned)d->look,
          (unsigned)d->fb, (unsigned)d->ignore, (unsigned)d->tex, (unsigned)d->repeat);
    } else {
      log("[gp-frame] %08x cmd=%02x data=%06x\n", (unsigned)d->word, (unsigned)(d->op >> 24), (unsigned)(d->op & 0xffffffu));
    }
  }
}

/* The game's last values of the screen registers (24-bit data). */
static u32 game_vp[4] = {0x439800u /* 240.0 */, 0xc30800u /* -136.0 */, 0x450000u /* 2048.0 */, 0x450000u},
           game_offset[2] = {(2048u - 240u) << 4, (2048u - 136u) << 4},
           game_scissor[4] = {0, (271u << 10) | 479u, 0, (271u << 10) | 479u}; /* scissor 1 2, region 1 2 */
static void noteGameScreen(u32 cmd, u32 data) {
  switch (cmd) {
    case GE_CMD_VIEWPORTXSCALE: game_vp[0] = data; break;
    case GE_CMD_VIEWPORTYSCALE: game_vp[1] = data; break;
    case GE_CMD_VIEWPORTXCENTER: game_vp[2] = data; break;
    case GE_CMD_VIEWPORTYCENTER: game_vp[3] = data; break;
    case GE_CMD_OFFSETX: game_offset[0] = data; break;
    case GE_CMD_OFFSETY: game_offset[1] = data; break;
    case GE_CMD_SCISSOR1: game_scissor[0] = data; break;
    case GE_CMD_SCISSOR2: game_scissor[1] = data; break;
    case GE_CMD_REGION1: game_scissor[2] = data; break;
    case GE_CMD_REGION2: game_scissor[3] = data; break;
    default: break;
  }
}
static float gpFloat(u32 data) { union { u32 i; float f; } v; v.i = data << 8; return v.f; }
static u32 gpData(float f) { union { u32 i; float f; } v; v.f = f; return v.i >> 8; }
/* A screen coordinate (0..1023) at the current scale; `end`: an inclusive
 * last pixel, whose next pixel scales. Clamped to the draw buffer. */
static u32 scaleCoordinate(u32 v, int end, u32 limit, u32 num) {
  u32 r = end ? ((v + 1u) * num) / gp_scale_den - 1u : (v * num) / gp_scale_den;
  return r > limit ? limit : r;
}
/* Our value of a screen register: screen pixel x on the PSP, center + scale
 * * ndc - offset, becomes factor times that. The GE's offset stays ours,
 * 2048 - WIDTH / 2 (games use 0 too: scaled, it went negative and Tekken's
 * fight was off the screen), and the center carries the game's offset:
 * center' = f (center - offset) + 2048 - WIDTH / 2. A game's OFFSETX word
 * becomes a VIEWPORTXCENTER from it (the princess game moves its character
 * window with OFFSETX after the center): either order comes out right. */
static u32 screenValue(u32 cmd) {
  const float factor = (float)gp_scale_num / (float)gp_scale_den, factor_y = (float)gp_scale_num_y / (float)gp_scale_den;
  switch (cmd) {
    case GE_CMD_VIEWPORTXSCALE: return gpData(gpFloat(game_vp[0]) * factor);
    case GE_CMD_VIEWPORTYSCALE: return gpData(gpFloat(game_vp[1]) * factor_y);
    case GE_CMD_VIEWPORTXCENTER:
      return gpData(factor * (gpFloat(game_vp[2]) - (float)(game_offset[0] & 0xffffu) / 16.0f) + (float)(2048 - WIDTH / 2));
    case GE_CMD_VIEWPORTYCENTER:
      return gpData(factor_y * (gpFloat(game_vp[3]) - (float)(game_offset[1] & 0xffffu) / 16.0f) + (float)(2048 - HEIGHT / 2));
    case GE_CMD_OFFSETX: return (2048 - (WIDTH / 2)) << 4;
    case GE_CMD_OFFSETY: return (2048 - (HEIGHT / 2)) << 4;
    default: {
      const u32 v = game_scissor[cmd == GE_CMD_SCISSOR1 ? 0 : cmd == GE_CMD_SCISSOR2 ? 1 : cmd == GE_CMD_REGION1 ? 2 : 3];
      const int end = cmd == GE_CMD_SCISSOR2 || cmd == GE_CMD_REGION2;
      return (scaleCoordinate((v >> 10) & 0x3ffu, end, HEIGHT - 1u, gp_scale_num_y) << 10) |
             scaleCoordinate(v & 0x3ffu, end, WIDTH - 1u, gp_scale_num);
    }
  }
}

/* A draw into a buffer we do not keep, or sampling one: NOPed (upstream).
 * Not a clear sampling one -- clear mode reads no texture: Tekken's depth
 * clear came after its bloom sprite with texturing still on. */
static int dropsDraw(void) {
  return state.ignore_framebuf || (state.ignore_texture && (state.ge_cmd[GE_CMD_TEXTUREMAPENABLE] & 1u) &&
                                   !(state.ge_cmd[GE_CMD_CLEARMODE] & 1u));
}

/* A list released to the GE while it is parsed (see sceGeListEnQueuePatched):
 * the GE stops at the stall address (PPSSPP: pc == stall), which is put on the
 * first main-list word not parsed yet, so the GE draws the start of a frame
 * while the rest is parsed -- Tekken builds its frame, then enqueues it whole,
 * and the parse (8-12 ms) came on top of the GE's time. Moved on only at the
 * main list, all it CALLed before parsed and written back; not while a BJUMP
 * there may skip past it, after a SIGNAL, nor once an inset copy was made
 * (its tiles are joined at the end). */
extern int (*_sceGeListUpdateStallAddr)(int qid, void *stall);
/* Only for the straightened copy of a main list (linearizeList): on Tekken's
 * own -- JUMPs back and forth between chunks -- the GE ran past the stall into
 * words not parsed yet (camera, HUD wrong; the user). */
/* Off (30.10). The straightened copy with releasing drew more frames on the
 * heavy arena (160-165 in 5 s against 115-124), yet the game ran slower (the
 * user): Tekken catches up with logic steps on the same CPU, and parse + copy
 * (~22 ms a frame, at more frames) left it less time than the parse in place
 * (~16 ms) -- what counts is the CPU a frame, not the GE starting early. The
 * groups (in place, released) broke the picture, why not found (no stall
 * passed, no word the GE ran rewritten after, BJUMPs not taken). Both kept,
 * and checked by the tests. */
static int release_enabled = 0, release_qid = -1;
/* ... but for short lists: menus, dialogs, fades (~1.2k commands a frame,
 * the copy ~0.1 ms) -- there the GE waited ~3.4 ms a frame for the parse to
 * end, and a frame of 15-20 ms missed its VBlank now and then: the fades
 * stepped (the user; smooth without GePatch). A main list is short when the
 * last one was (a fight's are ~85k). */
#define SHORT_LIST_COMMANDS 16384u
#define SHORT_RELEASE_EVERY 256u
static int short_release = 1;
static u32 last_list_commands = 0xffffffffu;
static u32 release_high, release_count_at, release_blocked, release_every = 4096u, perf_releases;
static const u32 *release_bjump_until, *parse_end;
static unsigned release_pool_start;
/* (GP_LOG) the lists released: stall updates, the first one's time after the
 * enqueue, and what held the releases back */
static u32 rel_lists, rel_first_us, rel_first_count, rel_enqueued_at, rel_first_done;
static u32 rel_held_blocked, rel_held_inset, rel_held_pending, rel_held_bjump, rel_block_code[4];
static int group_live; /* (below) */
/* Words the parse may still change behind it (a release there would let the
 * GE run them first): a draw's BASE/VADDR/VTYPE set since the last draw, its
 * BASE used by its VADDR alone so far -- an inset 2D draw points them at our
 * pool when its PRIM comes (released
 * between them, the fades' tiles came out as squares and stripes: the user,
 * 30.10); while a movie plays, the TEXBUFWIDTH0/TEXADDR0 a movie picture
 * points at real VRAM. (A FRAMEBUFWIDTH was one: made ours at once now.
 * Hardware, 30.10: the GE stops where its address is the stall, reached in
 * order or by a JUMP alike.) */
static int parsePending(void) {
  /* (an inset needs its BASE used by its VADDR only: since_draw 7, base_users 1) */
  const u32 since = state.since_draw, users = state.base_users;
  return ((since & 1u) && (users == 0u || (users == 1u && (since & 2u)))) || movie_depth;
}
static void releaseParsed(const u32 *next, unsigned commands) {
  if (group_live) return; /* (released at the groups' ends: groupRelease) */
  if (release_bjump_until && next >= release_bjump_until) release_bjump_until = NULL; /* (passed) */
  if (commands - release_count_at < release_every) return;
  if (release_blocked || state.pending || inset.used_vertices != release_pool_start || parsePending()) {
    if (GP_LOG) {
      if (release_blocked) ++rel_held_blocked;
      else if (state.pending) ++rel_held_pending;
      else ++rel_held_inset;
    }
    return;
  }
  if (release_bjump_until) { if (GP_LOG) ++rel_held_bjump; return; }
  if (GP_LOG && !rel_first_done) {
    rel_first_done = 1;
    rel_first_us += sceKernelGetSystemTimeLow() - rel_enqueued_at;
    ++rel_first_count;
  }
  const u32 rel_started = cpuCycles();
  if (legacy_cache) sceKernelDcacheWritebackAll();
  else flushWrites();
  _sceGeListUpdateStallAddr(release_qid, (void *)(release_high | (u32)next));
  if (GP_PROFILE) { prof_rel_cycles += cpuCycles() - rel_started; ++prof_rel_count; }
  release_count_at = commands;
  ++perf_releases;
}
/* A JUMP of the main list (at `list`): with a BJUMP's target still ahead in
 * this part of it, where the GE may land is not in order any more -- no
 * releasing. A target already passed is no hold. (It was cleared only at a
 * release, every 4096 words: Tekken's main list BJUMPs, passes the target,
 * JUMPs -- nothing was ever released, the GE waited for the whole parse.) */
static void releaseJump(const u32 *list) {
  if (!release_bjump_until || group_live) return; /* (the groups: groupJump) */
  if (list >= release_bjump_until) release_bjump_until = NULL;
  else release_blocked = 2;
}
/* A BJUMP of the main list (`target`): no release before the GE could land there. */
static void releaseBjump(const u32 *list, u32 target) {
  const u32 *to = (const u32 *)target;
  if (group_live) return; /* (the groups: groupBjump) */
  if (to <= list) release_blocked = 1; /* (codes, for the log: 1 BJUMP back, 2 JUMP, 3 SIGNAL) */
  else if (!release_bjump_until || to > release_bjump_until) release_bjump_until = to;
}


/* The main list run where it is, by way of a short list of ours (in
 * LINEAR_LIST): the JUMP ending each chunk turned, until the GE is done, into
 * a JUMP to an entry of ours that is the game's JUMP as it was -- no copy of
 * ~70k words a frame (the straightened copy cost 5-6 ms). Not CALLs: the GE
 * nests two CALLs and refuses a third (uOFW ge.c: RADR1/RADR2 only), and the
 * game's sublists call sublists. The GE is given our list stalled at the
 * entry after a group parsed whole, so it never JUMPs on into a chunk not
 * parsed -- by way of a NOP before it: the GE stops at a stall it comes to
 * in order, not one a JUMP lands on (29.10: the game's own list released,
 * the GE JUMPed past the stall into words not parsed). A BJUMP whose landing is in a later chunk (Tekken: ~2 a frame)
 * keeps the chunks up to it in one group, their JUMPs as they are. The GE's
 * offset is the chunk's at our entry, as at the JUMP; its BASE is ours when
 * the chunk's has to be (the BASE word just before the JUMP made ours; the
 * entry sets the game's again). From a list that sets its BASE before using
 * one, with BASE and offset 0 set first. A BJUMP back, a SIGNAL, a JUMP whose
 * BASE cannot be ours: no group closed after it -- the rest one group. */
static int group_enabled = 1, group_live, group_aborted;
static u32 *grp_top, *grp_top_end, *grp_flushed, *grp_stall;
static const u32 *grp_piece, *grp_open[8];
static unsigned grp_opens;
#define grp_word_at ((u32 **)(uintptr_t)GROUP_TABLES)
#define grp_word_orig ((u32 *)(uintptr_t)(GROUP_TABLES + GRP_WORDS * sizeof(void *)))
#define grp_word_ours (grp_word_orig + GRP_WORDS)
static unsigned grp_words;
static u32 grp_lists, grp_groups, grp_merged, grp_aborts, grp_releases;
/* the chunks parsed: of the groups closed (a BJUMP there: back into what the
 * GE may have run -- given up), and of the group still open (a BJUMP there:
 * a loop within it, as the GE will run it) */
#define grp_done_lo ((const u32 **)(uintptr_t)(GROUP_TABLES + GRP_WORDS * (sizeof(void *) + 8u)))
#define grp_done_hi (grp_done_lo + GRP_DONE)
static const u32 *grp_cur_lo[32], *grp_cur_hi[32];
static unsigned grp_done, grp_cur;
static int groupIn(const u32 *const *lo, const u32 *const *hi, unsigned n, const u32 *at) {
  unsigned k;
  for (k = 0; k < n; ++k) if (at >= lo[k] && at <= hi[k]) return 1;
  return 0;
}
/* (GP_LOG: why, and the first of a log's period: where) */
static u32 grp_why[6], grp_ab_at, grp_ab_word, grp_ab_prev, grp_ab_base, grp_ab_offset, grp_ab_to, grp_bjumps;
static void groupAbortAt(unsigned why, const u32 *at, u32 base, u32 offset, u32 to) {
  if (!group_live || group_aborted) return;
  group_aborted = 1;
  ++grp_aborts;
  if (GP_LOG) {
    ++grp_why[why];
    if (!grp_ab_at) {
      grp_ab_at = (u32)at; grp_ab_word = at ? at[0] : 0u; grp_ab_prev = at ? at[-1] : 0u;
      grp_ab_base = base | why; grp_ab_offset = offset; grp_ab_to = to;
    }
  }
}
#define groupAbort() groupAbortAt(4u, NULL, 0u, 0u, 0u) /* (a SIGNAL) */
static void groupSet(u32 *at, u32 ours) {
  grp_word_at[grp_words] = at;
  grp_word_orig[grp_words] = *at;
  grp_word_ours[grp_words++] = ours;
  *at = ours;
  noteWrite(at, 4u);
}
/* Off: released group by group, the picture broke on hardware (stripes, the
 * camera, the HUD HP -- the GE ahead of the parse, how not yet known); the GE
 * given the list once parsed, Tekken ran at full speed (the user, 30.10). */
static int group_release = 0;
#if GP_LOG
/* (GP_LOG) What the GE may have run, and the parse changing it after: at
 * each release the GE's way from the last stall to the new one is walked
 * (BJUMPs not taken) and kept as runs -- list words run in order, and the
 * vertices its non-indexed draws read; a write the parse notes into one
 * after is logged (kind 1 list, 2 vertices): the GE had the old one. */
/* a bit per 8 bytes of 0x08800000..0x0A800000: 512 KB, in the guard pool after
 * the groups' list */
#define RETRO_BITS ((u8 *)0x0A740000u) /* (the guard band's pool: these probes are for group releases only) */
#define RETRO_LO 0x08800000u
#define RETRO_HI 0x0A800000u
static const u32 *rw_pc, *rw_stack[8], *rw_run_start;
static u32 rw_offset_stack[8], rw_base, rw_offset, rw_vaddr, rw_vtype;
static unsigned rw_depth, rw_runs, rw_full;
static u32 retro_count, retro_logged, retro_walked, retro_at[8], retro_kind[8], retro_now[8], retro_parse[8];
static const u32 *retro_parse_at; /* (where the parse is: groupJump's j) */
/* the walk's last transfers (from, to, the word), and where it got into the
 * chunk the parse is about to begin (grp_next): logged once a period */
static u32 esc_from[16], esc_to[16], esc_word[16], esc_n, esc_logged, esc_count;
static const u32 *grp_next;
static void retroEscape(void);
static void retroRun(u32 start, u32 end, u32 kind) {
  (void)start; (void)end; (void)kind;
  ++rw_runs;
}
/* the map: the words the parse has read (8-byte granules) */
static void parsedMark(const u32 *p) {
  const u32 a = (u32)p & 0x0fffffffu;
  if (a >= RETRO_LO && a < RETRO_HI) { const u32 b = (a - RETRO_LO) >> 3; RETRO_BITS[b >> 3] |= (u8)(1u << (b & 7u)); }
}
static int parsedSeen(const u32 *p) {
  const u32 a = (u32)p & 0x0fffffffu, b = (a - RETRO_LO) >> 3;
  if (a < RETRO_LO || a >= RETRO_HI || (a >= 0x0A700000u && a < 0x0A740000u))
    return 1; /* (our list; outside the map: not checked) */
  return (RETRO_BITS[b >> 3] >> (b & 7u)) & 1u;
}
static void retroStart(void) {
  memset(RETRO_BITS, 0, (RETRO_HI - RETRO_LO) >> 6);
  esc_n = 0;
  rw_pc = rw_run_start = (const u32 *)GUARD_VERTICES;
  rw_base = rw_offset = rw_vaddr = rw_vtype = 0;
  rw_depth = rw_runs = rw_full = 0;
  retro_on = 1;
}
static void retroJumped(const u32 *to) { /* the run so far ends, another begins */
  esc_from[esc_n & 15u] = (u32)rw_pc; esc_to[esc_n & 15u] = (u32)to; esc_word[esc_n & 15u] = *rw_pc; ++esc_n;
  if (!parsedSeen(to) && !(to[0] >> 24 == GE_CMD_BONEMATRIXDATA && to[11] >> 24 == GE_CMD_BONEMATRIXDATA &&
                            to[12] >> 24 == GE_CMD_RET))
    retroEscape(); /* (the GE there before the parse; bone matrices the parse never reads) */
  retroRun((u32)rw_run_start, (u32)(rw_pc + 1), 1u);
  rw_pc = rw_run_start = to;
}
static void retroWalk(const u32 *stall) {
  unsigned steps = 0;
  while (rw_pc != stall && steps++ < 400000u) {
    const u32 w = *rw_pc, cmd = w >> 24, data = w & 0xffffffu;
    if (!retro_on) return;
    const u32 *to = (const u32 *)(((rw_base | data) + rw_offset) & 0x0ffffffcu);
    ++retro_walked;
    switch (cmd) {
      case GE_CMD_BASE: rw_base = (data << 8) & 0x0f000000u; ++rw_pc; break;
      case GE_CMD_OFFSETADDR: rw_offset = data << 8; ++rw_pc; break;
      case GE_CMD_VADDR: rw_vaddr = ((rw_base | data) + rw_offset) & 0x0fffffffu; ++rw_pc; break;
      case GE_CMD_VERTEXTYPE: rw_vtype = data; ++rw_pc; break;
      case GE_CMD_PRIM:
        if (!(rw_vtype & GE_VTYPE_IDX_MASK) && rw_vaddr) {
          u32 size, pos_off, visit_off;
          vertexInfoCached(rw_vtype, &size, &pos_off, &visit_off);
          retroRun(rw_vaddr, rw_vaddr + (data & 0xffffu) * size, 2u);
          rw_vaddr += (data & 0xffffu) * size;
        }
        ++rw_pc;
        break;
      case GE_CMD_JUMP: retroJumped(to); break;
      case GE_CMD_CALL:
        if (rw_depth < 8u) { rw_offset_stack[rw_depth] = rw_offset; rw_stack[rw_depth++] = rw_pc + 1; }
        retroJumped(to);
        break;
      case GE_CMD_RET:
        if (rw_depth) { --rw_depth; rw_offset = rw_offset_stack[rw_depth]; retroJumped(rw_stack[rw_depth]); }
        else ++rw_pc;
        break;
      case GE_CMD_END: retro_on = 0; return; /* (lost: not followed) */
      default: ++rw_pc; break;
    }
  }
  retroRun((u32)rw_run_start, (u32)rw_pc, 1u); /* (up to the stall; the run goes on from it) */
  rw_run_start = rw_pc;
}
static void retroEscape(void) {
  ++esc_count;
  if (esc_logged < 2u) {
    unsigned k;
    ++esc_logged;
    log("[gp-escape] the walk got to a word the parse has not read (next chunk %08x), stall %08x, %u transfers; last:\n",
        (unsigned)grp_next, (unsigned)grp_stall, (unsigned)esc_n);
    for (k = esc_n > 16u ? esc_n - 16u : 0u; k < esc_n; ++k)
      log("[gp-escape]   %08x (%08x) -> %08x\n", (unsigned)esc_from[k & 15u], (unsigned)esc_word[k & 15u],
          (unsigned)esc_to[k & 15u]);
  }
}
static void retroCheck(const void *p, unsigned size) {
  if (1) return; /* (the map is the parse's now) */
  const u32 a = (u32)p & 0x0fffffffu, e = a + (size ? size : 1u);
  u32 b;
  unsigned k;
  if (a < RETRO_LO || e > RETRO_HI) return;
  for (b = (a - RETRO_LO) >> 3; b <= (e - 1u - RETRO_LO) >> 3; ++b)
    if (RETRO_BITS[b >> 3] & (1u << (b & 7u))) {
      ++retro_count;
      for (k = 0; k < retro_logged; ++k) if (retro_at[k] == a) return;
      if (retro_logged < 8u) {
        retro_at[retro_logged] = a; retro_kind[retro_logged] = size;
        retro_now[retro_logged] = *(const u32 *)((u32)p & ~3u);
        retro_parse[retro_logged] = (u32)retro_parse_at;
        ++retro_logged;
      }
      return;
    }
}
#endif
static void groupRelease(unsigned commands) {
  if (!group_release) return;
  if (commands - release_count_at < release_every) return;
  if (release_blocked || state.pending || inset.used_vertices != release_pool_start || parsePending()) return;
  noteWrite(grp_flushed, (u32)(grp_top - grp_flushed) * 4u);
  grp_flushed = grp_top;
  if (legacy_cache) sceKernelDcacheWritebackAll();
  else flushWrites();
#if GP_LOG
  if (retro_on) retroWalk(grp_stall);
#endif
  _sceGeListUpdateStallAddr(release_qid, (void *)grp_stall);
  release_count_at = commands;
  ++perf_releases;
  ++grp_releases;
}
#if GP_LOG
/* (GP_LOG) Where the GE is while the parse goes on (uOFW hardware.h: LISTADDR
 * 0xBD400108, STALLADDR 0xBD40010C, EXEC 0xBD400100 bit 0 running): in our
 * list up to the stall, in chunks released, elsewhere (sublists) -- or where
 * it may not be: chunks not released yet, our list past the stall. */
static u32 gpr_top, gpr_done, gpr_other, gpr_idle, gpr_bad_cur, gpr_bad_piece, gpr_bad_past;
static u32 gpr_at, gpr_stall_hw, gpr_stall, gpr_piece, gpr_j, gpr_why;
static void groupProbe(const u32 *j) {
  const u32 exec = *(volatile u32 *)0xBD400100u, pc = *(volatile u32 *)0xBD400108u & 0x0fffffffu;
  const u32 *const at = (const u32 *)pc;
  unsigned why = 0;
  if (!(exec & 1u)) { ++gpr_idle; return; }
  if (at >= (const u32 *)GUARD_VERTICES && at < grp_top_end) {
    if (at <= grp_stall) { ++gpr_top; return; }
    ++gpr_bad_past; why = 1;
  } else if (groupIn(grp_done_lo, grp_done_hi, grp_done, at)) {
    ++gpr_done; return;
  } else if (groupIn(grp_cur_lo, grp_cur_hi, grp_cur, at)) {
    ++gpr_bad_cur; why = 2;
  } else if (at >= grp_piece && at <= j) {
    ++gpr_bad_piece; why = 3;
  } else {
    ++gpr_other; return;
  }
  if (!gpr_at) {
    gpr_at = pc; gpr_stall_hw = *(volatile u32 *)0xBD40010Cu; gpr_stall = (u32)grp_stall;
    gpr_piece = (u32)grp_piece; gpr_j = (u32)j; gpr_why = why;
  }
}
#endif
/* A JUMP of the main list (at `j`, to `target`, with this BASE and offset):
 * its group closed there, unless a BJUMP's landing is still ahead. */
static void groupJump(const u32 *j, u32 target, u32 base, u32 offset, unsigned commands) {
  unsigned k;
  if (!group_live || group_aborted) return;
#if GP_LOG
  groupProbe(j);
  retro_parse_at = j;
#endif
  for (k = 0; k < grp_opens;) /* the landings in the chunk ending here */
    if (grp_open[k] >= grp_piece && grp_open[k] <= j) grp_open[k] = grp_open[--grp_opens];
    else ++k;
  if (grp_opens) {
    ++grp_merged;
    if (grp_cur == 32u) groupAbortAt(5u, j, base, offset, target);
    else { grp_cur_lo[grp_cur] = grp_piece; grp_cur_hi[grp_cur++] = j; }
  } else {
    /* where the GE is to land, less the offset it adds: its BASE and data */
    const u32 to = ((u32)grp_top - offset) & 0x0ffffffcu; /* (the NOP before the entry) */
    const int base_too = (to & 0x0f000000u) != base;
    if ((base_too && (j - 1 < grp_piece || j[-1] >> 24 != GE_CMD_BASE)) ||
        grp_words + 2u > GRP_WORDS || grp_top + 3 > grp_top_end || grp_done + grp_cur + 1u > GRP_DONE) {
      groupAbortAt(base_too && (j - 1 < grp_piece || j[-1] >> 24 != GE_CMD_BASE) ? 2u : 3u, j, base, offset, target);
    } else {
      for (k = 0; k < grp_cur; ++k) { grp_done_lo[grp_done] = grp_cur_lo[k]; grp_done_hi[grp_done++] = grp_cur_hi[k]; }
      grp_done_lo[grp_done] = grp_piece;
      grp_done_hi[grp_done++] = j;
      grp_cur = 0;
      *grp_top++ = 0u; /* a NOP the chunk lands on: the GE comes to the stall after it in order */
      grp_stall = grp_top;
      if (base_too) {
        *grp_top++ = j[-1]; /* (the game's BASE again) */
        groupSet((u32 *)j - 1, ((u32)GE_CMD_BASE << 24) | ((to >> 8) & 0x0f0000u));
      }
      *grp_top++ = *j; /* (the game's JUMP: its BASE and offset as at it) */
      groupSet((u32 *)j, ((u32)GE_CMD_JUMP << 24) | (to & 0xffffffu));
      ++grp_groups;
#if GP_LOG
      grp_next = (const u32 *)target;
#endif
      groupRelease(commands);
    }
  }
  grp_piece = (const u32 *)target;
}
static void groupBjump(const u32 *list, u32 target) {
  const u32 *const to = (const u32 *)target;
  unsigned k;
  if (!group_live || group_aborted) return;
  if ((to >= grp_piece && to <= list) || groupIn(grp_cur_lo, grp_cur_hi, grp_cur, to))
    return; /* back within the open group: the GE loops there, in it */
  if (GP_LOG) ++grp_bjumps;
  for (k = 0; k < grp_opens;) /* the landings already passed in this chunk (Tekken: 9 short ones in one) */
    if (grp_open[k] >= grp_piece && grp_open[k] <= list) grp_open[k] = grp_open[--grp_opens];
    else ++k;
  if (groupIn(grp_done_lo, grp_done_hi, grp_done, to)) groupAbortAt(0u, list, (u32)grp_piece, grp_done, target);
  else if (grp_opens == 8u) groupAbortAt(1u, list, 0u, 0u, target);
  else grp_open[grp_opens++] = to;
}
/* The parse done: our list written back (the last chunk ends with the END) */
static void groupFinish(void) {
  noteWrite(grp_flushed, (u32)(grp_top - grp_flushed) * 4u);
  grp_flushed = grp_top;
}
/* At the GE's completion: the game's words back where ours still stand */
static unsigned groupRestore(void) {
  unsigned k, n = grp_words;
  for (k = grp_words; k-- > 0;) /* (last first: a word set twice back to the game's) */
    if (*grp_word_at[k] == grp_word_ours[k]) {
      *grp_word_at[k] = grp_word_orig[k];
      if (!legacy_cache) noteWrite(grp_word_at[k], 4u);
    }
  grp_words = 0;
  return n;
}

/* The 3D scene's loop: what patchGeList's short path does (the CMD_HOT
 * cases, the plain registers, the matrix runs), the parser's state in
 * registers -- read here, written back when it stops. Tekken's fight is
 * 70-80k words a frame; through patchGeList's loop each cost 100-200 cycles,
 * most of them loads and stores of that state. It stops at the first word it
 * does not take (anything patched, a 2D or dropped draw, a word of ours or
 * saved for putting back) and gives it back for patchGeList to parse. */
#if GP_LOG
/* (GP_LOG) the 3D draws' textures: swizzled or not, where, which format --
 * the GE reads a texture that is not swizzled line by line through its 8 KiB
 * cache, from RAM at 2x: DW's movie picture so took 26-47 ms */
static u32 tex_draws[2][2][16], tex_vertices[2][2];
static void noteTexture(u32 count) {
  const u32 addr = currentTexture(), swizzled = state.ge_cmd[GE_CMD_TEXMODE] & 1u;
  const u32 in_vram = addr >= FAKE_VRAM && addr < FAKE_VRAM + 0x200000u;
  ++tex_draws[swizzled][in_vram][state.ge_cmd[GE_CMD_TEXFORMAT] & 15u];
  tex_vertices[swizzled][in_vram] += count;
}
#endif
static u32 *parseHot(u32 *list, u32 *stall, unsigned *commands_io) {
  u32 base = state.base, offset = state.offset, vtype = state.vertex_type, vaddr = state.vertex_addr;
  u32 iaddr = state.index_addr, since = state.since_draw, users = state.base_users, seen = state.pending_seen;
  u32 *base_word = state.base_word, *vaddr_word = state.vaddr_word, *vtype_word = state.vtype_word;
  u32 vaddr_offset = state.vaddr_offset, depth = state.curr_stack, draws = 0;
  /* The vertices drawn since vaddr, not added to it yet (of type vadv_type):
   * Tekken sets VADDR again before nearly every draw (8.4k a frame, 8.1k
   * draws), so the vertex size is mostly never needed. */
  u32 vadv = 0, vadv_type = 0;
  const u32 *const inset_lo = inset.word_count ? inset.lo : (const u32 *)0xffffffffu;
  const u32 *const inset_hi = inset.word_count ? inset.hi : NULL;
  /* (texture and buffer state only change on words it gives back) */
  const u32 ignore_fb = state.ignore_framebuf;
  u32 ignore_tex = state.ignore_texture;
  unsigned commands = *commands_io;
  for (; list != stall; ++list) {
    const u32 op = *list, cmd = op >> 24, cls = cmd_class[cmd], data = op & 0xffffffu;
#if GP_LOG
    if (retro_on) parsedMark(list);
#endif
    if (GP_PROFILE) ++prof_hot_cmd[cmd];
    if (!(cls & (CMD_HANDLED | CMD_MATRIX))) {
      ++commands;
      if (cls & CMD_KEEP) state.ge_cmd[cmd] = data;
      continue;
    }
    if (cls & CMD_MATRIX) {
      if (GP_GUARD && cmd >= GE_CMD_WORLDMATRIXNUMBER && cmd <= GE_CMD_PROJMATRIXDATA) break; /* (followed there) */
      ++commands;
      while (list + 1 != stall && (list[1] >> 24) == cmd) { ++list; ++commands; }
      continue;
    }
    if (!(cls & CMD_HOT)) break;
    switch (cmd) {
      case GE_CMD_BASE:
        if ((const u32 *)list >= inset_lo && (const u32 *)list <= inset_hi) goto stop;
        base = (data << 8) & 0x0f000000;
        base_word = list;
        since |= 1u;
        users = 0;
        seen |= 1u;
        break;
      case GE_CMD_VADDR:
        if ((const u32 *)list >= inset_lo && (const u32 *)list <= inset_hi) goto stop;
        ++users;
        seen |= 2u;
        vaddr = ((base | data) + offset) & 0x0fffffff;
        vadv = 0;
        vaddr_word = list;
        vaddr_offset = offset;
        if (since & 1u) since |= 2u;
        break;
      case GE_CMD_IADDR:
        ++users;
        iaddr = ((base | data) + offset) & 0x0fffffff;
        break;
      case GE_CMD_VERTEXTYPE:
        if ((const u32 *)list >= inset_lo && (const u32 *)list <= inset_hi) goto stop;
        vtype = data;
        vtype_word = list;
        since |= 4u;
        seen |= 4u;
        break;
      case GE_CMD_OFFSETADDR:
        offset = data << 8;
        break;
      case GE_CMD_ORIGIN:
        offset = (u32)list;
        break;
      case GE_CMD_BJUMP:
        ++users;
        if (release_qid >= 0 && !depth) {
          releaseBjump(list, ((base | data) + offset) & 0x0ffffffc);
          groupBjump(list, ((base | data) + offset) & 0x0ffffffc);
        }
        break;
      case GE_CMD_BOUNDINGBOX:
        if ((vtype & GE_VTYPE_THROUGH_MASK) == GE_VTYPE_THROUGH) goto stop;
        since = 0;
        break;
      case GE_CMD_PRIM:
        if ((vtype & GE_VTYPE_THROUGH_MASK) == GE_VTYPE_THROUGH || GP_GUARD ||
            ignore_fb || (ignore_tex && (state.ge_cmd[GE_CMD_TEXTUREMAPENABLE] & 1u) &&
                          !(state.ge_cmd[GE_CMD_CLEARMODE] & 1u)))
          goto stop;
        since = 0;
        ++draws;
#if GP_LOG
        if (state.ge_cmd[GE_CMD_TEXTUREMAPENABLE] & 1u) noteTexture(data & 0xffffu);
#endif
        if (vtype & GE_VTYPE_IDX_MASK) {
          iaddr += (data & 0xffffu) << (((vtype & GE_VTYPE_IDX_MASK) >> GE_VTYPE_IDX_SHIFT) - 1u);
        } else {
          if (vadv && vadv_type != vtype) { /* (another type: the last ones' size first) */
            u32 vertex_size, pos_off, visit_off;
            vertexInfoCached(vadv_type, &vertex_size, &pos_off, &visit_off);
            vaddr += vadv * vertex_size;
            vadv = 0;
          }
          vadv += data & 0xffffu;
          vadv_type = vtype;
        }
        break;
      case GE_CMD_CALL:
      {
        const u32 address = ((base | data) + offset) & 0x0ffffffc, *to = (const u32 *)address;
        if (!address) goto stop; /* (the general path ends the list there) */
        if (GP_GUARD && saved_used) goto stop; /* (maybe the guard band's, in a PRIM's place) */
        ++users;
        state.address = address;
        if (to[0] >> 24 == GE_CMD_BONEMATRIXDATA && to[11] >> 24 == GE_CMD_BONEMATRIXDATA &&
            to[12] >> 24 == GE_CMD_RET) {
          if (GP_PROFILE) ++prof_bone_calls;
          break;
        }
        if (depth < sizeof(state.stack) / sizeof(StackEntry)) {
          StackEntry *e = &state.stack[depth++];
          e->list = list;
          e->offset = offset;
#if GP_LOG
          e->commands = commands + 1u;
          e->writes = perf_writes;
          e->draws_2d = perf_2d_prims;
#endif
          list = (u32 *)(address - 4);
        }
        break;
      }
      case GE_CMD_RET:
        if (depth) {
          const StackEntry *e = &state.stack[--depth];
          list = e->list;
          offset = e->offset;
          /* (releaseParsed's own first test here: ~3.7k RETs a frame, most not due) */
          if (release_qid >= 0 && !depth && !group_live &&
              (release_bjump_until || commands + 1u - release_count_at >= release_every)) {
            state.curr_stack = depth; /* (what releaseParsed reads besides its own) */
            state.since_draw = since;
            state.base_users = users;
            releaseParsed(list + 1, commands + 1u);
          }
        }
        break;
      case GE_CMD_JUMP:
      {
        const u32 address = ((base | data) + offset) & 0x0ffffffc;
        if (!address) goto stop;
        ++users;
        if (!depth) {
          releaseJump(list);
          state.since_draw = since; /* (what groupRelease reads) */
          state.base_users = users;
          groupJump(list, address, base, offset, commands);
        }
        state.address = address;
        list = (u32 *)(address - 4);
        break;
      }
      case GE_CMD_NOP:
        if (saved_used) goto stop; /* maybe a draw of the game's we dropped */
        break;
      /* Tekken's fight left this loop ~1340 times a frame for these (out to
       * patchGeList's whole way and back in): what its cases do, here. */
      case GE_CMD_STENCILTESTENABLE:
        state.ge_cmd[cmd] = data;
        if (data & 1u) stencil_frames_left = STENCIL_HOLD_FRAMES;
        break;
      case GE_CMD_TEXLEVEL:
      {
        const u32 game = savedOriginal(list, op); /* (ours on a parse again) */
        if (game >> 24 != cmd) goto stop;
        state.ge_cmd[cmd] = game & 0xffffffu;
        patchTexLevel(list, game);
        if (*list != op) noteWrite(list, 4u);
        break;
      }
      case GE_CMD_TEXADDR0:
      case GE_CMD_TEXBUFWIDTH0:
      {
        const u32 game = savedOriginal(list, op);
        u32 ptr = state.texbufptr[0], width = state.texbufwidth[0];
        if (game >> 24 != cmd) goto stop;
        if (cmd == GE_CMD_TEXADDR0) ptr = game; else width = game;
        if (ptr && width) {
          const u32 texaddr = ((width & 0x0f0000) << 8) | (ptr & 0xffffff);
          if (texaddr != state.framebuf && findFramebuf(texaddr) >= 0) goto stop; /* (a frame buffer: noted there) */
          ptr = width = 0;
          ignore_tex = 0;
        }
        if (cmd == GE_CMD_TEXADDR0) tex0_word = list; else tbw0_word = list;
        state.texbufptr[0] = ptr;
        state.texbufwidth[0] = width;
        state.ge_cmd[cmd] = game & 0xffffffu;
        break;
      }
      default:
        goto stop;
    }
    ++commands;
  }
stop:
  if (GP_PROFILE && list != stall) ++prof_hot_stop[*list >> 24];
  if (vadv) {
    u32 vertex_size, pos_off, visit_off;
    vertexInfoCached(vadv_type, &vertex_size, &pos_off, &visit_off);
    vaddr += vadv * vertex_size;
  }
  state.base = base;
  state.offset = offset;
  state.vertex_type = vtype;
  state.vertex_addr = vaddr;
  state.index_addr = iaddr;
  state.since_draw = since;
  state.base_users = users;
  state.pending_seen = seen;
  state.base_word = base_word;
  state.vaddr_word = vaddr_word;
  state.vtype_word = vtype_word;
  state.vaddr_offset = vaddr_offset;
  state.curr_stack = depth;
  state.ignore_texture = ignore_tex;
  if (draws) {
    state.has_draws = 1;
    frame_draws += draws;
    perf_3d_prims += draws;
    frame_3d_draw = 1;
  }
  *commands_io = commands;
  return list;
}

static int linear_live; /* (the copy made as the parse goes: below) */
static int linearMore(u32 **stall);
void patchGeList(u32 *list, u32 *stall) {
  unsigned commands = 0;
  u32 cmd_started, prim_started = 0, inset_started;
  const unsigned pool_start = inset.used_vertices;
  const u32 parse_started = cpuCycles();
  /* the short path below; off for the tests' full parse and a frame dump */
  const int hot_parse = !force_full_parse && !(GP_LOG && dump_frame);
  if (!cmd_class[GE_CMD_PRIM]) buildCommandClasses();
  if (GP_PROFILE) { sec_mark = parse_started; sec_last = 2; }

  StackEntry *stack_entry;
  StackEntry stack_entry_buf;

  for (;;) { /* (again at a copy's frontier, made as the parse goes: linearMore) */
  for (; list && list != stall; list++) { /* (stall NULL: to the END) */

    if (hot_parse && !state.pending) { /* the 3D scene's own loop, as far as it goes */
      const u32 hot_started = cpuCycles(), hot_from = commands, rel_before = prof_rel_cycles;
      list = parseHot(list, stall, &commands);
      if (GP_PROFILE) {
        prof_hot_cycles += cpuCycles() - hot_started - (prof_rel_cycles - rel_before);
        prof_hot_words += commands - hot_from;
        ++prof_hot_calls;
      }
      if (list == stall) break;
    }
    u32 op = *list, cmd = op >> 24, data, cls = cmd_class[cmd];
#if GP_LOG
    if (retro_on) parsedMark(list);
#endif
    SECTION(cls & CMD_MATRIX ? 0u : (cls & CMD_HANDLED) ? 2u : 1u, cmd);
    if (GP_LOG && dump_frame) dumpCommand(list, op);
    const u32 raw = op;
    ++commands;
    /* Matrix data (bones above all) is most of a skinned 3D list and is never
     * patched or needed here: skip the whole run before any other work. */
    if (cls & CMD_MATRIX) {
      /* world, view and projection are followed (guard band); the rest skipped */
      const unsigned *const before = (const unsigned *)list;
      if (GP_GUARD && cmd >= GE_CMD_WORLDMATRIXNUMBER && cmd <= GE_CMD_PROJMATRIXDATA) {
        gp_matrix_command(&matrices, cmd, op & 0xffffffu);
        while (list + 1 != stall && (list[1] >> 24) == cmd) {
          ++list; ++commands;
          gp_matrix_command(&matrices, cmd, *list & 0xffffffu);
        }
      } else {
        while (list + 1 != stall && (list[1] >> 24) == cmd) { ++list; ++commands; }
      }
      if (GP_LOG) perf_matrix_words += (u32)((const unsigned *)list - before) + 1u;
      continue;
    }
#if GP_LOG && GP_SUBMEASURE
    if (state.curr_stack) {
      sub_hash = sub_hash * 33u + raw;
      sub_uncacheable |= sub_uncache[cmd];
    }
#endif
    if (!(cls & CMD_HANDLED) && !force_full_parse) {
      if (GP_LOG) ++perf_fast_words; /* (only logged) */
      if (cls & CMD_KEEP) state.ge_cmd[cmd] = op & 0xffffff;
      continue;
    }
    /* The commands of a 3D scene, with no inset waiting: what their cases
     * below do, without the rest of the way there and back (the common
     * checks, the word read again, the inset's flags) -- 50-odd instructions
     * a command, most of the parse's time in Tekken's fight. Anything else
     * (a 2D or dropped draw, a word of ours, a word saved for putting back)
     * goes on below. No register these set is read from state.ge_cmd. */
    if ((cls & CMD_HOT) && hot_parse && !state.pending) {
      data = op & 0xffffff;
      switch (cmd) {
        case GE_CMD_BASE:
          if (inset.word_count && (const uint32_t *)list >= inset.lo && (const uint32_t *)list <= inset.hi) break;
          state.base = (data << 8) & 0x0f000000;
          state.base_word = list;
          state.since_draw |= 1u;
          state.base_users = 0;
          state.pending_seen |= 1u;
          continue;
        case GE_CMD_VADDR:
          if (inset.word_count && (const uint32_t *)list >= inset.lo && (const uint32_t *)list <= inset.hi) break;
          ++state.base_users;
          state.pending_seen |= 2u;
          state.vertex_addr = ((state.base | data) + state.offset) & 0x0fffffff;
          state.vaddr_word = list;
          state.vaddr_offset = state.offset;
          if (state.since_draw & 1u) state.since_draw |= 2u;
          continue;
        case GE_CMD_IADDR:
          ++state.base_users;
          state.index_addr = ((state.base | data) + state.offset) & 0x0fffffff;
          continue;
        case GE_CMD_VERTEXTYPE:
          if (inset.word_count && (const uint32_t *)list >= inset.lo && (const uint32_t *)list <= inset.hi) break;
          state.vertex_type = data;
          state.vtype_word = list;
          state.since_draw |= 4u;
          state.pending_seen |= 4u;
          continue;
        case GE_CMD_OFFSETADDR:
          state.offset = data << 8;
          continue;
        case GE_CMD_ORIGIN:
          state.offset = (u32)list;
          continue;
        case GE_CMD_BJUMP:
          ++state.base_users;
          if (release_qid >= 0 && !state.curr_stack) {
            releaseBjump(list, ((state.base | data) + state.offset) & 0x0ffffffc);
            groupBjump(list, ((state.base | data) + state.offset) & 0x0ffffffc);
          }
          continue;
        case GE_CMD_BOUNDINGBOX:
          if ((state.vertex_type & GE_VTYPE_THROUGH_MASK) == GE_VTYPE_THROUGH) break;
          state.since_draw = 0;
          continue;
        case GE_CMD_PRIM:
        {
          u32 vertex_size, pos_off, visit_off;
          if ((state.vertex_type & GE_VTYPE_THROUGH_MASK) == GE_VTYPE_THROUGH || GP_GUARD || dropsDraw()) break;
          state.since_draw = 0;
          state.has_draws = 1;
          ++frame_draws;
          ++perf_3d_prims;
          frame_3d_draw = 1;
          vertexInfoCached(state.vertex_type, &vertex_size, &pos_off, &visit_off);
          AdvanceVerts(data & 0xffffu, vertex_size);
          continue;
        }
        case GE_CMD_CALL:
        {
          const u32 *to;
          ++state.base_users;
          state.address = ((state.base | data) + state.offset) & 0x0ffffffc;
          to = (const u32 *)state.address;
          if (to[0] >> 24 == GE_CMD_BONEMATRIXDATA && to[11] >> 24 == GE_CMD_BONEMATRIXDATA &&
              to[12] >> 24 == GE_CMD_RET)
            continue;
          if (state.curr_stack < sizeof(state.stack) / sizeof(StackEntry)) {
            stack_entry = &state.stack[state.curr_stack++];
            stack_entry->list = list;
            stack_entry->offset = state.offset;
#if GP_LOG
            stack_entry->commands = commands;
            stack_entry->writes = perf_writes;
            stack_entry->draws_2d = perf_2d_prims;
#endif
            list = (u32 *)(state.address - 4);
          }
          continue;
        }
        case GE_CMD_RET:
          if (state.curr_stack) {
            stack_entry = &state.stack[--state.curr_stack];
            list = stack_entry->list;
            state.offset = stack_entry->offset;
            if (release_qid >= 0 && !state.curr_stack) releaseParsed(list + 1, commands);
          }
          continue;
        case GE_CMD_JUMP:
          ++state.base_users;
          state.address = ((state.base | data) + state.offset) & 0x0ffffffc;
          if (!state.curr_stack) {
            releaseJump(list);
            groupJump(list, state.address, state.base, state.offset, commands);
          }
          list = (u32 *)(state.address - 4);
          continue;
        case GE_CMD_NOP:
          if (saved_used) break; /* maybe a draw of the game's we dropped */
          continue;
        default:
          break;
      }
    }
    if (cls & CMD_SAVED) {
      const u32 t = cpuCycles();
      op = savedOriginal(list, op);
      cmd = op >> 24;
      if (GP_PROFILE) saved_cycles += cpuCycles() - t;
    }
    /* Only BASE, VADDR and VTYPE words can be ours (UV inset). */
    if (cls & CMD_INSET) {
      const u32 t = cpuCycles();
      op = gp_inset_original(&inset, (uint32_t *)list, op);
      cmd = op >> 24;
      if (GP_PROFILE) inset_cycles += cpuCycles() - t;
    }
    data = op & 0xffffff;

    state.ge_cmd[cmd] = data;
    cmd_started = cpuCycles();
    if (GP_PROFILE) pre_cycles += cmd_started - sec_mark;
    switch (cmd) {
      // Skip matrix data

      // case GE_CMD_BONEMATRIXNUMBER:
        // if (*(list+12) >> 24 == GE_CMD_BONEMATRIXDATA)
          // list += 12;
        // break;

      // case GE_CMD_WORLDMATRIXNUMBER:
        // if (*(list+12) >> 24 == GE_CMD_WORLDMATRIXDATA)
          // list += 12;
        // break;

      // case GE_CMD_VIEWMATRIXNUMBER:
        // if (*(list+12) >> 24 == GE_CMD_VIEWMATRIXDATA)
          // list += 12;
        // break;
      // case GE_CMD_TGENMATRIXNUMBER:
        // if (*(list+12) >> 24 == GE_CMD_TGENMATRIXDATA)
          // list += 12;
        // break;

      // case GE_CMD_PROJMATRIXNUMBER:
        // if (*(list+16) >> 24 == GE_CMD_PROJMATRIXDATA)
          // list += 16;
        // break;

      // Handle control flow commands

      case GE_CMD_BASE:
        state.base = (data << 8) & 0x0f000000;
        state.base_word = list;
        state.since_draw |= 1u;
        state.base_users = 0;
        state.pending_seen |= 1u;
        break;

      case GE_CMD_OFFSETADDR:
        state.offset = data << 8;
        break;
      case GE_CMD_ORIGIN:
        state.offset = (u32)list;
        break;
      // TODO: need to save other states, too?
      case GE_CMD_CALL:
      {
        const u32 *to;
        insetBaseUser();
        state.address = ((state.base | data) + state.offset) & 0x0ffffffc;
        to = (const u32 *)state.address;
        if (to[0] >> 24 == GE_CMD_BONEMATRIXDATA && to[11] >> 24 == GE_CMD_BONEMATRIXDATA &&
            to[12] >> 24 == GE_CMD_RET) {
          break;
        }
        /* (push() without the copy: a CALL is one command in eight in 3D) */
        if (state.curr_stack < sizeof(state.stack) / sizeof(StackEntry)) {
          stack_entry = &state.stack[state.curr_stack++];
          stack_entry->list = list;
          stack_entry->offset = state.offset;
#if GP_LOG
#if GP_SUBMEASURE
          if (state.curr_stack == 1u) { sub_hash = 0; sub_uncacheable = 0; }
          stack_entry->target = state.address;
          stack_entry->drops = perf_dropped_fb + perf_dropped_tex;
#endif
          stack_entry->commands = commands;
          stack_entry->writes = perf_writes;
          stack_entry->draws_2d = perf_2d_prims;
#endif
          list = (u32 *)(state.address - 4);
        }
        break;
      }
      // TODO: is it okay to always take the branch?
      case GE_CMD_BJUMP:
        /* Taken only when the BOUNDINGBOX before it found nothing on screen:
         * what it skips is what the GE draws when something is. Parsed on
         * as if not taken -- the draw is patched for when it runs, and is
         * harmless when the GE skips it. (Always taking it left Tekken's
         * arena backgrounds, behind such tests, unpatched: black.) */
        insetBaseUser();
        if (release_qid >= 0 && !state.curr_stack) {
          releaseBjump(list, ((state.base | data) + state.offset) & 0x0ffffffc);
          groupBjump(list, ((state.base | data) + state.offset) & 0x0ffffffc);
        }
        break;

      case GE_CMD_JUMP:
        insetBaseUser();
        state.address = ((state.base | data) + state.offset) & 0x0ffffffc;
        if (!state.curr_stack) {
          releaseJump(list);
          groupJump(list, state.address, state.base, state.offset, commands);
        }
        list = (u32 *)(state.address - 4);
        break;
      case GE_CMD_RET:
        // Ignore returns when the stack is empty
        stack_entry = pop();
        if (stack_entry) {
          list = stack_entry->list;
          state.offset = stack_entry->offset;
          if (release_qid >= 0 && !state.curr_stack) releaseParsed(list + 1, commands);
          if (GP_LOG && !state.curr_stack) {
            const u32 words = commands - stack_entry->commands;
            if (perf_writes == stack_entry->writes && perf_2d_prims == stack_entry->draws_2d) {
              perf_clean_words += words; ++perf_clean_calls;
            } else {
              perf_dirty_words += words; ++perf_dirty_calls;
            }
#if GP_LOG && GP_SUBMEASURE
            if (perf_2d_prims != stack_entry->draws_2d || perf_dropped_fb + perf_dropped_tex != stack_entry->drops)
              sub_uncacheable = 1;
            subNote(stack_entry->target, words);
#endif
          }
        }
        break;
      case GE_CMD_END:
      {
        u32 prev = *(list-1);
        insetAbort(); /* GE state may carry into what runs next */
        switch (prev >> 24) {
          case GE_CMD_SIGNAL:
          {
            u8 behaviour = (prev >> 16) & 0xff;
            release_blocked = 3; /* where the GE goes next is not followed for releasing */
            groupAbort();
            u16 signal = prev & 0xffff;
            u16 enddata = data & 0xffff;
            u32 target;

            switch (behaviour) {
              case PSP_GE_SIGNAL_SYNC:
                list += 2;
                break;
              case PSP_GE_SIGNAL_JUMP:
                target = (((signal << 16) | enddata) & 0x0ffffffc);
                list = (u32 *)(target - 4);
                break;
              case PSP_GE_SIGNAL_CALL:
                target = (((signal << 16) | enddata) & 0x0ffffffc);
                stack_entry = &stack_entry_buf;
                stack_entry->list = list;
                stack_entry->base = state.base;
                stack_entry->offset = state.offset;
                if (push(stack_entry) == 0)
                  list = (u32 *)(target - 4);
                break;
              case PSP_GE_SIGNAL_RET:
                // Ignore returns when the stack is empty
                stack_entry = pop();
                if (stack_entry) {
                  list = stack_entry->list;
                  state.base = stack_entry->base;
                  state.offset = stack_entry->offset;
                }
                break;

              default:
                break;
            }
            break;
          }

          case GE_CMD_FINISH:
            goto finish;
          default:
            break;
        }
        break;
      }

      // Patch vertices

      case GE_CMD_IADDR:
        insetBaseUser();
        state.index_addr = ((state.base | data) + state.offset) & 0x0fffffff;
        break;

      case GE_CMD_VADDR:
        insetBaseUser();
        state.pending_seen |= 2u;
        state.vertex_addr = ((state.base | data) + state.offset) & 0x0fffffff;
        state.vaddr_word = list;
        state.vaddr_offset = state.offset;
        if (state.since_draw & 1u) state.since_draw |= 2u;
        break;

      case GE_CMD_VERTEXTYPE:
        state.vertex_type = data;
        state.vtype_word = list;
        state.since_draw |= 4u;
        state.pending_seen |= 4u;
        break;

      case GE_CMD_BEZIER:
      case GE_CMD_SPLINE:
      {
        insetNextDraw();
        state.since_draw = 0;
        state.has_draws = 1;
        if (dropsDraw()) {
          changeWord(list, op, 0u); /* NOP until the GE is done */
          break;
        }

        if ((state.vertex_type & GE_VTYPE_THROUGH_MASK) == GE_VTYPE_THROUGH) {
          u8 num_points_u = data & 0xff;
          u8 num_points_v = (data >> 8) & 0xff;

          u32 count = num_points_u * num_points_v;
          u32 vertex_size = 0, pos_off = 0, visit_off = 0;
          getVertexInfo(state.vertex_type, &vertex_size, &pos_off, &visit_off);

          AdvanceVerts(count, vertex_size);
        }

        break;
      }

      case GE_CMD_BOUNDINGBOX:
      {
        insetNextDraw();
        state.since_draw = 0;
        if (dropsDraw())
          break;

        if ((state.vertex_type & GE_VTYPE_THROUGH_MASK) == GE_VTYPE_THROUGH) {
          u32 count = data;
          u32 vertex_size = 0, pos_off = 0, visit_off = 0;
          getVertexInfo(state.vertex_type, &vertex_size, &pos_off, &visit_off);

          AdvanceVerts(count, vertex_size);
        }

        break;
      }

      case GE_CMD_PRIM:
      {
        /* A 3D draw with no inset waiting (nearly all of a 3D scene): counted,
         * dropped if it must be, and the vertex pointer moved on -- the same
         * as below without the 2D work (it was most of the parse's time). */
        if (!state.pending && (state.vertex_type & GE_VTYPE_THROUGH_MASK) != GE_VTYPE_THROUGH && !GP_GUARD) {
          u32 vertex_size, pos_off, visit_off;
          state.extend_open = state.run_open = 0;
          state.since_draw = 0;
          state.has_draws = 1;
          ++frame_draws;
          ++perf_3d_prims;
          frame_3d_draw = 1;
          if (GP_LOG) tracePassState(data);
          if (dropsDraw()) {
            if (state.ignore_framebuf) ++perf_dropped_fb;
            else ++perf_dropped_tex;
            changeWord(list, op, 0u); /* NOP until the GE is done */
            break;
          }
          vertexInfoCached(state.vertex_type, &vertex_size, &pos_off, &visit_off);
          AdvanceVerts(data & 0xffffu, vertex_size);
          break;
        }
        /* This draw's BASE, VADDR and VTYPE words are its own: all set since
         * the previous draw, BASE first, and BASE read by nothing but VADDR. */
        u32 own_words = state.since_draw == 7u && state.base_users == 1u;
        state.extend_open = state.pending && !(state.pending_seen & 6u);
        /* Own BASE+VADDR, no VTYPE since the inset draw: joins the run. */
        state.run_open = state.pending && state.pending_seen == 3u && state.since_draw == 3u &&
                         state.base_users == 1u;
        if (!state.extend_open && !state.run_open) insetNextDraw();
        state.since_draw = 0;
        ++frame_draws;
        if ((state.vertex_type & GE_VTYPE_THROUGH_MASK) != GE_VTYPE_THROUGH) { ++perf_3d_prims; frame_3d_draw = 1; }
        else if (isMoviePicture(data)) ++frame_movie_draw;
        if (state.ignore_framebuf) ++perf_dropped_fb;
        else if (dropsDraw()) ++perf_dropped_tex;
        tracePassState(data);
        state.has_draws = 1;
        // Dragon Ball Z Tenkaichi Tag Team uses the same GE list again,
        // therefore NOPing it makes character invisible.
        if (dropsDraw()) {
          changeWord(list, op, 0u); /* NOP until the GE is done */
          break;
        }
        if ((state.vertex_type & GE_VTYPE_THROUGH_MASK) && isMoviePicture(data)) useRealVramTexture();
        if ((state.vertex_type & GE_VTYPE_THROUGH_MASK) != GE_VTYPE_THROUGH) {
          /* the vertex size cached by VTYPE: worked out again on each 3D
           * draw it took a third of the parse's PRIM time */
          u32 vertex_size, pos_off, visit_off;
          if (GP_GUARD) guardBand(list, data);
          vertexInfoCached(state.vertex_type, &vertex_size, &pos_off, &visit_off);
          /* the next 3D draw goes on from here unless it sets VADDR/IADDR */
          AdvanceVerts(data & 0xffffu, vertex_size);
        }

        prim_started = cpuCycles();
        if ((state.vertex_type & GE_VTYPE_THROUGH_MASK) == GE_VTYPE_THROUGH) {
          u16 count = data & 0xffff;
          u32 vertex_size = 0, pos_off = 0, visit_off = 0;
          vertexInfoCached(state.vertex_type, &vertex_size, &pos_off, &visit_off);

          u32 lower = 0, upper = count;
          u32 morphs = ((state.vertex_type >> GE_VTYPE_MORPHCOUNT_SHIFT) & 7u) + 1u;
          u32 one_size = vertex_size / morphs;
          int pos = (state.vertex_type & GE_VTYPE_POS_MASK) >> GE_VTYPE_POS_SHIFT;
          unsigned pos_size = possize[pos] / 3;
          u32 index_size = ((state.vertex_type & GE_VTYPE_IDX_MASK) >> GE_VTYPE_IDX_SHIFT);
          u32 vertex_addr, i, morph;
          int scaled_ok = 1;
          if (!count) break;
          if (index_size) {
            index_size = 1u << (index_size - 1u);
            if (state.index_addr < 0x08400000u ||
                (uint64_t)state.index_addr + (uint64_t)count * index_size > 0x0A400000u) {
              ++vertex_bounds_errors;
              break;
            }
            GetIndexBounds((void *)state.index_addr, count, state.vertex_type, &lower, &upper);
            if (upper == 0xFFFFFFFFu) { ++vertex_bounds_errors; break; }
            ++upper;
          }
          if (!vertex_size || upper < lower || upper - lower > 65536u ||
              state.vertex_addr < 0x08400000u ||
              (uint64_t)state.vertex_addr + (uint64_t)upper * vertex_size > 0x0A400000u) {
            ++vertex_bounds_errors;
            break;
          }
          if (state.ge_cmd[GE_CMD_TEXTUREMAPENABLE] & 1u) dumpSpriteBatch(data, vertex_size, pos_off);
          vertex_addr = state.vertex_addr + lower * vertex_size;
          {
            u32 scale_started = cpuCycles();
            if (morphs == 1u) /* one run of positions: one range */
              scaled_ok = gp_vertex_scale_run(&vertex_restore, (void *)(vertex_addr + pos_off), upper - lower, vertex_size, pos_size);
            else
              for (i = lower; i < upper; ++i, vertex_addr += vertex_size)
                for (morph = 0; morph < morphs; ++morph)
                  scaled_ok &= gp_vertex_scale(&vertex_restore, (void *)(vertex_addr + morph * one_size + pos_off), pos_size);
            noteWrite((const void *)(state.vertex_addr + lower * vertex_size), (upper - lower) * vertex_size);
            prim_scale_cycles += cpuCycles() - scale_started;
            perf_scaled_vertices += (upper - lower) * morphs;
            ++perf_2d_prims;
          }
          inset_started = cpuCycles();
          if (state.extend_open || state.run_open) {
            const int clamp = (state.ge_cmd[GE_CMD_TEXTUREMAPENABLE] & 1u) &&
                              !(state.ge_cmd[GE_CMD_CLEARMODE] & 1u) &&
                              !(state.ge_cmd[GE_CMD_TEXMODE] & 0x70000u) &&
                              state.ge_cmd[GE_CMD_TEXFILTER] == 0x101u;
            if (state.extend_open) {
              if (!index_size && scaled_ok && insetExtend(data, vertex_size, pos_off, clamp))
                state.extend_open = 0;
            } else if (!index_size && scaled_ok && insetJoinRun(data, vertex_size, pos_off, clamp)) {
              state.run_open = 0;
            }
          } else if ((state.ge_cmd[GE_CMD_TEXTUREMAPENABLE] & 1u) &&
              !(state.ge_cmd[GE_CMD_CLEARMODE] & 1u) &&
              state.ge_cmd[GE_CMD_TEXFILTER] == 0x101u) {
            unsigned reason = !own_words ? 1u : index_size ? 2u : !scaled_ok ? 3u :
                              (state.ge_cmd[GE_CMD_TEXMODE] & 0x70000u) ? 4u :
                              insetCandidate(data, vertex_size, pos_off);
            if (reason) noteInsetSkip(reason, data);
          }
          prim_inset_cycles += cpuCycles() - inset_started;
          AdvanceVerts(count, vertex_size);
        }

        break;
      }

      // Patch GE commands

      // case GE_CMD_DITHERENABLE:
        // *list = (cmd << 24) | 1;
        // break;

      case GE_CMD_STENCILTESTENABLE:
        if (data & 1u) stencil_frames_left = STENCIL_HOLD_FRAMES;
        break;

      case GE_CMD_FRAMEBUFPIXFORMAT:
        /* The stencil lives in the framebuffer's alpha bits. SC marks its
         * fighters ALWAYS/REPLACE and draws shadows GREATER, so on 565 the
         * shadows fell over everything. See chooseDrawFormat; 8888 at 960x544
         * would not fit before the depth buffer. */
        if ((data & 3u) == GE_FORMAT_8888) truecolor_frames_left = STENCIL_HOLD_FRAMES;
        draw_format = chooseDrawFormat(data & 3u);
        changeWord(list, op, (cmd << 24) | draw_format);
        break;

      case GE_CMD_DITHERENABLE:
        if (truecolor_frames_left && draw_format != GE_FORMAT_8888) *list = (cmd << 24) | 1u;
        break;
      case GE_CMD_FRAMEBUFPTR:
      case GE_CMD_FRAMEBUFWIDTH:
      {
        if (cmd == GE_CMD_FRAMEBUFPTR) {
          *list = (cmd << 24) | (VRAM_DRAW_BUFFER_OFFSET & 0xffffff);
          state.framebufptr = op;
        } else {
          /* a stride not 512 or 960: a target not ours, whatever its
           * FRAMEBUFPTR (below) -- the width word the 1 KB nowhere at once,
           * not once that comes (the GE may have run it then: released
           * between them, Tekken's shadow target of stride 128 drew into our
           * draw buffer, stripes) */
          const u32 game_pitch = op & 0xffffu;
          if (game_pitch != 512u && game_pitch != 960u)
            changeWord(list, op, (cmd << 24) | ((VRAM_1KB >> 24) << 16) | 0);
          else
            changeWord(list, op, (cmd << 24) | ((VRAM_DRAW_BUFFER_OFFSET >> 24) << 16) | (game_pitch == 512u ? PITCH : game_pitch));
          state.framebufwidth = op;
        }
        if (state.framebufptr && state.framebufwidth) {
          state.framebuf = FAKE_VRAM | (state.framebufptr & 0xffffff);
          // This allows more games to work, but causes weird triangles in Sonic.
          u32 pitch = state.framebufwidth & 0xffff;
          if (pitch == 512 || pitch == 960) {
            state.ignore_framebuf = 0;
          } else {
            state.ignore_framebuf = 1;
            noteIgnore(0u, state.framebuf, pitch);
          }

          insertFramebuf(state.framebuf);
          state.framebufptr = 0;
          state.framebufwidth = 0;
        }

        break;
      }

      case GE_CMD_ZBUFPTR:
        *list = (cmd << 24) | (VRAM_DEPTH_BUFFER_OFFSET & 0xffffff);
        break;

      case GE_CMD_ZBUFWIDTH:
        *list = (cmd << 24) | ((VRAM_DEPTH_BUFFER_OFFSET >> 24) << 16) | DEPTH_PITCH;
        break;
      case GE_CMD_TEXADDR0:
      case GE_CMD_TEXADDR1:
      case GE_CMD_TEXADDR2:
      case GE_CMD_TEXADDR3:
      case GE_CMD_TEXADDR4:
      case GE_CMD_TEXADDR5:
      case GE_CMD_TEXADDR6:
      case GE_CMD_TEXADDR7:
      case GE_CMD_TEXBUFWIDTH0:
      case GE_CMD_TEXBUFWIDTH1:
      case GE_CMD_TEXBUFWIDTH2:
      case GE_CMD_TEXBUFWIDTH3:
      case GE_CMD_TEXBUFWIDTH4:
      case GE_CMD_TEXBUFWIDTH5:
      case GE_CMD_TEXBUFWIDTH6:
      case GE_CMD_TEXBUFWIDTH7:
      {
        int index;
        if (cmd >= GE_CMD_TEXADDR0 && cmd <= GE_CMD_TEXADDR7) {
          index = cmd - GE_CMD_TEXADDR0;
          state.texbufptr[index] = op;
          if (!index) tex0_word = list;
        } else {
          index = cmd - GE_CMD_TEXBUFWIDTH0;
          if (!index) tbw0_word = list;
          state.texbufwidth[index] = op;
        }
        if (state.texbufptr[index] && state.texbufwidth[index]) {
          u32 texaddr = ((state.texbufwidth[index] & 0x0f0000) << 8) | (state.texbufptr[index] & 0xffffff);
          if (texaddr != state.framebuf && findFramebuf(texaddr) >= 0) {
            state.ignore_texture = 1;
            noteIgnore(1u, texaddr, state.ge_cmd[GE_CMD_TEXFORMAT] & 15u);
          } else {
            state.ignore_texture = 0;
          }

          state.texbufptr[index] = 0;
          state.texbufwidth[index] = 0;
        }

        break;
      }
      case GE_CMD_TEXLEVEL:
        patchTexLevel(list, op);
        break;

      case GE_CMD_DITH0:
        game_dither_set = 1;
        break;

      /* The game's viewport, offset, scissor and region, scaled: a 3D scene
       * in a window (a character preview) stays in its window. Until 1.24
       * they were set to the whole screen whatever the game asked. */
      case GE_CMD_VIEWPORTXSCALE: case GE_CMD_VIEWPORTYSCALE:
      case GE_CMD_VIEWPORTXCENTER: case GE_CMD_VIEWPORTYCENTER:
      case GE_CMD_OFFSETX: case GE_CMD_OFFSETY:
      case GE_CMD_SCISSOR1: case GE_CMD_SCISSOR2: case GE_CMD_REGION1: case GE_CMD_REGION2:
        noteGameScreen(cmd, data);
        if (cmd == GE_CMD_OFFSETX || cmd == GE_CMD_OFFSETY) { /* the center from the new offset; ours stays */
          const u32 center = cmd == GE_CMD_OFFSETX ? GE_CMD_VIEWPORTXCENTER : GE_CMD_VIEWPORTYCENTER;
          changeWord(list, op, (center << 24) | screenValue(center));
        } else {
          changeWord(list, op, (cmd << 24) | screenValue(cmd));
        }
        break;

      default:
        break;
    }
    if (GP_PROFILE) {
      const u32 now = cpuCycles();
      cmd_cycles[cmd] += now - cmd_started;
      if (cmd == GE_CMD_PRIM && prim_started && (state.vertex_type & GE_VTYPE_THROUGH_MASK)) prim_2d_cycles += now - prim_started;
      prim_started = 0;
    }
    if (GP_PROFILE) ++cmd_count[cmd];
    if (*list != raw) noteWrite(list, 4u);
    if (state.extend_open || state.run_open) { /* NOP'd, non-through, indexed or out of bounds */
      state.extend_open = state.run_open = 0;
      insetAbort();
    }
  }
  if (!(linear_live && list && list == stall && linearMore(&stall))) break;
  }
finish:
  parse_end = list; /* the END (or the stall) the parse stopped at */
  SECTION(2, sec_last_cmd);
  if (GP_PROFILE) parse_cycles += cpuCycles() - parse_started;
  perf_commands += commands;
  insetAbort(); /* never leave a draw redirected past the parsed range */
  gp_inset_join_tiles(&inset); /* before the GE reads the copies */
  if (legacy_cache) {
    sceKernelDcacheWritebackInvalidateAll();
  } else {
    u32 flush_started = sceKernelGetSystemTimeLow();
    if (inset.used_vertices > pool_start)
      noteWrite(inset.pool + pool_start, (inset.used_vertices - pool_start) * sizeof(GpInsetVertex));
    flushWrites();
    perf_flush_us += sceKernelGetSystemTimeLow() - flush_started;
  }
}

void *(* _sceGeEdramGetAddr)(void);
unsigned int (* _sceGeEdramGetSize)(void);
int (* _sceGeGetList)(int qid, void *list, int *flag);
int (* _sceGeListUpdateStallAddr)(int qid, void *stall);
int (* _sceGeListEnQueue)(const void *list, void *stall, int cbid, PspGeListArgs *arg);
int (* _sceGeListEnQueueHead)(const void *list, void *stall, int cbid, PspGeListArgs *arg);
int (* _sceDisplaySetFrameBuf)(void *topaddr, int bufferwidth, int pixelformat, int sync);
int (* _sceGeDrawSync)(int syncType);
int (* _sceGeListSync)(int qid, int syncType);

void *sceGeEdramGetAddrPatched(void) {
  return (void *)FAKE_VRAM;
}

unsigned int sceGeEdramGetSizePatched(void) {
  return 4 * 1024 * 1024;
}

static u32 *emitCommand(u32 *p, unsigned cmd, u32 data) {
  *p = (cmd << 24) | (data & 0xffffff);
  return p + 1;
}

/* For games drawing 8888: dither on for the frames that follow, with a 4x4
 * Bayer matrix unless the game set its own (sceGuSetDither's layout). */
static u32 *emitDither(u32 *p) {
  static const signed char bayer[16] = {-4, 0, -3, 1, 2, -2, 3, -1, -3, 1, -4, 0, 3, -1, 2, -2};
  unsigned row;
  if (!truecolor_frames_left || draw_format == GE_FORMAT_8888) return p;
  if (!game_dither_set) {
    for (row = 0; row < 4u; ++row) {
      const signed char *m = bayer + row * 4u;
      p = emitCommand(p, GE_CMD_DITH0 + row, (u32)(m[0] & 15) | ((u32)(m[1] & 15) << 4) |
                                            ((u32)(m[2] & 15) << 8) | ((u32)(m[3] & 15) << 12));
    }
  }
  return emitCommand(p, GE_CMD_DITHERENABLE, 1);
}


/* Draws the 5551/4444 draw buffer as a texture, as 565, then transfers that
 * to the display buffer (Adrenaline reads it as 565). The GE renders only into
 * VRAM (its frame address is a VRAM offset), and the only VRAM left is the
 * depth buffer: the frame is finished when it is shown, and the game clears
 * depth at the start of the next one. Textures are at most 512 texels per
 * side, so the frame goes in 64-pixel columns, two rows (512 + 32 lines). */

/* What the parser rewrites, for the current size: games set some of it once
 * (depth buffer, strides, viewport, scissor), so a mode change sends it. */
static u32 *emitModeRegisters(u32 *p) {
  union { float f; u32 i; } t;
  if (!mode_changed)
    return p;
  mode_changed = 0;
  p = emitCommand(p, GE_CMD_FRAMEBUFPTR, VRAM_DRAW_BUFFER_OFFSET & 0xffffff);
  p = emitCommand(p, GE_CMD_FRAMEBUFWIDTH, ((VRAM_DRAW_BUFFER_OFFSET >> 24) << 16) | PITCH);
  p = emitCommand(p, GE_CMD_ZBUFPTR, VRAM_DEPTH_BUFFER_OFFSET & 0xffffff);
  p = emitCommand(p, GE_CMD_ZBUFWIDTH, ((VRAM_DEPTH_BUFFER_OFFSET >> 24) << 16) | DEPTH_PITCH);
  {
    static const u8 screen[] = {GE_CMD_VIEWPORTXSCALE, GE_CMD_VIEWPORTYSCALE, GE_CMD_VIEWPORTXCENTER,
                                GE_CMD_VIEWPORTYCENTER, GE_CMD_OFFSETX, GE_CMD_OFFSETY, GE_CMD_SCISSOR1,
                                GE_CMD_SCISSOR2, GE_CMD_REGION1, GE_CMD_REGION2};
    unsigned k;
    (void)t;
    for (k = 0; k < sizeof(screen); ++k) p = emitCommand(p, screen[k], screenValue(screen[k]));
  }
  return p;
}

/* Right when the mode changes, ahead of the game's next lists. */
static void sendModeRegisters(void) {
  u32 *p = (u32 *)((RENDER_LIST + 0x2000) | 0xA0000000);
  p = emitModeRegisters(p);
  p = emitCommand(p, GE_CMD_FINISH, 0);
  emitCommand(p, GE_CMD_END, 0);
  _sceGeListEnQueue((void *)(RENDER_LIST + 0x2000), NULL, -1, NULL);
}

/* A frame left in VRAM for the Vita to copy (it does, with its own DMA, ~1
 * MiB in well under a millisecond): the GE's copy of 960x544 to the display
 * buffer each frame was GE time Tekken's arenas, bound by the GE at 2x, do
 * not have. The GE draws into that buffer again only once the Vita has taken
 * the frame (waitFrameTaken, before any list of ours or the game's). */
static u32 frames_left_in_vram, take_timeouts;
static void waitFrameTaken(void) {
  u32 started;
  if (*(volatile u32 *)GP_FRAME_TAKEN_ADDR == frames_left_in_vram) return;
  started = sceKernelGetSystemTimeLow();
  while (*(volatile u32 *)GP_FRAME_TAKEN_ADDR != frames_left_in_vram) {
    if (sceKernelGetSystemTimeLow() - started > 8000u) { /* a Vita side gone: drawn over, as before */
      ++take_timeouts;
      frames_left_in_vram = *(volatile u32 *)GP_FRAME_TAKEN_ADDR;
      return;
    }
    sceKernelDelayThread(100);
  }
}
static int presentDirect(void) {
  u32 *start = (u32 *)(RENDER_LIST | 0xA0000000), *p = start;
  waitFrameTaken(); /* (the last one first) */
  p = emitDither(p); /* what the copy's list also carried */
  p = emitModeRegisters(p);
  if (p != start) {
    p = emitCommand(p, GE_CMD_FINISH, 0);
    emitCommand(p, GE_CMD_END, 0);
    _sceGeListEnQueue((void *)RENDER_LIST, NULL, -1, NULL);
  }
  *(volatile u32 *)GP_FRAME_SIZE_ADDR = WIDTH | (HEIGHT << 16);
  *(volatile u32 *)GP_FRAME_FORMAT_ADDR = draw_format;
  *(volatile u32 *)DRAW_NATIVE = 1;
  *(volatile u32 *)GP_FRAME_READY_ADDR = ++frames_left_in_vram;
  ++copies_queued;
  return 0;
}
static int vitaTakesFrames(void) {
  return (*(volatile u32 *)GP_VIDEO_ADDR & GP_VIDEO_TAKES_FRAMES) && PITCH == WIDTH && draw_format != GE_FORMAT_8888;
}

static u32 display_back = 1; /* the buffer the next frame goes to */
int copyFrameBuffer(void) {
  int result;
  const int two = !gp_stock && (*(volatile u32 *)GP_VIDEO_ADDR & GP_VIDEO_TWO_BUFFERS);
  const u32 dst = two && display_back ? DISPLAY_BUFFER2 : DISPLAY_BUFFER;
  waitFrameTaken(); /* the Vita still copying the display buffer's last frame */
  /* sceGuCopyImage's commands in the frame's own format (Adrenaline shows
   * 565, 5551, 4444 and 8888 as they are: no conversion pass on the GE), then
   * the dither state for 8888 games drawn in 16 bits. The display buffer is
   * WIDTH pixels wide, the size Adrenaline is told. */
  u32 *p = (u32 *)(RENDER_LIST | 0xA0000000);
  p = emitCommand(p, GE_CMD_TRANSFERSRC, VRAM_DRAW_BUFFER_OFFSET & 0xffffff);
  p = emitCommand(p, GE_CMD_TRANSFERSRCW, ((VRAM_DRAW_BUFFER_OFFSET & 0xff000000) >> 8) | PITCH);
  p = emitCommand(p, GE_CMD_TRANSFERSRCPOS, 0);
  p = emitCommand(p, GE_CMD_TRANSFERDST, dst & 0xffffff);
  p = emitCommand(p, GE_CMD_TRANSFERDSTW, ((dst & 0xff000000) >> 8) | WIDTH);
  p = emitCommand(p, GE_CMD_TRANSFERDSTPOS, 0);
  p = emitCommand(p, GE_CMD_TRANSFERSIZE, ((HEIGHT - 1) << 10) | (WIDTH - 1));
  p = emitCommand(p, GE_CMD_TRANSFERSTART, draw_format == GE_FORMAT_8888 ? 1 : 0);
  if (two) { /* the frame done: its number to DISPLAY_FRONT (one 32-bit pixel) */
    const u32 from = DISPLAY_FRONT_VALUES + display_back * 64u;
    volatile u32 *const values = (volatile u32 *)(DISPLAY_FRONT_VALUES | 0xA0000000u);
    values[0] = 0u; values[16] = 1u; /* (uncached: the GE reads them) */
    p = emitCommand(p, GE_CMD_TRANSFERSRC, from & 0xffffff);
    p = emitCommand(p, GE_CMD_TRANSFERSRCW, ((from & 0xff000000) >> 8) | 8u);
    p = emitCommand(p, GE_CMD_TRANSFERSRCPOS, 0);
    p = emitCommand(p, GE_CMD_TRANSFERDST, DISPLAY_FRONT & 0xffffff);
    p = emitCommand(p, GE_CMD_TRANSFERDSTW, ((DISPLAY_FRONT & 0xff000000) >> 8) | 8u);
    p = emitCommand(p, GE_CMD_TRANSFERDSTPOS, 0);
    p = emitCommand(p, GE_CMD_TRANSFERSIZE, 0);
    p = emitCommand(p, GE_CMD_TRANSFERSTART, 1);
    display_back ^= 1u;
  }
  p = emitDither(p);
  p = emitModeRegisters(p);
  p = emitCommand(p, GE_CMD_FINISH, 0);
  emitCommand(p, GE_CMD_END, 0);
  result = _sceGeListEnQueue((void *)RENDER_LIST, NULL, -1, NULL);
  *(volatile u32 *)GP_FRAME_SIZE_ADDR = WIDTH | (HEIGHT << 16);
  *(volatile u32 *)GP_FRAME_FORMAT_ADDR = draw_format;
  *(volatile u32 *)DRAW_NATIVE = 1;
  if (result >= 0) ++copies_queued;
  else ++copy_errors;
  return result;
}
int sceGeListUpdateStallAddrPatched(int qid, void *stall) {
  HOOK_ENTERED(1);
  int k1 = pspSdkSetK1(0);
  char info[64];
  if (_sceGeGetList(qid, info, NULL) == 0) {
    u16 state = *(u16 *)(info + 0x08);
    if (state != 3) { // completed
      void *list = *(void **)(info + 0x18); // previous stall
      if (!list)
        list = *(void **)(info + 0x14); // list
      if (((u32)list & 0x0fffffff) < ((u32)stall & 0x0fffffff)) {
        u32 started = sceKernelGetSystemTimeLow();
        patchGeList((u32 *)((u32)list & 0x0fffffff), (u32 *)((u32)stall & 0x0fffffff));
        perf_parse_us += sceKernelGetSystemTimeLow() - started;
        ++perf_lists;
      }
    }
  }
  pspSdkSetK1(k1);
  return _sceGeListUpdateStallAddr(qid, stall);
}
/* The GE has finished everything queued: put our changes back now. Games
 * that never wait for the GE (the princess game: no sync in 5 s) rewrite
 * their vertices each frame where ours still stood; seen as scaled already,
 * the new ones were drawn 1:1, and the old values went back over them. */
static void restoreCompletedVertices(void);
static void restoreIfIdle(void) {
  if ((vertex_restore.used || vertex_restore.log_used || saved_used || inset.word_count) && _sceGeDrawSync(1) == 0)
    restoreCompletedVertices();
}

static void noteEvent(char kind, u32 info); /* (GP_LOG, below) */
static u32 dialog_open; /* (below: a system dialog is up) */
/* (GP_LOG) the gaps between frames shown, in VBlanks (1..6+), and how long
 * the frames took (ms: <10 <15 <20 <25 <30 <40 40+) */
static u32 shown_gaps[7], shown_last, drawn_ms[7], last_flip;
static void noteFrameTime(u32 *h, u32 us) {
  const u32 ms = us / 1000u;
  h[ms < 10u ? 0 : ms < 15u ? 1 : ms < 20u ? 2 : ms < 25u ? 3 : ms < 30u ? 4 : ms < 40u ? 5 : 6]++;
}
/* (GP_LOG) at the game's SetFrameBuf */
static void noteFrameShown(void) {
  const u32 now = sceKernelGetSystemTimeLow(), took = now - last_flip;
  last_flip = now;
  if (took < 500000u) {
    const u32 gap = (now - shown_last + 8341u) / 16683u;
    noteFrameTime(drawn_ms, took);
    if (now - shown_last < 500000u) shown_gaps[gap < 1u ? 1 : gap > 6u ? 6 : gap]++;
  }
  shown_last = now;
}

/* The game's VBlank wait. Tekken waits for one (WaitVblankStartCB) after
 * each frame: one 30 ms past it waited on to the next, 7-12 ms of nothing,
 * no frame the better for it. A game behind the time its
 * waits would have come at (a VBlank each, from the last real one) is not
 * held: the next step at once. Ahead, it waits as ever -- 60 fps scenes
 * unchanged. More than 100 ms behind (loading, or long enough without a
 * wait), the time starts again from now, not held either: a wait there to
 * start it again came every ~8 frames in the fight, 7-12 ms each -- a frame
 * shown later, as if two were skipped (the user). */
#define PACE_FRAME_US 16683u
static int (*_sceDisplayWaitVblankStartCB)(void), (*_sceDisplayWaitVblankStart)(void);
static u32 pace_start, pace_frames, perf_pace_free;
static int pacedWait(int (*wait)(void), u32 which) {
  const u32 now = sceKernelGetSystemTimeLow();
  const int behind = (int)(now - (pace_start + (pace_frames + 1u) * PACE_FRAME_US)); /* (its VBlank) */
  int result;
  ++pace_frames;
#if GP_LOG
  noteEvent('V', which);
#endif
  /* (no frame dropped by this, the next one only begun at once -- 30 ms
   * frames, not 33) */
  if (!dialog_open && !movie_playing && behind >= 0) {
    if (behind >= 100000) pace_start = now - pace_frames * PACE_FRAME_US; /* (the next VBlank's from now) */
    ++perf_pace_free;
#if GP_LOG
    noteEvent('v', 0xffffffffu); /* (not waited) */
#endif
    return 0;
  }
  result = wait();
  pace_start = sceKernelGetSystemTimeLow() - pace_frames * PACE_FRAME_US; /* from the real one */
#if GP_LOG
  noteEvent('v', (u32)which);
#endif
  return result;
}
static int sceDisplayWaitVblankStartCBPatched(void) { return pacedWait(_sceDisplayWaitVblankStartCB, 1u); }
static int sceDisplayWaitVblankStartPatched(void) { return pacedWait(_sceDisplayWaitVblankStart, 0u); }

/* The main list straightened. Tekken's runs through chunks JUMPing forward
 * and back; released while parsed (releaseParsed), the GE ran from one of
 * them into another not parsed yet -- its stall stops it at or past the stall
 * address, it seems, not only at it (camera and HUD went wrong). Copied in
 * the order the GE runs it into a buffer above all of the game's memory, the
 * GE meets only what was parsed before the stall, whichever the rule. The
 * JUMPs are followed and left out; a BJUMP forward lands in the copy (BASE set
 * for it, the game's put back after); the sublists stay where they are. A
 * list this cannot copy (ORIGIN, SIGNAL, a BJUMP back, a BASE or offset in what
 * a BJUMP skips, too long) is run and parsed where it is, unreleased. */
#define LINEAR_LIST 0x0A700000u /* (the guard band's cut vertices after its 256 KB) */
#define LINEAR_WORDS (0x40000u / 4u - 16u)
static u32 linear_lists, linear_fallbacks, linear_us;
/* (GP_LOG) the main list's chunks: JUMPs, the longest run between two, and
 * JUMPs taken with a BJUMP's landing still ahead (one chunk into another) */
static u32 linear_jumps, linear_chunk_max, linear_cross, linear_chunk_at;
static int linear_qid = -1; /* the copy the GE was last given */
/* (GP_LOG) why lists were not copied: 1 too long, 2 BASE in a skip, 3 offset
 * in a skip, 4 ORIGIN, 5 SIGNAL, 6 JUMP/CALL/BJUMP before a BASE, 7 BJUMP back,
 * 8 BJUMPs > 32 open, 9 END after SIGNAL, 10 landing never reached; the last one's
 * word and where it was */
static u32 linear_why[11], linear_why_word, linear_why_at;
#define LINEAR_REFUSE(code) do { if (GP_LOG) { ++linear_why[code]; linear_why_word = *p; linear_why_at = (u32)p; } return 0; } while (0)
/* Whether the list at `p` sets its BASE before anything uses one: a BJUMP's
 * landing then does not care which BASE it came with. (Tekken's: a BASE and a
 * CALL in what a BJUMP skips, a BASE again where it lands.) */
static int linearBaseSetFirst(const u32 *p) {
  unsigned k;
  for (k = 0; k < 64u; ++k) {
    const u32 cmd = p[k] >> 24;
    if (cmd == GE_CMD_BASE) return 1;
    if (cmd == GE_CMD_VADDR || cmd == GE_CMD_IADDR || cmd == GE_CMD_JUMP || cmd == GE_CMD_BJUMP ||
        cmd == GE_CMD_CALL || cmd == GE_CMD_RET || cmd == GE_CMD_END || cmd == GE_CMD_ORIGIN ||
        cmd == GE_CMD_OFFSETADDR || cmd == GE_CMD_SIGNAL)
      return 0;
  }
  return 0;
}
#define fix_open(k) (lin.fix[k].open)
/* the commands linearizeList looks at; every other word is copied as it is */
static const unsigned char linear_special[256] = {
  [GE_CMD_BASE] = 1, [GE_CMD_OFFSETADDR] = 1, [GE_CMD_ORIGIN] = 1, [GE_CMD_SIGNAL] = 1,
  [GE_CMD_JUMP] = 1, [GE_CMD_CALL] = 1, [GE_CMD_BJUMP] = 1, [GE_CMD_END] = 1,
};
/* The copy is made a chunk at a time, as the parse goes (linearMore): the
 * parse reads each chunk while it is still in the CPU's cache -- whole, the
 * copy (~300 KiB in Tekken's fight) was out of it again by then and read from
 * RAM twice (a plain word cost the parse ~60 cycles, most of it that read).
 * The parse is held (its stall) before words not final yet: a BJUMP's BASE
 * and BJUMP, set where it lands. A list found not to copy after its first
 * chunk goes on where it is: the copy JUMPs there, no releasing after. */
static struct {
  const u32 *p, *land; /* where the copy is at in the list; the nearest landing ahead */
  u32 n, base, offset, base_set, steps, fixes;
  struct { u32 base_at, jump_at, open, base, offset; const u32 *target; } fix[32];
  u32 frontier, flushed, done, refused; /* words final; written back; END copied; why not */
} lin;
static u32 linear_chunk = 1024u; /* words: 4 KiB, a quarter of the data cache */
static void linearBegin(const u32 *list) {
  memset(&lin, 0, sizeof(lin));
  lin.p = list;
  if (GP_LOG) linear_chunk_at = 0;
}
/* Copies on to at least `until` words (or the END); 0 when the list cannot be
 * copied (lin.refused, the word at lin.p not copied: the copy is whole up to it). */
static int linearRun(u32 until) {
  u32 *const out = (u32 *)LINEAR_LIST;
  unsigned k;
  u32 n = lin.n, base = lin.base, offset = lin.offset, base_set = lin.base_set, fixes = lin.fixes;
  unsigned steps = lin.steps;
  const u32 *p = lin.p, *land = lin.land;
  int result = 1;
#undef LINEAR_REFUSE
#define LINEAR_REFUSE(code) do { if (GP_LOG) { ++linear_why[code]; linear_why_word = *p; linear_why_at = (u32)p; } \
  lin.refused = (code); result = 0; goto leave; } while (0)
  if (until > LINEAR_WORDS - 8u) until = LINEAR_WORDS - 8u;
  while (n < until) {
    u32 w, cmd, data;
    if (!p) { lin.refused = 1; result = 0; goto leave; }
    { /* the plain words up to the next that matters or a landing, a few
       * instructions each (Tekken: ~60k a frame, this copy took 5.7 ms) */
      u32 *o = out + n;
      u32 *const o_end = out + until;
      while (o < o_end && p != land) {
        const u32 v = *p;
        if (linear_special[v >> 24]) break;
        *o++ = v;
        ++p;
      }
      steps += (unsigned)(o - (out + n));
      n = (u32)(o - out);
      if (n >= until) break;
    }
    if (++steps > LINEAR_WORDS || n + 8u > LINEAR_WORDS) LINEAR_REFUSE(1);
    for (k = 0; k < fixes; ++k) /* a BJUMP lands here: BASE as it was there, then on */
      if (fix_open(k) && lin.fix[k].target == p) {
        const u32 landing = ((u32)&out[n] & 0x0fffffffu) - lin.fix[k].offset;
        if (lin.fix[k].base != base && !linearBaseSetFirst(p)) LINEAR_REFUSE(2); /* a BASE in what it skips */
        out[lin.fix[k].base_at] = (GE_CMD_BASE << 24) | ((landing >> 8) & 0x0f0000u);
        out[lin.fix[k].jump_at] = (GE_CMD_BJUMP << 24) | (landing & 0xffffffu);
        out[n++] = (GE_CMD_BASE << 24) | ((base >> 8) & 0x0f0000u);
        lin.fix[k].open = 0;
      }
    if (p == land) { /* landed: the next one */
      land = NULL;
      for (k = 0; k < fixes; ++k)
        if (fix_open(k) && (!land || lin.fix[k].target < land)) land = lin.fix[k].target;
    }
    w = *p;
    cmd = w >> 24;
    data = w & 0xffffffu;
    switch (cmd) {
      case GE_CMD_BASE:
        base = (data << 8) & 0x0f000000u;
        base_set = 1;
        break;
      case GE_CMD_OFFSETADDR:
        for (k = 0; k < fixes; ++k) if (fix_open(k)) LINEAR_REFUSE(3); /* (in what a BJUMP skips) */
        offset = data << 8;
        break;
      case GE_CMD_ORIGIN:
        LINEAR_REFUSE(4);
      case GE_CMD_SIGNAL:
        LINEAR_REFUSE(5);
      case GE_CMD_JUMP:
        if (!base_set) LINEAR_REFUSE(6); /* (the GE's BASE from before the list) */
        if (GP_LOG) {
          ++linear_jumps;
          if (n - linear_chunk_at > linear_chunk_max) linear_chunk_max = n - linear_chunk_at;
          linear_chunk_at = n;
          if (land) ++linear_cross;
        }
        p = (const u32 *)(((base | data) + offset) & 0x0ffffffcu);
        continue;
      case GE_CMD_CALL:
        if (!base_set) LINEAR_REFUSE(6);
        break;
      case GE_CMD_BJUMP:
      {
        const u32 *const target = (const u32 *)(((base | data) + offset) & 0x0ffffffcu);
        if (!base_set) LINEAR_REFUSE(6);
        if (target <= p) LINEAR_REFUSE(7);
        /* a slot of one landed (Tekken: a BJUMP over a few words at each of
         * ~100 bounding boxes a frame), else a new one */
        for (k = 0; k < fixes && fix_open(k); ++k) {}
        if (k == 32u) LINEAR_REFUSE(8);
        if (k == fixes) ++fixes;
        lin.fix[k].open = 1;
        lin.fix[k].base_at = n;
        out[n++] = 0; /* (BASE for the copy's landing: at the landing) */
        lin.fix[k].jump_at = n;
        out[n++] = 0;
        lin.fix[k].target = target;
        lin.fix[k].base = base;
        lin.fix[k].offset = offset;
        if (!land || target < land) land = target;
        out[n++] = (GE_CMD_BASE << 24) | ((base >> 8) & 0x0f0000u); /* the game's, not taken */
        ++p;
        continue;
      }
      case GE_CMD_END:
      {
        const u32 prev = n ? out[n - 1u] >> 24 : 0u;
        if (prev == GE_CMD_SIGNAL) LINEAR_REFUSE(9);
        for (k = 0; k < fixes; ++k) if (fix_open(k)) LINEAR_REFUSE(10); /* a landing never reached */
        out[n++] = w;
        lin.done = 1;
        goto leave;
      }
      default:
        break;
    }
    out[n++] = w;
    ++p;
  }
leave:
  lin.n = n; lin.base = base; lin.offset = offset; lin.base_set = base_set; lin.fixes = fixes;
  lin.steps = steps; lin.p = p; lin.land = land;
  /* final up to the first BJUMP still waiting for its landing */
  lin.frontier = n;
  for (k = 0; k < fixes; ++k)
    if (fix_open(k) && lin.fix[k].base_at < lin.frontier) lin.frontier = lin.fix[k].base_at;
  return result;
}
/* The whole list (the tests; and what a list must be to be copied at all):
 * the copy's words, 0 if it cannot be copied. */
static u32 linearizeList(const u32 *list) {
  linearBegin(list);
  while (!lin.done)
    if (!linearRun(LINEAR_WORDS)) return 0;
  return lin.n;
}
/* The copy so far written back for the GE (its parsed part is released as
 * the parse goes): from what was last, to the frontier. */
static void linearFlush(void) {
  if (lin.frontier > lin.flushed) {
    const u32 from = lin.flushed & ~15u; /* (from the line's start) */
    sceKernelDcacheWritebackRange((u32 *)LINEAR_LIST + from, (lin.frontier - from) * 4u);
    lin.flushed = lin.frontier;
  }
}
/* A list found not to copy after its first chunk: the copy goes on where it
 * is. Each BJUMP waiting for its landing jumps there in the list; then BASE
 * and a JUMP to the word not copied. The JUMP's BASE is the game's when it
 * can be (the word's high address bits are the BASE's, as the game's own
 * JUMPs within a list) or the list sets its own first; otherwise it is the
 * JUMP's, the one way there (counted: linear_base_lost). No releasing after
 * (the JUMP leaves the copy: the stall would be in the list). */
static u32 linear_late, linear_base_lost;
static void linearGoOn(void) {
  u32 *const out = (u32 *)LINEAR_LIST;
  const u32 to = ((u32)lin.p & 0x0fffffffu) - lin.offset;
  unsigned k;
  for (k = 0; k < lin.fixes; ++k)
    if (fix_open(k)) {
      const u32 t = ((u32)lin.fix[k].target & 0x0fffffffu) - lin.fix[k].offset;
      if ((t & 0x0f000000u) != lin.fix[k].base && !linearBaseSetFirst(lin.fix[k].target) && GP_LOG) ++linear_base_lost;
      out[lin.fix[k].base_at] = (GE_CMD_BASE << 24) | ((t >> 8) & 0x0f0000u);
      out[lin.fix[k].jump_at] = (GE_CMD_BJUMP << 24) | (t & 0xffffffu);
      lin.fix[k].open = 0;
    }
  if ((to & 0x0f000000u) != lin.base && !linearBaseSetFirst(lin.p) && GP_LOG) ++linear_base_lost;
  out[lin.n++] = (GE_CMD_BASE << 24) | ((to >> 8) & 0x0f0000u);
  out[lin.n++] = (GE_CMD_JUMP << 24) | (to & 0xffffffu);
  lin.frontier = lin.n;
  lin.done = 1;
  release_blocked = 3; /* (code 3: where the GE goes next is not followed) */
  ++linear_late;
}
/* The parse at the copy's frontier (its stall): copied on, written back, the
 * stall moved. 0 when there is no more (the END was copied). */
static int linear_live; /* the parse is on a copy made as it goes */
static int linearMore(u32 **stall) {
  const u32 linear_started = GP_LOG ? sceKernelGetSystemTimeLow() : 0u;
  const u32 before = lin.frontier;
  if (lin.done) return 0;
  while (!lin.done && lin.frontier == before)
    if (!linearRun(lin.n + linear_chunk)) linearGoOn();
  linearFlush();
  *stall = (u32 *)LINEAR_LIST + lin.frontier;
  if (GP_LOG) linear_us += sceKernelGetSystemTimeLow() - linear_started;
  return lin.frontier != before;
}

int sceGeListEnQueuePatched(const void *list, void *stall, int cbid, PspGeListArgs *arg) {
  HOOK_ENTERED(2);
  if (GP_LOG) noteEvent('E', (u32)list);
#if GP_LOG
  render_thread = sceKernelGetThreadId();
#endif
  u32 started = sceKernelGetSystemTimeLow();
  waitFrameTaken(); /* the GE draws over the frame in VRAM */
  restoreIfIdle();
  /* the game's words back once our list is done, the GE idle or not (a
   * display copy after it): the next list may be parsed through the same
   * chunks, and our list is written again -- a JUMP of ours left there led
   * the parse and the GE into it (Tekken: camera and HUD HP gone) */
  if (grp_words && (linear_qid < 0 || _sceGeListSync(linear_qid, 1) <= 0)) {
    groupRestore();
    if (legacy_cache) sceKernelDcacheWritebackAll();
    else flushWrites();
  }
  recent_lists[recent_list_count++ & 3u] = (u32 *)((u32)list & 0x0fffffff);
  resetGeState();
  /* Queued at once, stalled at its first word, and released while parsed
   * (releaseParsed); then the game's stall (none, mostly). */
  /* the straightened copy, when the GE is done with the last one (the display
   * copy queued before it may still run: only the copy's own list counts) */
  if (release_enabled && group_enabled && list && !stall && (linear_qid < 0 || _sceGeListSync(linear_qid, 1) <= 0) &&
      linearBaseSetFirst((const u32 *)((u32)list & 0x0fffffffu))) {
    const int qid = _sceGeListEnQueue((void *)LINEAR_LIST, (void *)LINEAR_LIST, cbid, arg);
    linear_qid = qid;
    if (qid >= 0) {
      const u32 *const first = (const u32 *)((u32)list & 0x0fffffffu);
      u32 *t = (u32 *)LINEAR_LIST;
      *t++ = (u32)GE_CMD_OFFSETADDR << 24; /* (0, as a list starts) */
      *t++ = ((u32)GE_CMD_BASE << 24) | (((u32)first >> 8) & 0x0f0000u);
      *t++ = ((u32)GE_CMD_JUMP << 24) | ((u32)first & 0xffffffu);
      grp_piece = first;
      grp_top = t;
      grp_flushed = grp_stall = (u32 *)LINEAR_LIST;
      grp_top_end = (u32 *)LINEAR_LIST + LINEAR_WORDS;
      grp_opens = grp_done = grp_cur = 0;
      group_aborted = 0;
      release_qid = qid;
      release_high = 0;
      release_count_at = release_blocked = 0;
      release_bjump_until = NULL;
      release_pool_start = inset.used_vertices;
      group_live = 1;
#if GP_LOG
      if (group_release) retroStart();
#endif
      ++grp_lists;
      patchGeList((u32 *)((u32)list & 0x0fffffff), NULL);
      groupFinish();
      group_live = 0;
#if GP_LOG
      retro_on = 0;
#endif
      release_qid = -1;
      if (legacy_cache) sceKernelDcacheWritebackAll();
      else flushWrites();
      _sceGeListUpdateStallAddr(qid, stall); /* (NULL: the rest, to our END) */
      perf_parse_us += sceKernelGetSystemTimeLow() - started;
      ++perf_lists;
      return qid;
    }
  }
  const int release_now = release_enabled || (short_release && last_list_commands < SHORT_LIST_COMMANDS);
  const u32 commands_before = perf_commands;
  if (release_now && list && !stall && (linear_qid < 0 || _sceGeListSync(linear_qid, 1) <= 0)) {
    const u32 linear_started = GP_LOG ? sceKernelGetSystemTimeLow() : 0u;
    int copied;
    linearBegin((const u32 *)((u32)list & 0x0fffffffu));
    copied = linearRun(linear_chunk);
    while (copied && !lin.done && !lin.frontier) copied = linearRun(lin.n + linear_chunk); /* (a BJUMP at the start) */
    if (GP_LOG) linear_us += sceKernelGetSystemTimeLow() - linear_started;
    if (copied) {
      linearFlush();
      list = (const void *)LINEAR_LIST;
      ++linear_lists;
    } else {
      ++linear_fallbacks;
    }
  }
  if (release_now && list == (const void *)LINEAR_LIST) {
    const int qid = _sceGeListEnQueue(list, (void *)list, cbid, arg);
    linear_qid = qid;
    if (qid >= 0) {
      const u32 every_was = release_every;
      if (!release_enabled) release_every = SHORT_RELEASE_EVERY;
      release_qid = qid;
      if (GP_LOG) { ++rel_lists; rel_enqueued_at = sceKernelGetSystemTimeLow(); rel_first_done = 0; }
      release_high = (u32)list & 0xf0000000u;
      release_count_at = release_blocked = 0;
      release_bjump_until = NULL;
      release_pool_start = inset.used_vertices;
      linear_live = 1;
      patchGeList((u32 *)((u32)list & 0x0fffffff), (u32 *)LINEAR_LIST + lin.frontier);
      linear_live = 0;
      linearFlush(); /* (what a late JUMP away added) */
      release_qid = -1;
      release_every = every_was;
      last_list_commands = perf_commands - commands_before;
      if (GP_LOG) ++rel_block_code[release_blocked & 3u];
      /* the game's own stall back: none (NULL: "no stall", as it enqueued it --
       * not past the END the parse saw, where a SIGNAL's jump may not go) */
      _sceGeListUpdateStallAddr(qid, stall);
      perf_parse_us += sceKernelGetSystemTimeLow() - started;
      ++perf_lists;
      return qid;
    }
  }
  patchGeList((u32 *)((u32)list & 0x0fffffff), (u32 *)((u32)stall & 0x0fffffff));
  if (list && !stall) last_list_commands = perf_commands - commands_before;
  perf_parse_us += sceKernelGetSystemTimeLow() - started;
  ++perf_lists;
#if GP_TRACE
  ++trace_enqueue_count;
  trace_state("enqueue", trace_enqueue_count, (unsigned)list,
              (unsigned)state.has_draws, (unsigned)state.ge_cmd[GE_CMD_FRAMEBUFWIDTH],
              (unsigned)state.ge_cmd[GE_CMD_TEXFORMAT]);
#endif
  return _sceGeListEnQueue(list, stall, cbid, arg);
}
int sceGeListEnQueueHeadPatched(const void *list, void *stall, int cbid, PspGeListArgs *arg) {
  HOOK_ENTERED(3);
  u32 started = sceKernelGetSystemTimeLow();
  waitFrameTaken();
  restoreIfIdle();
  resetGeState();
  patchGeList((u32 *)((u32)list & 0x0fffffff), (u32 *)((u32)stall & 0x0fffffff));
  perf_parse_us += sceKernelGetSystemTimeLow() - started;
  ++perf_lists;
  return _sceGeListEnQueueHead(list, stall, cbid, arg);
}

// The upstream v0.19.1 policy: games with a fixed framebuffer present at
// GE completion; games that swap buffers present at SetFrameBuf. Do not turn
// VBlank waits into GPU submissions, or present partial enqueue batches.
static void restoreCompletedVertices(void) {
  u32 started = sceKernelGetSystemTimeLow();
  unsigned restored = gp_vertex_restore_complete(&vertex_restore, legacy_cache ? NULL : noteWrite);
  unsigned words = gp_inset_complete(&inset, legacy_cache ? NULL : noteWrite);
  words += restoreSavedWords();
  words += groupRestore();
  guard_vertex_used = guard_command_used = 0; /* the GE is done with the cut triangles */
  /* Written back for the GE; nothing here needs the CPU cache dropped. */
  if (legacy_cache) { if (restored || words) sceKernelDcacheWritebackAll(); }
  else flushWrites();
  perf_restore_us += sceKernelGetSystemTimeLow() - started;
}
/* (GP_LOG) how the game asks after the GE: [0] DrawSync, [1] ListSync; waits
 * (syncType 0) and peeks (1), and the peeks answered "busy" -- does it skip
 * drawing a frame while the GE is busy? */
static u32 sync_waits[2], sync_peeks[2], sync_peeks_busy[2];
/* (GP_LOG) the order of the game's GE calls in a frame, now and then: where
 * does it wait for the GE, and for what -- before showing a frame, before
 * building the next? Events: E enqueue (and its list), D/d a DrawSync
 * begun/done, L/l a ListSync, F SetFrameBuf; times in microseconds. */
#define EV_MAX 160u
static u32 ev_time[EV_MAX], ev_info[EV_MAX];
static char ev_kind[EV_MAX];
static unsigned ev_count, ev_armed;
static void noteEvent(char kind, u32 info) {
  if (!ev_armed || ev_count >= EV_MAX) return;
  ev_time[ev_count] = sceKernelGetSystemTimeLow();
  ev_info[ev_count] = info;
  ev_kind[ev_count++] = kind;
}
static void noteSync(unsigned which, int sync_type, int result) {
  if (sync_type == 0) ++sync_waits[which];
  else {
    ++sync_peeks[which];
    if (result > 0) ++sync_peeks_busy[which];
  }
}
int sceGeListSyncPatched(int qid, int syncType) {
  HOOK_ENTERED(4);
  u32 started = sceKernelGetSystemTimeLow();
  int result;
  if (GP_LOG) noteEvent('L', (u32)syncType);
  result = _sceGeListSync(qid, syncType);
  if (GP_LOG) noteEvent('l', (u32)result);
  if (GP_LOG) noteSync(1u, syncType, result);
  if (measure_only) return result;
  if (syncType == 0) perf_sync_wait_us += sceKernelGetSystemTimeLow() - started;
  // One list completing is insufficient if other lists still reference data.
  if (result == 0 && _sceGeDrawSync(1) == 0) restoreCompletedVertices();
  return result;
}
#if GP_PROFILE
/* The bare cost of reading a frame's commands: its first list walked (calls
 * followed, nothing done) right after the GE finished it, twice -- cold, then
 * warm. A slow cold walk means the parse waits on memory, not on its code. */
static volatile u32 walk_sink;
static u32 walkList(const u32 *p, u32 *cycles) {
  const u32 *stack[64];
  unsigned depth = 0;
  u32 base = 0, words = 0, sum = 0;
  const u32 started = cpuCycles();
  while (words < 400000u) {
    const u32 w = *p, cmd = w >> 24;
    ++words;
    sum += w;
    if (cmd == GE_CMD_BASE) {
      base = (w << 8) & 0x0f000000u;
    } else if (cmd == GE_CMD_CALL || cmd == GE_CMD_JUMP) {
      const u32 to = (base | (w & 0xffffffu)) & 0x0ffffffcu;
      if (to < 0x08400000u || to >= 0x0C000000u) break;
      if (cmd == GE_CMD_CALL) {
        if (depth == 64u) break;
        stack[depth++] = p + 1;
      }
      p = (const u32 *)to;
      continue;
    } else if (cmd == GE_CMD_RET) {
      if (!depth) break;
      p = stack[--depth];
      continue;
    } else if (cmd == GE_CMD_FINISH || cmd == GE_CMD_END) {
      break;
    }
    ++p;
  }
  *cycles = cpuCycles() - started;
  walk_sink = sum;
  return words;
}
static const u32 *walk_candidate;
static unsigned walk_tick;
#endif
int sceGeDrawSyncPatched(int syncType) {
  HOOK_ENTERED(5);
  u32 started = sceKernelGetSystemTimeLow();
  int result;
  if (GP_LOG) noteEvent('D', (u32)syncType);
  result = _sceGeDrawSync(syncType);
  if (GP_LOG) noteEvent('d', (u32)result);
  if (GP_LOG) noteSync(0u, syncType, result);
  if (measure_only) {
    if (syncType == 0) { perf_sync_wait_us += sceKernelGetSystemTimeLow() - started; ++perf_syncs; }
    return result;
  }
  if (syncType == 0) {
    const u32 waited = sceKernelGetSystemTimeLow() - started;
    perf_sync_wait_us += waited;
    ++perf_syncs;
    /* Once: a short list the GE took over 15 ms for (DW's movie frames). */
    if (!slow_dumped && waited > 15000u && recent_list_count && recent_list_count <= 4u) {
      unsigned l, i, t, targets = 0;
      u32 base = 0, target[3];
      slow_dumped = 1;
      for (l = 0; l < recent_list_count; ++l) {
        const u32 *p = recent_lists[l];
        log("[gp-slowlist] wait_us=%u list=%u/%u at=%08x at_ms=%u\n", (unsigned)waited, l, recent_list_count,
            (unsigned)p, (unsigned)(sceKernelGetSystemTimeLow() / 1000u));
        for (i = 0; i < 160u; i += 8u) {
          log("[gp-slowlist] %08x %08x %08x %08x %08x %08x %08x %08x\n",
              (unsigned)p[i], (unsigned)p[i + 1], (unsigned)p[i + 2], (unsigned)p[i + 3],
              (unsigned)p[i + 4], (unsigned)p[i + 5], (unsigned)p[i + 6], (unsigned)p[i + 7]);
          for (t = 0; t < 8u; ++t) {
            const u32 w = p[i + t];
            if ((w >> 24) == GE_CMD_BASE) base = (w << 8) & 0x0f000000u;
            if (((w >> 24) == GE_CMD_CALL || (w >> 24) == GE_CMD_JUMP) && targets < 3u) {
              const u32 to = (base | (w & 0xffffffu)) & 0x0ffffffcu;
              if (to >= 0x08400000u && to < 0x0A400000u && to != (u32)(p + i + t + 1)) target[targets++] = to;
            }
          }
          if ((p[i] >> 24) == GE_CMD_END || (p[i + 7] >> 24) == GE_CMD_END) break;
        }
      }
      /* What those lists call or jump to, where the drawing usually is. */
      for (t = 0; t < targets; ++t) {
        const u32 *p = (const u32 *)target[t];
        log("[gp-slowlist] target %u at=%08x\n", t, (unsigned)target[t]);
        for (i = 0; i < 96u; i += 8u) {
          log("[gp-slowlist] %08x %08x %08x %08x %08x %08x %08x %08x\n",
              (unsigned)p[i], (unsigned)p[i + 1], (unsigned)p[i + 2], (unsigned)p[i + 3],
              (unsigned)p[i + 4], (unsigned)p[i + 5], (unsigned)p[i + 6], (unsigned)p[i + 7]);
          if ((p[i] >> 24) == GE_CMD_RET || (p[i + 7] >> 24) == GE_CMD_RET ||
              (p[i] >> 24) == GE_CMD_END || (p[i + 7] >> 24) == GE_CMD_END) break;
        }
      }
    }
#if GP_PROFILE
    walk_candidate = recent_list_count ? recent_lists[0] : NULL;
#endif
    recent_list_count = 0;
  }
  if (result == 0) restoreCompletedVertices();
#if GP_PROFILE
  if (syncType == 0 && result == 0 && walk_candidate && ++walk_tick >= 240u) {
    u32 cold, warm, words;
    walk_tick = 0;
    words = walkList(walk_candidate, &cold);
    walkList(walk_candidate, &warm);
    log("[gp-walk] list=%08x words=%u cold_cycles=%u (%u per word) warm_cycles=%u (%u per word)\n",
        (unsigned)walk_candidate, (unsigned)words, (unsigned)cold, words ? (unsigned)(cold / words) : 0u,
        (unsigned)warm, words ? (unsigned)(warm / words) : 0u);
  }
#endif
  /* A game that shows its frames (SetFrameBuf in the last 100 ms) has them
   * copied then; one that keeps a fixed framebuffer, at the GE's completion.
   * (Upstream counted syncs down from SetFrameBuf: Tekken waits three times a
   * frame, and every frame was copied twice, the first half drawn.) */
  if ((!framebuf_seen || sceKernelGetSystemTimeLow() - framebuf_set_at > 100000u) && syncType == 0 && result == 0) {
    if (copyFrameBuffer() >= 0) rendered_in_sync = 1;
  }
#if GP_TRACE
  ++trace_sync_count;
  trace_state("sync", trace_sync_count, (unsigned)result, (unsigned)syncType,
              (unsigned)framebuf_set_at, (unsigned)rendered_in_sync);
  if (trace_sync_count <= 8u || trace_sync_count % 120u == 0u)
    log("[gp-vertices3] sync=%u active=%u peak=%u full=%u unsupported=%u bounds=%u zbuf=%06x/%06x ztest=%06x/%06x clear=%06x\n",
        trace_sync_count, vertex_restore.used, vertex_restore.peak,
        vertex_restore.exhausted, vertex_restore.unsupported, vertex_bounds_errors,
        (unsigned)state.ge_cmd[GE_CMD_ZBUFPTR], (unsigned)state.ge_cmd[GE_CMD_ZBUFWIDTH],
        (unsigned)state.ge_cmd[GE_CMD_ZTESTENABLE], (unsigned)state.ge_cmd[GE_CMD_ZTEST],
        (unsigned)state.ge_cmd[GE_CMD_CLEARMODE]);
  if (trace_sync_count <= 8u || trace_sync_count % 120u == 0u)
    log("[gp-inset1] sync=%u applied=%u reused=%u unsupported=%u full=%u overwritten=%u peak=%u\n",
        trace_sync_count, inset.applied, inset.reused, inset.unsupported, inset.full,
        inset.overwritten, inset.peak_vertices);
#endif
  return result;
}
/* Where a game's frame time goes: CPU time in our list parsing, CPU time
 * blocked waiting for the GE (the GE's drawing, ours included), frames shown. */
/* System dialogs (savedata, message): one line per dialog with what its
 * frames cost, from its InitStart to its ShutdownStart. */
static u32 dialog_open, dialog_kind, dialog_start, dialog_frames, dialog_parse_us, dialog_wait_us;
static u32 dialog_commands, dialog_inset, dialog_converted, dialog_copies;
int (*_sceUtilitySavedataInitStart)(void *);
int (*_sceUtilitySavedataShutdownStart)(void);
int (*_sceUtilityMsgDialogInitStart)(void *);
int (*_sceUtilityMsgDialogShutdownStart)(void);
static void dialogBegin(u32 kind) {
  dialog_open = 1; dialog_kind = kind; dialog_start = sceKernelGetSystemTimeLow();
  dialog_frames = dialog_parse_us = dialog_wait_us = dialog_commands = 0;
  dialog_inset = inset.applied; dialog_converted = 0; dialog_copies = copies_queued;
  dialog_parse_us = perf_parse_us; dialog_wait_us = perf_sync_wait_us; dialog_commands = perf_commands;
}
static void dialogEnd(void) {
  u32 ms;
  (void)ms; /* only logged */
  if (!dialog_open) return;
  dialog_open = 0;
  ms = (sceKernelGetSystemTimeLow() - dialog_start) / 1000u;
  log("[gp-dialog] kind=%s ms=%u frames=%u fps10=%u parse_ms=%u ge_wait_ms=%u commands=%u inset=%u converted_frames=%u format=%u stencil_hold=%u\n",
      dialog_kind ? "message" : "savedata", (unsigned)ms, (unsigned)dialog_frames,
      ms ? (unsigned)(dialog_frames * 10000u / ms) : 0u,
      (unsigned)((perf_parse_us - dialog_parse_us) / 1000u), (unsigned)((perf_sync_wait_us - dialog_wait_us) / 1000u),
      (unsigned)(perf_commands - dialog_commands), (unsigned)(inset.applied - dialog_inset),
      (unsigned)dialog_converted, (unsigned)draw_format, (unsigned)stencil_frames_left);
}
int sceUtilitySavedataInitStartPatched(void *params) { dialogBegin(0); return _sceUtilitySavedataInitStart(params); }
int sceUtilitySavedataShutdownStartPatched(void) { dialogEnd(); return _sceUtilitySavedataShutdownStart(); }
int sceUtilityMsgDialogInitStartPatched(void *params) { dialogBegin(1); return _sceUtilityMsgDialogInitStart(params); }
int sceUtilityMsgDialogShutdownStartPatched(void) { dialogEnd(); return _sceUtilityMsgDialogShutdownStart(); }

/* (GP_LOG) frame pacing: frames by VBlanks taken (1, 2, 3, 4+), the longest
 * frame, and the most parse and GE wait in one frame */
static u32 pace_hist[4], pace_max_us, pace_parse_max, pace_wait_max, pace_last, pace_parse_at, pace_wait_at;
static void reportPerformance(void) {
  u32 now = sceKernelGetSystemTimeLow(), elapsed = now - perf_window_start;
  if (GP_LOG && pace_last) {
    const u32 frame = now - pace_last, vblanks = (frame + 8341u) / 16683u;
    ++pace_hist[vblanks <= 1u ? 0 : vblanks >= 4u ? 3 : vblanks - 1u];
    if (frame > pace_max_us) pace_max_us = frame;
    if (perf_parse_us - pace_parse_at > pace_parse_max) pace_parse_max = perf_parse_us - pace_parse_at;
    if (perf_sync_wait_us - pace_wait_at > pace_wait_max) pace_wait_max = perf_sync_wait_us - pace_wait_at;
  }
  pace_last = now;
  pace_parse_at = perf_parse_us;
  pace_wait_at = perf_sync_wait_us;
  ++perf_frames;
  if (dialog_open) { ++dialog_frames; if (draw_format != GE_FORMAT_565) ++dialog_converted; }
  if (stencil_frames_left) --stencil_frames_left;
  if (truecolor_frames_left) --truecolor_frames_left;
  if (alpha_frames_left) --alpha_frames_left;
  if (!perf_window_start) { perf_window_start = now; return; }
  if (elapsed < 5000000u) return;
  log("[gp-prof] 2d_prims=%u scaled_vertices=%u scale_ms=%u flush_ms=%u restore_ms=%u copy_ms=%u\n",
      (unsigned)perf_2d_prims, (unsigned)perf_scaled_vertices, (unsigned)(perf_scale_us / 1000u),
      (unsigned)(perf_flush_us / 1000u), (unsigned)(perf_restore_us / 1000u), (unsigned)(perf_copy_us / 1000u));
#if GP_LOG
  {
    unsigned sw, vr, f;
    for (sw = 0; sw < 2u; ++sw)
      for (vr = 0; vr < 2u; ++vr) {
        char line[200];
        int n = sprintf(line, "[gp-tex] %s %s vertices=%u draws by format:", sw ? "swizzled" : "linear", vr ? "vram" : "ram",
                        (unsigned)tex_vertices[sw][vr]);
        for (f = 0; f < 11u; ++f) n += sprintf(line + n, " %u", (unsigned)tex_draws[sw][vr][f]);
        log("%s\n", line);
      }
    memset(tex_draws, 0, sizeof(tex_draws));
    memset(tex_vertices, 0, sizeof(tex_vertices));
  }
#endif
#if GP_LOG
  if (ev_count) {
    unsigned k;
    for (k = 0; k < ev_count; ++k)
      log("[gp-event] %c %08x at %u us (+%u)\n", ev_kind[k], (unsigned)ev_info[k], (unsigned)(ev_time[k] - ev_time[0]),
          k ? (unsigned)(ev_time[k] - ev_time[k - 1]) : 0u);
  }
  ev_count = 0;
  ev_armed = 1; /* the next frames, until EV_MAX events */
#endif
  log("[gp-linear] copied=%u not=%u why: long=%u base=%u offset=%u origin=%u signal=%u nobase=%u back=%u many=%u endsig=%u nolanding=%u last=%08x at %08x\n",
      (unsigned)linear_lists, (unsigned)linear_fallbacks, (unsigned)linear_why[1], (unsigned)linear_why[2],
      (unsigned)linear_why[3], (unsigned)linear_why[4], (unsigned)linear_why[5], (unsigned)linear_why[6],
      (unsigned)linear_why[7], (unsigned)linear_why[8], (unsigned)linear_why[9], (unsigned)linear_why[10],
      (unsigned)linear_why_word, (unsigned)linear_why_at);
  log("[gp-linear] jumps=%u chunk_max=%u cross=%u\n", (unsigned)linear_jumps, (unsigned)linear_chunk_max,
      (unsigned)linear_cross);
  linear_jumps = linear_chunk_max = linear_cross = 0;
  log("[gp-group] lists=%u groups=%u merged=%u aborted=%u releases=%u\n", (unsigned)grp_lists, (unsigned)grp_groups,
      (unsigned)grp_merged, (unsigned)grp_aborts, (unsigned)grp_releases);
  log("[gp-group] why: bjump_back=%u opens=%u base=%u full=%u signal=%u merge_full=%u bjumps=%u first at %08x: %08x %08x base|why %08x offset %08x to %08x\n",
      (unsigned)grp_why[0], (unsigned)grp_why[1], (unsigned)grp_why[2], (unsigned)grp_why[3], (unsigned)grp_why[4],
      (unsigned)grp_why[5], (unsigned)grp_bjumps, (unsigned)grp_ab_at, (unsigned)grp_ab_prev, (unsigned)grp_ab_word,
      (unsigned)grp_ab_base, (unsigned)grp_ab_offset, (unsigned)grp_ab_to);
  grp_why[0] = grp_why[1] = grp_why[2] = grp_why[3] = grp_why[4] = grp_why[5] = grp_bjumps = grp_ab_at = 0;
#if GP_LOG
  log("[gp-geat] top=%u done=%u other=%u idle=%u BAD cur=%u piece=%u past=%u first why=%u pc %08x stall_hw %08x ours %08x piece %08x j %08x\n",
      (unsigned)gpr_top, (unsigned)gpr_done, (unsigned)gpr_other, (unsigned)gpr_idle, (unsigned)gpr_bad_cur,
      (unsigned)gpr_bad_piece, (unsigned)gpr_bad_past, (unsigned)gpr_why, (unsigned)gpr_at, (unsigned)gpr_stall_hw,
      (unsigned)gpr_stall, (unsigned)gpr_piece, (unsigned)gpr_j);
  gpr_top = gpr_done = gpr_other = gpr_idle = gpr_bad_cur = gpr_bad_piece = gpr_bad_past = gpr_at = 0;
  log("[gp-retro] written_after_run=%u walked=%u runs=%u%s\n", (unsigned)retro_count, (unsigned)retro_walked, rw_runs,
      rw_full ? " (some outside the map)" : "");
  {
    unsigned k;
    for (k = 0; k < retro_logged; ++k)
      log("[gp-retro]   at %08x size %u now %08x parse at %08x\n", (unsigned)retro_at[k], (unsigned)retro_kind[k],
          (unsigned)retro_now[k], (unsigned)retro_parse[k]);
  }
  retro_count = retro_logged = retro_walked = 0;
  log("[gp-escape] walks into the next chunk: %u\n", (unsigned)esc_count);
  esc_count = esc_logged = 0;
#endif
  grp_lists = grp_groups = grp_merged = grp_aborts = grp_releases = 0;
  log("[gp-linear] copy_ms=%u vblank_waits_not_held=%u late=%u base_lost=%u\n",
      (unsigned)(linear_us / 1000u), (unsigned)perf_pace_free, (unsigned)linear_late,
      (unsigned)linear_base_lost);
  perf_pace_free = linear_late = linear_base_lost = 0;
  linear_lists = linear_fallbacks = linear_us = 0;
  { unsigned why; for (why = 0; why < 11u; ++why) linear_why[why] = 0; }
  log("[gp-release] lists=%u updates=%u first_after_us=%u (of %u) held: blocked=%u inset=%u pending=%u bjump=%u; ended blocked: none=%u bjump_back=%u jump=%u signal=%u\n",
      (unsigned)rel_lists, (unsigned)perf_releases, rel_first_count ? (unsigned)(rel_first_us / rel_first_count) : 0u,
      (unsigned)rel_first_count, (unsigned)rel_held_blocked, (unsigned)rel_held_inset, (unsigned)rel_held_pending,
      (unsigned)rel_held_bjump, (unsigned)rel_block_code[0], (unsigned)rel_block_code[1], (unsigned)rel_block_code[2],
      (unsigned)rel_block_code[3]);
  rel_lists = perf_releases = rel_first_us = rel_first_count = 0;
  rel_held_blocked = rel_held_inset = rel_held_pending = rel_held_bjump = 0;
  rel_block_code[0] = rel_block_code[1] = rel_block_code[2] = rel_block_code[3] = 0;
  log("[gp-sync] drawsync waits=%u peeks=%u busy=%u, listsync waits=%u peeks=%u busy=%u\n",
      (unsigned)sync_waits[0], (unsigned)sync_peeks[0], (unsigned)sync_peeks_busy[0],
      (unsigned)sync_waits[1], (unsigned)sync_peeks[1], (unsigned)sync_peeks_busy[1]);
  sync_waits[0] = sync_waits[1] = sync_peeks[0] = sync_peeks[1] = sync_peeks_busy[0] = sync_peeks_busy[1] = 0;
  log("[gp-direct] frames_left_in_vram=%u take_timeouts=%u\n", (unsigned)frames_left_in_vram,
      (unsigned)take_timeouts);
  log("[gp-draws] setting=%u video=%u dialog=%u 3d_prims=%u dropped_fb=%u dropped_tex=%u vertex_full=%u vertex_unsupported=%u bounds=%u saved_full=%u vram_copies=%u vram_kb=%u\n",
      (unsigned)*(volatile u32 *)GP_MODE_ADDR, (unsigned)*(volatile u32 *)GP_VIDEO_ADDR,
      (unsigned)dialog_open, (unsigned)perf_3d_prims, (unsigned)perf_dropped_fb, (unsigned)perf_dropped_tex,
      vertex_restore.exhausted, vertex_restore.unsupported, vertex_bounds_errors, (unsigned)lod_full,
      (unsigned)perf_vram_copies, (unsigned)(perf_vram_bytes >> 10));
  log("[gp-runs] runs=%u one_by_one=%u peak_ranges=%u hash_peak=%u\n", vertex_restore.runs, vertex_restore.one_by_one,
      vertex_restore.peak_ranges, vertex_restore.peak);
  vertex_restore.runs = vertex_restore.one_by_one = vertex_restore.peak_ranges = vertex_restore.peak = 0;
  perf_2d_prims = perf_scaled_vertices = perf_scale_us = perf_flush_us = perf_restore_us = perf_copy_us = 0;
  log("[gp-guard] checked=%u dropped_here=%u cut=%u vertices=%u not_cut=%u deep=%u other=%u ms=%u\n",
      (unsigned)perf_guard_checked, (unsigned)perf_guard_dropped, (unsigned)perf_guard_cut,
      (unsigned)perf_guard_vertices, (unsigned)perf_guard_full, (unsigned)perf_guard_deep, (unsigned)perf_guard_other,
      (unsigned)(perf_guard_us / 1000u));
  perf_guard_checked = perf_guard_dropped = perf_guard_other = perf_guard_us = perf_guard_deep = 0;
  perf_guard_cut = perf_guard_vertices = perf_guard_full = 0;
  log("[gp-sub] clean_calls=%u clean_words=%u dirty_calls=%u dirty_words=%u\n", (unsigned)perf_clean_calls,
      (unsigned)perf_clean_words, (unsigned)perf_dirty_calls, (unsigned)perf_dirty_words);
  perf_clean_words = perf_dirty_words = perf_clean_calls = perf_dirty_calls = 0;
#if GP_LOG && GP_SUBMEASURE
  log("[gp-cache] same_calls=%u same_words=%u same_pure_calls=%u same_pure_words=%u pure_words=%u lost=%u\n",
      (unsigned)sub_same_calls, (unsigned)sub_same_words, (unsigned)sub_same_pure_calls,
      (unsigned)sub_same_pure_words, (unsigned)sub_pure_words, (unsigned)sub_lost);
  sub_same_calls = sub_same_words = sub_same_pure_calls = sub_same_pure_words = sub_pure_words = sub_lost = 0;
#endif
  log("[gp-hot] calls=%u words=%u kcycles=%u (%u per word) releases=%u kcycles=%u\n", (unsigned)prof_hot_calls,
      (unsigned)prof_hot_words, (unsigned)(prof_hot_cycles / 1000u),
      prof_hot_words ? (unsigned)(prof_hot_cycles / prof_hot_words) : 0u, (unsigned)prof_rel_count,
      (unsigned)(prof_rel_cycles / 1000u));
  prof_hot_cycles = prof_hot_words = prof_hot_calls = prof_rel_cycles = prof_rel_count = 0;
#if GP_PROFILE
  {
    unsigned r, c, best, which;
    for (which = 0; which < 2u; ++which) {
      u32 *const h = which ? prof_hot_stop : prof_hot_cmd;
      char line[256];
      int n = sprintf(line, which ? "[gp-hot] stops:" : "[gp-hot] words:");
      for (r = 0; r < 14u; ++r) {
        best = 0;
        for (c = 1; c < 256u; ++c) if (h[c] > h[best]) best = c;
        if (!h[best]) break;
        n += sprintf(line + n, " %02x=%u", best, (unsigned)h[best]);
        h[best] = 0;
      }
      log("%s\n", line);
      memset(h, 0, 256u * sizeof(u32));
    }
    log("[gp-hot] bone_calls=%u\n", (unsigned)prof_bone_calls);
    prof_bone_calls = 0;
  }
#endif
  log("[gp-parse] matrix_words=%u fast_words=%u kernel_free_kb=%u\n", (unsigned)perf_matrix_words,
      (unsigned)perf_fast_words, (unsigned)(sceKernelPartitionMaxFreeMemSize(1) >> 10));
  perf_matrix_words = perf_fast_words = 0;
  if (GP_PROFILE) {
    unsigned k, top;
    for (top = 0; top < 16u; ++top) {
      unsigned best = 0;
      for (k = 1; k < 256u; ++k) if (cmd_cycles[k] > cmd_cycles[best]) best = k;
      if (!cmd_cycles[best]) break;
      log("[gp-cmd] %02x count=%u kcycles=%u\n", best, (unsigned)cmd_count[best], (unsigned)(cmd_cycles[best] / 1000u));
      cmd_cycles[best] = 0;
    }
    log("[gp-cmd] prim 2d kcycles=%u scale=%u inset=%u\n", (unsigned)(prim_2d_cycles / 1000u),
        (unsigned)(prim_scale_cycles / 1000u), (unsigned)(prim_inset_cycles / 1000u));
    log("[gp-cmd] sections kcycles matrix=%u fast=%u handled=%u parse=%u\n", (unsigned)(sec_cycles[0] / 1000u),
        (unsigned)(sec_cycles[1] / 1000u), (unsigned)(sec_cycles[2] / 1000u), (unsigned)(parse_cycles / 1000u));
    {
      unsigned k, top;
      for (top = 0; top < 12u; ++top) {
        unsigned best = 0;
        for (k = 1; k < 256u; ++k) if (word_cycles[k] > word_cycles[best]) best = k;
        if (!word_cycles[best]) break;
        log("[gp-word] %02x words=%u kcycles=%u\n", best, (unsigned)word_count[best], (unsigned)(word_cycles[best] / 1000u));
        word_cycles[best] = 0;
      }
      memset(word_cycles, 0, sizeof(word_cycles));
      memset(word_count, 0, sizeof(word_count));
    }
    log("[gp-cmd] before case kcycles=%u saved=%u inset=%u\n", (unsigned)(pre_cycles / 1000u),
        (unsigned)(saved_cycles / 1000u), (unsigned)(inset_cycles / 1000u));
    sec_cycles[0] = sec_cycles[1] = sec_cycles[2] = parse_cycles = 0;
    pre_cycles = saved_cycles = inset_cycles = 0;
    prim_2d_cycles = prim_scale_cycles = prim_inset_cycles = 0;
    memset(cmd_cycles, 0, sizeof(cmd_cycles));
    memset(cmd_count, 0, sizeof(cmd_count));
  }
  perf_3d_prims = perf_dropped_fb = perf_dropped_tex = 0;
  vertex_restore.exhausted = vertex_restore.unsupported = vertex_bounds_errors = 0;
  lod_full = perf_vram_copies = perf_vram_bytes = 0;
  if (ge_samples) {
    log("[gp-ge] busy=%u%% of %u looks\n", (unsigned)(ge_busy_samples * 100u / ge_samples), (unsigned)ge_samples);
    ge_samples = ge_busy_samples = 0;
  }
  log("[gp-perf] measure=%u ms=%u frames=%u fps10=%u cpu=%d bus=%d lists=%u commands=%u parse_ms=%u ge_wait_ms=%u syncs=%u copies=%u copy_err=%u inset=%u format=%u legacy565=%u\n",
      (unsigned)(measure_only | legacy_cache << 1),
      (unsigned)(elapsed / 1000u), (unsigned)perf_frames, (unsigned)(perf_frames * 10000u / (elapsed / 1000u)), /* no 64-bit division in a kernel PRX */
      scePowerGetCpuClockFrequencyInt(), scePowerGetBusClockFrequencyInt(),
      (unsigned)perf_lists, (unsigned)perf_commands, (unsigned)(perf_parse_us / 1000u), (unsigned)(perf_sync_wait_us / 1000u),
      (unsigned)perf_syncs, copies_queued, copy_errors, inset.applied, (unsigned)draw_format,
      (unsigned)force_565);
  perf_window_start = now;
  if (dialog_open) { /* rebase the dialog's start values on the new window */
    dialog_parse_us -= perf_parse_us; dialog_wait_us -= perf_sync_wait_us; dialog_commands -= perf_commands;
    dialog_inset -= inset.applied;
  }
  log("[gp-shown] gaps_vblanks 1=%u 2=%u 3=%u 4=%u 5=%u 6+=%u; frame_ms %u %u %u %u %u %u %u\n",
      (unsigned)shown_gaps[1], (unsigned)shown_gaps[2], (unsigned)shown_gaps[3], (unsigned)shown_gaps[4],
      (unsigned)shown_gaps[5], (unsigned)shown_gaps[6], (unsigned)drawn_ms[0], (unsigned)drawn_ms[1],
      (unsigned)drawn_ms[2], (unsigned)drawn_ms[3], (unsigned)drawn_ms[4], (unsigned)drawn_ms[5], (unsigned)drawn_ms[6]);
  memset(shown_gaps, 0, sizeof(shown_gaps));
  memset(drawn_ms, 0, sizeof(drawn_ms));
  log("[gp-frames] vblanks 1=%u 2=%u 3=%u 4+=%u longest_ms=%u parse_max_ms=%u wait_max_ms=%u\n",
      (unsigned)pace_hist[0], (unsigned)pace_hist[1], (unsigned)pace_hist[2], (unsigned)pace_hist[3],
      (unsigned)(pace_max_us / 1000u), (unsigned)(pace_parse_max / 1000u), (unsigned)(pace_wait_max / 1000u));
  pace_hist[0] = pace_hist[1] = pace_hist[2] = pace_hist[3] = 0;
  pace_max_us = pace_parse_max = pace_wait_max = 0;
  perf_frames = perf_lists = perf_syncs = perf_commands = 0;
  pace_parse_at = pace_wait_at = 0;
  perf_parse_us = perf_sync_wait_us = 0;
  copies_queued = 0;
  inset.applied = 0;
}

/* The game's own movie calls (mpeg.prx, sceMpeg via syscalls; PSMF players
 * call it too): sceMpegCreate comes before the first frame, so the mode is
 * 480x272 before the movie draws anything. The module is loaded for a movie
 * and may be unloaded after: hooked whenever Adrenaline's systemctrl starts
 * it (its exports are linked at load, before the start). */
static int (*_sceMpegCreate)(int, int, int, int, int, int, int);
static int (*_sceMpegDelete)(int);
static int (*_sceMpegAvcDecode)(int, int, int, int, int);
/* what PSMF players decode with: YCbCr, then the colour conversion */
static int (*_sceMpegAvcDecodeYCbCr)(int, int, int, int, int);
static int (*_sceMpegAvcCsc)(int, int, int, int, int);
static u32 mpeg_decodes; /* calls seen, for the log */
static u32 mpeg_frames_ago = 0xffffu;
static STMOD_HANDLER previous_start_handler;
static int sceMpegCreatePatched(int a, int b, int c, int d, int e, int f, int g) {
  int result = _sceMpegCreate(a, b, c, d, e, f, g);
  if (result == 0) mpeg_frames_ago = 0;
  return result;
}
static int sceMpegDeletePatched(int mpeg) {
  mpeg_frames_ago = 0xffffu; /* the movie is over */
  return _sceMpegDelete(mpeg);
}
static int sceMpegAvcDecodePatched(int a, int b, int c, int d, int e) {
  mpeg_frames_ago = 0; ++mpeg_decodes;
  return _sceMpegAvcDecode(a, b, c, d, e);
}
static int sceMpegAvcDecodeYCbCrPatched(int a, int b, int c, int d, int e) {
  mpeg_frames_ago = 0; ++mpeg_decodes;
  return _sceMpegAvcDecodeYCbCr(a, b, c, d, e);
}
static int sceMpegAvcCscPatched(int a, int b, int c, int d, int e) {
  mpeg_frames_ago = 0; ++mpeg_decodes;
  return _sceMpegAvcCsc(a, b, c, d, e);
}
static void hookMpeg(void) {
  const u32 create = sctrlHENFindFunction("sceMpeg_library", "sceMpeg", 0xD8C5F121);
  if (!create) { log("[gp-mpeg] sceMpegCreate not found\n"); return; }
  _sceMpegCreate = (void *)create;
  _sceMpegDelete = (void *)sctrlHENFindFunction("sceMpeg_library", "sceMpeg", 0x606A4649);
  _sceMpegAvcDecode = (void *)sctrlHENFindFunction("sceMpeg_library", "sceMpeg", 0x0E3C2E9D);
  sctrlHENPatchSyscall((void *)_sceMpegCreate, sceMpegCreatePatched);
  if (_sceMpegDelete) sctrlHENPatchSyscall((void *)_sceMpegDelete, sceMpegDeletePatched);
  if (_sceMpegAvcDecode) sctrlHENPatchSyscall((void *)_sceMpegAvcDecode, sceMpegAvcDecodePatched);
  _sceMpegAvcDecodeYCbCr = (void *)sctrlHENFindFunction("sceMpeg_library", "sceMpeg", 0xF0EB1125);
  _sceMpegAvcCsc = (void *)sctrlHENFindFunction("sceMpeg_library", "sceMpeg", 0x31BD0272);
  if (_sceMpegAvcDecodeYCbCr) sctrlHENPatchSyscall((void *)_sceMpegAvcDecodeYCbCr, sceMpegAvcDecodeYCbCrPatched);
  if (_sceMpegAvcCsc) sctrlHENPatchSyscall((void *)_sceMpegAvcCsc, sceMpegAvcCscPatched);
  log("[gp-mpeg] hooked create=%08x delete=%08x decode=%08x at_ms=%u\n", (unsigned)create,
      (unsigned)_sceMpegDelete, (unsigned)_sceMpegAvcDecode, (unsigned)(sceKernelGetSystemTimeLow() / 1000u));
}

/* Below sceMpeg: the AVC decoder itself (avcodec.prx). DW calls none of the
 * sceMpeg functions through syscalls (0 decodes seen), so it decodes with
 * this, or through a library linked into the game that calls it. */
static int (*_sceVideocodecDecode)(void *, int);
static int (*_sceVideocodecOpen)(void *, int), (*_sceVideocodecInit)(void *, int);
static int (*_sceVideocodecStop)(void *, int), (*_sceVideocodecDelete)(void *, int);
static const char *videocodec_module;
/* Opened: a movie is about to play. DW draws it 1.2 s before the first decode,
 * and at 960x544 each of those frames took 47 ms. Kept until closed (or 3 s
 * without a decode: a decoder left open). */
static u32 videocodec_open;
#define VIDEOCODEC_IDLE_FRAMES 180u
static int sceVideocodecDecodePatched(void *buffer, int type) {
  mpeg_frames_ago = 0; ++mpeg_decodes; movie_playing = 1;
  return _sceVideocodecDecode(buffer, type);
}
static int sceVideocodecOpenPatched(void *buffer, int type) {
  videocodec_open = 1; mpeg_frames_ago = 0; movie_playing = 1;
  return _sceVideocodecOpen(buffer, type);
}
static int sceVideocodecInitPatched(void *buffer, int type) {
  videocodec_open = 1; mpeg_frames_ago = 0; movie_playing = 1;
  return _sceVideocodecInit(buffer, type);
}
static int sceVideocodecStopPatched(void *buffer, int type) {
  videocodec_open = 0; mpeg_frames_ago = 0xffffu; movie_playing = 0; /* skipped or over */
  return _sceVideocodecStop(buffer, type);
}
static int sceVideocodecDeletePatched(void *buffer, int type) {
  videocodec_open = 0; mpeg_frames_ago = 0xffffu; movie_playing = 0;
  return _sceVideocodecDelete(buffer, type);
}
static const char *const videocodec_modules[] = {"sceAvcodec_driver", "sceAvcodec_Driver", "sceVideocodec_Driver"};
static void hookVideocodec(const char *started) {
  unsigned i;
  for (i = 0; i < sizeof(videocodec_modules) / sizeof(videocodec_modules[0]); ++i) {
    u32 decode;
    if (started && strcmp(started, videocodec_modules[i]) != 0) continue;
    decode = sctrlHENFindFunction(videocodec_modules[i], "sceVideocodec", 0xDBA273FA);
    if (!decode) continue;
    _sceVideocodecDecode = (void *)decode;
    sctrlHENPatchSyscall((void *)decode, sceVideocodecDecodePatched);
    _sceVideocodecOpen = (void *)sctrlHENFindFunction(videocodec_modules[i], "sceVideocodec", 0xC01EC829);
    _sceVideocodecInit = (void *)sctrlHENFindFunction(videocodec_modules[i], "sceVideocodec", 0x17099F0A);
    _sceVideocodecStop = (void *)sctrlHENFindFunction(videocodec_modules[i], "sceVideocodec", 0xA2F0564E);
    _sceVideocodecDelete = (void *)sctrlHENFindFunction(videocodec_modules[i], "sceVideocodec", 0x307E6E1C);
    if (_sceVideocodecOpen) sctrlHENPatchSyscall((void *)_sceVideocodecOpen, sceVideocodecOpenPatched);
    if (_sceVideocodecInit) sctrlHENPatchSyscall((void *)_sceVideocodecInit, sceVideocodecInitPatched);
    if (_sceVideocodecStop) sctrlHENPatchSyscall((void *)_sceVideocodecStop, sceVideocodecStopPatched);
    if (_sceVideocodecDelete) sctrlHENPatchSyscall((void *)_sceVideocodecDelete, sceVideocodecDeletePatched);
    videocodec_module = videocodec_modules[i];
    log("[gp-mpeg] hooked sceVideocodecDecode=%08x open=%08x init=%08x stop=%08x delete=%08x in %s at_ms=%u\n",
        (unsigned)decode, (unsigned)_sceVideocodecOpen, (unsigned)_sceVideocodecInit, (unsigned)_sceVideocodecStop,
        (unsigned)_sceVideocodecDelete, videocodec_module, (unsigned)(sceKernelGetSystemTimeLow() / 1000u));
    return;
  }
}
static int onModuleStart(SceModule *mod) {
  static unsigned logged;
  if (strcmp(mod->modname, "sceMpeg_library") == 0) hookMpeg();
  hookVideocodec(mod->modname);
  if (logged < 24u) { /* which modules a movie brings, to know what to hook */
    ++logged;
    log("[gp-mpeg] module start %s at_ms=%u\n", mod->modname, (unsigned)(sceKernelGetSystemTimeLow() / 1000u));
  }
  return previous_start_handler ? previous_start_handler(mod) : 0;
}

static void followDump(void) {
  static u32 held;
  SceCtrlData pad;
  u32 combo;
  if (dump_frame) { /* the frame dumped is over */
    dump_frame = 0;
    writeDump();
    log("[gp-frame] end, %u lines\n", (unsigned)dump_lines);
  }
  {
    const int k1 = pspSdkSetK1(0); /* the pad is on this module's stack */
    const int got = sceCtrlPeekBufferPositive(&pad, 1);
    pspSdkSetK1(k1);
    if (got <= 0) return;
  }
  combo = (pad.Buttons & (PSP_CTRL_RTRIGGER | PSP_CTRL_SELECT)) == (PSP_CTRL_RTRIGGER | PSP_CTRL_SELECT);
  {
    /* (GP_LOG) the frames a screenshot is too slow for, dumped by themselves:
     * a system dialog opening or closing, a movie over -- the next frame and
     * two more three frames apart; 30 at most */
    static u32 was_dialog, was_movie, auto_wait, auto_more, auto_total;
    if ((dialog_open != 0u) != (was_dialog != 0u) || (was_movie && !movie_playing)) {
      auto_wait = 1; auto_more = 2;
      log("[gp-frame] auto: %s at_ms=%u\n", dialog_open != was_dialog ? (dialog_open ? "dialog opens" : "dialog closes") : "movie over",
          (unsigned)(sceKernelGetSystemTimeLow() / 1000u));
    }
    was_dialog = dialog_open; was_movie = movie_playing;
    if (auto_wait && !--auto_wait && auto_total < 30u) {
      ++auto_total;
      combo = 1; held = 0; /* (as R+SELECT just pressed) */
      if (auto_more) { --auto_more; auto_wait = 3; }
    }
  }
  if (combo && !held) {
    dump_buffer = (DumpLine *)DUMP_BUFFER;
    dump_frame = 1; dump_lines = 0;
    log("[gp-frame] begin at_ms=%u\n", (unsigned)(sceKernelGetSystemTimeLow() / 1000u));
  }
  held = combo;
}

static void followMode(void) {
  if (GP_LOG) followDump();
  {
    gp_stock = *(volatile u32 *)GP_MODE_ADDR == 0u;
    if (mpeg_frames_ago < 0xffffu) ++mpeg_frames_ago;
    if (frame_movie_draw && (frame_3d_draw || frame_draws > MOVIE_MAX_DRAWS)) {
      static u32 rejected;
      if (rejected < 40u) {
        ++rejected;
        log("[gp-movie] not a movie frame: pictures=%u 3d=%u draws=%u at_ms=%u\n", (unsigned)frame_movie_draw,
            (unsigned)frame_3d_draw, (unsigned)frame_draws, (unsigned)(sceKernelGetSystemTimeLow() / 1000u));
      }
    }
    if (frame_movie_draw && !frame_3d_draw && frame_draws <= MOVIE_MAX_DRAWS) movie_frames_ago = 0;
    else if (movie_frames_ago < 0xffffu) ++movie_frames_ago;
    {
      static u32 frames_since_3d = 0xffffu;
      if (frame_3d_draw) frames_since_3d = 0;
      else if (frames_since_3d < 0xffffu) ++frames_since_3d;
      frame_movie_draw = frame_3d_draw = frame_draws = 0;
      /* a save dialog's animated icon is a movie too: not while one is open;
       * nor while 3D is drawn -- Tekken keeps its decoder busy in a fight,
       * and the squeezed depth buffer left the fight black */
      movie_playing = !dialog_open && frames_since_3d >= 2u &&
                      (mpeg_frames_ago < MOVIE_LEAVE_FRAMES ||
                       (videocodec_open && mpeg_frames_ago < VIDEOCODEC_IDLE_FRAMES));
    }
    if (movie_playing != movie_depth) {
      movie_depth = movie_playing;
      mode_changed = 1;
      sendModeRegisters(); /* the depth registers, ahead of the next frame */
      log("[gp-mode1] movie depth %s at_ms=%u mpeg_ago=%u draw_ago=%u decodes=%u open=%u\n",
          movie_depth ? "squeezed" : "back", (unsigned)(sceKernelGetSystemTimeLow() / 1000u), (unsigned)mpeg_frames_ago,
          (unsigned)movie_frames_ago, (unsigned)mpeg_decodes, (unsigned)videocodec_open);
    }
  }
}

int sceDisplaySetFrameBufPatched(void *topaddr, int bufferwidth, int pixelformat, int sync) {
  HOOK_ENTERED(6);
  if (GP_LOG) noteEvent('F', (u32)sync);
  ++*(volatile u32 *)GP_FRAME_COUNT_ADDR; /* Adrenaline's FPS counter: the game's frames */
  reportPerformance();
  if (measure_only) return _sceDisplaySetFrameBuf(topaddr, bufferwidth, pixelformat, sync);
  restoreIfIdle();
  if (GP_LOG) noteFrameShown();
  if (!rendered_in_sync) {
    u32 copy_started = sceKernelGetSystemTimeLow();
#if GP_LOG
    /* the GE's time for the display copy alone: now and then, with the GE
     * idle, the copy waited for (the game's thread, a few ms in 5 s) */
    static unsigned copy_probe;
    if (++copy_probe >= 300u && _sceGeDrawSync(1) == 0) {
      copy_probe = 0;
      copyFrameBuffer();
      _sceGeDrawSync(0);
      log("[gp-copy] display copy %ux%u took %u us on the GE\n", (unsigned)WIDTH, (unsigned)HEIGHT,
          (unsigned)(sceKernelGetSystemTimeLow() - copy_started));
    } else
#endif
    if (vitaTakesFrames() && _sceGeDrawSync(1) == 0) presentDirect();
    else copyFrameBuffer();
    perf_copy_us += sceKernelGetSystemTimeLow() - copy_started;
  }
  followMode(); /* after the copy: the frame was drawn with the old depth buffer */
  vram_copied_count = 0; /* a new frame: its VRAM textures are copied again */
  vram_slot ^= 1u;        /* into the other place */
  rendered_in_sync = 0;
  framebuf_set_at = sceKernelGetSystemTimeLow();
  framebuf_seen = 1;
#if GP_TRACE
  ++trace_display_count;
  trace_state("display", trace_display_count, (unsigned)topaddr,
              (unsigned)bufferwidth, (unsigned)pixelformat, (unsigned)sync);
#endif
  return _sceDisplaySetFrameBuf(topaddr, bufferwidth, pixelformat, sync);
}

/* Pure CPU cost of the parser per command, on a synthetic list in the unused
 * pool: separates our code from memory and preemption in [gp-perf] parse_ms. */
static void benchmarkParser(void) {
  u32 *l = (u32 *)VERTICES_BUFFER;
  unsigned n = 0, i, k, started, elapsed;
  (void)elapsed; /* only logged */
  for (k = 0; n + 32u < 4096u; ++k) {
    l[n++] = (GE_CMD_VERTEXTYPE << 24) | 0x0001A3u;   /* 3D, not through */
    l[n++] = (GE_CMD_BASE << 24) | 0x080000u;
    l[n++] = (GE_CMD_VADDR << 24) | 0x800000u;
    l[n++] = (GE_CMD_TEXFUNC << 24) | 0x010100u;
    l[n++] = (GE_CMD_ALPHABLENDENABLE << 24) | 1u;
    l[n++] = (GE_CMD_BONEMATRIXNUMBER << 24) | 0u;
    for (i = 0; i < 12u; ++i) l[n++] = (GE_CMD_BONEMATRIXDATA << 24) | (i << 8);
    l[n++] = (GE_CMD_TEXADDR0 << 24) | 0x100000u;
    l[n++] = (GE_CMD_TEXBUFWIDTH0 << 24) | 0x080100u;
    l[n++] = (GE_CMD_PRIM << 24) | (3u << 16) | 3u;
  }
  l[n++] = (GE_CMD_FINISH << 24);
  l[n++] = (GE_CMD_END << 24);
  resetGeState();
  started = sceKernelGetSystemTimeLow();
  patchGeList(l, NULL);
  elapsed = sceKernelGetSystemTimeLow() - started;
  log("[gp-bench] commands=%u us=%u ns_per_command=%u\n", n, elapsed, elapsed * 1000u / n);
  resetGeState();
  game_dither_set = 0;
  perf_commands = 0;
}

int module_start(SceSize args, void *argp) {
  SceCtrlData pad;
  (void)args;
  (void)argp;
  sceCtrlPeekBufferPositive(&pad, 1);
  if (pad.Buttons & PSP_CTRL_LTRIGGER)
    return 0;
  gp_stock = *(volatile u32 *)GP_MODE_ADDR == 0u;
  *(volatile u32 *)(DISPLAY_FRONT | 0xA0000000u) = 0u; /* (the Vita shows the first buffer till a frame is copied) */
  /* held at boot: R measure only (SELECT opens the VSH menu; the d-pad moves
   * the menu the game is started from) */
  measure_only = (pad.Buttons & PSP_CTRL_RTRIGGER) != 0;
  legacy_cache = (pad.Buttons & PSP_CTRL_TRIANGLE) != 0;
  /* GePatch's otherwise unused range between the display copy and its own
   * render list: user RAM the GE reads, like that render list. */
  if (GP_LOG) benchmarkParser(); /* its result is only logged */
  /* The top 128 KiB of that range hold the tile records (CPU only). */
  memset(&vertex_restore, 0, sizeof(vertex_restore));
  memset((void *)UPPER_TABLES, 0, UPPER_TABLES_END - UPPER_TABLES);
  gp_inset_init(&inset, (GpInsetVertex *)VERTICES_BUFFER,
                (0x0A700000u - VERTICES_BUFFER) / sizeof(GpInsetVertex)); /* (to the straightened copy's pool) */
  gp_inset_set_tiles(&inset, (GpInsetTiles *)(RENDER_LIST - 0x20000));
  _sceGeEdramGetAddr = (void *)sctrlHENFindFunction("sceGE_Manager", "sceGe_driver", 0xE47E40E4);
  _sceGeEdramGetSize = (void *)sctrlHENFindFunction("sceGE_Manager", "sceGe_driver", 0x1F6752AD);
  _sceGeGetList = (void *)sctrlHENFindFunction("sceGE_Manager", "sceGe_driver", 0x67B01D8E);
  _sceGeListUpdateStallAddr = (void *)sctrlHENFindFunction("sceGE_Manager", "sceGe_driver", 0xE0D68148);
  _sceGeListEnQueue = (void *)sctrlHENFindFunction("sceGE_Manager", "sceGe_driver", 0xAB49E76A);
  _sceGeListEnQueueHead = (void *)sctrlHENFindFunction("sceGE_Manager", "sceGe_driver", 0x1C0D95A6);
  _sceDisplaySetFrameBuf = (void *)sctrlHENFindFunction("sceDisplay_Service", "sceDisplay_driver", 0x289D82FE);
  _sceGeDrawSync = (void *)sctrlHENFindFunction("sceGE_Manager", "sceGe_driver", 0xB287BD61);
  _sceGeListSync = (void *)sctrlHENFindFunction("sceGE_Manager", "sceGe_driver", 0x03444EB4);
  if (!_sceGeEdramGetAddr || !_sceGeEdramGetSize || !_sceGeGetList ||
      !_sceGeListUpdateStallAddr || !_sceGeListEnQueue || !_sceGeListEnQueueHead ||
      !_sceDisplaySetFrameBuf || !_sceGeDrawSync || !_sceGeListSync) {
    log("[gp-sync2] required export missing; no hooks installed\n");
    return -1;
  }
  if (measure_only) {
    sctrlHENPatchSyscall((void *)_sceDisplaySetFrameBuf, sceDisplaySetFrameBufPatched);
    sctrlHENPatchSyscall((void *)_sceGeDrawSync, sceGeDrawSyncPatched);
    sctrlHENPatchSyscall((void *)_sceGeListSync, sceGeListSyncPatched); /* (only counted) */
    sceKernelDcacheWritebackInvalidateAll();
    sceKernelIcacheClearAll();
    log("[gp-mode1] loaded measure-only: nothing patched\n");
    return 0;
  }
  sctrlHENPatchSyscall((void *)_sceGeEdramGetAddr, sceGeEdramGetAddrPatched);
  sctrlHENPatchSyscall((void *)_sceGeEdramGetSize, sceGeEdramGetSizePatched);
  sctrlHENPatchSyscall((void *)_sceGeListUpdateStallAddr, sceGeListUpdateStallAddrPatched);
  sctrlHENPatchSyscall((void *)_sceGeListEnQueue, sceGeListEnQueuePatched);
  sctrlHENPatchSyscall((void *)_sceGeListEnQueueHead, sceGeListEnQueueHeadPatched);
  sctrlHENPatchSyscall((void *)_sceDisplaySetFrameBuf, sceDisplaySetFrameBufPatched);
  sctrlHENPatchSyscall((void *)_sceGeDrawSync, sceGeDrawSyncPatched);
  sctrlHENPatchSyscall((void *)_sceGeListSync, sceGeListSyncPatched);
  _sceDisplayWaitVblankStartCB = (void *)sctrlHENFindFunction("sceDisplay_Service", "sceDisplay_driver", 0x46F186C3);
  _sceDisplayWaitVblankStart = (void *)sctrlHENFindFunction("sceDisplay_Service", "sceDisplay_driver", 0x984C27E7);
  if (_sceDisplayWaitVblankStartCB) sctrlHENPatchSyscall((void *)_sceDisplayWaitVblankStartCB, sceDisplayWaitVblankStartCBPatched);
  if (_sceDisplayWaitVblankStart) sctrlHENPatchSyscall((void *)_sceDisplayWaitVblankStart, sceDisplayWaitVblankStartPatched);
  previous_start_handler = sctrlHENSetStartModuleHandler(onModuleStart);
  if (GP_LOG) startLogThread(); /* lines from here on are written off the game's thread */
#if GP_LOG
  {
    /* PSPs from the Slim on have 4 MiB of eDRAM, 2 MiB of it on by default
     * (uOFW ge.c: sceGeEdramSetSize(0x400000) when the Tachyon is newer than
     * 0x4FFFFF). At 960x544 our draw and depth buffers fill 2 MiB, and the
     * game's VRAM is RAM (FAKE_VRAM): its textures read from there, four times
     * the pixels. Is the rest there on the Vita? Asked, tried, put back. */
    int (*get_hw)(void) = (void *)sctrlHENFindFunction("sceGE_Manager", "sceGe_driver", 0x547EC5F0);
    int (*get_size)(void) = (void *)sctrlHENFindFunction("sceGE_Manager", "sceGe_driver", 0x5D28A52B);
    int (*set_size)(int) = (void *)sctrlHENFindFunction("sceGE_Manager", "sceGe_driver", 0xD8633888);
    int hw = get_hw ? get_hw() : -1, before = get_size ? get_size() : -1, set = -1, after = -1;
    u32 high = 0, low = 0, high_back = 0;
    if (set_size && hw == 0x400000) {
      volatile u32 *const lo = (volatile u32 *)0x441FFFF0u, *const hi = (volatile u32 *)0x443FFFF0u; /* uncached */
      set = set_size(0x400000);
      after = get_size ? get_size() : -1;
      if (set == 0) {
        const u32 keep_lo = *lo;
        *hi = 0x5A5A1234u;
        *lo = 0xA5A5C0DEu;
        high = *hi; /* 0x5A5A1234 if the upper 2 MiB are memory of their own */
        low = *lo;
        *hi = 0x12345678u;
        high_back = *hi;
        *lo = keep_lo;
        set_size(0x200000); /* (back, as GePatch has it for now) */
      }
    }
    log("[gp-edram] hw_size=%08x size=%08x set_4mb=%d size_then=%08x upper=%08x lower=%08x upper_again=%08x\n",
        (unsigned)hw, (unsigned)before, set, (unsigned)after, (unsigned)high, (unsigned)low, (unsigned)high_back);
  }
#endif
  if (sctrlHENFindFunction("sceMpeg_library", "sceMpeg", 0xD8C5F121)) hookMpeg(); /* loaded already */
  hookVideocodec(NULL);
  _sceUtilitySavedataInitStart = (void *)sctrlHENFindFunction("sceUtility_Driver", "sceUtility", 0x50C4CD57);
  _sceUtilitySavedataShutdownStart = (void *)sctrlHENFindFunction("sceUtility_Driver", "sceUtility", 0x9790B33C);
  _sceUtilityMsgDialogInitStart = (void *)sctrlHENFindFunction("sceUtility_Driver", "sceUtility", 0x2AD8E239);
  _sceUtilityMsgDialogShutdownStart = (void *)sctrlHENFindFunction("sceUtility_Driver", "sceUtility", 0x67AF3428);
  if (_sceUtilitySavedataInitStart && _sceUtilitySavedataShutdownStart) {
    sctrlHENPatchSyscall((void *)_sceUtilitySavedataInitStart, sceUtilitySavedataInitStartPatched);
    sctrlHENPatchSyscall((void *)_sceUtilitySavedataShutdownStart, sceUtilitySavedataShutdownStartPatched);
  }
  if (_sceUtilityMsgDialogInitStart && _sceUtilityMsgDialogShutdownStart) {
    sctrlHENPatchSyscall((void *)_sceUtilityMsgDialogInitStart, sceUtilityMsgDialogInitStartPatched);
    sctrlHENPatchSyscall((void *)_sceUtilityMsgDialogShutdownStart, sceUtilityMsgDialogShutdownStartPatched);
  }
  log("[gp-mode1] dialog hooks savedata=%u message=%u\n",
      (unsigned)(_sceUtilitySavedataInitStart && _sceUtilitySavedataShutdownStart),
      (unsigned)(_sceUtilityMsgDialogInitStart && _sceUtilityMsgDialogShutdownStart));

  sceKernelDcacheWritebackInvalidateAll();
  sceKernelIcacheClearAll();
  log("[gp-mode1] loaded trace=%u vertices3=1 uv-inset=1 stencil-format=%u perf=1 pool=%08x\n", (unsigned)GP_TRACE,
      (unsigned)!force_565, (unsigned)VERTICES_BUFFER);
  return 0;
}
