/**
 * @file postfx.h
 * @brief Full-screen post-processing chain applied to a finished renderer frame.
 *
 * An effect is a C function run over the framebuffer in one or more passes. Each pass reads `src`
 * (the result of the previous pass) and must write every pixel of its rows into `dst`; the chain
 * swaps them after every pass and copies the final result back into the renderer's framebuffer.
 * Effects may also use `aux`, one float per pixel that persists across the passes of the same
 * effect (e.g. SSAO stores raw occlusion in pass 0 and blurs it in pass 1). An effect that needs work
 * done once for the whole image first (shatter finds its shards) has a `prepare` function, run alone
 * before its passes, which fills `scratch_size` bytes the passes then read. Rows are split across
 * the renderer's job system, so pass functions run concurrently on disjoint row ranges and must
 * only write rows [row_begin, row_end) of dst/aux.
 *
 * Built-in effects: SSAO, chromatic aberration, PS1-style dither + color quantization, vignette,
 * color adjustment (brightness/contrast/saturation), grayscale (with a tint, e.g. sepia), depth of field,
 * the shockwave and radial blur distortions, cracks (straight cuts across the screen) and shatter (the
 * screen breaking along them). WPE_PostFxConfig
 * describes any of them as data (scene files, scripts).
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

#include <stddef.h>

#include "renderer/renderer.h"
#include "renderer/texture.h"

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
    const void* scratch; /* What the effect's prepare function wrote (NULL without one). */
} WPE_PostContext;

/** Processes rows [row_begin, row_end) of one pass. Runs concurrently on disjoint row ranges. */
typedef void (*WPE_PostPassFn)(const WPE_PostContext* context, const void* params, uint32_t pass,
                               uint32_t row_begin, uint32_t row_end);

/** Runs once, alone, before an effect's passes; fills scratch (the effect's scratch_size bytes). */
typedef void (*WPE_PostPrepareFn)(const WPE_PostContext* context, const void* params, void* scratch);

typedef struct WPE_PostEffect {
    const char* name;
    WPE_PostPassFn pass;
    uint32_t pass_count;
    WPE_PostPrepareFn prepare; /* Optional. */
    size_t scratch_size;       /* Bytes prepare fills (the chain keeps them). */
    const void* params;        /* Borrowed for the duration of wpe_postfx_apply(). */
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

/* Grayscale (flashbacks, memories): the image's brightness alone, times a tint ({1, 1, 1}: gray; warmer for
 * sepia), mixed in by strength, so a script can fade into it. */
typedef struct WPE_GrayscaleParams {
    float strength;               /* 0 = unchanged, 1 = fully gray. */
    float tint_r, tint_g, tint_b; /* Multiplies the gray (0..2). */
} WPE_GrayscaleParams;

/* Cracks ("Judgement Cut"): straight cuts across the screen, like broken glass. Each line crosses near a
 * center point at a random angle. Only pixels near a cut change: at the cut the image is pulled across it
 * (a seam), near it the image is sheared along it by an amount that wanders along the cut (the streaks of
 * fractured glass), and on one side a rim tinted by the glow color darkens bright areas and lights dark ones.
 * An optional core blends the cut itself toward the core color. `count`
 * lines show (up to WPE_POSTFX_MAX_CRACKS); a fraction grows the newest one out from where it crosses, so
 * raising count over time draws the cuts in one by one. The same seed, center and spread always give the
 * same lines (wpe_postfx_crack_lines), so other effects can follow them. */
#define WPE_POSTFX_MAX_CRACKS 32u
typedef struct WPE_CracksParams {
    float count;              /* Lines shown (0..32), the last one partly when fractional. */
    uint32_t seed;            /* Which lines. */
    float center_x, center_y; /* Where the cuts cross (fractions of the screen). */
    float spread;             /* How far from it each line may pass (a fraction of the screen's height). */
    float offset_pixels;      /* How far the image is pulled across a cut (each: half to 1.5 times this). */
    float width_pixels;       /* The core's half width (0: no core line). */
    float glow_pixels;        /* The rim's width, on one side of each cut (0: no rim). */
    float bevel_pixels;       /* How far from a cut the image is sheared along it (0: no streaks). */
    float strength;           /* 0..1: everything above fades with it. */
    float core_r, core_g, core_b; /* The color the core blends toward. */
    float glow_r, glow_g, glow_b; /* The rim's tint: added over dark areas, darkened toward over bright. */
} WPE_CracksParams;

/* One crack line in pixels: a point it passes through, its unit direction, and how far it reaches from the
 * point each way (the whole screen, or less while it grows). */
typedef struct WPE_CrackLine {
    float x, y;
    float dx, dy;
    float reach;
} WPE_CrackLine;

/**
 * The lines a cracks effect draws on a width x height screen, as it draws them.
 *
 * @param out At least WPE_POSTFX_MAX_CRACKS lines.
 * @return How many lines show (those partly grown included).
 *
 * @thread_safety Any thread
 */
uint32_t wpe_postfx_crack_lines(const WPE_CracksParams* params, uint32_t width, uint32_t height,
                                WPE_CrackLine* out);
/* Shatter: the screen breaks along the cracks' lines (the same count, seed, center and spread give the same
 * lines) into the pieces they enclose, and as `time` runs each piece flies outward, turns and falls. The
 * pieces show a frame the host captured (`capture` names it; the engine fills in `image`), or the live
 * frame; behind them the live scene shows, darkened by `background`. At most
 * WPE_POSTFX_MAX_SHATTER_LINES lines break the screen. */
#define WPE_POSTFX_MAX_SHATTER_LINES 16u
typedef struct WPE_ShatterParams {
    float count;
    uint32_t seed;
    float center_x, center_y;
    float spread;
    float time;               /* Seconds since the break. */
    float speed;              /* How fast pieces fly outward (screen heights per second). */
    float gravity;            /* How fast they fall (screen heights per second, per second). */
    float spin;               /* The fastest a piece turns (radians per second). */
    float background;         /* 0: the live scene shows behind the pieces; 1: black. */
    uint32_t capture;         /* Which of the host's captured frames the pieces show (0: the live frame). */
    const WPE_Texture* image; /* Not a parameter: the captured frame, set by the host (NULL: live). */
} WPE_ShatterParams;
/* Depth of field: what's near `focus_distance` is sharp, and the image blurs with distance from there,
 * in front and behind. Distances are along the view (view units). */
typedef struct WPE_DepthOfFieldParams {
    float focus_distance; /* Sharpest distance. */
    float focus_range;    /* Within this of focus_distance (either side) stays sharp. */
    float falloff;        /* Beyond the sharp range, the blur grows to full over this distance. */
    float radius_pixels;  /* Full blur radius (0..16). */
} WPE_DepthOfFieldParams;

/* A ring that warps the image outward, centered on a screen point; grow `radius` over a moment for a
 * shockwave (an impact, a slash). Positions are fractions of the screen ((0, 0) top-left); distances
 * fractions of its height. */
typedef struct WPE_ShockwaveParams {
    float center_x, center_y;
    float radius;          /* The ring's distance from the center. */
    float width;           /* How thick the ring is. */
    float strength_pixels; /* How far the ring pushes the image (negative pulls it in). */
} WPE_ShockwaveParams;

/* Smears the image toward a screen point (a dash, a heavy hit), more toward the edges. */
typedef struct WPE_RadialBlurParams {
    float center_x, center_y; /* Fractions of the screen. */
    float strength;           /* 0..1: how far toward the center each pixel smears. */
} WPE_RadialBlurParams;

/* ---- Configured effects (scene files, scripts) ---------------------------------------------------- */

#define WPE_POSTFX_MAX_EFFECTS 8u /* Effects a scene can have. */

typedef enum WPE_PostFxKind {
    WPE_POSTFX_SSAO = 0,
    WPE_POSTFX_CHROMATIC_ABERRATION,
    WPE_POSTFX_PS1,
    WPE_POSTFX_VIGNETTE,
    WPE_POSTFX_COLOR_ADJUST,
    WPE_POSTFX_DEPTH_OF_FIELD,
    WPE_POSTFX_SHOCKWAVE,
    WPE_POSTFX_RADIAL_BLUR,
    WPE_POSTFX_GRAYSCALE,
    WPE_POSTFX_CRACKS,
    WPE_POSTFX_SHATTER,
} WPE_PostFxKind;
#define WPE_POSTFX_KIND_COUNT 11u

/** One configured built-in effect; only the parameters matching `kind` are used. */
typedef struct WPE_PostFxConfig {
    WPE_PostFxKind kind;
    bool enabled;
    WPE_SsaoParams ssao;
    WPE_ChromaticAberrationParams chromatic_aberration;
    WPE_Ps1Params ps1;
    WPE_VignetteParams vignette;
    WPE_ColorAdjustParams color_adjust;
    WPE_DepthOfFieldParams depth_of_field;
    WPE_ShockwaveParams shockwave;
    WPE_RadialBlurParams radial_blur;
    WPE_GrayscaleParams grayscale;
    WPE_CracksParams cracks;
    WPE_ShatterParams shatter;
} WPE_PostFxConfig;

/** A parameter of an effect kind: its name (in scene files and scripts), where it lives in a
 * WPE_PostFxConfig (a float, or a uint32_t when `integer`) and its allowed range. */
typedef struct WPE_PostFxParam {
    const char* name;
    size_t offset;
    double min, max;
    bool integer;
} WPE_PostFxParam;

/** @return The kind's name ("ssao", "depth_of_field", ...), or NULL. @thread_safety Any thread */
const char* wpe_postfx_kind_name(WPE_PostFxKind kind);

/** @return Whether `name` is a kind's name (then *out_kind receives it). @thread_safety Any thread */
bool wpe_postfx_kind_from_name(const char* name, WPE_PostFxKind* out_kind);

/** @return The kind's parameters in their file order (static storage); *out_count receives how many.
 * @thread_safety Any thread */
const WPE_PostFxParam* wpe_postfx_params(WPE_PostFxKind kind, uint32_t* out_count);

/** @return A parameter's value in a config. @thread_safety Any thread */
double wpe_postfx_param_get(const WPE_PostFxConfig* config, const WPE_PostFxParam* param);

/** Sets a parameter (clamped to its range; rounded for integers). @thread_safety Any thread */
void wpe_postfx_param_set(WPE_PostFxConfig* config, const WPE_PostFxParam* param, double value);

/** @return An enabled config of `kind` with its default parameters. @thread_safety Any thread */
WPE_PostFxConfig wpe_postfx_config_default(WPE_PostFxKind kind);

/** @return The effect a config describes (`params` points into `config`, which must outlive its use).
 * @thread_safety Any thread */
WPE_PostEffect wpe_postfx_from_config(const WPE_PostFxConfig* config);

/** @return The built-in effect with its default parameters (`params` points at static defaults).
 * @thread_safety Any thread */
WPE_PostEffect wpe_postfx_ssao(const WPE_SsaoParams* params);
WPE_PostEffect wpe_postfx_chromatic_aberration(const WPE_ChromaticAberrationParams* params);
WPE_PostEffect wpe_postfx_ps1(const WPE_Ps1Params* params);
WPE_PostEffect wpe_postfx_vignette(const WPE_VignetteParams* params);
WPE_PostEffect wpe_postfx_color_adjust(const WPE_ColorAdjustParams* params);
WPE_PostEffect wpe_postfx_depth_of_field(const WPE_DepthOfFieldParams* params);
WPE_PostEffect wpe_postfx_shockwave(const WPE_ShockwaveParams* params);
WPE_PostEffect wpe_postfx_radial_blur(const WPE_RadialBlurParams* params);
WPE_PostEffect wpe_postfx_grayscale(const WPE_GrayscaleParams* params);
WPE_PostEffect wpe_postfx_cracks(const WPE_CracksParams* params);
WPE_PostEffect wpe_postfx_shatter(const WPE_ShatterParams* params);

/** @return Linear view-space depth for a window depth value. @thread_safety Any thread */
float wpe_postfx_linear_depth(const WPE_PostCamera* camera, float window_depth);

#endif /* WPE_GRAPHICS_POSTFX_H */
