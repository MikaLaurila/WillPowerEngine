#define WPE_LOG_MODULE "graphics"
#include "graphics/debug_lines.h"

#include "core/alloc.h"
#include "core/debug.h"
#include "core/log.h"

/* After transparent scene geometry, before draw2d (UINT64_MAX). */
#define WPE__DEBUG_LINES_SORT_KEY (UINT64_MAX - 1)
/* NDC depth pulled toward the camera so lines lying on surfaces win the depth test. */
#define WPE__DEPTH_BIAS 1e-5f

typedef struct wpe__LineVertex {
    float clip[4];
    float color[4];
} wpe__LineVertex;

struct WPE_DebugLines {
    WPE_Mat4 view_projection;
    float half_width_ndc_x, half_width_ndc_y; /* Half the line width in NDC units per axis. */
    float pixels_x, pixels_y;                 /* NDC to pixel scale per axis. */
    wpe__LineVertex* vertices;                /* 4 per line */
    uint32_t* indices;                        /* 6 per line */
    uint32_t count, capacity;
    bool out_of_memory;
};

static void wpe__vs_line(const void* uniforms, const void* vertex, WPE_Vec4* out_position, float* out) {
    (void)uniforms;
    const wpe__LineVertex* v = vertex;
    *out_position = wpe_vec4(v->clip[0], v->clip[1], v->clip[2], v->clip[3]);
    for (int i = 0; i < 4; ++i)
        out[i] = v->color[i];
}

static bool wpe__fs_line(const void* uniforms, const WPE_FragmentInput* in, WPE_Vec4* out_color) {
    (void)uniforms;
    *out_color = wpe_vec4(in->varyings[0], in->varyings[1], in->varyings[2], in->varyings[3]);
    return true;
}

static const WPE_ShaderProgram wpe__program_line = {
    .vertex = wpe__vs_line, .fragment = wpe__fs_line, .varying_count = 4};

WPE_Result wpe_debug_lines_create(WPE_DebugLines** out_lines) {
    if (!out_lines) return WPE_RESULT_ERROR_NULL_ARG;
    *out_lines = wpe_alloc_zeroed(1, sizeof **out_lines);
    return *out_lines ? WPE_RESULT_OK : WPE_RESULT_ERROR_OUT_OF_MEMORY;
}

void wpe_debug_lines_destroy(WPE_DebugLines* lines) {
    if (!lines) return;
    wpe_free(lines->vertices);
    wpe_free(lines->indices);
    wpe_free(lines);
}

void wpe_debug_lines_begin(WPE_DebugLines* lines, const WPE_Mat4* view_projection, uint32_t width,
                           uint32_t height, float line_width) {
    if (!wpe_check(lines != NULL && view_projection != NULL)) return;
    const float w = (float)(width ? width : 1);
    const float h = (float)(height ? height : 1);
    const float half = (line_width > 1.0f ? line_width : 1.0f) * 0.5f;
    lines->view_projection = *view_projection;
    lines->half_width_ndc_x = half * 2.0f / w;
    lines->half_width_ndc_y = half * 2.0f / h;
    lines->pixels_x = w * 0.5f;
    lines->pixels_y = h * 0.5f;
    lines->count = 0;
    lines->out_of_memory = false;
}

static bool wpe__reserve(WPE_DebugLines* lines) {
    if (lines->count < lines->capacity) return true;
    const uint32_t capacity = lines->capacity ? lines->capacity * 2 : 256;
    wpe__LineVertex* vertices = wpe_realloc(lines->vertices, (size_t)capacity * 4 * sizeof *vertices);
    if (!vertices) return false;
    lines->vertices = vertices;
    uint32_t* indices = wpe_realloc(lines->indices, (size_t)capacity * 6 * sizeof *indices);
    if (!indices) return false;
    lines->indices = indices;
    for (uint32_t q = lines->capacity; q < capacity; ++q) {
        const uint32_t b = q * 4;
        const uint32_t idx[6] = {b, b + 1, b + 2, b, b + 2, b + 3};
        for (int i = 0; i < 6; ++i)
            indices[q * 6 + (uint32_t)i] = idx[i];
    }
    lines->capacity = capacity;
    return true;
}

static WPE_Vec4 wpe__lerp4(WPE_Vec4 a, WPE_Vec4 b, float t) {
    return wpe_vec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
                    a.w + (b.w - a.w) * t);
}

void wpe_debug_lines_add(WPE_DebugLines* lines, WPE_Vec3 a, WPE_Vec3 b, WPE_Color color) {
    if (!wpe_check(lines != NULL) || lines->out_of_memory) return;
    WPE_Vec4 p0 = wpe_mat4_mul_vec4(&lines->view_projection, wpe_vec4(a.x, a.y, a.z, 1.0f));
    WPE_Vec4 p1 = wpe_mat4_mul_vec4(&lines->view_projection, wpe_vec4(b.x, b.y, b.z, 1.0f));

    /* Clip against the near plane (z >= -w) so both endpoints have a positive w. */
    const float d0 = p0.z + p0.w;
    const float d1 = p1.z + p1.w;
    if (!(d0 >= 0.0f) && !(d1 >= 0.0f)) return;
    if (d0 < 0.0f) p0 = wpe__lerp4(p0, p1, d0 / (d0 - d1));
    if (d1 < 0.0f) p1 = wpe__lerp4(p1, p0, d1 / (d1 - d0));
    if (!(p0.w > 0.0f) || !(p1.w > 0.0f)) return;

    /* Screen-space direction gives the perpendicular offset of the quad's long edges. */
    const float x0 = p0.x / p0.w, y0 = p0.y / p0.w;
    const float x1 = p1.x / p1.w, y1 = p1.y / p1.w;
    float dx = (x1 - x0) * lines->pixels_x;
    float dy = (y1 - y0) * lines->pixels_y;
    const float length = sqrtf(dx * dx + dy * dy);
    if (length > 1e-6f) {
        dx /= length;
        dy /= length;
    } else {
        dx = 1.0f;
        dy = 0.0f;
    }
    const float ox = -dy * lines->half_width_ndc_x;
    const float oy = dx * lines->half_width_ndc_y;

    if (!wpe__reserve(lines)) {
        lines->out_of_memory = true;
        return;
    }
    const WPE_Vec4 c = wpe_color_to_vec4(color);
    wpe__LineVertex* v = &lines->vertices[(size_t)lines->count * 4];
    const WPE_Vec4 ends[2] = {p0, p1};
    static const struct {
        int end;
        float side;
    } corners[4] = {{0, 1.0f}, {0, -1.0f}, {1, -1.0f}, {1, 1.0f}};
    for (int i = 0; i < 4; ++i) {
        const WPE_Vec4 p = ends[corners[i].end];
        const float s = corners[i].side;
        v[i] = (wpe__LineVertex){
            .clip = {p.x + ox * s * p.w, p.y + oy * s * p.w, p.z - WPE__DEPTH_BIAS * p.w, p.w},
            .color = {c.x, c.y, c.z, c.w},
        };
    }
    ++lines->count;
}

uint32_t wpe_debug_lines_count(const WPE_DebugLines* lines) {
    return lines ? lines->count : 0;
}

WPE_Result wpe_debug_lines_submit(WPE_DebugLines* lines, WPE_Renderer* renderer, bool depth_test) {
    if (!lines || !renderer) return WPE_RESULT_ERROR_NULL_ARG;
    if (lines->out_of_memory) {
        wpe_log_error("debug lines dropped: out of memory while recording");
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }
    if (lines->count == 0) return WPE_RESULT_OK;
    return wpe_renderer_submit(
        renderer, &(WPE_DrawDesc){.program = &wpe__program_line,
                                  .vertices = lines->vertices,
                                  .vertex_stride = sizeof(wpe__LineVertex),
                                  .vertex_count = lines->count * 4,
                                  .indices = lines->indices,
                                  .index_count = lines->count * 6,
                                  .state = {.cull = WPE_CULL_NONE,
                                            .depth_func = depth_test ? WPE_DEPTH_LEQUAL : WPE_DEPTH_ALWAYS,
                                            .depth_write = false,
                                            .blend = WPE_BLEND_ALPHA},
                                  .sort_key = WPE__DEBUG_LINES_SORT_KEY});
}
