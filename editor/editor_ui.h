/**
 * @file editor_ui.h
 * @brief The editor's user interface (Dear ImGui, on the GPU in the window and on the software
 *        rasterizer headless; see ui_backend.h): menu and toolbar,
 *        hierarchy, inspector, viewport with picking, gizmos and a fly camera, assets,
 *        environment, and a console that also runs JSON commands.
 *
 * Every change the UI makes is an editor command (editor.h), exactly as an agent would send it, so
 * the UI and agents share one code path, undo history and validation.
 *
 * - Dependencies: editor, ui_backend, cimgui, SDL3 (events)
 * - Thread affinity: main thread only
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_EDITOR_EDITOR_UI_H
#define WPE_EDITOR_EDITOR_UI_H

#include "editor/editor.h"
#include "platform/platform.h"
#include "renderer/renderer.h"

typedef struct WPE_EditorUi WPE_EditorUi;

/**
 * Creates the UI for an editor (one per process).
 *
 * @param gpu The window's renderer (wpe_window_renderer()) to draw the UI on the GPU, or NULL to draw
 *            it with the software rasterizer (headless).
 *
 * @thread_safety Main thread only
 */
[[nodiscard]] WPE_Result wpe_editor_ui_create(WPE_Editor* editor, WPE_PlatformRenderer* gpu,
                                              WPE_EditorUi** out_ui);

/** Destroys the UI; NULL is a no-op. @thread_safety Main thread only */
void wpe_editor_ui_destroy(WPE_EditorUi* ui);

/** Feeds an OS event to the UI (and, in play mode with the viewport focused, to the game). */
void wpe_editor_ui_handle_event(WPE_EditorUi* ui, const WPE_PlatformEvent* event);

/**
 * Runs one editor frame: the engine (edit or play mode), then the UI. A GPU UI is presented in the
 * window (`renderer` is unused and may be NULL); a software UI is rendered into `renderer`, whose
 * framebuffer is the display.
 *
 * @return WPE_RESULT_OK or the first error.
 *
 * @thread_safety Main thread only
 */
WPE_Result wpe_editor_ui_frame(WPE_EditorUi* ui, WPE_Renderer* renderer, float dt);

/**
 * GPU UI: reads back the last frame shown in the window, exactly as the user sees it.
 *
 * @param out_image Receives the image; free with wpe_image_free().
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_INVALID_STATE (a software UI, or
 *         no frame yet), WPE_RESULT_ERROR_OUT_OF_MEMORY or WPE_RESULT_ERROR.
 *
 * @thread_safety Main thread only
 */
WPE_Result wpe_editor_ui_capture(WPE_EditorUi* ui, WPE_Image* out_image);

/** @return Whether the user chose File > Quit. */
bool wpe_editor_ui_quit_requested(const WPE_EditorUi* ui);

/** @return Whether the game in play mode asks for a captured mouse (the window applies it). */
bool wpe_editor_ui_mouse_captured(const WPE_EditorUi* ui);

#endif /* WPE_EDITOR_EDITOR_UI_H */
