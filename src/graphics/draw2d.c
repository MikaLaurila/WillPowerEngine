#define WPE_LOG_MODULE "graphics"
#include "graphics/draw2d.h"

#include "core/alloc.h"
#include "core/debug.h"
#include "core/log.h"

typedef struct wpe__Vertex2D {
    float x, y; /* Canvas pixels. */
    float u, v; /* Normalized texture coordinates. */
    float color[4];
} wpe__Vertex2D;

/* A run of consecutive quads sharing one texture (NULL = solid color). */
typedef struct wpe__Batch {
    const WPE_Texture* texture;
    uint32_t first_quad;
    uint32_t quad_count;
} wpe__Batch;

typedef struct wpe__Uniforms2D {
    const WPE_Texture* texture;
    float inv_width, inv_height;
} wpe__Uniforms2D;

struct WPE_Draw2D {
    uint32_t canvas_width, canvas_height;
    wpe__Vertex2D* vertices; /* 4 per quad */
    uint32_t* indices;       /* 6 per quad, rebuilt on growth */
    uint32_t quad_count, quad_capacity;
    wpe__Batch* batches;
    uint32_t batch_count, batch_capacity;
    bool out_of_memory;
};

/* Sort keys above every 3D key (scene keys use at most bit 63 plus bits 30..61). */
#define WPE__DRAW2D_SORT_KEY UINT64_MAX

static const WPE_Sampler wpe__sampler_2d = {.mag_filter = WPE_FILTER_NEAREST,
                                            .min_filter = WPE_FILTER_NEAREST,
                                            .wrap_u = WPE_WRAP_CLAMP,
                                            .wrap_v = WPE_WRAP_CLAMP};

static void wpe__vs_2d(const void* uniforms, const void* vertex, WPE_Vec4* out_position, float* out) {
    const wpe__Uniforms2D* u = uniforms;
    const wpe__Vertex2D* v = vertex;
    *out_position =
        wpe_vec4(v->x * u->inv_width * 2.0f - 1.0f, 1.0f - v->y * u->inv_height * 2.0f, 0.0f, 1.0f);
    out[0] = v->u;
    out[1] = v->v;
    for (int i = 0; i < 4; ++i)
        out[2 + i] = v->color[i];
}

static bool wpe__fs_2d(const void* uniforms, const WPE_FragmentInput* in, WPE_Vec4* out_color) {
    const wpe__Uniforms2D* u = uniforms;
    const float* v = in->varyings;
    WPE_Vec4 color = wpe_vec4(v[2], v[3], v[4], v[5]);
    if (u->texture) {
        const WPE_Vec4 t =
            wpe_texture_sample_lod(u->texture, &wpe__sampler_2d, (WPE_Vec2){.x = v[0], .y = v[1]}, 0.0f);
        color = wpe_vec4(color.x * t.x, color.y * t.y, color.z * t.z, color.w * t.w);
    }
    if (!(color.w > 0.0f)) return false; /* Fully transparent texels leave the pixel untouched. */
    *out_color = color;
    return true;
}

static const WPE_ShaderProgram wpe__program_2d = {
    .vertex = wpe__vs_2d, .fragment = wpe__fs_2d, .varying_count = 6};

WPE_Result wpe_draw2d_create(WPE_Draw2D** out_draw2d) {
    if (!out_draw2d) return WPE_RESULT_ERROR_NULL_ARG;
    *out_draw2d = wpe_alloc_zeroed(1, sizeof **out_draw2d);
    return *out_draw2d ? WPE_RESULT_OK : WPE_RESULT_ERROR_OUT_OF_MEMORY;
}

void wpe_draw2d_destroy(WPE_Draw2D* d) {
    if (!d) return;
    wpe_free(d->vertices);
    wpe_free(d->indices);
    wpe_free(d->batches);
    wpe_free(d);
}

void wpe_draw2d_begin(WPE_Draw2D* d, uint32_t canvas_width, uint32_t canvas_height) {
    if (!wpe_check(d != NULL)) return;
    d->canvas_width = canvas_width ? canvas_width : 1;
    d->canvas_height = canvas_height ? canvas_height : 1;
    d->quad_count = 0;
    d->batch_count = 0;
    d->out_of_memory = false;
}

static bool wpe__reserve_quad(WPE_Draw2D* d) {
    if (d->quad_count < d->quad_capacity) return true;
    const uint32_t capacity = d->quad_capacity ? d->quad_capacity * 2 : 256;
    wpe__Vertex2D* vertices = wpe_realloc(d->vertices, (size_t)capacity * 4 * sizeof *vertices);
    if (!vertices) return false;
    d->vertices = vertices;
    uint32_t* indices = wpe_realloc(d->indices, (size_t)capacity * 6 * sizeof *indices);
    if (!indices) return false;
    d->indices = indices;
    /* Indices are static per quad slot: two counter-clockwise triangles in NDC. */
    for (uint32_t q = d->quad_capacity; q < capacity; ++q) {
        const uint32_t b = q * 4;
        const uint32_t idx[6] = {b, b + 2, b + 1, b, b + 3, b + 2};
        for (int i = 0; i < 6; ++i)
            indices[q * 6 + (uint32_t)i] = idx[i];
    }
    d->quad_capacity = capacity;
    return true;
}

static void wpe__push_quad(WPE_Draw2D* d, const WPE_Texture* texture, WPE_Rect dest, float u0, float v0,
                           float u1, float v1, WPE_Color color) {
    if (d->out_of_memory || !(dest.width > 0.0f) || !(dest.height > 0.0f)) return;
    if (!wpe__reserve_quad(d)) {
        d->out_of_memory = true;
        return;
    }
    if (d->batch_count == 0 || d->batches[d->batch_count - 1].texture != texture) {
        if (d->batch_count == d->batch_capacity) {
            const uint32_t capacity = d->batch_capacity ? d->batch_capacity * 2 : 32;
            wpe__Batch* batches = wpe_realloc(d->batches, (size_t)capacity * sizeof *batches);
            if (!batches) {
                d->out_of_memory = true;
                return;
            }
            d->batches = batches;
            d->batch_capacity = capacity;
        }
        d->batches[d->batch_count++] = (wpe__Batch){.texture = texture, .first_quad = d->quad_count};
    }

    const WPE_Vec4 c = wpe_color_to_vec4(color);
    const float x0 = dest.x, y0 = dest.y, x1 = dest.x + dest.width, y1 = dest.y + dest.height;
    wpe__Vertex2D* v = &d->vertices[(size_t)d->quad_count * 4];
    v[0] = (wpe__Vertex2D){x0, y0, u0, v0, {c.x, c.y, c.z, c.w}};
    v[1] = (wpe__Vertex2D){x1, y0, u1, v0, {c.x, c.y, c.z, c.w}};
    v[2] = (wpe__Vertex2D){x1, y1, u1, v1, {c.x, c.y, c.z, c.w}};
    v[3] = (wpe__Vertex2D){x0, y1, u0, v1, {c.x, c.y, c.z, c.w}};
    ++d->quad_count;
    ++d->batches[d->batch_count - 1].quad_count;
}

void wpe_draw2d_rect(WPE_Draw2D* d, WPE_Rect rect, WPE_Color color) {
    if (!wpe_check(d != NULL)) return;
    wpe__push_quad(d, NULL, rect, 0, 0, 0, 0, color);
}

void wpe_draw2d_image(WPE_Draw2D* d, const WPE_Texture* texture, WPE_Rect source, WPE_Rect dest,
                      WPE_Color tint) {
    if (!wpe_check(d != NULL && texture != NULL)) return;
    const float iw = 1.0f / (float)texture->levels[0].width, ih = 1.0f / (float)texture->levels[0].height;
    wpe__push_quad(d, texture, dest, source.x * iw, source.y * ih, (source.x + source.width) * iw,
                   (source.y + source.height) * ih, tint);
}

void wpe_draw2d_text(WPE_Draw2D* d, const WPE_BitmapFont* font, float x, float y, const char* text,
                     WPE_Color color, float scale) {
    if (!wpe_check(d != NULL && font != NULL && text != NULL)) return;
    float pen_x = x, pen_y = y;
    for (const char* p = text; *p; ++p) {
        if (*p == '\n') {
            pen_x = x;
            pen_y += (float)font->line_height * scale;
            continue;
        }
        const WPE_FontGlyph* g = wpe_font_glyph(font, *p);
        const WPE_Rect source = {(float)g->x, (float)g->y, (float)g->width, (float)g->height};
        const WPE_Rect dest = {pen_x, pen_y, (float)g->width * scale, (float)g->height * scale};
        wpe_draw2d_image(d, font->atlas, source, dest, color);
        pen_x += (float)(g->width + font->glyph_spacing) * scale;
    }
}

WPE_Result wpe_draw2d_submit(WPE_Draw2D* d, WPE_Renderer* renderer) {
    if (!d || !renderer) return WPE_RESULT_ERROR_NULL_ARG;
    if (d->out_of_memory) {
        wpe_log_error("2D batch dropped: out of memory while recording");
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }
    const WPE_RenderState state = {.cull = WPE_CULL_NONE,
                                   .depth_func = WPE_DEPTH_ALWAYS,
                                   .depth_write = false,
                                   .blend = WPE_BLEND_ALPHA};
    for (uint32_t b = 0; b < d->batch_count; ++b) {
        const wpe__Batch* batch = &d->batches[b];
        const wpe__Uniforms2D uniforms = {.texture = batch->texture,
                                          .inv_width = 1.0f / (float)d->canvas_width,
                                          .inv_height = 1.0f / (float)d->canvas_height};
        /* Each batch indexes its own vertex range, so indices are rebased to start at zero. */
        const WPE_Result result = wpe_renderer_submit(
            renderer, &(WPE_DrawDesc){.program = &wpe__program_2d,
                                      .uniforms = &uniforms,
                                      .uniforms_size = sizeof uniforms,
                                      .vertices = &d->vertices[(size_t)batch->first_quad * 4],
                                      .vertex_stride = sizeof(wpe__Vertex2D),
                                      .vertex_count = batch->quad_count * 4,
                                      .indices = d->indices,
                                      .index_count = batch->quad_count * 6,
                                      .state = state,
                                      .sort_key = WPE__DRAW2D_SORT_KEY});
        if (result != WPE_RESULT_OK) return result;
    }
    return WPE_RESULT_OK;
}
