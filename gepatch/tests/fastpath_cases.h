/* The 3D scene's short path vs the full parse: random lists of what Tekken's
 * fight is made of -- sublists CALLed (some again), BASE/VADDR/IADDR/VTYPE,
 * 3D and 2D draws, bounding boxes, ORIGIN/OFFSETADDR, NOPs, textures (some
 * from a frame buffer: dropped draws) and plain registers -- must leave the
 * same words, vertices, counts and parse state. */
static uint32_t hot_seed = 777u;
static int hot_no_2d; /* every other round: no 2D (an inset's words cover the list and send BASE etc. the long way) */
static uint32_t *hot_bone; /* a bone matrix sublist: 12 BONEMATRIXDATA and RET, stepped over */
static uint32_t hotRandom(uint32_t n) {
    hot_seed = hot_seed * 1103515245u + 12345u;
    return (hot_seed >> 12) % n;
}
static unsigned hotEmit(uint32_t *l, unsigned n, const uint32_t *verts, int in_sublist) {
    const uint32_t v = (uint32_t)(uintptr_t)verts;
    switch (hotRandom(18)) {
        case 0: l[n++] = command(GE_CMD_BASE, (v >> 8) & 0x0f0000); break;
        case 1: l[n++] = command(GE_CMD_VADDR, (v + hotRandom(64) * 4u) & 0xffffff); break;
        case 2: l[n++] = command(GE_CMD_IADDR, (v + 256u + hotRandom(16) * 2u) & 0xffffff); break;
        case 3: { /* 3D (s16 position, sometimes indexed) or, rarely, through */
            const uint32_t through = !hot_no_2d && hotRandom(5) == 0 ? GE_VTYPE_THROUGH : 0u;
            const uint32_t index = hotRandom(3) == 0 ? (1u << 11) : 0u;
            const uint32_t color = hotRandom(2) ? (7u << 2) : 0u; /* (two sizes: the size of the drawn ones counts) */
            l[n++] = command(GE_CMD_VERTEXTYPE, through | index | color | (2u << 7) | (1u << 0)); break;
        }
        case 4: case 5: l[n++] = command(GE_CMD_PRIM, ((3u + hotRandom(4)) << 16) | (2u + hotRandom(6))); break;
        case 6: l[n++] = command(GE_CMD_BOUNDINGBOX, 8); break;
        case 7: l[n++] = command(GE_CMD_BJUMP, (uint32_t)(uintptr_t)(l + n + 1) & 0xffffff); break;
        case 8: l[n++] = command(GE_CMD_NOP, 0); break;
        case 9: l[n++] = command(GE_CMD_OFFSETADDR, 0); break;
        case 10: /* a texture, now and then the frame buffer: draws dropped */
            l[n++] = command(GE_CMD_TEXADDR0, hotRandom(3) == 0 ? 0x044000u : 0x100000u);
            l[n++] = command(GE_CMD_TEXBUFWIDTH0, (hotRandom(3) == 0 ? 0x0A0000u : 0x090000u) | 256u);
            break;
        case 11: l[n++] = command(GE_CMD_TEXTUREMAPENABLE, hotRandom(2)); break;
        case 12: l[n++] = command(GE_CMD_TEXLEVEL, GE_TEXLEVEL_MODE_AUTO); break;
        case 13: { /* a bone matrix: a run of 12 */
            unsigned k;
            l[n++] = command(GE_CMD_BONEMATRIXNUMBER, 0);
            for (k = 0; k < 12u; ++k) l[n++] = command(GE_CMD_BONEMATRIXDATA, hotRandom(0x1000000));
            break;
        }
        case 14: l[n++] = command(GE_CMD_CLEARMODE, hotRandom(3) == 0 ? 0x401u : 0u); break;
        case 15: { /* a bone matrix CALLed */
            const uint32_t to = (uint32_t)(uintptr_t)hot_bone;
            l[n++] = command(GE_CMD_BASE, (to >> 8) & 0x0f0000);
            l[n++] = command(GE_CMD_CALL, to & 0xffffff);
            break;
        }
        case 16: l[n++] = command(GE_CMD_STENCILTESTENABLE, hotRandom(2)); break;
        default: l[n++] = command(GE_CMD_TEXFORMAT, hotRandom(4)); break;
    }
    (void)in_sublist;
    return n;
}
static void run_hotpath_tests(void) {
    uint32_t *const area = (uint32_t *)0x08200000u, *const verts = (uint32_t *)0x08500000u; /* above 0x08400000: user RAM */
    static uint32_t original[0x8000], fast_words[0x8000], vert_original[1024], fast_verts[1024];
    static GeState fast_state;
    u32 fast_stencil = 0, *fast_tbw0 = NULL, *fast_tex0 = NULL;
    u32 fast_counts[4], full_counts[4];
    unsigned round, i, pass, dropped_total = 0, through_total = 0;
    hot_bone = area + 0x3000u;
    for (i = 0; i < 12u; ++i) hot_bone[i] = command(GE_CMD_BONEMATRIXDATA, i);
    hot_bone[12] = command(GE_CMD_RET, 0);
    for (round = 0; round < 120u; ++round) {
        uint32_t *subs[6];
        uint32_t *main_list = area;
        unsigned n = 0, s, k;
        hot_no_2d = round & 1u;
        for (i = 0; i < 1024u; ++i) verts[i] = (uint32_t)(i * 2654435761u) & 0x00ff00ffu; /* small positions */
        for (s = 0; s < 6u; ++s) { /* sublists, each ending in RET */
            unsigned m = 0;
            subs[s] = area + 0x1000u + s * 0x200u;
            for (k = 0; k < 3u + hotRandom(20); ++k) m = hotEmit(subs[s], m, verts, 1);
            subs[s][m++] = command(GE_CMD_RET, 0);
        }
        main_list[n++] = command(GE_CMD_FRAMEBUFPTR, 0x044000); /* a second frame buffer on record */
        main_list[n++] = command(GE_CMD_FRAMEBUFWIDTH, 0x040000 | 512);
        main_list[n++] = command(GE_CMD_FRAMEBUFPTR, 0x000000);
        main_list[n++] = command(GE_CMD_FRAMEBUFWIDTH, 0x040000 | 512);
        for (k = 0; k < 200u; ++k) {
            if (hotRandom(4) == 0) {
                const uint32_t to = (uint32_t)(uintptr_t)subs[hotRandom(6)];
                main_list[n++] = command(GE_CMD_BASE, (to >> 8) & 0x0f0000);
                main_list[n++] = command(GE_CMD_CALL, to & 0xffffff);
            } else n = hotEmit(main_list, n, verts, 0);
        }
        /* a fixed tail: the last values of these are the short path's own */
        main_list[n++] = command(GE_CMD_BASE, ((uint32_t)(uintptr_t)verts >> 8) & 0x0f0000);
        main_list[n++] = command(GE_CMD_VADDR, (uint32_t)(uintptr_t)verts & 0xffffff);
        main_list[n++] = command(GE_CMD_IADDR, ((uint32_t)(uintptr_t)verts + 256u) & 0xffffff);
        main_list[n++] = command(GE_CMD_VERTEXTYPE, (2u << 7) | 1u);
        main_list[n++] = command(GE_CMD_FINISH, 0);
        main_list[n++] = command(GE_CMD_END, 0);
        memcpy(original, area, sizeof(original));
        memcpy(vert_original, verts, sizeof(vert_original));

        force_full_parse = 0;
        /* twice: the first records the sublists, the second takes them from the records
           (and the first, within its frame, the sublists CALLed again) */
        for (pass = 0; pass < 2u; ++pass) {
            fast_counts[0] = perf_3d_prims; fast_counts[1] = frame_draws; fast_counts[2] = perf_dropped_tex; fast_counts[3] = perf_2d_prims;
            stencil_frames_left = 0; tbw0_word = tex0_word = NULL;
            resetGeState(); patchGeList(main_list, NULL);
            fast_stencil = stencil_frames_left; fast_tbw0 = tbw0_word; fast_tex0 = tex0_word;
            fast_counts[0] = perf_3d_prims - fast_counts[0]; fast_counts[1] = frame_draws - fast_counts[1];
            fast_counts[2] = perf_dropped_tex - fast_counts[2]; fast_counts[3] = perf_2d_prims - fast_counts[3];
            memcpy(fast_words, area, sizeof(fast_words));
            memcpy(fast_verts, verts, sizeof(fast_verts));
            fast_state = state;
            sync_result = 0; sceGeDrawSyncPatched(0);
            /* (FRAMEBUFPTR is rewritten for good, as upstream: every run from the game's words) */
            memcpy(area, original, sizeof(original));
            memcpy(verts, vert_original, sizeof(vert_original));
        }

        force_full_parse = 1;
        full_counts[0] = perf_3d_prims; full_counts[1] = frame_draws; full_counts[2] = perf_dropped_tex; full_counts[3] = perf_2d_prims;
        stencil_frames_left = 0; tbw0_word = tex0_word = NULL;
        resetGeState(); patchGeList(main_list, NULL);
        require(fast_stencil == stencil_frames_left && fast_tbw0 == tbw0_word && fast_tex0 == tex0_word,
                "short path keeps the stencil hold and the texture words the same");
        full_counts[0] = perf_3d_prims - full_counts[0]; full_counts[1] = frame_draws - full_counts[1];
        full_counts[2] = perf_dropped_tex - full_counts[2]; full_counts[3] = perf_2d_prims - full_counts[3];
        force_full_parse = 0;
        require(!memcmp(fast_words, area, sizeof(fast_words)), "short path patches the same words");
        require(!memcmp(fast_verts, verts, sizeof(fast_verts)), "short path scales the same vertices");
        require(!memcmp(fast_counts, full_counts, sizeof(fast_counts)), "short path counts the same draws");
        dropped_total += fast_counts[2]; through_total += fast_counts[3];
        /* registers nothing reads may stay unrecorded on the short path */
        for (i = 0; i < 256u; ++i)
            if (!(cmd_class[i] & CMD_KEEP)) fast_state.ge_cmd[i] = state.ge_cmd[i] = 0;
        /* the stack above its depth is what was pushed and popped: a recorded
           sublist applied is not pushed at all */
        for (i = 0; i < sizeof(state.stack) / sizeof(StackEntry); ++i) {
            if (i >= state.curr_stack) {
                memset(&fast_state.stack[i], 0, sizeof(StackEntry));
                memset(&state.stack[i], 0, sizeof(StackEntry));
            }
            fast_state.stack[i].commands = state.stack[i].commands = 0;
            fast_state.stack[i].writes = state.stack[i].writes = 0;
            fast_state.stack[i].draws_2d = state.stack[i].draws_2d = 0;
        }
        fast_state.address = state.address = 0; /* (a CALL's scratch) */
        require(!memcmp(&fast_state, &state, sizeof(state)), "short path leaves the same parse state");
        sync_result = 0; sceGeDrawSyncPatched(0);
    }
    require(dropped_total && through_total, "the random lists had dropped and 2D draws");
    frame_3d_draw = frame_movie_draw = frame_draws = 0; /* no 3D frame left for the movie tests */
    puts("PASS: 3D scene short path equals the full parse on 120 random lists with sublists, draws, drops");
}

/* The main list straightened (linearizeList): what a GE runs of it -- the
 * words but BASE and the control flow, the sublists' too -- the same in the
 * copy as in a list of chunks JUMPing back and forth, with its BJUMPs taken
 * and not; and the lists it cannot copy refused. */
static unsigned geRun(const u32 *list, int take_bjumps, u32 *seq, unsigned max) {
    const u32 *stack[8];
    u32 stack_offset[8]; /* (a CALL keeps the offset, its RET puts it back: as the GE) */
    unsigned depth = 0, n = 0, steps = 0;
    u32 base = 0, offset = 0;
    const u32 *pc = list;
    while (steps++ < 100000u && n < max) {
        const u32 w = *pc, cmd = w >> 24, data = w & 0xffffffu;
        const u32 *to = (const u32 *)(uintptr_t)(((base | data) + offset) & 0x0ffffffcu);
        switch (cmd) {
            case GE_CMD_BASE: base = (data << 8) & 0x0f000000u; ++pc; continue;
            case GE_CMD_OFFSETADDR: offset = data << 8; ++pc; continue;
            case GE_CMD_JUMP: pc = to; continue;
            case GE_CMD_BJUMP: pc = take_bjumps ? to : pc + 1; continue;
            case GE_CMD_CALL:
                if (depth == 2u) return 0; /* (the GE nests two CALLs, a third refused: uOFW ge.c) */
                stack_offset[depth] = offset; stack[depth++] = pc + 1;
                pc = to;
                continue;
            case GE_CMD_RET:
                if (depth) { --depth; pc = stack[depth]; offset = stack_offset[depth]; continue; }
                ++pc;
                continue;
            case GE_CMD_NOP: ++pc; continue; /* (does nothing: ours, before a stall) */
            case GE_CMD_END: return n;
            default: seq[n++] = w; ++pc; continue;
        }
    }
    return n;
}
static void run_linear_tests(void) {
    uint32_t *const a = (uint32_t *)0x08781000u, *const b = (uint32_t *)0x08780000u; /* b below a */
    uint32_t *const c = (uint32_t *)0x0A180000u, *const sub = (uint32_t *)0x08782000u; /* c: "VRAM" */
    static u32 seq0[512], seq1[512];
    unsigned n, k, take, len0, len1, words;
    u32 marker = 0x100;
#define BASE_OF(p) command(GE_CMD_BASE, ((uint32_t)(uintptr_t)(p) >> 8) & 0x0f0000)
#define LO(p) ((uint32_t)(uintptr_t)(p) & 0xffffff)
    n = 0;
    sub[n++] = command(GE_CMD_TEXFORMAT, 1); sub[n++] = command(GE_CMD_TEXFUNC, 2); sub[n++] = command(GE_CMD_RET, 0);
    n = 0; /* a: start, a CALL, a BJUMP over two words, then on to b (below) */
    a[n++] = BASE_OF(a);
    a[n++] = command(GE_CMD_TEXWRAP, marker++);
    a[n++] = BASE_OF(sub); a[n++] = command(GE_CMD_CALL, LO(sub));
    a[n++] = BASE_OF(a);
    a[n++] = command(GE_CMD_BOUNDINGBOX, 4);
    a[n++] = command(GE_CMD_BJUMP, LO(a + n + 3));
    a[n++] = command(GE_CMD_CALL, LO(sub)); /* (BASE: the game's, after the BJUMP not taken) */
    a[n++] = command(GE_CMD_TEXWRAP, marker++);
    a[n++] = command(GE_CMD_TEXWRAP, marker++); /* the landing */
    a[n++] = BASE_OF(b); a[n++] = command(GE_CMD_JUMP, LO(b));
    n = 0; /* b: words, into c ("VRAM", above), then back to a's end */
    b[n++] = command(GE_CMD_TEXWRAP, marker++);
    b[n++] = BASE_OF(c); b[n++] = command(GE_CMD_JUMP, LO(c));
    n = 0;
    c[n++] = command(GE_CMD_TEXWRAP, marker++);
    c[n++] = BASE_OF(sub); c[n++] = command(GE_CMD_CALL, LO(sub));
    c[n++] = BASE_OF(a); c[n++] = command(GE_CMD_JUMP, LO(a + 0x100));
    n = 0x100;
    a[n++] = command(GE_CMD_TEXWRAP, marker++);
    a[n++] = command(GE_CMD_FINISH, 0);
    a[n++] = command(GE_CMD_END, 0);
    words = linearizeList(a);
    require(words > 10u, "a list of chunks JUMPing back and forth straightened");
    for (take = 0; take < 2u; ++take) {
        len0 = geRun(a, (int)take, seq0, 512);
        len1 = geRun((const u32 *)LINEAR_LIST, (int)take, seq1, 512);
        require(len0 && len0 == len1 && !memcmp(seq0, seq1, len0 * 4u),
                take ? "the copy runs as the list, its BJUMPs taken" : "the copy runs as the list, its BJUMPs not taken");
    }
    {   /* more BJUMPs than slots, each landed before the next */
        uint32_t *const d = (uint32_t *)0x08784000u;
        n = 0;
        d[n++] = BASE_OF(d);
        for (k = 0; k < 40u; ++k) {
            d[n] = command(GE_CMD_BJUMP, LO(d + n + 2)); ++n;
            d[n++] = command(GE_CMD_TEXWRAP, marker++);
            d[n++] = command(GE_CMD_TEXFUNC, k);
        }
        d[n++] = command(GE_CMD_FINISH, 0);
        d[n++] = command(GE_CMD_END, 0);
        require(linearizeList(d) > 120u, "40 BJUMPs, one after another: copied");
        for (take = 0; take < 2u; ++take) {
            len0 = geRun(d, (int)take, seq0, 512);
            len1 = geRun((const u32 *)LINEAR_LIST, (int)take, seq1, 512);
            require(len0 && len0 == len1 && !memcmp(seq0, seq1, len0 * 4u), "and run as the list");
        }
    }
    {   /* what it leaves where it is */
        const u32 keep = a[1];
        a[1] = command(GE_CMD_ORIGIN, 0);
        require(!linearizeList(a), "an ORIGIN in the main list: not copied");
        a[1] = keep;
        k = 6; /* the BJUMP: back now */
        a[k] = command(GE_CMD_BJUMP, LO(a));
        require(!linearizeList(a), "a BJUMP back: not copied");
        a[k] = command(GE_CMD_BJUMP, LO(a + k + 3));
        a[k + 1] = BASE_OF(c); /* a BASE in what it skips, one set again after the landing */
        require(linearizeList(a) > 10u, "a BASE in what a BJUMP skips, set again where it lands: copied");
        for (take = 0; take < 2u; ++take) {
            len0 = geRun(a, (int)take, seq0, 512);
            len1 = geRun((const u32 *)LINEAR_LIST, (int)take, seq1, 512);
            require(len0 && len0 == len1 && !memcmp(seq0, seq1, len0 * 4u), "and runs as the list");
        }
        {
            const u32 landing = a[k + 3];
            a[k + 3] = command(GE_CMD_VADDR, 0x1000); /* used before the BASE is set again */
            require(!linearizeList(a), "a BASE in what a BJUMP skips, used where it lands: not copied");
            a[k + 3] = landing;
        }
        a[k + 1] = command(GE_CMD_CALL, LO(sub));
        require(linearizeList(a) == words, "and copied again as it was");
    }
#undef BASE_OF
#undef LO
    puts("PASS: main lists of JUMPing chunks straightened, run as they were");
}

/* A list released to the GE while it is parsed: every stall the GE is given
 * lies in the main list, with all that came before it (the sublists it
 * CALLed) patched, never between a BJUMP and where it lands, and at the end
 * the stall is past the END. */
static uint32_t *rel_main, *rel_subs[20];
static unsigned rel_calls, rel_bjump_at, rel_bjump_to, rel_end, rel_bad, rel_last;
/* a stall in the straightened copy: every sublist CALLed before it patched,
   not between a BJUMP and where it lands */
static void releaseCheck(const u32 *stall) {
    const u32 *const copy = (const u32 *)LINEAR_LIST, *q;
    u32 base = 0;
    rel_last = stall ? (unsigned)(stall - copy) : ~0u;
    if (!stall) return; /* the game's own: no stall */
    if (stall < copy || stall >= copy + LINEAR_WORDS) { ++rel_bad; return; }
    for (q = copy; q < stall; ++q) {
        const u32 cmd = *q >> 24, data = *q & 0xffffffu;
        if (!*q) ++rel_bad; /* (a BJUMP's BASE or BJUMP not set yet) */
        if (cmd == GE_CMD_BASE) base = (data << 8) & 0x0f000000u;
        else if (cmd == GE_CMD_CALL) {
            const u32 *to = (const u32 *)(uintptr_t)((base | data) & 0x0ffffffcu);
            unsigned k;
            for (k = 0; k < rel_calls; ++k)
                if (to == rel_subs[k] && (rel_subs[k][0] & 0xffffu) != 960u) ++rel_bad;
        } else if (cmd == GE_CMD_BJUMP) {
            const u32 *to = (const u32 *)(uintptr_t)((base | data) & 0x0ffffffcu);
            if (stall > q && stall < to) ++rel_bad;
        }
    }
}
static unsigned part2_count;
static void part2Check(const u32 *stall) { if (stall) { releaseCheck(stall); ++part2_count; } }
static void run_release_tests_with(u32 chunk);
static void run_late_tests(void);
static void run_release_tests(void) {
    static u32 whole[512], whole_n;
    run_release_tests_with(0x100000u);
    whole_n = lin.n;
    memcpy(whole, (const void *)LINEAR_LIST, sizeof(whole));
    run_release_tests_with(7u);
    require(lin.n == whole_n && !memcmp(whole, (const void *)LINEAR_LIST, whole_n * 4u),
            "the copy made 7 words at a time as the parse went: the same as made whole");
    run_release_tests_with(1u);
    require(lin.n == whole_n && !memcmp(whole, (const void *)LINEAR_LIST, whole_n * 4u),
            "and a word at a time");
    linear_chunk = 1024u;
    run_late_tests();
}
static void run_release_tests_with(u32 chunk) {
    unsigned k, i, n = 0, updates;
    group_enabled = 0; /* (the straightened copy's tests; the groups': run_group_tests) */
    linear_chunk = chunk;
    rel_main = (uint32_t *)0x08600000u;
    rel_calls = 20;
    for (k = 0; k < rel_calls; ++k) {
        uint32_t *sub = rel_subs[k] = (uint32_t *)(0x08610000u + k * 0x1000u);
        const uint32_t to = (uint32_t)(uintptr_t)sub;
        sub[0] = command(GE_CMD_FRAMEBUFWIDTH, 0x040000 | 512); /* patched to 960 */
        for (i = 1; i < 300u; ++i) sub[i] = command(GE_CMD_TEXFORMAT, i & 3u);
        sub[300] = command(GE_CMD_RET, 0);
        rel_main[n++] = command(GE_CMD_BASE, (to >> 8) & 0x0f0000);
        rel_main[n++] = command(GE_CMD_CALL, to & 0xffffff);
        if (k == 10u) { /* a BJUMP over the next two CALLs */
            rel_bjump_at = n;
            rel_bjump_to = n + 1u + 6u;
            rel_main[n++] = command(GE_CMD_BJUMP, (uint32_t)(uintptr_t)(rel_main + rel_bjump_to) & 0xffffff);
        } else rel_main[n++] = command(GE_CMD_TEXFORMAT, 0);
    }
    rel_main[n++] = command(GE_CMD_FINISH, 0);
    rel_end = n;
    rel_main[n++] = command(GE_CMD_END, 0);
    release_enabled = 1; /* (off in the game: see main.c) */
    release_every = 500u;
    stall_updates = rel_bad = 0;
    stall_check = releaseCheck;
    sceGeListEnQueuePatched(rel_main, NULL, -1, NULL);
    stall_check = NULL;
    updates = stall_updates;
    require(rel_last == ~0u, "released while parsed: then the game's own stall back (none)");
    require(updates > 5u && !rel_bad, "released while parsed: stalls in the copy, after what is patched, not over a BJUMP");
    sync_result = 0; sceGeDrawSyncPatched(0);
    {   /* Tekken's main list: a BJUMP, its target passed, then a JUMP on to
           the rest of the list elsewhere -- released there too */
        uint32_t *const part2 = rel_main + 0x800;
        unsigned m = 0, in_part2 = 0;
        const u32 before = stall_updates;
        n = 0;
        rel_main[n++] = command(GE_CMD_BASE, ((uint32_t)(uintptr_t)rel_main >> 8) & 0x0f0000);
        rel_main[n++] = command(GE_CMD_BJUMP, (uint32_t)(uintptr_t)(rel_main + 3) & 0xffffff);
        rel_main[n++] = command(GE_CMD_TEXFORMAT, 0);
        rel_main[n++] = command(GE_CMD_TEXFORMAT, 1); /* the BJUMP's target, passed */
        rel_main[n++] = command(GE_CMD_JUMP, (uint32_t)(uintptr_t)part2 & 0xffffff);
        for (k = 0; k < rel_calls; ++k) {
            const uint32_t to = (uint32_t)(uintptr_t)rel_subs[k];
            rel_subs[k][0] = command(GE_CMD_FRAMEBUFWIDTH, 0x040000 | 512);
            part2[m++] = command(GE_CMD_BASE, (to >> 8) & 0x0f0000);
            part2[m++] = command(GE_CMD_CALL, to & 0xffffff);
        }
        part2[m++] = command(GE_CMD_FINISH, 0);
        part2[m++] = command(GE_CMD_END, 0);
        release_every = 500u;
        stall_check = part2Check;
        part2_count = 0;
        rel_bad = 0;
        sceGeListEnQueuePatched(rel_main, NULL, -1, NULL);
        stall_check = NULL;
        in_part2 = part2_count;
        require(stall_updates > before + 2u && in_part2 > 2u && !rel_bad, "released after a BJUMP whose target was passed, and a JUMP");
        sync_result = 0; sceGeDrawSyncPatched(0);
    }
    release_every = 4096u;
    release_enabled = 0;
    group_enabled = 1;
    printf("PASS: a list released to the GE while parsed, stall by stall (copied %u words at a time)\n", (unsigned)chunk);
}
/* A list that turns out not to copy after its first chunk (an ORIGIN late in
 * it): the copy JUMPs to it where it is, a BJUMP waiting there lands in it;
 * the GE runs the same, the sublists after are parsed, nothing released after. */
static unsigned late_stalls_after;
static u32 late_jump_at;
static void lateCheck(const u32 *stall) {
    if (!stall) return;
    if (stall > (const u32 *)LINEAR_LIST + late_jump_at) ++late_stalls_after;
    releaseCheck(stall);
}
static void run_late_tests(void) {
    uint32_t *const m = (uint32_t *)0x08640000u;
    static u32 seq0[16384], seq1[16384];
    unsigned k, n, take, len0, len1, with_bjump;
    group_enabled = 0;
    for (with_bjump = 0; with_bjump < 2u; ++with_bjump) {
        const u32 late_before = linear_late;
        n = 0;
        for (k = 0; k < rel_calls; ++k) rel_subs[k][0] = command(GE_CMD_FRAMEBUFWIDTH, 0x040000 | 512);
        m[n++] = command(GE_CMD_BASE, ((uint32_t)(uintptr_t)m >> 8) & 0x0f0000);
        for (k = 0; k < 10u; ++k) { /* sublists, in the copy */
            const uint32_t to = (uint32_t)(uintptr_t)rel_subs[k];
            m[n++] = command(GE_CMD_BASE, (to >> 8) & 0x0f0000);
            m[n++] = command(GE_CMD_CALL, to & 0xffffff);
            m[n++] = command(GE_CMD_TEXFORMAT, k & 3u);
        }
        m[n++] = command(GE_CMD_BASE, ((uint32_t)(uintptr_t)m >> 8) & 0x0f0000);
        if (with_bjump) { m[n] = command(GE_CMD_BJUMP, (uint32_t)(uintptr_t)(m + n + 4) & 0xffffff); ++n; }
        m[n++] = command(GE_CMD_TEXWRAP, 1);
        m[n++] = command(GE_CMD_ORIGIN, 0); /* not to copy: from here where it is */
        m[n++] = command(GE_CMD_OFFSETADDR, 0); /* (the addresses after it absolute again) */
        m[n++] = command(GE_CMD_TEXWRAP, 2);
        for (k = 10; k < rel_calls; ++k) { /* sublists after: parsed there */
            const uint32_t to = (uint32_t)(uintptr_t)rel_subs[k];
            m[n++] = command(GE_CMD_BASE, (to >> 8) & 0x0f0000);
            m[n++] = command(GE_CMD_CALL, to & 0xffffff);
            m[n++] = command(GE_CMD_TEXFORMAT, k & 3u);
        }
        m[n++] = command(GE_CMD_FINISH, 0);
        m[n++] = command(GE_CMD_END, 0);
        require(!linearizeList(m), "a late ORIGIN: the list as a whole not copied");
        release_enabled = 1;
        release_every = 4u;
        linear_chunk = 7u;
        late_stalls_after = 0;
        late_jump_at = 0x7fffffffu;
        rel_bad = 0;
        stall_check = lateCheck;
        sceGeListEnQueuePatched(m, NULL, -1, NULL);
        stall_check = NULL;
        late_jump_at = lin.n;
        require(linear_late == late_before + 1u, "copied a chunk at a time: at the ORIGIN the copy goes on where the list is");
        for (k = 0; k < rel_calls; ++k)
            require((rel_subs[k][0] & 0xffffu) == 960u, "the sublists CALLed before and after it parsed");
        require(!rel_bad && !late_stalls_after, "released only within the copy, before its JUMP away");
        for (take = 0; take < 2u; ++take) {
            len0 = geRun(m, (int)take, seq0, 16384);
            len1 = geRun((const u32 *)LINEAR_LIST, (int)take, seq1, 16384);
            require(len0 > 6000u && len0 < 16384u && len0 == len1 && !memcmp(seq0, seq1, len0 * 4u),
                    with_bjump ? "the GE runs the same, a BJUMP over the ORIGIN taken or not" : "the GE runs the same");
        }
        sync_result = 0; sceGeDrawSyncPatched(0);
        release_enabled = 0;
        release_every = 4096u;
        linear_chunk = 1024u;
    }
    group_enabled = 1;
    puts("PASS: a list found not to copy after its first chunk goes on where it is");
}

/* The main list run where it is, its chunks CALLed in groups from a list of
 * ours (groupJump): the GE runs the same as the list's own JUMPs would,
 * BJUMPs taken or not (one whose landing is a chunk further on keeps those
 * chunks one group); every stall is in our list, after groups whose sublists
 * are all patched, the GE never past it; once the GE is done the list is
 * byte for byte the game's again. */
static unsigned grp_bad, grp_stalls, grp_changed;
/* what the GE ran up to the last stall (BJUMPs not taken): no word of it
 * changed by the parse after (the GE has run it) */
static u32 grp_ran[16384], grp_now[16384];
static unsigned grp_ran_len;
/* what the GE runs of our list up to `stall`: the sublists it CALLs patched */
static void groupStallCheck(const u32 *stall) {
    const u32 *stack[8], *pc = (const u32 *)LINEAR_LIST;
    u32 stack_offset[8];
    unsigned depth = 0, steps = 0, k;
    u32 base = 0, offset = 0;
    unsigned ran = 0;
    if (!stall) return;
    ++grp_stalls;
    if (stall < (const u32 *)LINEAR_LIST || stall > (const u32 *)LINEAR_LIST + 256) { ++grp_bad; return; }
    while (steps++ < 200000u) {
        const u32 w = *pc, cmd = w >> 24, data = w & 0xffffffu;
        const u32 *to = (const u32 *)(uintptr_t)(((base | data) + offset) & 0x0ffffffcu);
        if (pc == stall) { /* (the GE waits here, however it came: hardware, 30.10) */
            if (memcmp(grp_ran, grp_now, grp_ran_len * 4u)) ++grp_changed;
            memcpy(grp_ran, grp_now, ran * 4u);
            grp_ran_len = ran;
            return;
        }
        for (k = 0; k < rel_calls; ++k)
            if (pc == rel_subs[k] && (rel_subs[k][0] & 0xffffu) != 960u) ++grp_bad; /* a sublist not parsed yet */
        switch (cmd) {
            case GE_CMD_BASE: base = (data << 8) & 0x0f000000u; ++pc; continue;
            case GE_CMD_OFFSETADDR: offset = data << 8; ++pc; continue;
            case GE_CMD_JUMP: pc = to; continue;
            case GE_CMD_CALL:
                if (depth == 2u) { ++grp_bad; return; } /* (a third refused) */
                stack_offset[depth] = offset; stack[depth++] = pc + 1;
                pc = to;
                continue;
            case GE_CMD_RET: if (depth) { --depth; pc = stack[depth]; offset = stack_offset[depth]; continue; } ++grp_bad; return;
            case GE_CMD_END: ++grp_bad; return; /* (the list's end before the stall: ran past it) */
            case GE_CMD_NOP: case GE_CMD_BJUMP: ++pc; continue; /* (BJUMPs not taken, as geRun's 0) */
            default: if (ran < 16384u) grp_now[ran++] = w; ++pc; continue;
        }
    }
    ++grp_bad;
}
static void groupNormalize(u32 *seq, unsigned n) { /* (sublists' strides: ours, patched, against the game's) */
    unsigned k;
    for (k = 0; k < n; ++k)
        if (seq[k] >> 24 == GE_CMD_FRAMEBUFWIDTH || seq[k] >> 24 == GE_CMD_FRAMEBUFPTR) seq[k] &= 0xff000000u; /* (targets: ours) */
}
static void run_group_tests(void) {
    uint32_t *const a = (uint32_t *)0x08650000u, *const b = (uint32_t *)0x08660000u;
    uint32_t *const c = (uint32_t *)0x08670000u, *const d = (uint32_t *)0x08680000u;
    uint32_t *const nest = (uint32_t *)0x08690000u; /* a sublist calling a sublist */
    static uint32_t saved[4][64];
    static u32 seq0[16384], seq1[16384];
    unsigned k, n, take, frame, len0, len1, sub = 0;
    const u32 groups_before = grp_groups, merged_before = grp_merged, aborts_before = grp_aborts;
#define BASE_OF(p) command(GE_CMD_BASE, ((uint32_t)(uintptr_t)(p) >> 8) & 0x0f0000)
#define LO(p) ((uint32_t)(uintptr_t)(p) & 0xffffff)
#define CALL_SUB(arr) do { arr[n++] = BASE_OF(rel_subs[sub]); arr[n++] = command(GE_CMD_CALL, LO(rel_subs[sub])); ++sub; } while (0)
    for (k = 0; k < rel_calls; ++k) rel_subs[k][0] = command(GE_CMD_FRAMEBUFWIDTH, 0x040000 | 512);
    n = 0; /* a: a CALL, a BJUMP landing in a itself, on to c (above) */
    a[n++] = BASE_OF(a);
    CALL_SUB(a);
    a[n++] = BASE_OF(a);
    a[n] = command(GE_CMD_BJUMP, LO(a + n + 3)); ++n;
    a[n++] = command(GE_CMD_TEXWRAP, 1); a[n++] = command(GE_CMD_TEXWRAP, 2);
    a[n++] = command(GE_CMD_TEXWRAP, 3); /* (the landing) */
    CALL_SUB(a);
    a[n++] = command(GE_CMD_FRAMEBUFWIDTH, 0x0A0000 | 128); /* (its FRAMEBUFPTR in the next chunk) */
    a[n++] = BASE_OF(c); a[n++] = command(GE_CMD_JUMP, LO(c));
    n = 0; /* c: a BJUMP landing in b, the next chunk, then on to b (below) */
    c[n++] = command(GE_CMD_FRAMEBUFPTR, 0x0ce000);
    c[n++] = command(GE_CMD_FRAMEBUFPTR, 0x000000); c[n++] = command(GE_CMD_FRAMEBUFWIDTH, 0x040000 | 512); /* (ours again) */
    c[n++] = command(GE_CMD_TEXWRAP, 4);
    c[n++] = BASE_OF(b);
    c[n++] = command(GE_CMD_BJUMP, LO(b + 2));
    c[n++] = command(GE_CMD_TEXWRAP, 5);
    CALL_SUB(c);
    c[n++] = BASE_OF(b); c[n++] = command(GE_CMD_JUMP, LO(b));
    n = 0; /* b: the landing, a CALL, an offset changed, on to d */
    b[n++] = command(GE_CMD_TEXWRAP, 6); b[n++] = command(GE_CMD_TEXWRAP, 7);
    b[n++] = command(GE_CMD_TEXWRAP, 8); /* (c's BJUMP lands here) */
    CALL_SUB(b);
    b[n++] = BASE_OF(nest); b[n++] = command(GE_CMD_CALL, LO(nest)); /* (two deep: the GE's most) */
    nest[0] = BASE_OF(rel_subs[sub]); nest[1] = command(GE_CMD_CALL, LO(rel_subs[sub])); ++sub;
    nest[2] = command(GE_CMD_RET, 0);
    b[n++] = command(GE_CMD_OFFSETADDR, 0x100); /* +0x10000 from here */
    b[n++] = BASE_OF(d); b[n++] = command(GE_CMD_JUMP, LO(d) - 0x10000u);
    n = 0; /* d: the offset carried over to a CALL, the end */
    d[n++] = BASE_OF(rel_subs[sub]); d[n++] = command(GE_CMD_CALL, LO(rel_subs[sub]) - 0x10000u); ++sub;
    d[n++] = command(GE_CMD_OFFSETADDR, 0);
    d[n++] = BASE_OF(d); /* nine short BJUMPs ahead in one chunk, each passed before the next (Tekken's) */
    for (k = 0; k < 9u; ++k) { d[n] = command(GE_CMD_BJUMP, LO(d + n + 2)); ++n; d[n++] = command(GE_CMD_TEXWRAP, 10 + k); }
    CALL_SUB(d);
    d[n++] = command(GE_CMD_FINISH, 0);
    d[n++] = command(GE_CMD_END, 0);
    memcpy(saved[0], a, sizeof(saved[0])); memcpy(saved[1], b, sizeof(saved[1]));
    memcpy(saved[2], c, sizeof(saved[2])); memcpy(saved[3], d, sizeof(saved[3]));
    release_enabled = 1;
    group_enabled = 1;
    release_every = 4u;
    group_release = 1; /* (the releasing itself checked; off in the plugin) */
    grp_bad = grp_stalls = grp_changed = grp_ran_len = 0;
    stall_check = groupStallCheck;
    sceGeListEnQueuePatched(a, NULL, -1, NULL);
    stall_check = NULL;
    if (0) printf("dbg groups %u merged %u aborts %u lists %u words %u stalls %u bad %u\n", (unsigned)(grp_groups - groups_before), (unsigned)(grp_merged - merged_before), (unsigned)grp_aborts, (unsigned)grp_lists, grp_words, grp_stalls, grp_bad);
    require(grp_groups > groups_before + 1u && grp_merged > merged_before, "the main list in groups, the one over a chunk's end merged");
    require(grp_aborts == aborts_before, "groups to the end: BJUMPs' landings passed are let go");
    require(grp_stalls > 1u && !grp_bad, "released group by group: the GE never past what is parsed");
    require(!grp_changed, "no word the GE has run changed by the parse after it (a FRAMEBUFWIDTH waiting for its FRAMEBUFPTR)");
    {   /* nor after the last stall */
        const unsigned len = geRun((const u32 *)LINEAR_LIST, 0, seq1, 16384);
        require(len >= grp_ran_len && !memcmp(seq1, grp_ran, grp_ran_len * 4u), "what the GE ran, as the list ends up");
    }
    for (k = 0; k < sub; ++k) require((rel_subs[k][0] & 0xffffu) == 960u, "every sublist parsed");
    for (frame = 0; frame < 2u; ++frame) {
        if (frame) { /* the next frame's list: ours done, the GE not idle yet (the display copy after it) */
            sync_result = 1; list_sync_result = 0;
            sceGeListEnQueuePatched(a, NULL, -1, NULL);
            sync_result = 0;
            require(grp_aborts == aborts_before, "the next frame in groups too");
        }
        for (take = 0; take < 2u; ++take) {
            len1 = geRun((const u32 *)LINEAR_LIST, (int)take, seq1, 16384);
            groupNormalize(seq1, len1);
            /* the list as the game made it, run on its own */
            {
                static uint32_t now[4][64];
                memcpy(now[0], a, sizeof(now[0])); memcpy(now[1], b, sizeof(now[1]));
                memcpy(now[2], c, sizeof(now[2])); memcpy(now[3], d, sizeof(now[3]));
                memcpy(a, saved[0], sizeof(saved[0])); memcpy(b, saved[1], sizeof(saved[1]));
                memcpy(c, saved[2], sizeof(saved[2])); memcpy(d, saved[3], sizeof(saved[3]));
                len0 = geRun(a, (int)take, seq0, 16384);
                groupNormalize(seq0, len0);
                memcpy(a, now[0], sizeof(now[0])); memcpy(b, now[1], sizeof(now[1]));
                memcpy(c, now[2], sizeof(now[2])); memcpy(d, now[3], sizeof(now[3]));
            }
            require(len0 > 1000u && len0 == len1 && !memcmp(seq0, seq1, len0 * 4u),
                    take ? (frame ? "the next frame: the GE runs our list as the game's, BJUMPs taken" : "the GE runs our list as the game's, BJUMPs taken") : (frame ? "the next frame: the GE runs our list as the game's, BJUMPs not taken" : "the GE runs our list as the game's, BJUMPs not taken"));
        }
    }
    sync_result = 0; sceGeDrawSyncPatched(0);
    require(c[0] >> 24 == GE_CMD_FRAMEBUFPTR && c[1] >> 24 == GE_CMD_FRAMEBUFPTR, "(c's targets)");
    c[0] = saved[2][0]; c[1] = saved[2][1]; /* (FRAMEBUFPTR words stay ours: upstream's, never put back) */
    require(!memcmp(saved[0], a, sizeof(saved[0])) && !memcmp(saved[1], b, sizeof(saved[1])) &&
            !memcmp(saved[2], c, sizeof(saved[2])) && !memcmp(saved[3], d, sizeof(saved[3])),
            "once the GE is done: the list the game's again, its JUMPs back");
    release_every = 4096u;
    release_enabled = 0;
    group_release = 0;
#undef CALL_SUB
#undef BASE_OF
#undef LO
    puts("PASS: the main list run where it is, its chunks JUMPing by way of ours, in groups");
}


/* A Vita that copies frames from VRAM itself: SetFrameBuf with the GE done
 * queues no copy and counts a frame left in VRAM; the next list waits until
 * the Vita says it took it, and a Vita that never does is waited for 8 ms. */
static void run_direct_tests(void) {
    uint32_t *l = (uint32_t *)0x08700000u;
    unsigned before, submitted;
    u32 ready;
    l[0] = command(GE_CMD_FINISH, 0);
    l[1] = command(GE_CMD_END, 0);
    *(volatile u32 *)GP_VIDEO_ADDR = GP_VIDEO_TAKES_FRAMES;
    sync_result = 0;
    rendered_in_sync = 0;
    mode_changed = 0; truecolor_frames_left = 0; /* nothing but the frame to show */
    before = copies; submitted = submissions;
    sceDisplaySetFrameBufPatched((void *)FAKE_VRAM, 512, 3, 1);
    ready = *(volatile u32 *)GP_FRAME_READY_ADDR;
    require(copies == before && ready == frames_left_in_vram && ready != *(volatile u32 *)GP_FRAME_TAKEN_ADDR,
            "the Vita takes frames: no GE copy, a frame left in VRAM");
    *(volatile u32 *)GP_FRAME_TAKEN_ADDR = ready; /* the Vita copied it */
    sceGeListEnQueuePatched(l, NULL, -1, NULL);
    require(submissions > submitted && !take_timeouts, "the next list once the frame is taken");
    sceDisplaySetFrameBufPatched((void *)FAKE_VRAM, 512, 3, 1);
    {   /* nobody takes this one: the next list after 8 ms, not never */
        const u32 timeouts = take_timeouts;
        fake_time_advance = 1000u;
        sceGeListEnQueuePatched(l, NULL, -1, NULL);
        fake_time_advance = 0;
        require(take_timeouts == timeouts + 1u, "a Vita side that never takes the frame is not waited for forever");
    }
    *(volatile u32 *)GP_VIDEO_ADDR = 0;
    sync_result = 0; sceGeDrawSyncPatched(0);
    puts("PASS: frames left in VRAM for the Vita to copy, the GE waiting for it");
}


/* The game's VBlank wait: a frame done before its
 * VBlank waits for it; one past it goes on at once; far behind (loading), it
 * waits again and the time starts from there. */
static unsigned pace_waits;
static int paceWaitStub(void) { fake_time_us += PACE_FRAME_US - fake_time_us % PACE_FRAME_US; ++pace_waits; return 0; }
static void run_pace_tests(void) {
    pace_waits = 0;
    fake_time_us = 1000000u;
    pacedWait(paceWaitStub, 1u); /* (first: far behind the zero time) */
    require(pace_waits == 0u, "far behind: not held, the time from now");
    fake_time_us += PACE_FRAME_US - fake_time_us % PACE_FRAME_US; /* (at a VBlank, as after a wait) */
    pace_start = fake_time_us - pace_frames * PACE_FRAME_US;
    pace_waits = 1u;
    fake_time_us += 9000u;
    pacedWait(paceWaitStub, 1u);
    require(pace_waits == 2u, "a quick frame waits for its VBlank");
    fake_time_us += 30000u;
    pacedWait(paceWaitStub, 1u);
    require(pace_waits == 2u, "a frame past its VBlank: not held");
    fake_time_us += 13000u; /* (a quick one after: still behind) */
    pacedWait(paceWaitStub, 1u);
    require(pace_waits == 2u, "and the next, still behind: not held");
    fake_time_us += 2000u; /* 45.7 ms done of 50.0 due: ahead now */
    pacedWait(paceWaitStub, 1u);
    require(pace_waits == 3u, "ahead again: waits");
    fake_time_us += 150000u;
    pacedWait(paceWaitStub, 1u);
    require(pace_waits == 3u, "more than 100 ms behind: not held either");
    fake_time_us += 5000u; /* the time from that one: 5 ms of the next 16.7 */
    pacedWait(paceWaitStub, 1u);
    require(pace_waits == 4u, "and after it, a quick frame waits again");
    fake_time_us += 30000u;
    pacedWait(paceWaitStub, 1u);
    require(pace_waits == 4u, "behind again: not held");
    dialog_open = 1;
    fake_time_us += 30000u;
    pacedWait(paceWaitStub, 1u);
    require(pace_waits == 5u, "a system dialog up: always waits");
    dialog_open = 0;
    puts("PASS: the game's VBlank wait not held when behind; far behind the time starts again");
}


/* Short main lists (menus, fades) go by the straightened copy and are
 * released as they are parsed, the GE drawing meanwhile; a fight's long ones
 * (the last list long) are parsed where they are and given whole. */
static const u32 *short_stall[64];
static unsigned short_stalls;
static void shortStallNote(const u32 *stall) { if (stall && short_stalls < 64u) short_stall[short_stalls++] = stall; }
static void run_short_release_tests(void) {
    uint32_t *const l = (uint32_t *)0x08730000u, *const sub = (uint32_t *)0x08738000u;
    unsigned n = 0, k;
    u32 lists_before, releases_before;
    sub[0] = command(GE_CMD_TEXWRAP, 1); sub[1] = command(GE_CMD_RET, 0);
    l[n++] = command(GE_CMD_BASE, (0x08738000u >> 8) & 0x0f0000);
    for (k = 0; k < 200u; ++k) { l[n++] = command(GE_CMD_TEXFUNC, k); l[n++] = command(GE_CMD_CALL, 0x738000); }
    l[n++] = command(GE_CMD_FINISH, 0);
    l[n++] = command(GE_CMD_END, 0);
    short_release = 1;
    linear_qid = -1;
    last_list_commands = 1000u;
    lists_before = linear_lists; releases_before = perf_releases;
    sceGeListEnQueuePatched(l, NULL, -1, NULL);
    require(linear_lists == lists_before + 1u && perf_releases > releases_before,
            "a short list (the last one short): copied, released while parsed");
    require(last_list_commands >= 600u && last_list_commands < 1000u, "its size kept for the next");
    sync_result = 0; sceGeDrawSyncPatched(0);
    linear_qid = -1;
    last_list_commands = 100000u;
    lists_before = linear_lists; releases_before = perf_releases;
    sceGeListEnQueuePatched(l, NULL, -1, NULL);
    require(linear_lists == lists_before && perf_releases == releases_before,
            "after a long one: parsed where it is, given whole");
    sync_result = 0; sceGeDrawSyncPatched(0);
    {   /* a sublist setting a draw's BASE/VADDR/VTYPE, its PRIM after the RET:
           no stall there (an inset 2D draw points those words at our pool at
           its PRIM: the GE must not have run them) */
        int16_t *v = (int16_t *)0x08739000u;
        uint32_t *const own = (uint32_t *)0x0873A000u;
        const u32 *copy = (const u32 *)LINEAR_LIST;
        unsigned at_prim = 0;
        v[0] = 0; v[1] = 0; v[2] = 0; v[3] = 64; v[4] = 64; v[5] = 0;
        for (k = 0; k < 300u; ++k) own[k] = command(GE_CMD_TEXWRAP, k); /* (past a release's due) */
        own[300] = command(GE_CMD_BASE, (0x08739000u >> 8) & 0x0f0000);
        own[301] = command(GE_CMD_VADDR, 0x739000);
        own[302] = command(GE_CMD_VERTEXTYPE, GE_VTYPE_THROUGH | GE_VTYPE_POS_16BIT);
        own[303] = command(GE_CMD_RET, 0);
        n = 0;
        l[n++] = command(GE_CMD_BASE, (0x08738000u >> 8) & 0x0f0000);
        for (k = 0; k < 300u; ++k) l[n++] = command(GE_CMD_CALL, 0x738000);
        l[n++] = command(GE_CMD_CALL, 0x73A000);
        l[n++] = command(GE_CMD_PRIM, (6u << 16) | 2u);
        l[n++] = command(GE_CMD_FINISH, 0);
        l[n++] = command(GE_CMD_END, 0);
        short_stalls = 0;
        stall_check = shortStallNote;
        linear_qid = -1;
        last_list_commands = 1000u;
        sceGeListEnQueuePatched(l, NULL, -1, NULL);
        stall_check = NULL;
        for (k = 0; k < 400u; ++k) if (copy[k] >> 24 == GE_CMD_PRIM) { at_prim = k; break; }
        require(at_prim && short_stalls > 1u, "(the copy's PRIM; released before it)");
        for (k = 0; k < short_stalls; ++k)
            require(short_stall[k] != copy + at_prim, "no release between a draw's own words and its PRIM");
        sync_result = 0; sceGeDrawSyncPatched(0);
    }
    short_release = 0;
    linear_qid = -1;
    puts("PASS: short lists copied and released, long ones parsed in place");
}

/* Two display buffers with an Adrenaline that shows the one DISPLAY_FRONT
 * names: frames copied into them in turn, each copy followed by a one-pixel
 * transfer of its number to DISPLAY_FRONT; without the bit (or a stock
 * Adrenaline), one buffer as ever. */
static void copyTransfers(u32 *dst, u32 *src, unsigned *n) {
    const u32 *p = (const u32 *)(RENDER_LIST | 0xA0000000u);
    unsigned i, d = 0, s = 0;
    for (i = 0; i < 128u && p[i] >> 24 != GE_CMD_END; ++i) {
        if (p[i] >> 24 == GE_CMD_TRANSFERDST && d < 4u) dst[d++] = p[i] & 0xffffffu;
        if (p[i] >> 24 == GE_CMD_TRANSFERSRC && s < 4u) src[s++] = p[i] & 0xffffffu;
    }
    *n = d;
}
static void run_two_buffer_tests(void) {
    u32 dst[4], src[4];
    unsigned n;
    const volatile u32 *values = (const volatile u32 *)(DISPLAY_FRONT_VALUES | 0xA0000000u);
    gp_stock = 0;
    display_back = 1;
    *(volatile u32 *)GP_VIDEO_ADDR = GP_VIDEO_TWO_BUFFERS;
    rendered_in_sync = 0;
    sceDisplaySetFrameBufPatched((void *)FAKE_VRAM, 512, 3, 1);
    copyTransfers(dst, src, &n);
    require(n == 2u && dst[0] == (DISPLAY_BUFFER2 & 0xffffffu) && dst[1] == (DISPLAY_FRONT & 0xffffffu) &&
            src[1] == ((DISPLAY_FRONT_VALUES + 64u) & 0xffffffu) && values[16] == 1u,
            "two buffers: the frame into the second, then 1 to DISPLAY_FRONT");
    sync_result = 0; sceGeDrawSyncPatched(0);
    rendered_in_sync = 0;
    sceDisplaySetFrameBufPatched((void *)FAKE_VRAM, 512, 3, 1);
    copyTransfers(dst, src, &n);
    require(n == 2u && dst[0] == (DISPLAY_BUFFER & 0xffffffu) && src[1] == (DISPLAY_FRONT_VALUES & 0xffffffu) &&
            values[0] == 0u, "the next frame into the first, then 0");
    sync_result = 0; sceGeDrawSyncPatched(0);
    *(volatile u32 *)GP_VIDEO_ADDR = 0;
    rendered_in_sync = 0;
    sceDisplaySetFrameBufPatched((void *)FAKE_VRAM, 512, 3, 1);
    copyTransfers(dst, src, &n);
    require(n == 1u && dst[0] == (DISPLAY_BUFFER & 0xffffffu), "without the bit: one buffer");
    sync_result = 0; sceGeDrawSyncPatched(0);
    puts("PASS: two display buffers in turn, the one done named after its copy; one without the bit");
}

/* Fast path vs full path: a pseudo-random list of every non-control, non-draw
 * command must leave the same words and parse state. */
static void run_fastpath_tests(void) {
    uint32_t *l = (uint32_t *)0x08060000u, original[2002], fast_words[2002];
    static GeState fast_state;
    u32 fast_dither, screen_vp[4], screen_offset[2], screen_scissor[4];
    uint32_t seed = 12345u;
    unsigned i, n = 0;
    for (i = 0; i < 2000u; ++i) {
        unsigned cmd;
        do {
            seed = seed * 1103515245u + 12345u;
            cmd = (seed >> 16) & 0xFFu;
        } while (cmd == GE_CMD_CALL || cmd == GE_CMD_JUMP || cmd == GE_CMD_BJUMP || cmd == GE_CMD_RET ||
                 cmd == GE_CMD_END || cmd == GE_CMD_FINISH || cmd == GE_CMD_SIGNAL || cmd == GE_CMD_ORIGIN ||
                 cmd == GE_CMD_PRIM || cmd == GE_CMD_BEZIER || cmd == GE_CMD_SPLINE || cmd == GE_CMD_BOUNDINGBOX);
        seed = seed * 1103515245u + 12345u;
        l[n++] = command(cmd, seed >> 8);
    }
    l[n++] = command(GE_CMD_FINISH, 0);
    l[n++] = command(GE_CMD_END, 0);
    memcpy(original, l, n * 4u);

    force_full_parse = 0;
    game_dither_set = 0;
    memcpy(screen_vp, game_vp, sizeof(screen_vp)); memcpy(screen_offset, game_offset, sizeof(screen_offset));
    memcpy(screen_scissor, game_scissor, sizeof(screen_scissor));
    resetGeState(); patchGeList(l, NULL);
    memcpy(fast_words, l, n * 4u);
    fast_state = state;
    fast_dither = game_dither_set;
    sync_result = 0; sceGeDrawSyncPatched(0);

    memcpy(l, original, n * 4u);
    force_full_parse = 1;
    game_dither_set = 0;
    /* the game's screen registers carry over lists, as on the GE: both runs from the same */
    memcpy(game_vp, screen_vp, sizeof(screen_vp)); memcpy(game_offset, screen_offset, sizeof(screen_offset));
    memcpy(game_scissor, screen_scissor, sizeof(screen_scissor));
    resetGeState(); patchGeList(l, NULL);
    force_full_parse = 0;
    require(!memcmp(fast_words, l, n * 4u), "fast path patches the same words");
    /* Registers nothing reads may stay unrecorded on the fast path. */
    for (i = 0; i < 256u; ++i)
        if (!(cmd_class[i] & CMD_KEEP) && ((cmd_class[i] & CMD_HOT) || !(cmd_class[i] & CMD_HANDLED)))
            fast_state.ge_cmd[i] = state.ge_cmd[i] = 0;
    require(!memcmp(&fast_state, &state, sizeof(state)), "fast path leaves the same parse state");
    require(fast_dither == game_dither_set, "fast path sees the game's dither matrix the same");
    sceGeDrawSyncPatched(0);
    puts("PASS: table-driven fast path equals the full parse on 2000 random commands");
    run_hotpath_tests();
    run_linear_tests();
    run_release_tests();
    run_group_tests();
    run_direct_tests();
    run_pace_tests();
    run_short_release_tests();
    run_two_buffer_tests();
}

/* Upstream stride rule (Fast1 also took 480; Tekken 6 then showed stripes of
 * old frames, so 480 is dropped again). 8888 games get the GE dither. */
static void run_stride_tests(void) {
    uint32_t *l = (uint32_t *)0x08060000u;
    const uint32_t *copy = (const uint32_t *)(RENDER_LIST | 0xA0000000u);
    unsigned i, dith0 = 0, enable = 0;
    l[0] = command(GE_CMD_FRAMEBUFPTR, 0x000000);
    l[1] = command(GE_CMD_FRAMEBUFWIDTH, 0x0A0000 | 480);
    l[2] = command(GE_CMD_FINISH, 0);
    l[3] = command(GE_CMD_END, 0);
    {
        const unsigned flushes = whole_cache_flushes, ranges = writeback_ranges;
        resetGeState(); patchGeList(l, NULL);
        require(whole_cache_flushes == flushes && writeback_ranges > ranges,
                "a parse writes back the words it changed, never the whole cache");
    }
    require(state.ignore_framebuf, "480-stride target dropped, as upstream");
    sync_result = 0; sceGeDrawSyncPatched(0);

    {   /* auto mip level one finer at 2x: biased once, put back after the GE */
        l[0] = command(GE_CMD_TEXLEVEL, (0xF0u << 16) | GE_TEXLEVEL_MODE_AUTO); /* bias -1 */
        l[1] = command(GE_CMD_TEXLEVEL, (0x10u << 16) | GE_TEXLEVEL_MODE_CONST);
        l[2] = command(GE_CMD_FINISH, 0);
        l[3] = command(GE_CMD_END, 0);
        resetGeState(); patchGeList(l, NULL);
        resetGeState(); patchGeList(l, NULL);
        require(l[0] == command(GE_CMD_TEXLEVEL, 0u << 16), "auto LOD +1 at 2x, once per parse cycle");
        require(l[1] == command(GE_CMD_TEXLEVEL, (0x10u << 16) | GE_TEXLEVEL_MODE_CONST), "constant LOD kept");
        sync_result = 0; sceGeDrawSyncPatched(0);
        require(l[0] == command(GE_CMD_TEXLEVEL, (0xF0u << 16) | GE_TEXLEVEL_MODE_AUTO), "LOD word put back after the GE");
    }

    {   /* a list parsed again before the GE finished: worked out from the
           game's own words, not from ours */
        l[0] = command(GE_CMD_FRAMEBUFPTR, 0x000000);
        l[1] = command(GE_CMD_FRAMEBUFWIDTH, 0x040000 | 512);
        l[2] = command(GE_CMD_TEXLEVEL, GE_TEXLEVEL_MODE_AUTO);
        l[3] = command(GE_CMD_FINISH, 0);
        l[4] = command(GE_CMD_END, 0);
        resetGeState(); patchGeList(l, NULL);
        require((l[1] & 0xffff) == 960 && l[2] == command(GE_CMD_TEXLEVEL, 0x10u << 16), "960x544: stride and LOD");
        resetGeState(); patchGeList(l, NULL);
        require((l[1] & 0xffff) == 960 && l[2] == command(GE_CMD_TEXLEVEL, 0x10u << 16), "parsed again: once, not twice");
        sync_result = 0; sceGeDrawSyncPatched(0);
        require(l[1] == command(GE_CMD_FRAMEBUFWIDTH, 0x040000 | 512), "stride word put back after the GE");
    }

    {   /* runs: a draw's positions as one range; the same draw again, another
           scale, a draw over part of it, then everything put back */
        int16_t *v = (int16_t *)(l + 32); /* 4 vertices, 6 shorts (12 bytes) apart */
        unsigned k;
        for (k = 0; k < 4u; ++k) { v[k * 6] = (int16_t)(10 + k); v[k * 6 + 1] = (int16_t)-(int)(20 + k); v[k * 6 + 2] = 77; }
        require(gp_vertex_scale_run(&vertex_restore, v, 4, 12, 2) && v[0] == 20 && v[1] == -40 && v[18] == 26 && v[20] == 77,
                "run: scaled once, z left alone");
        require(gp_vertex_scale_run(&vertex_restore, v, 4, 12, 2) && v[0] == 20 && v[18] == 26, "run: the same draw again, once");
        require(gp_vertex_scale_run(&vertex_restore, v + 6, 2, 12, 2) && v[6] == 22 && v[12] == 24,
                "run: part of a run, not scaled twice");
        require(gp_vertex_scale(&vertex_restore, v + 12, 2) && v[12] == 24, "one vertex of a run: not twice");
        gp_scale_num = 3; gp_scale_den = 2; gp_scale_num_y = 3; /* (vertex_restore's scale changed) */
        require(gp_vertex_scale_run(&vertex_restore, v, 4, 12, 2) && v[0] == 15 && v[1] == -30 && v[18] == 20,
                "run: another scale, from the originals");
        gp_scale_num = 2; gp_scale_den = 1; gp_scale_num_y = 2;
        gp_vertex_restore_complete(&vertex_restore, NULL);
        for (k = 0; k < 4u; ++k)
            require(v[k * 6] == (int16_t)(10 + k) && v[k * 6 + 1] == (int16_t)-(int)(20 + k) && v[k * 6 + 2] == 77,
                    "run: the game's values back");
    }

    {   /* a game that never waits for the GE refills its vertices where ours
           still stand: new values are scaled as new, and putting back leaves
           what the game wrote alone */
        int16_t *v = (int16_t *)(l + 48);
        v[0] = 10; v[1] = 20; v[6] = 30; v[7] = 40;
        require(gp_vertex_scale_run(&vertex_restore, v, 2, 12, 2) && v[0] == 20 && v[6] == 60, "refill: first frame scaled");
        v[0] = 11; v[1] = 21; /* the game's next frame, vertex 0 only */
        require(gp_vertex_scale_run(&vertex_restore, v, 2, 12, 2) && v[0] == 22 && v[1] == 42 && v[6] == 60,
                "refill: the rewritten vertex scaled from its new value, the other kept");
        v[6] = 33; v[7] = 44; /* written again, not parsed before the GE is done */
        gp_vertex_restore_complete(&vertex_restore, NULL);
        require(v[0] == 11 && v[1] == 21 && v[6] == 33 && v[7] == 44, "refill: put back where ours stood, the game's left");
    }

    {   /* a 2D vertex referenced again at another scale: from its original */
        int16_t *xy = (int16_t *)(l + 16);
        xy[0] = 100; xy[1] = -50;
        require(gp_vertex_scale(&vertex_restore, xy, 2u) && xy[0] == 200 && xy[1] == -100, "2x");
        require(gp_vertex_scale(&vertex_restore, xy, 2u) && xy[0] == 200, "same scale: once");
        gp_scale_num = 3; gp_scale_den = 2; gp_scale_num_y = 3;
        require(gp_vertex_scale(&vertex_restore, xy, 2u) && xy[0] == 150 && xy[1] == -75, "1.5x after the change, not 3x");
        gp_scale_num = 2; gp_scale_den = 1; gp_scale_num_y = 2;
        gp_vertex_restore_complete(&vertex_restore, NULL);
        require(xy[0] == 100 && xy[1] == -50, "original put back");
    }

    {   /* a movie: the depth buffer squeezed
           to the end of VRAM between frames, and back after it */
        *(volatile u32 *)GP_VIDEO_ADDR = 0;
        mpeg_frames_ago = 0; videocodec_open = 1;
        followMode();
        l[0] = command(GE_CMD_ZBUFPTR, 0x100000);
        l[1] = command(GE_CMD_ZBUFWIDTH, 0x040000 | 512);
        l[2] = command(GE_CMD_FINISH, 0);
        l[3] = command(GE_CMD_END, 0);
        resetGeState(); patchGeList(l, NULL);
        require(movie_depth && l[0] == command(GE_CMD_ZBUFPTR, MOVIE_DEPTH) &&
                l[1] == command(GE_CMD_ZBUFWIDTH, 0x040000 | MOVIE_DEPTH_STRIDE), "movie: depth squeezed at 960x544");
        /* the decoder busy in a 3D scene (Tekken's fight): the depth buffer back */
        frame_3d_draw = 1; mpeg_frames_ago = 0;
        followMode();
        require(!movie_depth, "decoder busy while 3D is drawn: not a movie, depth buffer whole");
        mpeg_frames_ago = 0; followMode(); /* one frame without 3D: still not */
        require(!movie_depth, "3D a frame ago: still not a movie");
        mpeg_frames_ago = 0xffffu; videocodec_open = 0;
        followMode();
        resetGeState(); patchGeList(l, NULL);
        require(!movie_depth && l[0] == command(GE_CMD_ZBUFPTR, 0x100000) &&
                l[1] == command(GE_CMD_ZBUFWIDTH, 0x040000 | 960), "after the movie: the depth buffer back");
        sync_result = 0; sceGeDrawSyncPatched(0);
    }

    {   /* guard band: identity matrices, NDC = position; at 960 pixels the GE
           drops |x| >= 4.27 screens, the PSP's 480 only |x| >= 8.53 */
        static const float identity12[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
        static const float identity16[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        static const float small[9] = {-0.5f, -0.5f, 0, 0.5f, -0.5f, 0, 0, 0.5f, 0};
        static const float wide[9] = {-6.0f, -0.5f, 0, 6.0f, -0.5f, 0, 0, 0.5f, 0};
        static const float huge[9] = {-12.0f, -0.5f, 0, 12.0f, -0.5f, 0, 0, 0.5f, 0};
        GpMatrices m;
        union { float f; uint32_t i; } v;
        const float lx = 2048.0f / 480.0f, ly = 2048.0f / 272.0f;
        unsigned k;
        memset(&m, 0, sizeof(m));
        gp_matrix_command(&m, GE_CMD_WORLDMATRIXNUMBER, 0);
        for (k = 0; k < 12u; ++k) { v.f = identity12[k]; gp_matrix_command(&m, GE_CMD_WORLDMATRIXDATA, v.i >> 8); }
        gp_matrix_command(&m, GE_CMD_VIEWMATRIXNUMBER, 0);
        for (k = 0; k < 12u; ++k) { v.f = identity12[k]; gp_matrix_command(&m, GE_CMD_VIEWMATRIXDATA, v.i >> 8); }
        gp_matrix_command(&m, GE_CMD_PROJMATRIXNUMBER, 0);
        for (k = 0; k < 16u; ++k) { v.f = identity16[k]; gp_matrix_command(&m, GE_CMD_PROJMATRIXDATA, v.i >> 8); }
        {
            GpGuardDraw d;
            float out[3 * 16 * 3];
            unsigned n, i;
            memset(&d, 0, sizeof(d));
            d.vtype = GE_VTYPE_POS_FLOAT; d.prim = 3; d.count = 3;
            d.limit_x = lx; d.limit_y = ly; d.psp_x = 2 * lx; d.psp_y = 2 * ly;
            d.vertices = (const uint8_t *)small;
            require(gp_guard_prim(&m, &d) == GP_GUARD_FITS, "guard band: a small triangle fits");
            d.vertices = (const uint8_t *)wide;
            require(gp_guard_prim(&m, &d) == GP_GUARD_DROPPED_HERE, "guard band: 12 screens wide is dropped at 960, kept at 480");
            d.vertices = (const uint8_t *)huge;
            require(gp_guard_prim(&m, &d) == GP_GUARD_FITS, "guard band: dropped on the PSP too is not ours");
            d.vertices = (const uint8_t *)wide;
            require(gp_guard_out_vtype(GE_VTYPE_POS_FLOAT) == GE_VTYPE_POS_FLOAT && gp_guard_out_size(GE_VTYPE_POS_FLOAT) == 12u,
                    "guard band: cut vertices, float positions");
            n = gp_guard_cut(&m, &d, (uint8_t *)out, 48);
            require(n >= 3u && n % 3u == 0u, "guard band: the wide triangle cut into triangles");
            for (i = 0; i < n; ++i)
                require(out[i * 3] <= 0.9f * lx + 1e-3f && out[i * 3] >= -0.9f * lx - 1e-3f &&
                        out[i * 3 + 1] >= -0.5f - 1e-3f && out[i * 3 + 1] <= 0.5f + 1e-3f,
                        "guard band: cut vertices inside 0.9 of the band, on the triangle");
            {   /* the cut keeps the winding: same sign of area as the original */
                const float area = (out[3] - out[0]) * (out[7] - out[1]) - (out[4] - out[1]) * (out[6] - out[0]);
                const float orig = (wide[3] - wide[0]) * (wide[7] - wide[1]) - (wide[4] - wide[1]) * (wide[6] - wide[0]);
                require((area > 0) == (orig > 0), "guard band: winding kept");
            }
            require(gp_guard_cut(&m, &d, (uint8_t *)out, 2) == 0u, "guard band: no room, nothing cut");
        }
    }

    {   /* a 3D window (a character preview) on the right half: viewport
           centre 2048 + 120, scale 120, scissor 240..479 -> twice, in place */
        union { float f; uint32_t i; } t;
        t.f = 2168.0f; l[0] = command(GE_CMD_VIEWPORTXCENTER, t.i >> 8);
        t.f = 120.0f; l[1] = command(GE_CMD_VIEWPORTXSCALE, t.i >> 8);
        l[2] = command(GE_CMD_OFFSETX, (2048u - 240u) << 4);
        l[3] = command(GE_CMD_SCISSOR1, 240u | (0u << 10));
        l[4] = command(GE_CMD_SCISSOR2, 479u | (271u << 10));
        l[5] = command(GE_CMD_FINISH, 0);
        l[6] = command(GE_CMD_END, 0);
        resetGeState(); patchGeList(l, NULL);
        resetGeState(); patchGeList(l, NULL); /* again: from the game's words */
        t.i = (l[0] & 0xffffff) << 8;
        /* PSP pixel of ndc 0: 2168 - 1808 = 360 -> here 720 = c' - (2048 - 480) */
        require(t.f == 720.0f + 1568.0f, "window: centre maps the PSP pixel to twice it");
        t.i = (l[1] & 0xffffff) << 8;
        require(t.f == 240.0f, "window: scale twice the game's");
        require(l[3] == command(GE_CMD_SCISSOR1, 480u) && l[4] == command(GE_CMD_SCISSOR2, 959u | (543u << 10)),
                "window: scissor scaled, not the whole screen");
        sync_result = 0; sceGeDrawSyncPatched(0);
        require(l[3] == command(GE_CMD_SCISSOR1, 240u), "window: the game's scissor back after the GE");
        /* the princess game: the window moved by OFFSETX, set after the centre
           (1668: PSP pixel 380 at ndc 0 -> 760 here, a centre of 760 + 1568);
           the offset word becomes that centre, the GE's offset stays ours */
        t.f = 2048.0f; l[0] = command(GE_CMD_VIEWPORTXCENTER, t.i >> 8);
        l[2] = command(GE_CMD_OFFSETX, 1668u << 4);
        resetGeState(); patchGeList(l, NULL);
        t.f = 2328.0f;
        require(l[2] == command(GE_CMD_VIEWPORTXCENTER, t.i >> 8), "window: the offset after the centre, in a new centre");
        /* a game with offset 0 and centre 240 (Tekken's fight): the same picture */
        t.f = 240.0f; l[0] = command(GE_CMD_VIEWPORTXCENTER, t.i >> 8);
        l[2] = command(GE_CMD_OFFSETX, 0);
        resetGeState(); patchGeList(l, NULL);
        t.f = 2048.0f;
        require(l[0] == command(GE_CMD_VIEWPORTXCENTER, t.i >> 8) || l[2] == command(GE_CMD_VIEWPORTXCENTER, t.i >> 8),
                "window: offset 0 and centre 240 is the centre of the screen, not off it");
        require(l[2] == command(GE_CMD_VIEWPORTXCENTER, t.i >> 8), "window: from the last offset");
        t.f = 2048.0f; l[0] = command(GE_CMD_VIEWPORTXCENTER, t.i >> 8);
        l[2] = command(GE_CMD_OFFSETX, (2048u - 240u) << 4);
        sync_result = 0; sceGeDrawSyncPatched(0);
        /* back to the usual whole screen for what follows */
        t.f = 2048.0f; l[0] = command(GE_CMD_VIEWPORTXCENTER, t.i >> 8);
        t.f = 240.0f; l[1] = command(GE_CMD_VIEWPORTXSCALE, t.i >> 8);
        l[3] = command(GE_CMD_SCISSOR1, 0);
        resetGeState(); patchGeList(l, NULL);
        sync_result = 0; sceGeDrawSyncPatched(0);
    }

    {   /* Tekken's arenas: a draw behind BOUNDINGBOX + BJUMP is patched for when
           the GE runs it (the branch is taken only when nothing is visible) */
        l[0] = command(GE_CMD_BASE, 0x080000);
        l[1] = command(GE_CMD_BJUMP, (u32)(uintptr_t)(l + 4) & 0xffffffu);
        l[2] = command(GE_CMD_FRAMEBUFPTR, 0);
        l[3] = command(GE_CMD_FRAMEBUFWIDTH, 0x040000 | 512);
        l[4] = command(GE_CMD_FINISH, 0);
        l[5] = command(GE_CMD_END, 0);
        resetGeState(); patchGeList(l, NULL);
        require((l[3] & 0xffff) == 960, "the draw after a BJUMP is patched");
        sync_result = 0; sceGeDrawSyncPatched(0);
    }

    {   /* Tekken's bloom: a sprite sampling the other frame buffer is dropped
           until the GE is done, and the depth clear after it (texturing still
           on, a shared sublist) is not: upstream's NOP stayed, and every later
           frame went uncleared -- the fight one solid colour */
        int16_t *v = (int16_t *)(l + 64);
        unsigned k;
        for (k = 0; k < 12u; ++k) v[k] = 0;
        l[0] = command(GE_CMD_FRAMEBUFPTR, 0x044000);
        l[1] = command(GE_CMD_FRAMEBUFWIDTH, 0x040000 | 512);
        l[2] = command(GE_CMD_FRAMEBUFPTR, 0x000000);
        l[3] = command(GE_CMD_FRAMEBUFWIDTH, 0x040000 | 512);
        l[4] = command(GE_CMD_TEXTUREMAPENABLE, 1);
        l[5] = command(GE_CMD_TEXADDR0, 0x044000);
        l[6] = command(GE_CMD_TEXBUFWIDTH0, 0x0A0000 | 512);
        l[7] = command(GE_CMD_VERTEXTYPE, GE_VTYPE_THROUGH | (2u << 7));
        l[8] = command(GE_CMD_BASE, ((u32)(uintptr_t)v >> 8) & 0x0f0000);
        l[9] = command(GE_CMD_VADDR, (u32)(uintptr_t)v & 0xffffff);
        l[10] = command(GE_CMD_PRIM, (6u << 16) | 2u);
        l[11] = command(GE_CMD_CLEARMODE, 0x401);
        l[12] = command(GE_CMD_PRIM, (6u << 16) | 2u);
        l[13] = command(GE_CMD_CLEARMODE, 0);
        l[14] = command(GE_CMD_FINISH, 0);
        l[15] = command(GE_CMD_END, 0);
        resetGeState(); patchGeList(l, NULL);
        require(l[10] == 0u, "a sprite sampling the other frame buffer is dropped");
        require(l[12] == command(GE_CMD_PRIM, (6u << 16) | 2u), "the clear after it is kept: clear mode samples nothing");
        resetGeState(); patchGeList(l, NULL);
        require(l[10] == 0u && l[12] == command(GE_CMD_PRIM, (6u << 16) | 2u), "parsed again before the GE: the same");
        sync_result = 0; sceGeDrawSyncPatched(0);
        require(l[10] == command(GE_CMD_PRIM, (6u << 16) | 2u), "the dropped draw is put back after the GE");
        l[11] = command(GE_CMD_NOP, 0); /* the game's own NOP stays one */
        resetGeState(); patchGeList(l, NULL);
        require(l[11] == 0u, "a game's NOP is left alone");
        sync_result = 0; sceGeDrawSyncPatched(0);
        require(l[10] == command(GE_CMD_PRIM, (6u << 16) | 2u) && l[11] == 0u, "and after the GE");
    }

    {   /* games flip Y: the sign survives the runtime (unsigned) size */
        union { float f; uint32_t i; } t;
        t.f = -136.0f; l[0] = command(GE_CMD_VIEWPORTYSCALE, t.i >> 8);
        t.f = 240.0f; l[1] = command(GE_CMD_VIEWPORTXSCALE, t.i >> 8);
        l[2] = command(GE_CMD_FINISH, 0);
        l[3] = command(GE_CMD_END, 0);
        resetGeState(); patchGeList(l, NULL);
        t.i = (l[0] & 0xffffff) << 8;
        require(t.f == -(float)(HEIGHT / 2), "negative viewport Y scale stays negative");
        t.i = (l[1] & 0xffffff) << 8;
        require(t.f == (float)(WIDTH / 2), "viewport X scale at the mode's width");
        sync_result = 0; sceGeDrawSyncPatched(0);
    }

    truecolor_frames_left = 0;
    game_dither_set = 0;
    l[0] = command(GE_CMD_FRAMEBUFPIXFORMAT, GE_FORMAT_8888);
    l[1] = command(GE_CMD_DITHERENABLE, 0);
    l[2] = command(GE_CMD_FINISH, 0);
    l[3] = command(GE_CMD_END, 0);
    stencil_frames_left = 0;
    resetGeState(); patchGeList(l, NULL);
    require(l[0] == command(GE_CMD_FRAMEBUFPIXFORMAT, GE_FORMAT_565), "8888 drawn as 565");
    require(l[1] == command(GE_CMD_DITHERENABLE, 1), "an 8888 game's dither switch is turned on");
    framebuf_seen = 0; rendered_in_sync = 0;
    sceGeDrawSyncPatched(0);
    for (i = 0; i < 40u; ++i) {
        if (copy[i] == command(GE_CMD_DITH0, (u32)(-4 & 15) | (0u << 4) | ((u32)(-3 & 15) << 8) | (1u << 12))) dith0 = 1;
        if (copy[i] == command(GE_CMD_DITHERENABLE, 1)) enable = 1;
        if ((copy[i] >> 24) == GE_CMD_END) break;
    }
    require(dith0 && enable, "display copy leaves a Bayer matrix and dither on for 8888 games");
    for (i = 0; i < STENCIL_HOLD_FRAMES; ++i) sceDisplaySetFrameBufPatched((void *)FAKE_VRAM, 512, 3, 1);
    l[0] = command(GE_CMD_FRAMEBUFPIXFORMAT, GE_FORMAT_565);
    l[1] = command(GE_CMD_DITHERENABLE, 0);
    resetGeState(); patchGeList(l, NULL);
    require(l[1] == command(GE_CMD_DITHERENABLE, 0), "565 games keep their own dither setting");
    sceGeDrawSyncPatched(0);
    puts("PASS: upstream stride rule; GE dither for 8888 games only");
}
