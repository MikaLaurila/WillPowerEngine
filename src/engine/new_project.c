/*
 * New projects: the files a game starts from (wpe_project_create, `wpe_editor <dir> --init`).
 */
#define WPE_LOG_MODULE "engine"
#include <stdio.h>
#include <string.h>

#include "core/log.h"
#include "engine/project.h"
#include "engine/scene_file_internal.h"
#include "platform/platform.h"

/* The starter scene: a camera, a sun, a ground and a spinning cube. */
static const char wpe__main_scene[] =
    "{\n"
    "  \"format\": \"wpe.scene\",\n"
    "  \"version\": 1,\n"
    "  \"environment\": {\"clear_color\": [0.38, 0.5, 0.66], \"ambient\": [0.3, 0.32, 0.36]},\n"
    "  \"camera\": 1,\n"
    "  \"entities\": [\n"
    "    {\"id\": 1, \"name\": \"camera\", \"transform\": {\"position\": [0, 3, 8], \"euler\": [-18, 0, "
    "0]},\n"
    "     \"camera\": {}},\n"
    "    {\"id\": 2, \"name\": \"sun\", \"transform\": {\"euler\": [-50, 35, 0]},\n"
    "     \"light\": {\"type\": \"directional\", \"intensity\": 0.9, \"cast_shadows\": true}},\n"
    "    {\"id\": 3, \"name\": \"ground\", \"transform\": {}, \"mesh\": {\"model\": "
    "\"meshes/ground.prim\"},\n"
    "     \"collider\": {\"shape\": \"box\", \"size\": [20, 1, 20], \"offset\": [0, -0.5, 0]}},\n"
    "    {\"id\": 4, \"name\": \"cube\", \"transform\": {\"position\": [0, 1, 0]}, \"mesh\": {\"model\": "
    "\"meshes/cube.prim\"},\n"
    "     \"script\": {\"path\": \"scripts/spin.luau\", \"properties\": {\"speed\": 1}}}\n"
    "  ]\n"
    "}\n";

static const char wpe__ground_prim[] = "{\"shape\": \"plane\", \"size\": 20, \"segments\": 10, \"color\": "
                                       "[0.45, 0.5, 0.45], \"roughness\": 0.9}\n";

static const char wpe__cube_prim[] =
    "{\"shape\": \"box\", \"size\": [1, 1, 1], \"color\": [0.93, 0.6, 0.22], \"roughness\": 0.6}\n";

static const char wpe__spin_script[] =
    "-- Spins its entity around the Y axis.\n"
    "-- Edit this file while the game runs: the editor and `wpe_runtime --watch` reload it.\n"
    "local Spin = {}\n"
    "-- Declared properties: every entity using the script gets them, and the editor shows them.\n"
    "Spin.properties = { speed = 1 } -- radians per second\n"
    "\n"
    "function Spin:on_update(dt)\n"
    "\twpe.rotate(self.entity, vector.create(0, 1, 0), self.speed * dt)\n"
    "end\n"
    "\n"
    "return Spin\n";

static bool wpe__any_file(const char* relative_path, void* user_data) {
    (void)relative_path;
    *(bool*)user_data = true;
    return false;
}

/* An action of keyboard keys (with their scales) plus one pad binding. */
static WPE_InputActionDesc wpe__action(const char* name, const WPE_Key* keys, const float* scales,
                                       uint32_t key_count, WPE_InputBinding pad) {
    WPE_InputActionDesc action = {0};
    snprintf(action.name, sizeof action.name, "%s", name);
    for (uint32_t i = 0; i < key_count; ++i) {
        action.bindings[action.binding_count++] =
            (WPE_InputBinding){.source = WPE_INPUT_SOURCE_KEY, .code = keys[i], .scale = scales[i]};
    }
    action.bindings[action.binding_count++] = pad;
    return action;
}

#define WPE__LETTER(c) (WPE_KEY_A + (WPE_Key)((c) - 'a'))

static WPE_Result wpe__write_text(const char* directory, const char* relative, const char* text) {
    char path[1024];
    if (snprintf(path, sizeof path, "%s/%s", directory, relative) >= (int)sizeof path) {
        wpe_log_error("the project path '%s' is too long", directory);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    const WPE_Result result = wpe__write_file_atomic(path, text, strlen(text));
    if (result != WPE_RESULT_OK) wpe_log_error("cannot write '%s': %s", path, wpe_result_to_string(result));
    return result;
}

WPE_Result wpe_project_create(const char* os_directory, const char* name) {
    if (!os_directory || !name) return WPE_RESULT_ERROR_NULL_ARG;
    if (!name[0] || strlen(name) >= WPE_PROJECT_NAME_MAX) {
        wpe_log_error("a project name is 1 to %u bytes", WPE_PROJECT_NAME_MAX - 1);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    bool occupied = false;
    if (wpe_directory_exists(os_directory)) {
        const WPE_Result walked = wpe_directory_walk(os_directory, wpe__any_file, &occupied);
        if (walked != WPE_RESULT_OK) return walked;
    }
    if (occupied) {
        wpe_log_error("'%s' already has files; a new project needs an empty or new directory", os_directory);
        return WPE_RESULT_ERROR_ALREADY_EXISTS;
    }

    WPE_ProjectSettings settings;
    wpe_project_settings_default(&settings);
    snprintf(settings.name, sizeof settings.name, "%s", name);
    snprintf(settings.start_scene, sizeof settings.start_scene, "scenes/main.scene");
    /* Movement on WASD, the arrows and the left stick; jumping on Space and the pad's south button. */
    const WPE_Key x_keys[] = {WPE__LETTER('d'), WPE__LETTER('a'), WPE_KEY_RIGHT, WPE_KEY_LEFT};
    const WPE_Key y_keys[] = {WPE__LETTER('w'), WPE__LETTER('s'), WPE_KEY_UP, WPE_KEY_DOWN};
    const float signs[] = {1, -1, 1, -1};
    const WPE_Key jump_key[] = {WPE_KEY_SPACE};
    const float one[] = {1};
    settings.actions[0] =
        wpe__action("move_x", x_keys, signs, 4,
                    (WPE_InputBinding){.source = WPE_INPUT_SOURCE_PAD_AXIS, .code = WPE_PAD_LEFT_X});
    settings.actions[1] = wpe__action(
        "move_y", y_keys, signs, 4,
        (WPE_InputBinding){.source = WPE_INPUT_SOURCE_PAD_AXIS, .code = WPE_PAD_LEFT_Y, .scale = -1});
    settings.actions[2] =
        wpe__action("jump", jump_key, one, 1,
                    (WPE_InputBinding){.source = WPE_INPUT_SOURCE_PAD_BUTTON, .code = WPE_PAD_SOUTH});
    settings.action_count = 3;

    WPE_JsonWriter* writer = NULL;
    WPE_Result result = wpe_json_writer_create(&writer);
    if (result == WPE_RESULT_OK) result = wpe_project_write(&settings, writer);
    const char* project = NULL;
    if (result == WPE_RESULT_OK) result = wpe_json_writer_finish(writer, &project, NULL);

    static const char* const directories[] = {"", "/scenes", "/scripts", "/meshes"};
    for (size_t i = 0; result == WPE_RESULT_OK && i < sizeof directories / sizeof directories[0]; ++i) {
        char path[1024];
        if (snprintf(path, sizeof path, "%s%s", os_directory, directories[i]) >= (int)sizeof path) {
            wpe_log_error("the project path '%s' is too long", os_directory);
            result = WPE_RESULT_ERROR_INVALID_ARG;
        } else {
            result = wpe_directory_create(path);
        }
    }
    const struct {
        const char* path;
        const char* text;
    } files[] = {{WPE_PROJECT_FILE, project},
                 {"scenes/main.scene", wpe__main_scene},
                 {"meshes/ground.prim", wpe__ground_prim},
                 {"meshes/cube.prim", wpe__cube_prim},
                 {"scripts/spin.luau", wpe__spin_script}};
    for (size_t i = 0; result == WPE_RESULT_OK && i < sizeof files / sizeof files[0]; ++i)
        result = wpe__write_text(os_directory, files[i].path, files[i].text);
    wpe_json_writer_destroy(writer);
    if (result == WPE_RESULT_OK) wpe_log_info("created project '%s' in '%s'", name, os_directory);
    return result;
}
