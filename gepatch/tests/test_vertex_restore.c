#include "../vertex_restore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void check(int value) { if (!value) abort(); }
int main(void) {
    static GpVertexRestore state;
    static float vertices[GP_VERTEX_SLOTS][3];
    unsigned i;
    short xyz[3] = {-3, 7, 0x1234};
    short original[3];
    memcpy(original, xyz, sizeof(xyz));
    check(gp_vertex_scale(&state, xyz, 2));
    check(xyz[0] == -6 && xyz[1] == 14 && xyz[2] == 0x1234);
    check(gp_vertex_scale(&state, xyz, 2) && xyz[0] == -6);
    check(gp_vertex_restore_complete(&state, NULL) == 1 && !memcmp(xyz, original, sizeof(xyz)));
    for (i = 0; i < 1000; ++i) {
        vertices[0][0] = 40; vertices[0][1] = 70; vertices[0][2] = 123;
        check(gp_vertex_scale(&state, vertices[0], 4));
        check(vertices[0][0] == 80 && vertices[0][1] == 140 && vertices[0][2] == 123);
        check(gp_vertex_restore_complete(&state, NULL) == 1 && !state.used);
        check(vertices[0][0] == 40 && vertices[0][1] == 70);
    }
    for (i = 0; i < GP_VERTEX_SLOTS * 3u / 4u; ++i) {
        vertices[i][0] = (float)i; vertices[i][1] = 5; vertices[i][2] = 123;
        check(gp_vertex_scale(&state, vertices[i], 4));
    }
    vertices[i][0] = 37; vertices[i][1] = 48;
    check(!gp_vertex_scale(&state, vertices[i], 4));
    check(vertices[i][0] == 37 && vertices[i][1] == 48 && state.exhausted == 1);
    check(gp_vertex_restore_complete(&state, NULL) == GP_VERTEX_SLOTS * 3u / 4u);
    for (i = 0; i < GP_VERTEX_SLOTS * 3u / 4u; ++i)
        check(vertices[i][0] == (float)i && vertices[i][1] == 5 && vertices[i][2] == 123);
    xyz[0] = 20000; xyz[1] = 5;
    check(!gp_vertex_scale(&state, xyz, 2) && xyz[0] == 20000 && xyz[1] == 5 && !state.used);
    puts("PASS: 1000 reuse epochs, signed XY/depth, capacity/atomic rejection and exact restoration");
    return 0;
}
