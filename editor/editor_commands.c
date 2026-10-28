/*
 * The editor's JSON commands (reference: docs/editor.md). Each command writes exactly one JSON
 * value as its result (null when it has nothing to return), also when it fails, so replies stay
 * well-formed.
 */
#define WPE_LOG_MODULE "editor"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "core/alloc.h"
#include "editor/editor_internal.h"
#include "graphics/graphics.h"
#include "platform/platform.h"
#include "scene/scene.h"

typedef struct wpe__Call {
    WPE_Editor* editor;
    const WPE_JsonValue* args;
    WPE_JsonWriter* w;
    char error[512];
} wpe__Call;

WPE_PRINTF_LIKE(3, 4)
static WPE_Result wpe__fail(wpe__Call* call, WPE_Result result, const char* format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(call->error, sizeof call->error, format, args);
    va_end(args);
    return result;
}

/* Writes null and returns `result` (with the message, if the command did not set one). */
static WPE_Result wpe__null(wpe__Call* call, WPE_Result result) {
    wpe_json_write_null(call->w);
    if (result != WPE_RESULT_OK && !call->error[0])
        snprintf(call->error, sizeof call->error, "%s", wpe_result_to_string(result));
    return result;
}

static WPE_Ecs* wpe__ecs(const wpe__Call* call) {
    return wpe_engine_ecs(call->editor->engine);
}

/* Reads argument `key` as a live, non-helper entity. */
static WPE_Result wpe__entity_arg(wpe__Call* call, const char* key, WPE_Entity* out) {
    const WPE_JsonValue* v = wpe_json_get(call->args, key);
    const double id = wpe_json_number(v, -1.0);
    if (wpe_json_type(v) != WPE_JSON_NUMBER || id < 1.0 || id > (double)UINT32_MAX || id != floor(id))
        return wpe__fail(call, WPE_RESULT_ERROR_INVALID_ARG, "\"%s\" must be an entity id", key);
    *out = (WPE_Entity)id;
    if (!wpe_ecs_is_alive(wpe__ecs(call), *out) || wpe_editor_is_helper(call->editor, *out))
        return wpe__fail(call, WPE_RESULT_ERROR_NOT_FOUND,
                         "entity %u does not exist (list_entities shows the ids)", *out);
    return WPE_RESULT_OK;
}

static const char* wpe__string_arg(const wpe__Call* call, const char* key) {
    return wpe_json_string(wpe_json_get(call->args, key));
}

/* ---- Queries ---------------------------------------------------------------------------------------- */

static const char* const wpe__component_keys[] = {
    "transform",    "mesh", "camera", "light", "animator", "rigidbody", "collider",
    "audio_source", "ui",   "sprite", "text",  "button",   "script"};
static const WPE_ComponentType wpe__component_types[] = {WPE_COMP_TRANSFORM,
                                                         WPE_COMP_MESH,
                                                         WPE_COMP_CAMERA,
                                                         WPE_COMP_LIGHT,
                                                         WPE_COMP_ANIMATOR,
                                                         WPE_COMP_PHYSICS_RIGIDBODY,
                                                         WPE_COMP_PHYSICS_COLLIDER,
                                                         WPE_COMP_AUDIO_SOURCE,
                                                         WPE_COMP_UI,
                                                         WPE_COMP_SPRITE,
                                                         WPE_COMP_TEXT,
                                                         WPE_COMP_BUTTON,
                                                         WPE_COMP_SCRIPT};
static_assert(sizeof wpe__component_keys / sizeof wpe__component_keys[0] ==
                  sizeof wpe__component_types / sizeof wpe__component_types[0],
              "one type per component key");

static WPE_Result wpe__cmd_list_entities(wpe__Call* call) {
    WPE_Ecs* ecs = wpe__ecs(call);
    wpe_json_write_array_begin(call->w, false);
    WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){0});
    while (wpe_ecs_iter_next(&it)) {
        if (wpe_editor_is_helper(call->editor, it.entity)) continue;
        wpe_json_write_object_begin(call->w);
        wpe_json_write_key(call->w, "id");
        wpe_json_write_integer(call->w, it.entity);
        const char* name = wpe_scene_get_name(ecs, it.entity);
        wpe_json_write_key(call->w, "name");
        if (name) {
            wpe_json_write_string(call->w, name);
        } else {
            wpe_json_write_null(call->w);
        }
        const WPE_Entity parent = wpe_scene_get_parent(ecs, it.entity);
        wpe_json_write_key(call->w, "parent");
        if (parent) {
            wpe_json_write_integer(call->w, parent);
        } else {
            wpe_json_write_null(call->w);
        }
        wpe_json_write_key(call->w, "components");
        wpe_json_write_array_begin(call->w, true);
        for (size_t i = 0; i < sizeof wpe__component_keys / sizeof wpe__component_keys[0]; ++i) {
            if (wpe_ecs_has_component(ecs, it.entity, wpe__component_types[i]))
                wpe_json_write_string(call->w, wpe__component_keys[i]);
        }
        wpe_json_write_array_end(call->w);
        const WPE_Entity root = wpe_prefab_root(ecs, it.entity);
        if (root != WPE_ENTITY_NULL) {
            /* Part of a prefab instance: which prefab, and the instance's root. */
            wpe_json_write_key(call->w, "prefab");
            wpe_json_write_object_begin(call->w);
            wpe_json_write_key(call->w, "path");
            wpe_json_write_string(call->w, wpe_prefab_path(ecs, it.entity));
            wpe_json_write_key(call->w, "root");
            wpe_json_write_integer(call->w, root);
            wpe_json_write_object_end(call->w);
        }
        wpe_json_write_object_end(call->w);
    }
    wpe_json_write_array_end(call->w);
    return WPE_RESULT_OK;
}

static WPE_Result wpe__cmd_get_entity(wpe__Call* call) {
    WPE_Entity e = WPE_ENTITY_NULL;
    const WPE_Result result = wpe__entity_arg(call, "id", &e);
    if (result != WPE_RESULT_OK) return wpe__null(call, result);
    const WPE_SceneContext context = wpe__editor_context(call->editor);
    return wpe_scene_save_entity(&context, e, call->w);
}

static WPE_Result wpe__cmd_get_environment(wpe__Call* call) {
    return wpe_scene_write_environment(wpe_engine_environment(call->editor->engine), call->w);
}

static WPE_Result wpe__cmd_project(wpe__Call* call) {
    return wpe_project_write(wpe_engine_project(call->editor->engine), call->w);
}

static WPE_Result wpe__cmd_status(wpe__Call* call) {
    WPE_Editor* editor = call->editor;
    static const char* const modes[] = {"edit", "play", "paused"};
    WPE_JsonWriter* w = call->w;
    wpe_json_write_object_begin(w);
    wpe_json_write_key(w, "project");
    wpe_json_write_string(w, editor->project_dir);
    wpe_json_write_key(w, "scene");
    wpe_json_write_string(w, editor->scene_path);
    wpe_json_write_key(w, "mode");
    wpe_json_write_string(w, modes[editor->mode]);
    wpe_json_write_key(w, "dirty");
    wpe_json_write_bool(w, editor->dirty);
    wpe_json_write_key(w, "selection");
    const WPE_Entity selection = wpe_editor_selection(editor);
    if (selection) {
        wpe_json_write_integer(w, selection);
    } else {
        wpe_json_write_null(w);
    }
    wpe_json_write_key(w, "entities");
    wpe_json_write_integer(w, (int64_t)wpe_ecs_entity_count(wpe__ecs(call)) - (editor->camera ? 1 : 0));
    wpe_json_write_key(w, "undo");
    wpe_json_write_integer(w, editor->undo_count);
    wpe_json_write_key(w, "redo");
    wpe_json_write_integer(w, editor->redo_count);
    wpe_json_write_object_end(w);
    return WPE_RESULT_OK;
}

typedef struct wpe__AssetList {
    WPE_JsonWriter* w;
    const char* prefix;
} wpe__AssetList;

static bool wpe__list_asset(const char* path, void* user_data) {
    const wpe__AssetList* list = user_data;
    if (strncmp(path, list->prefix, strlen(list->prefix)) == 0) wpe_json_write_string(list->w, path);
    return true;
}

static WPE_Result wpe__cmd_list_assets(wpe__Call* call) {
    const char* prefix = wpe__string_arg(call, "prefix");
    wpe__AssetList list = {.w = call->w, .prefix = prefix ? prefix : ""};
    wpe_json_write_array_begin(call->w, false);
    const WPE_Result result = wpe_vfs_list(wpe_engine_vfs(call->editor->engine), wpe__list_asset, &list);
    wpe_json_write_array_end(call->w);
    return result;
}

/* ---- Edits ------------------------------------------------------------------------------------------ */

static WPE_Result wpe__cmd_create_entity(wpe__Call* call) {
    const WPE_JsonValue* description = wpe_json_get(call->args, "entity");
    if (!description)
        return wpe__null(call, wpe__fail(call, WPE_RESULT_ERROR_INVALID_ARG,
                                         "\"entity\" (a scene entity object) is required"));
    WPE_Result result = wpe__editor_begin_change(call->editor);
    WPE_Entity e = WPE_ENTITY_NULL;
    const WPE_SceneContext context = wpe__editor_context(call->editor);
    if (result == WPE_RESULT_OK) result = wpe_scene_create_entity(&context, description, &e);
    if (result != WPE_RESULT_OK) return wpe__null(call, result);
    call->editor->selection = e;
    wpe_json_write_integer(call->w, e);
    return WPE_RESULT_OK;
}

static WPE_Result wpe__cmd_duplicate_entity(wpe__Call* call) {
    WPE_Entity source = WPE_ENTITY_NULL;
    WPE_Result result = wpe__entity_arg(call, "id", &source);
    if (result != WPE_RESULT_OK) return wpe__null(call, result);
    /* The saved entity is a valid creation description (its "id" is ignored); children are not copied. */
    const WPE_SceneContext context = wpe__editor_context(call->editor);
    WPE_JsonWriter* writer = NULL;
    WPE_JsonDocument* doc = NULL;
    result = wpe_json_writer_create(&writer);
    if (result == WPE_RESULT_OK) result = wpe_scene_save_entity(&context, source, writer);
    const char* text = NULL;
    size_t size = 0;
    if (result == WPE_RESULT_OK) result = wpe_json_writer_finish(writer, &text, &size);
    if (result == WPE_RESULT_OK) result = wpe_json_parse(text, size, &doc, NULL);
    WPE_Entity e = WPE_ENTITY_NULL;
    if (result == WPE_RESULT_OK) result = wpe__editor_begin_change(call->editor);
    if (result == WPE_RESULT_OK) result = wpe_scene_create_entity(&context, wpe_json_root(doc), &e);
    wpe_json_free(doc);
    wpe_json_writer_destroy(writer);
    if (result != WPE_RESULT_OK) return wpe__null(call, result);
    call->editor->selection = e;
    wpe_json_write_integer(call->w, e);
    return WPE_RESULT_OK;
}
static WPE_Result wpe__cmd_delete_entity(wpe__Call* call) {
    WPE_Entity e = WPE_ENTITY_NULL;
    WPE_Result result = wpe__entity_arg(call, "id", &e);
    if (result == WPE_RESULT_OK) result = wpe__editor_begin_change(call->editor);
    if (result == WPE_RESULT_OK) result = wpe_scene_destroy_recursive(wpe__ecs(call), e);
    return wpe__null(call, result);
}

static WPE_Result wpe__cmd_set_component(wpe__Call* call) {
    WPE_Entity e = WPE_ENTITY_NULL;
    WPE_Result result = wpe__entity_arg(call, "id", &e);
    const char* component = wpe__string_arg(call, "component");
    const WPE_JsonValue* value = wpe_json_get(call->args, "value");
    if (result == WPE_RESULT_OK && (!component || !value))
        result = wpe__fail(call, WPE_RESULT_ERROR_INVALID_ARG, "\"component\" and \"value\" are required");
    if (result == WPE_RESULT_OK) result = wpe__editor_begin_change(call->editor);
    const WPE_SceneContext context = wpe__editor_context(call->editor);
    if (result == WPE_RESULT_OK) result = wpe_scene_set_component(&context, e, component, value);
    if (result != WPE_RESULT_OK) return wpe__null(call, result);
    return wpe_scene_save_entity(&context, e, call->w); /* The entity as it is now. */
}

static WPE_Result wpe__cmd_remove_component(wpe__Call* call) {
    WPE_Entity e = WPE_ENTITY_NULL;
    WPE_Result result = wpe__entity_arg(call, "id", &e);
    const char* component = wpe__string_arg(call, "component");
    if (result == WPE_RESULT_OK && !component)
        result = wpe__fail(call, WPE_RESULT_ERROR_INVALID_ARG, "\"component\" is required");
    if (result == WPE_RESULT_OK) result = wpe__editor_begin_change(call->editor);
    const WPE_SceneContext context = wpe__editor_context(call->editor);
    if (result == WPE_RESULT_OK) result = wpe_scene_remove_component(&context, e, component);
    return wpe__null(call, result);
}

static WPE_Result wpe__cmd_set_parent(wpe__Call* call) {
    WPE_Entity e = WPE_ENTITY_NULL, parent = WPE_ENTITY_NULL;
    WPE_Result result = wpe__entity_arg(call, "id", &e);
    const WPE_JsonValue* parent_value = wpe_json_get(call->args, "parent");
    if (result == WPE_RESULT_OK && wpe_json_type(parent_value) != WPE_JSON_NULL && parent_value)
        result = wpe__entity_arg(call, "parent", &parent);
    const bool keep_world = wpe_json_bool(wpe_json_get(call->args, "keep_world"), true);
    const WPE_Entity root = result == WPE_RESULT_OK ? wpe_prefab_root(wpe__ecs(call), e) : WPE_ENTITY_NULL;
    if (root != WPE_ENTITY_NULL && root != e)
        result = wpe__fail(call, WPE_RESULT_ERROR_INVALID_STATE,
                           "entity %u is part of a '%s' instance, whose structure comes from the prefab: "
                           "unpack_prefab it to restructure it",
                           e, wpe_prefab_path(wpe__ecs(call), e));
    if (result == WPE_RESULT_OK) result = wpe__editor_begin_change(call->editor);
    if (result == WPE_RESULT_OK) {
        result = wpe_scene_set_parent(wpe__ecs(call), e, parent, keep_world);
        if (result == WPE_RESULT_ERROR_INVALID_ARG)
            wpe__fail(call, result, "that parent would make the hierarchy a cycle");
    }
    return wpe__null(call, result);
}

static WPE_Result wpe__cmd_set_environment(wpe__Call* call) {
    const WPE_JsonValue* value = wpe_json_get(call->args, "value");
    if (!value)
        return wpe__null(call, wpe__fail(call, WPE_RESULT_ERROR_INVALID_ARG, "\"value\" is required"));
    WPE_SceneEnvironment environment = *wpe_engine_environment(call->editor->engine);
    WPE_Result result = wpe_scene_read_environment(value, "set_environment", &environment);
    if (result == WPE_RESULT_OK) result = wpe__editor_begin_change(call->editor);
    if (result != WPE_RESULT_OK) return wpe__null(call, result);
    *wpe_engine_environment(call->editor->engine) = environment;
    wpe_physics_set_gravity(wpe_engine_physics(call->editor->engine), environment.gravity);
    return wpe_scene_write_environment(&environment, call->w);
}

static const char* const wpe__gizmo_names[] = {"move", "rotate", "scale"};

static WPE_Result wpe__write_view(WPE_JsonWriter* w, const WPE_EditorView* view) {
    wpe_json_write_object_begin(w);
    wpe_json_write_key(w, "grid");
    wpe_json_write_bool(w, view->grid);
    wpe_json_write_key(w, "grid_spacing");
    wpe_json_write_number(w, view->grid_spacing);
    wpe_json_write_key(w, "physics");
    wpe_json_write_bool(w, view->physics);
    wpe_json_write_key(w, "lights");
    wpe_json_write_bool(w, view->lights);
    wpe_json_write_key(w, "gizmo");
    wpe_json_write_string(w, wpe__gizmo_names[view->gizmo]);
    wpe_json_write_key(w, "gizmo_space");
    wpe_json_write_string(w, view->gizmo_local ? "local" : "world");
    wpe_json_write_key(w, "hot_reload");
    wpe_json_write_bool(w, view->hot_reload);
    wpe_json_write_key(w, "snap");
    wpe_json_write_bool(w, view->snap);
    wpe_json_write_key(w, "snap_move");
    wpe_json_write_number(w, view->snap_move);
    wpe_json_write_key(w, "snap_rotate");
    wpe_json_write_number(w, view->snap_rotate);
    wpe_json_write_key(w, "snap_scale");
    wpe_json_write_number(w, view->snap_scale);
    wpe_json_write_object_end(w);
    return WPE_RESULT_OK;
}

static WPE_Result wpe__cmd_get_view(wpe__Call* call) {
    return wpe__write_view(call->w, &call->editor->view);
}

/* Reads one set_view key into `view`. */
static WPE_Result wpe__read_view_key(wpe__Call* call, const char* key, const WPE_JsonValue* v,
                                     WPE_EditorView* view) {
    bool* flag = strcmp(key, "grid") == 0         ? &view->grid
                 : strcmp(key, "physics") == 0    ? &view->physics
                 : strcmp(key, "lights") == 0     ? &view->lights
                 : strcmp(key, "hot_reload") == 0 ? &view->hot_reload
                 : strcmp(key, "snap") == 0       ? &view->snap
                                                  : NULL;
    if (flag) {
        if (wpe_json_type(v) != WPE_JSON_BOOL)
            return wpe__fail(call, WPE_RESULT_ERROR_INVALID_ARG, "\"%s\" must be true or false", key);
        *flag = wpe_json_bool(v, false);
        return WPE_RESULT_OK;
    }
    /* Positive steps and sizes, with their ranges. */
    const struct {
        const char* key;
        float* value;
        double min, max;
    } numbers[] = {{"grid_spacing", &view->grid_spacing, 0.01, 1000.0},
                   {"snap_move", &view->snap_move, 0.001, 1000.0},
                   {"snap_rotate", &view->snap_rotate, 0.1, 180.0},
                   {"snap_scale", &view->snap_scale, 0.001, 100.0}};
    for (size_t i = 0; i < sizeof numbers / sizeof numbers[0]; ++i) {
        if (strcmp(key, numbers[i].key) != 0) continue;
        const double number = wpe_json_number(v, 0.0);
        if (wpe_json_type(v) != WPE_JSON_NUMBER || !(number >= numbers[i].min && number <= numbers[i].max))
            return wpe__fail(call, WPE_RESULT_ERROR_INVALID_ARG, "\"%s\" must be %g..%g", key, numbers[i].min,
                             numbers[i].max);
        *numbers[i].value = (float)number;
        return WPE_RESULT_OK;
    }
    const char* text = wpe_json_string(v);
    if (strcmp(key, "gizmo") == 0) {
        for (int i = 0; text && i < 3; ++i) {
            if (strcmp(text, wpe__gizmo_names[i]) != 0) continue;
            view->gizmo = (WPE_EditorGizmo)i;
            return WPE_RESULT_OK;
        }
        return wpe__fail(call, WPE_RESULT_ERROR_INVALID_ARG,
                         "\"gizmo\" is \"move\", \"rotate\" or \"scale\"");
    }
    if (strcmp(key, "gizmo_space") == 0) {
        if (text && (strcmp(text, "world") == 0 || strcmp(text, "local") == 0)) {
            view->gizmo_local = strcmp(text, "local") == 0;
            return WPE_RESULT_OK;
        }
        return wpe__fail(call, WPE_RESULT_ERROR_INVALID_ARG, "\"gizmo_space\" is \"world\" or \"local\"");
    }
    return wpe__fail(call, WPE_RESULT_ERROR_INVALID_ARG,
                     "unknown view setting \"%s\" (grid, grid_spacing, physics, lights, gizmo, gizmo_space, "
                     "hot_reload, snap, "
                     "snap_move, snap_rotate, snap_scale)",
                     key);
}

static WPE_Result wpe__cmd_set_view(wpe__Call* call) {
    const WPE_JsonValue* value = wpe_json_get(call->args, "value");
    if (wpe_json_type(value) != WPE_JSON_OBJECT)
        return wpe__null(call, wpe__fail(call, WPE_RESULT_ERROR_INVALID_ARG, "\"value\" must be an object"));
    /* All or nothing: a bad key leaves the view unchanged. */
    WPE_EditorView view = call->editor->view;
    for (uint32_t i = 0; i < wpe_json_count(value); ++i) {
        const char* key = NULL;
        const WPE_JsonValue* member = wpe_json_member(value, i, &key);
        const WPE_Result result = wpe__read_view_key(call, key, member, &view);
        if (result != WPE_RESULT_OK) return wpe__null(call, result);
    }
    call->editor->view = view;
    return wpe__write_view(call->w, &view);
}

static WPE_Result wpe__cmd_select(wpe__Call* call) {
    const WPE_JsonValue* id = wpe_json_get(call->args, "id");
    WPE_Entity e = WPE_ENTITY_NULL;
    WPE_Result result = WPE_RESULT_OK;
    if (id && wpe_json_type(id) != WPE_JSON_NULL) result = wpe__entity_arg(call, "id", &e);
    if (result == WPE_RESULT_OK) call->editor->selection = e;
    return wpe__null(call, result);
}

/* ---- Scenes, history, modes ------------------------------------------------------------------------ */

/* Unsaved changes are only ever lost on purpose: commands that replace the scene need "discard": true. */
static WPE_Result wpe__guard_unsaved(wpe__Call* call, const char* command) {
    if (!wpe_editor_dirty(call->editor) || wpe_json_bool(wpe_json_get(call->args, "discard"), false))
        return WPE_RESULT_OK;
    return wpe__fail(call, WPE_RESULT_ERROR_INVALID_STATE,
                     "the scene has unsaved changes: save_scene first, or %s with \"discard\": true",
                     command);
}

static WPE_Result wpe__cmd_new_scene(wpe__Call* call) {
    WPE_Result result = wpe__guard_unsaved(call, "new_scene");
    if (result == WPE_RESULT_OK) result = wpe__editor_new_scene(call->editor);
    return wpe__null(call, result);
}

static WPE_Result wpe__cmd_load_scene(wpe__Call* call) {
    const char* path = wpe__string_arg(call, "path");
    if (!path) return wpe__null(call, wpe__fail(call, WPE_RESULT_ERROR_INVALID_ARG, "\"path\" is required"));
    WPE_Result result = wpe__guard_unsaved(call, "load_scene");
    if (result == WPE_RESULT_OK) result = wpe__editor_load_scene(call->editor, path);
    return wpe__null(call, result);
}

static WPE_Result wpe__cmd_save_scene(wpe__Call* call) {
    return wpe__null(call, wpe__editor_save_scene(call->editor, wpe__string_arg(call, "path")));
}

static WPE_Result wpe__cmd_undo(wpe__Call* call) {
    const WPE_Result result = wpe__editor_undo(call->editor);
    if (result == WPE_RESULT_ERROR_NOT_FOUND)
        return wpe__null(call, wpe__fail(call, result, "nothing to undo"));
    return wpe__null(call, result);
}

static WPE_Result wpe__cmd_redo(wpe__Call* call) {
    const WPE_Result result = wpe__editor_redo(call->editor);
    if (result == WPE_RESULT_ERROR_NOT_FOUND)
        return wpe__null(call, wpe__fail(call, result, "nothing to redo"));
    return wpe__null(call, result);
}

static WPE_Result wpe__cmd_play(wpe__Call* call) {
    return wpe__null(call, wpe__editor_play(call->editor));
}

static WPE_Result wpe__cmd_pause(wpe__Call* call) {
    const WPE_Result result = wpe__editor_pause(call->editor);
    if (result == WPE_RESULT_ERROR_INVALID_STATE)
        return wpe__null(call, wpe__fail(call, result, "not playing"));
    return wpe__null(call, result);
}

static WPE_Result wpe__cmd_stop(wpe__Call* call) {
    return wpe__null(call, wpe__editor_stop(call->editor));
}

static WPE_Result wpe__cmd_frames(wpe__Call* call) {
    const double count = wpe_json_number(wpe_json_get(call->args, "count"), 1.0);
    const double dt = wpe_json_number(wpe_json_get(call->args, "dt"), 1.0 / 60.0);
    if (!(count >= 1.0 && count <= 100000.0 && count == floor(count)) || !(dt > 0.0 && dt <= 0.25))
        return wpe__null(call, wpe__fail(call, WPE_RESULT_ERROR_INVALID_ARG,
                                         "\"count\" must be 1..100000 and \"dt\" in (0, 0.25]"));
    WPE_Result result = WPE_RESULT_OK;
    for (uint32_t i = 0; i < (uint32_t)count && result == WPE_RESULT_OK; ++i)
        result = wpe_editor_frame(call->editor, dt);
    return wpe__null(call, result);
}

static WPE_Result wpe__cmd_screenshot(wpe__Call* call) {
    const char* path = wpe__string_arg(call, "path");
    const char* target = wpe__string_arg(call, "target");
    if (!path)
        return wpe__null(call,
                         wpe__fail(call, WPE_RESULT_ERROR_INVALID_ARG, "\"path\" (an OS path) is required"));
    if (target && strcmp(target, "editor") == 0) {
        if (!call->editor->screenshot)
            return wpe__null(call,
                             wpe__fail(call, WPE_RESULT_ERROR_UNSUPPORTED, "no editor window in this mode"));
        return wpe__null(call, call->editor->screenshot(call->editor->screenshot_user_data, path));
    }
    if (target && strcmp(target, "game") != 0)
        return wpe__null(
            call, wpe__fail(call, WPE_RESULT_ERROR_INVALID_ARG, "\"target\" is \"game\" or \"editor\""));
    /* Render the current state first so the image matches the world. */
    WPE_Result result = wpe_editor_frame(call->editor, 0.0);
    if (result == WPE_RESULT_OK) result = wpe_engine_screenshot(call->editor->engine, path);
    return wpe__null(call, result);
}

static WPE_Result wpe__cmd_camera(wpe__Call* call) {
    WPE_Editor* editor = call->editor;
    WPE_Transform* t = wpe_ecs_get_component(wpe__ecs(call), editor->camera, WPE_COMP_TRANSFORM);
    if (!t)
        return wpe__null(call, wpe__fail(call, WPE_RESULT_ERROR_INVALID_STATE,
                                         "the editor camera exists in edit mode only"));
    float position[3], target[3];
    const WPE_JsonValue* p = wpe_json_get(call->args, "position");
    const WPE_JsonValue* g = wpe_json_get(call->args, "target");
    bool ok = wpe_json_count(p) == 3 && wpe_json_count(g) == 3;
    for (uint32_t i = 0; ok && i < 3; ++i) {
        ok = wpe_json_type(wpe_json_at(p, i)) == WPE_JSON_NUMBER &&
             wpe_json_type(wpe_json_at(g, i)) == WPE_JSON_NUMBER;
        position[i] = (float)wpe_json_number(wpe_json_at(p, i), 0.0);
        target[i] = (float)wpe_json_number(wpe_json_at(g, i), 0.0);
    }
    if (!ok)
        return wpe__null(
            call, wpe__fail(call, WPE_RESULT_ERROR_INVALID_ARG, "\"position\" and \"target\" are [x, y, z]"));
    const WPE_Vec3 eye = wpe_vec3(position[0], position[1], position[2]);
    const WPE_Vec3 at = wpe_vec3(target[0], target[1], target[2]);
    const WPE_Vec3 forward = wpe_vec3_sub(at, eye);
    const WPE_Vec3 up =
        fabsf(forward.y) > 0.999f * wpe_vec3_length(forward) ? wpe_vec3(0, 0, -1) : wpe_vec3(0, 1, 0);
    const WPE_Mat4 view = wpe_mat4_look_at(eye, at, up);
    WPE_Mat4 world;
    WPE_Vec3 scale;
    if (wpe_vec3_length(forward) < 1e-6f || !wpe_mat4_inverse(&view, &world))
        return wpe__null(call,
                         wpe__fail(call, WPE_RESULT_ERROR_INVALID_ARG, "position and target must differ"));
    wpe_mat4_decompose(&world, &t->position, &t->rotation, &scale);
    return wpe__null(call, WPE_RESULT_OK);
}

/* ---- Prefabs ---------------------------------------------------------------------------------------- */

/* Whether another instance of the prefab `path` exists (besides the one rooted at `except`). */
static bool wpe__other_instances(WPE_Ecs* ecs, WPE_Entity except, const char* path) {
    WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_PREFAB)});
    while (wpe_ecs_iter_next(&it)) {
        if (it.entity != except && wpe_prefab_root(ecs, it.entity) == it.entity &&
            strcmp(wpe_prefab_path(ecs, it.entity), path) == 0)
            return true;
    }
    return false;
}

/* Writes `root`'s tree as the prefab `path` (and links it); other instances of the prefab are rebuilt
 * from the new file by reloading the scene, which changes entity ids. */
static WPE_Result wpe__write_prefab(wpe__Call* call, WPE_Entity root, const char* path) {
    WPE_Editor* editor = call->editor;
    char os_path[2048], folder[2048];
    if (!wpe_vfs_path_is_valid(path) || !wpe_prefab_is_path(path) ||
        snprintf(os_path, sizeof os_path, "%s/%s", editor->project_dir, path) >= (int)sizeof os_path)
        return wpe__null(call, wpe__fail(call, WPE_RESULT_ERROR_INVALID_ARG,
                                         "\"path\" must be an asset path ending in \".prefab\""));
    memcpy(folder, os_path, sizeof folder);
    char* slash = strrchr(folder, '/');
    if (slash) *slash = '\0';
    WPE_Result result = wpe__editor_begin_change(editor);
    if (result != WPE_RESULT_OK) return wpe__null(call, result);
    result = wpe_directory_create(folder);
    const WPE_SceneContext context = wpe__editor_context(editor);
    if (result == WPE_RESULT_OK) result = wpe_prefab_save_file(&context, root, path, os_path);
    if (result != WPE_RESULT_OK) {
        wpe__editor_abort_change(editor, true);
        return wpe__null(call, result);
    }
    const bool reload = wpe__other_instances(wpe__ecs(call), root, path);
    if (reload) {
        wpe__Snapshot snapshot;
        result = wpe__editor_snapshot(editor, &snapshot);
        if (result == WPE_RESULT_OK) result = wpe__editor_restore(editor, &snapshot, "prefab update");
        wpe_free(snapshot.text);
        if (result != WPE_RESULT_OK) return wpe__null(call, result);
    } else {
        editor->selection = root;
    }
    wpe_log_info("saved prefab %s", path);
    wpe_json_write_object_begin(call->w);
    wpe_json_write_key(call->w, "path");
    wpe_json_write_string(call->w, path);
    wpe_json_write_key(call->w, "reloaded"); /* Other instances were rebuilt: entity ids changed. */
    wpe_json_write_bool(call->w, reload);
    wpe_json_write_object_end(call->w);
    return WPE_RESULT_OK;
}

static WPE_Result wpe__cmd_save_prefab(wpe__Call* call) {
    WPE_Entity e = WPE_ENTITY_NULL;
    const WPE_Result result = wpe__entity_arg(call, "id", &e);
    if (result != WPE_RESULT_OK) return wpe__null(call, result);
    const char* path = wpe__string_arg(call, "path");
    if (!path)
        return wpe__null(call, wpe__fail(call, WPE_RESULT_ERROR_INVALID_ARG,
                                         "\"path\" (e.g. prefabs/crate.prefab) is required"));
    return wpe__write_prefab(call, e, path);
}

/* The instance root of the "id" argument's entity. */
static WPE_Result wpe__instance_arg(wpe__Call* call, WPE_Entity* out_member, WPE_Entity* out_root) {
    WPE_Result result = wpe__entity_arg(call, "id", out_member);
    if (result != WPE_RESULT_OK) return result;
    *out_root = wpe_prefab_root(wpe__ecs(call), *out_member);
    if (*out_root == WPE_ENTITY_NULL)
        return wpe__fail(call, WPE_RESULT_ERROR_NOT_FOUND, "entity %u is not part of a prefab instance",
                         *out_member);
    return WPE_RESULT_OK;
}

static WPE_Result wpe__cmd_apply_prefab(wpe__Call* call) {
    WPE_Entity member = WPE_ENTITY_NULL, root = WPE_ENTITY_NULL;
    const WPE_Result result = wpe__instance_arg(call, &member, &root);
    if (result != WPE_RESULT_OK) return wpe__null(call, result);
    char path[WPE_VFS_PATH_MAX];
    snprintf(path, sizeof path, "%s", wpe_prefab_path(wpe__ecs(call), root));
    return wpe__write_prefab(call, root, path);
}

static WPE_Result wpe__cmd_revert_prefab(wpe__Call* call) {
    WPE_Entity member = WPE_ENTITY_NULL, root = WPE_ENTITY_NULL;
    WPE_Result result = wpe__instance_arg(call, &member, &root);
    const char* component = wpe__string_arg(call, "component");
    if (result == WPE_RESULT_OK && wpe_json_get(call->args, "component") && !component)
        result = wpe__fail(call, WPE_RESULT_ERROR_INVALID_ARG, "\"component\" must be a component key");
    if (result == WPE_RESULT_OK) result = wpe__editor_begin_change(call->editor);
    if (result != WPE_RESULT_OK) return wpe__null(call, result);
    const WPE_SceneContext context = wpe__editor_context(call->editor);
    result = wpe_prefab_revert(&context, member, component, &root);
    if (result != WPE_RESULT_OK) {
        wpe__editor_abort_change(call->editor, true);
        return wpe__null(call, result);
    }
    if (!component) call->editor->selection = root; /* The instance was rebuilt: a new root. */
    wpe_json_write_integer(call->w, root);
    return WPE_RESULT_OK;
}

static WPE_Result wpe__cmd_unpack_prefab(wpe__Call* call) {
    WPE_Entity member = WPE_ENTITY_NULL, root = WPE_ENTITY_NULL;
    WPE_Result result = wpe__instance_arg(call, &member, &root);
    if (result == WPE_RESULT_OK) result = wpe__editor_begin_change(call->editor);
    if (result == WPE_RESULT_OK) result = wpe_prefab_unpack(wpe__ecs(call), member);
    return wpe__null(call, result);
}

/* ---- Camera ---------------------------------------------------------------------------------------- */

static void wpe__grow(WPE_Vec3* lo, WPE_Vec3* hi, WPE_Vec3 p) {
    for (int i = 0; i < 3; ++i) {
        lo->v[i] = p.v[i] < lo->v[i] ? p.v[i] : lo->v[i];
        hi->v[i] = p.v[i] > hi->v[i] ? p.v[i] : hi->v[i];
    }
}

/* World-space bounds of the meshes of an entity and its descendants; false when there are none. */
static bool wpe__subtree_bounds(WPE_Ecs* ecs, WPE_Entity root, WPE_Vec3* out_lo, WPE_Vec3* out_hi) {
    WPE_Entity* stack = wpe_alloc((size_t)wpe_ecs_entity_count(ecs) * sizeof *stack);
    if (!stack) return false;
    WPE_Vec3 lo = wpe_vec3(1e30f, 1e30f, 1e30f), hi = wpe_vec3(-1e30f, -1e30f, -1e30f);
    bool found = false;
    uint32_t depth = 0;
    stack[depth++] = root;
    while (depth > 0) {
        const WPE_Entity e = stack[--depth];
        const WPE_MeshRenderer* mesh = wpe_ecs_get_component(ecs, e, WPE_COMP_MESH);
        WPE_Mat4 world;
        if (mesh && mesh->model && mesh->mesh < mesh->model->mesh_count &&
            wpe_scene_compute_world_matrix(ecs, e, &world)) {
            const WPE_Aabb* b = &mesh->model->meshes[mesh->mesh].bounds;
            for (int corner = 0; corner < 8; ++corner) {
                const WPE_Vec3 p =
                    wpe_vec3(corner & 1 ? b->max.x : b->min.x, corner & 2 ? b->max.y : b->min.y,
                             corner & 4 ? b->max.z : b->min.z);
                wpe__grow(&lo, &hi, wpe_mat4_transform_point(&world, p));
            }
            found = true;
        }
        const WPE_Hierarchy* h = wpe_ecs_get_component(ecs, e, WPE_COMP_HIERARCHY);
        for (WPE_Entity c = h ? h->first_child : WPE_ENTITY_NULL; c != WPE_ENTITY_NULL;) {
            const WPE_Hierarchy* ch = wpe_ecs_get_component(ecs, c, WPE_COMP_HIERARCHY);
            if (depth < wpe_ecs_entity_count(ecs)) stack[depth++] = c;
            c = ch ? ch->next_sibling : WPE_ENTITY_NULL;
        }
    }
    wpe_free(stack);
    *out_lo = lo;
    *out_hi = hi;
    return found;
}

static WPE_Result wpe__cmd_focus(wpe__Call* call) {
    WPE_Editor* editor = call->editor;
    WPE_Ecs* ecs = wpe__ecs(call);
    WPE_Entity e = wpe_editor_selection(editor);
    if (wpe_json_get(call->args, "id")) {
        const WPE_Result result = wpe__entity_arg(call, "id", &e);
        if (result != WPE_RESULT_OK) return wpe__null(call, result);
    }
    if (e == WPE_ENTITY_NULL)
        return wpe__null(call,
                         wpe__fail(call, WPE_RESULT_ERROR_INVALID_ARG, "select an entity or give \"id\""));
    WPE_Transform* t = wpe_ecs_get_component(ecs, editor->camera, WPE_COMP_TRANSFORM);
    if (!t)
        return wpe__null(call, wpe__fail(call, WPE_RESULT_ERROR_INVALID_STATE,
                                         "the editor camera exists in edit mode only"));
    /* A sphere around the entity's meshes (or a unit sphere at its origin), seen whole. */
    WPE_Vec3 lo, hi, center;
    float radius = 1.0f;
    if (wpe__subtree_bounds(ecs, e, &lo, &hi)) {
        center = wpe_vec3_scale(wpe_vec3_add(lo, hi), 0.5f);
        radius = wpe_vec3_length(wpe_vec3_sub(hi, lo)) * 0.5f;
        radius = radius > 0.25f ? radius : 0.25f;
    } else {
        WPE_Mat4 world;
        center = wpe_scene_compute_world_matrix(ecs, e, &world)
                     ? wpe_mat4_transform_point(&world, wpe_vec3(0, 0, 0))
                     : wpe_vec3(0, 0, 0);
    }
    const WPE_Camera* camera = wpe_ecs_get_component(ecs, editor->camera, WPE_COMP_CAMERA);
    const float fov = camera && camera->fov_y > 0.0f ? camera->fov_y : 1.0f;
    const float distance = radius / sinf(fov * 0.5f) * 1.1f;
    const WPE_Vec3 forward = wpe_quat_rotate_vec3(t->rotation, wpe_vec3(0, 0, -1));
    t->position = wpe_vec3_sub(center, wpe_vec3_scale(forward, distance));
    wpe_json_write_object_begin(call->w);
    const struct {
        const char* key;
        WPE_Vec3 v;
    } parts[] = {{"position", t->position}, {"target", center}};
    for (size_t p = 0; p < 2; ++p) {
        wpe_json_write_key(call->w, parts[p].key);
        wpe_json_write_array_begin(call->w, true);
        for (int i = 0; i < 3; ++i)
            wpe_json_write_number(call->w, parts[p].v.v[i]);
        wpe_json_write_array_end(call->w);
    }
    wpe_json_write_object_end(call->w);
    return WPE_RESULT_OK;
}

static WPE_Result wpe__cmd_run_lua(wpe__Call* call) {
    const char* code = wpe__string_arg(call, "code");
    if (!code) return wpe__null(call, wpe__fail(call, WPE_RESULT_ERROR_INVALID_ARG, "\"code\" is required"));
    WPE_Result result = wpe__editor_begin_change(call->editor); /* Scripts may change the world. */
    if (result != WPE_RESULT_OK) return wpe__null(call, result);
    result = wpe_script_host_run(wpe_engine_scripts(call->editor->engine), "run_lua", code);
    /* A failing chunk may have changed part of the world: put all of it back. */
    if (result != WPE_RESULT_OK) wpe__editor_abort_change(call->editor, true);
    return wpe__null(call, result);
}

static WPE_Result wpe__cmd_export(wpe__Call* call) {
    const char* output = wpe__string_arg(call, "output");
    const char* runtime = wpe__string_arg(call, "runtime");
    if (!output)
        return wpe__null(
            call, wpe__fail(call, WPE_RESULT_ERROR_INVALID_ARG, "\"output\" (an OS path) is required"));
    char default_runtime[4096];
    if (!runtime) {
        /* The runtime installed next to the editor. */
        if (wpe_platform_executable_path(default_runtime, sizeof default_runtime) != WPE_RESULT_OK)
            return wpe__null(call, wpe__fail(call, WPE_RESULT_ERROR_NOT_FOUND, "give \"runtime\""));
        char* slash = strrchr(default_runtime, '/');
        char* backslash = strrchr(default_runtime, '\\');
        if (backslash && (!slash || backslash > slash)) slash = backslash;
        const size_t directory = slash ? (size_t)(slash - default_runtime) + 1 : 0;
        snprintf(default_runtime + directory, sizeof default_runtime - directory, "wpe_runtime%s",
                 WPE_EXE_SUFFIX);
        runtime = default_runtime;
    }
    WPE_ExportStats stats;
    const WPE_Result result = wpe_export_game(&(WPE_ExportDesc){.project_dir = call->editor->project_dir,
                                                                .runtime_path = runtime,
                                                                .output_path = output},
                                              &stats);
    if (result != WPE_RESULT_OK) return wpe__null(call, result);
    wpe_json_write_object_begin(call->w);
    wpe_json_write_key(call->w, "files");
    wpe_json_write_integer(call->w, stats.files);
    wpe_json_write_key(call->w, "bytes");
    wpe_json_write_integer(call->w, (int64_t)stats.output_bytes);
    wpe_json_write_object_end(call->w);
    return WPE_RESULT_OK;
}

static void wpe__write_log_line(const wpe__LogLine* line, void* user_data) {
    wpe_json_write_string(user_data, line->text);
}

static void wpe__write_warning(const wpe__LogLine* line, void* user_data) {
    if (line->level >= WPE_LOG_LEVEL_WARN) wpe_json_write_string(user_data, line->text);
}

static WPE_Result wpe__cmd_log(wpe__Call* call) {
    const double count = wpe_json_number(wpe_json_get(call->args, "count"), 50.0);
    wpe_json_write_array_begin(call->w, false);
    wpe__editor_log_since(call->editor, 0,
                          count >= 1.0 && count <= WPE__EDITOR_LOG_LINES ? (uint32_t)count : 50,
                          wpe__write_log_line, call->w);
    wpe_json_write_array_end(call->w);
    return WPE_RESULT_OK;
}

static WPE_Result wpe__cmd_quit(wpe__Call* call) {
    const WPE_Result result = wpe__guard_unsaved(call, "quit");
    if (result == WPE_RESULT_OK) call->editor->quit_requested = true;
    return wpe__null(call, result);
}

static WPE_Result wpe__cmd_help(wpe__Call* call);

/* ---- Dispatch --------------------------------------------------------------------------------------- */

static const struct {
    const char* name;
    WPE_Result (*fn)(wpe__Call* call);
    const char* usage;
} wpe__commands[] = {
    {"help", wpe__cmd_help, "{} -> the command list"},
    {"status", wpe__cmd_status, "{} -> project, scene, mode, dirty, selection, counts"},
    {"project", wpe__cmd_project, "{} -> project.json settings"},
    {"list_entities", wpe__cmd_list_entities, "{} -> [{id, name, parent, components}]"},
    {"get_entity", wpe__cmd_get_entity, "{id} -> the entity in the scene-file schema (ids are handles)"},
    {"create_entity", wpe__cmd_create_entity, "{entity: {name, parent, transform, mesh, ...}} -> id"},
    {"duplicate_entity", wpe__cmd_duplicate_entity, "{id} -> id of the copy (without children)"},
    {"delete_entity", wpe__cmd_delete_entity, "{id} (with its children)"},
    {"set_component", wpe__cmd_set_component,
     "{id, component, value} -> the entity (replaces the component)"},
    {"remove_component", wpe__cmd_remove_component, "{id, component}"},
    {"set_parent", wpe__cmd_set_parent, "{id, parent: id|null, keep_world = true}"},
    {"save_prefab", wpe__cmd_save_prefab,
     "{id, path}: save the entity and its descendants as a prefab file; they become an instance of it"},
    {"apply_prefab", wpe__cmd_apply_prefab,
     "{id}: write the instance (any member's id) into its prefab -> {path, reloaded}"},
    {"revert_prefab", wpe__cmd_revert_prefab,
     "{id, component?}: revert one component of a member, or (no component) rebuild the instance -> root id"},
    {"unpack_prefab", wpe__cmd_unpack_prefab, "{id}: make the instance plain entities"},
    {"select", wpe__cmd_select, "{id|null}"},
    {"get_environment", wpe__cmd_get_environment, "{} -> the scene environment"},
    {"set_environment", wpe__cmd_set_environment, "{value: {...}} (merged) -> the environment"},
    {"get_view", wpe__cmd_get_view,
     "{} -> {grid, grid_spacing, physics, lights, gizmo, gizmo_space, hot_reload, snap, snap_move, "
     "snap_rotate, "
     "snap_scale}"},
    {"set_view", wpe__cmd_set_view, "{value: {...}} (merged; not saved or undone) -> the view"},
    {"new_scene", wpe__cmd_new_scene, "{discard = false} (refused with unsaved changes unless discard)"},
    {"load_scene", wpe__cmd_load_scene,
     "{path, discard = false} (refused with unsaved changes unless discard)"},
    {"save_scene", wpe__cmd_save_scene, "{path?} (asset path; default: the open scene)"},
    {"undo", wpe__cmd_undo, "{} (entity ids change)"},
    {"redo", wpe__cmd_redo, "{} (entity ids change)"},
    {"play", wpe__cmd_play, "{} (also resumes)"},
    {"pause", wpe__cmd_pause, "{}"},
    {"stop", wpe__cmd_stop, "{} (restores the scene as it was before play)"},
    {"frames", wpe__cmd_frames, "{count = 1, dt = 1/60}: run frames"},
    {"screenshot", wpe__cmd_screenshot, "{path, target = \"game\"|\"editor\"}"},
    {"camera", wpe__cmd_camera, "{position: [x,y,z], target: [x,y,z]}: place the editor camera"},
    {"focus", wpe__cmd_focus, "{id = selection}: frame the entity and its children -> {position, target}"},
    {"run_lua", wpe__cmd_run_lua, "{code}: run Luau with the wpe API (undoable)"},
    {"list_assets", wpe__cmd_list_assets, "{prefix?} -> asset paths"},
    {"export", wpe__cmd_export, "{output, runtime?} -> {files, bytes}"},
    {"log", wpe__cmd_log, "{count = 50} -> recent log lines"},
    {"quit", wpe__cmd_quit,
     "{discard = false} close the editor window (refused with unsaved changes unless discard)"},
};

static WPE_Result wpe__cmd_help(wpe__Call* call) {
    wpe_json_write_object_begin(call->w);
    for (size_t i = 0; i < sizeof wpe__commands / sizeof wpe__commands[0]; ++i) {
        wpe_json_write_key(call->w, wpe__commands[i].name);
        wpe_json_write_string(call->w, wpe__commands[i].usage);
    }
    wpe_json_write_object_end(call->w);
    return WPE_RESULT_OK;
}

WPE_Result wpe_editor_execute_value(WPE_Editor* editor, const WPE_JsonValue* command, WPE_JsonWriter* reply) {
    if (!editor || !command || !reply) return WPE_RESULT_ERROR_NULL_ARG;
    const uint64_t log_start = wpe__editor_log_position(editor);
    wpe__Call call = {.editor = editor, .args = command, .w = reply};
    const char* name = wpe_json_string(wpe_json_get(command, "cmd"));
    wpe_json_write_object_begin(reply);
    wpe_json_write_key(reply, "result");
    WPE_Result result = WPE_RESULT_ERROR_INVALID_ARG;
    bool found = false;
    const uint64_t changes_before = editor->changes;
    /* {"coalesce": key}: edits with the same key in a row form one undo step (e.g. a slider drag). */
    editor->coalesce_next = wpe_json_string(wpe_json_get(command, "coalesce"));
    if (!editor->coalesce_next) editor->last_coalesce[0] = '\0';
    for (size_t i = 0; name && i < sizeof wpe__commands / sizeof wpe__commands[0] && !found; ++i) {
        if (strcmp(wpe__commands[i].name, name) != 0) continue;
        found = true;
        result = wpe__commands[i].fn(&call);
    }
    /* Failed edits leave no undo step: they changed nothing (run_lua restores what it changed itself). */
    if (found && result != WPE_RESULT_OK && editor->changes != changes_before && strcmp(name, "run_lua") != 0)
        wpe__editor_abort_change(editor, false);
    editor->coalesce_next = NULL;
    if (!found) {
        wpe_json_write_null(reply);
        wpe__fail(&call, WPE_RESULT_ERROR_INVALID_ARG,
                  wpe_json_type(command) != WPE_JSON_OBJECT ? "a command is a JSON object"
                  : name                                    ? "unknown command (\"help\" lists them)"
                                                            : "\"cmd\" is required");
    }
    wpe_json_write_key(reply, "ok");
    wpe_json_write_bool(reply, result == WPE_RESULT_OK);
    if (result != WPE_RESULT_OK) {
        wpe_json_write_key(reply, "error");
        wpe_json_write_string(reply, call.error[0] ? call.error : wpe_result_to_string(result));
    }
    wpe_json_write_key(reply, "log");
    wpe_json_write_array_begin(reply, false);
    wpe__editor_log_since(editor, log_start, 64, wpe__write_warning, reply);
    wpe_json_write_array_end(reply);
    wpe_json_write_object_end(reply);
    return result;
}

WPE_Result wpe_editor_execute(WPE_Editor* editor, const char* command, size_t size, WPE_JsonWriter* reply) {
    if (!editor || !command || !reply) return WPE_RESULT_ERROR_NULL_ARG;
    WPE_JsonDocument* doc = NULL;
    WPE_JsonError error;
    const WPE_Result parsed = wpe_json_parse(command, size, &doc, &error);
    if (parsed != WPE_RESULT_OK) {
        wpe_json_write_object_begin(reply);
        wpe_json_write_key(reply, "result");
        wpe_json_write_null(reply);
        wpe_json_write_key(reply, "ok");
        wpe_json_write_bool(reply, false);
        wpe_json_write_key(reply, "error");
        char message[192];
        snprintf(message, sizeof message, "invalid JSON at %u:%u: %s", error.line, error.column,
                 error.message);
        wpe_json_write_string(reply, message);
        wpe_json_write_object_end(reply);
        return parsed;
    }
    const WPE_Result result = wpe_editor_execute_value(editor, wpe_json_root(doc), reply);
    wpe_json_free(doc);
    return result;
}
