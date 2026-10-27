#define WPE_LOG_MODULE "engine"
#include "engine/project.h"

#include <stdio.h>
#include <string.h>

#include "core/alloc.h"
#include "engine/json_read.h"

static const char* const wpe__axis_ranges[] = {"full", "positive", "negative"};

void wpe_project_settings_default(WPE_ProjectSettings* out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "WillPowerEngine Game");
    out->width = 320;
    out->height = 240;
    out->scale = 3;
    out->vsync = true;
    out->resizable = true;
    out->fixed_rate = 60.0;
    out->max_steps = 8;
    for (uint32_t i = 0; i < WPE_AUDIO_BUS_COUNT; ++i)
        out->volumes[i] = 1.0f;
    out->script_memory_mb = 64;
    out->script_time_limit_ms = 250.0;
    out->input = wpe_input_settings_default();
}

/* One binding object: exactly one source key, plus "scale" and (axes only) "range". */
static bool wpe__parse_binding(wpe__JsonReader* r, const WPE_JsonValue* v, const char* action,
                               WPE_InputBinding* out) {
    if (!WPE__JSON_CHECK_OBJECT(r, v, "binding", "key", "mouse_button", "pad_button", "pad_axis", "scale",
                                "range"))
        return false;
    static const char* const sources[] = {"key", "mouse_button", "pad_button", "pad_axis"};
    const char* source_key = NULL;
    for (uint32_t s = 0; s < 4; ++s) {
        if (!wpe_json_get(v, sources[s])) continue;
        if (source_key)
            return wpe__json_fail(r, v, WPE_RESULT_ERROR_PARSE,
                                  "action \"%s\": a binding has one source, not both "
                                  "\"%s\" and \"%s\"",
                                  action, source_key, sources[s]);
        source_key = sources[s];
        out->source = (WPE_InputSource)s;
    }
    if (!source_key)
        return wpe__json_fail(
            r, v, WPE_RESULT_ERROR_PARSE,
            "action \"%s\": a binding needs \"key\", \"mouse_button\", \"pad_button\" or \"pad_axis\"",
            action);
    const WPE_JsonValue* name_value = wpe_json_get(v, source_key);
    const char* name = wpe_json_string(name_value);
    bool known = false;
    switch (out->source) {
    case WPE_INPUT_SOURCE_KEY:
        known = wpe_input_key_from_name(name, &out->code);
        break;
    case WPE_INPUT_SOURCE_MOUSE_BUTTON: {
        WPE_MouseButton button = WPE_MOUSE_LEFT;
        known = wpe_input_mouse_button_from_name(name, &button);
        out->code = (uint32_t)button;
        break;
    }
    case WPE_INPUT_SOURCE_PAD_BUTTON: {
        WPE_GamepadButton button = WPE_PAD_SOUTH;
        known = wpe_input_pad_button_from_name(name, &button);
        out->code = (uint32_t)button;
        break;
    }
    case WPE_INPUT_SOURCE_PAD_AXIS: {
        WPE_GamepadAxis axis = WPE_PAD_LEFT_X;
        known = wpe_input_pad_axis_from_name(name, &axis);
        out->code = (uint32_t)axis;
        break;
    }
    }
    if (!known)
        return wpe__json_fail(r, name_value, WPE_RESULT_ERROR_PARSE, "action \"%s\": unknown %s name \"%s\"",
                              action, source_key, name ? name : "(not a string)");
    out->scale = 1.0f;
    uint32_t range = WPE_AXIS_FULL;
    if (!wpe__json_read_float(r, v, "scale", -100.0, 100.0, &out->scale) ||
        !wpe__json_read_enum(r, v, "range", wpe__axis_ranges, 3, &range))
        return false;
    if (out->scale == 0.0f)
        return wpe__json_fail(r, v, WPE_RESULT_ERROR_PARSE, "action \"%s\": \"scale\" is zero", action);
    if (range != WPE_AXIS_FULL && out->source != WPE_INPUT_SOURCE_PAD_AXIS)
        return wpe__json_fail(r, v, WPE_RESULT_ERROR_PARSE,
                              "action \"%s\": \"range\" applies to pad axes only", action);
    out->range = (WPE_AxisRange)range;
    return true;
}

static bool wpe__parse_input(wpe__JsonReader* r, const WPE_JsonValue* v, WPE_ProjectSettings* out) {
    uint32_t keyboard_player = out->input.keyboard_player + 1;
    if (!WPE__JSON_CHECK_OBJECT(r, v, "input", "keyboard_player", "stick_deadzone", "stick_outer",
                                "trigger_deadzone", "press_threshold", "actions") ||
        !wpe__json_read_uint(r, v, "keyboard_player", 1, WPE_INPUT_MAX_PLAYERS, &keyboard_player) ||
        !wpe__json_read_float(r, v, "stick_deadzone", 0.01, 0.9, &out->input.stick_deadzone) ||
        !wpe__json_read_float(r, v, "stick_outer", 0.1, 1.0, &out->input.stick_outer) ||
        !wpe__json_read_float(r, v, "trigger_deadzone", 0.01, 0.9, &out->input.trigger_deadzone) ||
        !wpe__json_read_float(r, v, "press_threshold", 0.05, 1.0, &out->input.press_threshold))
        return false;
    out->input.keyboard_player = keyboard_player - 1;
    if (!(out->input.stick_outer > out->input.stick_deadzone))
        return wpe__json_fail(r, v, WPE_RESULT_ERROR_PARSE, "\"stick_outer\" must exceed \"stick_deadzone\"");
    const WPE_JsonValue* actions = wpe_json_get(v, "actions");
    if (!actions) return true;
    if (wpe_json_type(actions) != WPE_JSON_OBJECT)
        return wpe__json_fail(r, actions, WPE_RESULT_ERROR_PARSE,
                              "\"actions\" must map action names to binding lists");
    if (wpe_json_count(actions) > WPE_INPUT_MAX_ACTIONS)
        return wpe__json_fail(r, actions, WPE_RESULT_ERROR_PARSE, "at most %u actions",
                              WPE_INPUT_MAX_ACTIONS);
    for (uint32_t i = 0; i < wpe_json_count(actions); ++i) {
        const char* name = NULL;
        const WPE_JsonValue* bindings = wpe_json_member(actions, i, &name);
        WPE_InputActionDesc* action = &out->actions[out->action_count];
        *action = (WPE_InputActionDesc){0};
        if (name[0] == '\0' || strlen(name) >= sizeof action->name)
            return wpe__json_fail(r, bindings, WPE_RESULT_ERROR_PARSE, "action names must be 1 to %zu bytes",
                                  sizeof action->name - 1);
        memcpy(action->name, name, strlen(name) + 1);
        if (wpe_json_type(bindings) != WPE_JSON_ARRAY || wpe_json_count(bindings) > WPE_INPUT_MAX_BINDINGS)
            return wpe__json_fail(r, bindings, WPE_RESULT_ERROR_PARSE,
                                  "action \"%s\" must list at most %u bindings", name,
                                  WPE_INPUT_MAX_BINDINGS);
        for (uint32_t b = 0; b < wpe_json_count(bindings); ++b) {
            if (!wpe__parse_binding(r, wpe_json_at(bindings, b), name, &action->bindings[b])) return false;
        }
        action->binding_count = wpe_json_count(bindings);
        ++out->action_count;
    }
    return true;
}

static bool wpe__parse_project(wpe__JsonReader* r, const WPE_JsonValue* root, WPE_ProjectSettings* out) {
    if (!WPE__JSON_CHECK_OBJECT(r, root, "project", "format", "version", "name", "start_scene", "font",
                                "display", "simulation", "assets", "audio", "scripting", "prefabs", "input"))
        return false;
    const char* format = wpe_json_string(wpe_json_get(root, "format"));
    if (!format || strcmp(format, "wpe.project") != 0)
        return wpe__json_fail(r, root, WPE_RESULT_ERROR_PARSE,
                              "not a project file (\"format\" must be \"wpe.project\")");
    uint32_t version = 0;
    if (!wpe__json_read_uint(r, root, "version", 1, UINT32_MAX, &version)) return false;
    if (version == 0) return wpe__json_fail(r, root, WPE_RESULT_ERROR_PARSE, "\"version\" is required");
    if (version > WPE_PROJECT_FORMAT_VERSION)
        return wpe__json_fail(r, root, WPE_RESULT_ERROR_UNSUPPORTED,
                              "format version %u is newer than this engine's (%u)", version,
                              WPE_PROJECT_FORMAT_VERSION);
    if (!wpe__json_read_string(r, root, "name", out->name, sizeof out->name) ||
        !wpe__json_read_string(r, root, "start_scene", out->start_scene, sizeof out->start_scene) ||
        !wpe__json_read_string(r, root, "font", out->font, sizeof out->font))
        return false;
    if (out->start_scene[0] && !wpe_vfs_path_is_valid(out->start_scene))
        return wpe__json_fail(r, wpe_json_get(root, "start_scene"), WPE_RESULT_ERROR_PARSE,
                              "\"start_scene\" must be an asset path");
    if (out->font[0] && !wpe_vfs_path_is_valid(out->font))
        return wpe__json_fail(r, wpe_json_get(root, "font"), WPE_RESULT_ERROR_PARSE,
                              "\"font\" must be an asset path");

    const WPE_JsonValue* display = wpe_json_get(root, "display");
    if (display && (!WPE__JSON_CHECK_OBJECT(r, display, "display", "width", "height", "scale", "vsync",
                                            "resizable", "fullscreen") ||
                    !wpe__json_read_uint(r, display, "width", 16, 4096, &out->width) ||
                    !wpe__json_read_uint(r, display, "height", 16, 4096, &out->height) ||
                    !wpe__json_read_uint(r, display, "scale", 1, 16, &out->scale) ||
                    !wpe__json_read_bool(r, display, "vsync", &out->vsync) ||
                    !wpe__json_read_bool(r, display, "resizable", &out->resizable) ||
                    !wpe__json_read_bool(r, display, "fullscreen", &out->fullscreen)))
        return false;
    const WPE_JsonValue* simulation = wpe_json_get(root, "simulation");
    if (simulation && (!WPE__JSON_CHECK_OBJECT(r, simulation, "simulation", "fixed_rate", "max_steps") ||
                       !wpe__json_read_double(r, simulation, "fixed_rate", 1.0, 1000.0, &out->fixed_rate) ||
                       !wpe__json_read_uint(r, simulation, "max_steps", 1, 64, &out->max_steps)))
        return false;
    const WPE_JsonValue* assets = wpe_json_get(root, "assets");
    if (assets && (!WPE__JSON_CHECK_OBJECT(r, assets, "assets", "max_texture_size") ||
                   !wpe__json_read_uint(r, assets, "max_texture_size", 0, 16384, &out->max_texture_size)))
        return false;
    const WPE_JsonValue* audio = wpe_json_get(root, "audio");
    if (audio && (!WPE__JSON_CHECK_OBJECT(r, audio, "audio", "master", "effects", "music") ||
                  !wpe__json_read_float(r, audio, "master", 0, 4, &out->volumes[WPE_AUDIO_BUS_MASTER]) ||
                  !wpe__json_read_float(r, audio, "effects", 0, 4, &out->volumes[WPE_AUDIO_BUS_EFFECTS]) ||
                  !wpe__json_read_float(r, audio, "music", 0, 4, &out->volumes[WPE_AUDIO_BUS_MUSIC])))
        return false;
    const WPE_JsonValue* scripting = wpe_json_get(root, "scripting");
    if (scripting &&
        (!WPE__JSON_CHECK_OBJECT(r, scripting, "scripting", "memory_limit_mb", "time_limit_ms") ||
         !wpe__json_read_uint(r, scripting, "memory_limit_mb", 1, 4096, &out->script_memory_mb) ||
         !wpe__json_read_double(r, scripting, "time_limit_ms", 1.0, 60000.0, &out->script_time_limit_ms)))
        return false;
    const WPE_JsonValue* prefabs = wpe_json_get(root, "prefabs");
    if (prefabs) {
        if (wpe_json_type(prefabs) != WPE_JSON_OBJECT || wpe_json_count(prefabs) > WPE_PROJECT_MAX_PREFABS)
            return wpe__json_fail(r, prefabs, WPE_RESULT_ERROR_PARSE,
                                  "\"prefabs\" must map at most %u names to model asset paths",
                                  WPE_PROJECT_MAX_PREFABS);
        for (uint32_t i = 0; i < wpe_json_count(prefabs); ++i) {
            const char* name = NULL;
            const WPE_JsonValue* path = wpe_json_member(prefabs, i, &name);
            WPE_ProjectPrefab* prefab = &out->prefabs[i];
            if (name[0] == '\0' || strlen(name) >= sizeof prefab->name)
                return wpe__json_fail(r, path, WPE_RESULT_ERROR_PARSE, "prefab names must be 1 to %zu bytes",
                                      sizeof prefab->name - 1);
            const char* text = wpe_json_string(path);
            if (!text || !wpe_vfs_path_is_valid(text))
                return wpe__json_fail(r, path, WPE_RESULT_ERROR_PARSE,
                                      "prefab \"%s\" must name a model asset path", name);
            memcpy(prefab->name, name, strlen(name) + 1);
            memcpy(prefab->path, text, strlen(text) + 1);
            out->prefab_count = i + 1;
        }
    }
    const WPE_JsonValue* input = wpe_json_get(root, "input");
    return !input || wpe__parse_input(r, input, out);
}

WPE_Result wpe_project_parse(const char* text, size_t size, const char* name, WPE_ProjectSettings* out) {
    if (!text || !out) return WPE_RESULT_ERROR_NULL_ARG;
    wpe_project_settings_default(out);
    wpe__JsonReader r = {.name = name ? name : WPE_PROJECT_FILE, .kind = "project"};
    WPE_JsonDocument* doc = NULL;
    WPE_JsonError error;
    WPE_Result result = wpe_json_parse(text, size, &doc, &error);
    if (result == WPE_RESULT_ERROR_PARSE)
        wpe_log_error("project '%s', line %u, column %u: %s", r.name, error.line, error.column,
                      error.message);
    if (result != WPE_RESULT_OK) return result;
    if (!wpe__parse_project(&r, wpe_json_root(doc), out)) {
        result = r.result;
        wpe_project_settings_default(out);
    }
    wpe_json_free(doc);
    return result;
}

WPE_Result wpe_project_load(const WPE_Vfs* vfs, WPE_ProjectSettings* out) {
    if (!vfs || !out) return WPE_RESULT_ERROR_NULL_ARG;
    void* data = NULL;
    size_t size = 0;
    WPE_Result result = wpe_vfs_read_all(vfs, WPE_PROJECT_FILE, &data, &size);
    if (result == WPE_RESULT_ERROR_NOT_FOUND)
        wpe_log_error("no %s found in the game's files", WPE_PROJECT_FILE);
    if (result == WPE_RESULT_OK) result = wpe_project_parse(data, size, WPE_PROJECT_FILE, out);
    wpe_free(data);
    return result;
}

static void wpe__key_number(WPE_JsonWriter* w, const char* key, double value) {
    wpe_json_write_key(w, key);
    wpe_json_write_number(w, value);
}

static void wpe__key_integer(WPE_JsonWriter* w, const char* key, int64_t value) {
    wpe_json_write_key(w, key);
    wpe_json_write_integer(w, value);
}

static void wpe__key_bool(WPE_JsonWriter* w, const char* key, bool value) {
    wpe_json_write_key(w, key);
    wpe_json_write_bool(w, value);
}

static void wpe__key_string(WPE_JsonWriter* w, const char* key, const char* value) {
    wpe_json_write_key(w, key);
    wpe_json_write_string(w, value);
}

WPE_Result wpe_project_write(const WPE_ProjectSettings* s, WPE_JsonWriter* w) {
    if (!s || !w) return WPE_RESULT_ERROR_NULL_ARG;
    wpe_json_write_object_begin(w);
    wpe__key_string(w, "format", "wpe.project");
    wpe__key_integer(w, "version", WPE_PROJECT_FORMAT_VERSION);
    wpe__key_string(w, "name", s->name);
    wpe__key_string(w, "start_scene", s->start_scene);
    wpe__key_string(w, "font", s->font);
    wpe_json_write_key(w, "display");
    wpe_json_write_object_begin(w);
    wpe__key_integer(w, "width", s->width);
    wpe__key_integer(w, "height", s->height);
    wpe__key_integer(w, "scale", s->scale);
    wpe__key_bool(w, "vsync", s->vsync);
    wpe__key_bool(w, "resizable", s->resizable);
    wpe__key_bool(w, "fullscreen", s->fullscreen);
    wpe_json_write_object_end(w);
    wpe_json_write_key(w, "simulation");
    wpe_json_write_object_begin(w);
    wpe_json_write_key(w, "fixed_rate");
    wpe_json_write_double(w, s->fixed_rate);
    wpe__key_integer(w, "max_steps", s->max_steps);
    wpe_json_write_object_end(w);
    wpe_json_write_key(w, "assets");
    wpe_json_write_object_begin(w);
    wpe__key_integer(w, "max_texture_size", s->max_texture_size);
    wpe_json_write_object_end(w);
    wpe_json_write_key(w, "audio");
    wpe_json_write_object_begin(w);
    wpe__key_number(w, "master", s->volumes[WPE_AUDIO_BUS_MASTER]);
    wpe__key_number(w, "effects", s->volumes[WPE_AUDIO_BUS_EFFECTS]);
    wpe__key_number(w, "music", s->volumes[WPE_AUDIO_BUS_MUSIC]);
    wpe_json_write_object_end(w);
    wpe_json_write_key(w, "scripting");
    wpe_json_write_object_begin(w);
    wpe__key_integer(w, "memory_limit_mb", s->script_memory_mb);
    wpe_json_write_key(w, "time_limit_ms");
    wpe_json_write_double(w, s->script_time_limit_ms);
    wpe_json_write_object_end(w);

    wpe_json_write_key(w, "prefabs");
    wpe_json_write_object_begin(w);
    for (uint32_t i = 0; i < s->prefab_count && i < WPE_PROJECT_MAX_PREFABS; ++i)
        wpe__key_string(w, s->prefabs[i].name, s->prefabs[i].path);
    wpe_json_write_object_end(w);

    wpe_json_write_key(w, "input");
    wpe_json_write_object_begin(w);
    wpe__key_integer(w, "keyboard_player", (int64_t)s->input.keyboard_player + 1);
    wpe__key_number(w, "stick_deadzone", s->input.stick_deadzone);
    wpe__key_number(w, "stick_outer", s->input.stick_outer);
    wpe__key_number(w, "trigger_deadzone", s->input.trigger_deadzone);
    wpe__key_number(w, "press_threshold", s->input.press_threshold);
    wpe_json_write_key(w, "actions");
    wpe_json_write_object_begin(w);
    WPE_Result result = WPE_RESULT_OK;
    for (uint32_t a = 0; a < s->action_count && a < WPE_INPUT_MAX_ACTIONS; ++a) {
        const WPE_InputActionDesc* action = &s->actions[a];
        wpe_json_write_key(w, action->name);
        wpe_json_write_array_begin(w, false);
        for (uint32_t b = 0; b < action->binding_count && b < WPE_INPUT_MAX_BINDINGS; ++b) {
            const WPE_InputBinding* binding = &action->bindings[b];
            const char* source = NULL;
            const char* name = NULL;
            switch (binding->source) {
            case WPE_INPUT_SOURCE_KEY:
                source = "key";
                name = wpe_input_key_name(binding->code);
                break;
            case WPE_INPUT_SOURCE_MOUSE_BUTTON:
                source = "mouse_button";
                name = wpe_input_mouse_button_name((WPE_MouseButton)binding->code);
                break;
            case WPE_INPUT_SOURCE_PAD_BUTTON:
                source = "pad_button";
                name = wpe_input_pad_button_name((WPE_GamepadButton)binding->code);
                break;
            case WPE_INPUT_SOURCE_PAD_AXIS:
                source = "pad_axis";
                name = wpe_input_pad_axis_name((WPE_GamepadAxis)binding->code);
                break;
            }
            if (!source || !name) {
                wpe_log_error("action '%s': binding %u has no name and cannot be saved", action->name, b);
                result = WPE_RESULT_ERROR_INVALID_ARG;
                name = "?";
                source = "key";
            }
            wpe_json_write_object_begin(w);
            wpe__key_string(w, source, name);
            const float scale = binding->scale != 0.0f ? binding->scale : 1.0f;
            if (scale != 1.0f) wpe__key_number(w, "scale", scale);
            if (binding->range != WPE_AXIS_FULL && binding->range <= WPE_AXIS_NEGATIVE)
                wpe__key_string(w, "range", wpe__axis_ranges[binding->range]);
            wpe_json_write_object_end(w);
        }
        wpe_json_write_array_end(w);
    }
    wpe_json_write_object_end(w);
    wpe_json_write_object_end(w);
    wpe_json_write_object_end(w);
    return result;
}
