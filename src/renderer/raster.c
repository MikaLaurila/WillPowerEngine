#include <math.h>

#include "renderer/renderer_internal.h"

/* ---- Color conversion ----------------------------------------------------------------------------- */

static uint32_t wpe__unorm8(float v) {
    if (!(v > 0.0f)) return 0; /* Also maps NaN to 0. */
    if (v >= 1.0f) return 255;
    return (uint32_t)(v * 255.0f + 0.5f);
}

WPE_Color wpe_color_from_vec4(WPE_Vec4 c) {
    return (wpe__unorm8(c.w) << 24) | (wpe__unorm8(c.x) << 16) | (wpe__unorm8(c.y) << 8) | wpe__unorm8(c.z);
}

WPE_Vec4 wpe_color_to_vec4(WPE_Color c) {
    const float s = 1.0f / 255.0f;
    return wpe_vec4((float)((c >> 16) & 0xFF) * s, (float)((c >> 8) & 0xFF) * s, (float)(c & 0xFF) * s,
                    (float)(c >> 24) * s);
}

static WPE_Color wpe__blend(WPE_BlendMode mode, WPE_Vec4 src, WPE_Color dst_packed) {
    switch (mode) {
    case WPE_BLEND_ALPHA: {
        const float a = src.w < 0.0f ? 0.0f : (src.w > 1.0f ? 1.0f : src.w);
        const WPE_Vec4 dst = wpe_color_to_vec4(dst_packed);
        return wpe_color_from_vec4(wpe_vec4(src.x * a + dst.x * (1.0f - a), src.y * a + dst.y * (1.0f - a),
                                            src.z * a + dst.z * (1.0f - a), a + dst.w * (1.0f - a)));
    }
    case WPE_BLEND_ADDITIVE: {
        const float a = src.w < 0.0f ? 0.0f : (src.w > 1.0f ? 1.0f : src.w);
        const WPE_Vec4 dst = wpe_color_to_vec4(dst_packed);
        return wpe_color_from_vec4(wpe_vec4(dst.x + src.x * a, dst.y + src.y * a, dst.z + src.z * a, dst.w));
    }
    case WPE_BLEND_OPAQUE:
    default:
        return wpe_color_from_vec4(src);
    }
}

/* 4x4 Bayer matrix thresholds, (index + 0.5) / 16, for WPE_BLEND_ALPHA_DITHER. */
static const float wpe__bayer4[4][4] = {
    {0.5f / 16, 8.5f / 16, 2.5f / 16, 10.5f / 16},
    {12.5f / 16, 4.5f / 16, 14.5f / 16, 6.5f / 16},
    {3.5f / 16, 11.5f / 16, 1.5f / 16, 9.5f / 16},
    {15.5f / 16, 7.5f / 16, 13.5f / 16, 5.5f / 16},
};

static bool wpe__dither_keeps(float alpha, int32_t x, int32_t y) {
    return alpha > wpe__bayer4[y & 3][x & 3];
}

static bool wpe__depth_passes(WPE_DepthFunc func, float depth, float stored) {
    switch (func) {
    case WPE_DEPTH_LESS:
        return depth < stored;
    case WPE_DEPTH_LEQUAL:
        return depth <= stored;
    case WPE_DEPTH_ALWAYS:
    default:
        return true;
    }
}

/* ---- Edge functions ------------------------------------------------------------------------------- */

/*
 * Edge a->b evaluated at fixed-point point p: (b.x - a.x)(p.y - a.y) - (b.y - a.y)(p.x - a.x).
 * Setup triangles have positive area, so interior points give non-negative values on all edges.
 */
typedef struct wpe__Edge {
    int64_t value;  /* At the current pixel center. */
    int64_t step_x; /* Change per pixel to the right. */
    int64_t step_y; /* Change per pixel downward. */
    int64_t bias;   /* 0 for top-left edges, -1 otherwise (top-left fill rule). */
} wpe__Edge;

static wpe__Edge wpe__edge_setup(int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t px, int32_t py) {
    const int64_t dx = (int64_t)bx - ax;
    const int64_t dy = (int64_t)by - ay;
    /* With positive-area (clockwise on a y-down screen) winding, top edges run left to right and left edges
     * run upward. */
    const bool top_left = (dy == 0 && dx > 0) || dy < 0;
    return (wpe__Edge){
        .value = dx * ((int64_t)py - ay) - dy * ((int64_t)px - ax),
        .step_x = -dy * WPE__RASTER_SUBPIXEL_ONE,
        .step_y = dx * WPE__RASTER_SUBPIXEL_ONE,
        .bias = top_left ? 0 : -1,
    };
}

/* ---- Tile rasterization --------------------------------------------------------------------------- */

static void wpe__clear_tile(WPE_Renderer* renderer, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    const WPE_ClearDesc* clear = &renderer->clear;
    const WPE_Framebuffer* fb = &renderer->framebuffer;
    for (int32_t y = y0; y <= y1; ++y) {
        const size_t row = (size_t)y * fb->width;
        for (int32_t x = x0; x <= x1; ++x) {
            if (clear->clear_color) fb->color[row + (size_t)x] = clear->color;
            if (clear->clear_depth) fb->depth[row + (size_t)x] = clear->depth;
        }
    }
}

/* Rasterizes the part of `tri` inside the pixel rectangle [x0, x1] x [y0, y1]. Returns fragments shaded. */
static uint64_t wpe__raster_triangle(WPE_Renderer* renderer, const wpe__SetupTri* tri, int32_t x0, int32_t y0,
                                     int32_t x1, int32_t y1) {
    const wpe__Draw* draw = renderer->sorted[tri->draw];
    const WPE_DrawDesc* desc = &draw->desc;
    const WPE_RenderState state = desc->state;
    const uint32_t varying_count = desc->program->varying_count;
    const WPE_FragmentShaderFn fragment = desc->program->fragment;
    const WPE_Framebuffer* fb = &renderer->framebuffer;

    const int32_t px = x0 * WPE__RASTER_SUBPIXEL_ONE + WPE__RASTER_SUBPIXEL_ONE / 2;
    const int32_t py = y0 * WPE__RASTER_SUBPIXEL_ONE + WPE__RASTER_SUBPIXEL_ONE / 2;
    /* Edge i is opposite vertex i, so its value is vertex i's (unnormalized) barycentric weight. */
    wpe__Edge row_edges[3] = {
        wpe__edge_setup(tri->x[1], tri->y[1], tri->x[2], tri->y[2], px, py),
        wpe__edge_setup(tri->x[2], tri->y[2], tri->x[0], tri->y[0], px, py),
        wpe__edge_setup(tri->x[0], tri->y[0], tri->x[1], tri->y[1], px, py),
    };
    const float inv_area = 1.0f / (float)tri->area;

    float varyings[WPE_MAX_VARYINGS];
    WPE_FragmentInput input = {.front_facing = tri->front_facing, .varyings = varyings};
    uint64_t shaded = 0;

    /*
     * UV derivatives. With A = sum(l_i * uv_i / w_i) and B = sum(l_i / w_i), uv = A / B, so
     * d(uv)/dx = (dA/dx - uv * dB/dx) / B. The barycentric weights are affine in screen space, so
     * dA/dx and dB/dx are per-triangle constants computed here once.
     */
    const bool derivatives = desc->program->uv_derivatives;
    const uint32_t uv_k = desc->program->uv_varying;
    float dA_dx[2] = {0, 0}, dA_dy[2] = {0, 0}, dB_dx = 0.0f, dB_dy = 0.0f;
    if (derivatives) {
        for (int i = 0; i < 3; ++i) {
            const float dl_dx = (float)row_edges[i].step_x * inv_area;
            const float dl_dy = (float)row_edges[i].step_y * inv_area;
            dB_dx += dl_dx * tri->inv_w[i];
            dB_dy += dl_dy * tri->inv_w[i];
            for (int c = 0; c < 2; ++c) {
                dA_dx[c] += dl_dx * tri->varyings[i][uv_k + (uint32_t)c];
                dA_dy[c] += dl_dy * tri->varyings[i][uv_k + (uint32_t)c];
            }
        }
    }

    for (int32_t y = y0; y <= y1; ++y) {
        int64_t e0 = row_edges[0].value, e1 = row_edges[1].value, e2 = row_edges[2].value;
        const size_t row = (size_t)y * fb->width;

        for (int32_t x = x0; x <= x1;
             ++x, e0 += row_edges[0].step_x, e1 += row_edges[1].step_x, e2 += row_edges[2].step_x) {
            if ((e0 + row_edges[0].bias) < 0 || (e1 + row_edges[1].bias) < 0 ||
                (e2 + row_edges[2].bias) < 0) {
                continue;
            }

            const float l0 = (float)e0 * inv_area;
            const float l1 = (float)e1 * inv_area;
            const float l2 = (float)e2 * inv_area;
            const float depth = l0 * tri->z[0] + l1 * tri->z[1] + l2 * tri->z[2];
            const size_t index = row + (size_t)x;
            if (!wpe__depth_passes(state.depth_func, depth, fb->depth[index])) continue;

            /* Varyings were divided by w at setup; interpolate linearly and divide by interpolated 1/w. */
            const float inv_w = l0 * tri->inv_w[0] + l1 * tri->inv_w[1] + l2 * tri->inv_w[2];
            const float w = 1.0f / inv_w;
            for (uint32_t k = 0; k < varying_count; ++k) {
                varyings[k] =
                    (l0 * tri->varyings[0][k] + l1 * tri->varyings[1][k] + l2 * tri->varyings[2][k]) * w;
            }

            if (derivatives) {
                const float u = varyings[uv_k], v = varyings[uv_k + 1];
                input.uv_dx = (WPE_Vec2){.x = (dA_dx[0] - u * dB_dx) * w, .y = (dA_dx[1] - v * dB_dx) * w};
                input.uv_dy = (WPE_Vec2){.x = (dA_dy[0] - u * dB_dy) * w, .y = (dA_dy[1] - v * dB_dy) * w};
            }
            input.x = x;
            input.y = y;
            input.depth = depth;
            WPE_Vec4 color;
            ++shaded;
            if (!fragment(desc->uniforms, &input, &color)) continue;

            if (state.blend == WPE_BLEND_ALPHA_DITHER) {
                if (!wpe__dither_keeps(color.w, x, y)) continue;
                color.w = 1.0f;
                fb->color[index] = wpe_color_from_vec4(color);
            } else {
                fb->color[index] = wpe__blend(state.blend, color, fb->color[index]);
            }
            if (state.depth_write) fb->depth[index] = depth;
        }

        for (int i = 0; i < 3; ++i)
            row_edges[i].value += row_edges[i].step_y;
    }
    return shaded;
}

void wpe__renderer_raster_tiles(void* context, uint32_t begin, uint32_t end, uint32_t thread_index) {
    (void)thread_index;
    WPE_Renderer* renderer = context;
    const WPE_Framebuffer* fb = &renderer->framebuffer;

    for (uint32_t tile = begin; tile < end; ++tile) {
        const int32_t tx = (int32_t)(tile % renderer->tiles_x);
        const int32_t ty = (int32_t)(tile / renderer->tiles_x);
        const int32_t x0 = tx * (int32_t)WPE_RENDERER_TILE_SIZE;
        const int32_t y0 = ty * (int32_t)WPE_RENDERER_TILE_SIZE;
        int32_t x1 = x0 + (int32_t)WPE_RENDERER_TILE_SIZE - 1;
        int32_t y1 = y0 + (int32_t)WPE_RENDERER_TILE_SIZE - 1;
        x1 = x1 > (int32_t)fb->width - 1 ? (int32_t)fb->width - 1 : x1;
        y1 = y1 > (int32_t)fb->height - 1 ? (int32_t)fb->height - 1 : y1;

        if (renderer->clear.clear_color || renderer->clear.clear_depth)
            wpe__clear_tile(renderer, x0, y0, x1, y1);

        uint64_t shaded = 0;
        for (uint32_t r = renderer->tile_offsets[tile]; r < renderer->tile_offsets[tile + 1]; ++r) {
            const wpe__SetupTri* tri = renderer->tri_table[renderer->tile_refs[r]];
            const int32_t rx0 = tri->min_px > x0 ? tri->min_px : x0;
            const int32_t ry0 = tri->min_py > y0 ? tri->min_py : y0;
            const int32_t rx1 = tri->max_px < x1 ? tri->max_px : x1;
            const int32_t ry1 = tri->max_py < y1 ? tri->max_py : y1;
            if (rx0 <= rx1 && ry0 <= ry1) shaded += wpe__raster_triangle(renderer, tri, rx0, ry0, rx1, ry1);
        }
        renderer->tile_fragments[tile] = shaded;
    }
}
