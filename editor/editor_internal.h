/**
 * @file editor_internal.h
 * @brief Private state shared by the editor model (editor.c) and its commands (editor_commands.c).
 *        Not a public API.
 */
#ifndef WPE_EDITOR_EDITOR_INTERNAL_H
#define WPE_EDITOR_EDITOR_INTERNAL_H

#include <SDL3/SDL.h>

#include "core/log.h"
#include "editor/editor.h"

#define WPE__EDITOR_UNDO_MAX 64u
#define WPE__EDITOR_LOG_LINES 256u
#define WPE__EDITOR_LOG_TEXT 512u
#define WPE__EDITOR_RELOAD_SECONDS 0.5 /* Hot reload: how often scripts are checked for changes. */

typedef struct wpe__Snapshot {
    char* text; /* A scene document (owned). */
    size_t size;
} wpe__Snapshot;

/* Recent INFO+ messages (the console; replies carry the WARN+ ones), written by the log sink from any thread.
 */
typedef struct wpe__LogLine {
    WPE_LogLevel level;
    char text[WPE__EDITOR_LOG_TEXT];
} wpe__LogLine;

struct WPE_Editor {
    WPE_Engine* engine;
    char project_dir[1024];
    char scene_path[WPE_VFS_PATH_MAX];
    WPE_Entity camera;       /* Editor helper camera (excluded from saves). */
    WPE_Entity scene_camera; /* The camera the scene renders from in play mode and saves. */
    WPE_Entity selection;
    WPE_Entity previewed; /* The particle emitter played in edit mode (the selection, when it has one). */
    WPE_EditorMode mode;
    bool dirty;
    bool dirty_before_change; /* Restored when a change is aborted. */
    uint64_t changes;         /* Undo steps recorded so far (begin_change calls). */
    /* Undo coalescing: a command carrying the same "coalesce" key as the previous change joins its step. */
    const char* coalesce_next; /* Key of the command being executed (borrowed during execution). */
    char last_coalesce[128];
    bool coalesced;      /* The current change joined the previous step. */
    bool quit_requested; /* The "quit" command (the window closes after the current frame). */
    WPE_EditorView view;
    double since_reload; /* Seconds since scripts were last checked for changes. */

    wpe__Snapshot undo[WPE__EDITOR_UNDO_MAX];
    uint32_t undo_count;
    wpe__Snapshot redo[WPE__EDITOR_UNDO_MAX];
    uint32_t redo_count;
    wpe__Snapshot play; /* The scene as it was when play mode started. */

    WPE_EditorScreenshotFn screenshot;
    void* screenshot_user_data;

    SDL_Mutex* log_mutex;
    WPE_LogSinkId log_sink;
    wpe__LogLine log[WPE__EDITOR_LOG_LINES];
    uint64_t log_written; /* Total lines ever written (index = n % WPE__EDITOR_LOG_LINES). */
    uint64_t log_cleared; /* Lines before this position are hidden (clear_log). */
};

/* Scene context for saving and editing (excludes the editor's helpers). */
WPE_SceneContext wpe__editor_context(WPE_Editor* editor);

/* Writes the scene (helpers excluded) as text; *out owns the result. */
WPE_Result wpe__editor_snapshot(WPE_Editor* editor, wpe__Snapshot* out);

/* Replaces the world with a snapshot, recreating the helpers; the selection is cleared. */
WPE_Result wpe__editor_restore(WPE_Editor* editor, const wpe__Snapshot* snapshot, const char* name);

/* Records an undo step before a change (clears redo). */
WPE_Result wpe__editor_begin_change(WPE_Editor* editor);

/* Drops the undo step recorded by the last begin_change (the change failed); with `restore`, also
 * puts the world back as it was (for changes that may have been partly applied). */
WPE_Result wpe__editor_abort_change(WPE_Editor* editor, bool restore);

/* Undo/redo; WPE_RESULT_ERROR_NOT_FOUND when there is nothing to undo/redo. */
WPE_Result wpe__editor_undo(WPE_Editor* editor);
WPE_Result wpe__editor_redo(WPE_Editor* editor);

/* Modes. */
WPE_Result wpe__editor_play(WPE_Editor* editor);
WPE_Result wpe__editor_pause(WPE_Editor* editor);
WPE_Result wpe__editor_stop(WPE_Editor* editor);

/* Scene files; paths are asset paths inside the project directory. */
WPE_Result wpe__editor_new_scene(WPE_Editor* editor);
WPE_Result wpe__editor_load_scene(WPE_Editor* editor, const char* path);
WPE_Result wpe__editor_save_scene(WPE_Editor* editor, const char* path);

/* Copies log lines written since `since` (at most `max`, none from before the last clear) through `fn`;
 * returns the new position. */
uint64_t wpe__editor_log_since(WPE_Editor* editor, uint64_t since, uint32_t max,
                               void (*fn)(const wpe__LogLine* line, void* user_data), void* user_data);
uint64_t wpe__editor_log_position(WPE_Editor* editor);
/* Hides the lines logged so far (the console's clear). */
void wpe__editor_log_clear(WPE_Editor* editor);

#endif /* WPE_EDITOR_EDITOR_INTERNAL_H */
