#define WPE_LOG_MODULE "graphics"
#include "graphics/postfx.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "core/alloc.h"
#include "core/debug.h"
#include "core/log.h"

struct WPE_PostChain {
    WPE_Color* ping;
    float* aux;
    size_t capacity;         /* Pixels. */
    void* scratch;           /* For effects' prepare functions; grows to the largest asked for. */
    size_t scratch_capacity; /* Bytes. */
};

/* Rows per parallel chunk; fixed so output is independent of the worker count. */
#define WPE__POSTFX_ROW_GRAIN 8u
#define WPE__TWO_PI_F 6.28318530717958647692f

WPE_Result wpe_postfx_create(WPE_PostChain** out_chain) {
    if (!out_chain) return WPE_RESULT_ERROR_NULL_ARG;
    *out_chain = wpe_alloc_zeroed(1, sizeof **out_chain);
    return *out_chain ? WPE_RESULT_OK : WPE_RESULT_ERROR_OUT_OF_MEMORY;
}

void wpe_postfx_destroy(WPE_PostChain* chain) {
    if (!chain) return;
    wpe_free(chain->ping);
    wpe_free(chain->aux);
    wpe_free(chain->scratch);
    wpe_free(chain);
}

float wpe_postfx_linear_depth(const WPE_PostCamera* camera, float window_depth) {
    const float n = camera->near_z, f = camera->far_z;
    const float z_ndc = window_depth * 2.0f - 1.0f;
    if (camera->orthographic) return n + (z_ndc + 1.0f) * 0.5f * (f - n);
    return 2.0f * n * f / (f + n - z_ndc * (f - n));
}

typedef struct wpe__PassJob {
    const WPE_PostContext* context;
    const WPE_PostEffect* effect;
    uint32_t pass;
} wpe__PassJob;

static void wpe__run_pass_rows(void* job_ptr, uint32_t begin, uint32_t end, uint32_t thread_index) {
    (void)thread_index;
    const wpe__PassJob* job = job_ptr;
    job->effect->pass(job->context, job->effect->params, job->pass, begin, end);
}

WPE_Result wpe_postfx_apply(WPE_PostChain* chain, WPE_Renderer* renderer, const WPE_PostEffect* effects,
                            uint32_t effect_count, const WPE_PostCamera* camera) {
    if (!chain || !renderer || (!effects && effect_count) || !camera) return WPE_RESULT_ERROR_NULL_ARG;
    WPE_Framebuffer* fb = wpe_renderer_framebuffer_for_write(renderer);
    if (!fb) {
        wpe_log_error("post-processing requires a finished frame (renderer is recording)");
        return WPE_RESULT_ERROR_INVALID_STATE;
    }
    for (uint32_t e = 0; e < effect_count; ++e) {
        if (effects[e].enabled && (!effects[e].pass || effects[e].pass_count == 0)) {
            wpe_log_error("post effect '%s' has no pass function", effects[e].name ? effects[e].name : "?");
            return WPE_RESULT_ERROR_INVALID_ARG;
        }
    }

    const size_t pixels = (size_t)fb->width * fb->height;
    if (pixels > chain->capacity) {
        WPE_Color* ping = wpe_realloc(chain->ping, pixels * sizeof *ping);
        if (!ping) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
        chain->ping = ping;
        float* aux = wpe_realloc(chain->aux, pixels * sizeof *aux);
        if (!aux) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
        chain->aux = aux;
        chain->capacity = pixels;
    }
    for (uint32_t e = 0; e < effect_count; ++e) {
        if (!effects[e].enabled || !effects[e].prepare || effects[e].scratch_size <= chain->scratch_capacity)
            continue;
        void* scratch = wpe_realloc(chain->scratch, effects[e].scratch_size);
        if (!scratch) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
        chain->scratch = scratch;
        chain->scratch_capacity = effects[e].scratch_size;
    }

    WPE_JobSystem* jobs = wpe_renderer_jobs(renderer);
    WPE_Color* src = fb->color;
    WPE_Color* dst = chain->ping;
    for (uint32_t e = 0; e < effect_count; ++e) {
        if (!effects[e].enabled) continue;
        const void* scratch = NULL;
        if (effects[e].prepare) {
            const WPE_PostContext context = {
                .src = src, .depth = fb->depth, .width = fb->width, .height = fb->height, .camera = *camera};
            effects[e].prepare(&context, effects[e].params, chain->scratch);
            scratch = chain->scratch;
        }
        for (uint32_t pass = 0; pass < effects[e].pass_count; ++pass) {
            const WPE_PostContext context = {.src = src,
                                             .dst = dst,
                                             .aux = chain->aux,
                                             .depth = fb->depth,
                                             .width = fb->width,
                                             .height = fb->height,
                                             .camera = *camera,
                                             .scratch = scratch};
            wpe__PassJob job = {.context = &context, .effect = &effects[e], .pass = pass};
            wpe_jobs_parallel_for(jobs, fb->height, WPE__POSTFX_ROW_GRAIN, wpe__run_pass_rows, &job);
            WPE_Color* swap = src;
            src = dst;
            dst = swap;
        }
    }
    if (src != fb->color) memcpy(fb->color, src, pixels * sizeof *fb->color);
    return WPE_RESULT_OK;
}

/* ---- Shared helpers -------------------------------------------------------------------------------- */

static const float wpe__bayer4[4][4] = {
    {0.5f / 16, 8.5f / 16, 2.5f / 16, 10.5f / 16},
    {12.5f / 16, 4.5f / 16, 14.5f / 16, 6.5f / 16},
    {3.5f / 16, 11.5f / 16, 1.5f / 16, 9.5f / 16},
    {15.5f / 16, 7.5f / 16, 13.5f / 16, 5.5f / 16},
};

static WPE_Color wpe__texel_clamped(const WPE_PostContext* c, int32_t x, int32_t y) {
    x = x < 0 ? 0 : (x >= (int32_t)c->width ? (int32_t)c->width - 1 : x);
    y = y < 0 ? 0 : (y >= (int32_t)c->height ? (int32_t)c->height - 1 : y);
    return c->src[(size_t)y * c->width + (size_t)x];
}

static float wpe__clamp01(float v) {
    return v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f;
}

/* a, then b with weight t (0..1); alpha from b. */
static WPE_Color wpe__mix_colors(WPE_Color a, WPE_Color b, float t) {
    const WPE_Vec4 x = wpe_color_to_vec4(a), y = wpe_color_to_vec4(b);
    return wpe_color_from_vec4(
        wpe_vec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t, y.w));
}

static WPE_Color wpe__scale_rgb(WPE_Color c, float s) {
    WPE_Vec4 v = wpe_color_to_vec4(c);
    return wpe_color_from_vec4(wpe_vec4(v.x * s, v.y * s, v.z * s, v.w));
}

/* ---- SSAO ------------------------------------------------------------------------------------------ */

/* Eight directions with increasing length, rotated per pixel by a 4x4 pattern and blurred over 4x4. */
static const float wpe__ssao_kernel[8][2] = {{1.0f, 0.0f},       {0.0f, 0.875f},   {-0.75f, 0.0f},
                                             {0.0f, -0.625f},    {0.354f, 0.354f}, {-0.31f, 0.31f},
                                             {-0.265f, -0.265f}, {0.22f, -0.22f}};

static void wpe__ssao_pass(const WPE_PostContext* c, const void* params_ptr, uint32_t pass,
                           uint32_t row_begin, uint32_t row_end) {
    const WPE_SsaoParams* p = params_ptr;
    const uint32_t w = c->width, h = c->height;
    if (pass == 0) {
        for (uint32_t y = row_begin; y < row_end; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const size_t i = (size_t)y * w + x;
                c->dst[i] = c->src[i];
                const float d = c->depth[i];
                if (!(d < 1.0f)) {
                    c->aux[i] = 1.0f; /* Background. */
                    continue;
                }
                const float z = wpe_postfx_linear_depth(&c->camera, d);
                const float angle = wpe__bayer4[y & 3][x & 3] * 2.0f * WPE_PI;
                const float ca = cosf(angle), sa = sinf(angle);
                float occlusion = 0.0f;
                for (int k = 0; k < 8; ++k) {
                    const float kx = wpe__ssao_kernel[k][0], ky = wpe__ssao_kernel[k][1];
                    const int32_t sx = (int32_t)x + (int32_t)lroundf((kx * ca - ky * sa) * p->radius_pixels);
                    const int32_t sy = (int32_t)y + (int32_t)lroundf((kx * sa + ky * ca) * p->radius_pixels);
                    if (sx < 0 || sy < 0 || sx >= (int32_t)w || sy >= (int32_t)h) continue;
                    const float sd = c->depth[(size_t)sy * w + (size_t)sx];
                    const float diff = z - wpe_postfx_linear_depth(&c->camera, sd);
                    if (diff > p->bias && diff < p->depth_range) occlusion += 1.0f - diff / p->depth_range;
                }
                c->aux[i] = 1.0f - p->strength * occlusion / 8.0f;
            }
        }
        return;
    }
    /* Pass 1: a 4x4 box blur cancels the rotation pattern, then darken. */
    for (uint32_t y = row_begin; y < row_end; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            float sum = 0.0f;
            int n = 0;
            for (int32_t dy = -2; dy < 2; ++dy) {
                for (int32_t dx = -2; dx < 2; ++dx) {
                    const int32_t sx = (int32_t)x + dx, sy = (int32_t)y + dy;
                    if (sx < 0 || sy < 0 || sx >= (int32_t)w || sy >= (int32_t)h) continue;
                    sum += c->aux[(size_t)sy * w + (size_t)sx];
                    ++n;
                }
            }
            const size_t i = (size_t)y * w + x;
            c->dst[i] = wpe__scale_rgb(c->src[i], wpe__clamp01(sum / (float)n));
        }
    }
}

/*
 * Pass 1 reads aux values of neighboring rows written by pass 0. This is safe because the chain
 * runs each pass as a complete parallel_for, so all of pass 0 finishes before pass 1 starts.
 */

/* ---- Simple per-pixel effects ----------------------------------------------------------------------- */

static void wpe__chromatic_pass(const WPE_PostContext* c, const void* params_ptr, uint32_t pass,
                                uint32_t row_begin, uint32_t row_end) {
    (void)pass;
    const WPE_ChromaticAberrationParams* p = params_ptr;
    const float cx = (float)c->width * 0.5f, cy = (float)c->height * 0.5f;
    for (uint32_t y = row_begin; y < row_end; ++y) {
        for (uint32_t x = 0; x < c->width; ++x) {
            const float dx = ((float)x + 0.5f - cx) / cx, dy = ((float)y + 0.5f - cy) / cy;
            const int32_t ox = (int32_t)lroundf(dx * p->strength_pixels * 0.7071f);
            const int32_t oy = (int32_t)lroundf(dy * p->strength_pixels * 0.7071f);
            const WPE_Color r = wpe__texel_clamped(c, (int32_t)x + ox, (int32_t)y + oy);
            const WPE_Color g = c->src[(size_t)y * c->width + x];
            const WPE_Color b = wpe__texel_clamped(c, (int32_t)x - ox, (int32_t)y - oy);
            c->dst[(size_t)y * c->width + x] = (g & 0xFF00FF00u) | (r & 0x00FF0000u) | (b & 0x000000FFu);
        }
    }
}

static void wpe__ps1_pass(const WPE_PostContext* c, const void* params_ptr, uint32_t pass, uint32_t row_begin,
                          uint32_t row_end) {
    (void)pass;
    const WPE_Ps1Params* p = params_ptr;
    const float steps = (float)(p->levels > 1 ? p->levels - 1 : 1);
    for (uint32_t y = row_begin; y < row_end; ++y) {
        for (uint32_t x = 0; x < c->width; ++x) {
            const size_t i = (size_t)y * c->width + x;
            const float threshold = (wpe__bayer4[y & 3][x & 3] - 0.5f) * p->dither_strength;
            WPE_Vec4 v = wpe_color_to_vec4(c->src[i]);
            for (int k = 0; k < 3; ++k)
                v.v[k] = wpe__clamp01(floorf(v.v[k] * steps + 0.5f + threshold) / steps);
            c->dst[i] = wpe_color_from_vec4(v);
        }
    }
}

static void wpe__vignette_pass(const WPE_PostContext* c, const void* params_ptr, uint32_t pass,
                               uint32_t row_begin, uint32_t row_end) {
    (void)pass;
    const WPE_VignetteParams* p = params_ptr;
    const float cx = (float)c->width * 0.5f, cy = (float)c->height * 0.5f;
    for (uint32_t y = row_begin; y < row_end; ++y) {
        for (uint32_t x = 0; x < c->width; ++x) {
            const float dx = ((float)x + 0.5f - cx) / cx, dy = ((float)y + 0.5f - cy) / cy;
            const float dist = sqrtf(dx * dx + dy * dy) * 0.7071f; /* 1 at the corners */
            float t = (dist - p->radius) / fmaxf(1.0f - p->radius, 1e-3f);
            t = wpe__clamp01(t);
            const float factor = 1.0f - p->strength * t * t * (3.0f - 2.0f * t);
            const size_t i = (size_t)y * c->width + x;
            c->dst[i] = wpe__scale_rgb(c->src[i], factor);
        }
    }
}

static void wpe__color_adjust_pass(const WPE_PostContext* c, const void* params_ptr, uint32_t pass,
                                   uint32_t row_begin, uint32_t row_end) {
    (void)pass;
    const WPE_ColorAdjustParams* p = params_ptr;
    for (uint32_t y = row_begin; y < row_end; ++y) {
        for (uint32_t x = 0; x < c->width; ++x) {
            const size_t i = (size_t)y * c->width + x;
            WPE_Vec4 v = wpe_color_to_vec4(c->src[i]);
            for (int k = 0; k < 3; ++k)
                v.v[k] = (v.v[k] - 0.5f) * p->contrast + 0.5f + p->brightness;
            const float luma = 0.299f * v.x + 0.587f * v.y + 0.114f * v.z;
            for (int k = 0; k < 3; ++k)
                v.v[k] = wpe__clamp01(luma + (v.v[k] - luma) * p->saturation);
            c->dst[i] = wpe_color_from_vec4(v);
        }
    }
}

static void wpe__grayscale_pass(const WPE_PostContext* c, const void* params_ptr, uint32_t pass,
                                uint32_t row_begin, uint32_t row_end) {
    (void)pass;
    const WPE_GrayscaleParams* p = params_ptr;
    const float tint[3] = {p->tint_r, p->tint_g, p->tint_b};
    for (uint32_t y = row_begin; y < row_end; ++y) {
        for (uint32_t x = 0; x < c->width; ++x) {
            const size_t i = (size_t)y * c->width + x;
            WPE_Vec4 v = wpe_color_to_vec4(c->src[i]);
            const float luma = 0.299f * v.x + 0.587f * v.y + 0.114f * v.z;
            for (int k = 0; k < 3; ++k)
                v.v[k] = wpe__clamp01(v.v[k] + (luma * tint[k] - v.v[k]) * p->strength);
            c->dst[i] = wpe_color_from_vec4(v);
        }
    }
}

/* ---- Depth of field ----------------------------------------------------------------------------------- */

#define WPE__DOF_MAX_RADIUS 16

/*
 * Pass 0 stores each pixel's blur radius in aux and copies the image; passes 1 and 2 blur it
 * horizontally, then vertically (a separable box over each pixel's own radius). A sample counts when
 * it lies behind the pixel and within the pixel's radius (the pixel's blur reaches over it), or lies in
 * front and its own blur reaches the pixel (a blurred foreground spills over what's behind). So sharp
 * objects in front of a blurred background keep their edges, and blurred ones in front soften theirs.
 */
static void wpe__dof_pass(const WPE_PostContext* c, const void* params_ptr, uint32_t pass, uint32_t row_begin,
                          uint32_t row_end) {
    const WPE_DepthOfFieldParams* p = params_ptr;
    const uint32_t w = c->width, h = c->height;
    const float radius = fminf(fmaxf(p->radius_pixels, 0.0f), (float)WPE__DOF_MAX_RADIUS);
    const int32_t taps = (int32_t)ceilf(radius); /* No pixel's blur reaches further. */
    if (pass == 0) {
        for (uint32_t y = row_begin; y < row_end; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const size_t i = (size_t)y * w + x;
                c->dst[i] = c->src[i];
                /* The background (nothing drawn) counts as far away. */
                const float z =
                    c->depth[i] < 1.0f ? wpe_postfx_linear_depth(&c->camera, c->depth[i]) : c->camera.far_z;
                const float off = fabsf(z - p->focus_distance) - p->focus_range;
                const float amount =
                    off <= 0.0f ? 0.0f : (p->falloff > 0.0f ? wpe__clamp01(off / p->falloff) : 1.0f);
                c->aux[i] = amount * radius;
            }
        }
        return;
    }
    const bool horizontal = pass == 1;
    for (uint32_t y = row_begin; y < row_end; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const size_t i = (size_t)y * w + x;
            const float own = c->aux[i];
            const float depth = c->depth[i];
            float sum[3] = {0.0f, 0.0f, 0.0f};
            float weight = 0.0f;
            for (int32_t k = -taps; k <= taps; ++k) {
                const int32_t sx = horizontal ? (int32_t)x + k : (int32_t)x;
                const int32_t sy = horizontal ? (int32_t)y : (int32_t)y + k;
                if (sx < 0 || sy < 0 || sx >= (int32_t)w || sy >= (int32_t)h) continue;
                const size_t j = (size_t)sy * w + (size_t)sx;
                const float reach = (float)(k < 0 ? -k : k);
                const bool behind = c->depth[j] >= depth;
                if (k != 0 && !(behind ? reach <= own : reach <= c->aux[j])) continue;
                const WPE_Vec4 v = wpe_color_to_vec4(c->src[j]);
                sum[0] += v.x;
                sum[1] += v.y;
                sum[2] += v.z;
                weight += 1.0f;
            }
            const float a = wpe_color_to_vec4(c->src[i]).w;
            c->dst[i] = wpe_color_from_vec4(wpe_vec4(sum[0] / weight, sum[1] / weight, sum[2] / weight, a));
        }
    }
}

/* ---- Distortions ------------------------------------------------------------------------------------ */

/* Each pixel shows the image from a little toward the center across the ring, so the ring pushes it
 * outward; the push peaks mid-ring and fades smoothly to its edges. With a length, the distance and the
 * direction are taken from the nearest point of the line instead. */
static void wpe__shockwave_pass(const WPE_PostContext* c, const void* params_ptr, uint32_t pass,
                                uint32_t row_begin, uint32_t row_end) {
    (void)pass;
    const WPE_ShockwaveParams* p = params_ptr;
    const float h = (float)c->height;
    const float cx = p->center_x * (float)c->width, cy = p->center_y * h;
    const float width = p->width > 1e-4f ? p->width : 1e-4f;
    const float half = fmaxf(p->length, 0.0f) * 0.5f;
    const float ux = cosf(p->angle), uy = sinf(p->angle);
    for (uint32_t y = row_begin; y < row_end; ++y) {
        for (uint32_t x = 0; x < c->width; ++x) {
            const size_t i = (size_t)y * c->width + x;
            const float rx = ((float)x + 0.5f - cx) / h, ry = ((float)y + 0.5f - cy) / h;
            const float along = fminf(fmaxf(rx * ux + ry * uy, -half), half);
            const float nx = cx + along * ux * h,
                        ny = cy + along * uy * h; /* The nearest point, in pixels. */
            const float dx = rx - along * ux, dy = ry - along * uy;
            const float d = sqrtf(dx * dx + dy * dy);
            const float ring = (d - p->radius) / width;
            if (!(fabsf(ring) < 1.0f) || !(d > 1e-6f)) {
                c->dst[i] = c->src[i];
                continue;
            }
            const float bump = (1.0f - ring * ring) * (1.0f - ring * ring);
            const float push = p->strength_pixels * bump / d / h; /* Along the unit direction, in heights. */
            const int32_t sx = (int32_t)floorf(nx + (dx - dx * push) * h);
            const int32_t sy = (int32_t)floorf(ny + (dy - dy * push) * h);
            c->dst[i] = wpe__texel_clamped(c, sx, sy);
        }
    }
}

/* Each pixel outside the horizon shows the image from further out (the pull) and turned around the center
 * (the swirl), both weighted by a Gaussian of the distance past the horizon (`reach` its spread); inside the
 * horizon the image, swirled fully, fades to black by `darkness` over a pixel and a half; the ring is added
 * on top. */
static void wpe__black_hole_pass(const WPE_PostContext* c, const void* params_ptr, uint32_t pass,
                                 uint32_t row_begin, uint32_t row_end) {
    (void)pass;
    const WPE_BlackHoleParams* p = params_ptr;
    const float h = (float)c->height;
    const float cx = p->center_x * (float)c->width, cy = p->center_y * h;
    const float horizon = fmaxf(p->radius, 0.0f) * h;
    const float reach = fmaxf(p->reach * h, 1e-3f);
    const float ring = fmaxf(p->ring_pixels, 0.0f);
    const float darkness = wpe__clamp01(p->darkness);
    for (uint32_t y = row_begin; y < row_end; ++y) {
        for (uint32_t x = 0; x < c->width; ++x) {
            const size_t i = (size_t)y * c->width + x;
            const float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            const float d = sqrtf(dx * dx + dy * dy);
            const float past = fmaxf(d - horizon, 0.0f);
            const float weight = expf(-0.5f * (past / reach) * (past / reach));
            const float glow = ring > 0.0f ? fmaxf(0.0f, 1.0f - fabsf(d - horizon) / ring) : 0.0f;
            if (weight < 1e-3f && glow <= 0.0f) {
                c->dst[i] = c->src[i];
                continue;
            }
            const float from = d + p->pull_pixels * weight;
            const float angle = atan2f(dy, dx) + p->swirl * weight;
            WPE_Vec4 v = wpe_color_to_vec4(wpe__texel_clamped(c, (int32_t)floorf(cx + from * cosf(angle)),
                                                              (int32_t)floorf(cy + from * sinf(angle))));
            const float inside = wpe__clamp01((horizon - d) / 1.5f + 0.5f) * darkness;
            const float keep = 1.0f - inside;
            v = wpe_vec4(wpe__clamp01(v.x * keep + p->ring_r * glow),
                         wpe__clamp01(v.y * keep + p->ring_g * glow),
                         wpe__clamp01(v.z * keep + p->ring_b * glow), v.w);
            c->dst[i] = wpe_color_from_vec4(v);
        }
    }
}

static uint32_t wpe__hash32(uint32_t x); /* With the cracks, below. */

/* Acrylic: passes 0 and 2 blur rows, 1 and 3 columns (box blurs of radius_pixels * strength), pass 4
 * saturates, tints and adds the grain (a hash of the pixel, so it stays still). */
static void wpe__acrylic_pass(const WPE_PostContext* c, const void* params_ptr, uint32_t pass,
                              uint32_t row_begin, uint32_t row_end) {
    const WPE_AcrylicParams* p = params_ptr;
    const float strength = wpe__clamp01(p->strength);
    const int32_t radius = (int32_t)lroundf(fminf(fmaxf(p->radius_pixels, 0.0f), 32.0f) * strength);
    const int32_t w = (int32_t)c->width;
    if (pass < 4) {
        const bool rows = (pass & 1u) == 0;
        for (uint32_t y = row_begin; y < row_end; ++y) {
            for (int32_t x = 0; x < w; ++x) {
                const size_t i = (size_t)y * c->width + (size_t)x;
                if (radius <= 0) {
                    c->dst[i] = c->src[i];
                    continue;
                }
                float sum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                for (int32_t k = -radius; k <= radius; ++k) {
                    const WPE_Vec4 v = wpe_color_to_vec4(rows ? wpe__texel_clamped(c, x + k, (int32_t)y)
                                                              : wpe__texel_clamped(c, x, (int32_t)y + k));
                    sum[0] += v.x;
                    sum[1] += v.y;
                    sum[2] += v.z;
                    sum[3] += v.w;
                }
                const float n = 1.0f / (float)(2 * radius + 1);
                c->dst[i] = wpe_color_from_vec4(wpe_vec4(sum[0] * n, sum[1] * n, sum[2] * n, sum[3] * n));
            }
        }
        return;
    }
    const float tint = wpe__clamp01(p->tint) * strength;
    const float saturation = 1.0f + (fminf(fmaxf(p->saturation, 0.0f), 3.0f) - 1.0f) * strength;
    const float noise = fminf(fmaxf(p->noise, 0.0f), 0.25f) * strength;
    for (uint32_t y = row_begin; y < row_end; ++y) {
        for (uint32_t x = 0; x < c->width; ++x) {
            const size_t i = (size_t)y * c->width + x;
            WPE_Vec4 v = wpe_color_to_vec4(c->src[i]);
            if (strength <= 0.0f) {
                c->dst[i] = c->src[i];
                continue;
            }
            const float gray = 0.299f * v.x + 0.587f * v.y + 0.114f * v.z;
            const float grain = noise * ((float)(wpe__hash32((uint32_t)i * 2654435761u + 0x51ED27u) >> 8) /
                                             16777216.0f * 2.0f -
                                         1.0f);
            const float rgb[3] = {v.x, v.y, v.z};
            const float tints[3] = {p->tint_r, p->tint_g, p->tint_b};
            float out[3];
            for (int k = 0; k < 3; ++k) {
                const float rich = gray + (rgb[k] - gray) * saturation;
                out[k] = wpe__clamp01(rich + (tints[k] - rich) * tint + grain);
            }
            c->dst[i] = wpe_color_from_vec4(wpe_vec4(out[0], out[1], out[2], v.w));
        }
    }
}

/* ---- Cracks ----------------------------------------------------------------------------------------- */

/* A well-mixed 32-bit hash (the "lowbias32" finalizer): the lines come from it, the same everywhere. */
static uint32_t wpe__hash32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

/* A number in [0, 1) for (seed, line, which). */
static float wpe__crack_random(uint32_t seed, uint32_t line, uint32_t which) {
    return (float)(wpe__hash32(seed * 0x9E3779B9u ^ wpe__hash32(line * 4u + which)) >> 8) / 16777216.0f;
}

uint32_t wpe_postfx_crack_lines(const WPE_CracksParams* p, uint32_t width, uint32_t height,
                                WPE_CrackLine* out) {
    if (!p || !out || !(p->count > 0.0f)) return 0;
    const float shown = fminf(p->count, (float)WPE_POSTFX_MAX_CRACKS);
    const uint32_t count = (uint32_t)ceilf(shown);
    const float w = (float)width, h = (float)height;
    const float diagonal = sqrtf(w * w + h * h);
    for (uint32_t i = 0; i < count; ++i) {
        /* A point in the disk of radius `spread` around the center (uniform over its area), and an angle. */
        const float r = sqrtf(wpe__crack_random(p->seed, i, 0)) * p->spread * h;
        const float around = wpe__crack_random(p->seed, i, 1) * 6.2831853f;
        const float angle = wpe__crack_random(p->seed, i, 2) * 3.14159265f;
        const float grown = fminf(shown - (float)i, 1.0f);
        out[i] = (WPE_CrackLine){.x = p->center_x * w + r * cosf(around),
                                 .y = p->center_y * h + r * sinf(around),
                                 .dx = cosf(angle),
                                 .dy = sinf(angle),
                                 .reach = diagonal * grown};
    }
    return count;
}

static void wpe__channels(WPE_Color c, float out[3]) {
    out[0] = (float)((c >> 16) & 0xFFu) / 255.0f;
    out[1] = (float)((c >> 8) & 0xFFu) / 255.0f;
    out[2] = (float)(c & 0xFFu) / 255.0f;
}

/* Jagged noise along a line in [0, 1]: random values at whole steps of `x`, joined by straight segments (so a
 * band's edge breaks in sharp corners, like glass). */
static float wpe__line_noise(uint32_t seed, uint32_t line, float x) {
    const float cell = floorf(x);
    const float f = x - cell;
    const uint32_t i = (uint32_t)(int32_t)cell;
    const float a = (float)(wpe__hash32(seed ^ wpe__hash32(line * 0x632BE5ABu + i)) >> 8) / 16777216.0f;
    const float b = (float)(wpe__hash32(seed ^ wpe__hash32(line * 0x632BE5ABu + i + 1u)) >> 8) / 16777216.0f;
    return a + (b - a) * f;
}

/* Like broken glass, only pixels near a cut change; the nearest cut shapes them. Within two pixels of it
 * the image is pulled across the cut (a seam); within the bevel zone it is sheared along the cut by an
 * amount that wanders along it (shear_waves times per 100 pixels; at 0 each cut slides by its own amount) and
 * fades with distance (the streaks of fractured glass); on one side a rim tinted by the glow color darkens
 * bright areas toward it and lights dark ones with it, so it shows on both; and an optional core blends the
 * cut itself toward the core color. */
static void wpe__cracks_pass(const WPE_PostContext* c, const void* params_ptr, uint32_t pass,
                             uint32_t row_begin, uint32_t row_end) {
    (void)pass;
    const WPE_CracksParams* p = params_ptr;
    WPE_CrackLine lines[WPE_POSTFX_MAX_CRACKS];
    const uint32_t count = p->strength > 0.0f ? wpe_postfx_crack_lines(p, c->width, c->height, lines) : 0;
    const float width = fmaxf(p->width_pixels, 0.0f), glow = fmaxf(p->glow_pixels, 0.0f);
    const float bevel = fmaxf(p->bevel_pixels, 0.0f);
    const float shear_zone = bevel > 0.0f ? bevel + 2.0f : 0.0f;
    const float zone = fmaxf(fmaxf(glow, shear_zone), fmaxf(width, 2.0f));
    const float core_color[3] = {p->core_r, p->core_g, p->core_b};
    const float tint[3] = {p->glow_r, p->glow_g, p->glow_b};
    /* Each cut's rim side and how far it pulls (between half and one and a half times offset). */
    float rim_side[WPE_POSTFX_MAX_CRACKS], pull[WPE_POSTFX_MAX_CRACKS];
    for (uint32_t l = 0; l < count; ++l) {
        rim_side[l] = wpe__crack_random(p->seed, l, 3) < 0.5f ? -1.0f : 1.0f;
        pull[l] = p->offset_pixels * (0.5f + wpe__crack_random(p->seed, l, 4));
    }
    for (uint32_t y = row_begin; y < row_end; ++y) {
        for (uint32_t x = 0; x < c->width; ++x) {
            const size_t i = (size_t)y * c->width + x;
            const float px = (float)x + 0.5f, py = (float)y + 0.5f;
            uint32_t nearest = UINT32_MAX;
            float d = zone, side = 0.0f, along = 0.0f, tip = 0.0f;
            float slide_x = 0.0f, slide_y = 0.0f; /* Each side of every cut slides along it, opposite ways. */
            for (uint32_t l = 0; l < count; ++l) {
                const WPE_CrackLine* line = &lines[l];
                const float vx = px - line->x, vy = py - line->y;
                const float a = vx * line->dx + vy * line->dy;
                if (fabsf(a) > line->reach) continue; /* Past the end of a growing line. */
                const float s = vx * -line->dy + vy * line->dx;
                const float slide = s >= 0.0f ? p->slide_pixels : -p->slide_pixels;
                slide_x += line->dx * slide;
                slide_y += line->dy * slide;
                if (fabsf(s) < d) {
                    nearest = l;
                    d = fabsf(s);
                    side = s;
                    along = a;
                    /* A growing line's end fades in over a few pixels instead of stopping flat. */
                    tip = fminf((line->reach - fabsf(a)) / (zone + 1.0f), 1.0f);
                }
            }
            slide_x *= p->strength;
            slide_y *= p->strength;
            if (nearest == UINT32_MAX) {
                c->dst[i] =
                    slide_x == 0.0f && slide_y == 0.0f
                        ? c->src[i]
                        : wpe__texel_clamped(c, (int32_t)floorf(px - slide_x), (int32_t)floorf(py - slide_y));
                continue;
            }
            const WPE_CrackLine* line = &lines[nearest];
            const float sign = side >= 0.0f ? 1.0f : -1.0f;
            const float nx = -line->dy, ny = line->dx; /* The cut's normal. */
            const float amount = p->strength * tip;
            /* The seam: pulled across the cut, fading over two pixels. */
            const float across = pull[nearest] * sign * (1.0f - wpe__clamp01(d * 0.5f)) * amount;
            float off_x = nx * across + slide_x, off_y = ny * across + slide_y;
            /* The streaks: sheared along the cut. */
            if (d < shear_zone) {
                const float jitter =
                    wpe__line_noise(p->seed, nearest, along * fmaxf(p->shear_waves, 0.0f) * 0.01f) * 2.0f -
                    1.0f;
                const float shear = jitter * (1.0f - d / shear_zone) * bevel * 0.6f * amount;
                off_x += line->dx * shear;
                off_y += line->dy * shear;
            }
            float rgb[3];
            wpe__channels(wpe__texel_clamped(c, (int32_t)floorf(px - off_x), (int32_t)floorf(py - off_y)),
                          rgb);
            /* The rim, on one side: darker toward the tint over bright areas, lit with it over dark ones. */
            if (glow > 0.0f && d < glow && sign == rim_side[nearest]) {
                const float rim = (1.0f - d / glow) * 0.3f * amount;
                const float lum = 0.299f * rgb[0] + 0.587f * rgb[1] + 0.114f * rgb[2];
                for (int k = 0; k < 3; ++k) {
                    rgb[k] = lum > 0.5f ? rgb[k] * (1.0f - rim * (1.0f - wpe__clamp01(tint[k])) * 0.55f)
                                        : rgb[k] + rim * tint[k];
                }
            }
            /* The cut itself, when it has a core. */
            if (width > 0.0f && d < width) {
                const float blend = (1.0f - d / width) * amount;
                for (int k = 0; k < 3; ++k)
                    rgb[k] += (wpe__clamp01(core_color[k]) - rgb[k]) * blend;
            }
            const float a = (float)((c->src[i] >> 24) & 0xFFu) / 255.0f;
            c->dst[i] = wpe_color_from_vec4(
                wpe_vec4(wpe__clamp01(rgb[0]), wpe__clamp01(rgb[1]), wpe__clamp01(rgb[2]), a));
        }
    }
}
/* ---- Shatter ---------------------------------------------------------------------------------------- */

#define WPE__MAX_SHARDS 512u
#define WPE__SHARD_TABLE 1024u /* Open addressing; twice the shards. */

/* A piece: the pixels on the same side of every line (its mask). It turns about its center and moves. */
typedef struct wpe__Shard {
    uint32_t mask;
    float cx, cy; /* Its center (the average of its pixels). */
    float radius; /* Reaches every pixel of it from the center. */
    float ox, oy; /* Where it has moved. */
    float cs, sn; /* Its turn (cosine, sine). */
    float order;  /* Pieces with a higher order are drawn over lower ones. */
} wpe__Shard;

typedef struct wpe__ShardSum {
    uint32_t mask;
    uint32_t samples;
    float sum_x, sum_y, min_x, min_y, max_x, max_y;
} wpe__ShardSum;

typedef struct wpe__ShatterScratch {
    uint32_t line_count;
    float lx[WPE_POSTFX_MAX_SHATTER_LINES], ly[WPE_POSTFX_MAX_SHATTER_LINES]; /* A point on each line. */
    float nx[WPE_POSTFX_MAX_SHATTER_LINES], ny[WPE_POSTFX_MAX_SHATTER_LINES]; /* Its unit normal. */
    uint32_t shard_count;
    wpe__Shard shards[WPE__MAX_SHARDS];
    wpe__ShardSum table[WPE__SHARD_TABLE]; /* Used while preparing; samples == 0 is empty. */
} wpe__ShatterScratch;

/* Which side of each line a point is on: the piece it belongs to. */
static uint32_t wpe__shard_mask(const wpe__ShatterScratch* s, float x, float y) {
    uint32_t mask = 0;
    for (uint32_t l = 0; l < s->line_count; ++l) {
        if ((x - s->lx[l]) * s->nx[l] + (y - s->ly[l]) * s->ny[l] >= 0.0f) mask |= 1u << l;
    }
    return mask;
}

/* Finds the pieces (sampling every other pixel), then where each has moved and turned at `time`. */
static void wpe__shatter_prepare(const WPE_PostContext* c, const void* params_ptr, void* scratch_ptr) {
    const WPE_ShatterParams* p = params_ptr;
    wpe__ShatterScratch* s = scratch_ptr;
    const WPE_CracksParams lines_params = {.count =
                                               fminf(ceilf(p->count), (float)WPE_POSTFX_MAX_SHATTER_LINES),
                                           .seed = p->seed,
                                           .center_x = p->center_x,
                                           .center_y = p->center_y,
                                           .spread = p->spread};
    WPE_CrackLine lines[WPE_POSTFX_MAX_CRACKS];
    s->line_count = wpe_postfx_crack_lines(&lines_params, c->width, c->height, lines);
    for (uint32_t l = 0; l < s->line_count; ++l) {
        s->lx[l] = lines[l].x;
        s->ly[l] = lines[l].y;
        s->nx[l] = -lines[l].dy;
        s->ny[l] = lines[l].dx;
    }
    memset(s->table, 0, sizeof s->table);
    for (uint32_t y = 0; y < c->height; y += 2) {
        for (uint32_t x = 0; x < c->width; x += 2) {
            const float px = (float)x + 0.5f, py = (float)y + 0.5f;
            const uint32_t mask = wpe__shard_mask(s, px, py);
            uint32_t slot = wpe__hash32(mask) & (WPE__SHARD_TABLE - 1);
            for (uint32_t probe = 0; probe < WPE__SHARD_TABLE; ++probe) {
                wpe__ShardSum* sum = &s->table[slot];
                if (sum->samples == 0) {
                    *sum = (wpe__ShardSum){.mask = mask, .min_x = px, .min_y = py, .max_x = px, .max_y = py};
                }
                if (sum->mask == mask) {
                    ++sum->samples;
                    sum->sum_x += px;
                    sum->sum_y += py;
                    sum->min_x = fminf(sum->min_x, px);
                    sum->min_y = fminf(sum->min_y, py);
                    sum->max_x = fmaxf(sum->max_x, px);
                    sum->max_y = fmaxf(sum->max_y, py);
                    break;
                }
                slot = (slot + 1) & (WPE__SHARD_TABLE - 1);
            }
        }
    }
    const float h = (float)c->height;
    const float center_x = p->center_x * (float)c->width, center_y = p->center_y * h;
    s->shard_count = 0;
    for (uint32_t i = 0; i < WPE__SHARD_TABLE && s->shard_count < WPE__MAX_SHARDS; ++i) {
        const wpe__ShardSum* sum = &s->table[i];
        if (sum->samples == 0) continue;
        wpe__Shard* shard = &s->shards[s->shard_count++];
        const float cx = sum->sum_x / (float)sum->samples, cy = sum->sum_y / (float)sum->samples;
        /* Out to the farthest corner of its bounds, plus the sampling gap. */
        const float far_x = fmaxf(cx - sum->min_x, sum->max_x - cx) + 2.0f;
        const float far_y = fmaxf(cy - sum->min_y, sum->max_y - cy) + 2.0f;
        /* Away from where the cuts cross, a little faster or slower each, with a small upward kick. */
        const uint32_t key = p->seed ^ wpe__hash32(sum->mask + 0x51ED27u);
        float dx = cx - center_x, dy = cy - center_y;
        const float length = sqrtf(dx * dx + dy * dy);
        if (length > 1e-3f) {
            dx /= length;
            dy /= length;
        } else {
            const float angle = wpe__crack_random(key, 0, 0) * 6.2831853f;
            dx = cosf(angle);
            dy = sinf(angle);
        }
        const float speed = p->speed * h * (0.6f + 0.8f * wpe__crack_random(key, 0, 1));
        const float vx = dx * speed, vy = dy * speed - p->speed * h * 0.4f * wpe__crack_random(key, 0, 2);
        const float t = p->time;
        const float turn = p->spin * (2.0f * wpe__crack_random(key, 0, 3) - 1.0f) * t;
        *shard = (wpe__Shard){.mask = sum->mask,
                              .cx = cx,
                              .cy = cy,
                              .radius = sqrtf(far_x * far_x + far_y * far_y),
                              .ox = vx * t,
                              .oy = vy * t + 0.5f * p->gravity * h * t * t,
                              .cs = cosf(turn),
                              .sn = sinf(turn),
                              .order = wpe__crack_random(key, 0, 4)};
    }
    /* Draw order: highest first, so the first piece found over a pixel is the one on top. */
    for (uint32_t i = 1; i < s->shard_count; ++i) {
        const wpe__Shard shard = s->shards[i];
        uint32_t j = i;
        for (; j > 0 && s->shards[j - 1].order < shard.order; --j)
            s->shards[j] = s->shards[j - 1];
        s->shards[j] = shard;
    }
}

/* Each pixel shows the piece now over it (found by moving the pixel back to where that piece was), with a
 * pale edge along the cut, or the scene behind. */
static void wpe__shatter_pass(const WPE_PostContext* c, const void* params_ptr, uint32_t pass,
                              uint32_t row_begin, uint32_t row_end) {
    (void)pass;
    const WPE_ShatterParams* p = params_ptr;
    const wpe__ShatterScratch* s = c->scratch;
    const WPE_TextureLevel* image = p->image ? &p->image->levels[0] : NULL;
    const float dim = 1.0f - wpe__clamp01(p->background);
    /* The pieces fade into what's behind them as time runs (eased, so they linger before going). */
    const float gone = p->fade > 0.0f ? wpe__clamp01(p->time / p->fade) : 0.0f;
    const float opacity = 1.0f - gone * gone;
    for (uint32_t y = row_begin; y < row_end; ++y) {
        for (uint32_t x = 0; x < c->width; ++x) {
            const size_t i = (size_t)y * c->width + x;
            const float px = (float)x + 0.5f, py = (float)y + 0.5f;
            bool covered = false;
            for (uint32_t k = 0; k < s->shard_count && !covered; ++k) {
                const wpe__Shard* shard = &s->shards[k];
                const float qx = px - shard->cx - shard->ox, qy = py - shard->cy - shard->oy;
                if (qx * qx + qy * qy > shard->radius * shard->radius) continue;
                const float sx = shard->cx + shard->cs * qx + shard->sn * qy;
                const float sy = shard->cy - shard->sn * qx + shard->cs * qy;
                if (!(sx >= 0.0f && sy >= 0.0f && sx < (float)c->width && sy < (float)c->height)) continue;
                if (wpe__shard_mask(s, sx, sy) != shard->mask) continue;
                covered = true;
                WPE_Color color;
                if (image && image->width > 0 && image->height > 0) {
                    const uint32_t ix = (uint32_t)(sx * (float)image->width / (float)c->width);
                    const uint32_t iy = (uint32_t)(sy * (float)image->height / (float)c->height);
                    color =
                        image->texels[(size_t)(iy < image->height ? iy : image->height - 1) * image->width +
                                      (ix < image->width ? ix : image->width - 1)];
                } else {
                    color = c->src[(size_t)sy * c->width + (size_t)sx];
                }
                /* Glass edges: lighter within a pixel of a cut. */
                float nearest = 1e9f;
                for (uint32_t l = 0; l < s->line_count; ++l)
                    nearest = fminf(nearest, fabsf((sx - s->lx[l]) * s->nx[l] + (sy - s->ly[l]) * s->ny[l]));
                color = nearest < 1.0f && p->time > 0.0f ? wpe__scale_rgb(color, 1.35f) : color;
                c->dst[i] =
                    opacity < 1.0f ? wpe__mix_colors(wpe__scale_rgb(c->src[i], dim), color, opacity) : color;
            }
            if (!covered) c->dst[i] = wpe__scale_rgb(c->src[i], dim);
        }
    }
}

#define WPE__RADIAL_SAMPLES 8

static void wpe__radial_blur_pass(const WPE_PostContext* c, const void* params_ptr, uint32_t pass,
                                  uint32_t row_begin, uint32_t row_end) {
    (void)pass;
    const WPE_RadialBlurParams* p = params_ptr;
    const float cx = p->center_x * (float)c->width, cy = p->center_y * (float)c->height;
    const float strength = wpe__clamp01(p->strength);
    for (uint32_t y = row_begin; y < row_end; ++y) {
        for (uint32_t x = 0; x < c->width; ++x) {
            const float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float sum[3] = {0.0f, 0.0f, 0.0f};
            for (int k = 0; k < WPE__RADIAL_SAMPLES; ++k) {
                const float t = strength * (float)k / (float)WPE__RADIAL_SAMPLES;
                const WPE_Color s = wpe__texel_clamped(c, (int32_t)floorf(px + (cx - px) * t),
                                                       (int32_t)floorf(py + (cy - py) * t));
                const WPE_Vec4 v = wpe_color_to_vec4(s);
                sum[0] += v.x;
                sum[1] += v.y;
                sum[2] += v.z;
            }
            const size_t i = (size_t)y * c->width + x;
            const float n = (float)WPE__RADIAL_SAMPLES;
            c->dst[i] = wpe_color_from_vec4(
                wpe_vec4(sum[0] / n, sum[1] / n, sum[2] / n, wpe_color_to_vec4(c->src[i]).w));
        }
    }
}

/* ---- Curve distortion ------------------------------------------------------------------------------- */

#define WPE__CURVE_MAX_TAPS 16u
#define WPE__CURVE_CUTOFF 0.03f /* Intensity below which pixels are left alone (exp(-3.5)). */

/* The image at a point (pixel centers at .5), bilinear, clamped to the edges. */
static void wpe__sample_bilinear(const WPE_PostContext* c, float x, float y, float out[3]) {
    x = fminf(fmaxf(x - 0.5f, -1.0f), (float)c->width);
    y = fminf(fmaxf(y - 0.5f, -1.0f), (float)c->height);
    const float fx = floorf(x), fy = floorf(y);
    const float tx = x - fx, ty = y - fy;
    const int32_t x0 = (int32_t)fx, y0 = (int32_t)fy;
    float a[3], b[3], d[3], e[3];
    wpe__channels(wpe__texel_clamped(c, x0, y0), a);
    wpe__channels(wpe__texel_clamped(c, x0 + 1, y0), b);
    wpe__channels(wpe__texel_clamped(c, x0, y0 + 1), d);
    wpe__channels(wpe__texel_clamped(c, x0 + 1, y0 + 1), e);
    for (int k = 0; k < 3; ++k) {
        const float top = a[k] + (b[k] - a[k]) * tx, bottom = d[k] + (e[k] - d[k]) * tx;
        out[k] = top + (bottom - top) * ty;
    }
}

/* Each pixel near the curve shows the image from further across it (along the curve's perpendicular), blended
 * over a spread of such offsets so the push smears; red and blue come from either side of the push, a core
 * line and a tint go on top. The nearest point on the polyline decides everything, so it all varies smoothly
 * between the points. */
static void wpe__curve_distort_pass(const WPE_PostContext* c, const void* params_ptr, uint32_t pass,
                                    uint32_t row_begin, uint32_t row_end) {
    (void)pass;
    const WPE_CurveDistortParams* p = params_ptr;
    const uint32_t n = p->points ? p->point_count : 0;
    const float strength = wpe__clamp01(p->strength);
    const float width = fmaxf(p->width, 0.5f), taper = wpe__clamp01(p->taper);
    const float prism = fmaxf(p->prism, 0.0f), core = fmaxf(p->core, 0.0f), ripple = fmaxf(p->ripple, 0.0f);
    const uint32_t taps =
        p->smear_taps < 1 ? 1 : (p->smear_taps > WPE__CURVE_MAX_TAPS ? WPE__CURVE_MAX_TAPS : p->smear_taps);
    const float half = taps > 1 ? 0.5f * (float)(taps - 1) : 1.0f;
    const float tint[3] = {p->tint.x, p->tint.y, p->tint.z};
    /* Pixels further than this from every point are untouched (the falloff is below the cutoff there). */
    const float reach = width * 3.5f + 1.0f;
    float min_x = INFINITY, min_y = INFINITY, max_x = -INFINITY, max_y = -INFINITY;
    for (uint32_t k = 0; k < (n >= 2 && strength > 0.0f ? n : 0); ++k) {
        const float* point = &p->points[(size_t)k * 3];
        min_x = fminf(min_x, point[0] - reach);
        max_x = fmaxf(max_x, point[0] + reach);
        min_y = fminf(min_y, point[1] - reach);
        max_y = fmaxf(max_y, point[1] + reach);
    }
    for (uint32_t y = row_begin; y < row_end; ++y) {
        for (uint32_t x = 0; x < c->width; ++x) {
            const size_t i = (size_t)y * c->width + x;
            const float px = (float)x + 0.5f, py = (float)y + 0.5f;
            c->dst[i] = c->src[i];
            if (!(px >= min_x && px <= max_x && py >= min_y && py <= max_y)) continue;
            /* The nearest point on the polyline: its segment and where along it. */
            float best_d2 = INFINITY, best_t = 0.0f;
            uint32_t seg = 0;
            for (uint32_t k = 0; k + 1 < n; ++k) {
                const float* a = &p->points[(size_t)k * 3];
                const float vx = a[3] - a[0], vy = a[4] - a[1];
                const float len2 = vx * vx + vy * vy;
                const float t =
                    len2 > 1e-6f ? wpe__clamp01(((px - a[0]) * vx + (py - a[1]) * vy) / len2) : 0.0f;
                const float dx = px - (a[0] + vx * t), dy = py - (a[1] + vy * t);
                const float d2 = dx * dx + dy * dy;
                if (d2 < best_d2) {
                    best_d2 = d2;
                    best_t = t;
                    seg = k;
                }
            }
            const float dist = sqrtf(best_d2);
            /* Widest and strongest in the middle of the curve, pinched toward its ends. */
            const float u = ((float)seg + best_t) / (float)(n - 1);
            const float profile = fmaxf(powf(fmaxf(sinf(WPE_PI * u), 0.0f), 2.5f), 0.2f);
            const float twidth = (1.0f - taper) + taper * profile;
            const float tdisp = 1.0f - taper * 0.5f * (1.0f - profile);
            float intensity = strength * expf(-dist / (width * twidth));
            if (intensity < WPE__CURVE_CUTOFF) continue;
            /* Weaker over surfaces in front of the curve. */
            const float* a = &p->points[(size_t)seg * 3];
            const float curve_depth = a[2] + (a[5] - a[2]) * best_t;
            if (c->depth[i] < curve_depth - 1e-4f) intensity *= 1.0f - wpe__clamp01(p->depth_fade);
            if (intensity < WPE__CURVE_CUTOFF) continue;
            /* A smooth tangent: central differences at the segment's ends, blended across it. */
            const uint32_t before = seg == 0 ? 0 : seg - 1, after = seg + 2 < n ? seg + 2 : n - 1;
            const float* b = &p->points[(size_t)before * 3];
            const float* f = &p->points[(size_t)after * 3];
            float tx = a[3] - b[0], ty = a[4] - b[1];
            tx += (f[0] - a[0] - tx) * best_t;
            ty += (f[1] - a[1] - ty) * best_t;
            const float tl = sqrtf(tx * tx + ty * ty);
            const float nx = tl > 1e-4f ? -ty / tl : 0.0f, ny = tl > 1e-4f ? tx / tl : 1.0f;
            const float jitter = ripple > 0.0f
                                     ? (wpe__line_noise(0x5EEDu, 0, p->phase * 12.0f + dist * 0.3f) - 0.5f) *
                                           2.0f * ripple * intensity
                                     : 0.0f;
            const float disp = p->amplitude * intensity * tdisp;
            const float spread = fmaxf(p->smear_spread, 0.0f) * disp;
            float rgb[3] = {0.0f, 0.0f, 0.0f}, sum = 0.0f;
            for (uint32_t k = 0; k < taps; ++k) {
                const float s = taps > 1 ? ((float)k - half) / half : 0.0f;  /* -1..1 */
                const float weight = 1.0f - fabsf(s) * half / (half + 1.0f); /* A triangle, ends included. */
                const float off = disp + s * spread + jitter;
                float tap[3];
                wpe__sample_bilinear(c, px + nx * off, py + ny * off, tap);
                for (int ch = 0; ch < 3; ++ch)
                    rgb[ch] += tap[ch] * weight;
                sum += weight;
            }
            for (int ch = 0; ch < 3; ++ch)
                rgb[ch] /= sum;
            if (prism > 0.0f) {
                const float blend = wpe__clamp01(intensity * 0.6f);
                const float off_r = disp + jitter + prism * intensity,
                            off_b = disp + jitter - prism * intensity;
                float red[3], blue[3];
                wpe__sample_bilinear(c, px + nx * off_r, py + ny * off_r, red);
                wpe__sample_bilinear(c, px + nx * off_b, py + ny * off_b, blue);
                rgb[0] += (red[0] - rgb[0]) * blend;
                rgb[2] += (blue[2] - rgb[2]) * blend;
            }
            const float core_radius = core * (0.2f + 0.8f * profile);
            if (core_radius > 0.0f && dist < core_radius) {
                const float blend = (1.0f - dist / core_radius) * wpe__clamp01(intensity) * 0.85f;
                for (int ch = 0; ch < 3; ++ch)
                    rgb[ch] += (1.0f - rgb[ch]) * blend;
            }
            const float tint_blend = wpe__clamp01(p->tint.w) * wpe__clamp01(intensity) * 0.5f;
            for (int ch = 0; ch < 3; ++ch)
                rgb[ch] = wpe__clamp01(rgb[ch] + (tint[ch] - rgb[ch]) * tint_blend);
            c->dst[i] = wpe_color_from_vec4(wpe_vec4(rgb[0], rgb[1], rgb[2], wpe_color_to_vec4(c->src[i]).w));
        }
    }
}

/* ---- Constructors ---------------------------------------------------------------------------------- */

static const WPE_SsaoParams wpe__ssao_defaults = {
    .radius_pixels = 6.0f, .strength = 0.8f, .depth_range = 1.0f, .bias = 0.02f};
static const WPE_ChromaticAberrationParams wpe__chromatic_defaults = {.strength_pixels = 2.0f};
static const WPE_Ps1Params wpe__ps1_defaults = {.levels = 32, .dither_strength = 1.0f};
static const WPE_VignetteParams wpe__vignette_defaults = {.strength = 0.5f, .radius = 0.5f};
static const WPE_ColorAdjustParams wpe__color_adjust_defaults = {
    .brightness = 0.0f, .contrast = 1.0f, .saturation = 1.0f};
static const WPE_DepthOfFieldParams wpe__dof_defaults = {
    .focus_distance = 8.0f, .focus_range = 2.0f, .falloff = 10.0f, .radius_pixels = 3.0f};

WPE_PostEffect wpe_postfx_depth_of_field(const WPE_DepthOfFieldParams* params) {
    return (WPE_PostEffect){.name = "depth_of_field",
                            .pass = wpe__dof_pass,
                            .pass_count = 3,
                            .params = params ? params : &wpe__dof_defaults,
                            .enabled = true};
}

WPE_PostEffect wpe_postfx_ssao(const WPE_SsaoParams* params) {
    return (WPE_PostEffect){.name = "ssao",
                            .pass = wpe__ssao_pass,
                            .pass_count = 2,
                            .params = params ? params : &wpe__ssao_defaults,
                            .enabled = true};
}

WPE_PostEffect wpe_postfx_chromatic_aberration(const WPE_ChromaticAberrationParams* params) {
    return (WPE_PostEffect){.name = "chromatic_aberration",
                            .pass = wpe__chromatic_pass,
                            .pass_count = 1,
                            .params = params ? params : &wpe__chromatic_defaults,
                            .enabled = true};
}

WPE_PostEffect wpe_postfx_ps1(const WPE_Ps1Params* params) {
    return (WPE_PostEffect){.name = "ps1",
                            .pass = wpe__ps1_pass,
                            .pass_count = 1,
                            .params = params ? params : &wpe__ps1_defaults,
                            .enabled = true};
}

WPE_PostEffect wpe_postfx_vignette(const WPE_VignetteParams* params) {
    return (WPE_PostEffect){.name = "vignette",
                            .pass = wpe__vignette_pass,
                            .pass_count = 1,
                            .params = params ? params : &wpe__vignette_defaults,
                            .enabled = true};
}

WPE_PostEffect wpe_postfx_color_adjust(const WPE_ColorAdjustParams* params) {
    return (WPE_PostEffect){.name = "color_adjust",
                            .pass = wpe__color_adjust_pass,
                            .pass_count = 1,
                            .params = params ? params : &wpe__color_adjust_defaults,
                            .enabled = true};
}

static const WPE_ShockwaveParams wpe__shockwave_defaults = {
    .center_x = 0.5f, .center_y = 0.5f, .radius = 0.3f, .width = 0.08f, .strength_pixels = 6.0f};
static const WPE_RadialBlurParams wpe__radial_blur_defaults = {
    .center_x = 0.5f, .center_y = 0.5f, .strength = 0.15f};
static const WPE_CracksParams wpe__cracks_defaults = {.count = 8.0f,
                                                      .seed = 1,
                                                      .center_x = 0.5f,
                                                      .center_y = 0.5f,
                                                      .spread = 0.15f,
                                                      .offset_pixels = 3.0f,
                                                      .slide_pixels = 1.5f,
                                                      .width_pixels = 0.0f,
                                                      .glow_pixels = 5.0f,
                                                      .bevel_pixels = 5.0f,
                                                      .shear_waves = 8.0f,
                                                      .strength = 1.0f,
                                                      .core_r = 0.05f,
                                                      .core_g = 0.06f,
                                                      .core_b = 0.08f,
                                                      .glow_r = 0.1f,
                                                      .glow_g = 0.2f,
                                                      .glow_b = 0.6f};

WPE_PostEffect wpe_postfx_cracks(const WPE_CracksParams* params) {
    return (WPE_PostEffect){.name = "cracks",
                            .pass = wpe__cracks_pass,
                            .pass_count = 1,
                            .params = params ? params : &wpe__cracks_defaults,
                            .enabled = true};
}

static const WPE_ShatterParams wpe__shatter_defaults = {.count = 10.0f,
                                                        .seed = 1,
                                                        .center_x = 0.5f,
                                                        .center_y = 0.5f,
                                                        .spread = 0.15f,
                                                        .speed = 0.35f,
                                                        .gravity = 1.2f,
                                                        .spin = 2.5f,
                                                        .background = 0.6f};

WPE_PostEffect wpe_postfx_shatter(const WPE_ShatterParams* params) {
    return (WPE_PostEffect){.name = "shatter",
                            .pass = wpe__shatter_pass,
                            .pass_count = 1,
                            .prepare = wpe__shatter_prepare,
                            .scratch_size = sizeof(wpe__ShatterScratch),
                            .params = params ? params : &wpe__shatter_defaults,
                            .enabled = true};
}

static const WPE_BlackHoleParams wpe__black_hole_defaults = {.center_x = 0.5f,
                                                             .center_y = 0.5f,
                                                             .radius = 0.08f,
                                                             .reach = 0.15f,
                                                             .pull_pixels = 20.0f,
                                                             .swirl = 1.5f,
                                                             .darkness = 1.0f,
                                                             .ring_pixels = 2.0f,
                                                             .ring_r = 0.8f,
                                                             .ring_g = 0.4f,
                                                             .ring_b = 1.2f};

WPE_PostEffect wpe_postfx_black_hole(const WPE_BlackHoleParams* params) {
    return (WPE_PostEffect){.name = "black_hole",
                            .pass = wpe__black_hole_pass,
                            .pass_count = 1,
                            .params = params ? params : &wpe__black_hole_defaults,
                            .enabled = true};
}

static const WPE_AcrylicParams wpe__acrylic_defaults = {.strength = 1.0f,
                                                        .radius_pixels = 6.0f,
                                                        .tint_r = 0.08f,
                                                        .tint_g = 0.1f,
                                                        .tint_b = 0.16f,
                                                        .tint = 0.45f,
                                                        .saturation = 1.4f,
                                                        .noise = 0.04f};

WPE_PostEffect wpe_postfx_acrylic(const WPE_AcrylicParams* params) {
    return (WPE_PostEffect){.name = "acrylic",
                            .pass = wpe__acrylic_pass,
                            .pass_count = 5,
                            .params = params ? params : &wpe__acrylic_defaults,
                            .enabled = true};
}

WPE_PostEffect wpe_postfx_curve_distort(const WPE_CurveDistortParams* params) {
    return (WPE_PostEffect){.name = "curve_distort",
                            .pass = wpe__curve_distort_pass,
                            .pass_count = 1,
                            .params = params,
                            .enabled = params != NULL};
}

static const WPE_GrayscaleParams wpe__grayscale_defaults = {
    .strength = 1.0f, .tint_r = 1.0f, .tint_g = 1.0f, .tint_b = 1.0f};

WPE_PostEffect wpe_postfx_grayscale(const WPE_GrayscaleParams* params) {
    return (WPE_PostEffect){.name = "grayscale",
                            .pass = wpe__grayscale_pass,
                            .pass_count = 1,
                            .params = params ? params : &wpe__grayscale_defaults,
                            .enabled = true};
}

WPE_PostEffect wpe_postfx_shockwave(const WPE_ShockwaveParams* params) {
    return (WPE_PostEffect){.name = "shockwave",
                            .pass = wpe__shockwave_pass,
                            .pass_count = 1,
                            .params = params ? params : &wpe__shockwave_defaults,
                            .enabled = true};
}

WPE_PostEffect wpe_postfx_radial_blur(const WPE_RadialBlurParams* params) {
    return (WPE_PostEffect){.name = "radial_blur",
                            .pass = wpe__radial_blur_pass,
                            .pass_count = 1,
                            .params = params ? params : &wpe__radial_blur_defaults,
                            .enabled = true};
}

/* ---- Under water ------------------------------------------------------------------------------------- */

static const WPE_UnderwaterParams wpe__underwater_defaults = {.surface_y = -1.0f,
                                                              .left = -1.0f,
                                                              .right = 2.0f,
                                                              .bottom = 2.0f,
                                                              .color_r = 0.05f,
                                                              .color_g = 0.3f,
                                                              .color_b = 0.5f,
                                                              .tint = 0.2f,
                                                              .deep_tint = 0.55f,
                                                              .deep = 1.5f,
                                                              .darken = 0.3f,
                                                              .murk = 0.7f,
                                                              .murk_start = 8.0f,
                                                              .murk_distance = 30.0f,
                                                              .wobble_pixels = 1.0f,
                                                              .wobble_rows = 24.0f,
                                                              .wobble_speed = 0.6f,
                                                              .wave_pixels = 1.5f,
                                                              .wave_length = 48.0f,
                                                              .glow = 0.35f};

#define WPE__UNDERWATER_PERIOD 600.0f /* Seconds the clock wraps at (sways and waves stay smooth enough). */

/* Below the (waving) line: the image sampled from a sideways-swayed row, mixed toward the water's color by
 * the larger of the depth's tint and the murk (combined as two layers of fog), then darkened by depth; the
 * line's own pixels glow. */
static void wpe__underwater_pass(const WPE_PostContext* c, const void* params_ptr, uint32_t pass,
                                 uint32_t row_begin, uint32_t row_end) {
    (void)pass;
    const WPE_UnderwaterParams* p = params_ptr;
    const float w = (float)c->width, h = (float)c->height;
    const float left = p->left * w, right = p->right * w, line = p->surface_y * h, floor = p->bottom * h;
    const float deep = fmaxf(p->deep, 1e-4f) * h;
    const float phase = p->time * p->wobble_speed * WPE__TWO_PI_F;
    const float row_step = WPE__TWO_PI_F / fmaxf(p->wobble_rows, 1.0f);
    const float wave_step = WPE__TWO_PI_F / fmaxf(p->wave_length, 1.0f);
    const float water[3] = {p->color_r * 255.0f, p->color_g * 255.0f, p->color_b * 255.0f};
    for (uint32_t y = row_begin; y < row_end; ++y) {
        const float fy = (float)y + 0.5f;
        const int32_t sway = (int32_t)lroundf(p->wobble_pixels * sinf((float)y * row_step + phase));
        for (uint32_t x = 0; x < c->width; ++x) {
            const size_t i = (size_t)y * c->width + x;
            const float fx = (float)x + 0.5f;
            /* The line waves along x, travelling slowly (half the sway's pace). */
            const float surface = line + p->wave_pixels * sinf(fx * wave_step + 0.5f * phase);
            if (fy < surface || fy >= floor || fx < left || fx >= right) {
                c->dst[i] = c->src[i];
                continue;
            }
            const float below = fy - surface;
            const float g = fminf(below / deep, 1.0f);
            float mix = p->tint + (p->deep_tint - p->tint) * g;
            if (p->murk_distance > 0.0f && c->depth) {
                const float z = wpe_postfx_linear_depth(&c->camera, c->depth[i]);
                const float murk = p->murk * wpe__clamp01((z - p->murk_start) / p->murk_distance);
                mix = 1.0f - (1.0f - wpe__clamp01(mix)) * (1.0f - murk);
            }
            mix = wpe__clamp01(mix);
            const float shade = 1.0f - wpe__clamp01(p->darken) * g;
            const float glow = below < 1.5f ? p->glow * 255.0f * (1.0f - below / 1.5f) : 0.0f;
            const WPE_Color s = wpe__texel_clamped(c, (int32_t)x + sway, (int32_t)y);
            const float rgb[3] = {(float)((s >> 16) & 0xFFu), (float)((s >> 8) & 0xFFu), (float)(s & 0xFFu)};
            uint32_t out[3];
            for (int k = 0; k < 3; ++k) {
                const float v = (rgb[k] + (water[k] - rgb[k]) * mix) * shade + glow;
                out[k] = (uint32_t)lroundf(fminf(fmaxf(v, 0.0f), 255.0f));
            }
            c->dst[i] = (s & 0xFF000000u) | (out[0] << 16) | (out[1] << 8) | out[2];
        }
    }
}

WPE_PostEffect wpe_postfx_underwater(const WPE_UnderwaterParams* params) {
    return (WPE_PostEffect){.name = "underwater",
                            .pass = wpe__underwater_pass,
                            .pass_count = 1,
                            .params = params ? params : &wpe__underwater_defaults,
                            .enabled = true};
}

void wpe_postfx_advance(WPE_PostFxConfig* configs, uint32_t count, float dt) {
    if (!configs || !(dt > 0.0f)) return;
    for (uint32_t i = 0; i < count; ++i) {
        if (configs[i].kind != WPE_POSTFX_UNDERWATER) continue;
        configs[i].underwater.time = fmodf(configs[i].underwater.time + dt, WPE__UNDERWATER_PERIOD);
    }
}

/* ---- Configured effects ----------------------------------------------------------------------------- */

static const char* const wpe__kind_names[] = {"ssao",         "chromatic_aberration",
                                              "ps1",          "vignette",
                                              "color_adjust", "depth_of_field",
                                              "shockwave",    "radial_blur",
                                              "grayscale",    "cracks",
                                              "shatter",      "black_hole",
                                              "acrylic",      "underwater"};
static_assert(sizeof wpe__kind_names / sizeof wpe__kind_names[0] == WPE_POSTFX_KIND_COUNT,
              "a name for every effect kind");

/* Each kind's parameters in scene-file order, with the ranges files and scripts may use. */
static const WPE_PostFxParam wpe__ssao_params[] = {
    {"radius_pixels", offsetof(WPE_PostFxConfig, ssao.radius_pixels), 0.5, 64, false},
    {"strength", offsetof(WPE_PostFxConfig, ssao.strength), 0, 1, false},
    {"depth_range", offsetof(WPE_PostFxConfig, ssao.depth_range), 0, 1e6, false},
    {"bias", offsetof(WPE_PostFxConfig, ssao.bias), 0, 1e6, false}};
static const WPE_PostFxParam wpe__chromatic_params[] = {
    {"strength_pixels", offsetof(WPE_PostFxConfig, chromatic_aberration.strength_pixels), 0, 64, false}};
static const WPE_PostFxParam wpe__ps1_params[] = {
    {"levels", offsetof(WPE_PostFxConfig, ps1.levels), 2, 256, true},
    {"dither_strength", offsetof(WPE_PostFxConfig, ps1.dither_strength), 0, 1, false}};
static const WPE_PostFxParam wpe__vignette_params[] = {
    {"strength", offsetof(WPE_PostFxConfig, vignette.strength), 0, 1, false},
    {"radius", offsetof(WPE_PostFxConfig, vignette.radius), 0, 1.5, false}};
static const WPE_PostFxParam wpe__color_adjust_params[] = {
    {"brightness", offsetof(WPE_PostFxConfig, color_adjust.brightness), -1, 1, false},
    {"contrast", offsetof(WPE_PostFxConfig, color_adjust.contrast), 0, 4, false},
    {"saturation", offsetof(WPE_PostFxConfig, color_adjust.saturation), 0, 4, false}};
static const WPE_PostFxParam wpe__dof_params[] = {
    {"focus_distance", offsetof(WPE_PostFxConfig, depth_of_field.focus_distance), 0, 1e6, false},
    {"focus_range", offsetof(WPE_PostFxConfig, depth_of_field.focus_range), 0, 1e6, false},
    {"falloff", offsetof(WPE_PostFxConfig, depth_of_field.falloff), 0, 1e6, false},
    {"radius_pixels", offsetof(WPE_PostFxConfig, depth_of_field.radius_pixels), 0, 16, false}};
static const WPE_PostFxParam wpe__shockwave_params[] = {
    {"center_x", offsetof(WPE_PostFxConfig, shockwave.center_x), -1, 2, false},
    {"center_y", offsetof(WPE_PostFxConfig, shockwave.center_y), -1, 2, false},
    {"radius", offsetof(WPE_PostFxConfig, shockwave.radius), 0, 4, false},
    {"width", offsetof(WPE_PostFxConfig, shockwave.width), 0.001, 2, false},
    {"strength_pixels", offsetof(WPE_PostFxConfig, shockwave.strength_pixels), -64, 64, false},
    {"length", offsetof(WPE_PostFxConfig, shockwave.length), 0, 4, false},
    {"angle", offsetof(WPE_PostFxConfig, shockwave.angle), -50, 50, false}};
static const WPE_PostFxParam wpe__radial_blur_params[] = {
    {"center_x", offsetof(WPE_PostFxConfig, radial_blur.center_x), -1, 2, false},
    {"center_y", offsetof(WPE_PostFxConfig, radial_blur.center_y), -1, 2, false},
    {"strength", offsetof(WPE_PostFxConfig, radial_blur.strength), 0, 1, false}};
static const WPE_PostFxParam wpe__black_hole_params[] = {
    {"center_x", offsetof(WPE_PostFxConfig, black_hole.center_x), -1, 2, false},
    {"center_y", offsetof(WPE_PostFxConfig, black_hole.center_y), -1, 2, false},
    {"radius", offsetof(WPE_PostFxConfig, black_hole.radius), 0, 4, false},
    {"reach", offsetof(WPE_PostFxConfig, black_hole.reach), 0.001, 4, false},
    {"pull_pixels", offsetof(WPE_PostFxConfig, black_hole.pull_pixels), -512, 512, false},
    {"swirl", offsetof(WPE_PostFxConfig, black_hole.swirl), -50, 50, false},
    {"darkness", offsetof(WPE_PostFxConfig, black_hole.darkness), 0, 1, false},
    {"ring_pixels", offsetof(WPE_PostFxConfig, black_hole.ring_pixels), 0, 64, false},
    {"ring_r", offsetof(WPE_PostFxConfig, black_hole.ring_r), 0, 4, false},
    {"ring_g", offsetof(WPE_PostFxConfig, black_hole.ring_g), 0, 4, false},
    {"ring_b", offsetof(WPE_PostFxConfig, black_hole.ring_b), 0, 4, false}};
static const WPE_PostFxParam wpe__acrylic_params[] = {
    {"strength", offsetof(WPE_PostFxConfig, acrylic.strength), 0, 1, false},
    {"radius_pixels", offsetof(WPE_PostFxConfig, acrylic.radius_pixels), 0, 32, false},
    {"tint_r", offsetof(WPE_PostFxConfig, acrylic.tint_r), 0, 1, false},
    {"tint_g", offsetof(WPE_PostFxConfig, acrylic.tint_g), 0, 1, false},
    {"tint_b", offsetof(WPE_PostFxConfig, acrylic.tint_b), 0, 1, false},
    {"tint", offsetof(WPE_PostFxConfig, acrylic.tint), 0, 1, false},
    {"saturation", offsetof(WPE_PostFxConfig, acrylic.saturation), 0, 3, false},
    {"noise", offsetof(WPE_PostFxConfig, acrylic.noise), 0, 0.25, false}};
static const WPE_PostFxParam wpe__underwater_params[] = {
    {"surface_y", offsetof(WPE_PostFxConfig, underwater.surface_y), -2, 2, false},
    {"left", offsetof(WPE_PostFxConfig, underwater.left), -2, 3, false},
    {"right", offsetof(WPE_PostFxConfig, underwater.right), -2, 3, false},
    {"bottom", offsetof(WPE_PostFxConfig, underwater.bottom), -2, 3, false},
    {"color_r", offsetof(WPE_PostFxConfig, underwater.color_r), 0, 1, false},
    {"color_g", offsetof(WPE_PostFxConfig, underwater.color_g), 0, 1, false},
    {"color_b", offsetof(WPE_PostFxConfig, underwater.color_b), 0, 1, false},
    {"tint", offsetof(WPE_PostFxConfig, underwater.tint), 0, 1, false},
    {"deep_tint", offsetof(WPE_PostFxConfig, underwater.deep_tint), 0, 1, false},
    {"deep", offsetof(WPE_PostFxConfig, underwater.deep), 0.01, 100, false},
    {"darken", offsetof(WPE_PostFxConfig, underwater.darken), 0, 1, false},
    {"murk", offsetof(WPE_PostFxConfig, underwater.murk), 0, 1, false},
    {"murk_start", offsetof(WPE_PostFxConfig, underwater.murk_start), 0, 1e6, false},
    {"murk_distance", offsetof(WPE_PostFxConfig, underwater.murk_distance), 0, 1e6, false},
    {"wobble_pixels", offsetof(WPE_PostFxConfig, underwater.wobble_pixels), 0, 32, false},
    {"wobble_rows", offsetof(WPE_PostFxConfig, underwater.wobble_rows), 1, 1024, false},
    {"wobble_speed", offsetof(WPE_PostFxConfig, underwater.wobble_speed), 0, 20, false},
    {"wave_pixels", offsetof(WPE_PostFxConfig, underwater.wave_pixels), 0, 32, false},
    {"wave_length", offsetof(WPE_PostFxConfig, underwater.wave_length), 1, 4096, false},
    {"glow", offsetof(WPE_PostFxConfig, underwater.glow), 0, 1, false}};
static const WPE_PostFxParam wpe__grayscale_params[] = {
    {"strength", offsetof(WPE_PostFxConfig, grayscale.strength), 0, 1, false},
    {"tint_r", offsetof(WPE_PostFxConfig, grayscale.tint_r), 0, 2, false},
    {"tint_g", offsetof(WPE_PostFxConfig, grayscale.tint_g), 0, 2, false},
    {"tint_b", offsetof(WPE_PostFxConfig, grayscale.tint_b), 0, 2, false}};
static const WPE_PostFxParam wpe__cracks_params[] = {
    {"count", offsetof(WPE_PostFxConfig, cracks.count), 0, WPE_POSTFX_MAX_CRACKS, false},
    {"seed", offsetof(WPE_PostFxConfig, cracks.seed), 0, 1e6, true},
    {"center_x", offsetof(WPE_PostFxConfig, cracks.center_x), -1, 2, false},
    {"center_y", offsetof(WPE_PostFxConfig, cracks.center_y), -1, 2, false},
    {"spread", offsetof(WPE_PostFxConfig, cracks.spread), 0, 2, false},
    {"offset_pixels", offsetof(WPE_PostFxConfig, cracks.offset_pixels), 0, 32, false},
    {"slide_pixels", offsetof(WPE_PostFxConfig, cracks.slide_pixels), 0, 32, false},
    {"width_pixels", offsetof(WPE_PostFxConfig, cracks.width_pixels), 0, 16, false},
    {"glow_pixels", offsetof(WPE_PostFxConfig, cracks.glow_pixels), 0, 32, false},
    {"bevel_pixels", offsetof(WPE_PostFxConfig, cracks.bevel_pixels), 0, 32, false},
    {"shear_waves", offsetof(WPE_PostFxConfig, cracks.shear_waves), 0, 100, false},
    {"strength", offsetof(WPE_PostFxConfig, cracks.strength), 0, 1, false},
    {"core_r", offsetof(WPE_PostFxConfig, cracks.core_r), 0, 4, false},
    {"core_g", offsetof(WPE_PostFxConfig, cracks.core_g), 0, 4, false},
    {"core_b", offsetof(WPE_PostFxConfig, cracks.core_b), 0, 4, false},
    {"glow_r", offsetof(WPE_PostFxConfig, cracks.glow_r), 0, 4, false},
    {"glow_g", offsetof(WPE_PostFxConfig, cracks.glow_g), 0, 4, false},
    {"glow_b", offsetof(WPE_PostFxConfig, cracks.glow_b), 0, 4, false}};
static const WPE_PostFxParam wpe__shatter_params[] = {
    {"count", offsetof(WPE_PostFxConfig, shatter.count), 0, WPE_POSTFX_MAX_SHATTER_LINES, false},
    {"seed", offsetof(WPE_PostFxConfig, shatter.seed), 0, 1e6, true},
    {"center_x", offsetof(WPE_PostFxConfig, shatter.center_x), -1, 2, false},
    {"center_y", offsetof(WPE_PostFxConfig, shatter.center_y), -1, 2, false},
    {"spread", offsetof(WPE_PostFxConfig, shatter.spread), 0, 2, false},
    {"time", offsetof(WPE_PostFxConfig, shatter.time), 0, 1e3, false},
    {"speed", offsetof(WPE_PostFxConfig, shatter.speed), 0, 10, false},
    {"gravity", offsetof(WPE_PostFxConfig, shatter.gravity), -10, 10, false},
    {"spin", offsetof(WPE_PostFxConfig, shatter.spin), 0, 30, false},
    {"background", offsetof(WPE_PostFxConfig, shatter.background), 0, 1, false},
    {"capture", offsetof(WPE_PostFxConfig, shatter.capture), 0, 8, true},
    {"fade", offsetof(WPE_PostFxConfig, shatter.fade), 0, 1e3, false}};

/* Scene files read at most WPE_POSTFX_MAX_PARAMS parameters per effect. */
#define WPE__FITS(t)                                                                                         \
    static_assert(sizeof(t) / sizeof((t)[0]) <= WPE_POSTFX_MAX_PARAMS, #t " has too many parameters")
WPE__FITS(wpe__ssao_params);
WPE__FITS(wpe__cracks_params);
WPE__FITS(wpe__shatter_params);
WPE__FITS(wpe__underwater_params);
#undef WPE__FITS

static const struct {
    const WPE_PostFxParam* params;
    uint32_t count;
} wpe__kind_params[WPE_POSTFX_KIND_COUNT] = {
#define WPE__TABLE(t) {t, (uint32_t)(sizeof(t) / sizeof((t)[0]))}
    WPE__TABLE(wpe__ssao_params),         WPE__TABLE(wpe__chromatic_params),
    WPE__TABLE(wpe__ps1_params),          WPE__TABLE(wpe__vignette_params),
    WPE__TABLE(wpe__color_adjust_params), WPE__TABLE(wpe__dof_params),
    WPE__TABLE(wpe__shockwave_params),    WPE__TABLE(wpe__radial_blur_params),
    WPE__TABLE(wpe__grayscale_params),    WPE__TABLE(wpe__cracks_params),
    WPE__TABLE(wpe__shatter_params),      WPE__TABLE(wpe__black_hole_params),
    WPE__TABLE(wpe__acrylic_params),      WPE__TABLE(wpe__underwater_params),
#undef WPE__TABLE
};

const char* wpe_postfx_kind_name(WPE_PostFxKind kind) {
    return (uint32_t)kind < WPE_POSTFX_KIND_COUNT ? wpe__kind_names[kind] : NULL;
}

bool wpe_postfx_kind_from_name(const char* name, WPE_PostFxKind* out_kind) {
    for (uint32_t i = 0; name && i < WPE_POSTFX_KIND_COUNT; ++i) {
        if (strcmp(wpe__kind_names[i], name) != 0) continue;
        if (out_kind) *out_kind = (WPE_PostFxKind)i;
        return true;
    }
    return false;
}

const WPE_PostFxParam* wpe_postfx_params(WPE_PostFxKind kind, uint32_t* out_count) {
    const bool valid = (uint32_t)kind < WPE_POSTFX_KIND_COUNT;
    if (out_count) *out_count = valid ? wpe__kind_params[kind].count : 0;
    return valid ? wpe__kind_params[kind].params : NULL;
}

double wpe_postfx_param_get(const WPE_PostFxConfig* config, const WPE_PostFxParam* param) {
    const unsigned char* at = (const unsigned char*)config + param->offset;
    if (param->integer) {
        uint32_t value;
        memcpy(&value, at, sizeof value);
        return value;
    }
    float value;
    memcpy(&value, at, sizeof value);
    return value;
}

void wpe_postfx_param_set(WPE_PostFxConfig* config, const WPE_PostFxParam* param, double value) {
    if (!(value == value)) value = param->min; /* NaN */
    value = value < param->min ? param->min : value > param->max ? param->max : value;
    unsigned char* at = (unsigned char*)config + param->offset;
    if (param->integer) {
        const uint32_t whole = (uint32_t)lround(value);
        memcpy(at, &whole, sizeof whole);
    } else {
        const float single = (float)value;
        memcpy(at, &single, sizeof single);
    }
}

WPE_PostFxConfig wpe_postfx_config_default(WPE_PostFxKind kind) {
    return (WPE_PostFxConfig){.kind = kind,
                              .enabled = true,
                              .ssao = wpe__ssao_defaults,
                              .chromatic_aberration = wpe__chromatic_defaults,
                              .ps1 = wpe__ps1_defaults,
                              .vignette = wpe__vignette_defaults,
                              .color_adjust = wpe__color_adjust_defaults,
                              .depth_of_field = wpe__dof_defaults,
                              .shockwave = wpe__shockwave_defaults,
                              .radial_blur = wpe__radial_blur_defaults,
                              .grayscale = wpe__grayscale_defaults,
                              .cracks = wpe__cracks_defaults,
                              .shatter = wpe__shatter_defaults,
                              .black_hole = wpe__black_hole_defaults,
                              .acrylic = wpe__acrylic_defaults,
                              .underwater = wpe__underwater_defaults};
}

WPE_PostEffect wpe_postfx_from_config(const WPE_PostFxConfig* c) {
    WPE_PostEffect effect = {0};
    switch (c->kind) {
    case WPE_POSTFX_SSAO:
        effect = wpe_postfx_ssao(&c->ssao);
        break;
    case WPE_POSTFX_CHROMATIC_ABERRATION:
        effect = wpe_postfx_chromatic_aberration(&c->chromatic_aberration);
        break;
    case WPE_POSTFX_PS1:
        effect = wpe_postfx_ps1(&c->ps1);
        break;
    case WPE_POSTFX_VIGNETTE:
        effect = wpe_postfx_vignette(&c->vignette);
        break;
    case WPE_POSTFX_COLOR_ADJUST:
        effect = wpe_postfx_color_adjust(&c->color_adjust);
        break;
    case WPE_POSTFX_DEPTH_OF_FIELD:
        effect = wpe_postfx_depth_of_field(&c->depth_of_field);
        break;
    case WPE_POSTFX_SHOCKWAVE:
        effect = wpe_postfx_shockwave(&c->shockwave);
        break;
    case WPE_POSTFX_RADIAL_BLUR:
        effect = wpe_postfx_radial_blur(&c->radial_blur);
        break;
    case WPE_POSTFX_GRAYSCALE:
        effect = wpe_postfx_grayscale(&c->grayscale);
        break;
    case WPE_POSTFX_CRACKS:
        effect = wpe_postfx_cracks(&c->cracks);
        break;
    case WPE_POSTFX_SHATTER:
        effect = wpe_postfx_shatter(&c->shatter);
        break;
    case WPE_POSTFX_BLACK_HOLE:
        effect = wpe_postfx_black_hole(&c->black_hole);
        break;
    case WPE_POSTFX_ACRYLIC:
        effect = wpe_postfx_acrylic(&c->acrylic);
        break;
    case WPE_POSTFX_UNDERWATER:
        effect = wpe_postfx_underwater(&c->underwater);
        break;
    }
    effect.enabled = c->enabled;
    return effect;
}
