/**
 * @file ui_backend.h
 * @brief Dear ImGui (through cimgui) for the editor, drawn one of two ways:
 *        - **GPU**: into a window through its SDL renderer (Direct3D, Metal, Vulkan or OpenGL, as SDL
 *          picks), the way Dear ImGui's own SDL_Renderer backend does. The window uses this.
 *        - **Software**: into a WPE_Renderer framebuffer by the engine's rasterizer, which needs no
 *          window or GPU. Headless runs use this, so they can still capture the whole editor.
 *
 * Per frame: wpe_ui_backend_handle_event() for each OS event → wpe_ui_backend_new_frame() → ImGui calls (ig*)
 * → software: wpe_ui_backend_render() inside a renderer frame (between begin_frame and end_frame); GPU:
 * wpe_ui_backend_present().
 *
 * Textures: ImGui's own (the font atlas) are created and updated by the backend as ImGui requests.
 * Images shown with igImage (the game view) are uploaded with wpe_ui_backend_set_image().
 *
 * One backend per process (ImGui keeps a global current context).
 *
 * - Dependencies: core, renderer, platform (events, the window's renderer), cimgui, SDL3
 * - Thread affinity: main thread only
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_EDITOR_UI_BACKEND_H
#define WPE_EDITOR_UI_BACKEND_H

#include <stdint.h>

#include "core/result.h"
#include "platform/platform.h"
#include "renderer/renderer.h"

typedef struct WPE_UiBackend WPE_UiBackend;

/**
 * Creates the ImGui context (docking enabled, dark style, no .ini file: layouts are set in code).
 *
 * @param gpu The renderer to draw with (wpe_window_renderer()), or NULL to draw in software.
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_OUT_OF_MEMORY or
 *         WPE_RESULT_ERROR_INVALID_STATE (a backend already exists).
 *
 * @thread_safety Main thread only
 */
[[nodiscard]] WPE_Result wpe_ui_backend_create(WPE_PlatformRenderer* gpu, WPE_UiBackend** out_ui);

/** Destroys the context and every texture it created; NULL is a no-op. @thread_safety Main thread only */
void wpe_ui_backend_destroy(WPE_UiBackend* ui);

/** @return Whether the backend draws through a GPU renderer (false: software). */
bool wpe_ui_backend_is_gpu(const WPE_UiBackend* ui);

/** Feeds an OS event (keyboard, mouse, text, focus) to ImGui. @thread_safety Main thread only */
void wpe_ui_backend_handle_event(WPE_UiBackend* ui, const WPE_PlatformEvent* event);

/** @return Whether ImGui wants the mouse / keyboard (so the application should not act on them). */
bool wpe_ui_backend_wants_mouse(const WPE_UiBackend* ui);
bool wpe_ui_backend_wants_keyboard(const WPE_UiBackend* ui);

/**
 * Gets the GPU renderer's output size in pixels: the display size to pass to wpe_ui_backend_new_frame().
 * Software backends report 0 x 0 (their display is the framebuffer they render into).
 *
 * @thread_safety Main thread only
 */
void wpe_ui_backend_output_size(const WPE_UiBackend* ui, uint32_t* out_width, uint32_t* out_height);

/** Starts an ImGui frame for a display of the given size in pixels. @thread_safety Main thread only */
void wpe_ui_backend_new_frame(WPE_UiBackend* ui, uint32_t width, uint32_t height, float dt);

/**
 * Software: finishes the ImGui frame and submits it to a recording renderer frame whose framebuffer
 * has the display size. Buffers stay valid until the next wpe_ui_backend_render(), so end the renderer frame
 * first.
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_INVALID_STATE (a GPU backend),
 *         WPE_RESULT_ERROR_OUT_OF_MEMORY or a renderer error.
 *
 * @thread_safety Main thread only
 */
WPE_Result wpe_ui_backend_render(WPE_UiBackend* ui, WPE_Renderer* renderer);

/**
 * GPU: finishes the ImGui frame, draws it over `clear_color` and shows it in the window. The frame is
 * kept for wpe_ui_backend_capture().
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_INVALID_STATE (a software
 *         backend), WPE_RESULT_ERROR_OUT_OF_MEMORY or WPE_RESULT_ERROR (SDL failure; logged).
 *
 * @thread_safety Main thread only
 */
WPE_Result wpe_ui_backend_present(WPE_UiBackend* ui, WPE_Color clear_color);

/**
 * GPU: reads back the last frame wpe_ui_backend_present() showed (opaque pixels).
 *
 * @param out_image Receives the image; free with wpe_image_free().
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_INVALID_STATE (a software
 *         backend, or nothing presented yet), WPE_RESULT_ERROR_OUT_OF_MEMORY or WPE_RESULT_ERROR.
 *
 * @thread_safety Main thread only
 */
WPE_Result wpe_ui_backend_capture(WPE_UiBackend* ui, WPE_Image* out_image);

/**
 * Creates or updates an image for igImage, shown with nearest filtering.
 *
 * @param id In: the image to update (0 creates one). Out: its ImTextureID (as uint64_t); it can change
 *           when the size does.
 * @param texels width x height pixels (0xAARRGGBB), copied.
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_INVALID_ARG (an unknown id or a
 *         zero size), WPE_RESULT_ERROR_OUT_OF_MEMORY or WPE_RESULT_ERROR (SDL failure; logged).
 *
 * @thread_safety Main thread only
 */
WPE_Result wpe_ui_backend_set_image(WPE_UiBackend* ui, uint64_t* id, uint32_t width, uint32_t height,
                                    const WPE_Color* texels);

#endif /* WPE_EDITOR_UI_BACKEND_H */
