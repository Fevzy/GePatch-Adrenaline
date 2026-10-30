/* GPL-2.0-or-later. Half-texel UV inset for linear-filtered through draws.
 *
 * Bilinear sampling of a tile in a texture atlas reads the neighbouring tile
 * at the tile's edges once the picture is drawn larger than the PSP screen.
 * The clamp the native renderer applies per triangle in its pixel shader is
 * lo = min(UV) + 0.5, hi = max(lo, max(UV) - 0.5), clamp(UV, lo, hi).
 * Here the same clamp is applied to the vertex UVs of each sprite or quad.
 *
 * Integer (u8/u16) UVs cannot hold half a texel, so the draw gets a float-UV
 * copy of its vertices. Nothing is inserted into the guest's command list:
 * only the values of the draw's own BASE, VADDR and VTYPE words change, and
 * only when the next draw sets all three again itself (see main.c). */
#ifndef GP_UV_INSET_H
#define GP_UV_INSET_H
#include <stdint.h>
#include <stddef.h>

#define GP_INSET_EDITS 1024u
#define GP_INSET_HASH 8192u /* at most 3072 words: probes stay short */
#define GP_INSET_VTYPE 0x80019Fu /* through, float UV, RGBA8888, float XYZ */

typedef struct GpInsetVertex {
    float u, v;
    uint32_t color;
    float x, y, z;
} GpInsetVertex;

typedef struct GpInsetSource {
    uint32_t type;      /* guest VTYPE */
    uint32_t primitive; /* PRIM data: type << 16 | count */
    uint32_t vertices;  /* first vertex address */
    uint32_t stride, position;
    uint32_t texture;   /* texture address: tiles only join within one texture */
} GpInsetSource;

/* An axis-aligned sprite or quad of an inset draw, before clamping. One
 * picture is often drawn as many tiles, in one draw or many (Tekken's 128x128
 * quads): an edge another tile continues -- same texture, same screen edge,
 * same UV there, same span across -- must not be clamped, or the joins show. */
#define GP_INSET_TILES 2048u
#define GP_INSET_TILE_HASH 4096u
typedef struct GpInsetTile {
    GpInsetVertex *v;
    uint32_t texture;
    uint16_t count, joined; /* joined: 1 left, 2 right, 4 top, 8 bottom */
    float x0, x1, y0, y1;   /* screen rectangle */
    float ul, ur, vt, vb;   /* UV at the left/right/top/bottom edge */
} GpInsetTile;
typedef struct GpInsetTiles {
    uint16_t left[GP_INSET_TILE_HASH], top[GP_INSET_TILE_HASH];
    uint16_t next_left[GP_INSET_TILES], next_top[GP_INSET_TILES];
    GpInsetTile tile[GP_INSET_TILES];
} GpInsetTiles;

typedef struct GpInsetWord {
    uint32_t *address;
    uint32_t original, value;
} GpInsetWord;

typedef struct GpInset {
    GpInsetWord words[GP_INSET_EDITS * 3u];
    uint16_t hash[GP_INSET_HASH];
    GpInsetVertex *pool;
    unsigned capacity, used_vertices, word_count;
    /* the lowest and highest word changed: most lookups (every BASE, VADDR
     * and VTYPE of a 3D scene) are elsewhere, and a probe of the 16 KiB hash
     * missed the cache each time */
    const uint32_t *lo, *hi;
    unsigned applied, reused, unsupported, full, overwritten, peak_vertices;
    GpInsetTiles *tiles; /* optional; without it every tile is clamped */
    unsigned tile_count, joined;
} GpInset;

void gp_inset_init(GpInset *, GpInsetVertex *pool, unsigned capacity);
void gp_inset_set_tiles(GpInset *, GpInsetTiles *);
/* After a list is parsed, before the GE runs it: unclamp the edges where one
 * tile continues another (see GpInsetTile). */
void gp_inset_join_tiles(GpInset *);
/* The guest's value of a word we changed, so a repeated parse sees the
 * original list. Any other value is returned unchanged. */
uint32_t gp_inset_original(const GpInset *, const uint32_t *address, uint32_t current);
/* Our value for `address`, or 0 when the word is not ours. */
uint32_t gp_inset_value(const GpInset *, const uint32_t *address);
/* Writes the copy at the pool cursor, with the inset when `clamp` (a draw
 * that only continues an inset one is copied as it is). Returns its vertex
 * count, 0 when the draw is not supported (the cursor is then unchanged). */
unsigned gp_inset_build(GpInset *, const GpInsetSource *, GpInsetVertex **first, int clamp);
/* One source vertex in the copy's layout (for diagnostics). */
int gp_inset_read(const GpInsetSource *, unsigned index, GpInsetVertex *out);
void gp_inset_rollback(GpInset *, unsigned cursor);
/* Records and writes `count` words (BASE/VADDR/VTYPE of a draw or a run of
 * draws). 0 when full: nothing is written then. */
int gp_inset_commit(GpInset *, uint32_t *const words[], const uint32_t values[], unsigned count);
/* At global GE completion: put back every word still holding our value and
 * release the copies. A word the game has rewritten since is left alone. */
unsigned gp_inset_complete(GpInset *, void (*written)(const void *, unsigned));
/* The clamp itself, for one sprite / triangle / quad; exposed for tests. */
void gp_inset_group(GpInsetVertex *group, unsigned count);
#endif
