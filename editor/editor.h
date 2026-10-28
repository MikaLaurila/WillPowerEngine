/**
 * @file editor.h
 * @brief The editor's model: a project open in an embedded engine, the scene being edited, the
 *        selection, undo/redo, play mode, and the JSON command interface through which both the UI
 *        and AI agents change things (docs/editor.md lists the commands).
 *
 * Every change goes through wpe_editor_execute(), so whatever a person can do in the UI an agent
 * can do with a command, and every change can be undone. Commands use the scene-file schema
 * (docs/scenes.md) with live entity handles as ids.
 *
 * Edit mode renders the scene from the editor's own camera without simulating; play mode runs the
 * game from a snapshot of the scene, and stopping restores the snapshot.
 *
 * - Dependencies: engine, core (JSON)
 * - Thread affinity: main thread only
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_EDITOR_EDITOR_H
#define WPE_EDITOR_EDITOR_H

#include <stdint.h>

#include "core/json.h"
#include "core/result.h"
#include "engine/engine.h"

typedef struct WPE_Editor WPE_Editor;

typedef struct WPE_EditorDesc {
    const char* project_dir; /* Required: the project directory (project.json at its root). */
    const char* scene;       /* Scene to open (asset path); NULL opens the project's start scene. */
    bool no_audio_device;    /* Mix silently (batch runs, tests). */
    uint32_t workers;        /* Render threads (0 = one per core). */
} WPE_EditorDesc;

typedef enum WPE_EditorMode {
    WPE_EDITOR_EDIT = 0,
    WPE_EDITOR_PLAY,
    WPE_EDITOR_PAUSED,
} WPE_EditorMode;

typedef enum WPE_EditorGizmo {
    WPE_EDITOR_GIZMO_MOVE = 0,
    WPE_EDITOR_GIZMO_ROTATE,
    WPE_EDITOR_GIZMO_SCALE,
} WPE_EditorGizmo;

/** How the editor shows the scene: not part of the scene, never saved with it or undone. */
typedef struct WPE_EditorView {
    bool grid;             /* A ground grid in edit mode (default on). */
    float grid_spacing;    /* World units between grid lines (default 1). */
    bool physics;          /* Collider wireframes, in edit and play mode (default off). */
    bool lights;           /* An icon at every light in edit mode; clicking one selects it (default on). */
    WPE_EditorGizmo gizmo; /* The viewport gizmo's mode (default move). */
    bool gizmo_local;      /* Gizmo axes: the entity's own (true) or the world's (false, default). Scaling
                              always uses the entity's own axes. */
    bool hot_reload;       /* Reload changed scripts twice a second (default on). */
    bool snap;             /* Gizmo drags snap (default off; holding Ctrl inverts it while dragging). */
    float snap_move;   /* Move step in world units (default 0.5): world positions land on its multiples. */
    float snap_rotate; /* Rotation step in degrees (default 15). */
    float snap_scale;  /* Scale step (default 0.1): scales land on its multiples. */
} WPE_EditorView;

/**
 * Opens a project in an embedded engine, in edit mode.
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_OUT_OF_MEMORY or an engine error.
 *
 * @thread_safety Main thread only
 */
[[nodiscard]] WPE_Result wpe_editor_create(const WPE_EditorDesc* desc, WPE_Editor** out_editor);

/** Closes the project; NULL is a no-op. @thread_safety Main thread only */
void wpe_editor_destroy(WPE_Editor* editor);

/**
 * Runs one command, a JSON object {"cmd": name, ...arguments}, and writes the reply, a JSON object
 * {"ok": bool, "result": ..., "error": "...", "log": [...]}. Messages logged while the command ran
 * (warnings and errors from loaders, scripts...) are included under "log".
 *
 * @return WPE_RESULT_OK when the command succeeded; its error otherwise (the reply explains it).
 *
 * @thread_safety Main thread only
 */
WPE_Result wpe_editor_execute(WPE_Editor* editor, const char* command, size_t size, WPE_JsonWriter* reply);

/** wpe_editor_execute() of an already parsed command object. @thread_safety Main thread only */
WPE_Result wpe_editor_execute_value(WPE_Editor* editor, const WPE_JsonValue* command, WPE_JsonWriter* reply);

/** Runs one engine frame in the current mode (edit mode renders only). @thread_safety Main thread only */
WPE_Result wpe_editor_frame(WPE_Editor* editor, double dt);

/** @return The embedded engine (borrowed). @thread_safety Main thread only */
WPE_Engine* wpe_editor_engine(const WPE_Editor* editor);

/** @return The current mode. @thread_safety Main thread only */
WPE_EditorMode wpe_editor_mode(const WPE_Editor* editor);

/** @return The selected entity, or WPE_ENTITY_NULL. @thread_safety Main thread only */
WPE_Entity wpe_editor_selection(const WPE_Editor* editor);

/** @return The editor's camera entity (never saved), and whether it is an editing helper. */
WPE_Entity wpe_editor_camera(const WPE_Editor* editor);
bool wpe_editor_is_helper(const WPE_Editor* editor, WPE_Entity entity);

/** @return The view settings (changed with the "set_view" command). @thread_safety Main thread only */
WPE_EditorView wpe_editor_view(const WPE_Editor* editor);

/** @return The asset path of the scene being edited ("" for a new, unsaved scene). */
const char* wpe_editor_scene_path(const WPE_Editor* editor);

/** @return Whether the scene changed since it was loaded or saved. */
bool wpe_editor_dirty(const WPE_Editor* editor);

/** @return The number of undo and redo steps available. */
uint32_t wpe_editor_undo_count(const WPE_Editor* editor);
uint32_t wpe_editor_redo_count(const WPE_Editor* editor);

/** @return Whether the "quit" command asked the editor application to close. */
bool wpe_editor_quit_requested(const WPE_Editor* editor);

/** @return The project directory given at creation (borrowed). */
const char* wpe_editor_project_dir(const WPE_Editor* editor);

/**
 * Optional hook for the "screenshot" command's "editor" target (the whole editor window, drawn by
 * the UI): writes the editor's current image as PNG.
 */
typedef WPE_Result (*WPE_EditorScreenshotFn)(void* user_data, const char* os_path);
void wpe_editor_set_screenshot_hook(WPE_Editor* editor, WPE_EditorScreenshotFn fn, void* user_data);

#endif /* WPE_EDITOR_EDITOR_H */
