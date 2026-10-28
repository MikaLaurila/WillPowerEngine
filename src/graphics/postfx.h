/**
 * @file postfx.h
 * @brief Full-screen post-processing chain applied to a finished renderer frame.
 *
 * An effect is a C function run over the framebuffer in one or more passes. Each pass reads `src`
 * (the result of the previous pass) and must write every pixel of its rows into `dst`; the chain
 * swaps them after every pass and copies the final result back into the renderer's framebuffer.
 * Effects may also use `aux`, one float per pixel that persists across the passes of the same
 * effect (e.g. SSAO stores raw occlusion in pass 0 and blurs it in pass 1). Rows are split across
 * the renderer's job system, so pass functions run concurrently on disjoint row ranges and must
 * only write rows [row_begin, row_end) of dst/aux.
 *
 * Built-in effects: SSAO, chromatic aberration, PS1-style dither + color quantization, vignette,
 * color adjustment (brightness/contrast/saturation) and depth of field.
 *
 * Usage: wpe_renderer_end_frame() → wpe_postfx_apply() → optionally another renderer frame
 * without clearing for HUD/2D that should not be post-processed.
 *
 * - Dependencies: renderer, jobs
 * - Thread affinity: apply on the renderer's owning thread; pass functions on worker threads
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_GRAPHICS_POSTFX_H
#define WPE_GRAPHICS_POSTFX_H

#include "renderer/renderer.h"

/** Camera parameters effects need to linearize depth. */
typedef struct WPE_PostCamera {
    float near_z;
    float far_z;
    bool orthographic;
} WPE_PostCamera;

typedef struct WPE_PostContext {
    const WPE_Color* src;
    WPE_Color* dst;
    float* aux;         /* width * height floats, shared by the passes of one effect. */
    const float* depth; /* Window depth in [0, 1] from the rendered frame. */
    uint32_t width;
    uint32_t height;
    WPE_PostCamera camera;
} WPE_PostContext;

/** Processes rows [row_begin, row_end) of one pass. Runs concurrently on disjoint row ranges. */
typedef void (*WPE_PostPassFn)(const WPE_PostContext* context, const void* params, uint32_t pass,
                               uint32_t row_begin, uint32_t row_end);

typedef struct WPE_PostEffect {
    const char* name;
    WPE_PostPassFn pass;
    uint32_t pass_count;
    const void* params; /* Borrowed for the duration of wpe_postfx_apply(). */
    bool enabled;
} WPE_PostEffect;

typedef struct WPE_PostChain WPE_PostChain;

/** @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or WPE_RESULT_ERROR_OUT_OF_MEMORY. @thread_safety Any
 * thread */
[[nodiscard]] WPE_Result wpe_postfx_create(WPE_PostChain** out_chain);

/** Destroys a chain; NULL is a no-op. @thread_safety Owning thread */
void wpe_postfx_destroy(WPE_PostChain* chain);

/**
 * Runs the enabled effects in order on the renderer's framebuffer (between frames).
 *
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_INVALID_ARG (an enabled
 *         effect without a pass function or with zero passes); WPE_RESULT_ERROR_INVALID_STATE (a
 *         frame is recording); WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Renderer's owning thread
 */
WPE_Result wpe_postfx_apply(WPE_PostChain* chain, WPE_Renderer* renderer, const WPE_PostEffect* effects,
                            uint32_t effect_count, const WPE_PostCamera* camera);

/* ---- Built-in effects ---------------------------------------------------------------------------- */

typedef struct WPE_SsaoParams {
    float radius_pixels; /* Sampling radius on screen. */
    float strength;      /* 0 = off, 1 = full darkening where fully occluded. */
    float depth_range;   /* Occluders farther in front than this (view units) are ignored (halo control). */
    float bias;          /* Minimum depth difference (view units) that counts as occlusion. */
} WPE_SsaoParams;

typedef struct WPE_ChromaticAberrationParams {
    float strength_pixels; /* Red/blue channel offset at the screen corners. */
} WPE_ChromaticAberrationParams;

typedef struct WPE_Ps1Params {
    uint32_t levels;       /* Color levels per channel after quantization (e.g. 32 = 15-bit color). */
    float dither_strength; /* Ordered (4x4 Bayer) dither amplitude in quantization steps (0..1). */
} WPE_Ps1Params;

typedef struct WPE_VignetteParams {
    float strength; /* Darkening at the corners (0..1). */
    float radius;   /* Normalized distance from the center where darkening starts (0..1.5). */
} WPE_VignetteParams;

typedef struct WPE_ColorAdjustParams {
    float brightness; /* Added (-1..1). */
    float contrast;   /* Multiplier around 0.5 (1 = unchanged). */
    float saturation; /* 0 = grayscale, 1 = unchanged. */
} WPE_ColorAdjustParams;

/* Depth of field: what's near `focus_distance` is sharp, and the image blurs with distance from there,
 * in front and behind. Distances are along the view (view units). */
typedef struct WPE_DepthOfFieldParams {
    float focus_distance; /* Sharpest distance. */
    float focus_range;    /* Within this of focus_distance (either side) stays sharp. */
    float falloff;        /* Beyond the sharp range, the blur grows to full over this distance. */
    float radius_pixels;  /* Full blur radius (0..16). */
} WPE_DepthOfFieldParams;

/** @return The built-in effect with its default parameters (`params` points at static defaults).
 * @thread_safety Any thread */
WPE_PostEffect wpe_postfx_ssao(const WPE_SsaoParams* params);
WPE_PostEffect wpe_postfx_chromatic_aberration(const WPE_ChromaticAberrationParams* params);
WPE_PostEffect wpe_postfx_ps1(const WPE_Ps1Params* params);
WPE_PostEffect wpe_postfx_vignette(const WPE_VignetteParams* params);
WPE_PostEffect wpe_postfx_color_adjust(const WPE_ColorAdjustParams* params);
WPE_PostEffect wpe_postfx_depth_of_field(const WPE_DepthOfFieldParams* params);

/** @return Linear view-space depth for a window depth value. @thread_safety Any thread */
float wpe_postfx_linear_depth(const WPE_PostCamera* camera, float window_depth);

#endif /* WPE_GRAPHICS_POSTFX_H */
