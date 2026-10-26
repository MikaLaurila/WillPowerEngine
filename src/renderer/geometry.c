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

void wpe__renderer_setup_triangles(void* context, uint32_t begin, uint32_t end, uint32_t thread_index) {
    (void)thread_index;
    WPE_Renderer* renderer = context;
    static const float no_varyings[WPE_MAX_VARYINGS];

    for (uint32_t j = begin; j < end; ++j) {
        const wpe__Job* job = &renderer->tri_jobs[j];
        const wpe__Draw* draw = renderer->sorted[job->draw];
        const WPE_DrawDesc* desc = &draw->desc;
        const uint32_t varying_count = desc->program->varying_count;
        wpe__TriOutput* output = &renderer->tri_outputs[j];
        output->count = 0;
        output->culled = 0;
        output->clipped = 0;

        for (uint32_t t = job->begin; t < job->end; ++t) {
            const WPE_Vec4* positions[3];
            const float* varyings[3];
            uint32_t codes[3];
            bool finite = true;
            for (int i = 0; i < 3; ++i) {
                const uint32_t vertex =
                    desc->indices ? desc->indices[t * 3 + (uint32_t)i] : t * 3 + (uint32_t)i;
                positions[i] = &renderer->positions[draw->vertex_base + vertex];
                varyings[i] = varying_count
                                  ? renderer->varyings + draw->varying_base + (size_t)vertex * varying_count
                                  : no_varyings;
                const WPE_Vec4 p = *positions[i];
                finite = finite && isfinite(p.x) && isfinite(p.y) && isfinite(p.z) && isfinite(p.w);
                codes[i] = wpe__outcode(p);
            }

            /* Non-finite positions would poison clipping and fixed-point conversion; drop them. */
            if (!finite || (codes[0] & codes[1] & codes[2])) {
                ++output->clipped; /* Entirely outside one plane. */
                continue;
            }
            const uint32_t crossing = codes[0] | codes[1] | codes[2];
            if (crossing == 0) {
                wpe__setup_triangle(renderer, output, job->draw, desc, positions, varyings);
            } else {
                ++output->clipped;
                wpe__clip_and_setup(renderer, output, job->draw, desc, positions, varyings, crossing);
            }
        }
    }
}
