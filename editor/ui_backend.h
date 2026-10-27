/**
 * @file ui_backend.h
 * @brief Dear ImGui (through cimgui) on the engine's software rasterizer: the editor's UI is drawn
 *        into a WPE_Renderer framebuffer like everything else, so headless runs capture exactly
 *        what a user sees.
 *
 * Per frame: wpe_ui_handle_event() for each OS event → wpe_ui_new_frame() → ImGui calls (ig*) →
 * wpe_ui_render() inside a renderer frame (between begin_frame and end_frame).
 *
 * Textures: ImGui's own (the font atlas) are created and updated by the backend as ImGui requests.
 * Engine textures shown with igImage are registered with wpe_ui_texture_id().
 *
 * One backend per process (ImGui keeps a global current context).
 *
 * - Dependencies: core, renderer, platform (events), cimgui, SDL3 (event translation)
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
#include "renderer/texture.h"

typedef struct WPE_UiBackend WPE_UiBackend;

/**
 * Creates the ImGui context (docking enabled, dark style, no .ini file: layouts are set in code).
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_OUT_OF_MEMORY or
 *         WPE_RESULT_ERROR_INVALID_STATE (a backend already exists).
 *
 * @thread_safety Main thread only
 */
[[nodiscard]] WPE_Result wpe_ui_create(WPE_UiBackend** out_ui);

/** Destroys the context and every texture it created; NULL is a no-op. @thread_safety Main thread only */
void wpe_ui_destroy(WPE_UiBackend* ui);

/** Feeds an OS event (keyboard, mouse, text, focus) to ImGui. @thread_safety Main thread only */
void wpe_ui_handle_event(WPE_UiBackend* ui, const WPE_PlatformEvent* event);

/** @return Whether ImGui wants the mouse / keyboard (so the application should not act on them). */
bool wpe_ui_wants_mouse(const WPE_UiBackend* ui);
bool wpe_ui_wants_keyboard(const WPE_UiBackend* ui);

/** Starts an ImGui frame for a display of the given size in pixels. @thread_safety Main thread only */
void wpe_ui_new_frame(WPE_UiBackend* ui, uint32_t width, uint32_t height, float dt);

/**
 * Finishes the ImGui frame and submits it to a recording renderer frame whose framebuffer has the
 * display size. Buffers stay valid until the next wpe_ui_render(), so end the renderer frame first.
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_OUT_OF_MEMORY or a renderer error.
 *
 * @thread_safety Main thread only
 */
WPE_Result wpe_ui_render(WPE_UiBackend* ui, WPE_Renderer* renderer);

/**
 * Makes an engine texture drawable with igImage (registering it on first use).
 *
 * @return The ImTextureID to use (as uint64_t), or 0 when out of memory.
 *
 * @thread_safety Main thread only; the texture must outlive its use by ImGui
 */
uint64_t wpe_ui_texture_id(WPE_UiBackend* ui, const WPE_Texture* texture);

#endif /* WPE_EDITOR_UI_BACKEND_H */
