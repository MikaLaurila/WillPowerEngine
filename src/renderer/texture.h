/**
 * @file texture.h
 * @brief Immutable mipmapped textures and the sampling functions shaders call.
 *
 * Textures store WPE_Color (0xAARRGGBB) texels with an optional full mip chain. Color textures
 * are flagged sRGB so mip levels are averaged in linear light; data textures (normal maps,
 * metallic/roughness) are averaged as-is. Sampled values are returned unconverted in [0, 1]
 * (the engine shades in gamma space, matching the target retro look).
 *
 * Level of detail follows the GPU convention: lod = log2(max(|duv/dx|, |duv/dy|) * size) + bias,
 * using the screen-space UV derivatives the rasterizer provides (see WPE_ShaderProgram).
 *
 * - Dependencies: core, math
 * - Thread affinity: create/destroy on any one thread; sampling is safe concurrently (read-only)
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_RENDERER_TEXTURE_H
#define WPE_RENDERER_TEXTURE_H

#include <stdint.h>

#include "core/result.h"
#include "math/wpe_math.h"

/** Supports textures up to 2^(WPE_TEXTURE_MAX_LEVELS - 1) texels per side. */
#define WPE_TEXTURE_MAX_LEVELS 15u
#define WPE_TEXTURE_MAX_DIMENSION (1u << (WPE_TEXTURE_MAX_LEVELS - 1))

typedef struct WPE_TextureLevel {
    uint32_t width;
    uint32_t height;
    const uint32_t* texels; /* 0xAARRGGBB, rows tightly packed. */
} WPE_TextureLevel;

typedef struct WPE_Texture {
    uint32_t level_count;
    WPE_TextureLevel levels[WPE_TEXTURE_MAX_LEVELS];
    bool srgb;
} WPE_Texture;

typedef struct WPE_TextureDesc {
    uint32_t width;
    uint32_t height;
    const uint32_t* texels; /* width * height 0xAARRGGBB texels; copied. */
    bool generate_mips;
    bool srgb;              /* Average mips in linear light (color data). */
    uint32_t max_dimension; /* If non-zero, box-filter down by powers of two until both sides fit. */
} WPE_TextureDesc;

typedef enum WPE_TextureFilter {
    WPE_FILTER_NEAREST = 0,
    WPE_FILTER_BILINEAR,
} WPE_TextureFilter;

typedef enum WPE_MipFilter {
    WPE_MIP_NONE = 0, /* Always level 0. */
    WPE_MIP_NEAREST,
    WPE_MIP_LINEAR, /* Trilinear when combined with a bilinear min filter. */
} WPE_MipFilter;

typedef enum WPE_TextureWrap {
    WPE_WRAP_REPEAT = 0,
    WPE_WRAP_CLAMP,
    WPE_WRAP_MIRROR,
} WPE_TextureWrap;

typedef struct WPE_Sampler {
    WPE_TextureFilter mag_filter;
    WPE_TextureFilter min_filter;
    WPE_MipFilter mip_filter;
    WPE_TextureWrap wrap_u;
    WPE_TextureWrap wrap_v;
    float lod_bias;
} WPE_Sampler;

/**
 * Creates a texture, copying the texels and optionally building the mip chain (2x2 box filter).
 *
 * @param out_texture Receives the texture; release with wpe_texture_destroy().
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_INVALID_ARG (size 0 or above
 *         WPE_TEXTURE_MAX_DIMENSION) or WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Any thread
 */
[[nodiscard]] WPE_Result wpe_texture_create(const WPE_TextureDesc* desc, WPE_Texture** out_texture);

/**
 * Replaces the texels of a texture without mips (created with generate_mips = false and no
 * downscaling), e.g. to show a frame rendered elsewhere. Do not call while a frame that samples the
 * texture is being rendered.
 *
 * @param texels width * height texels, as at creation.
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or WPE_RESULT_ERROR_INVALID_STATE (the texture has mips).
 *
 * @thread_safety Owning thread, no concurrent sampling
 */
WPE_Result wpe_texture_update(WPE_Texture* texture, const uint32_t* texels);

/** Destroys a texture (ownership consumed); NULL is a no-op. @thread_safety Any thread, no concurrent
 * sampling */
void wpe_texture_destroy(WPE_Texture* texture);

/**
 * Samples with an explicit level of detail.
 *
 * @param uv Texture coordinates; non-finite values sample texel (0, 0).
 * @return RGBA in [0, 1].
 *
 * @thread_safety Any thread
 */
WPE_Vec4 wpe_texture_sample_lod(const WPE_Texture* texture, const WPE_Sampler* sampler, WPE_Vec2 uv,
                                float lod);

/**
 * Samples using screen-space UV derivatives to select the level of detail.
 *
 * @thread_safety Any thread
 */
WPE_Vec4 wpe_texture_sample(const WPE_Texture* texture, const WPE_Sampler* sampler, WPE_Vec2 uv,
                            WPE_Vec2 duv_dx, WPE_Vec2 duv_dy);

#endif /* WPE_RENDERER_TEXTURE_H */
