/* Synthetic GE streams only: this does not load or execute a game. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include <math.h>
#include "../main.c"
#include "gepatch_reference_slice.h"

static unsigned copies, submissions, hooks;
static int sync_result;
static int list_sync_result;
static int fail_copy;
static void require(int ok, const char *message) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}
int pspSdkSetK1(int value) { return 0; }
int sceIoOpen(const char *s,int a,int b) { return -1; }
int sceIoLseek(int a,int b,int c) { return 0; }
int sceIoWrite(int a,const void *b,int c) { return c; }
int sceIoClose(int a) { return 0; }
static unsigned whole_cache_flushes;
void sceKernelDcacheWritebackInvalidateAll(void) { ++whole_cache_flushes; }
void sceKernelDcacheWritebackAll(void) {}
static unsigned writeback_ranges;
void sceKernelDcacheWritebackRange(const void *p, unsigned n) { (void)p; (void)n; ++writeback_ranges; }
void sceKernelDcacheInvalidateRange(const void *p, unsigned n) { (void)p; (void)n; }
void sceKernelDcacheWritebackInvalidateRange(const void *p, unsigned n) { (void)p; (void)n; }
void sceKernelIcacheClearAll(void) {}
static unsigned fake_time_us, fake_time_advance; /* advance: per reading (a wait that must end) */
unsigned sceKernelGetSystemTimeLow(void) { fake_time_us += fake_time_advance; return fake_time_us; }
int scePowerGetCpuClockFrequencyInt(void) { return 333; }
int scePowerGetBusClockFrequencyInt(void) { return 166; }
int sceCtrlPeekBufferPositive(SceCtrlData *pad,int n) { pad->Buttons = 0; return n; }
uintptr_t sctrlHENFindFunction(const char *m,const char *l,unsigned n) { return 0; }
void sctrlHENPatchSyscall(void *address,void *function) { ++hooks; }
STMOD_HANDLER sctrlHENSetStartModuleHandler(STMOD_HANDLER h) { (void)h; return NULL; }
unsigned sceKernelPartitionMaxFreeMemSize(int pid) { (void)pid; return 0; }
SceUID sceKernelAllocPartitionMemory(int p, const char *n, int t, unsigned s, void *a) { (void)p; (void)n; (void)t; (void)s; (void)a; return -1; }
void *sceKernelGetBlockHeadAddr(SceUID b) { (void)b; return NULL; }
int sceKernelCpuSuspendIntr(void) { return 0; }
void sceKernelCpuResumeIntr(int i) { (void)i; }
SceUID sceKernelCreateThread(const char *n, SceKernelThreadEntry e, int p, int s, unsigned a, void *o) { (void)n; (void)e; (void)p; (void)s; (void)a; (void)o; return -1; }
int sceKernelStartThread(SceUID t, SceSize n, void *a) { (void)t; (void)n; (void)a; return -1; }
int sceKernelDelayThread(unsigned us) { (void)us; return 0; }
int sceGuStart(int a,void *b) { return 0; }
void sceGuCopyImage(int format,int sx,int sy,int w,int h,int pitch,void *src,int dx,int dy,int dpitch,void *dst) {
    require(w == 960 && h == 544 && pitch == 960 && dpitch == 960, "copy dimensions");
}
int sceGuFinish(void) { return 0; }
static int next_qid = 42;
static PspGeListArgs *last_args;
static int queue(const void *list,void *stall,int cb,PspGeListArgs *arg) {
    ++submissions;
    last_args = arg;
    if (list == (void *)RENDER_LIST) {
        if (fail_copy) { fail_copy = 0; return -1; }
        ++copies;
    }
    return next_qid++; /* distinct ids, as the firmware hands out */
}
/* the stall updates of a list released while parsed (see run_release_tests) */
static unsigned stall_updates;
static void (*stall_check)(const u32 *stall);
static int update_stall(int qid, void *stall) {
    (void)qid;
    ++stall_updates;
    if (stall_check) stall_check((const u32 *)((uintptr_t)stall & 0x0fffffffu));
    return 0;
}
static int set_frame(void *addr,int stride,int format,int sync) { return 0; }
static int ge_sync(int mode) { return sync_result; }
static int ge_list_sync(int qid, int mode) { return list_sync_result; }
static u32 command(unsigned cmd, u32 data) { return cmd << 24 | (data & 0xFFFFFFu); }
static u32 float24(float value) { u32 bits; memcpy(&bits, &value, 4); return bits >> 8; }
static float from24(u32 value) { float f; value <<= 8; memcpy(&f, &value, 4); return f; }
#include "inset_cases.h"
#include "stencil_cases.h"
#include "fastpath_cases.h"

int main(void) {
    unsigned i;
    u32 *list = VirtualAlloc((void *)0x08000000u, 0x03000000u, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    void *flag = VirtualAlloc((void *)0xABCD0000u, 0x10000u, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    /* The uncached alias the display copy list is written through. */
    void *uncached = VirtualAlloc((void *)(RENDER_LIST | 0xA0000000u), 0x10000u, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    /* ... and the one DISPLAY_FRONT's values are written through */
    void *front = VirtualAlloc((void *)((DISPLAY_FRONT | 0xA0000000u) & ~0xffffu), 0x10000u, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    short *vertices = (short *)0x08820000u;
    u32 *sublist = (u32 *)0x08040000u;
    setvbuf(stdout, NULL, _IONBF, 0); /* a crash must not swallow earlier PASS lines */
    require(front != NULL, "the display front alias");
    require(list == (void *)0x08000000u && flag == (void *)0xABCD0000u &&
            uncached == (void *)(RENDER_LIST | 0xA0000000u), "synthetic address space allocation");
    *(volatile u32 *)GP_MODE_ADDR = 2; /* (our Adrenaline's side there) */
    _sceGeListEnQueue = queue;
    _sceGeListEnQueueHead = queue;
    _sceGeListUpdateStallAddr = update_stall;
    release_enabled = 0; /* (its own tests: run_release_tests, run_linear_tests) */
    short_release = 0; /* (its own test: run_short_release_tests) */
    _sceDisplaySetFrameBuf = set_frame;
    _sceGeDrawSync = ge_sync;
    _sceGeListSync = ge_list_sync;

    for (i = 0; i < 3; ++i) {
        vertices[i * 3] = (short)(20 + i);
        vertices[i * 3 + 1] = (short)(40 + i);
        vertices[i * 3 + 2] = 123;
    }
    list[0] = command(GE_CMD_BASE, 0x080000);
    list[1] = command(GE_CMD_VADDR, 0x820000);
    list[2] = command(GE_CMD_VERTEXTYPE, GE_VTYPE_THROUGH | GE_VTYPE_POS_16BIT);
    list[3] = command(GE_CMD_VIEWPORTXSCALE, float24(120.0f));
    list[4] = command(GE_CMD_VIEWPORTYCENTER, float24(2100.0f));
    list[5] = command(GE_CMD_OFFSETX, 1808u << 4);
    list[6] = command(GE_CMD_SCISSOR1, 10 | 20 << 10);
    list[7] = command(GE_CMD_SCISSOR2, 479 | 271 << 10);
    list[8] = command(GE_CMD_PRIM, 3 << 16 | 3);
    list[9] = command(GE_CMD_FINISH, 0);
    list[10] = command(GE_CMD_END, 0);
    {
        u32 original_words[11], first_words[11];
        short original_vertices[9], first_vertices[9];
        memcpy(original_words, list, sizeof(original_words));
        memcpy(original_vertices, vertices, sizeof(original_vertices));
        resetGeState(); patchGeList(list, NULL);
        memcpy(first_words, list, sizeof(first_words));
        memcpy(first_vertices, vertices, sizeof(first_vertices));
        restoreCompletedVertices();
        require(!memcmp(vertices, original_vertices, sizeof(original_vertices)), "coordinates restored exactly at completion");
        memcpy(list, original_words, sizeof(original_words));
        memcpy(vertices, original_vertices, sizeof(original_vertices));
        reference_resetGeState(); reference_patchGeList(list, NULL);
        /* Upstream set the viewport and scissor to the whole screen; they are
         * the game's own here, scaled 2x: 120 -> 240; center y 2100 with the
         * offset 1912 -> 2 * 188 + 1776; scissor from (10, 20) -> (20, 40). */
        require(first_words[3] == command(GE_CMD_VIEWPORTXSCALE, float24(240.0f)) &&
                first_words[4] == command(GE_CMD_VIEWPORTYCENTER, float24(2152.0f)) &&
                first_words[6] == command(GE_CMD_SCISSOR1, 20 | 40 << 10) &&
                first_words[7] == command(GE_CMD_SCISSOR2, 959 | 543 << 10), "the game's viewport and scissor, scaled");
        /* the game's OFFSETX carries into the centre (ours: 2 * (2048 - 1808) + 1568) */
        require(first_words[5] == command(GE_CMD_VIEWPORTXCENTER, float24(2048.0f)), "the game's offset in the centre");
        first_words[3] = list[3]; first_words[4] = list[4]; first_words[5] = list[5]; first_words[6] = list[6];
        require(!memcmp(first_words, list, sizeof(first_words)), "command results match immutable upstream");
        require(!memcmp(first_vertices, vertices, sizeof(first_vertices)), "vertex/depth results match immutable upstream");
        require(vertices[2] == 123, "depth remains unchanged");
    }
    // Log-observed format: u16 UV, RGBA8888, float XYZ, no padding.
    {
        typedef struct { uint16_t u, v; uint32_t color; float x, y, z; } HudVertex;
        HudVertex *hud = (HudVertex *)vertices;
        HudVertex original[3];
        u32 stride, pos, pad;
        getVertexInfo(0x80019Eu, &stride, &pos, &pad);
        require(stride == 20 && pos == 8 && pad == 0, "actual no-padding HUD format decoded");
        for (i = 0; i < 3; ++i) {
            hud[i].u = 0x1111; hud[i].v = 0x2222; hud[i].color = 0x654321ABu;
            hud[i].x = 20.0f + i; hud[i].y = 30.0f + i; hud[i].z = 123.0f;
        }
        memcpy(original, hud, sizeof(original));
        list[2] = command(GE_CMD_VERTEXTYPE, 0x80019Eu);
        sceGeListEnQueuePatched(list, NULL, -1, NULL);
        require(hud[0].x == 40 && hud[0].y == 60, "first HUD submission scaled");
        require(hud[0].u == original[0].u && hud[0].v == original[0].v &&
                hud[0].color == original[0].color && hud[0].z == original[0].z,
                "UV/color/Z not used as markers or changed");
        sceGeListEnQueuePatched(list, NULL, -1, NULL);
        require(hud[0].x == 40 && vertex_restore.used + vertex_restore.log_used == 3, "shared references scale once before fence");
        sync_result = 1;
        sceGeDrawSyncPatched(1);
        require(hud[0].x == 40 && vertex_restore.used + vertex_restore.log_used == 3, "busy GE leaves GPU inputs intact");
        list_sync_result = 0;
        sceGeListSyncPatched(42, 0);
        require(vertex_restore.used + vertex_restore.log_used == 3, "one completed list cannot restore another busy list's inputs");
        sync_result = 0;
        sceGeDrawSyncPatched(0);
        require(!memcmp(original, hud, sizeof(original)) && !(vertex_restore.used + vertex_restore.log_used), "successful fence restores all original bytes");
        // The previously broken equal-bits game update is now unambiguous:
        // original data has already been restored before control returns.
        hud[0].x = 40;
        hud[0].y = 70;
        sceGeListEnQueuePatched(list, NULL, -1, NULL);
        require(hud[0].x == 80 && hud[0].y == 140, "new game value equal to prior scaled value is scaled correctly");
        sceGeListSyncPatched(42, 0);
        require(hud[0].x == 40 && hud[0].y == 70 && !(vertex_restore.used + vertex_restore.log_used), "list completion plus global idle retires edits");
    }
    copies = 0; framebuf_seen = 0; rendered_in_sync = 0;
    // Presentation is a separate experiment from the reverted geometry.
    state.has_draws = 1;
    sceGeListEnQueuePatched(list, NULL, -1, NULL);
    require(copies == 0, "no partial-frame copy from enqueue");
    sync_result = 0;
    require(sceGeDrawSyncPatched(0) == 0 && copies == 1, "fixed framebuffer copies after completed GE work");
    sceDisplaySetFrameBufPatched((void *)FAKE_VRAM, 512, 3, 1);
    require(copies == 1, "SetFrameBuf does not repeat completed-GE copy");
    sceGeDrawSyncPatched(0);
    require(copies == 1, "swapping game does not copy before SetFrameBuf");
    sceDisplaySetFrameBufPatched((void *)FAKE_VRAM, 512, 3, 1);
    require(copies == 2, "swapping game copies at SetFrameBuf");
    sceGeDrawSyncPatched(0); sceGeDrawSyncPatched(0); sceGeDrawSyncPatched(0); /* Tekken: three waits a frame */
    require(copies == 2, "a swapping game's later syncs in a frame copy nothing (upstream: half-drawn copies)");
    fake_time_us += 200000u; /* no frame shown for 200 ms: a fixed framebuffer again */
    sceGeDrawSyncPatched(0);
    require(copies == 3, "no SetFrameBuf for a while: copied at completion again");
    sceDisplaySetFrameBufPatched((void *)FAKE_VRAM, 512, 3, 1);
    require(copies == 3, "and not again at the SetFrameBuf after it");
    copies = 2;
    framebuf_seen = 0; rendered_in_sync = 0;
    sync_result = -1;
    sceGeDrawSyncPatched(0);
    require(copies == 2, "failed GE sync does not present");
    sync_result = 1;
    sceGeDrawSyncPatched(1);
    require(copies == 2, "peek/busy sync does not present");
    sync_result = 0;
    fail_copy = 1;
    sceGeDrawSyncPatched(0);
    require(copies == 2 && rendered_in_sync == 0, "failed copy is not marked presented");
    sceDisplaySetFrameBufPatched((void *)FAKE_VRAM, 512, 3, 1);
    require(copies == 3, "display retries failed completion copy");
    restoreCompletedVertices();
    run_inset_tests(list);
    run_stencil_tests(list);
    run_fastpath_tests();
    run_stride_tests();
    *(volatile u32 *)GP_MODE_ADDR = 0;
    stencil_frames_left = alpha_frames_left = STENCIL_HOLD_FRAMES;
    require(module_start(0, NULL) == -1 && gp_stock &&
            chooseDrawFormat(GE_FORMAT_5551) == GE_FORMAT_565,
            "an Adrenaline without our side (nothing at the setting): 960x544, frames 565 even with stencil");
    stencil_frames_left = alpha_frames_left = 0;
    *(volatile u32 *)GP_MODE_ADDR = 2;
    gp_stock = 0;
    require(module_start(0, NULL) == -1 && hooks == 0, "missing firmware export installs no hooks");
    puts("PASS: no-padding HUD reuse/fence/equal-value update; UV/color/Z preservation; first-pass upstream equivalence; copy policy and errors");
    VirtualFree(list, 0, MEM_RELEASE); VirtualFree(flag, 0, MEM_RELEASE);
    return 0;
}
