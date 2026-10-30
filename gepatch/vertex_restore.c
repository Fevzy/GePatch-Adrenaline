#include "vertex_restore.h"
#include <float.h>
#include <string.h>

unsigned gp_scale_num = 2, gp_scale_den = 1, gp_scale_num_y = 2;

/* x and y at the current scale; 0 if out of range. No division: the scales
 * are 2/1, 3/2 and 1/1, and an integer division costs this CPU tens of cycles
 * (four of them a vertex took most of a 2D draw's time). */
static int scale_xy_at(const uint32_t original[2], uint32_t scaled[2], unsigned width, unsigned num_x,
                       unsigned num_y, unsigned den) {
    static unsigned factor_num, factor_den;
    static float factor;
    const unsigned shift = den == 2u ? 1u : 0u; /* den 1 or 2 */
    unsigned axis;
    if (den != 1u && den != 2u) return 0;
    for (axis = 0; axis != 2u; ++axis) {
        const unsigned num = axis ? num_y : num_x; /* (960x544i: x2 across, x1 down) */
        if (width == 2u) {
            const int32_t value = (int16_t)original[axis];
            const uint32_t magnitude = (uint32_t)(value < 0 ? -value : value);
            /* to nearest, half away from zero */
            const uint32_t result = (magnitude * num + (shift ? 1u : 0u)) >> shift;
            if (result > (value < 0 ? 32768u : 32767u)) return 0;
            scaled[axis] = (uint16_t)(value < 0 ? -(int32_t)result : (int32_t)result);
        } else {
            float value, result;
            if (factor_num != num || factor_den != den) {
                factor_num = num; factor_den = den;
                factor = (float)num / (float)den;
            }
            memcpy(&value, &original[axis], 4);
            if (!(value >= -FLT_MAX / 2 && value <= FLT_MAX / 2)) return 0;
            result = value * factor;
            memcpy(&scaled[axis], &result, 4);
        }
    }
    return 1;
}

static unsigned page_of(uintptr_t address) { return (unsigned)(address >> 12) & (GP_VERTEX_PAGES - 1u); }
static int scale_xy(const uint32_t original[2], uint32_t scaled[2], unsigned width) {
    return scale_xy_at(original, scaled, width, gp_scale_num, gp_scale_num_y, gp_scale_den);
}

/* Does `p` still hold what was written there from `original` at `scale`
 * (num_y << 12 | num << 8 | den; the original itself where it could not be scaled)? If
 * not, the game has written the position again: it is its own now. */
static int holds_ours(const unsigned char *p, const uint32_t original[2], unsigned width, unsigned scale) {
    uint32_t ours[2] = {0, 0}, now[2] = {0, 0};
    if (!scale_xy_at(original, ours, width, (scale >> 8) & 0xfu, (scale >> 12) & 0xfu, scale & 0xffu)) {
        ours[0] = original[0]; ours[1] = original[1];
    }
    memcpy(&now[0], p, width);
    memcpy(&now[1], p + width, width);
    if (width == 2u) { ours[0] &= 0xffffu; ours[1] &= 0xffffu; }
    return now[0] == ours[0] && now[1] == ours[1];
}

/* A range already holds this position (scaled with it). */
static const GpVertexRange *in_ranges(const GpVertexRestore *state, uintptr_t address, unsigned width) {
    unsigned link = state->page_head[page_of(address)];
    for (; link; link = state->link_next[link - 1u]) {
        const GpVertexRange *r = &state->ranges[state->link_range[link - 1u]];
        if (address >= r->start && address < r->end && (address - r->start) % r->stride == 0 && r->width == width)
            return r;
    }
    return NULL;
}

/* Files range `index` under each page it covers; 0 when out of links. */
static int link_pages(GpVertexRestore *state, unsigned index) {
    const GpVertexRange *r = &state->ranges[index];
    const uintptr_t first = r->start >> 12, last = (r->end - 1u) >> 12;
    uintptr_t page;
    if (last - first + 1u > GP_VERTEX_PAGES || state->link_count + (unsigned)(last - first + 1u) > GP_VERTEX_LINKS) return 0;
    for (page = first; page <= last; ++page) {
        const unsigned bucket = (unsigned)page & (GP_VERTEX_PAGES - 1u);
        state->link_range[state->link_count] = (uint16_t)index;
        state->link_next[state->link_count] = state->page_head[bucket];
        state->page_head[bucket] = (uint16_t)(++state->link_count);
    }
    return 1;
}

static uint16_t current_scale(void) { return (uint16_t)(gp_scale_num_y << 12 | gp_scale_num << 8 | gp_scale_den); }

static void write_xy(unsigned char *p, const uint32_t v[2], unsigned width) {
    memcpy(p, &v[0], width);
    memcpy(p + width, &v[1], width);
}

int gp_vertex_scale_run(GpVertexRestore *state, void *first, unsigned count, unsigned stride, unsigned width) {
    const uintptr_t start = (uintptr_t)first;
    uintptr_t end;
    unsigned i, ok = 1;
    if (!count) return 1;
    if ((width != 2u && width != 4u) || stride < 2u * width) { ++state->unsupported; return 0; }
    ++state->runs;
    end = start + (uintptr_t)(count - 1u) * stride + 2u * width;
    /* Vertices are usually taken in order (SC's sit in the list itself):
     * past every range so far, none can overlap. */
    if (start < state->ranges_end) {
      uintptr_t page;
      for (page = start >> 12; page <= (end - 1u) >> 12; ++page) {
       unsigned link = state->page_head[(unsigned)page & (GP_VERTEX_PAGES - 1u)];
       for (; link; link = state->link_next[link - 1u]) {
        GpVertexRange *r = &state->ranges[state->link_range[link - 1u]];
        if (start < r->end && r->start < end) {
            if (r->start == start && r->end == end && r->stride == stride && r->width == width) {
                /* the same draw parsed again before the GE finished: kept,
                 * rescaled after a mode change, and positions the game has
                 * written again since (a game that never waits for the GE
                 * refills its buffers each frame) taken as new */
                unsigned k;
                const unsigned was = r->scale;
                r->scale = current_scale();
                for (k = 0; k < r->count; ++k) {
                    unsigned char *p = (unsigned char *)(start + (uintptr_t)k * stride);
                    uint32_t *original = state->log[r->first + k], scaled[2];
                    if (holds_ours(p, original, width, was)) {
                        if (was == r->scale) continue;
                    } else {
                        original[0] = original[1] = 0;
                        memcpy(&original[0], p, width);
                        memcpy(&original[1], p + width, width);
                    }
                    if (scale_xy(original, scaled, width)) write_xy(p, scaled, width);
                    else ok = 0;
                }
                return (int)ok;
            }
            goto one_by_one; /* in part: vertex by vertex */
        }
       }
      }
    }
    if (state->range_count >= GP_VERTEX_RANGES || state->log_used + count > GP_VERTEX_LOG) goto one_by_one;
    {
        GpVertexRange *r = &state->ranges[state->range_count];
        unsigned k;
        r->start = start; r->end = end; r->stride = stride; r->width = (uint16_t)width;
        r->scale = current_scale(); r->first = state->log_used; r->count = count;
        if (width == 2u && !((start | stride) & 1u) && (gp_scale_den == 1u || gp_scale_den == 2u)) {
            /* s16, the common case: straight loads and stores (a memcpy call
             * per coordinate made a vertex cost ~1000 cycles) */
            const uint32_t num = gp_scale_num, num_y = gp_scale_num_y, shift = gp_scale_den == 2u ? 1u : 0u;
            uint32_t (*log)[2] = &state->log[state->log_used];
            for (k = 0; k < count; ++k) {
                int16_t *p = (int16_t *)(start + (uintptr_t)k * stride);
                const int32_t x = p[0], y = p[1];
                const uint32_t ax = (uint32_t)(x < 0 ? -x : x), ay = (uint32_t)(y < 0 ? -y : y);
                const uint32_t rx = (ax * num + shift) >> shift, ry = (ay * num_y + shift) >> shift;
                log[k][0] = (uint16_t)x; log[k][1] = (uint16_t)y;
                if (rx > (x < 0 ? 32768u : 32767u) || ry > (y < 0 ? 32768u : 32767u)) { ok = 0; ++state->unsupported; continue; }
                p[0] = (int16_t)(x < 0 ? -(int32_t)rx : (int32_t)rx);
                p[1] = (int16_t)(y < 0 ? -(int32_t)ry : (int32_t)ry);
            }
        } else if (width == 4u && !((start | stride) & 3u)) {
            const float factor = (float)gp_scale_num / (float)gp_scale_den;
            const float factor_y = (float)gp_scale_num_y / (float)gp_scale_den;
            uint32_t (*log)[2] = &state->log[state->log_used];
            for (k = 0; k < count; ++k) {
                float *p = (float *)(start + (uintptr_t)k * stride);
                const float x = p[0], y = p[1];
                memcpy(log[k], p, 8);
                if (!(x >= -FLT_MAX / 2 && x <= FLT_MAX / 2 && y >= -FLT_MAX / 2 && y <= FLT_MAX / 2)) {
                    ok = 0; ++state->unsupported; continue;
                }
                p[0] = x * factor;
                p[1] = y * factor_y;
            }
        } else {
            for (k = 0; k < count; ++k) {
                unsigned char *p = (unsigned char *)(start + (uintptr_t)k * stride);
                uint32_t *original = state->log[state->log_used + k], scaled[2];
                original[0] = original[1] = 0;
                memcpy(&original[0], p, width);
                memcpy(&original[1], p + width, width);
                if (scale_xy(original, scaled, width)) write_xy(p, scaled, width);
                else { ok = 0; ++state->unsupported; } /* kept as it is; put back as it was */
            }
        }
        if (!link_pages(state, state->range_count)) { /* unfindable: undo, one by one */
            for (k = 0; k < count; ++k)
                write_xy((unsigned char *)(start + (uintptr_t)k * stride), state->log[state->log_used + k], width);
            goto one_by_one;
        }
        state->log_used += count;
        ++state->range_count;
        if (state->peak_ranges < state->range_count) state->peak_ranges = state->range_count;
        if (state->ranges_end < end) state->ranges_end = end;
        return (int)ok;
    }
one_by_one:
    ++state->one_by_one;
    for (i = 0; i < count; ++i)
        ok &= (unsigned)gp_vertex_scale(state, (void *)(start + (uintptr_t)i * stride), width);
    return (int)ok;
}

int gp_vertex_scale(GpVertexRestore *state, void *position_xy, unsigned width) {
    uintptr_t address = (uintptr_t)position_xy;
    {
        const GpVertexRange *r = state->range_count ? in_ranges(state, address, width) : NULL;
        if (r) { /* a run scaled it -- unless the game has written it again since */
            uint32_t *original = state->log[r->first + (unsigned)((address - r->start) / r->stride)], scaled[2];
            if (holds_ours((unsigned char *)position_xy, original, width, r->scale)) return r->scale == current_scale();
            original[0] = original[1] = 0;
            memcpy(&original[0], position_xy, width);
            memcpy(&original[1], (unsigned char *)position_xy + width, width);
            if (r->scale != current_scale() || !scale_xy(original, scaled, width)) return 0;
            write_xy((unsigned char *)position_xy, scaled, width);
            return 1;
        }
    }
    unsigned slot = (unsigned)(((address >> 2) ^ (address >> 15)) & (GP_VERTEX_SLOTS - 1u));
    uint32_t original[2] = {0, 0}, scaled[2] = {0, 0};
    unsigned axis;
    if (width != 2u && width != 4u) { ++state->unsupported; return 0; }
    while (state->entries[slot].address) {
        GpVertexEntry *entry = &state->entries[slot];
        if (entry->address == address) {
            if ((entry->width & 0xffu) != width) { ++state->unsupported; return 0; }
            /* parsed again before the GE finished: as it is, unless the mode
             * changed the scale since -- then from the game's own value --
             * or the game wrote the position again (its new value then) */
            if (!holds_ours((unsigned char *)position_xy, entry->original, width, entry->width >> 8)) {
                entry->original[0] = entry->original[1] = 0;
                memcpy(&entry->original[0], position_xy, width);
                memcpy(&entry->original[1], (unsigned char *)position_xy + width, width);
            } else if (entry->width >> 8 == current_scale()) {
                return 1;
            }
            memcpy(original, entry->original, sizeof(original));
            entry->width = width | current_scale() << 8;
            if (!scale_xy(original, scaled, width)) return 1; /* kept as it is */
            for (axis = 0; axis != 2u; ++axis)
                memcpy((unsigned char *)position_xy + axis * width, &scaled[axis], width);
            return 1;
        }
        slot = (slot + 1u) & (GP_VERTEX_SLOTS - 1u);
    }
    if (state->used >= GP_VERTEX_SLOTS * 3u / 4u) { ++state->exhausted; return 0; }
    for (axis = 0; axis != 2u; ++axis)
        memcpy(&original[axis], (unsigned char *)position_xy + axis * width, width);
    if (!scale_xy(original, scaled, width)) { ++state->unsupported; return 0; }
    state->entries[slot].address = address;
    state->entries[slot].width = width | current_scale() << 8;
    state->entries[slot].original[0] = original[0];
    state->entries[slot].original[1] = original[1];
    state->used_slots[state->used++] = (uint16_t)slot;
    if (state->peak < state->used) state->peak = state->used;
    for (axis = 0; axis != 2u; ++axis)
        memcpy((unsigned char *)position_xy + axis * width, &scaled[axis], width);
    return 1;
}

unsigned gp_vertex_restore_complete(GpVertexRestore *state, void (*written)(const void *, unsigned)) {
    unsigned i, axis, restored = state->used + state->log_used;
    for (i = 0; i < state->range_count; ++i) {
        const GpVertexRange *r = &state->ranges[i];
        unsigned k;
        /* only where our value still stands: what the game wrote since is its own */
        for (k = 0; k < r->count; ++k) {
            unsigned char *p = (unsigned char *)(r->start + (uintptr_t)k * r->stride);
            if (holds_ours(p, state->log[r->first + k], r->width, r->scale))
                write_xy(p, state->log[r->first + k], r->width);
        }
        if (written) written((const void *)r->start, (unsigned)(r->end - r->start));
    }
    if (state->range_count) memset(state->page_head, 0, sizeof(state->page_head));
    state->range_count = state->log_used = state->link_count = 0;
    state->ranges_end = 0;
    for (i = 0; i < state->used; ++i) {
        GpVertexEntry *entry = &state->entries[state->used_slots[i]];
        if (holds_ours((const unsigned char *)entry->address, entry->original, entry->width & 0xffu, entry->width >> 8))
        for (axis = 0; axis != 2u; ++axis)
            memcpy((void *)(entry->address + axis * (entry->width & 0xffu)), &entry->original[axis], entry->width & 0xffu);
        if (written) written((const void *)entry->address, 2u * (entry->width & 0xffu));
        entry->address = 0;
    }
    state->used = 0;
    return restored;
}
