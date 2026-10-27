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
    "transform", "mesh", "camera", "light", "animator", "rigidbody", "collider", "audio_source", "script"};
static const WPE_ComponentType wpe__component_types[] = {WPE_COMP_TRANSFORM,
                                                         WPE_COMP_MESH,
                                                         WPE_COMP_CAMERA,
                                                         WPE_COMP_LIGHT,
                                                         WPE_COMP_ANIMATOR,
                                                         WPE_COMP_PHYSICS_RIGIDBODY,
                                                         WPE_COMP_PHYSICS_COLLIDER,
                                                         WPE_COMP_AUDIO_SOURCE,
                                                         WPE_COMP_SCRIPT};

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

static WPE_Result wpe__cmd_select(wpe__Call* call) {
    const WPE_JsonValue* id = wpe_json_get(call->args, "id");
    WPE_Entity e = WPE_ENTITY_NULL;
    WPE_Result result = WPE_RESULT_OK;
    if (id && wpe_json_type(id) != WPE_JSON_NULL) result = wpe__entity_arg(call, "id", &e);
    if (result == WPE_RESULT_OK) call->editor->selection = e;
    return wpe__null(call, result);
}

/* ---- Scenes, history, modes ------------------------------------------------------------------------ */

static WPE_Result wpe__cmd_new_scene(wpe__Call* call) {
    return wpe__null(call, wpe__editor_new_scene(call->editor));
}

static WPE_Result wpe__cmd_load_scene(wpe__Call* call) {
    const char* path = wpe__string_arg(call, "path");
    if (!path) return wpe__null(call, wpe__fail(call, WPE_RESULT_ERROR_INVALID_ARG, "\"path\" is required"));
    return wpe__null(call, wpe__editor_load_scene(call->editor, path));
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
    call->editor->quit_requested = true;
    return wpe__null(call, WPE_RESULT_OK);
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
    {"select", wpe__cmd_select, "{id|null}"},
    {"get_environment", wpe__cmd_get_environment, "{} -> the scene environment"},
    {"set_environment", wpe__cmd_set_environment, "{value: {...}} (merged) -> the environment"},
    {"new_scene", wpe__cmd_new_scene, "{}"},
    {"load_scene", wpe__cmd_load_scene, "{path}"},
    {"save_scene", wpe__cmd_save_scene, "{path?} (asset path; default: the open scene)"},
    {"undo", wpe__cmd_undo, "{} (entity ids change)"},
    {"redo", wpe__cmd_redo, "{} (entity ids change)"},
    {"play", wpe__cmd_play, "{} (also resumes)"},
    {"pause", wpe__cmd_pause, "{}"},
    {"stop", wpe__cmd_stop, "{} (restores the scene as it was before play)"},
    {"frames", wpe__cmd_frames, "{count = 1, dt = 1/60}: run frames"},
    {"screenshot", wpe__cmd_screenshot, "{path, target = \"game\"|\"editor\"}"},
    {"camera", wpe__cmd_camera, "{position: [x,y,z], target: [x,y,z]}: place the editor camera"},
    {"run_lua", wpe__cmd_run_lua, "{code}: run Luau with the wpe API (undoable)"},
    {"list_assets", wpe__cmd_list_assets, "{prefix?} -> asset paths"},
    {"export", wpe__cmd_export, "{output, runtime?} -> {files, bytes}"},
    {"log", wpe__cmd_log, "{count = 50} -> recent log lines"},
    {"quit", wpe__cmd_quit, "{} close the editor window (unsaved changes are lost)"},
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
