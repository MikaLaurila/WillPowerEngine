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
 * outward; the push peaks mid-ring and fades smoothly to its edges. */
static void wpe__shockwave_pass(const WPE_PostContext* c, const void* params_ptr, uint32_t pass,
                                uint32_t row_begin, uint32_t row_end) {
    (void)pass;
    const WPE_ShockwaveParams* p = params_ptr;
    const float h = (float)c->height;
    const float cx = p->center_x * (float)c->width, cy = p->center_y * h;
    const float width = p->width > 1e-4f ? p->width : 1e-4f;
    for (uint32_t y = row_begin; y < row_end; ++y) {
        for (uint32_t x = 0; x < c->width; ++x) {
            const size_t i = (size_t)y * c->width + x;
            const float dx = ((float)x + 0.5f - cx) / h, dy = ((float)y + 0.5f - cy) / h;
            const float d = sqrtf(dx * dx + dy * dy);
            const float ring = (d - p->radius) / width;
            if (!(fabsf(ring) < 1.0f) || !(d > 1e-6f)) {
                c->dst[i] = c->src[i];
                continue;
            }
            const float bump = (1.0f - ring * ring) * (1.0f - ring * ring);
            const float push = p->strength_pixels * bump / d / h; /* Along the unit direction, in heights. */
            const int32_t sx = (int32_t)floorf(cx + (dx - dx * push) * h);
            const int32_t sy = (int32_t)floorf(cy + (dy - dy * push) * h);
            c->dst[i] = wpe__texel_clamped(c, sx, sy);
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

/* ---- Configured effects ----------------------------------------------------------------------------- */

static const char* const wpe__kind_names[WPE_POSTFX_KIND_COUNT] = {
    "ssao",         "chromatic_aberration", "ps1",       "vignette",
    "color_adjust", "depth_of_field",       "shockwave", "radial_blur"};

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
    {"strength_pixels", offsetof(WPE_PostFxConfig, shockwave.strength_pixels), -64, 64, false}};
static const WPE_PostFxParam wpe__radial_blur_params[] = {
    {"center_x", offsetof(WPE_PostFxConfig, radial_blur.center_x), -1, 2, false},
    {"center_y", offsetof(WPE_PostFxConfig, radial_blur.center_y), -1, 2, false},
    {"strength", offsetof(WPE_PostFxConfig, radial_blur.strength), 0, 1, false}};

static const struct {
    const WPE_PostFxParam* params;
    uint32_t count;
} wpe__kind_params[WPE_POSTFX_KIND_COUNT] = {
#define WPE__TABLE(t) {t, (uint32_t)(sizeof(t) / sizeof((t)[0]))}
    WPE__TABLE(wpe__ssao_params),      WPE__TABLE(wpe__chromatic_params),    WPE__TABLE(wpe__ps1_params),
    WPE__TABLE(wpe__vignette_params),  WPE__TABLE(wpe__color_adjust_params), WPE__TABLE(wpe__dof_params),
    WPE__TABLE(wpe__shockwave_params), WPE__TABLE(wpe__radial_blur_params),
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
                              .radial_blur = wpe__radial_blur_defaults};
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
    }
    effect.enabled = c->enabled;
    return effect;
}
