/**
 * @file script_panel.h
 * @brief The editor's Script panel: Luau files in tabs, edited with syntax highlighting, line numbers,
 *        auto-indent, undo, search, compile errors as you type and runtime errors from the log.
 *
 * Files are read, checked and written through editor commands (read_file, check_script, write_file), like
 * every other change the UI makes. Private to the editor UI.
 *
 * - Dependencies: editor, cimgui
 * - Thread affinity: the UI thread
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_EDITOR_SCRIPT_PANEL_H
#define WPE_EDITOR_SCRIPT_PANEL_H

#include <stdint.h>

#include "editor/editor.h"

typedef struct WPE_ScriptPanel WPE_ScriptPanel;

/** @return The panel, or NULL out of memory. */
WPE_ScriptPanel* wpe_script_panel_create(WPE_Editor* editor);

void wpe_script_panel_destroy(WPE_ScriptPanel* panel);

/** Opens a script in a tab (or switches to it) and shows `line` (1-based; 0 keeps the place), with the cursor
 * at `column` (1-based characters; 0: the line's first non-blank). @return false when it can't be read
 * (logged). */
bool wpe_script_panel_open(WPE_ScriptPanel* panel, const char* path, uint32_t line, uint32_t column);

/** Draws the "Script" window (and its prompts). Call once per UI frame. */
void wpe_script_panel_draw(WPE_ScriptPanel* panel);

/** @return Whether the code has the keyboard (the editor's own shortcuts then stay quiet). */
bool wpe_script_panel_focused(const WPE_ScriptPanel* panel);

/** @return Whether the panel's New script button was pressed since the last call (the UI owns the dialog). */
bool wpe_script_panel_take_new_request(WPE_ScriptPanel* panel);

/** @return Whether any open script has unsaved changes. */
bool wpe_script_panel_dirty(const WPE_ScriptPanel* panel);

/** Asks to save or drop the unsaved scripts before the editor closes. */
void wpe_script_panel_request_close_all(WPE_ScriptPanel* panel);

/** @return 1 once the scripts were saved or dropped for closing, -1 when that was cancelled, 0 while asking
 * (or not asked). Reading the answer clears it. */
int wpe_script_panel_close_all_result(WPE_ScriptPanel* panel);

#endif /* WPE_EDITOR_SCRIPT_PANEL_H */
