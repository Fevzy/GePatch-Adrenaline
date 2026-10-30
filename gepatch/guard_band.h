/* GPL-2.0-or-later. Where a 3D draw's vertices land on the GE's screen.
 *
 * The GE drops a triangle with a vertex outside its 4096x4096 screen space
 * (the guard band). The PSP's 480-pixel screen leaves 8.5 screens of it, 960
 * pixels only 4.3: big triangles near the camera -- DW's ground -- are kept
 * on the PSP and dropped at 960x544 (grey patches, none at 480x272; measured:
 * 1-10 thousand draws in 5 s of a battle). The game's matrices are followed,
 * positions transformed here, and such triangles cut to the guard band (as
 * the GE cuts at the near plane) into new vertices the GE then draws. */
#ifndef GP_GUARD_BAND_H
#define GP_GUARD_BAND_H
#include <stdint.h>

typedef struct GpMatrices {
    float world[12], view[12], proj[16];
    unsigned world_index, view_index, proj_index;
    float wvp[16]; /* clip = (x y z 1) * wvp, rows of 4 */
    int dirty;
} GpMatrices;

/* A GE matrix command (WORLD/VIEW/PROJ MATRIXNUMBER and DATA), 24-bit data. */
void gp_matrix_command(GpMatrices *m, unsigned cmd, uint32_t data);

enum { GP_GUARD_FITS = 0, GP_GUARD_DROPPED_HERE = 1, GP_GUARD_OTHER = 2 };

typedef struct GpGuardDraw {
    const uint8_t *vertices;
    uint32_t vtype;           /* guest VTYPE: no weights, no morph */
    unsigned prim, count;     /* 3 triangles, 4 strip, 5 fan */
    const void *indices;      /* NULL, or `index_size` bytes each (1, 2, 4) */
    unsigned index_size;
    float limit_x, limit_y;   /* guard band in NDC here: 2048 / viewport scale */
    float psp_x, psp_y;       /* ... and on the PSP */
} GpGuardDraw;

/* GP_GUARD_DROPPED_HERE: a triangle the PSP keeps is dropped here. */
int gp_guard_prim(GpMatrices *m, const GpGuardDraw *draw);

/* The VTYPE of the cut triangles: the guest's parts, as floats (colour 8888),
 * not indexed. */
uint32_t gp_guard_out_vtype(uint32_t vtype);
unsigned gp_guard_out_size(uint32_t vtype); /* bytes per vertex of that VTYPE */
/* The triangles of `draw` dropped here and kept on the PSP, cut to 0.9 of the
 * guard band (and the near plane), as a triangle list at `out` in
 * gp_guard_out_vtype(). Returns the vertex count; 0 if none, or if more than
 * `capacity` vertices were needed (then nothing is drawn extra). */
unsigned gp_guard_cut(GpMatrices *m, const GpGuardDraw *draw, uint8_t *out, unsigned capacity);
#endif
