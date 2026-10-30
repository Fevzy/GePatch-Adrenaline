/* GPL-2.0-or-later. Coordinates are owned by GE until a confirmed completion. */
#ifndef GP_VERTEX_RESTORE_H
#define GP_VERTEX_RESTORE_H
#include <stddef.h>
#include <stdint.h>

#define GP_VERTEX_SLOTS 16384u
typedef struct GpVertexEntry {
    uintptr_t address;
    uint32_t original[2];
    unsigned width; /* bytes per coordinate | (num_y << 12 | num << 8 | den it was patched at) << 8 */
} GpVertexEntry;
/* A draw's run of positions scaled at once (the usual case: each draw its
 * own vertices): one range instead of a hash entry per vertex, the originals
 * in order in `log`. The hash keeps what overlaps a range only in part. */
#define GP_VERTEX_RANGES 1024u
#define GP_VERTEX_LOG 16384u
#define GP_VERTEX_PAGES 1024u /* (address >> 12) mod 1024 */
#define GP_VERTEX_LINKS 4096u
typedef struct GpVertexRange {
    uintptr_t start, end;     /* first position, end of the last one */
    uint32_t stride, first;   /* bytes between positions; first log entry */
    uint16_t width, scale;    /* bytes per coordinate; num_y << 12 | num << 8 | den */
    uint32_t count;
} GpVertexRange;
typedef struct GpVertexRestore {
    GpVertexEntry entries[GP_VERTEX_SLOTS];
    uint16_t used_slots[GP_VERTEX_SLOTS];
    unsigned used, peak, exhausted, unsupported;
    GpVertexRange ranges[GP_VERTEX_RANGES];
    uint32_t log[GP_VERTEX_LOG][2]; /* originals of the ranges' positions, in order */
    unsigned range_count, log_used;
    /* Ranges by the 4 KiB pages they cover: a draw is checked against the
     * few on its pages (all of them, one by one, was slower than the hash). */
    uint16_t page_head[GP_VERTEX_PAGES];        /* 1 + first link, 0 none */
    uint16_t link_range[GP_VERTEX_LINKS], link_next[GP_VERTEX_LINKS];
    unsigned link_count;
    uintptr_t ranges_end; /* the highest range end: a run from there on overlaps none */
    unsigned runs, one_by_one, peak_ranges; /* statistics */
} GpVertexRestore;

/* Repeated references in the SAME unfinished GE epoch are patched only once
 * per scale (a mode change rescales them from the stored original).
 * There is deliberately no comparison of current bytes to a guessed result. */
int gp_vertex_scale(GpVertexRestore *state, void *position_xy, unsigned width);
/* `count` positions `stride` bytes apart from `first`: as gp_vertex_scale on
 * each, cheaper when the run overlaps no other (0 if one was not scaled). */
int gp_vertex_scale_run(GpVertexRestore *state, void *first, unsigned count, unsigned stride, unsigned width);
/* The scale through-mode coordinates get: num / den (2/1 or 3/2) across,
 * num_y / den down (the same, but 1/1 at 960x544i: half the lines a frame). */
extern unsigned gp_scale_num, gp_scale_den, gp_scale_num_y;
/* Call only after GE reports all drawing complete, before returning to guest.
 * `written` (may be NULL) is told every range put back, for cache writeback. */
unsigned gp_vertex_restore_complete(GpVertexRestore *state, void (*written)(const void *, unsigned));
#endif
