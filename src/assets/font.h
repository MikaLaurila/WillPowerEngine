/**
 * @file font.h
 * @brief Bitmap fonts loaded from a glyph sheet image.
 *
 * Sheet format: glyphs for characters 32..126 in ASCII order, laid out left to right and top to
 * bottom in cells separated by opaque magenta (#FF00FF) rows and columns. Each cell's size is the
 * glyph's size (variable width). Non-magenta pixels are kept as-is (ink is normally white so text
 * can be tinted); magenta separators become fully transparent in the atlas.
 *
 * - Dependencies: core, renderer (texture), platform (file I/O), assets/image
 * - Thread affinity: load/destroy on any one thread; a loaded font is read-only
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_ASSETS_FONT_H
#define WPE_ASSETS_FONT_H

#include <stddef.h>
#include <stdint.h>

#include "core/result.h"
#include "renderer/texture.h"

#define WPE_FONT_FIRST_CHAR 32u
#define WPE_FONT_LAST_CHAR 126u
#define WPE_FONT_GLYPH_COUNT (WPE_FONT_LAST_CHAR - WPE_FONT_FIRST_CHAR + 1u)

typedef struct WPE_FontGlyph {
    uint16_t x, y;          /* Top-left texel in the atlas. */
    uint16_t width, height; /* In texels. */
} WPE_FontGlyph;

typedef struct WPE_BitmapFont {
    WPE_Texture* atlas; /* Owned. No mips; sample with nearest filtering. */
    WPE_FontGlyph glyphs[WPE_FONT_GLYPH_COUNT];
    uint32_t line_height;   /* Tallest glyph + 1 texel of leading. */
    uint32_t glyph_spacing; /* Texels between adjacent glyphs. */
} WPE_BitmapFont;

/**
 * Loads a font from a PNG/JPEG glyph sheet.
 *
 * @param out_font Receives the font; release with wpe_font_destroy().
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_IO, WPE_RESULT_ERROR_PARSE (the
 *         sheet does not contain exactly WPE_FONT_GLYPH_COUNT cells; logged),
 *         WPE_RESULT_ERROR_UNSUPPORTED or WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Any thread
 */
[[nodiscard]] WPE_Result wpe_font_load(const char* path, WPE_BitmapFont** out_font);

/**
 * Builds a font from decoded sheet texels (same format as wpe_font_load()).
 *
 * @thread_safety Any thread
 */
[[nodiscard]] WPE_Result wpe_font_create(uint32_t width, uint32_t height, const uint32_t* texels,
                                         WPE_BitmapFont** out_font);

/** Frees a font (ownership consumed); NULL is a no-op. @thread_safety Any thread */
void wpe_font_destroy(WPE_BitmapFont* font);

/**
 * @return The glyph for `c`, falling back to '?' for characters outside 32..126 (newlines are
 *         handled by the text layout, not here).
 *
 * @thread_safety Any thread
 */
const WPE_FontGlyph* wpe_font_glyph(const WPE_BitmapFont* font, char c);

/**
 * Measures a UTF-8/ASCII string at scale 1 (bytes outside 32..126 render as '?'; '\n' starts a new
 * line).
 *
 * @param out_width Receives the widest line in texels. @param out_height Receives the total height.
 *
 * @thread_safety Any thread
 */
void wpe_font_measure(const WPE_BitmapFont* font, const char* text, uint32_t* out_width,
                      uint32_t* out_height);

#endif /* WPE_ASSETS_FONT_H */
