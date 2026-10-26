/**
 * @file draw2d.h
 * @brief Immediate-mode 2D drawing (rectangles, images, bitmap text) batched into renderer draws.
 *
 * Coordinates are in canvas pixels with the origin at the top-left; the canvas maps 1:1 onto the
 * framebuffer when they have the same size. Shapes with integer coordinates align exactly with
 * pixels, so bitmap text renders crisp. Everything is alpha-blended without depth testing, in
 * call order, after all 3D geometry (it uses the highest sort keys).
 *
 * Usage per frame: wpe_draw2d_begin() → draw calls → wpe_draw2d_submit() (between the renderer's
 * begin_frame and end_frame). Geometry stays alive until the next wpe_draw2d_begin(); do not record
 * more draw calls between wpe_draw2d_submit() and the renderer's end_frame (growth may move it).
 *
 * - Dependencies: renderer, assets (fonts)
 * - Thread affinity: owning thread
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_GRAPHICS_DRAW2D_H
#define WPE_GRAPHICS_DRAW2D_H

#include "assets/font.h"
#include "renderer/renderer.h"
#include "renderer/texture.h"

typedef struct WPE_Draw2D WPE_Draw2D;

typedef struct WPE_Rect {
    float x, y, width, height;
} WPE_Rect;

/** @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or WPE_RESULT_ERROR_OUT_OF_MEMORY. @thread_safety Any
 * thread */
[[nodiscard]] WPE_Result wpe_draw2d_create(WPE_Draw2D** out_draw2d);

/** Destroys the 2D context; NULL is a no-op. @thread_safety Owning thread */
void wpe_draw2d_destroy(WPE_Draw2D* draw2d);

/** Starts a new batch for a canvas of the given size (usually the framebuffer size). @thread_safety Owning
 * thread */
void wpe_draw2d_begin(WPE_Draw2D* draw2d, uint32_t canvas_width, uint32_t canvas_height);

/** Fills a rectangle. @thread_safety Owning thread */
void wpe_draw2d_rect(WPE_Draw2D* draw2d, WPE_Rect rect, WPE_Color color);

/**
 * Draws a texture region. `texture` is borrowed until the renderer frame ends.
 *
 * @param source Region in texels (top-left origin); sampled with nearest filtering.
 * @param tint Multiplied with the texels (0xFFFFFFFF for none).
 *
 * @thread_safety Owning thread
 */
void wpe_draw2d_image(WPE_Draw2D* draw2d, const WPE_Texture* texture, WPE_Rect source, WPE_Rect dest,
                      WPE_Color tint);

/**
 * Draws text with its top-left corner at (x, y). '\n' starts a new line. `font` is borrowed until
 * the renderer frame ends.
 *
 * @param scale Integer-ish magnification (1 = native size).
 *
 * @thread_safety Owning thread
 */
void wpe_draw2d_text(WPE_Draw2D* draw2d, const WPE_BitmapFont* font, float x, float y, const char* text,
                     WPE_Color color, float scale);

/**
 * Submits the batch to a recording renderer frame.
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_OUT_OF_MEMORY (a draw call
 *         failed to record earlier in this batch) or a renderer submit error.
 *
 * @thread_safety Owning thread
 */
WPE_Result wpe_draw2d_submit(WPE_Draw2D* draw2d, WPE_Renderer* renderer);

#endif /* WPE_GRAPHICS_DRAW2D_H */
