#define WPE_LOG_MODULE "assets"
#include "assets/font.h"

#include <string.h>

#include "assets/image.h"
#include "core/alloc.h"
#include "core/debug.h"
#include "core/log.h"
#include "platform/platform.h"

#define WPE__FONT_SEPARATOR 0xFFFF00FFu /* Opaque magenta. */

static bool wpe__is_separator(uint32_t texel) {
    return texel == WPE__FONT_SEPARATOR;
}

static bool wpe__row_is_separator(const uint32_t* texels, uint32_t width, uint32_t y) {
    for (uint32_t x = 0; x < width; ++x) {
        if (!wpe__is_separator(texels[(size_t)y * width + x])) return false;
    }
    return true;
}

static bool wpe__column_is_separator(const uint32_t* texels, uint32_t width, uint32_t x, uint32_t y0,
                                     uint32_t y1) {
    for (uint32_t y = y0; y < y1; ++y) {
        if (!wpe__is_separator(texels[(size_t)y * width + x])) return false;
    }
    return true;
}

WPE_Result wpe_font_create(uint32_t width, uint32_t height, const uint32_t* texels,
                           WPE_BitmapFont** out_font) {
    if (!texels || !out_font) return WPE_RESULT_ERROR_NULL_ARG;
    *out_font = NULL;
    if (width == 0 || height == 0 || width > UINT16_MAX || height > UINT16_MAX)
        return WPE_RESULT_ERROR_INVALID_ARG;

    WPE_BitmapFont* font = wpe_alloc_zeroed(1, sizeof *font);
    if (!font) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    font->glyph_spacing = 1;

    /* Bands are runs of non-separator rows; cells are runs of non-separator columns within a band. */
    uint32_t glyph_count = 0, tallest = 0;
    uint32_t y = 0;
    while (y < height) {
        if (wpe__row_is_separator(texels, width, y)) {
            ++y;
            continue;
        }
        const uint32_t band_top = y;
        while (y < height && !wpe__row_is_separator(texels, width, y))
            ++y;
        const uint32_t band_bottom = y;

        uint32_t x = 0;
        while (x < width) {
            if (wpe__column_is_separator(texels, width, x, band_top, band_bottom)) {
                ++x;
                continue;
            }
            const uint32_t cell_left = x;
            while (x < width && !wpe__column_is_separator(texels, width, x, band_top, band_bottom))
                ++x;
            if (glyph_count < WPE_FONT_GLYPH_COUNT) {
                font->glyphs[glyph_count] = (WPE_FontGlyph){.x = (uint16_t)cell_left,
                                                            .y = (uint16_t)band_top,
                                                            .width = (uint16_t)(x - cell_left),
                                                            .height = (uint16_t)(band_bottom - band_top)};
            }
            ++glyph_count;
            if (band_bottom - band_top > tallest) tallest = band_bottom - band_top;
        }
    }

    if (glyph_count != WPE_FONT_GLYPH_COUNT) {
        wpe_log_error("font sheet contains %u glyph cells, expected %u (characters %u..%u)", glyph_count,
                      WPE_FONT_GLYPH_COUNT, WPE_FONT_FIRST_CHAR, WPE_FONT_LAST_CHAR);
        wpe_free(font);
        return WPE_RESULT_ERROR_PARSE;
    }
    font->line_height = tallest + 1;

    /* Atlas: separators become transparent so bilinear/nearest sampling never bleeds magenta. */
    uint32_t* atlas = wpe_alloc((size_t)width * height * sizeof *atlas);
    if (!atlas) {
        wpe_free(font);
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < (size_t)width * height; ++i) {
        atlas[i] = wpe__is_separator(texels[i]) ? 0u : texels[i];
    }
    const WPE_Result result = wpe_texture_create(
        &(WPE_TextureDesc){.width = width, .height = height, .texels = atlas, .generate_mips = false},
        &font->atlas);
    wpe_free(atlas);
    if (result != WPE_RESULT_OK) {
        wpe_free(font);
        return result;
    }
    *out_font = font;
    return WPE_RESULT_OK;
}

WPE_Result wpe_font_load(const char* path, WPE_BitmapFont** out_font) {
    if (!path || !out_font) return WPE_RESULT_ERROR_NULL_ARG;
    *out_font = NULL;

    void* data = NULL;
    size_t size = 0;
    WPE_Result result = wpe_file_read_all(path, &data, &size);
    if (result != WPE_RESULT_OK) return result;
    result = wpe_font_load_memory(data, size, path, out_font);
    wpe_free(data);
    return result;
}

WPE_Result wpe_font_load_memory(const void* data, size_t size, const char* label, WPE_BitmapFont** out_font) {
    if (!data || !label || !out_font) return WPE_RESULT_ERROR_NULL_ARG;
    *out_font = NULL;
    WPE_Image image = {0};
    WPE_Result result = wpe_image_decode(data, size, label, &image);
    if (result != WPE_RESULT_OK) return result;
    result = wpe_font_create(image.width, image.height, image.pixels, out_font);
    wpe_image_free(&image);
    if (result == WPE_RESULT_ERROR_PARSE) wpe_log_error("'%s' is not a valid font sheet", label);
    return result;
}

void wpe_font_destroy(WPE_BitmapFont* font) {
    if (!font) return;
    wpe_texture_destroy(font->atlas);
    wpe_free(font);
}

const WPE_FontGlyph* wpe_font_glyph(const WPE_BitmapFont* font, char c) {
    wpe_assert(font);
    const unsigned char u = (unsigned char)c;
    const unsigned char code = (u >= WPE_FONT_FIRST_CHAR && u <= WPE_FONT_LAST_CHAR) ? u : (unsigned char)'?';
    return &font->glyphs[code - WPE_FONT_FIRST_CHAR];
}

void wpe_font_measure(const WPE_BitmapFont* font, const char* text, uint32_t* out_width,
                      uint32_t* out_height) {
    wpe_assert(font && text && out_width && out_height);
    uint32_t widest = 0, line = 0, lines = 1;
    bool line_has_glyph = false;
    for (const char* p = text; *p; ++p) {
        if (*p == '\n') {
            widest = line > widest ? line : widest;
            line = 0;
            line_has_glyph = false;
            ++lines;
            continue;
        }
        if (line_has_glyph) line += font->glyph_spacing;
        line += wpe_font_glyph(font, *p)->width;
        line_has_glyph = true;
    }
    *out_width = line > widest ? line : widest;
    *out_height = lines * font->line_height - 1; /* No trailing leading after the last line. */
}
