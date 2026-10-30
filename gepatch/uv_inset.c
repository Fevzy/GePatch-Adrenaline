/* GPL-2.0-or-later. See uv_inset.h. */
#include "uv_inset.h"
#include <string.h>
#include <float.h>

static float low(float a, float b) { return a < b ? a : b; }
static float high(float a, float b) { return a > b ? a : b; }
static int finite_value(float x) { return x >= -FLT_MAX && x <= FLT_MAX; }

void gp_inset_init(GpInset *s, GpInsetVertex *pool, unsigned capacity) {
    memset(s, 0, sizeof(*s));
    s->pool = pool;
    s->capacity = capacity;
}

static unsigned hash_address(const uint32_t *p) {
    return (unsigned)((((uintptr_t)p) >> 2) * 2654435761u) & (GP_INSET_HASH - 1u);
}
static const GpInsetWord *find_word(const GpInset *s, const uint32_t *p) {
    unsigned h = hash_address(p);
    while (s->hash[h]) {
        const GpInsetWord *w = &s->words[s->hash[h] - 1u];
        if (w->address == p) return w;
        h = (h + 1u) & (GP_INSET_HASH - 1u);
    }
    return NULL;
}
uint32_t gp_inset_original(const GpInset *s, const uint32_t *p, uint32_t current) {
    const GpInsetWord *w;
    const uint32_t op = current >> 24;
    /* Called for every command parsed; only these three are ever ours. In 3D
     * scenes the lookup for all the rest was most of the plugin's CPU time. */
    if (!s->word_count || p < s->lo || p > s->hi || (op != 0x01u && op != 0x10u && op != 0x12u)) return current;
    w = find_word(s, p);
    return w && w->value == current ? w->original : current;
}
uint32_t gp_inset_value(const GpInset *s, const uint32_t *p) {
    const GpInsetWord *w = s->word_count && p >= s->lo && p <= s->hi ? find_word(s, p) : NULL;
    return w ? w->value : 0u;
}

void gp_inset_group(GpInsetVertex *g, unsigned n) {
    float u0 = g[0].u, u1 = g[0].u, v0 = g[0].v, v1 = g[0].v, ulo, uhi, vlo, vhi;
    unsigned i;
    for (i = 1; i < n; ++i) {
        u0 = low(u0, g[i].u); u1 = high(u1, g[i].u);
        v0 = low(v0, g[i].v); v1 = high(v1, g[i].v);
    }
    ulo = u0 + 0.5f; uhi = high(ulo, u1 - 0.5f);
    vlo = v0 + 0.5f; vhi = high(vlo, v1 - 0.5f);
    for (i = 0; i < n; ++i) {
        g[i].u = low(high(g[i].u, ulo), uhi);
        g[i].v = low(high(g[i].v, vlo), vhi);
    }
}

void gp_inset_set_tiles(GpInset *s, GpInsetTiles *tiles) { s->tiles = tiles; s->tile_count = 0; }

/* Records an axis-aligned sprite (2 corners) or quad (4) before it is clamped:
 * every vertex on a corner, its U set by its X side and its V by its Y side. */
static void record_tile(GpInset *s, GpInsetVertex *v, unsigned n, uint32_t texture) {
    GpInsetTile *t;
    float x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y, ul = 0, ur = 0, vt = 0, vb = 0;
    int have_ul = 0, have_ur = 0, have_vt = 0, have_vb = 0;
    unsigned k;
    if (!s->tiles || s->tile_count >= GP_INSET_TILES) return;
    for (k = 1; k < n; ++k) {
        x0 = low(x0, v[k].x); x1 = high(x1, v[k].x);
        y0 = low(y0, v[k].y); y1 = high(y1, v[k].y);
    }
    if (x0 == x1 || y0 == y1) return;
    for (k = 0; k < n; ++k) {
        const int left = v[k].x == x0, top = v[k].y == y0;
        if (!left && v[k].x != x1) return;
        if (!top && v[k].y != y1) return;
        if (left) { if (have_ul && ul != v[k].u) return; ul = v[k].u; have_ul = 1; }
        else { if (have_ur && ur != v[k].u) return; ur = v[k].u; have_ur = 1; }
        if (top) { if (have_vt && vt != v[k].v) return; vt = v[k].v; have_vt = 1; }
        else { if (have_vb && vb != v[k].v) return; vb = v[k].v; have_vb = 1; }
    }
    if (!have_ul || !have_ur || !have_vt || !have_vb) return;
    t = &s->tiles->tile[s->tile_count++];
    t->v = v; t->texture = texture; t->count = (uint16_t)n; t->joined = 0;
    t->x0 = x0; t->x1 = x1; t->y0 = y0; t->y1 = y1;
    t->ul = ul; t->ur = ur; t->vt = vt; t->vb = vb;
}

static uint32_t float_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static unsigned tile_key(uint32_t texture, float edge, float uv, float a0, float a1, float b0, float b1) {
    uint32_t h = texture * 2654435761u;
    h = (h ^ float_bits(edge)) * 2246822519u;
    h = (h ^ float_bits(uv)) * 3266489917u;
    h = (h ^ float_bits(a0)) * 668265263u;
    h = (h ^ float_bits(a1)) * 374761393u;
    h = (h ^ float_bits(b0)) * 2654435761u;
    h = (h ^ float_bits(b1)) * 2246822519u;
    return (h ^ (h >> 15)) & (GP_INSET_TILE_HASH - 1u);
}

/* The shader clamp for one axis, without moving a joined edge. */
static void axis_range(float first, float second, int first_joined, int second_joined, float *lo, float *hi) {
    if (first <= second) {
        *lo = first_joined ? first : first + 0.5f;
        *hi = second_joined ? second : high(*lo, second - 0.5f);
    } else {
        *lo = second_joined ? second : second + 0.5f;
        *hi = first_joined ? first : high(*lo, first - 0.5f);
    }
}

void gp_inset_join_tiles(GpInset *s) {
    GpInsetTiles *m = s->tiles;
    unsigned i, j, k, n = s->tile_count;
    if (!m || !n) return;
    memset(m->left, 0, sizeof(m->left));
    memset(m->top, 0, sizeof(m->top));
    for (i = 0; i < n; ++i) {
        const GpInsetTile *t = &m->tile[i];
        unsigned h = tile_key(t->texture, t->x0, t->ul, t->y0, t->y1, t->vt, t->vb);
        m->next_left[i] = m->left[h]; m->left[h] = (uint16_t)(i + 1u);
        h = tile_key(t->texture, t->y0, t->vt, t->x0, t->x1, t->ul, t->ur);
        m->next_top[i] = m->top[h]; m->top[h] = (uint16_t)(i + 1u);
    }
    for (i = 0; i < n; ++i) {
        GpInsetTile *t = &m->tile[i];
        for (j = m->left[tile_key(t->texture, t->x1, t->ur, t->y0, t->y1, t->vt, t->vb)]; j; j = m->next_left[j - 1u]) {
            GpInsetTile *r = &m->tile[j - 1u];
            if (r != t && r->texture == t->texture && r->x0 == t->x1 && r->ul == t->ur &&
                r->y0 == t->y0 && r->y1 == t->y1 && r->vt == t->vt && r->vb == t->vb) {
                t->joined |= 2u; r->joined |= 1u;
            }
        }
        for (j = m->top[tile_key(t->texture, t->y1, t->vb, t->x0, t->x1, t->ul, t->ur)]; j; j = m->next_top[j - 1u]) {
            GpInsetTile *b = &m->tile[j - 1u];
            if (b != t && b->texture == t->texture && b->y0 == t->y1 && b->vt == t->vb &&
                b->x0 == t->x0 && b->x1 == t->x1 && b->ul == t->ul && b->ur == t->ur) {
                t->joined |= 8u; b->joined |= 4u;
            }
        }
    }
    for (i = 0; i < n; ++i) {
        const GpInsetTile *t = &m->tile[i];
        float ulo, uhi, vlo, vhi;
        if (!t->joined) continue;
        ++s->joined;
        axis_range(t->ul, t->ur, t->joined & 1u, t->joined & 2u, &ulo, &uhi);
        axis_range(t->vt, t->vb, t->joined & 4u, t->joined & 8u, &vlo, &vhi);
        for (k = 0; k < t->count; ++k) {
            GpInsetVertex *v = &t->v[k];
            const float u = v->x == t->x0 ? t->ul : t->ur, w = v->y == t->y0 ? t->vt : t->vb;
            v->u = low(high(u, ulo), uhi);
            v->v = low(high(w, vlo), vhi);
        }
    }
    s->tile_count = 0;
}

static int read_vertex(const GpInsetSource *d, unsigned i, GpInsetVertex *out) {
    unsigned tc = d->type & 3u, pos = (d->type >> 7) & 3u;
    uint64_t address = (uint64_t)d->vertices + (uint64_t)i * d->stride;
    const unsigned char *v;
    if (address < 0x08400000u || address + d->stride > 0x0A400000u) return 0;
    v = (const unsigned char *)(uintptr_t)address;
    if (tc == 1) { out->u = v[0]; out->v = v[1]; }
    else if (tc == 2) { uint16_t uv[2]; memcpy(uv, v, 4); out->u = uv[0]; out->v = uv[1]; }
    else memcpy(&out->u, v, 8);
    /* RGBA8888 is 4-byte aligned after the texture coordinates. */
    memcpy(&out->color, v + (tc == 3 ? 8 : 4), 4);
    if (pos == 2) {
        int16_t xy[2]; uint16_t z;
        memcpy(xy, v + d->position, 4); memcpy(&z, v + d->position + 4, 2);
        out->x = xy[0]; out->y = xy[1]; out->z = z; /* through-mode depth is unsigned */
    } else memcpy(&out->x, v + d->position, 12);
    return finite_value(out->u) && finite_value(out->v) &&
           finite_value(out->x) && finite_value(out->y) && finite_value(out->z);
}

int gp_inset_read(const GpInsetSource *d, unsigned i, GpInsetVertex *out) { return read_vertex(d, i, out); }

unsigned gp_inset_build(GpInset *s, const GpInsetSource *d, GpInsetVertex **first, int clamp) {
    unsigned prim = d->primitive >> 16, count = d->primitive & 0xFFFFu, group, i, cursor = s->used_vertices;
    GpInsetVertex *out;
    /* Through, RGBA8888, a texture coordinate, 16-bit or float position, and
     * no index, weights, normals or morphs. */
    if ((d->type & 0x00FFFFFFu & ~0x00800183u) != 0x1Cu || !(d->type & 0x800000u) ||
        !(d->type & 3u) || ((d->type >> 7) & 3u) < 2u) { ++s->unsupported; return 0; }
    if (prim == 6u && count >= 2u && !(count & 1u)) group = 2u;          /* sprites */
    else if (prim == 3u && count >= 3u && count % 3u == 0u) group = 3u;  /* triangles */
    else if ((prim == 4u || prim == 5u) && count == 4u) group = 4u;      /* one quad */
    else { ++s->unsupported; return 0; }
    if (count > s->capacity - s->used_vertices) { ++s->full; return 0; }
    out = s->pool + cursor;
    for (i = 0; i < count; ++i)
        if (!read_vertex(d, i, &out[i])) { ++s->unsupported; return 0; }
    if (clamp) {
        for (i = 0; i < count; i += group) {
            if (group != 3u) record_tile(s, out + i, group, d->texture);
            gp_inset_group(out + i, group);
        }
    }
    s->used_vertices += count;
    if (s->used_vertices > s->peak_vertices) s->peak_vertices = s->used_vertices;
    *first = out;
    return count;
}

void gp_inset_rollback(GpInset *s, unsigned cursor) {
    s->used_vertices = cursor;
    while (s->tile_count && s->tiles->tile[s->tile_count - 1u].v >= s->pool + cursor) --s->tile_count;
}

int gp_inset_commit(GpInset *s, uint32_t *const words[], const uint32_t values[], unsigned count) {
    unsigned k;
    if (s->word_count + count > GP_INSET_EDITS * 3u) { ++s->full; return 0; }
    for (k = 0; k < count; ++k) {
        unsigned h = hash_address(words[k]);
        GpInsetWord *w;
        while (s->hash[h]) h = (h + 1u) & (GP_INSET_HASH - 1u);
        w = &s->words[s->word_count];
        w->address = words[k];
        w->original = *words[k];
        w->value = values[k];
        s->hash[h] = (uint16_t)(++s->word_count);
        if (s->word_count == 1u || words[k] < s->lo) s->lo = words[k];
        if (s->word_count == 1u || words[k] > s->hi) s->hi = words[k];
        *words[k] = values[k];
    }
    ++s->applied;
    return 1;
}

unsigned gp_inset_complete(GpInset *s, void (*written)(const void *, unsigned)) {
    unsigned i, count = s->word_count;
    for (i = 0; i < count; ++i) {
        GpInsetWord *w = &s->words[i];
        if (*w->address == w->value) {
            *w->address = w->original;
            if (written) written(w->address, 4u);
        } else ++s->overwritten;
    }
    if (count) memset(s->hash, 0, sizeof(s->hash));
    s->word_count = 0;
    s->used_vertices = 0;
    s->tile_count = 0;
    return count;
}
