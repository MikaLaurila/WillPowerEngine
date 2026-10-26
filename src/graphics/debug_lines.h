/**
 * @file debug_lines.h
 * @brief Immediate-mode 3D line drawing for debug visualization (physics shapes, gizmos).
 *
 * Lines are world-space segments expanded into screen-aligned quads of a fixed pixel width, so
 * they look the same at any distance. They are optionally depth tested against the scene (with a
 * small bias so lines on surfaces stay visible) and drawn after all 3D geometry, before draw2d.
 *
 * Usage per frame: wpe_debug_lines_begin() → wpe_debug_lines_add()... → wpe_debug_lines_submit()
 * (between the renderer's begin_frame and end_frame). Geometry stays alive until the next begin.
 *
 * - Dependencies: core, math, renderer
 * - Thread affinity: owning thread
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_GRAPHICS_DEBUG_LINES_H
#define WPE_GRAPHICS_DEBUG_LINES_H

#include "math/wpe_math.h"
#include "renderer/renderer.h"

typedef struct WPE_DebugLines WPE_DebugLines;

/** @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or WPE_RESULT_ERROR_OUT_OF_MEMORY. @thread_safety Any
 * thread */
[[nodiscard]] WPE_Result wpe_debug_lines_create(WPE_DebugLines** out_lines);

/** Destroys the line batch; NULL is a no-op. @thread_safety Owning thread */
void wpe_debug_lines_destroy(WPE_DebugLines* lines);

/**
 * Starts a new batch.
 *
 * @param view_projection Camera projection * view (copied).
 * @param width, height Framebuffer size in pixels.
 * @param line_width Line width in pixels (values below 1 are raised to 1).
 *
 * @thread_safety Owning thread
 */
void wpe_debug_lines_begin(WPE_DebugLines* lines, const WPE_Mat4* view_projection, uint32_t width,
                           uint32_t height, float line_width);

/** Adds a world-space segment (clipped at the near plane). @thread_safety Owning thread */
void wpe_debug_lines_add(WPE_DebugLines* lines, WPE_Vec3 a, WPE_Vec3 b, WPE_Color color);

/** @return Number of segments recorded since begin (after near-plane rejection). @thread_safety Owning thread
 */
uint32_t wpe_debug_lines_count(const WPE_DebugLines* lines);

/**
 * Submits the batch to a recording renderer frame.
 *
 * @param depth_test Hide lines behind scene geometry when true; draw on top of everything when false.
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_OUT_OF_MEMORY (a line failed to
 *         record earlier in this batch) or a renderer submit error.
 *
 * @thread_safety Owning thread
 */
WPE_Result wpe_debug_lines_submit(WPE_DebugLines* lines, WPE_Renderer* renderer, bool depth_test);

#endif /* WPE_GRAPHICS_DEBUG_LINES_H */
