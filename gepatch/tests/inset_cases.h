/* UV inset (Inset1). Synthetic GE streams only: no game is loaded or run. */
typedef struct { uint16_t u, v; uint32_t color; float x, y, z; } InsetHud;

static void inset_require_close(float a, float b, const char *message) {
    require(fabsf(a - b) < 1e-4f, message);
}

static void inset_group_tests(void) {
    GpInsetVertex g[4];
    memset(g, 0, sizeof(g));
    g[0].u = 16; g[0].v = 8; g[1].u = 32; g[1].v = 24;
    gp_inset_group(g, 2);
    inset_require_close(g[0].u, 16.5f, "sprite min U moves half a texel in");
    inset_require_close(g[1].u, 31.5f, "sprite max U moves half a texel in");
    inset_require_close(g[0].v, 8.5f, "sprite min V moves half a texel in");
    inset_require_close(g[1].v, 23.5f, "sprite max V moves half a texel in");
    g[0].u = 16; g[1].u = 16.5f; gp_inset_group(g, 2);
    inset_require_close(g[0].u, 16.5f, "sub-texel span: shader clamp keeps lo");
    inset_require_close(g[1].u, 16.5f, "sub-texel span: both ends at lo");
    g[0].u = 0; g[1].u = 8; g[2].u = 4; g[3].u = 8;
    g[0].v = 0; g[1].v = 0; g[2].v = 4; g[3].v = 8;
    gp_inset_group(g, 4);
    inset_require_close(g[2].u, 4.0f, "interior UV inside [lo,hi] unchanged");
    inset_require_close(g[3].v, 7.5f, "quad max V inset");
}

/* sceGu order: BASE, VADDR, VTYPE, state, PRIM; then the next draw sets its
 * own BASE, VADDR and VTYPE again. */
static void inset_build_list(uint32_t *l, uint32_t second_prim_has_own_words) {
    unsigned n = 0;
    l[n++] = command(GE_CMD_BASE, 0x080000);
    l[n++] = command(GE_CMD_VADDR, 0x820000);
    l[n++] = command(GE_CMD_VERTEXTYPE, 0x80019Eu);
    l[n++] = command(GE_CMD_TEXTUREMAPENABLE, 1);
    l[n++] = command(GE_CMD_TEXFILTER, 0x101);
    l[n++] = command(GE_CMD_TEXMODE, 0);
    l[n++] = command(GE_CMD_TEXSIZE0, 0x0808);
    l[n++] = command(GE_CMD_PRIM, 6u << 16 | 2);           /* index 7 */
    if (second_prim_has_own_words) {
        l[n++] = command(GE_CMD_BASE, 0x080000);
        l[n++] = command(GE_CMD_VADDR, 0x830000);
        l[n++] = command(GE_CMD_VERTEXTYPE, 0x80019Eu);
    }
    l[n++] = command(GE_CMD_TEXTUREMAPENABLE, 0);
    l[n++] = command(GE_CMD_PRIM, 6u << 16 | 2);
    l[n++] = command(GE_CMD_FINISH, 0);
    l[n++] = command(GE_CMD_END, 0);
}

static void inset_set_vertices(InsetHud *h) {
    h[0].u = 16; h[0].v = 8; h[0].color = 0x80ABCDEFu; h[0].x = 20; h[0].y = 30; h[0].z = 7;
    h[1].u = 32; h[1].v = 24; h[1].color = 0x12345678u; h[1].x = 36; h[1].y = 46; h[1].z = 9;
}

static void run_inset_tests(uint32_t *list) {
    InsetHud *h = (InsetHud *)0x08820000u, *h2 = (InsetHud *)0x08830000u, saved[2];
    uint32_t before[16];
    GpInsetVertex *pool = (GpInsetVertex *)VERTICES_BUFFER;
    unsigned applied;
    inset_group_tests();
    gp_inset_init(&inset, pool, (RENDER_LIST - VERTICES_BUFFER) / sizeof(GpInsetVertex));
    sync_result = 0; sceGeDrawSyncPatched(0);

    inset_set_vertices(h); inset_set_vertices(h2);
    memcpy(saved, h, sizeof(saved));
    inset_build_list(list, 1);
    memcpy(before, list, sizeof(before));
    sceGeListEnQueuePatched(list, NULL, -1, NULL);
    require(inset.applied == 1, "textured linear sprite with its own words is inset");
    require(list[2] == command(GE_CMD_VERTEXTYPE, GP_INSET_VTYPE), "VTYPE now float UV/RGBA8888/float XYZ");
    require(((list[0] << 8) & 0x0F000000u) == 0x0A000000u, "BASE now reaches the pool");
    require((((list[0] << 8) & 0x0F000000u) | (list[1] & 0xFFFFFFu)) == ((uint32_t)(uintptr_t)pool & 0x0FFFFFFFu),
            "VADDR resolves to the inset copy");
    require(list[7] == before[7] && list[8] == before[8], "PRIM words and the next draw are untouched");
    inset_require_close(pool[0].u, 16.5f, "copy U0 inset");
    inset_require_close(pool[1].u, 31.5f, "copy U1 inset");
    inset_require_close(pool[0].v, 8.5f, "copy V0 inset");
    inset_require_close(pool[1].v, 23.5f, "copy V1 inset");
    require(pool[0].x == 40 && pool[1].y == 92, "copy carries the Vertices3-scaled positions");
    require(pool[0].color == 0x80ABCDEFu && pool[1].color == 0x12345678u && pool[1].z == 9,
            "copy keeps colour and depth");
    require(h[0].u == 16 && h[1].v == 24, "guest UVs themselves are not changed");

    /* The same list again before completion sees the original words. */
    applied = inset.applied;
    sync_result = 1; /* the GE still busy with the first */
    sceGeListEnQueuePatched(list, NULL, -1, NULL);
    require(inset.applied == applied && inset.reused == 1 && inset.used_vertices == 2,
            "repeated parse reuses the redirection, no second copy");
    sync_result = 1; sceGeDrawSyncPatched(1);
    require(list[2] == command(GE_CMD_VERTEXTYPE, GP_INSET_VTYPE), "busy GE keeps the redirection");
    sync_result = 0; sceGeDrawSyncPatched(0);
    require(!memcmp(list, before, sizeof(before)) && !memcmp(h, saved, sizeof(saved)),
            "completion restores the list and vertices exactly");
    require(inset.used_vertices == 0, "pool released at completion");

    /* The next draw continues the pointer, but the list ends after it. */
    inset_build_list(list, 0);
    memcpy(before, list, sizeof(before));
    applied = inset.applied;
    sceGeListEnQueuePatched(list, NULL, -1, NULL);
    require(inset.applied == applied && !memcmp(list, before, 8 * 4) && inset.used_vertices == 0,
            "draw followed by one that inherits VADDR/VTYPE keeps its words; copy rolled back");
    sceGeDrawSyncPatched(0);

    /* The list ends right after the draw: GE state may carry on, so no edit. */
    list[8] = command(GE_CMD_FINISH, 0); list[9] = command(GE_CMD_END, 0);
    inset_build_list(list, 1);
    list[8] = command(GE_CMD_FINISH, 0); list[9] = command(GE_CMD_END, 0);
    memcpy(before, list, sizeof(before));
    applied = inset.applied;
    sceGeListEnQueuePatched(list, NULL, -1, NULL);
    require(inset.applied == applied && !memcmp(list, before, 10 * 4), "last draw of a list is not redirected");
    sceGeDrawSyncPatched(0);

    /* BASE also used by something else (IADDR) before the draw: not ours alone. */
    inset_build_list(list, 1);
    list[3] = command(GE_CMD_IADDR, 0x840000);
    memcpy(before, list, sizeof(before));
    applied = inset.applied;
    sceGeListEnQueuePatched(list, NULL, -1, NULL);
    require(inset.applied == applied && !memcmp(list, before, 3 * 4), "shared BASE word is never changed");
    sceGeDrawSyncPatched(0);

    /* Nearest filtering has no neighbour bleed: untouched. */
    inset_build_list(list, 1);
    list[4] = command(GE_CMD_TEXFILTER, 0);
    applied = inset.applied;
    sceGeListEnQueuePatched(list, NULL, -1, NULL);
    require(inset.applied == applied, "nearest-filtered draw untouched");
    sceGeDrawSyncPatched(0);

    /* The game rewrites a word before completion: its value is kept. */
    inset_build_list(list, 1);
    sceGeListEnQueuePatched(list, NULL, -1, NULL);
    require(list[2] == command(GE_CMD_VERTEXTYPE, GP_INSET_VTYPE), "redirected again");
    list[2] = command(GE_CMD_VERTEXTYPE, 0x80011Eu);
    {
        unsigned overwritten = inset.overwritten;
        sceGeDrawSyncPatched(0);
        require(list[2] == command(GE_CMD_VERTEXTYPE, 0x80011Eu) && inset.overwritten == overwritten + 1,
                "completion never overwrites a word the game rewrote");
        require(list[0] == command(GE_CMD_BASE, 0x080000) && list[1] == command(GE_CMD_VADDR, 0x820000),
                "the other words of that draw are still restored");
    }
    memcpy(h, saved, sizeof(saved));

    /* A draw continuing the inset draw's vertex pointer (NULL vertices) joins
     * the copy right after it; the third draw sets its own words: commit. */
    {
        unsigned n = 0;
        InsetHud *chain = (InsetHud *)0x08820000u;
        inset_set_vertices(chain); inset_set_vertices(chain + 2);
        chain[2].u = 64; chain[2].v = 0; chain[3].u = 80; chain[3].v = 16;
        list[n++] = command(GE_CMD_BASE, 0x080000);
        list[n++] = command(GE_CMD_VADDR, 0x820000);
        list[n++] = command(GE_CMD_VERTEXTYPE, 0x80019Eu);
        list[n++] = command(GE_CMD_TEXTUREMAPENABLE, 1);
        list[n++] = command(GE_CMD_TEXFILTER, 0x101);
        list[n++] = command(GE_CMD_TEXMODE, 0);
        list[n++] = command(GE_CMD_TEXSIZE0, 0x0808);
        list[n++] = command(GE_CMD_PRIM, 6u << 16 | 2);
        list[n++] = command(GE_CMD_PRIM, 6u << 16 | 2);   /* inherits VADDR/VTYPE */
        list[n++] = command(GE_CMD_BASE, 0x080000);
        list[n++] = command(GE_CMD_VADDR, 0x830000);
        list[n++] = command(GE_CMD_VERTEXTYPE, 0x80019Eu);
        list[n++] = command(GE_CMD_PRIM, 6u << 16 | 2);
        list[n++] = command(GE_CMD_FINISH, 0);
        list[n++] = command(GE_CMD_END, 0);
        applied = inset.applied;
        sceGeListEnQueuePatched(list, NULL, -1, NULL);
        require(inset.applied == applied + 1 && list[2] == command(GE_CMD_VERTEXTYPE, GP_INSET_VTYPE),
                "chained draw: the first draw is redirected");
        require(inset.used_vertices == 4, "the continuing draw's vertices follow in the copy");
        inset_require_close(pool[2].u, 64.5f, "continuing draw inset too");
        inset_require_close(pool[3].v, 15.5f, "continuing draw inset too (V)");
        sceGeDrawSyncPatched(0);
    }

    /* A run sharing one VTYPE word (Tekken's tiles): every draw redirected to
     * its own copy, committed when a draw sets all its words again. */
    {
        unsigned k = 0, words_before = inset.word_count;
        InsetHud *tile = (InsetHud *)0x08820000u, *tile2 = (InsetHud *)0x08830000u;
        inset_set_vertices(tile); inset_set_vertices(tile2);
        list[k++] = command(GE_CMD_BASE, 0x080000);
        list[k++] = command(GE_CMD_VADDR, 0x820000);
        list[k++] = command(GE_CMD_VERTEXTYPE, 0x80019Eu);
        list[k++] = command(GE_CMD_TEXTUREMAPENABLE, 1);
        list[k++] = command(GE_CMD_TEXFILTER, 0x101);
        list[k++] = command(GE_CMD_TEXMODE, 0);
        list[k++] = command(GE_CMD_TEXSIZE0, 0x0808);
        list[k++] = command(GE_CMD_PRIM, 6u << 16 | 2);           /* 7 */
        list[k++] = command(GE_CMD_BASE, 0x080000);               /* 8 */
        list[k++] = command(GE_CMD_VADDR, 0x830000);              /* 9 */
        list[k++] = command(GE_CMD_PRIM, 6u << 16 | 2);           /* same VTYPE */
        list[k++] = command(GE_CMD_BASE, 0x080000);
        list[k++] = command(GE_CMD_VADDR, 0x820000);
        list[k++] = command(GE_CMD_VERTEXTYPE, 0x80019Eu);
        list[k++] = command(GE_CMD_PRIM, 6u << 16 | 2);
        list[k++] = command(GE_CMD_FINISH, 0);
        list[k++] = command(GE_CMD_END, 0);
        applied = inset.applied;
        sceGeListEnQueuePatched(list, NULL, -1, NULL);
        require(inset.applied == applied + 1 && inset.word_count == words_before + 5u,
                "run: VTYPE once, BASE/VADDR of both draws");
        require(list[2] == command(GE_CMD_VERTEXTYPE, GP_INSET_VTYPE), "run's shared VTYPE word redirected");
        require((((list[8] << 8) & 0x0F000000u) | (list[9] & 0xFFFFFFu)) ==
                    (((uint32_t)(uintptr_t)pool & 0x0FFFFFFFu) + 2u * sizeof(GpInsetVertex)),
                "second draw of the run reads its own copy");
        sceGeDrawSyncPatched(0);
        require(list[8] == command(GE_CMD_BASE, 0x080000) && list[9] == command(GE_CMD_VADDR, 0x830000),
                "run restored at completion");
    }

    /* One picture in four tiles: only the picture's outer edges are clamped. */
    {
        GpInsetVertex tiles[8];
        GpInsetSource src;
        GpInsetVertex *first;
        typedef struct { uint16_t u, v; uint32_t color; float x, y, z; } Tile;
        Tile *t = (Tile *)0x08840000u;
        unsigned k;
        const int ux[4][4] = {{0, 0, 64, 32}, {64, 0, 128, 32}, {0, 32, 64, 64}, {64, 32, 128, 64}};
        for (k = 0; k < 4; ++k) {
            t[2 * k].u = (uint16_t)ux[k][0]; t[2 * k].v = (uint16_t)ux[k][1];
            t[2 * k + 1].u = (uint16_t)ux[k][2]; t[2 * k + 1].v = (uint16_t)ux[k][3];
            t[2 * k].x = (float)ux[k][0] * 2; t[2 * k].y = (float)ux[k][1] * 2;
            t[2 * k + 1].x = (float)ux[k][2] * 2; t[2 * k + 1].y = (float)ux[k][3] * 2;
            t[2 * k].color = t[2 * k + 1].color = 0xFFFFFFFFu; t[2 * k].z = t[2 * k + 1].z = 0;
        }
        src.type = 0x80019Eu; src.primitive = 6u << 16 | 8u; src.vertices = 0x08840000u;
        src.stride = 20; src.position = 8;
        gp_inset_init(&inset, pool, (RENDER_LIST - 0x20000 - VERTICES_BUFFER) / sizeof(GpInsetVertex));
        gp_inset_set_tiles(&inset, (GpInsetTiles *)(RENDER_LIST - 0x20000));
        src.texture = 0x08123400u;
        require(gp_inset_build(&inset, &src, &first, 1) == 8u, "tiled sprites build");
        inset_require_close(first[1].u, 63.5f, "before joining every tile edge is clamped");
        gp_inset_join_tiles(&inset);
        memcpy(tiles, first, sizeof(tiles));
        inset_require_close(tiles[0].u, 0.5f, "outer left edge clamped");
        inset_require_close(tiles[1].u, 64.0f, "inner join (left tile right edge) not clamped");
        inset_require_close(tiles[1].v, 32.0f, "inner join (top tile bottom edge) not clamped");
        inset_require_close(tiles[3].u, 127.5f, "outer right edge clamped");
        inset_require_close(tiles[7].v, 63.5f, "outer bottom edge clamped");
        inset_require_close(tiles[6].v, 32.0f, "inner join (bottom tile top edge) not clamped");
        gp_inset_complete(&inset, NULL);

        /* The same picture as two separate draws (Tekken's tiles): joined too,
         * but not with a tile of another texture. */
        src.primitive = 6u << 16 | 2u;
        require(gp_inset_build(&inset, &src, &first, 1) == 2u, "left tile alone");
        src.vertices = 0x08840000u + 20u * 2u;
        {
            GpInsetVertex *second;
            GpInsetSource other = src;
            other.texture = 0x08999900u;
            require(gp_inset_build(&inset, &src, &second, 1) == 2u, "right tile, separate draw");
            gp_inset_join_tiles(&inset);
            inset_require_close(first[1].u, 64.0f, "join across draws: left tile's right edge free");
            inset_require_close(second[0].u, 64.0f, "join across draws: right tile's left edge free");
            inset_require_close(first[0].u, 0.5f, "outer edge still clamped");
            gp_inset_complete(&inset, NULL);
            src.vertices = 0x08840000u;
            require(gp_inset_build(&inset, &src, &first, 1) == 2u, "left tile again");
            other.vertices = 0x08840000u + 40u;
            require(gp_inset_build(&inset, &other, &second, 1) == 2u, "right tile from another texture");
            gp_inset_join_tiles(&inset);
            inset_require_close(first[1].u, 63.5f, "another texture: no join");
            gp_inset_complete(&inset, NULL);
        }
        gp_inset_set_tiles(&inset, NULL);
    }
    puts("PASS: UV inset clamp formula; redirect via own BASE/VADDR/VTYPE only; reuse/restore; inherited state, list end, shared BASE, nearest and rewritten words left alone");
}
