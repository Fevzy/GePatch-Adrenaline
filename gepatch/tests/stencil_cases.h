/* Stencil1: the draw buffer keeps the game's 16-bit format with alpha (where
 * the stencil lives); the display copy transfers it as it is and tells
 * Adrenaline the format. Synthetic only. */
static void run_stencil_tests(uint32_t *list) {
    const uint32_t *copy = (const uint32_t *)(RENDER_LIST | 0xA0000000u);
    typedef struct { uint16_t u, v; int16_t x, y, z; } CopyVertex;
    const CopyVertex *v = (const CopyVertex *)((PRESENT_VERTICES & 0x0fffffffu) | 0xA0000000u);
    unsigned i, prims = 0, area = 0, finished = 0, kicked = 0, restored_tfx = 0;
    uint32_t tex_format = 0xFFu, fb_format = 0xFFu, tex_address = 0xFFFFFFFFu, filter = 0xFFu;
    uint32_t fb_pointer = 0xFFFFFFFFu, transfer_dst = 0xFFFFFFFFu, transfer_src = 0xFFFFFFFFu;

    /* A game asking for 565 keeps 565 even with stencil commands: the PSP
     * has no stencil in 565 either (Tekken 6). */
    stencil_frames_left = 0; alpha_frames_left = 0;
    list[0] = command(GE_CMD_STENCILTESTENABLE, 1);
    list[1] = command(GE_CMD_FRAMEBUFPIXFORMAT, GE_FORMAT_565);
    list[2] = command(GE_CMD_FINISH, 0);
    list[3] = command(GE_CMD_END, 0);
    sceGeListEnQueuePatched(list, NULL, -1, NULL);
    require(list[1] == command(GE_CMD_FRAMEBUFPIXFORMAT, GE_FORMAT_565), "565 game with stencil stays 565");
    sceGeDrawSyncPatched(0);

    /* No stencil in use (movies, menus): 565 and the plain copy. */
    stencil_frames_left = 0;
    list[0] = command(GE_CMD_FRAMEBUFPIXFORMAT, GE_FORMAT_5551);
    list[1] = command(GE_CMD_FINISH, 0);
    list[2] = command(GE_CMD_END, 0);
    sceGeListEnQueuePatched(list, NULL, -1, NULL);
    require(list[0] == command(GE_CMD_FRAMEBUFPIXFORMAT, GE_FORMAT_565) && draw_format == GE_FORMAT_565,
            "without stencil use the draw buffer is 565");

    list[0] = command(GE_CMD_STENCILTESTENABLE, 1);
    list[1] = command(GE_CMD_FRAMEBUFPIXFORMAT, GE_FORMAT_5551);
    list[2] = command(GE_CMD_TEXFUNC, 0x010100);
    list[3] = command(GE_CMD_FINISH, 0);
    list[4] = command(GE_CMD_END, 0);
    sceGeListEnQueuePatched(list, NULL, -1, NULL);
    require(list[1] == command(GE_CMD_FRAMEBUFPIXFORMAT, GE_FORMAT_5551) && draw_format == GE_FORMAT_5551,
            "stencil in use: 5551, its alpha bit is the stencil");
    list[1] = command(GE_CMD_FRAMEBUFPIXFORMAT, GE_FORMAT_565);
    sceGeListEnQueuePatched(list, NULL, -1, NULL);
    require(list[1] == command(GE_CMD_FRAMEBUFPIXFORMAT, GE_FORMAT_5551),
            "one format for every target: a 565 request (system dialog) is drawn as 5551 too");
    list[1] = command(GE_CMD_FRAMEBUFPIXFORMAT, GE_FORMAT_5551);
    sceGeListEnQueuePatched(list, NULL, -1, NULL);

    framebuf_seen = 0; rendered_in_sync = 0; sync_result = 0; last_args = (PspGeListArgs *)1;
    sceGeDrawSyncPatched(0);
    require(last_args == NULL, "display copy queued without a kernel-memory argument block");
    for (i = 0; i < 64u && !finished; ++i) {
        const unsigned cmd = copy[i] >> 24, data = copy[i] & 0xFFFFFFu;
        if (cmd == GE_CMD_TRANSFERSRC) transfer_src = data;
        if (cmd == GE_CMD_TRANSFERDST) transfer_dst = data;
        if (cmd == GE_CMD_TRANSFERSTART) { require(data == 0u, "16-bit frame transferred 16-bit"); kicked = 1; }
        if (cmd == GE_CMD_PRIM) ++prims;
        if (cmd == GE_CMD_END) finished = 1;
    }
    require(finished && kicked && !prims, "the display copy is a plain transfer, no conversion pass");
    require(transfer_src == (VRAM_DRAW_BUFFER_OFFSET & 0xFFFFFFu) && transfer_dst == (DISPLAY_BUFFER & 0xFFFFFFu),
            "draw buffer transferred to the display buffer");
    require(*(volatile u32 *)GP_FRAME_SIZE_ADDR == (WIDTH | (HEIGHT << 16)) &&
            *(volatile u32 *)GP_FRAME_FORMAT_ADDR == GE_FORMAT_5551,
            "Adrenaline is told the frame's size and its 5551 format");
    (void)v; (void)area; (void)restored_tfx; (void)tex_format; (void)fb_format; (void)tex_address; (void)filter; (void)fb_pointer;

    /* 8888 does not fit at 960x544: 5551 while the stencil is in use. */
    list[0] = command(GE_CMD_FRAMEBUFPIXFORMAT, GE_FORMAT_8888);
    list[1] = command(GE_CMD_FINISH, 0);
    list[2] = command(GE_CMD_END, 0);
    sceGeListEnQueuePatched(list, NULL, -1, NULL);
    require(list[0] == command(GE_CMD_FRAMEBUFPIXFORMAT, GE_FORMAT_5551), "8888 drawn as 5551 while stencil is used");
    /* Two seconds of frames without stencil: back to 565 and the plain copy. */
    for (i = 0; i < STENCIL_HOLD_FRAMES; ++i) sceDisplaySetFrameBufPatched((void *)FAKE_VRAM, 512, 3, 1);
    list[0] = command(GE_CMD_FRAMEBUFPIXFORMAT, GE_FORMAT_8888);
    sceGeListEnQueuePatched(list, NULL, -1, NULL);
    require(list[0] == command(GE_CMD_FRAMEBUFPIXFORMAT, GE_FORMAT_565) && draw_format == GE_FORMAT_565,
            "stencil unused for a while: 565 again");
    framebuf_seen = 0; rendered_in_sync = 0; last_args = (PspGeListArgs *)1;
    sceGeDrawSyncPatched(0);
    require(last_args == NULL, "565 frames keep the plain image transfer");
    puts("PASS: 5551 only while stencil is used, one format for all targets, plain display copy in the frame's format, decay to 565");
}
