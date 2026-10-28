/*
 * The editor model: project, scene, selection, undo/redo (whole-scene snapshots), play mode and the
 * log of recent messages. Commands live in editor_commands.c.
 */
#define WPE_LOG_MODULE "editor"
#include <stdio.h>
#include <string.h>

#include "core/alloc.h"
#include "core/debug.h"
#include "editor/editor_internal.h"
#include "graphics/graphics.h"
#include "platform/platform.h"
#include "scene/scene.h"

/* ---- Log capture ------------------------------------------------------------------------------------ */

/* Runs under the log lock on whichever thread logged. */
static void wpe__editor_log_sink(const WPE_LogMessage* message, void* user_data) {
    if (message->level < WPE_LOG_LEVEL_INFO) return;
    WPE_Editor* editor = user_data;
    SDL_LockMutex(editor->log_mutex);
    wpe__LogLine* line = &editor->log[editor->log_written % WPE__EDITOR_LOG_LINES];
    line->level = message->level;
    snprintf(line->text, sizeof line->text, "[%s][%s] %s", wpe_log_level_name(message->level),
             message->module, message->text);
    ++editor->log_written;
    SDL_UnlockMutex(editor->log_mutex);
}

uint64_t wpe__editor_log_position(WPE_Editor* editor) {
    SDL_LockMutex(editor->log_mutex);
    const uint64_t position = editor->log_written;
    SDL_UnlockMutex(editor->log_mutex);
    return position;
}

uint64_t wpe__editor_log_since(WPE_Editor* editor, uint64_t since, uint32_t max,
                               void (*fn)(const wpe__LogLine* line, void* user_data), void* user_data) {
    SDL_LockMutex(editor->log_mutex);
    const uint64_t end = editor->log_written;
    uint64_t start = since;
    if (end - start > WPE__EDITOR_LOG_LINES) start = end - WPE__EDITOR_LOG_LINES; /* Older lines are gone. */
    if (end - start > max) start = end - max;
    for (uint64_t i = start; i < end; ++i)
        fn(&editor->log[i % WPE__EDITOR_LOG_LINES], user_data);
    SDL_UnlockMutex(editor->log_mutex);
    return end;
}

/* ---- Helpers and snapshots -------------------------------------------------------------------------- */

static bool wpe__editor_include(WPE_Entity entity, void* user_data) {
    return !wpe_editor_is_helper(user_data, entity);
}

WPE_SceneContext wpe__editor_context(WPE_Editor* editor) {
    return (WPE_SceneContext){.ecs = wpe_engine_ecs(editor->engine),
                              .assets = wpe_engine_assets(editor->engine),
                              .scripts = wpe_engine_scripts(editor->engine),
                              .include = wpe__editor_include,
                              .include_user_data = editor,
                              .prefabs = wpe_engine_prefabs(editor->engine)};
}

/* Creates the editor camera, looking at the origin from above and behind. */
static WPE_Result wpe__editor_create_helpers(WPE_Editor* editor) {
    WPE_Ecs* ecs = wpe_engine_ecs(editor->engine);
    editor->scene_camera = wpe_engine_camera(editor->engine);
    const WPE_Entity camera = wpe_ecs_spawn(ecs);
    if (camera == WPE_ENTITY_NULL) return WPE_RESULT_ERROR_CAPACITY;
    WPE_Transform t = wpe_transform_identity();
    const WPE_Mat4 view = wpe_mat4_look_at(wpe_vec3(8, 6, 10), wpe_vec3(0, 0, 0), wpe_vec3(0, 1, 0));
    WPE_Mat4 world;
    WPE_Vec3 scale;
    if (wpe_mat4_inverse(&view, &world)) wpe_mat4_decompose(&world, &t.position, &t.rotation, &scale);
    WPE_Camera cam = wpe_camera_default();
    cam.far_z = 2000.0f;
    WPE_Result result = wpe_ecs_add_component(ecs, camera, WPE_COMP_TRANSFORM, &t, NULL);
    if (result == WPE_RESULT_OK) result = wpe_ecs_add_component(ecs, camera, WPE_COMP_CAMERA, &cam, NULL);
    if (result == WPE_RESULT_OK) result = wpe_scene_set_name(ecs, camera, "editor camera");
    if (result != WPE_RESULT_OK) {
        wpe_ecs_despawn(ecs, camera);
        return result;
    }
    editor->camera = camera;
    if (editor->mode == WPE_EDITOR_EDIT) wpe_engine_set_camera(editor->engine, camera);
    return WPE_RESULT_OK;
}

static void wpe__snapshot_free(wpe__Snapshot* s) {
    wpe_free(s->text);
    *s = (wpe__Snapshot){0};
}

WPE_Result wpe__editor_snapshot(WPE_Editor* editor, wpe__Snapshot* out) {
    *out = (wpe__Snapshot){0};
    const WPE_SceneContext context = wpe__editor_context(editor);
    WPE_JsonWriter* writer = NULL;
    WPE_Result result = wpe_json_writer_create(&writer);
    if (result != WPE_RESULT_OK) return result;
    result = wpe_scene_save(&context, wpe_engine_environment(editor->engine), editor->scene_camera, writer);
    const char* text = NULL;
    size_t size = 0;
    if (result == WPE_RESULT_OK) result = wpe_json_writer_finish(writer, &text, &size);
    if (result == WPE_RESULT_OK) {
        out->text = wpe_alloc(size + 1);
        if (out->text) {
            memcpy(out->text, text, size + 1);
            out->size = size;
        } else {
            result = WPE_RESULT_ERROR_OUT_OF_MEMORY;
        }
    }
    wpe_json_writer_destroy(writer);
    return result;
}

WPE_Result wpe__editor_restore(WPE_Editor* editor, const wpe__Snapshot* snapshot, const char* name) {
    editor->selection = WPE_ENTITY_NULL;
    editor->camera = WPE_ENTITY_NULL;
    WPE_Result result = wpe_engine_load_scene_text(editor->engine, snapshot->text, snapshot->size, name);
    const WPE_Result helpers = wpe__editor_create_helpers(editor);
    return result != WPE_RESULT_OK ? result : helpers;
}

static void wpe__clear_stack(wpe__Snapshot* stack, uint32_t* count) {
    for (uint32_t i = 0; i < *count; ++i)
        wpe__snapshot_free(&stack[i]);
    *count = 0;
}

/* Pushes onto a bounded stack, dropping the oldest entry when full (ownership moves in). */
static void wpe__push(wpe__Snapshot* stack, uint32_t* count, wpe__Snapshot* snapshot) {
    if (*count == WPE__EDITOR_UNDO_MAX) {
        wpe__snapshot_free(&stack[0]);
        memmove(stack, stack + 1, (WPE__EDITOR_UNDO_MAX - 1) * sizeof *stack);
        --*count;
    }
    stack[(*count)++] = *snapshot;
    *snapshot = (wpe__Snapshot){0};
}

WPE_Result wpe__editor_begin_change(WPE_Editor* editor) {
    const char* coalesce = editor->coalesce_next;
    editor->coalesce_next = NULL;
    /* Continuous edits (a dragged slider) share one undo step. */
    if (coalesce && editor->last_coalesce[0] && strcmp(coalesce, editor->last_coalesce) == 0 &&
        editor->mode == WPE_EDITOR_EDIT) {
        editor->dirty = true;
        ++editor->changes;
        editor->coalesced = true;
        return WPE_RESULT_OK;
    }
    snprintf(editor->last_coalesce, sizeof editor->last_coalesce, "%s", coalesce ? coalesce : "");
    editor->coalesced = false;
    if (editor->mode != WPE_EDITOR_EDIT) {
        wpe_log_error("stop play mode before editing the scene");
        return WPE_RESULT_ERROR_INVALID_STATE;
    }
    wpe__Snapshot snapshot;
    const WPE_Result result = wpe__editor_snapshot(editor, &snapshot);
    if (result != WPE_RESULT_OK) return result;
    wpe__push(editor->undo, &editor->undo_count, &snapshot);
    wpe__clear_stack(editor->redo, &editor->redo_count);
    editor->dirty_before_change = editor->dirty;
    editor->dirty = true;
    ++editor->changes;
    return WPE_RESULT_OK;
}

WPE_Result wpe__editor_abort_change(WPE_Editor* editor, bool restore) {
    if (editor->coalesced) {
        /* The step belongs to earlier, successful edits of the same drag: keep it. */
        editor->coalesced = false;
        return WPE_RESULT_OK;
    }
    editor->last_coalesce[0] = '\0';
    if (editor->undo_count == 0) return WPE_RESULT_ERROR_NOT_FOUND;
    wpe__Snapshot snapshot = editor->undo[--editor->undo_count];
    const WPE_Result result =
        restore ? wpe__editor_restore(editor, &snapshot, "aborted change") : WPE_RESULT_OK;
    wpe__snapshot_free(&snapshot);
    editor->dirty = editor->dirty_before_change;
    return result;
}

/* Moves the current state onto `to` and restores the top of `from`. */
static WPE_Result wpe__editor_step(WPE_Editor* editor, wpe__Snapshot* from, uint32_t* from_count,
                                   wpe__Snapshot* to, uint32_t* to_count, const char* name) {
    editor->last_coalesce[0] = '\0';
    if (editor->mode != WPE_EDITOR_EDIT) {
        wpe_log_error("stop play mode before %s", name);
        return WPE_RESULT_ERROR_INVALID_STATE;
    }
    if (*from_count == 0) return WPE_RESULT_ERROR_NOT_FOUND;
    wpe__Snapshot current;
    WPE_Result result = wpe__editor_snapshot(editor, &current);
    if (result != WPE_RESULT_OK) return result;
    wpe__Snapshot target = from[--*from_count];
    result = wpe__editor_restore(editor, &target, name);
    wpe__snapshot_free(&target);
    wpe__push(to, to_count, &current);
    editor->dirty = true;
    return result;
}

WPE_Result wpe__editor_undo(WPE_Editor* editor) {
    return wpe__editor_step(editor, editor->undo, &editor->undo_count, editor->redo, &editor->redo_count,
                            "undo");
}

WPE_Result wpe__editor_redo(WPE_Editor* editor) {
    return wpe__editor_step(editor, editor->redo, &editor->redo_count, editor->undo, &editor->undo_count,
                            "redo");
}

/* ---- Modes ------------------------------------------------------------------------------------------ */

WPE_Result wpe__editor_play(WPE_Editor* editor) {
    if (editor->mode == WPE_EDITOR_PAUSED) {
        editor->mode = WPE_EDITOR_PLAY;
        wpe_engine_set_simulating(editor->engine, true);
        return WPE_RESULT_OK;
    }
    if (editor->mode == WPE_EDITOR_PLAY) return WPE_RESULT_OK;
    WPE_Result result = wpe__editor_snapshot(editor, &editor->play);
    if (result != WPE_RESULT_OK) return result;
    /* The game runs without the editor's helpers, from its own camera. */
    wpe_ecs_despawn(wpe_engine_ecs(editor->engine), editor->camera);
    editor->camera = WPE_ENTITY_NULL;
    editor->selection = WPE_ENTITY_NULL;
    wpe_engine_set_camera(editor->engine, editor->scene_camera);
    /* Keys pressed while editing (the play shortcut's included) were never the game's. */
    wpe_input_release_all(wpe_engine_input(editor->engine));
    editor->mode = WPE_EDITOR_PLAY;
    wpe_engine_set_simulating(editor->engine, true);
    wpe_log_info("play");
    return WPE_RESULT_OK;
}

WPE_Result wpe__editor_pause(WPE_Editor* editor) {
    if (editor->mode == WPE_EDITOR_EDIT) return WPE_RESULT_ERROR_INVALID_STATE;
    editor->mode = WPE_EDITOR_PAUSED;
    wpe_engine_set_simulating(editor->engine, false);
    return WPE_RESULT_OK;
}

WPE_Result wpe__editor_stop(WPE_Editor* editor) {
    if (editor->mode == WPE_EDITOR_EDIT) return WPE_RESULT_OK;
    wpe_engine_set_simulating(editor->engine, false);
    editor->mode = WPE_EDITOR_EDIT;
    /* Keys held when play stops would never see their release (the game stops getting key events). */
    wpe_input_set_mouse_captured(wpe_engine_input(editor->engine), false);
    wpe_input_release_all(wpe_engine_input(editor->engine));
    const WPE_Result result = wpe__editor_restore(editor, &editor->play, "play snapshot");
    wpe__snapshot_free(&editor->play);
    wpe_log_info("stopped");
    return result;
}

/* ---- Scenes ----------------------------------------------------------------------------------------- */

static void wpe__editor_reset_history(WPE_Editor* editor) {
    wpe__clear_stack(editor->undo, &editor->undo_count);
    wpe__clear_stack(editor->redo, &editor->redo_count);
    editor->dirty = false;
}

WPE_Result wpe__editor_new_scene(WPE_Editor* editor) {
    WPE_Result result = wpe__editor_stop(editor);
    static char empty[] = "{\"format\": \"wpe.scene\", \"version\": 1}"; /* Read only. */
    const wpe__Snapshot snapshot = {.text = empty, .size = sizeof empty - 1};
    if (result == WPE_RESULT_OK) result = wpe__editor_restore(editor, &snapshot, "new scene");
    editor->scene_path[0] = '\0';
    wpe__editor_reset_history(editor);
    return result;
}

WPE_Result wpe__editor_load_scene(WPE_Editor* editor, const char* path) {
    if (!wpe_vfs_path_is_valid(path)) {
        wpe_log_error("'%s' is not an asset path", path);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    WPE_Result result = wpe__editor_stop(editor);
    void* data = NULL;
    size_t size = 0;
    if (result == WPE_RESULT_OK)
        result = wpe_vfs_read_all(wpe_engine_vfs(editor->engine), path, &data, &size);
    if (result == WPE_RESULT_ERROR_NOT_FOUND) wpe_log_error("scene '%s' not found", path);
    if (result == WPE_RESULT_OK) {
        const wpe__Snapshot snapshot = {.text = data, .size = size};
        result = wpe__editor_restore(editor, &snapshot, path);
    }
    wpe_free(data);
    if (result == WPE_RESULT_OK) {
        snprintf(editor->scene_path, sizeof editor->scene_path, "%s", path);
        wpe__editor_reset_history(editor);
    }
    return result;
}

WPE_Result wpe__editor_save_scene(WPE_Editor* editor, const char* path) {
    if (!path || !path[0]) path = editor->scene_path;
    if (!path[0] || !wpe_vfs_path_is_valid(path)) {
        wpe_log_error("save the scene under an asset path (e.g. scenes/level.scene)");
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    if (editor->mode != WPE_EDITOR_EDIT) {
        wpe_log_error("stop play mode before saving");
        return WPE_RESULT_ERROR_INVALID_STATE;
    }
    char os_path[2048];
    if (snprintf(os_path, sizeof os_path, "%s/%s", editor->project_dir, path) >= (int)sizeof os_path)
        return WPE_RESULT_ERROR_INVALID_ARG;
    /* Create the folder for new scenes. */
    char folder[2048];
    memcpy(folder, os_path, sizeof folder);
    char* slash = strrchr(folder, '/');
    if (slash) *slash = '\0';
    WPE_Result result = wpe_directory_create(folder);
    const WPE_SceneContext context = wpe__editor_context(editor);
    if (result == WPE_RESULT_OK)
        result = wpe_scene_save_file(&context, wpe_engine_environment(editor->engine), editor->scene_camera,
                                     os_path);
    if (result == WPE_RESULT_OK) {
        if (path != editor->scene_path) snprintf(editor->scene_path, sizeof editor->scene_path, "%s", path);
        editor->dirty = false;
        wpe_log_info("saved %s", os_path);
    }
    return result;
}

/* ---- Lifecycle -------------------------------------------------------------------------------------- */

WPE_Result wpe_editor_create(const WPE_EditorDesc* desc, WPE_Editor** out_editor) {
    if (!desc || !desc->project_dir || !out_editor) return WPE_RESULT_ERROR_NULL_ARG;
    *out_editor = NULL;
    WPE_Editor* editor = wpe_alloc_zeroed(1, sizeof *editor);
    if (!editor) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    snprintf(editor->project_dir, sizeof editor->project_dir, "%s", desc->project_dir);
    editor->view = (WPE_EditorView){.grid = true,
                                    .grid_spacing = 1.0f,
                                    .icons = true,
                                    .hot_reload = true,
                                    .snap_move = 0.5f,
                                    .snap_rotate = 15.0f,
                                    .snap_scale = 0.1f,
                                    .snap_ui = 8.0f};
    editor->log_mutex = SDL_CreateMutex();
    WPE_Result result = editor->log_mutex ? WPE_RESULT_OK : WPE_RESULT_ERROR_OUT_OF_MEMORY;
    if (result == WPE_RESULT_OK) result = wpe_log_add_sink(wpe__editor_log_sink, editor, &editor->log_sink);
    /* The engine starts empty; the editor loads the scene itself (with its helpers). */
    if (result == WPE_RESULT_OK) {
        result = wpe_engine_create(&(WPE_EngineDesc){.project_dir = desc->project_dir,
                                                     .scene = "",
                                                     .embedded = true,
                                                     .no_audio_device = desc->no_audio_device,
                                                     .workers = desc->workers},
                                   &editor->engine);
    }
    if (result == WPE_RESULT_OK) {
        wpe_engine_set_simulating(editor->engine, false);
        const char* scene = desc->scene ? desc->scene : wpe_engine_project(editor->engine)->start_scene;
        result = scene[0] ? wpe__editor_load_scene(editor, scene) : wpe__editor_new_scene(editor);
    }
    if (result != WPE_RESULT_OK) {
        wpe_editor_destroy(editor);
        return result;
    }
    *out_editor = editor;
    return WPE_RESULT_OK;
}

void wpe_editor_destroy(WPE_Editor* editor) {
    if (!editor) return;
    wpe_engine_destroy(editor->engine);
    wpe__clear_stack(editor->undo, &editor->undo_count);
    wpe__clear_stack(editor->redo, &editor->redo_count);
    wpe__snapshot_free(&editor->play);
    if (editor->log_sink) wpe_log_remove_sink(editor->log_sink);
    SDL_DestroyMutex(editor->log_mutex);
    wpe_free(editor);
}

/* The camera the game renders from: the scene's, else the first camera that isn't the editor's. */
static WPE_Entity wpe__editor_game_camera(const WPE_Editor* editor) {
    WPE_Ecs* ecs = wpe_engine_ecs(editor->engine);
    if (editor->scene_camera != editor->camera &&
        wpe_ecs_has_component(ecs, editor->scene_camera, WPE_COMP_CAMERA))
        return editor->scene_camera;
    WPE_EcsIter it = wpe_ecs_query(
        ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_CAMERA) | WPE_ECS_MASK(WPE_COMP_TRANSFORM)});
    while (wpe_ecs_iter_next(&it)) {
        if (it.entity != editor->camera) return it.entity;
    }
    return WPE_ENTITY_NULL;
}

/* What the engine draws: in edit mode, the 3D view (editor camera, no UI) or the 2D view (the game's
 * camera, or none, with the UI); while playing, the game as it is. */
static void wpe__editor_apply_view(WPE_Editor* editor) {
    WPE_Engine* engine = editor->engine;
    if (editor->mode != WPE_EDITOR_EDIT) {
        wpe_engine_set_draw_ui(engine, true);
        wpe_engine_set_draw_scene(engine, true);
        return;
    }
    const WPE_Entity game = wpe__editor_game_camera(editor);
    wpe_engine_set_camera(engine, editor->view.view_2d ? game : editor->camera);
    wpe_engine_set_draw_scene(engine, !editor->view.view_2d || game != WPE_ENTITY_NULL);
    wpe_engine_set_draw_ui(engine, editor->view.view_2d);
}

/* Edit mode plays the selected particle emitter (and only it), from the start of its cycle when it becomes
 * selected, without changing its settings. */
static WPE_Result wpe__editor_preview_particles(WPE_Editor* editor, double dt) {
    WPE_Ecs* ecs = wpe_engine_ecs(editor->engine);
    const WPE_Entity selection = wpe_editor_selection(editor);
    const WPE_Entity wanted =
        wpe_ecs_has_component(ecs, selection, WPE_COMP_PARTICLES) ? selection : WPE_ENTITY_NULL;
    if (wanted != editor->previewed) {
        wpe_particles_clear(ecs, editor->previewed); /* No-op for an entity that is gone. */
        wpe_particles_clear(ecs, wanted);
        editor->previewed = wanted;
    }
    return wanted != WPE_ENTITY_NULL ? wpe_particles_preview(ecs, wanted, (float)dt) : WPE_RESULT_OK;
}

WPE_Result wpe_editor_frame(WPE_Editor* editor, double dt) {
    if (!editor) return WPE_RESULT_ERROR_NULL_ARG;
    const bool editing = editor->mode == WPE_EDITOR_EDIT;
    wpe__editor_apply_view(editor);
    if (editor->view.hot_reload) {
        editor->since_reload += dt;
        if (editor->since_reload >= WPE__EDITOR_RELOAD_SECONDS) {
            wpe_script_host_reload_changed(wpe_engine_scripts(editor->engine));
            editor->since_reload = 0.0;
        }
    }
    /* Edits don't step physics: bring bodies to the edited poses and scales (debug drawing shows them,
     * and play starts from them). */
    WPE_Result result = editing ? wpe_physics_sync(wpe_engine_physics(editor->engine)) : WPE_RESULT_OK;
    if (result == WPE_RESULT_OK && editing) result = wpe__editor_preview_particles(editor, dt);
    wpe_engine_set_debug_draw(
        editor->engine, &(WPE_EngineDebugDraw){.physics = editor->view.physics,
                                               .grid = editor->view.grid && editing && !editor->view.view_2d,
                                               .grid_spacing = editor->view.grid_spacing});
    const WPE_Result frame = wpe_engine_frame(editor->engine, dt);
    if (result == WPE_RESULT_OK) result = frame;
    /* A game that quits (wpe.quit) in play mode returns the editor to edit mode. */
    if (!wpe_engine_running(editor->engine)) {
        wpe_engine_cancel_quit(editor->engine);
        if (editor->mode != WPE_EDITOR_EDIT) {
            wpe_log_info("the game quit");
            const WPE_Result stopped = wpe__editor_stop(editor);
            if (result == WPE_RESULT_OK) result = stopped;
        }
    }
    return result;
}

/* ---- Accessors -------------------------------------------------------------------------------------- */

WPE_Engine* wpe_editor_engine(const WPE_Editor* editor) {
    return editor ? editor->engine : NULL;
}

WPE_EditorMode wpe_editor_mode(const WPE_Editor* editor) {
    return editor ? editor->mode : WPE_EDITOR_EDIT;
}

WPE_Entity wpe_editor_selection(const WPE_Editor* editor) {
    return editor && wpe_ecs_is_alive(wpe_engine_ecs(editor->engine), editor->selection) ? editor->selection
                                                                                         : WPE_ENTITY_NULL;
}

WPE_Entity wpe_editor_camera(const WPE_Editor* editor) {
    return editor ? editor->camera : WPE_ENTITY_NULL;
}

bool wpe_editor_is_helper(const WPE_Editor* editor, WPE_Entity entity) {
    return editor && entity != WPE_ENTITY_NULL && entity == editor->camera;
}

WPE_EditorView wpe_editor_view(const WPE_Editor* editor) {
    return editor ? editor->view : (WPE_EditorView){0};
}

const char* wpe_editor_scene_path(const WPE_Editor* editor) {
    return editor ? editor->scene_path : "";
}

bool wpe_editor_dirty(const WPE_Editor* editor) {
    return editor && editor->dirty;
}

uint32_t wpe_editor_undo_count(const WPE_Editor* editor) {
    return editor ? editor->undo_count : 0;
}

uint32_t wpe_editor_redo_count(const WPE_Editor* editor) {
    return editor ? editor->redo_count : 0;
}

bool wpe_editor_quit_requested(const WPE_Editor* editor) {
    return editor && editor->quit_requested;
}

const char* wpe_editor_project_dir(const WPE_Editor* editor) {
    return editor ? editor->project_dir : NULL;
}

void wpe_editor_set_screenshot_hook(WPE_Editor* editor, WPE_EditorScreenshotFn fn, void* user_data) {
    if (!editor) return;
    editor->screenshot = fn;
    editor->screenshot_user_data = user_data;
}
