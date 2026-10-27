/**
 * @file editor_ui.h
 * @brief The editor's user interface (Dear ImGui on the software rasterizer): menu and toolbar,
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

/** Creates the UI for an editor (one per process). @thread_safety Main thread only */
[[nodiscard]] WPE_Result wpe_editor_ui_create(WPE_Editor* editor, WPE_EditorUi** out_ui);

/** Destroys the UI; NULL is a no-op. @thread_safety Main thread only */
void wpe_editor_ui_destroy(WPE_EditorUi* ui);

/** Feeds an OS event to the UI (and, in play mode with the viewport focused, to the game). */
void wpe_editor_ui_handle_event(WPE_EditorUi* ui, const WPE_PlatformEvent* event);

/**
 * Runs one editor frame: the engine (edit or play mode), then the UI, rendered into `renderer`
 * (whose framebuffer is the window size).
 *
 * @return WPE_RESULT_OK or the first error.
 *
 * @thread_safety Main thread only
 */
WPE_Result wpe_editor_ui_frame(WPE_EditorUi* ui, WPE_Renderer* renderer, float dt);

/** @return Whether the user chose File > Quit. */
bool wpe_editor_ui_quit_requested(const WPE_EditorUi* ui);

/** @return Whether the game in play mode asks for a captured mouse (the window applies it). */
bool wpe_editor_ui_mouse_captured(const WPE_EditorUi* ui);

#endif /* WPE_EDITOR_EDITOR_UI_H */
