#define WPE_LOG_MODULE "graphics"
#include "graphics/postfx.h"

#include <math.h>
#include <string.h>

#include "core/alloc.h"
#include "core/debug.h"
#include "core/log.h"

struct WPE_PostChain {
    WPE_Color* ping;
    float* aux;
    size_t capacity; /* Pixels. */
};

/* Rows per parallel chunk; fixed so output is independent of the worker count. */
#define WPE__POSTFX_ROW_GRAIN 8u

WPE_Result wpe_postfx_create(WPE_PostChain** out_chain) {
    if (!out_chain) return WPE_RESULT_ERROR_NULL_ARG;
    *out_chain = wpe_alloc_zeroed(1, sizeof **out_chain);
    return *out_chain ? WPE_RESULT_OK : WPE_RESULT_ERROR_OUT_OF_MEMORY;
}

void wpe_postfx_destroy(WPE_PostChain* chain) {
    if (!chain) return;
    wpe_free(chain->ping);
    wpe_free(chain->aux);
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

    WPE_JobSystem* jobs = wpe_renderer_jobs(renderer);
    WPE_Color* src = fb->color;
    WPE_Color* dst = chain->ping;
    for (uint32_t e = 0; e < effect_count; ++e) {
        if (!effects[e].enabled) continue;
        for (uint32_t pass = 0; pass < effects[e].pass_count; ++pass) {
            const WPE_PostContext context = {.src = src,
                                             .dst = dst,
                                             .aux = chain->aux,
                                             .depth = fb->depth,
                                             .width = fb->width,
                                             .height = fb->height,
                                             .camera = *camera};
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

/* ---- Constructors ---------------------------------------------------------------------------------- */

static const WPE_SsaoParams wpe__ssao_defaults = {
    .radius_pixels = 6.0f, .strength = 0.8f, .depth_range = 1.0f, .bias = 0.02f};
static const WPE_ChromaticAberrationParams wpe__chromatic_defaults = {.strength_pixels = 2.0f};
static const WPE_Ps1Params wpe__ps1_defaults = {.levels = 32, .dither_strength = 1.0f};
static const WPE_VignetteParams wpe__vignette_defaults = {.strength = 0.5f, .radius = 0.5f};
static const WPE_ColorAdjustParams wpe__color_adjust_defaults = {
    .brightness = 0.0f, .contrast = 1.0f, .saturation = 1.0f};

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
