#include "guard_band.h"
#include <string.h>

/* GE command numbers (ge_constants.h is not shared with this file) */
#define CMD_WORLDNUM 0x3Au
#define CMD_WORLDDATA 0x3Bu
#define CMD_VIEWNUM 0x3Cu
#define CMD_VIEWDATA 0x3Du
#define CMD_PROJNUM 0x3Eu
#define CMD_PROJDATA 0x3Fu

void gp_matrix_command(GpMatrices *m, unsigned cmd, uint32_t data) {
    union { uint32_t i; float f; } v;
    v.i = data << 8; /* the top 24 bits of a float */
    switch (cmd) {
    case CMD_WORLDNUM: m->world_index = data & 15u; break;
    case CMD_VIEWNUM: m->view_index = data & 15u; break;
    case CMD_PROJNUM: m->proj_index = data & 15u; break;
    case CMD_WORLDDATA: if (m->world_index < 12u) m->world[m->world_index] = v.f; ++m->world_index; m->dirty = 1; break;
    case CMD_VIEWDATA: if (m->view_index < 12u) m->view[m->view_index] = v.f; ++m->view_index; m->dirty = 1; break;
    case CMD_PROJDATA: if (m->proj_index < 16u) m->proj[m->proj_index] = v.f; ++m->proj_index; m->dirty = 1; break;
    default: break;
    }
}

/* 4x3 (rows: x, y, z axes and the translation) times 4x3, then times the
 * 4x4 projection: the GE's order, (x y z 1) * world * view * proj. */
static void combine(GpMatrices *m) {
    float wv[12];
    unsigned r, c;
    for (r = 0; r < 4u; ++r)
        for (c = 0; c < 3u; ++c)
            wv[r * 3 + c] = m->world[r * 3] * m->view[c] + m->world[r * 3 + 1] * m->view[3 + c] +
                            m->world[r * 3 + 2] * m->view[6 + c] + (r == 3u ? m->view[9 + c] : 0.0f);
    for (r = 0; r < 4u; ++r)
        for (c = 0; c < 4u; ++c)
            m->wvp[r * 4 + c] = wv[r * 3] * m->proj[c] + wv[r * 3 + 1] * m->proj[4 + c] +
                                wv[r * 3 + 2] * m->proj[8 + c] + (r == 3u ? m->proj[12 + c] : 0.0f);
    m->dirty = 0;
}

/* The PSP's vertex layout: weights, UV, colour, normal, position, each at its
 * own alignment, the whole at the largest. */
typedef struct {
    unsigned size, tc_off, col_off, nrm_off, pos_off;
    unsigned tc, col, nrm, pos; /* VTYPE fields */
} Layout;

static unsigned align_up(unsigned v, unsigned a) { return a > 1u ? (v + a - 1u) & ~(a - 1u) : v; }

static void layout(uint32_t vtype, Layout *l) {
    static const uint8_t tcsize[4] = {0, 2, 4, 8}, tcalign[4] = {1, 1, 2, 4};
    static const uint8_t colsize[8] = {0, 0, 0, 0, 2, 2, 2, 4}, colalign[8] = {1, 1, 1, 1, 2, 2, 2, 4};
    static const uint8_t xyzsize[4] = {0, 3, 6, 12}, xyzalign[4] = {1, 1, 2, 4};
    static const uint8_t wtsize[4] = {0, 1, 2, 4};
    unsigned size = 0, biggest = 1, weight = (vtype >> 9) & 3u;
    l->tc = vtype & 3u;
    l->col = (vtype >> 2) & 7u;
    l->nrm = (vtype >> 5) & 3u;
    l->pos = (vtype >> 7) & 3u;
    if (weight) { size += wtsize[weight] * (((vtype >> 14) & 7u) + 1u); if (wtsize[weight] > biggest) biggest = wtsize[weight]; }
    if (l->tc) { size = align_up(size, tcalign[l->tc]); l->tc_off = size; size += tcsize[l->tc]; if (tcalign[l->tc] > biggest) biggest = tcalign[l->tc]; }
    if (l->col) { size = align_up(size, colalign[l->col]); l->col_off = size; size += colsize[l->col]; if (colalign[l->col] > biggest) biggest = colalign[l->col]; }
    if (l->nrm) { size = align_up(size, xyzalign[l->nrm]); l->nrm_off = size; size += xyzsize[l->nrm]; if (xyzalign[l->nrm] > biggest) biggest = xyzalign[l->nrm]; }
    if (l->pos) { size = align_up(size, xyzalign[l->pos]); l->pos_off = size; size += xyzsize[l->pos]; if (xyzalign[l->pos] > biggest) biggest = xyzalign[l->pos]; }
    l->size = align_up(size, biggest);
}

/* Three signed values: 8/16-bit ones are fractions (/128, /32768), as the GE
 * reads them outside through mode. */
static void read3(const uint8_t *p, unsigned kind, float out[3]) {
    if (kind == 1u) { const int8_t *s = (const int8_t *)p; out[0] = s[0] / 128.0f; out[1] = s[1] / 128.0f; out[2] = s[2] / 128.0f; }
    else if (kind == 2u) { int16_t s[3]; memcpy(s, p, 6); out[0] = s[0] / 32768.0f; out[1] = s[1] / 32768.0f; out[2] = s[2] / 32768.0f; }
    else memcpy(out, p, 12);
}

/* A vertex being cut: its clip position and its attributes, all linear. */
typedef struct {
    float c[4];   /* clip x, y, z, w */
    float a[12];  /* u v, r g b a, normal, position */
} Vertex;

static void decode(const uint8_t *v, const Layout *l, Vertex *out) {
    memset(out->a, 0, sizeof(out->a));
    if (l->tc == 1u) { out->a[0] = v[l->tc_off] / 128.0f; out->a[1] = v[l->tc_off + 1] / 128.0f; }
    else if (l->tc == 2u) { uint16_t t[2]; memcpy(t, v + l->tc_off, 4); out->a[0] = t[0] / 32768.0f; out->a[1] = t[1] / 32768.0f; }
    else if (l->tc == 3u) memcpy(out->a, v + l->tc_off, 8);
    if (l->col == 7u) {
        uint32_t c; memcpy(&c, v + l->col_off, 4);
        out->a[2] = (float)(c & 255u); out->a[3] = (float)((c >> 8) & 255u);
        out->a[4] = (float)((c >> 16) & 255u); out->a[5] = (float)(c >> 24);
    } else if (l->col >= 4u) {
        uint16_t c; memcpy(&c, v + l->col_off, 2);
        if (l->col == 4u) { /* 565 */
            out->a[2] = (float)(((c & 31u) * 255u + 15u) / 31u); out->a[3] = (float)((((c >> 5) & 63u) * 255u + 31u) / 63u);
            out->a[4] = (float)((((c >> 11) & 31u) * 255u + 15u) / 31u); out->a[5] = 255.0f;
        } else if (l->col == 5u) { /* 5551 */
            out->a[2] = (float)(((c & 31u) * 255u + 15u) / 31u); out->a[3] = (float)((((c >> 5) & 31u) * 255u + 15u) / 31u);
            out->a[4] = (float)((((c >> 10) & 31u) * 255u + 15u) / 31u); out->a[5] = (c >> 15) ? 255.0f : 0.0f;
        } else { /* 4444 */
            out->a[2] = (float)((c & 15u) * 17u); out->a[3] = (float)(((c >> 4) & 15u) * 17u);
            out->a[4] = (float)(((c >> 8) & 15u) * 17u); out->a[5] = (float)(((c >> 12) & 15u) * 17u);
        }
    }
    if (l->nrm) read3(v + l->nrm_off, l->nrm, out->a + 6);
    read3(v + l->pos_off, l->pos, out->a + 9);
}

/* Infinity or NaN: a float with all exponent bits set. The GE takes them;
 * here they are left alone, whatever the FPU would make of them. */
static int is_finite(float f) {
    uint32_t b;
    memcpy(&b, &f, 4);
    return ((b >> 23) & 0xffu) != 0xffu;
}
static int finite_matrix(const GpMatrices *m) {
    unsigned i;
    for (i = 0; i < 16u; ++i)
        if (!is_finite(m->wvp[i]) || m->wvp[i] > 1e18f || m->wvp[i] < -1e18f) return 0;
    return 1;
}

static void transform(const GpMatrices *m, const float *p, float c[4]) {
    c[0] = p[0] * m->wvp[0] + p[1] * m->wvp[4] + p[2] * m->wvp[8] + m->wvp[12];
    c[1] = p[0] * m->wvp[1] + p[1] * m->wvp[5] + p[2] * m->wvp[9] + m->wvp[13];
    c[2] = p[0] * m->wvp[2] + p[1] * m->wvp[6] + p[2] * m->wvp[10] + m->wvp[14];
    c[3] = p[0] * m->wvp[3] + p[1] * m->wvp[7] + p[2] * m->wvp[11] + m->wvp[15];
}

uint32_t gp_guard_out_vtype(uint32_t vtype) {
    uint32_t out = 3u << 7; /* float position */
    if (vtype & 3u) out |= 3u;
    if ((vtype >> 2) & 7u) out |= 7u << 2;
    if ((vtype >> 5) & 3u) out |= 3u << 5;
    return out;
}

unsigned gp_guard_out_size(uint32_t vtype) {
    return ((vtype & 3u) ? 8u : 0u) + (((vtype >> 2) & 7u) ? 4u : 0u) + (((vtype >> 5) & 3u) ? 12u : 0u) + 12u;
}

static uint8_t *encode(const Vertex *v, uint32_t vtype, uint8_t *p) {
    if (vtype & 3u) { memcpy(p, v->a, 8); p += 8; }
    if ((vtype >> 2) & 7u) {
        uint32_t c = 0;
        unsigned k;
        for (k = 0; k < 4u; ++k) {
            float f = v->a[2 + k] + 0.5f;
            unsigned b = f <= 0.0f ? 0u : f >= 255.0f ? 255u : (unsigned)f;
            c |= b << (8u * k);
        }
        memcpy(p, &c, 4); p += 4;
    }
    if ((vtype >> 5) & 3u) { memcpy(p, v->a + 6, 12); p += 12; }
    memcpy(p, v->a + 9, 12); p += 12;
    return p;
}

/* Planes as w-multiples: near z + w >= 0, and the guard band. */
static float plane(const float c[4], unsigned k, float lx, float ly) {
    switch (k) {
    case 0: return c[2] + c[3];
    case 1: return lx * c[3] - c[0];
    case 2: return lx * c[3] + c[0];
    case 3: return ly * c[3] - c[1];
    default: return ly * c[3] + c[1];
    }
}

static unsigned clip_plane(const Vertex *in, unsigned n, Vertex *out, unsigned k, float lx, float ly) {
    unsigned i, m = 0;
    for (i = 0; i < n; ++i) {
        const Vertex *a = &in[i], *b = &in[(i + 1u) % n];
        const float da = plane(a->c, k, lx, ly), db = plane(b->c, k, lx, ly);
        if (da >= 0.0f) out[m++] = *a;
        if ((da >= 0.0f) != (db >= 0.0f)) {
            const float t = da / (da - db);
            unsigned j;
            for (j = 0; j < 4u; ++j) out[m].c[j] = a->c[j] + (b->c[j] - a->c[j]) * t;
            for (j = 0; j < 12u; ++j) out[m].a[j] = a->a[j] + (b->a[j] - a->a[j]) * t;
            ++m;
        }
    }
    return m;
}

static int inside(const float c[4], float lx, float ly) {
    return c[0] < lx * c[3] && -c[0] < lx * c[3] && c[1] < ly * c[3] && -c[1] < ly * c[3];
}

/* Per triangle (in the GE's winding): 0 fine, 1 dropped here but kept on
 * the PSP. The near-plane cut first, as the GE does, then the guard band. */
static int classify(Vertex tri[3], const GpGuardDraw *d, Vertex *poly, unsigned *n) {
    unsigned i, here = 1, psp = 1;
    *n = clip_plane(tri, 3, poly, 0, 0, 0);
    if (*n < 3u) return 0;
    for (i = 0; i < *n; ++i) {
        if (!inside(poly[i].c, d->limit_x, d->limit_y)) here = 0;
        if (!inside(poly[i].c, d->psp_x, d->psp_y)) psp = 0;
    }
    return psp && !here;
}

static unsigned index_at(const GpGuardDraw *d, unsigned i) {
    if (!d->indices) return i;
    return d->index_size == 1u ? ((const uint8_t *)d->indices)[i] :
           d->index_size == 2u ? ((const uint16_t *)d->indices)[i] : ((const uint32_t *)d->indices)[i];
}

/* The triangles of a draw, in the GE's winding (odd strip ones swapped). */
static unsigned triangles(const GpGuardDraw *d) {
    if (d->prim == 3u) return d->count / 3u;
    return d->count >= 3u ? d->count - 2u : 0u;
}
static void triangle(const GpGuardDraw *d, unsigned t, unsigned v[3]) {
    if (d->prim == 3u) { v[0] = 3u * t; v[1] = 3u * t + 1u; v[2] = 3u * t + 2u; }
    else if (d->prim == 4u) { v[0] = t + (t & 1u); v[1] = t + 1u - (t & 1u); v[2] = t + 2u; }
    else { v[0] = 0; v[1] = t + 1u; v[2] = t + 2u; }
}

static void load(GpMatrices *m, const GpGuardDraw *d, const Layout *l, unsigned i, Vertex *out) {
    decode(d->vertices + index_at(d, i) * l->size, l, out);
    transform(m, out->a + 9, out->c);
}

int gp_guard_prim(GpMatrices *m, const GpGuardDraw *d) {
    Layout l;
    Vertex tri[3], poly[8];
    unsigned t, count, n;
    if (d->prim < 3u || d->prim > 5u || d->count < 3u) return GP_GUARD_OTHER;
    if (d->vtype & ((3u << 9) | (7u << 18))) return GP_GUARD_OTHER; /* skinned, morphed */
    layout(d->vtype, &l);
    if (!l.pos) return GP_GUARD_OTHER;
    if (m->dirty) combine(m);
    if (!finite_matrix(m)) return GP_GUARD_OTHER;
    count = triangles(d);
    for (t = 0; t < count; ++t) {
        unsigned v[3], k;
        triangle(d, t, v);
        for (k = 0; k < 3u; ++k) {
            /* positions only: what the check needs */
            float p[3];
            read3(d->vertices + index_at(d, v[k]) * l.size + l.pos_off, l.pos, p);
            if (!is_finite(p[0]) || !is_finite(p[1]) || !is_finite(p[2]) || p[0] > 1e18f || p[0] < -1e18f ||
                p[1] > 1e18f || p[1] < -1e18f || p[2] > 1e18f || p[2] < -1e18f) return GP_GUARD_OTHER;
            transform(m, p, tri[k].c);
        }
        if (classify(tri, d, poly, &n)) return GP_GUARD_DROPPED_HERE;
    }
    return GP_GUARD_FITS;
}

unsigned gp_guard_cut(GpMatrices *m, const GpGuardDraw *d, uint8_t *out, unsigned capacity) {
    const uint32_t vtype = gp_guard_out_vtype(d->vtype);
    const float cut_x = d->limit_x * 0.9f, cut_y = d->limit_y * 0.9f;
    Layout l;
    Vertex tri[3], poly[8], next[12];
    unsigned t, count, n, written = 0;
    if (gp_guard_prim(m, d) != GP_GUARD_DROPPED_HERE) return 0;
    layout(d->vtype, &l);
    count = triangles(d);
    for (t = 0; t < count; ++t) {
        unsigned v[3], k, p;
        triangle(d, t, v);
        for (k = 0; k < 3u; ++k) load(m, d, &l, v[k], &tri[k]);
        if (!classify(tri, d, poly, &n)) continue;
        for (p = 1; p <= 4u && n >= 3u; ++p) {
            n = clip_plane(poly, n, next, p, cut_x, cut_y);
            memcpy(poly, next, n * sizeof(Vertex));
        }
        if (n < 3u) continue;
        if (written + 3u * (n - 2u) > capacity) return 0;
        for (k = 1; k + 1u < n; ++k) { /* a fan, in the triangle's winding */
            out = encode(&poly[0], vtype, out);
            out = encode(&poly[k], vtype, out);
            out = encode(&poly[k + 1u], vtype, out);
            written += 3u;
        }
    }
    return written;
}
