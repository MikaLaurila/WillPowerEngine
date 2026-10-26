#define WPE_LOG_MODULE "renderer"
#include <math.h>
#include <string.h>

#include "core/alloc.h"
#include "core/debug.h"
#include "core/log.h"
#include "renderer/renderer_internal.h"

/*
 * Clip planes in homogeneous space; a vertex is inside a plane when its distance is >= 0.
 * The w-epsilon plane guarantees a positive w for the perspective divide even for matrices whose
 * near plane does not imply it (e.g. hand-built projections).
 */
enum {
    WPE__PLANE_W_EPSILON,
    WPE__PLANE_NEAR,
    WPE__PLANE_FAR,
    WPE__PLANE_LEFT,
    WPE__PLANE_RIGHT,
    WPE__PLANE_BOTTOM,
    WPE__PLANE_TOP,
    WPE__PLANE_COUNT
};

#define WPE__CLIP_W_EPSILON 1e-5f
/* Sutherland-Hodgman adds at most one vertex per plane. */
#define WPE__CLIP_MAX_VERTICES (3 + WPE__PLANE_COUNT)

typedef struct wpe__ClipVertex {
    WPE_Vec4 position;
    float varyings[WPE_MAX_VARYINGS];
} wpe__ClipVertex;

static float wpe__plane_distance(int plane, WPE_Vec4 p) {
    switch (plane) {
    case WPE__PLANE_W_EPSILON:
        return p.w - WPE__CLIP_W_EPSILON;
    case WPE__PLANE_NEAR:
        return p.z + p.w;
    case WPE__PLANE_FAR:
        return p.w - p.z;
    case WPE__PLANE_LEFT:
        return p.x + WPE__RASTER_GUARD_BAND * p.w;
    case WPE__PLANE_RIGHT:
        return WPE__RASTER_GUARD_BAND * p.w - p.x;
    case WPE__PLANE_BOTTOM:
        return p.y + WPE__RASTER_GUARD_BAND * p.w;
    case WPE__PLANE_TOP:
        return WPE__RASTER_GUARD_BAND * p.w - p.y;
    default:
        wpe_unreachable();
    }
}

/* Bit i is set when the position is outside plane i. NaN coordinates count as outside. */
static uint32_t wpe__outcode(WPE_Vec4 p) {
    uint32_t code = 0;
    for (int plane = 0; plane < WPE__PLANE_COUNT; ++plane) {
        if (!(wpe__plane_distance(plane, p) >= 0.0f)) code |= 1u << plane;
    }
    return code;
}

/* ---- Vertex stage --------------------------------------------------------------------------------- */

void wpe__renderer_shade_vertices(void* context, uint32_t begin, uint32_t end, uint32_t thread_index) {
    (void)thread_index;
    WPE_Renderer* renderer = context;
    for (uint32_t j = begin; j < end; ++j) {
        const wpe__Job* job = &renderer->vertex_jobs[j];
        const wpe__Draw* draw = renderer->sorted[job->draw];
        const WPE_DrawDesc* desc = &draw->desc;
        const uint32_t varying_count = desc->program->varying_count;
        const unsigned char* vertices = desc->vertices;

        for (uint32_t v = job->begin; v < job->end; ++v) {
            float scratch[WPE_MAX_VARYINGS];
            float* out_varyings =
                varying_count ? renderer->varyings + draw->varying_base + (size_t)v * varying_count : scratch;
            desc->program->vertex(desc->uniforms, vertices + (size_t)v * desc->vertex_stride,
                                  &renderer->positions[draw->vertex_base + v], out_varyings);
        }
    }
}

/* ---- Triangle setup ------------------------------------------------------------------------------- */

static int32_t wpe__floor_div(int32_t a, int32_t b) {
    const int32_t q = a / b;
    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

/* Returns a slot in the job's output, growing it if needed; NULL (and the OOM flag) on failure. */
static wpe__SetupTri* wpe__output_push(WPE_Renderer* renderer, wpe__TriOutput* output) {
    if (output->count == output->capacity) {
        const uint32_t capacity = output->capacity ? output->capacity * 2 : WPE__TRIANGLE_JOB_SIZE;
        wpe__SetupTri* tris = wpe_realloc(output->tris, (size_t)capacity * sizeof *tris);
        if (!tris) {
            atomic_store(&renderer->out_of_memory, true);
            return NULL;
        }
        output->tris = tris;
        output->capacity = capacity;
    }
    return &output->tris[output->count++];
}

/*
 * Projects a clip-space triangle (all vertices inside the clip volume), culls it and appends the
 * setup result to `output`.
 */
static void wpe__setup_triangle(WPE_Renderer* renderer, wpe__TriOutput* output, uint32_t draw_index,
                                const WPE_DrawDesc* desc, const WPE_Vec4* positions[3],
                                const float* varyings[3]) {
    const float width = (float)renderer->framebuffer.width;
    const float height = (float)renderer->framebuffer.height;
    const uint32_t varying_count = desc->program->varying_count;

    int32_t x[3], y[3];
    float z[3], inv_w[3];
    for (int i = 0; i < 3; ++i) {
        const WPE_Vec4 p = *positions[i];
        inv_w[i] = 1.0f / p.w;
        const float sx = (p.x * inv_w[i] * 0.5f + 0.5f) * width;
        const float sy = (0.5f - p.y * inv_w[i] * 0.5f) * height;
        x[i] = (int32_t)floorf(sx * (float)WPE__RASTER_SUBPIXEL_ONE + 0.5f);
        y[i] = (int32_t)floorf(sy * (float)WPE__RASTER_SUBPIXEL_ONE + 0.5f);
        const float depth = p.z * inv_w[i] * 0.5f + 0.5f;
        z[i] = depth < 0.0f ? 0.0f : (depth > 1.0f ? 1.0f : depth);
    }

    int64_t area = (int64_t)(x[1] - x[0]) * (y[2] - y[0]) - (int64_t)(x[2] - x[0]) * (y[1] - y[0]);
    if (area == 0) {
        ++output->culled;
        return;
    }
    /* Counter-clockwise in NDC (y up) becomes clockwise, i.e. negative area, in y-down screen space. */
    const bool front_facing = area < 0;
    if ((desc->state.cull == WPE_CULL_BACK && !front_facing) ||
        (desc->state.cull == WPE_CULL_FRONT && front_facing)) {
        ++output->culled;
        return;
    }

    /* Normalize to positive area by swapping vertices 1 and 2 so the rasterizer has one winding. */
    int order[3] = {0, 1, 2};
    if (area < 0) {
        order[1] = 2;
        order[2] = 1;
        area = -area;
    }

    int32_t min_x = x[0], max_x = x[0], min_y = y[0], max_y = y[0];
    for (int i = 1; i < 3; ++i) {
        min_x = x[i] < min_x ? x[i] : min_x;
        max_x = x[i] > max_x ? x[i] : max_x;
        min_y = y[i] < min_y ? y[i] : min_y;
        max_y = y[i] > max_y ? y[i] : max_y;
    }
    /* Pixels whose centers (p * 16 + 8) lie within the fixed-point bounds. */
    const int32_t half = WPE__RASTER_SUBPIXEL_ONE / 2;
    int32_t min_px = wpe__floor_div(min_x - half + WPE__RASTER_SUBPIXEL_ONE - 1, WPE__RASTER_SUBPIXEL_ONE);
    int32_t min_py = wpe__floor_div(min_y - half + WPE__RASTER_SUBPIXEL_ONE - 1, WPE__RASTER_SUBPIXEL_ONE);
    int32_t max_px = wpe__floor_div(max_x - half, WPE__RASTER_SUBPIXEL_ONE);
    int32_t max_py = wpe__floor_div(max_y - half, WPE__RASTER_SUBPIXEL_ONE);
    min_px = min_px < 0 ? 0 : min_px;
    min_py = min_py < 0 ? 0 : min_py;
    max_px =
        max_px > (int32_t)renderer->framebuffer.width - 1 ? (int32_t)renderer->framebuffer.width - 1 : max_px;
    max_py = max_py > (int32_t)renderer->framebuffer.height - 1 ? (int32_t)renderer->framebuffer.height - 1
                                                                : max_py;
    if (min_px > max_px || min_py > max_py) return; /* Covers no pixel center on screen. */

    wpe__SetupTri* tri = wpe__output_push(renderer, output);
    if (!tri) return;
    tri->min_px = min_px;
    tri->min_py = min_py;
    tri->max_px = max_px;
    tri->max_py = max_py;
    tri->area = area;
    tri->draw = draw_index;
    tri->front_facing = front_facing;
    for (int i = 0; i < 3; ++i) {
        const int src = order[i];
        tri->x[i] = x[src];
        tri->y[i] = y[src];
        tri->z[i] = z[src];
        tri->inv_w[i] = inv_w[src];
        for (uint32_t k = 0; k < varying_count; ++k)
            tri->varyings[i][k] = varyings[src][k] * inv_w[src];
    }
}

/* Clips a triangle against the planes in `planes_mask` and sets up the resulting fan. */
static void wpe__clip_and_setup(WPE_Renderer* renderer, wpe__TriOutput* output, uint32_t draw_index,
                                const WPE_DrawDesc* desc, const WPE_Vec4* positions[3],
                                const float* varyings[3], uint32_t planes_mask) {
    const uint32_t varying_count = desc->program->varying_count;
    wpe__ClipVertex buffers[2][WPE__CLIP_MAX_VERTICES];
    wpe__ClipVertex* in = buffers[0];
    wpe__ClipVertex* out = buffers[1];
    int in_count = 3;
    for (int i = 0; i < 3; ++i) {
        in[i].position = *positions[i];
        memcpy(in[i].varyings, varyings[i], varying_count * sizeof(float));
    }

    for (int plane = 0; plane < WPE__PLANE_COUNT && in_count >= 3; ++plane) {
        if (!(planes_mask & (1u << plane))) continue;
        int out_count = 0;
        for (int i = 0; i < in_count; ++i) {
            const wpe__ClipVertex* a = &in[i];
            const wpe__ClipVertex* b = &in[(i + 1) % in_count];
            const float da = wpe__plane_distance(plane, a->position);
            const float db = wpe__plane_distance(plane, b->position);
            const bool a_inside = da >= 0.0f;
            const bool b_inside = db >= 0.0f;
            if (a_inside) out[out_count++] = *a;
            if (a_inside != b_inside) {
                const float t = da / (da - db);
                wpe__ClipVertex* v = &out[out_count++];
                v->position = wpe_vec4(a->position.x + (b->position.x - a->position.x) * t,
                                       a->position.y + (b->position.y - a->position.y) * t,
                                       a->position.z + (b->position.z - a->position.z) * t,
                                       a->position.w + (b->position.w - a->position.w) * t);
                for (uint32_t k = 0; k < varying_count; ++k) {
                    v->varyings[k] = a->varyings[k] + (b->varyings[k] - a->varyings[k]) * t;
                }
            }
        }
        wpe_assert(out_count <= WPE__CLIP_MAX_VERTICES);
        wpe__ClipVertex* swap = in;
        in = out;
        out = swap;
        in_count = out_count;
    }

    for (int i = 1; i + 1 < in_count; ++i) {
        const WPE_Vec4* fan_positions[3] = {&in[0].position, &in[i].position, &in[i + 1].position};
        const float* fan_varyings[3] = {in[0].varyings, in[i].varyings, in[i + 1].varyings};
        wpe__setup_triangle(renderer, output, draw_index, desc, fan_positions, fan_varyings);
    }
}

static bool wpe__finite4(WPE_Vec4 p) {
    return isfinite(p.x) && isfinite(p.y) && isfinite(p.z) && isfinite(p.w);
}

/* Rejects, clips or directly sets up one shaded triangle. */
static void wpe__process_triangle(WPE_Renderer* renderer, wpe__TriOutput* output, uint32_t draw_index,
                                  const WPE_DrawDesc* desc, const WPE_Vec4* positions[3],
                                  const float* varyings[3]) {
    uint32_t codes[3];
    bool finite = true;
    for (int i = 0; i < 3; ++i) {
        finite = finite && wpe__finite4(*positions[i]);
        codes[i] = wpe__outcode(*positions[i]);
    }
    /* Non-finite positions would poison clipping and fixed-point conversion; drop them. */
    if (!finite || (codes[0] & codes[1] & codes[2])) {
        ++output->clipped; /* Entirely outside one plane. */
        return;
    }
    const uint32_t crossing = codes[0] | codes[1] | codes[2];
    if (crossing == 0) {
        wpe__setup_triangle(renderer, output, draw_index, desc, positions, varyings);
    } else {
        ++output->clipped;
        wpe__clip_and_setup(renderer, output, draw_index, desc, positions, varyings, crossing);
    }
}

/* ---- Adaptive subdivision ------------------------------------------------------------------------- */

/*
 * Whether an edge must be split. Depends only on the two endpoint positions (symmetric), so both
 * triangles sharing an edge make the same decision and no T-junctions appear.
 */
static bool wpe__edge_needs_split(const WPE_Renderer* renderer, WPE_Vec4 a, WPE_Vec4 b, float threshold) {
    const bool a_front = a.w > WPE__CLIP_W_EPSILON, b_front = b.w > WPE__CLIP_W_EPSILON;
    if (!a_front && !b_front) return false; /* Entirely behind the camera: will be rejected. */
    if (a_front != b_front) return true;    /* Crosses the camera plane: refine near the viewer. */
    const float dx = (a.x / a.w - b.x / b.w) * 0.5f * (float)renderer->framebuffer.width;
    const float dy = (a.y / a.w - b.y / b.w) * 0.5f * (float)renderer->framebuffer.height;
    return dx * dx + dy * dy > threshold * threshold;
}

/* Total order on positions so an edge's midpoint is computed identically from either side. */
static bool wpe__position_less(WPE_Vec4 a, WPE_Vec4 b) {
    for (int i = 0; i < 4; ++i) {
        if (a.v[i] != b.v[i]) return a.v[i] < b.v[i];
    }
    return false;
}

/* Returns the midpoint vertex of edge (i, j), creating and shading it on first use; UINT16_MAX when full. */
static uint16_t wpe__subdiv_midpoint(wpe__ThreadScratch* s, const WPE_DrawDesc* desc, uint32_t stride,
                                     uint16_t i, uint16_t j) {
    if (wpe__position_less(s->vertices[j].position, s->vertices[i].position)) {
        const uint16_t swap = i;
        i = j;
        j = swap;
    }
    for (uint32_t m = 0; m < s->midpoint_count; ++m) {
        if (s->midpoint_keys[m][0] == i && s->midpoint_keys[m][1] == j) return s->midpoint_keys[m][2];
    }
    if (s->vertex_count >= WPE__SUBDIV_MAX_VERTICES) return UINT16_MAX;

    const uint16_t index = (uint16_t)s->vertex_count++;
    wpe__SubdivVertex* v = &s->vertices[index];
    unsigned char* input = s->inputs + (size_t)index * stride;
    desc->program->vertex_lerp(s->vertices[i].input, s->vertices[j].input, 0.5f, input);
    v->input = input;
    desc->program->vertex(desc->uniforms, input, &v->position, v->varyings);
    s->midpoint_keys[s->midpoint_count][0] = i;
    s->midpoint_keys[s->midpoint_count][1] = j;
    s->midpoint_keys[s->midpoint_count][2] = index;
    ++s->midpoint_count;
    return index;
}

typedef struct wpe__SubdivTri {
    uint16_t v[3];
    uint8_t level;
} wpe__SubdivTri;

/*
 * Refines one source triangle with edge-based (red-green) splitting: each long edge is halved and
 * the triangle is re-triangulated according to how many of its edges were split (1, 2 or 3).
 * Leaf triangles keep the source winding and go through the regular clip/setup path.
 */
static void wpe__subdivide_triangle(WPE_Renderer* renderer, wpe__TriOutput* output, wpe__ThreadScratch* s,
                                    uint32_t draw_index, const WPE_DrawDesc* desc, const void* inputs[3],
                                    const WPE_Vec4* positions[3], const float* varyings[3]) {
    const uint32_t varying_count = desc->program->varying_count;
    const uint32_t stride = desc->vertex_stride;
    const float threshold = desc->subdivision_edge_pixels > 1.0f ? desc->subdivision_edge_pixels : 1.0f;
    const uint8_t max_level =
        (uint8_t)(desc->subdivision_levels < WPE_MAX_SUBDIVISION_LEVELS ? desc->subdivision_levels
                                                                        : WPE_MAX_SUBDIVISION_LEVELS);
    s->vertex_count = 3;
    s->midpoint_count = 0;
    for (int i = 0; i < 3; ++i) {
        s->vertices[i].input = inputs[i];
        s->vertices[i].position = *positions[i];
        memcpy(s->vertices[i].varyings, varyings[i], varying_count * sizeof(float));
    }

    wpe__SubdivTri stack[WPE__SUBDIV_STACK];
    uint32_t top = 0;
    stack[top++] = (wpe__SubdivTri){.v = {0, 1, 2}, .level = 0};
    bool split_any = false;

    while (top > 0) {
        const wpe__SubdivTri t = stack[--top];
        bool split[3] = {false, false, false}; /* edge k runs from v[k] to v[(k + 1) % 3] */
        int split_count = 0;
        if (t.level < max_level) {
            for (int k = 0; k < 3; ++k) {
                split[k] = wpe__edge_needs_split(renderer, s->vertices[t.v[k]].position,
                                                 s->vertices[t.v[(k + 1) % 3]].position, threshold);
                split_count += split[k];
            }
        }

        uint16_t mid[3] = {UINT16_MAX, UINT16_MAX, UINT16_MAX};
        bool ok = split_count > 0 && top + 4 <= WPE__SUBDIV_STACK;
        for (int k = 0; k < 3 && ok; ++k) {
            if (!split[k]) continue;
            mid[k] = wpe__subdiv_midpoint(s, desc, stride, t.v[k], t.v[(k + 1) % 3]);
            ok = mid[k] != UINT16_MAX;
        }
        if (!ok) {
            const WPE_Vec4* leaf_positions[3];
            const float* leaf_varyings[3];
            for (int i = 0; i < 3; ++i) {
                leaf_positions[i] = &s->vertices[t.v[i]].position;
                leaf_varyings[i] = s->vertices[t.v[i]].varyings;
            }
            wpe__process_triangle(renderer, output, draw_index, desc, leaf_positions, leaf_varyings);
            continue;
        }
        split_any = true;

        /* Rotate so the pattern is canonical: for one split it is edge 0, for two it is edges 0 and 1. */
        int r = 0;
        if (split_count == 1) r = split[0] ? 0 : (split[1] ? 1 : 2);
        if (split_count == 2) r = !split[2] ? 0 : (!split[0] ? 1 : 2);
        const uint16_t v0 = t.v[r], v1 = t.v[(r + 1) % 3], v2 = t.v[(r + 2) % 3];
        const uint16_t m01 = mid[r], m12 = mid[(r + 1) % 3], m20 = mid[(r + 2) % 3];
        const uint8_t level = (uint8_t)(t.level + 1);
        if (split_count == 1) {
            stack[top++] = (wpe__SubdivTri){.v = {v0, m01, v2}, .level = level};
            stack[top++] = (wpe__SubdivTri){.v = {m01, v1, v2}, .level = level};
        } else if (split_count == 2) {
            stack[top++] = (wpe__SubdivTri){.v = {m01, v1, m12}, .level = level};
            stack[top++] = (wpe__SubdivTri){.v = {v0, m01, m12}, .level = level};
            stack[top++] = (wpe__SubdivTri){.v = {v0, m12, v2}, .level = level};
        } else {
            stack[top++] = (wpe__SubdivTri){.v = {v0, m01, m20}, .level = level};
            stack[top++] = (wpe__SubdivTri){.v = {m01, v1, m12}, .level = level};
            stack[top++] = (wpe__SubdivTri){.v = {m20, m12, v2}, .level = level};
            stack[top++] = (wpe__SubdivTri){.v = {m01, m12, m20}, .level = level};
        }
    }
    if (split_any) ++output->subdivided;
}

bool wpe__renderer_reserve_scratch(WPE_Renderer* renderer, uint32_t threads, uint32_t max_stride) {
    if (threads > renderer->thread_scratch_count) {
        wpe__ThreadScratch* scratch =
            wpe_realloc(renderer->thread_scratch, (size_t)threads * sizeof *scratch);
        if (!scratch) return false;
        memset(scratch + renderer->thread_scratch_count, 0,
               (size_t)(threads - renderer->thread_scratch_count) * sizeof *scratch);
        renderer->thread_scratch = scratch;
        renderer->thread_scratch_count = threads;
    }
    const size_t needed = (size_t)max_stride * WPE__SUBDIV_MAX_VERTICES;
    for (uint32_t t = 0; t < threads; ++t) {
        wpe__ThreadScratch* s = &renderer->thread_scratch[t];
        if (s->inputs_capacity >= needed) continue;
        unsigned char* inputs = wpe_realloc(s->inputs, needed);
        if (!inputs) return false;
        s->inputs = inputs;
        s->inputs_capacity = needed;
    }
    return true;
}

/* ---- Triangle stage ------------------------------------------------------------------------------- */

void wpe__renderer_setup_triangles(void* context, uint32_t begin, uint32_t end, uint32_t thread_index) {
    WPE_Renderer* renderer = context;
    static const float no_varyings[WPE_MAX_VARYINGS];

    for (uint32_t j = begin; j < end; ++j) {
        const wpe__Job* job = &renderer->tri_jobs[j];
        const wpe__Draw* draw = renderer->sorted[job->draw];
        const WPE_DrawDesc* desc = &draw->desc;
        const uint32_t varying_count = desc->program->varying_count;
        const unsigned char* vertex_bytes = desc->vertices;
        const bool subdivide = desc->subdivision_levels > 0;
        wpe__TriOutput* output = &renderer->tri_outputs[j];
        output->count = 0;
        output->culled = 0;
        output->clipped = 0;
        output->subdivided = 0;

        for (uint32_t t = job->begin; t < job->end; ++t) {
            const WPE_Vec4* positions[3];
            const float* varyings[3];
            const void* inputs[3];
            for (int i = 0; i < 3; ++i) {
                const uint32_t vertex =
                    desc->indices ? desc->indices[t * 3 + (uint32_t)i] : t * 3 + (uint32_t)i;
                positions[i] = &renderer->positions[draw->vertex_base + vertex];
                varyings[i] = varying_count
                                  ? renderer->varyings + draw->varying_base + (size_t)vertex * varying_count
                                  : no_varyings;
                inputs[i] = vertex_bytes + (size_t)vertex * desc->vertex_stride;
            }

            if (subdivide && wpe__finite4(*positions[0]) && wpe__finite4(*positions[1]) &&
                wpe__finite4(*positions[2])) {
                wpe__subdivide_triangle(renderer, output, &renderer->thread_scratch[thread_index], job->draw,
                                        desc, inputs, positions, varyings);
            } else {
                wpe__process_triangle(renderer, output, job->draw, desc, positions, varyings);
            }
        }
    }
}
