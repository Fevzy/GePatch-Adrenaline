/*
  GE renderer (see gerender.h).

  Vertices are decoded and transformed on the CPU (world, view, projection,
  bone skinning, viewport) into the Vita's clip space, so the GPU only needs
  one vertex program; fragment programs, one per texture function / alpha
  test combination (gershaders.h), come precompiled from the shader pack and
  are patched per blend mode. Frames are built into one of three arenas and the draw
  thread shows the newest complete one.

  Not yet: lighting, fog, render-to-texture (draws into other framebuffers
  and textures read from VRAM are skipped), lines and points, splines,
  morphing, texture projection/environment mapping, DXT textures.
*/

#include <psp2/gxm.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/dmac.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vita2d.h>

#include "main.h"
#include "gerender.h"
#include "gershaders.h"

extern SceGxmContext *_vita2d_context;
/* libvita2d's GPU memory helpers */
void *gpu_alloc(SceKernelMemBlockType type, unsigned int size, unsigned int alignment, unsigned int attribs, SceUID *uid);
void *vertex_usse_alloc(unsigned int size, SceUID *uid, unsigned int *usse_offset);
void *fragment_usse_alloc(unsigned int size, SceUID *uid, unsigned int *usse_offset);
/* Adrenaline's dlmalloc spaces (utils.c) */
void *mspace_create(void *base, size_t size);
void *mspace_malloc(void *mspace, size_t size);
void mspace_free(void *mspace, void *ptr);

#define GR_ARENAS 8
/* Memory comes in 1 MiB steps here: two blocks, carved up. */
#define GR_GPU_BLOCK (8 * 1024 * 1024)
#define GR_CPU_BLOCK (4 * 1024 * 1024)
#define GR_ARENA_BYTES (384 * 1024)
#define GR_ARENA_VERTS (GR_ARENA_BYTES / sizeof(GrVertex))
#define GR_ARENA_DRAWS 2048
#define GR_INDEX_BYTES (65536 * 2)
#define GR_TEXMEM_SIZE (GR_GPU_BLOCK - GR_ARENAS * GR_ARENA_BYTES - GR_INDEX_BYTES)
#define GR_TEXTURES 1024
#define GR_TEMP_SIZE (1024 * 1024)
#define GR_FRAGMENT_VARIANTS 1024
#define GR_FRAGMENT_PROGRAMS 512
#define GR_DRAWS_BYTES (GR_ARENAS * GR_ARENA_DRAWS * sizeof(GrDraw))
#define GR_HEAP_SIZE ((GR_CPU_BLOCK - GR_TEMP_SIZE - GR_DRAWS_BYTES) & ~0xFFFu) /* compiler, patcher, programs */

/* GE commands the renderer reads */
enum {
	CMD_VADDR = 0x01, CMD_IADDR = 0x02, CMD_PRIM = 0x04, CMD_BASE = 0x10, CMD_VERTEXTYPE = 0x12,
	CMD_TEXTUREMAPENABLE = 0x1E, CMD_ALPHABLENDENABLE = 0x21, CMD_ALPHATESTENABLE = 0x22,
	CMD_ZTESTENABLE = 0x23, CMD_STENCILTESTENABLE = 0x24,
	CMD_BONEMATRIXNUMBER = 0x2A, CMD_BONEMATRIXDATA = 0x2B,
	CMD_WORLDMATRIXNUMBER = 0x3A, CMD_WORLDMATRIXDATA = 0x3B,
	CMD_VIEWMATRIXNUMBER = 0x3C, CMD_VIEWMATRIXDATA = 0x3D,
	CMD_PROJMATRIXNUMBER = 0x3E, CMD_PROJMATRIXDATA = 0x3F,
	CMD_VIEWPORTXSCALE = 0x42, CMD_VIEWPORTYSCALE = 0x43, CMD_VIEWPORTZSCALE = 0x44,
	CMD_VIEWPORTXCENTER = 0x45, CMD_VIEWPORTYCENTER = 0x46, CMD_VIEWPORTZCENTER = 0x47,
	CMD_TEXSCALEU = 0x48, CMD_TEXSCALEV = 0x49, CMD_TEXOFFSETU = 0x4A, CMD_TEXOFFSETV = 0x4B,
	CMD_OFFSETX = 0x4C, CMD_OFFSETY = 0x4D,
	CMD_MATERIALAMBIENT = 0x55, CMD_MATERIALALPHA = 0x58,
	CMD_FRAMEBUFPTR = 0x9C, CMD_FRAMEBUFWIDTH = 0x9D,
	CMD_TEXADDR0 = 0xA0, CMD_TEXBUFWIDTH0 = 0xA8, CMD_CLUTADDR = 0xB0, CMD_CLUTADDRUPPER = 0xB1,
	CMD_TEXSIZE0 = 0xB8, CMD_TEXMAPMODE = 0xC0, CMD_TEXMODE = 0xC2, CMD_TEXFORMAT = 0xC3,
	CMD_LOADCLUT = 0xC4, CMD_CLUTFORMAT = 0xC5, CMD_TEXFILTER = 0xC6, CMD_TEXWRAP = 0xC7,
	CMD_TEXFUNC = 0xC9, CMD_TEXENVCOLOR = 0xCA, CMD_CLEARMODE = 0xD3,
	CMD_SCISSOR1 = 0xD4, CMD_SCISSOR2 = 0xD5,
	CMD_ALPHATEST = 0xDB, CMD_STENCILTEST = 0xDC, CMD_STENCILOP = 0xDD, CMD_ZTEST = 0xDE,
	CMD_BLENDMODE = 0xDF, CMD_BLENDFIXEDA = 0xE0, CMD_BLENDFIXEDB = 0xE1,
	CMD_ZWRITEDISABLE = 0xE7, CMD_MASKRGB = 0xE8, CMD_MASKALPHA = 0xE9,
};

typedef struct {
	float x, y, z, w, u, v;
	uint32_t color;
	float fog; /* 1 = no fog */
} GrVertex;

typedef struct {
	uint32_t first, count, fb;
	uint16_t variant;
	uint32_t depth_func, stencil_func, stencil_fail, stencil_zfail, stencil_zpass; /* GXM enums */
	uint8_t textured, depth_write, stencil, stencil_ref, stencil_mask, stencil_wmask, scissor;
	uint16_t sc_x0, sc_y0, sc_x1, sc_y1;
	uint32_t blend; /* packed SceGxmBlendInfo fields, 0 = no blending, colour mask in the top byte */
	float env[4], fog[4];
	float aref;
	SceGxmTexture tex;
} GrDraw;

/* BUILDING collects the draws into one framebuffer; CLOSED when the game
   moves on to another one (or shows this one); READY when the game shows it. */
enum { ARENA_FREE, ARENA_BUILDING, ARENA_CLOSED, ARENA_READY, ARENA_SHOWING, ARENA_RETIRED };

typedef struct {
	GrVertex *verts;
	GrDraw *draws;
	uint32_t nverts, ndraws, topaddr, retired_at, fb, seq;
	volatile int state;
} GrArena;

typedef struct {
	uint32_t addr, bufw, size, format, clut_hash, check, frame, used_seq;
	void *data;
	SceGxmTexture tex;
} GrTexture;

typedef struct {
	SceGxmShaderPatcherId id;
	const SceGxmProgram *gxp;
	const SceGxmProgramParameter *env, *aref, *fog;
	int state; /* 0 not tried, 1 ready, -1 failed */
} GrVariant;

typedef struct {
	uint16_t variant;
	uint32_t blend;
	SceGxmFragmentProgram *program;
} GrFragmentProgram;

typedef struct {
	uint32_t frames, dropped, dropped_draws, draws, verts, skipped_fb, skipped_vram, skipped_prim, skipped_read;
	uint32_t tex_decodes, tex_hits, tex_unsupported, shaders, shader_failures, presents, build_us;
} GrStats;

static int gr_ready;
static GrArena arenas[GR_ARENAS];
static GrArena *building, *showing;
static uint32_t arena_seq;
/* the framebuffers the game shows: draws into others are render-to-texture */
static uint32_t display_fbs[4], display_count;
static volatile uint32_t shown_fb = 0xFFFFFFFF; /* the framebuffer the game shows */
static SceUID arena_lock;
static uint32_t present_count;
static GrStats stats;

/* GE state, as the walker sees it in execution order */
static uint32_t ge[256];
static float world[12], view[12], proj[16], bones[8 * 12];
static uint32_t world_i, view_i, proj_i, bone_i;
static uint32_t clut_bytes, clut_hash;
static uint8_t clut[1024];
static uint32_t frame_no;
static uint32_t tex_epoch; /* textures checked again: each frame and each TEXFLUSH */
static uint32_t vertex_addr, index_addr; /* resolved VADDR and IADDR */

/* The first reads in a report window that failed: what, address, size, vertex type. */
static uint32_t read_fails[4][4], read_fail_count;
static void readFailed(uint32_t what, uint32_t addr, uint32_t size) {
	stats.skipped_read++;
	if (read_fail_count < 4) {
		read_fails[read_fail_count][0] = what;
		read_fails[read_fail_count][1] = addr;
		read_fails[read_fail_count][2] = size;
		read_fails[read_fail_count][3] = ge[CMD_VERTEXTYPE];
		read_fail_count++;
	}
}

static GrTexture textures[GR_TEXTURES];
static uint32_t texture_count;
static uint8_t *texmem;
static uint32_t texmem_used;
static uint8_t *temp; /* PSP data being decoded */
static uint16_t *indices;

static SceGxmShaderPatcher *patcher;
static SceGxmShaderPatcherId vertex_id;
static const SceGxmProgram *vertex_gxp;
static SceGxmVertexProgram *vertex_program;
static GrVariant variants[GR_FRAGMENT_VARIANTS];
static GrFragmentProgram fragment_programs[GR_FRAGMENT_PROGRAMS];
static uint32_t fragment_program_count;
static void *heap;

static void *grAlloc(size_t size) { return heap ? mspace_malloc(heap, size) : NULL; }
static void grFree(void *p) { if (heap && p) mspace_free(heap, p); }

/* Every block the renderer holds, given back if it cannot start: ScePspemu
   needs its memory to run the game. */
static SceUID blocks[16];
static uint32_t block_count;

static void *grTrack(SceUID uid) {
	void *base = NULL;
	if (uid < 0)
		return NULL;
	if (sceKernelGetMemBlockBase(uid, &base) < 0 || block_count == 16) {
		sceKernelFreeMemBlock(uid);
		return NULL;
	}
	blocks[block_count++] = uid;
	return base;
}

/* CPU memory of our own, outside every heap. */
static void *grBlock(const char *name, uint32_t size) {
	return grTrack(sceKernelAllocMemBlock(name, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, (size + 0xFFF) & ~0xFFFu, NULL));
}

/* GPU memory in main memory: CDRAM belongs to vita2d (Adrenaline's screen
   and menu) and ScePspemu, taking it makes vita2d fail to start. */
static void *grGpu(const char *name, uint32_t size, uint32_t attribs) {
	uint32_t size4k = (size + 0xFFF) & ~0xFFFu;
	void *base = grTrack(sceKernelAllocMemBlock(name, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, size4k, NULL));
	if (base && sceGxmMapMemory(base, size4k, attribs) < 0) {
		sceKernelFreeMemBlock(blocks[--block_count]);
		return NULL;
	}
	return base;
}

static void grFreeAll(void) {
	while (block_count) {
		SceUID uid = blocks[--block_count];
		void *base = NULL;
		if (sceKernelGetMemBlockBase(uid, &base) >= 0)
			sceGxmUnmapMemory(base); /* fails harmlessly for CPU blocks */
		sceKernelFreeMemBlock(uid);
	}
	heap = NULL;
}

static void grLogMemory(const char *when) {
	SceKernelFreeMemorySizeInfo info;
	info.size = sizeof(info);
	if (sceKernelGetFreeMemorySize(&info) >= 0)
		sceClibPrintf("[gr] free memory %s: user=%u KiB cdram=%u KiB phycont=%u KiB\n", when,
			info.size_user >> 10, info.size_cdram >> 10, info.size_phycont >> 10);
}

static inline float f24(uint32_t data) {
	union { uint32_t i; float f; } u;
	u.i = data << 8;
	return u.f;
}

static inline uint32_t geAddr(uint32_t low, uint32_t high_bits) {
	return ((low & 0xFFFFFF) | high_bits) & 0x0FFFFFFF;
}

/* --------------------------------------------------------------- arenas */

static GrArena *arenaTake(void) {
	GrArena *a = NULL;
	sceKernelLockMutex(arena_lock, 1, NULL);
	for (int i = 0; i < GR_ARENAS && !a; i++)
		if (arenas[i].state == ARENA_FREE)
			a = &arenas[i];
	if (a) {
		a->state = ARENA_BUILDING;
		a->nverts = a->ndraws = 0;
	}
	sceKernelUnlockMutex(arena_lock, 1);
	return a;
}

/* A free arena, waiting for the draw thread up to 50 ms rather than dropping
   draws: the tap thread runs below the game anyway. */
static GrArena *arenaTakeWait(void) {
	for (int i = 0; i < 50; i++) {
		GrArena *a = arenaTake();
		if (a)
			return a;
		sceKernelDelayThread(1000);
	}
	return NULL;
}

/* Under arena_lock. */
static void arenaClose(GrArena *a) {
	if (a->ndraws) {
		a->state = ARENA_CLOSED;
		a->seq = ++arena_seq;
	} else {
		a->state = ARENA_FREE;
	}
}

static int isDisplay(uint32_t fb) {
	if (!display_count) /* nothing shown yet: take everything */
		return 1;
	for (uint32_t i = 0; i < display_count; i++)
		if (display_fbs[i] == fb)
			return 1;
	return 0;
}

/* Draws go to the framebuffer fb: a frame of another display framebuffer
   ends the one being collected. 0 when this draw cannot be kept. */
static int drawTarget(uint32_t fb) {
	if (!isDisplay(fb)) {
		stats.skipped_fb++;
		return 0;
	}
	if (!building)
		building = arenaTakeWait();
	if (!building) {
		stats.dropped_draws++;
		return 0;
	}
	if (!building->ndraws)
		building->fb = fb;
	return 1;
}

void grEndFrame(uint32_t topaddr) {
	uint32_t fb = topaddr & 0x1FFFF0;
	if (!gr_ready)
		return;
	frame_no++;
	tex_epoch++;
	if (!isDisplay(fb) || !display_count) {
		if (display_count < 4)
			display_fbs[display_count++] = fb;
	}
	int ready = 0;
	sceKernelLockMutex(arena_lock, 1, NULL);
	if (building && building->ndraws) { /* the game finished a frame */
		arenaClose(building);
		building = NULL;
	}
	/* everything drawn up to this flip, in order: never half a frame */
	for (int i = 0; i < GR_ARENAS; i++) {
		GrArena *a = &arenas[i];
		if (a->state != ARENA_CLOSED)
			continue;
		a->topaddr = fb;
		a->state = ARENA_READY;
		ready++;
		stats.draws += a->ndraws;
		stats.verts += a->nverts;
	}
	shown_fb = fb;
	if (ready)
		stats.frames++;
	else
		stats.dropped++;
	sceKernelUnlockMutex(arena_lock, 1);
	if (!building)
		building = arenaTake();
}

/* ------------------------------------------------------------- textures */

static inline uint32_t color16(uint32_t c, uint32_t format) {
	uint32_t r, g, b, a;
	switch (format) {
		case 0: /* 5650 */
			r = c & 0x1F; g = (c >> 5) & 0x3F; b = (c >> 11) & 0x1F;
			r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2); a = 0xFF;
			break;
		case 1: /* 5551 */
			r = c & 0x1F; g = (c >> 5) & 0x1F; b = (c >> 10) & 0x1F;
			r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2); a = (c & 0x8000) ? 0xFF : 0;
			break;
		default: /* 4444 */
			r = (c & 0xF) * 17; g = ((c >> 4) & 0xF) * 17; b = ((c >> 8) & 0xF) * 17; a = ((c >> 12) & 0xF) * 17;
			break;
	}
	return r | (g << 8) | (b << 16) | (a << 24);
}

static uint32_t hashBytes(const uint8_t *p, uint32_t n) {
	uint32_t h = 2166136261u;
	for (uint32_t i = 0; i < n; i++)
		h = (h ^ p[i]) * 16777619u;
	return h;
}

/* What tells a texture changed: a hash of all of it when it is small (text the
   game draws into a texture every frame), of 64 words spread over it otherwise. */
#define GR_TEXTURE_FULL_CHECK 0x40000
static uint32_t textureCheck(uint32_t addr, uint32_t bytes) {
	static uint32_t words[64];
	if (bytes <= GR_TEXTURE_FULL_CHECK && tapReadBlock(addr, temp + GR_TEMP_SIZE - GR_TEXTURE_FULL_CHECK, bytes))
		return hashBytes(temp + GR_TEMP_SIZE - GR_TEXTURE_FULL_CHECK, bytes);
	for (int i = 0; i < 64; i++) {
		words[i] = 0;
		tapReadBlock(addr + ((bytes / 64) * i & ~3u), &words[i], 4);
	}
	return hashBytes((const uint8_t *)words, sizeof(words));
}

/* Whether a frame not drawn to the screen yet draws with this texture: the
   draw being built now, or an arena waiting for the draw thread. */
static int textureInUse(const GrTexture *t) {
	uint32_t oldest = arena_seq + 1; /* the arena being built */
	for (int i = 0; i < GR_ARENAS; i++) {
		int s = arenas[i].state;
		if ((s == ARENA_CLOSED || s == ARENA_READY || s == ARENA_SHOWING || s == ARENA_RETIRED) && arenas[i].seq < oldest)
			oldest = arenas[i].seq;
	}
	return t->used_seq && t->used_seq >= oldest;
}

/* The current texture as a Vita texture, decoded once and checked against
   its PSP memory once per frame. NULL when it cannot be shown. */
static SceGxmTexture *textureCurrent(void) {
	uint32_t addr = geAddr(ge[CMD_TEXADDR0], (ge[CMD_TEXBUFWIDTH0] & 0x0F0000) << 8);
	uint32_t bufw = ge[CMD_TEXBUFWIDTH0] & 0x7FF;
	uint32_t format = ge[CMD_TEXFORMAT] & 0xF, swizzle = ge[CMD_TEXMODE] & 1;
	uint32_t w = 1u << (ge[CMD_TEXSIZE0] & 0xF), h = 1u << ((ge[CMD_TEXSIZE0] >> 8) & 0xF);
	uint32_t clut_format = ge[CMD_CLUTFORMAT] & 0x1FFFFF;
	static const uint8_t bits[8] = { 16, 16, 16, 32, 4, 8, 16, 32 };
	if (format > 7) {
		stats.tex_unsupported++;
		return NULL;
	}
	if (addr < 0x08000000)
		stats.skipped_vram++; /* counted: read through forwarded pages */
	if (w > 512) w = 512;
	if (h > 512) h = 512;
	if (bufw < w) bufw = w;
	uint32_t row = bufw * bits[format] / 8, rows = swizzle ? (h + 7) & ~7u : h, bytes = row * rows;
	if (swizzle)
		row = (row + 15) & ~15u, bytes = row * rows;
	if (bytes > GR_TEMP_SIZE)
		return NULL;
	uint32_t key_format = format | (swizzle << 4) | (format >= 4 ? clut_format << 5 : 0);
	uint32_t key_clut = format >= 4 ? clut_hash : 0, size = w | (h << 16);

	GrTexture *t = NULL;
	for (uint32_t i = 0; i < texture_count; i++) {
		GrTexture *c = &textures[i];
		if (c->addr == addr && c->bufw == bufw && c->size == size && c->format == key_format && c->clut_hash == key_clut) {
			t = c;
			break;
		}
	}
	if (t) {
		if (t->frame == tex_epoch) {
			stats.tex_hits++;
			t->used_seq = arena_seq + 1;
			return &t->tex;
		}
		uint32_t check = textureCheck(addr, bytes);
		t->frame = tex_epoch;
		if (check == t->check) {
			stats.tex_hits++;
			t->used_seq = arena_seq + 1;
			return &t->tex;
		}
		t->check = check;
		uint32_t stride = (w + 7) & ~7u, need = stride * h * 4;
		if (!textureInUse(t)) {
			/* rewritten in place below */
		} else if (texmem_used + need > GR_TEXMEM_SIZE) { /* full: start over, this one first */
			textures[0] = *t;
			t = &textures[0];
			texture_count = 1;
			texmem_used = 0;
		}
		if (textureInUse(t)) {
			t->data = texmem + texmem_used;
			texmem_used += (need + 255) & ~255u;
			sceGxmTextureInitLinear(&t->tex, t->data, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, w, h, 0);
		}
	} else {
		uint32_t stride = (w + 7) & ~7u, need = stride * h * 4;
		if (texture_count == GR_TEXTURES || texmem_used + need > GR_TEXMEM_SIZE) /* full: start over */
			texture_count = texmem_used = 0;
		t = &textures[texture_count++];
		t->addr = addr; t->bufw = bufw; t->size = size; t->format = key_format; t->clut_hash = key_clut;
		t->data = texmem + texmem_used;
		texmem_used += (need + 255) & ~255u;
		t->frame = tex_epoch;
		t->check = textureCheck(addr, bytes);
		sceGxmTextureInitLinear(&t->tex, t->data, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, w, h, 0);
	}
	if (!tapReadBlock(addr, temp, bytes)) {
		readFailed(1, addr, bytes);
		return NULL;
	}
	t->used_seq = arena_seq + 1; /* the arena being built closes with this seq */
	stats.tex_decodes++;
	uint32_t stride = (w + 7) & ~7u, *out = (uint32_t *)t->data;
	uint32_t clut_shift = (clut_format >> 2) & 0x1F, clut_mask = (clut_format >> 8) & 0xFF;
	uint32_t clut_base = ((clut_format >> 16) & 0x1F) << 4, clut32 = (clut_format & 3) == 3;
	for (uint32_t y = 0; y < h; y++) {
		for (uint32_t x = 0; x < w; x++) {
			uint32_t bit = x * bits[format], byte = bit / 8, at;
			if (swizzle) {
				uint32_t block = (y / 8) * (row / 16) + byte / 16;
				at = block * 128 + (y % 8) * 16 + byte % 16;
			} else {
				at = y * row + byte;
			}
			uint32_t c;
			switch (format) {
				case 3: c = *(uint32_t *)(temp + at); break;
				case 4: case 5: case 6: case 7: {
					uint32_t index = format == 4 ? (temp[at] >> (bit & 4)) & 0xF :
						format == 5 ? temp[at] : format == 6 ? *(uint16_t *)(temp + at) : *(uint32_t *)(temp + at);
					index = ((index >> clut_shift) & clut_mask) | clut_base;
					if (clut32)
						c = index * 4 + 4 <= sizeof(clut) ? *(uint32_t *)(clut + index * 4) : 0;
					else
						c = index * 2 + 2 <= sizeof(clut) ? color16(*(uint16_t *)(clut + index * 2), clut_format & 3) : 0;
					break;
				}
				default: c = color16(*(uint16_t *)(temp + at), format); break;
			}
			out[y * stride + x] = c;
		}
	}
	return &t->tex;
}

/* ------------------------------------------------------------- vertices */

typedef struct {
	uint32_t stride, through;
	uint32_t weight_type, weights, weight_off;
	uint32_t tc_type, tc_off, col_type, col_off, nrm_type, nrm_off, pos_type, pos_off;
} GrFormat;

static void vertexFormat(uint32_t vt, GrFormat *f) {
	static const uint8_t wsize[4] = { 0, 1, 2, 4 }, tsize[4] = { 0, 1, 2, 4 }, csize[8] = { 0, 0, 0, 0, 2, 2, 2, 4 };
	static const uint8_t nsize[4] = { 0, 1, 2, 4 }, psize[4] = { 0, 1, 2, 4 };
	uint32_t off = 0, align = 1, size;
	memset(f, 0, sizeof(*f));
	f->through = (vt >> 23) & 1;
	f->weight_type = (vt >> 9) & 3;
	f->weights = ((vt >> 14) & 7) + 1;
	f->tc_type = vt & 3;
	f->col_type = (vt >> 2) & 7;
	f->pos_type = (vt >> 7) & 3;
	f->nrm_type = (vt >> 5) & 3;
#define FIELD(sz, count, offvar) \
	size = (sz); \
	if (size) { off = (off + size - 1) & ~(size - 1); offvar = off; off += size * (count); if (size > align) align = size; }
	FIELD(wsize[f->weight_type], f->weights, f->weight_off);
	FIELD(tsize[f->tc_type], 2, f->tc_off);
	FIELD(csize[f->col_type], 1, f->col_off);
	FIELD(nsize[f->nrm_type], 3, f->nrm_off);
	FIELD(psize[f->pos_type], 3, f->pos_off);
#undef FIELD
	f->stride = (off + align - 1) & ~(align - 1);
}

static inline float component(const uint8_t *p, uint32_t type, int i, float s8, float s16, int is_unsigned) {
	switch (type) {
		case 1: return is_unsigned ? ((const uint8_t *)p)[i] * s8 : ((const int8_t *)p)[i] * s8;
		case 2: return is_unsigned ? ((const uint16_t *)p)[i] * s16 : ((const int16_t *)p)[i] * s16;
		case 3: return ((const float *)p)[i];
	}
	return 0.0f;
}

static inline void mul43(const float *m, const float *in, float *out) {
	out[0] = in[0] * m[0] + in[1] * m[3] + in[2] * m[6] + m[9];
	out[1] = in[0] * m[1] + in[1] * m[4] + in[2] * m[7] + m[10];
	out[2] = in[0] * m[2] + in[1] * m[5] + in[2] * m[8] + m[11];
}

static uint32_t vertexColor(const uint8_t *v, const GrFormat *f) {
	if (!f->col_type)
		return (ge[CMD_MATERIALAMBIENT] & 0xFFFFFF) | ((ge[CMD_MATERIALALPHA] & 0xFF) << 24);
	if (f->col_type == 7)
		return *(const uint32_t *)(v + f->col_off);
	return color16(*(const uint16_t *)(v + f->col_off), f->col_type - 4);
}

static inline void mul33(const float *m, const float *in, float *out) {
	out[0] = in[0] * m[0] + in[1] * m[3] + in[2] * m[6];
	out[1] = in[0] * m[1] + in[1] * m[4] + in[2] * m[7];
	out[2] = in[0] * m[2] + in[1] * m[5] + in[2] * m[8];
}

static inline float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static inline float sqrt1(float x) { return __builtin_sqrtf(x); }

static inline void normalize3(float *v) {
	float l = dot3(v, v);
	if (l > 0.0f) {
		l = 1.0f / sqrt1(l);
		v[0] *= l; v[1] *= l; v[2] *= l;
	}
}

static inline void rgb(uint32_t c, float *out) {
	out[0] = (c & 0xFF) / 255.0f; out[1] = ((c >> 8) & 0xFF) / 255.0f; out[2] = ((c >> 16) & 0xFF) / 255.0f;
}

/* x^y for lighting exponents: log2 from the atanh series, exp2 from Taylor's,
   both to about 1e-5. */
static inline float powf_safe(float x, float y) {
	union { float f; uint32_t i; } u;
	if (x <= 0.0f)
		return 0.0f;
	u.f = x;
	float e = (float)(int)((u.i >> 23) & 0xFF) - 127.0f;
	u.i = (u.i & 0x007FFFFF) | 0x3F800000;
	float m = u.f; /* 1..2 */
	float t = (m - 1.0f) / (m + 1.0f), t2 = t * t;
	float l2 = e + 2.8853901f * t * (1.0f + t2 * (1.0f / 3.0f + t2 * (1.0f / 5.0f + t2 * (1.0f / 7.0f))));
	float p = y * l2;
	if (p < -126.0f)
		return 0.0f;
	if (p > 126.0f)
		p = 126.0f;
	int w = (int)p;
	if ((float)w > p)
		w--;
	float z = (p - (float)w) * 0.6931472f; /* 0..ln 2 */
	float f = 1.0f + z * (1.0f + z * (0.5f + z * (1.0f / 6.0f + z * (1.0f / 24.0f + z * (1.0f / 120.0f + z * (1.0f / 720.0f))))));
	u.i = (uint32_t)(w + 127) << 23;
	return u.f * f;
}

enum { CMD_LIGHTINGENABLE = 0x17, CMD_LIGHTENABLE0 = 0x18, CMD_MATERIALUPDATE = 0x53, CMD_MATERIALEMISSIVE = 0x54,
	CMD_MATERIALDIFFUSE = 0x56, CMD_MATERIALSPECULAR = 0x57, CMD_MATERIALSPECULARCOEF = 0x5B, CMD_AMBIENTCOLOR = 0x5C,
	CMD_AMBIENTALPHA = 0x5D, CMD_LIGHTTYPE0 = 0x5F, CMD_LX0 = 0x63, CMD_LDX0 = 0x6F, CMD_LKA0 = 0x7B, CMD_LKS0 = 0x87,
	CMD_LKO0 = 0x8B, CMD_LAC0 = 0x8F, CMD_FOGENABLE = 0x1F, CMD_FOG1 = 0xCD, CMD_FOG2 = 0xCE, CMD_FOGCOLOR = 0xCF };

/* The GE's lighting, in world space: up to four directional, point or spot
   lights with ambient, diffuse and specular terms, the viewer along +z. */
static uint32_t lightVertex(const float *p, const float *n_in, uint32_t vcolor, int has_color) {
	uint32_t update = has_color ? ge[CMD_MATERIALUPDATE] & 7 : 0;
	float mat_ambient[3], mat_diffuse[3], mat_specular[3], emissive[3], ambient[3], n[3] = { n_in[0], n_in[1], n_in[2] };
	rgb((update & 1) ? vcolor : ge[CMD_MATERIALAMBIENT], mat_ambient);
	rgb((update & 2) ? vcolor : ge[CMD_MATERIALDIFFUSE], mat_diffuse);
	rgb((update & 4) ? vcolor : ge[CMD_MATERIALSPECULAR], mat_specular);
	rgb(ge[CMD_MATERIALEMISSIVE], emissive);
	rgb(ge[CMD_AMBIENTCOLOR], ambient);
	float alpha = ((update & 1) ? (vcolor >> 24) : (ge[CMD_MATERIALALPHA] & 0xFF)) / 255.0f;
	alpha *= (ge[CMD_AMBIENTALPHA] & 0xFF) / 255.0f;
	float c[3] = { emissive[0] + ambient[0] * mat_ambient[0], emissive[1] + ambient[1] * mat_ambient[1],
		emissive[2] + ambient[2] * mat_ambient[2] };
	normalize3(n);
	float shininess = f24(ge[CMD_MATERIALSPECULARCOEF]);
	for (int i = 0; i < 4; i++) {
		if (!(ge[CMD_LIGHTENABLE0 + i] & 1))
			continue;
		uint32_t type = ge[CMD_LIGHTTYPE0 + i];
		uint32_t kind = (type >> 8) & 3, comps = type & 3;
		float lp[3] = { f24(ge[CMD_LX0 + i * 3]), f24(ge[CMD_LX0 + i * 3 + 1]), f24(ge[CMD_LX0 + i * 3 + 2]) };
		float l[3], att = 1.0f;
		if (kind == 0) {
			l[0] = lp[0]; l[1] = lp[1]; l[2] = lp[2];
			normalize3(l);
		} else {
			l[0] = lp[0] - p[0]; l[1] = lp[1] - p[1]; l[2] = lp[2] - p[2];
			float d = sqrt1(dot3(l, l));
			if (d > 0.0f) { l[0] /= d; l[1] /= d; l[2] /= d; }
			float k = f24(ge[CMD_LKA0 + i * 3]) + f24(ge[CMD_LKA0 + i * 3 + 1]) * d + f24(ge[CMD_LKA0 + i * 3 + 2]) * d * d;
			att = k > 0.0f ? 1.0f / k : 1.0f;
			if (att > 1.0f) att = 1.0f;
			if (kind == 2) {
				float dir[3] = { f24(ge[CMD_LDX0 + i * 3]), f24(ge[CMD_LDX0 + i * 3 + 1]), f24(ge[CMD_LDX0 + i * 3 + 2]) };
				normalize3(dir);
				float spot = -dot3(l, dir);
				att *= spot >= f24(ge[CMD_LKO0 + i]) ? powf_safe(spot, f24(ge[CMD_LKS0 + i])) : 0.0f;
			}
		}
		float la[3], ld[3], ls[3];
		rgb(ge[CMD_LAC0 + i * 3], la);
		rgb(ge[CMD_LAC0 + i * 3 + 1], ld);
		rgb(ge[CMD_LAC0 + i * 3 + 2], ls);
		float diffuse = dot3(n, l);
		if (diffuse < 0.0f) diffuse = 0.0f;
		if (comps == 2)
			diffuse = powf_safe(diffuse, shininess);
		float specular = 0.0f;
		if (comps == 1 && diffuse > 0.0f) {
			float h[3] = { l[0], l[1], l[2] + 1.0f };
			normalize3(h);
			specular = powf_safe(dot3(n, h), shininess);
		}
		for (int k = 0; k < 3; k++)
			c[k] += att * (la[k] * mat_ambient[k] + ld[k] * mat_diffuse[k] * diffuse + ls[k] * mat_specular[k] * specular);
	}
	uint32_t out = 0;
	for (int k = 0; k < 3; k++) {
		float x = c[k] > 1.0f ? 1.0f : c[k];
		out |= (uint32_t)(x * 255.0f + 0.5f) << (k * 8);
	}
	if (alpha > 1.0f) alpha = 1.0f;
	return out | ((uint32_t)(alpha * 255.0f + 0.5f) << 24);
}

/* One PSP vertex as a renderer vertex, in Vita clip space. */
static void vertexOut(const uint8_t *v, const GrFormat *f, float tex_w, float tex_h, GrVertex *o) {
	float pos[3], u = 0.0f, t = 0.0f;
	if (f->tc_type) {
		const uint8_t *p = v + f->tc_off;
		if (f->through) {
			u = component(p, f->tc_type, 0, 1.0f, 1.0f, 1) / tex_w;
			t = component(p, f->tc_type, 1, 1.0f, 1.0f, 1) / tex_h;
		} else {
			u = component(p, f->tc_type, 0, 1.0f / 128.0f, 1.0f / 32768.0f, 1);
			t = component(p, f->tc_type, 1, 1.0f / 128.0f, 1.0f / 32768.0f, 1);
			u = u * f24(ge[CMD_TEXSCALEU]) + f24(ge[CMD_TEXOFFSETU]);
			t = t * f24(ge[CMD_TEXSCALEV]) + f24(ge[CMD_TEXOFFSETV]);
		}
	}
	o->u = u;
	o->v = t;
	o->color = vertexColor(v, f);
	o->fog = 1.0f;
	const uint8_t *p = v + f->pos_off;
	if (f->through) {
		float x = component(p, f->pos_type, 0, 1.0f, 1.0f, 0);
		float y = component(p, f->pos_type, 1, 1.0f, 1.0f, 0);
		float z = f->pos_type == 2 ? ((const uint16_t *)p)[2] : component(p, f->pos_type, 2, 1.0f, 1.0f, 0);
		o->x = x / 240.0f - 1.0f;
		o->y = 1.0f - y / 136.0f;
		o->z = z / 65535.0f;
		o->w = 1.0f;
		return;
	}
	pos[0] = component(p, f->pos_type, 0, 1.0f / 128.0f, 1.0f / 32768.0f, 0);
	pos[1] = component(p, f->pos_type, 1, 1.0f / 128.0f, 1.0f / 32768.0f, 0);
	pos[2] = component(p, f->pos_type, 2, 1.0f / 128.0f, 1.0f / 32768.0f, 0);
	float skinned[3], w[3], e[3], nrm[3] = { 0.0f, 0.0f, 1.0f }, snrm[3];
	int lit = (ge[CMD_LIGHTINGENABLE] & 1) && f->nrm_type;
	if (lit) {
		const uint8_t *np = v + f->nrm_off;
		nrm[0] = component(np, f->nrm_type, 0, 1.0f / 128.0f, 1.0f / 32768.0f, 0);
		nrm[1] = component(np, f->nrm_type, 1, 1.0f / 128.0f, 1.0f / 32768.0f, 0);
		nrm[2] = component(np, f->nrm_type, 2, 1.0f / 128.0f, 1.0f / 32768.0f, 0);
	}
	if (f->weight_type) {
		skinned[0] = skinned[1] = skinned[2] = 0.0f;
		snrm[0] = snrm[1] = snrm[2] = 0.0f;
		for (uint32_t i = 0; i < f->weights && i < 8; i++) {
			float weight = component(v + f->weight_off, f->weight_type, i, 1.0f / 128.0f, 1.0f / 32768.0f, 1);
			float b[3], bn[3];
			mul43(&bones[i * 12], pos, b);
			skinned[0] += b[0] * weight;
			skinned[1] += b[1] * weight;
			skinned[2] += b[2] * weight;
			if (lit) {
				mul33(&bones[i * 12], nrm, bn);
				snrm[0] += bn[0] * weight;
				snrm[1] += bn[1] * weight;
				snrm[2] += bn[2] * weight;
			}
		}
	} else {
		skinned[0] = pos[0]; skinned[1] = pos[1]; skinned[2] = pos[2];
		snrm[0] = nrm[0]; snrm[1] = nrm[1]; snrm[2] = nrm[2];
	}
	mul43(world, skinned, w);
	mul43(view, w, e);
	if (lit) {
		float wn[3];
		mul33(world, snrm, wn);
		o->color = lightVertex(w, wn, o->color, f->col_type != 0);
	}
	if (ge[CMD_FOGENABLE] & 1) {
		float fog = (f24(ge[CMD_FOG1]) + e[2]) * f24(ge[CMD_FOG2]);
		o->fog = fog < 0.0f ? 0.0f : fog > 1.0f ? 1.0f : fog;
	}
	float cx = e[0] * proj[0] + e[1] * proj[4] + e[2] * proj[8] + proj[12];
	float cy = e[0] * proj[1] + e[1] * proj[5] + e[2] * proj[9] + proj[13];
	float cz = e[0] * proj[2] + e[1] * proj[6] + e[2] * proj[10] + proj[14];
	float cw = e[0] * proj[3] + e[1] * proj[7] + e[2] * proj[11] + proj[15];
	float xs = f24(ge[CMD_VIEWPORTXSCALE]), xc = f24(ge[CMD_VIEWPORTXCENTER]) - (ge[CMD_OFFSETX] & 0xFFFF) / 16.0f;
	float ys = f24(ge[CMD_VIEWPORTYSCALE]), yc = f24(ge[CMD_VIEWPORTYCENTER]) - (ge[CMD_OFFSETY] & 0xFFFF) / 16.0f;
	float zs = f24(ge[CMD_VIEWPORTZSCALE]), zc = f24(ge[CMD_VIEWPORTZCENTER]);
	/* screen = clip / w * scale + centre, kept linear in clip space for the GPU's clipper */
	o->x = (xs * cx + xc * cw) / 240.0f - cw;
	o->y = cw - (ys * cy + yc * cw) / 136.0f;
	o->z = (zs * cz + zc * cw) / 65535.0f;
	o->w = cw;
}

/* --------------------------------------------------------------- draws */

static uint32_t blendFactor(uint32_t f, int src, uint32_t fixed) {
	switch (f) {
		case 0: return src ? SCE_GXM_BLEND_FACTOR_DST_COLOR : SCE_GXM_BLEND_FACTOR_SRC_COLOR;
		case 1: return src ? SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_COLOR : SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
		case 2: case 6: return SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
		case 3: case 7: return SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		case 4: case 8: return SCE_GXM_BLEND_FACTOR_DST_ALPHA;
		case 5: case 9: return SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
		default: /* fixed colour: GXM has no constant, black and white are exact */
			return (fixed & 0xFFFFFF) == 0 ? SCE_GXM_BLEND_FACTOR_ZERO : SCE_GXM_BLEND_FACTOR_ONE;
	}
}

static uint32_t blendState(void) {
	uint32_t mask = SCE_GXM_COLOR_MASK_ALL;
	if (ge[CMD_CLEARMODE] & 1) {
		mask = ((ge[CMD_CLEARMODE] >> 8) & 1) ? SCE_GXM_COLOR_MASK_R | SCE_GXM_COLOR_MASK_G | SCE_GXM_COLOR_MASK_B : 0;
		if ((ge[CMD_CLEARMODE] >> 9) & 1)
			mask |= SCE_GXM_COLOR_MASK_A;
		return 0x80000000u | (mask << 24);
	}
	if ((ge[CMD_MASKRGB] & 0xFFFFFF) == 0xFFFFFF)
		mask &= ~(SCE_GXM_COLOR_MASK_R | SCE_GXM_COLOR_MASK_G | SCE_GXM_COLOR_MASK_B);
	if (!(ge[CMD_ALPHABLENDENABLE] & 1))
		return mask == SCE_GXM_COLOR_MASK_ALL ? 0 : 0x80000000u | (mask << 24);
	uint32_t d = ge[CMD_BLENDMODE];
	uint32_t src = blendFactor(d & 0xF, 1, ge[CMD_BLENDFIXEDA]), dst = blendFactor((d >> 4) & 0xF, 0, ge[CMD_BLENDFIXEDB]);
	static const uint8_t eq[8] = { SCE_GXM_BLEND_FUNC_ADD, SCE_GXM_BLEND_FUNC_SUBTRACT, SCE_GXM_BLEND_FUNC_REVERSE_SUBTRACT,
		SCE_GXM_BLEND_FUNC_MIN, SCE_GXM_BLEND_FUNC_MAX, SCE_GXM_BLEND_FUNC_ADD, SCE_GXM_BLEND_FUNC_ADD, SCE_GXM_BLEND_FUNC_ADD };
	return 0x80000000u | (mask << 24) | (eq[(d >> 8) & 7] << 16) | (src << 8) | dst;
}

static const uint32_t depth_funcs[8] = {
	SCE_GXM_DEPTH_FUNC_NEVER, SCE_GXM_DEPTH_FUNC_ALWAYS, SCE_GXM_DEPTH_FUNC_EQUAL, SCE_GXM_DEPTH_FUNC_NOT_EQUAL,
	SCE_GXM_DEPTH_FUNC_LESS, SCE_GXM_DEPTH_FUNC_LESS_EQUAL, SCE_GXM_DEPTH_FUNC_GREATER, SCE_GXM_DEPTH_FUNC_GREATER_EQUAL,
};
static const uint32_t stencil_funcs[8] = {
	SCE_GXM_STENCIL_FUNC_NEVER, SCE_GXM_STENCIL_FUNC_ALWAYS, SCE_GXM_STENCIL_FUNC_EQUAL, SCE_GXM_STENCIL_FUNC_NOT_EQUAL,
	SCE_GXM_STENCIL_FUNC_LESS, SCE_GXM_STENCIL_FUNC_LESS_EQUAL, SCE_GXM_STENCIL_FUNC_GREATER, SCE_GXM_STENCIL_FUNC_GREATER_EQUAL,
};
static const uint32_t stencil_ops[8] = {
	SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_ZERO, SCE_GXM_STENCIL_OP_REPLACE, SCE_GXM_STENCIL_OP_INVERT,
	SCE_GXM_STENCIL_OP_INCR, SCE_GXM_STENCIL_OP_DECR, SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP,
};

/* A new draw with the current state, or the last one if its state is the same. */
static GrDraw *drawState(SceGxmTexture *tex) {
	GrDraw d;
	uint32_t clear = ge[CMD_CLEARMODE] & 1;
	memset(&d, 0, sizeof(d));
	d.fb = (ge[CMD_FRAMEBUFPTR] & 0xFFFFF0) & 0x1FFFF0;
	d.textured = tex != NULL;
	if (tex) {
		d.tex = *tex;
		uint32_t filter = ge[CMD_TEXFILTER], wrap = ge[CMD_TEXWRAP];
		sceGxmTextureSetMinFilter(&d.tex, (filter & 1) ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT);
		sceGxmTextureSetMagFilter(&d.tex, ((filter >> 8) & 1) ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT);
		sceGxmTextureSetUAddrMode(&d.tex, (wrap & 1) ? SCE_GXM_TEXTURE_ADDR_CLAMP : SCE_GXM_TEXTURE_ADDR_REPEAT);
		sceGxmTextureSetVAddrMode(&d.tex, ((wrap >> 8) & 1) ? SCE_GXM_TEXTURE_ADDR_CLAMP : SCE_GXM_TEXTURE_ADDR_REPEAT);
	}
	rgb(ge[CMD_FOGCOLOR], d.fog);
	d.fog[3] = 1.0f;
	uint32_t func = ge[CMD_TEXFUNC] & 7, rgba = (ge[CMD_TEXFUNC] >> 8) & 1, dbl = (ge[CMD_TEXFUNC] >> 16) & 1;
	uint32_t atest = (!clear && (ge[CMD_ALPHATESTENABLE] & 1)) ? (ge[CMD_ALPHATEST] & 7) : 8;
	d.variant = grVariantKey(d.textured, func, rgba, dbl, atest);
	d.aref = (ge[CMD_ALPHATEST] >> 8) & 0xFF;
	uint32_t env = ge[CMD_TEXENVCOLOR];
	d.env[0] = (env & 0xFF) / 255.0f; d.env[1] = ((env >> 8) & 0xFF) / 255.0f; d.env[2] = ((env >> 16) & 0xFF) / 255.0f; d.env[3] = 1.0f;
	d.blend = blendState();
	if (clear) {
		d.depth_func = SCE_GXM_DEPTH_FUNC_ALWAYS;
		d.depth_write = (ge[CMD_CLEARMODE] >> 10) & 1;
		if ((ge[CMD_CLEARMODE] >> 9) & 1) { /* alpha clear: the PSP keeps stencil in alpha */
			d.stencil = 1;
			d.stencil_func = SCE_GXM_STENCIL_FUNC_ALWAYS;
			d.stencil_fail = d.stencil_zfail = d.stencil_zpass = SCE_GXM_STENCIL_OP_REPLACE;
			d.stencil_ref = 0;
			d.stencil_mask = d.stencil_wmask = 0xFF;
		}
	} else if (ge[CMD_ZTESTENABLE] & 1) {
		d.depth_func = depth_funcs[ge[CMD_ZTEST] & 7];
		d.depth_write = !(ge[CMD_ZWRITEDISABLE] & 1);
	} else {
		d.depth_func = SCE_GXM_DEPTH_FUNC_ALWAYS;
		d.depth_write = 0;
	}
	if (!clear && (ge[CMD_STENCILTESTENABLE] & 1)) {
		d.stencil = 1;
		d.stencil_func = stencil_funcs[ge[CMD_STENCILTEST] & 7];
		d.stencil_ref = (ge[CMD_STENCILTEST] >> 8) & 0xFF;
		d.stencil_mask = (ge[CMD_STENCILTEST] >> 16) & 0xFF;
		d.stencil_fail = stencil_ops[ge[CMD_STENCILOP] & 7];
		d.stencil_zfail = stencil_ops[(ge[CMD_STENCILOP] >> 8) & 7];
		d.stencil_zpass = stencil_ops[(ge[CMD_STENCILOP] >> 16) & 7];
		d.stencil_wmask = ~ge[CMD_MASKALPHA] & 0xFF;
	}
	d.sc_x0 = ge[CMD_SCISSOR1] & 0x3FF; d.sc_y0 = (ge[CMD_SCISSOR1] >> 10) & 0x3FF;
	d.sc_x1 = ge[CMD_SCISSOR2] & 0x3FF; d.sc_y1 = (ge[CMD_SCISSOR2] >> 10) & 0x3FF;
	d.scissor = !(d.sc_x0 == 0 && d.sc_y0 == 0 && d.sc_x1 >= 479 && d.sc_y1 >= 271);
	if (building->ndraws) {
		GrDraw *last = &building->draws[building->ndraws - 1];
		if (last->first + last->count == building->nverts) {
			uint32_t first = last->first, count = last->count;
			d.first = first; d.count = count;
			if (!memcmp(last, &d, sizeof(d)))
				return last;
		}
	}
	if (building->ndraws == GR_ARENA_DRAWS)
		return NULL;
	d.first = building->nverts;
	d.count = 0;
	building->draws[building->ndraws] = d;
	return &building->draws[building->ndraws++];
}

/* Six vertices of a quad one PSP pixel wide around a to b (a point when a is b). */
static uint32_t quad(const GrVertex *a, const GrVertex *b, GrVertex *out) {
	float ax = a->x / a->w, ay = a->y / a->w, bx = b->x / b->w, by = b->y / b->w;
	float dx = (bx - ax) * 240.0f, dy = (by - ay) * 136.0f, l = sqrt1(dx * dx + dy * dy);
	float ox, oy, ex = 0.0f, ey = 0.0f;
	if (l < 1e-6f) { /* a point: a pixel-sized square */
		ox = 0.5f / 240.0f; oy = 0.0f; ex = 0.0f; ey = 0.5f / 136.0f;
		GrVertex c[4] = { *a, *a, *a, *a };
		c[0].x = (ax - ox - ex) * a->w; c[0].y = (ay - oy - ey) * a->w;
		c[1].x = (ax + ox - ex) * a->w; c[1].y = (ay + oy - ey) * a->w;
		c[2].x = (ax + ox + ex) * a->w; c[2].y = (ay + oy + ey) * a->w;
		c[3].x = (ax - ox + ex) * a->w; c[3].y = (ay - oy + ey) * a->w;
		out[0] = c[0]; out[1] = c[1]; out[2] = c[2]; out[3] = c[0]; out[4] = c[2]; out[5] = c[3];
		return 6;
	}
	ox = -dy / l * 0.5f / 240.0f;
	oy = dx / l * 0.5f / 136.0f;
	GrVertex c[4] = { *a, *a, *b, *b };
	c[0].x = (ax - ox) * a->w; c[0].y = (ay - oy) * a->w;
	c[1].x = (ax + ox) * a->w; c[1].y = (ay + oy) * a->w;
	c[2].x = (bx + ox) * b->w; c[2].y = (by + oy) * b->w;
	c[3].x = (bx - ox) * b->w; c[3].y = (by - oy) * b->w;
	out[0] = c[0]; out[1] = c[1]; out[2] = c[2]; out[3] = c[0]; out[4] = c[2]; out[5] = c[3];
	return 6;
}

static void drawPrim(uint32_t data) {
	uint32_t count = data & 0xFFFF, type = (data >> 16) & 7;
	uint32_t vt = ge[CMD_VERTEXTYPE];
	uint32_t vaddr = vertex_addr, iaddr = index_addr;
	uint32_t index_type = (vt >> 11) & 3;
	GrFormat f;
	vertexFormat(vt, &f);
	/* the GE moves past what it read, whether we draw it or not */
	uint32_t index_size = index_type == 1 ? 1 : index_type == 2 ? 2 : 0;
	uint32_t morphs = ((vt >> 18) & 7) + 1;
	if (index_size)
		index_addr = iaddr + count * index_size;
	else
		vertex_addr = vaddr + count * f.stride * morphs;
	if (!count || !f.pos_type)
		return;
	if (!drawTarget(ge[CMD_FRAMEBUFPTR] & 0x1FFFF0))
		return;
	SceGxmTexture *tex = NULL;
	float tex_w = 1.0f, tex_h = 1.0f;
	if ((ge[CMD_TEXTUREMAPENABLE] & 1) && !(ge[CMD_CLEARMODE] & 1) && f.tc_type) {
		tex = textureCurrent();
		if (!tex)
			return; /* not shown yet: skipped rather than drawn untextured */
		tex_w = (float)(1u << (ge[CMD_TEXSIZE0] & 0xF));
		tex_h = (float)(1u << ((ge[CMD_TEXSIZE0] >> 8) & 0xF));
	}
	/* the vertex data this primitive reads */
	uint32_t min_index = 0, max_index = count - 1;
	const uint8_t *idx8 = NULL;
	const uint16_t *idx16 = NULL;
	uint8_t *vdata;
	if (index_size) {
		if (count * index_size > GR_TEMP_SIZE / 4 || !tapReadBlock(iaddr, temp, count * index_size)) {
			readFailed(2, iaddr, count * index_size);
			return;
		}
		idx8 = temp;
		idx16 = (const uint16_t *)temp;
		min_index = 0xFFFF;
		max_index = 0;
		for (uint32_t i = 0; i < count; i++) {
			uint32_t n = index_size == 1 ? idx8[i] : idx16[i];
			if (n > max_index)
				max_index = n;
			if (n < min_index)
				min_index = n;
		}
	}
	uint32_t vstride = f.stride * morphs, vbytes = (max_index - min_index + 1) * vstride;
	/* vdata[i * vstride] is vertex i: the buffer starts min_index vertices before what is read */
	uint8_t *vread = temp + GR_TEMP_SIZE / 4;
	if (vbytes > GR_TEMP_SIZE * 3 / 4 || !tapReadBlock(vaddr + min_index * vstride, vread, vbytes)) {
		readFailed(3, vaddr + min_index * vstride, vbytes);
		return;
	}
	vdata = vread - min_index * vstride;
	uint32_t out_count = type == 6 ? (count / 2) * 6 : type == 3 ? count - count % 3 :
		type == 0 ? count * 6 : type == 1 ? (count / 2) * 6 : type == 2 ? (count >= 2 ? (count - 1) * 6 : 0) :
		count >= 3 ? (count - 2) * 3 : 0;
	if (!out_count || out_count > GR_ARENA_VERTS)
		return;
	if (building->nverts + out_count > GR_ARENA_VERTS || building->ndraws == GR_ARENA_DRAWS) {
		sceKernelLockMutex(arena_lock, 1, NULL);
		arenaClose(building);
		sceKernelUnlockMutex(arena_lock, 1);
		building = arenaTakeWait();
		if (!building) {
			stats.dropped_draws++;
			return;
		}
		building->fb = ge[CMD_FRAMEBUFPTR] & 0x1FFFF0;
	}
	GrDraw *d = drawState(tex);
	if (!d)
		return;
	GrVertex *out = &building->verts[building->nverts];
#define VERTEX(i, o) vertexOut(vdata + (index_size ? (index_size == 1 ? idx8[i] : idx16[i]) : (i)) * f.stride * morphs, &f, tex_w, tex_h, (o))
	uint32_t n = 0;
	if (type == 6) { /* sprites: corners from two vertices, colour and depth from the second */
		for (uint32_t i = 0; i + 1 < count; i += 2) {
			GrVertex a, b;
			VERTEX(i, &a);
			VERTEX(i + 1, &b);
			GrVertex c[4] = { a, b, b, a };
			c[0] = b; c[0].x = a.x / a.w * b.w; c[0].y = a.y / a.w * b.w; c[0].u = a.u; c[0].v = a.v;
			c[1] = b; c[1].y = a.y / a.w * b.w; c[1].v = a.v;
			c[2] = b;
			c[3] = b; c[3].x = a.x / a.w * b.w; c[3].u = a.u;
			out[n++] = c[0]; out[n++] = c[1]; out[n++] = c[2];
			out[n++] = c[0]; out[n++] = c[2]; out[n++] = c[3];
		}
	} else if (type == 3) {
		for (uint32_t i = 0; i < out_count; i++)
			VERTEX(i, &out[n++]);
	} else if (type < 3) { /* points and lines: one PSP pixel wide quads */
		GrVertex a, b;
		for (uint32_t i = 0; i < count; i++) {
			if (type == 0) {
				VERTEX(i, &a);
				n += quad(&a, &a, &out[n]);
			} else if (type == 1 && i + 1 < count && !(i & 1)) {
				VERTEX(i, &a);
				VERTEX(i + 1, &b);
				n += quad(&a, &b, &out[n]);
			} else if (type == 2 && i + 1 < count) {
				VERTEX(i, &a);
				VERTEX(i + 1, &b);
				n += quad(&a, &b, &out[n]);
			}
		}
	} else {
		GrVertex first, prev, cur;
		VERTEX(0, &first);
		VERTEX(1, &prev);
		GrVertex prev2 = first;
		for (uint32_t i = 2; i < count; i++) {
			VERTEX(i, &cur);
			if (type == 4) { /* strip */
				out[n++] = prev2; out[n++] = prev; out[n++] = cur;
				prev2 = prev;
			} else { /* fan */
				out[n++] = first; out[n++] = prev; out[n++] = cur;
			}
			prev = cur;
		}
	}
#undef VERTEX
	building->nverts += n;
	d->count += n;
}

void grCommand(uint32_t cmd, uint32_t data, uint32_t offset) {
	if (!gr_ready)
		return;
	switch (cmd) {
		case 0xCB: /* TEXFLUSH: the game changed a texture it uses */
			tex_epoch++;
			break;
		case CMD_VADDR: /* relative like jumps: BASE, then the list's offset */
			vertex_addr = ((((ge[CMD_BASE] << 8) & 0x0F000000) | (data & 0xFFFFFF)) + offset) & 0x0FFFFFFF;
			break;
		case CMD_IADDR:
			index_addr = ((((ge[CMD_BASE] << 8) & 0x0F000000) | (data & 0xFFFFFF)) + offset) & 0x0FFFFFFF;
			break;
		case CMD_PRIM: {
			uint32_t started = sceKernelGetProcessTimeLow();
			drawPrim(data);
			stats.build_us += sceKernelGetProcessTimeLow() - started;
			return;
		}
		case CMD_BONEMATRIXNUMBER: bone_i = data & 0x7F; break;
		case CMD_BONEMATRIXDATA: if (bone_i < 8 * 12) bones[bone_i] = f24(data); bone_i++; break;
		case CMD_WORLDMATRIXNUMBER: world_i = data & 0xF; break;
		case CMD_WORLDMATRIXDATA: if (world_i < 12) world[world_i] = f24(data); world_i++; break;
		case CMD_VIEWMATRIXNUMBER: view_i = data & 0xF; break;
		case CMD_VIEWMATRIXDATA: if (view_i < 12) view[view_i] = f24(data); view_i++; break;
		case CMD_PROJMATRIXNUMBER: proj_i = data & 0xF; break;
		case CMD_PROJMATRIXDATA: if (proj_i < 16) proj[proj_i] = f24(data); proj_i++; break;
		case CMD_LOADCLUT: {
			uint32_t addr = geAddr(ge[CMD_CLUTADDR], (ge[CMD_CLUTADDRUPPER] & 0x0F0000) << 8);
			clut_bytes = (data & 0x3F) * 32;
			if (clut_bytes > sizeof(clut))
				clut_bytes = sizeof(clut);
			if (!clut_bytes || !tapReadBlock(addr, clut, clut_bytes))
				clut_bytes = 0;
			clut_hash = hashBytes(clut, clut_bytes) ^ addr;
			break;
		}
	}
	ge[cmd] = data;
}

void grReset(void) {
	memset(ge, 0, sizeof(ge));
	vertex_addr = index_addr = 0;
	texture_count = texmem_used = 0;
	display_count = 0;
	ge[CMD_TEXFUNC] = 0;
	ge[CMD_SCISSOR2] = 479 | (271 << 10);
}

/* --------------------------------------------------------------- shaders */

static uint8_t *pack; /* GR_SHADER_PACK, programs used in place */
static uint32_t pack_count;

static int loadPack(void) {
	SceUID fd = sceIoOpen(GR_SHADER_PACK, SCE_O_RDONLY, 0);
	if (fd < 0) {
		sceClibPrintf("[gr] %s missing (%08x): run the GE shader baker\n", GR_SHADER_PACK, fd);
		return 0;
	}
	int size = sceIoLseek32(fd, 0, SCE_SEEK_END);
	sceIoLseek32(fd, 0, SCE_SEEK_SET);
	pack = size > 0 ? grAlloc(size) : NULL;
	int read = pack ? sceIoRead(fd, pack, size) : -1;
	sceIoClose(fd);
	GrShaderPackHeader *h = (GrShaderPackHeader *)pack;
	if (read != size || size < (int)sizeof(*h) || h->magic != GR_SHADER_MAGIC || h->version != GR_SHADER_VERSION) {
		sceClibPrintf("[gr] %s unusable (%d of %d bytes)\n", GR_SHADER_PACK, read, size);
		return 0;
	}
	pack_count = h->count;
	sceClibPrintf("[gr] shader pack: %u programs, %d bytes\n", pack_count, size);
	return 1;
}

static const SceGxmProgram *packProgram(uint32_t key) {
	GrShaderPackEntry *e = (GrShaderPackEntry *)(pack + sizeof(GrShaderPackHeader));
	for (uint32_t i = 0; i < pack_count; i++)
		if (e[i].key == key)
			return (const SceGxmProgram *)(pack + e[i].offset);
	return NULL;
}

static GrVariant *variant(uint32_t key) {
	GrVariant *v = &variants[key];
	if (v->state)
		return v->state > 0 ? v : NULL;
	v->state = -1;
	v->gxp = packProgram(key);
	if (!v->gxp || sceGxmProgramCheck(v->gxp) < 0 || sceGxmShaderPatcherRegisterProgram(patcher, v->gxp, &v->id) < 0) {
		stats.shader_failures++;
		return NULL;
	}
	v->env = sceGxmProgramFindParameterByName(v->gxp, "uEnv");
	v->aref = sceGxmProgramFindParameterByName(v->gxp, "uAref");
	v->fog = sceGxmProgramFindParameterByName(v->gxp, "uFog");
	v->state = 1;
	stats.shaders++;
	return v;
}

static SceGxmFragmentProgram *fragmentProgram(uint32_t key, uint32_t blend) {
	for (uint32_t i = 0; i < fragment_program_count; i++)
		if (fragment_programs[i].variant == key && fragment_programs[i].blend == blend)
			return fragment_programs[i].program;
	GrVariant *v = variant(key);
	if (!v || fragment_program_count == GR_FRAGMENT_PROGRAMS)
		return NULL;
	SceGxmBlendInfo info, *pinfo = NULL;
	if (blend) {
		memset(&info, 0, sizeof(info));
		info.colorMask = (blend >> 24) & 0xF;
		if (blend & 0xFFFFFF) {
			info.colorFunc = info.alphaFunc = (blend >> 16) & 0xFF;
			info.colorSrc = info.alphaSrc = (blend >> 8) & 0xFF;
			info.colorDst = info.alphaDst = blend & 0xFF;
		} else {
			info.colorFunc = info.alphaFunc = SCE_GXM_BLEND_FUNC_NONE;
			info.colorSrc = info.alphaSrc = SCE_GXM_BLEND_FACTOR_ONE;
			info.colorDst = info.alphaDst = SCE_GXM_BLEND_FACTOR_ZERO;
		}
		pinfo = &info;
	}
	SceGxmFragmentProgram *program = NULL;
	if (sceGxmShaderPatcherCreateFragmentProgram(patcher, v->id, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
			SCE_GXM_MULTISAMPLE_NONE, pinfo, vertex_gxp, &program) < 0)
		return NULL;
	fragment_programs[fragment_program_count].variant = key;
	fragment_programs[fragment_program_count].blend = blend;
	fragment_programs[fragment_program_count].program = program;
	fragment_program_count++;
	return program;
}

static int createVertexProgram(void) {
	if (!vertex_gxp || sceGxmProgramCheck(vertex_gxp) < 0 ||
			sceGxmShaderPatcherRegisterProgram(patcher, vertex_gxp, &vertex_id) < 0)
		return 0;
	const SceGxmProgramParameter *pos = sceGxmProgramFindParameterByName(vertex_gxp, "aPosition");
	const SceGxmProgramParameter *tc = sceGxmProgramFindParameterByName(vertex_gxp, "aTexcoord");
	const SceGxmProgramParameter *col = sceGxmProgramFindParameterByName(vertex_gxp, "aColor");
	const SceGxmProgramParameter *fog = sceGxmProgramFindParameterByName(vertex_gxp, "aFog");
	if (!pos || !tc || !col || !fog)
		return 0;
	SceGxmVertexAttribute attributes[4];
	attributes[0].streamIndex = 0;
	attributes[0].offset = 0;
	attributes[0].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
	attributes[0].componentCount = 4;
	attributes[0].regIndex = sceGxmProgramParameterGetResourceIndex(pos);
	attributes[1].streamIndex = 0;
	attributes[1].offset = 16;
	attributes[1].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
	attributes[1].componentCount = 2;
	attributes[1].regIndex = sceGxmProgramParameterGetResourceIndex(tc);
	attributes[2].streamIndex = 0;
	attributes[2].offset = 24;
	attributes[2].format = SCE_GXM_ATTRIBUTE_FORMAT_U8N;
	attributes[2].componentCount = 4;
	attributes[2].regIndex = sceGxmProgramParameterGetResourceIndex(col);
	attributes[3].streamIndex = 0;
	attributes[3].offset = 28;
	attributes[3].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
	attributes[3].componentCount = 1;
	attributes[3].regIndex = sceGxmProgramParameterGetResourceIndex(fog);
	SceGxmVertexStream stream;
	stream.stride = sizeof(GrVertex);
	stream.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
	return sceGxmShaderPatcherCreateVertexProgram(patcher, vertex_id, attributes, 4, &stream, 1, &vertex_program) >= 0;
}

static void *patcherAlloc(void *user, unsigned int size) { (void)user; return grAlloc(size); }
static void patcherFree(void *user, void *p) { (void)user; grFree(p); }

static int createPatcher(void) {
	SceUID uid;
	unsigned int vertex_offset, fragment_offset;
	void *buffer = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 64 * 1024, 4,
		SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE, &uid);
	void *vertex_usse = vertex_usse_alloc(64 * 1024, &uid, &vertex_offset);
	void *fragment_usse = fragment_usse_alloc(256 * 1024, &uid, &fragment_offset);
	if (!buffer || !vertex_usse || !fragment_usse)
		return 0;
	SceGxmShaderPatcherParams params;
	memset(&params, 0, sizeof(params));
	params.hostAllocCallback = patcherAlloc;
	params.hostFreeCallback = patcherFree;
	params.bufferMem = buffer;
	params.bufferMemSize = 64 * 1024;
	params.vertexUsseMem = vertex_usse;
	params.vertexUsseMemSize = 64 * 1024;
	params.vertexUsseOffset = vertex_offset;
	params.fragmentUsseMem = fragment_usse;
	params.fragmentUsseMemSize = 256 * 1024;
	params.fragmentUsseOffset = fragment_offset;
	return sceGxmShaderPatcherCreate(&params, &patcher) >= 0;
}

/* -------------------------------------------------------------- present */

void grInit(void) {
	arena_lock = sceKernelCreateMutex("AdrenalineGeRender", 0, 0, NULL);
	grLogMemory("before");
	uint8_t *gpu = grGpu("AdrenalineGeRenderGpu", GR_GPU_BLOCK, SCE_GXM_MEMORY_ATTRIB_READ);
	uint8_t *cpu = grBlock("AdrenalineGeRenderCpu", GR_CPU_BLOCK);
	int ok = gpu && cpu;
	if (ok) {
		for (int i = 0; i < GR_ARENAS; i++) {
			arenas[i].verts = (GrVertex *)(gpu + i * GR_ARENA_BYTES);
			arenas[i].draws = (GrDraw *)(cpu + i * GR_ARENA_DRAWS * sizeof(GrDraw));
		}
		indices = (uint16_t *)(gpu + GR_ARENAS * GR_ARENA_BYTES);
		texmem = gpu + GR_ARENAS * GR_ARENA_BYTES + GR_INDEX_BYTES;
		temp = cpu + GR_DRAWS_BYTES;
		heap = mspace_create(cpu + GR_CPU_BLOCK - GR_HEAP_SIZE, GR_HEAP_SIZE);
		ok = heap != NULL;
	}
	if (!ok) {
		sceClibPrintf("[gr] allocation failed after %u blocks: renderer off\n", block_count);
		grFreeAll();
		grLogMemory("after release");
		return;
	}
	grLogMemory("after");
	for (uint32_t i = 0; i < 65536; i++)
		indices[i] = i;
	grReset();
	gr_ready = 1;
	sceClibPrintf("[gr] ready\n");
}

/* The PSP keeps what is in a framebuffer from frame to frame, and games
   redraw only what changed (menus): each display framebuffer has a render
   target of ours at the Vita's resolution, colour and depth kept between
   scenes, that every frame drawn into it is added to. */
typedef struct {
	uint32_t fb;
	vita2d_texture *rt;
} GrTarget;
static GrTarget targets[4];
static uint32_t target_count;
static uint32_t rt_w = 960, rt_h = 544;
static float rt_scale = 2.0f;

/* Targets are 960x544 and made once (freeing them does not give the memory
   back); 1.5x draws into their top left 720x408. */
static vita2d_texture *targetFor(uint32_t fb) {
	/* One for all display framebuffers. Games draw whole frames into the
	   buffer they show next, or the same changes into both: drawing them
	   all into one target, frame after frame, is what the screen shows, and
	   two targets of different age shown in turn flicker. */
	if (target_count)
		return targets[0].rt;
	/* main memory: CDRAM is vita2d's and ScePspemu's */
	SceKernelMemBlockType type = vita2d_texture_get_alloc_memblock_type();
	vita2d_texture_set_alloc_memblock_type(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE);
	vita2d_texture *rt = vita2d_create_empty_texture_format(960, 544, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
	vita2d_texture_set_alloc_memblock_type(type);
	if (!rt || !rt->gxm_rtgt) {
		sceClibPrintf("[gr] render target for %06x failed\n", fb);
		return NULL;
	}
	sceGxmDepthStencilSurfaceSetForceLoadMode(&rt->gxm_sfd, SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED);
	sceGxmDepthStencilSurfaceSetForceStoreMode(&rt->gxm_sfd, SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED);
	targets[target_count].fb = fb;
	targets[target_count].rt = rt;
	target_count++;
	sceClibPrintf("[gr] render target %u for framebuffer %06x\n", target_count, fb);
	grLogMemory("after render target");
	return rt;
}

int grHasFrame(void) {
	return gr_ready && shown_fb != 0xFFFFFFFF;
}

static void drawArena(GrArena *a) {
	SceGxmContext *ctx = _vita2d_context;
	sceGxmSetVertexProgram(ctx, vertex_program);
	for (uint32_t i = 0; i < a->ndraws; i++) {
		GrDraw *d = &a->draws[i];
		if (!d->count)
			continue;
		SceGxmFragmentProgram *fp = fragmentProgram(d->variant, d->blend);
		if (!fp)
			continue;
		sceGxmSetFragmentProgram(ctx, fp);
		GrVariant *v = &variants[d->variant];
		void *uniforms;
		sceGxmReserveFragmentDefaultUniformBuffer(ctx, &uniforms);
		if (v->env)
			sceGxmSetUniformDataF(uniforms, v->env, 0, 4, d->env);
		if (v->aref)
			sceGxmSetUniformDataF(uniforms, v->aref, 0, 1, &d->aref);
		if (v->fog)
			sceGxmSetUniformDataF(uniforms, v->fog, 0, 4, d->fog);
		if (d->textured)
			sceGxmSetFragmentTexture(ctx, 0, &d->tex);
		sceGxmSetFrontDepthFunc(ctx, d->depth_func);
		sceGxmSetFrontDepthWriteEnable(ctx, d->depth_write ? SCE_GXM_DEPTH_WRITE_ENABLED : SCE_GXM_DEPTH_WRITE_DISABLED);
		if (d->stencil) {
			sceGxmSetFrontStencilFunc(ctx, d->stencil_func, d->stencil_fail, d->stencil_zfail, d->stencil_zpass,
				d->stencil_mask, d->stencil_wmask);
			sceGxmSetFrontStencilRef(ctx, d->stencil_ref);
		} else {
			sceGxmSetFrontStencilFunc(ctx, SCE_GXM_STENCIL_FUNC_ALWAYS, SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP,
				SCE_GXM_STENCIL_OP_KEEP, 0xFF, 0xFF);
		}
		if (d->scissor)
			sceGxmSetRegionClip(ctx, SCE_GXM_REGION_CLIP_OUTSIDE, (uint32_t)(d->sc_x0 * rt_scale), (uint32_t)(d->sc_y0 * rt_scale),
				(uint32_t)((d->sc_x1 + 1) * rt_scale) - 1, (uint32_t)((d->sc_y1 + 1) * rt_scale) - 1);
		else
			sceGxmSetRegionClip(ctx, SCE_GXM_REGION_CLIP_OUTSIDE, 0, 0, rt_w - 1, rt_h - 1);
		for (uint32_t done = 0; done < d->count;) {
			uint32_t n = d->count - done > 65535 ? 65535 - 65535 % 3 : d->count - done;
			sceGxmSetVertexStream(ctx, 0, &a->verts[d->first + done]);
			sceGxmDraw(ctx, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, indices, n);
			done += n;
		}
	}
	/* what vita2d expects */
	sceGxmSetFrontDepthFunc(ctx, SCE_GXM_DEPTH_FUNC_ALWAYS);
	sceGxmSetFrontDepthWriteEnable(ctx, SCE_GXM_DEPTH_WRITE_ENABLED);
	sceGxmSetFrontStencilFunc(ctx, SCE_GXM_STENCIL_FUNC_ALWAYS, SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP,
		SCE_GXM_STENCIL_OP_KEEP, 0xFF, 0xFF);
	sceGxmSetRegionClip(ctx, SCE_GXM_REGION_CLIP_OUTSIDE, 0, 0, 959, 543);
}

void grRender(void) {
	static int programs_state;
	if (!gr_ready)
		return;
	if (!programs_state) {
		/* the pack first: without it the patcher's memory is not taken */
		vertex_gxp = loadPack() ? packProgram(GR_VERTEX_KEY) : NULL;
		programs_state = (vertex_gxp && createPatcher() && createVertexProgram()) ? 1 : -1;
		sceClibPrintf("[gr] programs %s\n", programs_state > 0 ? "ready" : "failed");
	}
	if (programs_state < 0)
		return;
	present_count++;
	rt_w = 960; /* (GePatch: 960x544 only) */
	rt_h = 544;
	rt_scale = rt_w / 480.0f;
	/* the GPU is done with every frame drawn before: they are free */
	vita2d_wait_rendering_done();
	sceKernelLockMutex(arena_lock, 1, NULL);
	for (int i = 0; i < GR_ARENAS; i++)
		if (arenas[i].state == ARENA_RETIRED)
			arenas[i].state = ARENA_FREE;
	sceKernelUnlockMutex(arena_lock, 1);
	/* every frame the game finished, oldest first, in one scene */
	GrArena *order[GR_ARENAS];
	uint32_t count = 0;
	sceKernelLockMutex(arena_lock, 1, NULL);
	for (int i = 0; i < GR_ARENAS; i++) {
		if (arenas[i].state != ARENA_READY)
			continue;
		arenas[i].state = ARENA_SHOWING;
		uint32_t at = count++;
		while (at && order[at - 1]->seq > arenas[i].seq) { /* insertion by seq */
			order[at] = order[at - 1];
			at--;
		}
		order[at] = &arenas[i];
	}
	sceKernelUnlockMutex(arena_lock, 1);
	if (!count)
		return;
	vita2d_texture *rt = targetFor(order[0]->fb);
	if (rt) {
		vita2d_start_drawing_advanced(rt, 0);
		for (uint32_t i = 0; i < count; i++)
			drawArena(order[i]);
		vita2d_end_drawing();
		stats.presents += count;
	}
	sceKernelLockMutex(arena_lock, 1, NULL);
	for (uint32_t i = 0; i < count; i++) {
		order[i]->state = ARENA_RETIRED;
		order[i]->retired_at = present_count;
	}
	sceKernelUnlockMutex(arena_lock, 1);
}

int grPresent(void) {
	if (!gr_ready)
		return 0;
	if (!target_count)
		return 0;
	vita2d_draw_texture_part_scale(targets[0].rt, 0.0f, 0.0f, 0.0f, 0.0f, rt_w, rt_h, 960.0f / rt_w, 544.0f / rt_h);
	return 1;
}

/* A 24-bit BMP from rows of ABGR pixels, row(y) giving row y. */
static void writeBmp(const char *path, uint32_t w, uint32_t h, const uint32_t *(*row)(uint32_t y, void *arg), void *arg) {
	static uint8_t line[960 * 3];
	uint32_t pitch = w * 3, size = 54 + pitch * h;
	uint8_t hdr[54];
	memset(hdr, 0, sizeof(hdr));
	hdr[0] = 'B'; hdr[1] = 'M';
	memcpy(hdr + 2, &size, 4);
	hdr[10] = 54; hdr[14] = 40;
	memcpy(hdr + 18, &w, 4);
	memcpy(hdr + 22, &h, 4);
	hdr[26] = 1; hdr[28] = 24;
	SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
	if (fd < 0)
		return;
	sceIoWrite(fd, hdr, sizeof(hdr));
	for (uint32_t y = h; y-- > 0;) { /* bottom-up */
		const uint32_t *src = row(y, arg);
		for (uint32_t x = 0; x < w; x++) {
			uint32_t c = src ? src[x] : 0;
			line[x * 3] = (c >> 16) & 0xFF; line[x * 3 + 1] = (c >> 8) & 0xFF; line[x * 3 + 2] = c & 0xFF;
		}
		sceIoWrite(fd, line, pitch);
	}
	sceIoClose(fd);
}

static const uint32_t *rtRow(uint32_t y, void *arg) {
	vita2d_texture *rt = arg;
	return (const uint32_t *)((uint8_t *)vita2d_texture_get_datap(rt) + y * vita2d_texture_get_stride(rt));
}

static const uint32_t *pspRow(uint32_t y, void *arg) {
	static uint32_t row[512];
	(void)arg; /* ScePspemu's copy of the PSP screen, 512x272 ABGR, read the way Adrenaline reads it */
	sceDmacMemcpy(row, (void *)(SCE_PSPEMU_FRAMEBUFFER + y * 512 * 4), sizeof(row));
	return row;
}

void grDump(void) {
	static uint32_t dumps;
	char path[64];
	if (!gr_ready || dumps >= 100)
		return;
	uint32_t fb = shown_fb;
	vita2d_texture *rt = target_count ? targets[0].rt : NULL;
	if (!rt)
		return;
	sceIoMkdir("ux0:data/AdrenalineGE", 0777);
	snprintf(path, sizeof(path), "ux0:data/AdrenalineGE/dump%02u_vita.bmp", dumps);
	writeBmp(path, rt_w, rt_h, rtRow, rt);
	snprintf(path, sizeof(path), "ux0:data/AdrenalineGE/dump%02u_psp.bmp", dumps);
	writeBmp(path, 480, 272, pspRow, NULL);
	sceClibPrintf("[gr] dump %u: framebuffer %06x\n", dumps, fb);
	dumps++;
}

void grReport(void) {
	uint32_t f = stats.frames ? stats.frames : 1;
	grLogMemory("now");
	sceClibPrintf("[gr] displays=%u frames=%u dropped=%u dropped_draws=%u presents=%u draws/f=%u verts/f=%u build_us/f=%u tex_decodes=%u tex_hits=%u "
		"tex_unsupported=%u skipped fb=%u vram_tex=%u prim=%u read=%u shaders=%u failed=%u textures=%u texmem_kb=%u\n",
		display_count, stats.frames, stats.dropped, stats.dropped_draws, stats.presents, stats.draws / f, stats.verts / f, stats.build_us / f,
		stats.tex_decodes, stats.tex_hits, stats.tex_unsupported, stats.skipped_fb, stats.skipped_vram, stats.skipped_prim,
		stats.skipped_read, stats.shaders, stats.shader_failures, texture_count, texmem_used >> 10);
	for (uint32_t i = 0; i < read_fail_count; i++)
		sceClibPrintf("[gr] read failed: %s addr=%08x size=%u vtype=%06x\n",
			read_fails[i][0] == 1 ? "texture" : read_fails[i][0] == 2 ? "indices" : "vertices",
			read_fails[i][1], read_fails[i][2], read_fails[i][3]);
	read_fail_count = 0;
	uint32_t shaders = stats.shaders, failures = stats.shader_failures;
	memset(&stats, 0, sizeof(stats));
	stats.shaders = shaders;
	stats.shader_failures = failures;
}
