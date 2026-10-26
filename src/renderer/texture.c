#define WPE_LOG_MODULE "renderer"
#include "renderer/texture.h"

#include <math.h>
#include <string.h>

#include "core/alloc.h"
#include "core/debug.h"
#include "core/log.h"

/* ---- Creation ------------------------------------------------------------------------------------- */

static float wpe__srgb_to_linear(float c) {
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static uint32_t wpe__linear_to_srgb8(float c) {
    const float s = c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
    if (!(s > 0.0f)) return 0;
    if (s >= 1.0f) return 255;
    return (uint32_t)(s * 255.0f + 0.5f);
}

/*
 * Box-filters `src` into the next level. Odd source dimensions clamp the 2x2 footprint at the
 * edge, so every source texel contributes and nothing is read out of bounds.
 */
static void wpe__downsample(const WPE_TextureLevel* src, uint32_t* dst, uint32_t dst_width,
                            uint32_t dst_height, bool srgb, const float* to_linear) {
    for (uint32_t y = 0; y < dst_height; ++y) {
        const uint32_t y0 = y * 2 < src->height ? y * 2 : src->height - 1;
        const uint32_t y1 = y0 + 1 < src->height ? y0 + 1 : y0;
        for (uint32_t x = 0; x < dst_width; ++x) {
            const uint32_t x0 = x * 2 < src->width ? x * 2 : src->width - 1;
            const uint32_t x1 = x0 + 1 < src->width ? x0 + 1 : x0;
            const uint32_t texels[4] = {
                src->texels[(size_t)y0 * src->width + x0], src->texels[(size_t)y0 * src->width + x1],
                src->texels[(size_t)y1 * src->width + x0], src->texels[(size_t)y1 * src->width + x1]};

            uint32_t out = 0;
            for (int shift = 0; shift <= 24; shift += 8) {
                const bool color_channel = srgb && shift < 24; /* Alpha is always linear. */
                float sum = 0.0f;
                for (int i = 0; i < 4; ++i) {
                    const uint32_t v = (texels[i] >> shift) & 0xFFu;
                    sum += color_channel ? to_linear[v] : (float)v;
                }
                const uint32_t channel =
                    color_channel ? wpe__linear_to_srgb8(sum * 0.25f) : (uint32_t)(sum * 0.25f + 0.5f);
                out |= channel << shift;
            }
            dst[(size_t)y * dst_width + x] = out;
        }
    }
}

static void wpe__fill_to_linear(float to_linear[256]) {
    for (int i = 0; i < 256; ++i)
        to_linear[i] = wpe__srgb_to_linear((float)i / 255.0f);
}

/* Creates the texture from texels whose size already satisfies max_dimension. */
static WPE_Result wpe__texture_create_from(const WPE_TextureDesc* desc, uint32_t width, uint32_t height,
                                           const uint32_t* texels, WPE_Texture** out_texture);

WPE_Result wpe_texture_create(const WPE_TextureDesc* desc, WPE_Texture** out_texture) {
    if (!desc || !desc->texels || !out_texture) return WPE_RESULT_ERROR_NULL_ARG;
    *out_texture = NULL;
    if (desc->width == 0 || desc->height == 0 || desc->width > WPE_TEXTURE_MAX_DIMENSION ||
        desc->height > WPE_TEXTURE_MAX_DIMENSION) {
        wpe_log_error("invalid texture size %ux%u", desc->width, desc->height);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }

    uint32_t w = desc->width, h = desc->height;
    const uint32_t limit = desc->max_dimension;
    if (limit == 0 || (w <= limit && h <= limit))
        return wpe__texture_create_from(desc, w, h, desc->texels, out_texture);

    /* Downscale into a scratch buffer, halving until the texture fits the limit. */
    float to_linear[256];
    wpe__fill_to_linear(to_linear);
    WPE_TextureLevel current = {.width = w, .height = h, .texels = desc->texels};
    uint32_t* owned = NULL;
    while (current.width > limit || current.height > limit) {
        const uint32_t nw = current.width > 1 ? current.width / 2 : 1;
        const uint32_t nh = current.height > 1 ? current.height / 2 : 1;
        uint32_t* next = wpe_alloc((size_t)nw * nh * sizeof(uint32_t));
        if (!next) {
            wpe_free(owned);
            return WPE_RESULT_ERROR_OUT_OF_MEMORY;
        }
        wpe__downsample(&current, next, nw, nh, desc->srgb, to_linear);
        wpe_free(owned);
        owned = next;
        current = (WPE_TextureLevel){.width = nw, .height = nh, .texels = next};
    }
    const WPE_Result result =
        wpe__texture_create_from(desc, current.width, current.height, current.texels, out_texture);
    wpe_free(owned);
    return result;
}

static WPE_Result wpe__texture_create_from(const WPE_TextureDesc* desc, uint32_t width, uint32_t height,
                                           const uint32_t* source, WPE_Texture** out_texture) {
    /* Level layout; all levels share one allocation placed after the texture header. */
    WPE_Texture header = {.srgb = desc->srgb};
    size_t total_texels = 0;
    uint32_t w = width, h = height;
    for (;;) {
        header.levels[header.level_count++] = (WPE_TextureLevel){.width = w, .height = h};
        total_texels += (size_t)w * h;
        if (!desc->generate_mips || (w == 1 && h == 1)) break;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }

    WPE_Texture* texture = wpe_alloc(sizeof *texture + total_texels * sizeof(uint32_t));
    if (!texture) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    *texture = header;
    uint32_t* storage = (uint32_t*)(texture + 1);

    float to_linear[256];
    if (desc->srgb && texture->level_count > 1) wpe__fill_to_linear(to_linear);

    size_t offset = 0;
    for (uint32_t level = 0; level < texture->level_count; ++level) {
        WPE_TextureLevel* lvl = &texture->levels[level];
        uint32_t* texels = storage + offset;
        if (level == 0) {
            memcpy(texels, source, (size_t)lvl->width * lvl->height * sizeof(uint32_t));
        } else {
            wpe__downsample(&texture->levels[level - 1], texels, lvl->width, lvl->height, desc->srgb,
                            to_linear);
        }
        lvl->texels = texels;
        offset += (size_t)lvl->width * lvl->height;
    }

    *out_texture = texture;
    return WPE_RESULT_OK;
}

void wpe_texture_destroy(WPE_Texture* texture) {
    wpe_free(texture);
}

/* ---- Sampling ------------------------------------------------------------------------------------- */

static int32_t wpe__wrap(int32_t i, int32_t size, WPE_TextureWrap mode) {
    switch (mode) {
    case WPE_WRAP_CLAMP:
        return i < 0 ? 0 : (i >= size ? size - 1 : i);
    case WPE_WRAP_MIRROR: {
        const int32_t period = size * 2;
        int32_t m = i % period;
        if (m < 0) m += period;
        return m < size ? m : period - 1 - m;
    }
    case WPE_WRAP_REPEAT:
    default: {
        const int32_t m = i % size;
        return m < 0 ? m + size : m;
    }
    }
}

static WPE_Vec4 wpe__texel(const WPE_TextureLevel* level, int32_t x, int32_t y) {
    const uint32_t c = level->texels[(size_t)y * level->width + (size_t)x];
    const float s = 1.0f / 255.0f;
    return wpe_vec4((float)((c >> 16) & 0xFF) * s, (float)((c >> 8) & 0xFF) * s, (float)(c & 0xFF) * s,
                    (float)(c >> 24) * s);
}

/*
 * Reduces a texture coordinate into a bounded range before conversion to texel space, so the
 * float -> int conversion can never overflow (|uv| may be huge on far-away geometry).
 */
static float wpe__wrap_coord(float uv, WPE_TextureWrap mode) {
    if (!isfinite(uv)) return 0.0f;
    if (mode == WPE_WRAP_CLAMP) return uv < -1.0f ? -1.0f : (uv > 2.0f ? 2.0f : uv);
    /* Repeat has period 1 and mirror period 2; reducing modulo 2 preserves both exactly. */
    return uv - 2.0f * floorf(uv * 0.5f);
}

static WPE_Vec4 wpe__sample_level(const WPE_TextureLevel* level, const WPE_Sampler* sampler,
                                  WPE_TextureFilter filter, WPE_Vec2 uv) {
    const int32_t w = (int32_t)level->width;
    const int32_t h = (int32_t)level->height;
    const float u = wpe__wrap_coord(uv.x, sampler->wrap_u) * (float)w;
    const float v = wpe__wrap_coord(uv.y, sampler->wrap_v) * (float)h;

    if (filter == WPE_FILTER_NEAREST) {
        const int32_t x = wpe__wrap((int32_t)floorf(u), w, sampler->wrap_u);
        const int32_t y = wpe__wrap((int32_t)floorf(v), h, sampler->wrap_v);
        return wpe__texel(level, x, y);
    }

    const float fu = u - 0.5f, fv = v - 0.5f;
    const float bx = floorf(fu), by = floorf(fv);
    const float tx = fu - bx, ty = fv - by;
    const int32_t x0 = wpe__wrap((int32_t)bx, w, sampler->wrap_u),
                  x1 = wpe__wrap((int32_t)bx + 1, w, sampler->wrap_u);
    const int32_t y0 = wpe__wrap((int32_t)by, h, sampler->wrap_v),
                  y1 = wpe__wrap((int32_t)by + 1, h, sampler->wrap_v);
    const WPE_Vec4 a = wpe__texel(level, x0, y0), b = wpe__texel(level, x1, y0);
    const WPE_Vec4 c = wpe__texel(level, x0, y1), d = wpe__texel(level, x1, y1);
    WPE_Vec4 out;
    for (int i = 0; i < 4; ++i) {
        const float top = a.v[i] + (b.v[i] - a.v[i]) * tx;
        const float bottom = c.v[i] + (d.v[i] - c.v[i]) * tx;
        out.v[i] = top + (bottom - top) * ty;
    }
    return out;
}

WPE_Vec4 wpe_texture_sample_lod(const WPE_Texture* texture, const WPE_Sampler* sampler, WPE_Vec2 uv,
                                float lod) {
    wpe_assert(texture && sampler);
    lod += sampler->lod_bias;
    if (!(lod > 0.0f) || texture->level_count == 1 || sampler->mip_filter == WPE_MIP_NONE) {
        const WPE_TextureFilter filter = lod > 0.0f ? sampler->min_filter : sampler->mag_filter;
        return wpe__sample_level(&texture->levels[0], sampler, filter, uv);
    }

    const float max_level = (float)(texture->level_count - 1);
    if (lod > max_level) lod = max_level;
    if (sampler->mip_filter == WPE_MIP_NEAREST) {
        const uint32_t level = (uint32_t)(lod + 0.5f);
        return wpe__sample_level(&texture->levels[level], sampler, sampler->min_filter, uv);
    }

    const uint32_t level0 = (uint32_t)lod;
    const uint32_t level1 = level0 + 1 < texture->level_count ? level0 + 1 : level0;
    const float t = lod - (float)level0;
    const WPE_Vec4 a = wpe__sample_level(&texture->levels[level0], sampler, sampler->min_filter, uv);
    if (level1 == level0 || t == 0.0f) return a;
    const WPE_Vec4 b = wpe__sample_level(&texture->levels[level1], sampler, sampler->min_filter, uv);
    return wpe_vec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
                    a.w + (b.w - a.w) * t);
}

WPE_Vec4 wpe_texture_sample(const WPE_Texture* texture, const WPE_Sampler* sampler, WPE_Vec2 uv,
                            WPE_Vec2 duv_dx, WPE_Vec2 duv_dy) {
    wpe_assert(texture && sampler);
    const float w = (float)texture->levels[0].width;
    const float h = (float)texture->levels[0].height;
    const float dx = (duv_dx.x * w) * (duv_dx.x * w) + (duv_dx.y * h) * (duv_dx.y * h);
    const float dy = (duv_dy.x * w) * (duv_dy.x * w) + (duv_dy.y * h) * (duv_dy.y * h);
    const float rho_sq = dx > dy ? dx : dy;
    /* log2(sqrt(x)) = 0.5 * log2(x); non-finite derivatives fall back to the base level. */
    const float lod = rho_sq > 0.0f && isfinite(rho_sq) ? 0.5f * log2f(rho_sq) : 0.0f;
    return wpe_texture_sample_lod(texture, sampler, uv, lod);
}
