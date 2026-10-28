/*
 * The editor UI. Panels read the world directly but change it only through editor commands
 * (wpe_editor_execute), so the UI and agents share validation, undo and behavior.
 */
#define WPE_LOG_MODULE "editor"
#include "editor/editor_ui.h"

#include <SDL3/SDL.h>
#include <cimgui.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/alloc.h"
#include "core/log.h"
#include "editor/editor_internal.h"
#include "editor/ui_backend.h"
#include "graphics/graphics.h"
#include "graphics/scene_render.h"
#include "scene/scene.h"

#define WPE__UI_PATH 256
#define WPE__ASSET_PAYLOAD "WPE_ASSET"
#define WPE__ENTITY_PAYLOAD "WPE_ENTITY"
#define WPE__RING_SEGMENTS 48
#define WPE__BACKGROUND 0xFF202226u /* Behind the docked windows. */
#define WPE__LABEL_WIDTH 130.0f     /* Inspector and environment: room for field names right of the fields. */

/* ImGui colors are 0xAABBGGRR. */
#define WPE__COL(r, g, b, a)                                                                                 \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))

typedef enum wpe__Pending {
    WPE__PENDING_NONE = 0,
    WPE__PENDING_NEW,
    WPE__PENDING_LOAD,
    WPE__PENDING_QUIT
} wpe__Pending;

/* What a path popup did this frame. */
typedef enum wpe__PopupResult { WPE__POPUP_NONE = 0, WPE__POPUP_OK, WPE__POPUP_CANCEL } wpe__PopupResult;

typedef struct wpe__Edit {
    const WPE_JsonValue* target; /* The leaf being replaced. */
    WPE_JsonType type;
    double number;
    bool boolean;
    char string[WPE__UI_PATH];
    float vector[4];
    uint32_t vector_count;
} wpe__Edit;

struct WPE_EditorUi {
    WPE_Editor* editor;
    WPE_UiBackend* backend;
    uint64_t view_image; /* The game's framebuffer as a UI image (0 before the first frame). */
    uint32_t view_width, view_height;

    /* The viewport image on screen during the last frame. */
    float image_x, image_y, image_w, image_h;
    bool view_hovered, view_focused;

    /* Fly camera. */
    float yaw, pitch, speed;
    bool flying, camera_initialized;

    /* Gizmo (its mode and space are view settings of the editor). */
    int gizmo_axis; /* -1: none, 0..2: dragged axis. */
    int gizmo_hover;
    uint32_t gizmo_drag;
    WPE_Transform gizmo_start;
    float gizmo_mouse_x, gizmo_mouse_y;
    float gizmo_tangent_x, gizmo_tangent_y; /* Rotate: screen direction of a positive rotation. */
    WPE_Vec3 gizmo_start_center;            /* The entity's world position when the drag started. */

    /* 2D view: a UI element being dragged (its edges, as WPE__EDGE_* bits, or 0) and where it started. */
    WPE_Entity canvas_entity;
    int canvas_edges;
    uint32_t canvas_drag;
    WPE_UiRect canvas_start;
    float canvas_mouse_x, canvas_mouse_y;

    /* Inspector edits. */
    uint32_t edit_serial; /* Bumped when a widget becomes active: one undo step per edit gesture. */
    char add_path[WPE__UI_PATH];
    char add_property[64]; /* "Add property": its name and type (number, bool, string, vector). */
    int add_property_type;

    /* Assets. */
    char** assets;
    uint32_t asset_count, asset_capacity;
    bool assets_stale;
    char asset_filter[64];

    /* Popups and console. */
    char scene_path[WPE__UI_PATH];
    char export_path[512];
    char command[4096];
    char reply[4096];
    bool open_scene_popup, save_as_popup, export_popup;
    /* An action that would drop the scene, waiting for the unsaved-changes prompt (or for "save as"
     * when the prompt's Save needs a path first). */
    wpe__Pending pending;
    char pending_path[WPE__UI_PATH];
    /* "Save as prefab": the entity and the path being asked for. */
    bool prefab_popup;
    WPE_Entity prefab_entity;
    char prefab_path[WPE__UI_PATH];
    bool unsaved_popup;
    bool pending_after_save;
    bool layout_built;
    bool quit;
};

/* ---- Commands --------------------------------------------------------------------------------------- */

/* Runs a command built by `build` (which writes the arguments after "cmd"); keeps the reply text. */
static WPE_Result wpe__ui_run(WPE_EditorUi* ui, const char* text) {
    WPE_JsonWriter* reply = NULL;
    WPE_Result result = wpe_json_writer_create(&reply);
    if (result != WPE_RESULT_OK) return result;
    result = wpe_editor_execute(ui->editor, text, strlen(text), reply);
    const char* reply_text = NULL;
    if (wpe_json_writer_finish(reply, &reply_text, NULL) == WPE_RESULT_OK)
        snprintf(ui->reply, sizeof ui->reply, "%s", reply_text);
    wpe_json_writer_destroy(reply);
    return result;
}

/* Starts a command object: {"cmd": name, ...}; finish with wpe__ui_send(). */
static WPE_JsonWriter* wpe__ui_begin(const char* name) {
    WPE_JsonWriter* w = NULL;
    if (wpe_json_writer_create(&w) != WPE_RESULT_OK) return NULL;
    wpe_json_writer_set_compact(w, true);
    wpe_json_write_object_begin(w);
    wpe_json_write_key(w, "cmd");
    wpe_json_write_string(w, name);
    return w;
}

static WPE_Result wpe__ui_send(WPE_EditorUi* ui, WPE_JsonWriter* w) {
    if (!w) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    wpe_json_write_object_end(w);
    const char* text = NULL;
    WPE_Result result = wpe_json_writer_finish(w, &text, NULL);
    if (result == WPE_RESULT_OK) result = wpe__ui_run(ui, text);
    wpe_json_writer_destroy(w);
    return result;
}

static WPE_Result wpe__ui_simple(WPE_EditorUi* ui, const char* name) {
    return wpe__ui_send(ui, wpe__ui_begin(name));
}

/* A command with one string argument, optionally with "discard": true. */
static WPE_Result wpe__ui_string_command(WPE_EditorUi* ui, const char* name, const char* key,
                                         const char* value, bool discard) {
    WPE_JsonWriter* w = wpe__ui_begin(name);
    if (!w) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    if (key) {
        wpe_json_write_key(w, key);
        wpe_json_write_string(w, value);
    }
    if (discard) {
        wpe_json_write_key(w, "discard");
        wpe_json_write_bool(w, true);
    }
    return wpe__ui_send(ui, w);
}

/* Carries out the pending action; `discard` drops unsaved changes. */
static void wpe__ui_perform(WPE_EditorUi* ui, bool discard) {
    const wpe__Pending pending = ui->pending;
    ui->pending = WPE__PENDING_NONE;
    ui->pending_after_save = false;
    switch (pending) {
    case WPE__PENDING_NEW:
        wpe__ui_string_command(ui, "new_scene", NULL, NULL, discard);
        break;
    case WPE__PENDING_LOAD:
        wpe__ui_string_command(ui, "load_scene", "path", ui->pending_path, discard);
        break;
    case WPE__PENDING_QUIT:
        ui->quit = true;
        break;
    case WPE__PENDING_NONE:
        break;
    }
}

/* Runs an action that replaces the scene, asking first when it has unsaved changes. */
static void wpe__ui_guard(WPE_EditorUi* ui, wpe__Pending action, const char* path) {
    /* Unsaved changes belong to the edited scene: stop playing so it can be saved. */
    if (wpe_editor_mode(ui->editor) != WPE_EDITOR_EDIT) wpe__ui_simple(ui, "stop");
    ui->pending = action;
    snprintf(ui->pending_path, sizeof ui->pending_path, "%s", path ? path : "");
    if (wpe_editor_dirty(ui->editor)) {
        ui->unsaved_popup = true;
    } else {
        wpe__ui_perform(ui, false);
    }
}

/* The unsaved-changes prompt: Save, Don't save or Cancel. */
static void wpe__ui_unsaved_popup(WPE_EditorUi* ui) {
    if (ui->unsaved_popup) {
        igOpenPopup_Str("Unsaved changes", 0);
        ui->unsaved_popup = false;
    }
    if (!igBeginPopupModal("Unsaved changes", NULL, ImGuiWindowFlags_AlwaysAutoResize)) return;
    const char* path = wpe_editor_scene_path(ui->editor);
    igText("Save the changes to %s?", path[0] ? path : "the new scene");
    if (igButton("Save", (ImVec2_c){120, 0})) {
        igCloseCurrentPopup();
        if (!path[0]) {
            ui->pending_after_save = true; /* Continued once "save as" succeeds. */
            ui->save_as_popup = true;
        } else if (wpe__ui_simple(ui, "save_scene") == WPE_RESULT_OK) {
            wpe__ui_perform(ui, false);
        } else {
            ui->pending = WPE__PENDING_NONE; /* The console shows why. */
        }
    }
    igSameLine(0, -1);
    if (igButton("Don't save", (ImVec2_c){120, 0})) {
        igCloseCurrentPopup();
        wpe__ui_perform(ui, true);
    }
    igSameLine(0, -1);
    if (igButton("Cancel", (ImVec2_c){120, 0}) || igIsKeyPressed_Bool(ImGuiKey_Escape, false)) {
        igCloseCurrentPopup();
        ui->pending = WPE__PENDING_NONE;
    }
    igEndPopup();
}

/* set_view with one setting: a string (when `text` is set), a number (grid_spacing, snap_*) or a bool. */
static void wpe__ui_set_view(WPE_EditorUi* ui, const char* key, const char* text, double number, bool flag) {
    WPE_JsonWriter* w = wpe__ui_begin("set_view");
    if (!w) return;
    wpe_json_write_key(w, "value");
    wpe_json_write_object_begin(w);
    wpe_json_write_key(w, key);
    if (text) {
        wpe_json_write_string(w, text);
    } else if (strcmp(key, "grid_spacing") == 0 || strncmp(key, "snap_", 5) == 0) {
        wpe_json_write_double(w, number);
    } else {
        wpe_json_write_bool(w, flag);
    }
    wpe_json_write_object_end(w);
    wpe__ui_send(ui, w);
}

static void wpe__ui_entity_command(WPE_EditorUi* ui, const char* name, WPE_Entity entity) {
    WPE_JsonWriter* w = wpe__ui_begin(name);
    if (!w) return;
    wpe_json_write_key(w, "id");
    wpe_json_write_integer(w, entity);
    wpe__ui_send(ui, w);
}

/* ---- Generic JSON field editing ------------------------------------------------------------------------ */

/* Writes `v` with the edited leaf replaced. */
static void wpe__write_edited(WPE_JsonWriter* w, const WPE_JsonValue* v, const wpe__Edit* edit) {
    if (v == edit->target) {
        switch (edit->type) {
        case WPE_JSON_NUMBER:
            wpe_json_write_double(w, edit->number);
            break;
        case WPE_JSON_BOOL:
            wpe_json_write_bool(w, edit->boolean);
            break;
        case WPE_JSON_STRING:
            wpe_json_write_string(w, edit->string);
            break;
        case WPE_JSON_ARRAY:
            wpe_json_write_array_begin(w, true);
            for (uint32_t i = 0; i < edit->vector_count; ++i)
                wpe_json_write_number(w, edit->vector[i]);
            wpe_json_write_array_end(w);
            break;
        default:
            wpe_json_write_null(w);
            break;
        }
        return;
    }
    switch (wpe_json_type(v)) {
    case WPE_JSON_NULL:
        wpe_json_write_null(w);
        break;
    case WPE_JSON_BOOL:
        wpe_json_write_bool(w, wpe_json_bool(v, false));
        break;
    case WPE_JSON_NUMBER:
        wpe_json_write_double(w, wpe_json_number(v, 0.0));
        break;
    case WPE_JSON_STRING:
        wpe_json_write_string(w, wpe_json_string(v));
        break;
    case WPE_JSON_ARRAY:
        wpe_json_write_array_begin(w, true);
        for (uint32_t i = 0; i < wpe_json_count(v); ++i)
            wpe__write_edited(w, wpe_json_at(v, i), edit);
        wpe_json_write_array_end(w);
        break;
    case WPE_JSON_OBJECT:
        wpe_json_write_object_begin(w);
        for (uint32_t i = 0; i < wpe_json_count(v); ++i) {
            const char* key = NULL;
            const WPE_JsonValue* member = wpe_json_member(v, i, &key);
            wpe_json_write_key(w, key);
            wpe__write_edited(w, member, edit);
        }
        wpe_json_write_object_end(w);
        break;
    }
}

/* Choices for enumerated string fields, by component and key. */
static const char* const* wpe__choices(const char* component, const char* key, uint32_t* out_count) {
    static const char* const lights[] = {"directional", "point", "spot"};
    static const char* const bodies[] = {"static", "kinematic", "dynamic"};
    static const char* const shapes[] = {"box", "sphere", "capsule", "convex", "mesh"};
    static const char* const buses[] = {"effects", "music"};
    static const char* const fogs[] = {"none", "linear", "exponential"};
    static const char* const effects[] = {"ssao",         "chromatic_aberration", "ps1",       "vignette",
                                          "color_adjust", "depth_of_field",       "shockwave", "radial_blur"};
    static const char* const aligns[] = {"left", "center", "right"};
    static const char* const valigns[] = {"top", "middle", "bottom"};
    static const char* const billboard_modes[] = {"fixed", "axis_aligned", "axis_aligned_y",
                                                  "screen_aligned"};
    static const char* const billboard_blends[] = {"cutout", "alpha", "additive"};
    static const char* const particle_shapes[] = {"point", "sphere", "box"};
    static const char* const skies[] = {"color", "gradient", "panorama"};
    const struct {
        const char* component;
        const char* key;
        const char* const* choices;
        uint32_t count;
    } table[] = {
        {"light", "type", lights, 3},
        {"rigidbody", "type", bodies, 3},
        {"collider", "shape", shapes, 5},
        {"audio_source", "bus", buses, 2},
        {"fog", "mode", fogs, 3}, /* The environment's objects go by their own names (wpe__draw_value). */
        {"sky", "mode", skies, 3},
        {"environment", "effect", effects, 8},
        {"text", "align", aligns, 3},
        {"text", "valign", valigns, 3},
        {"billboard", "mode", billboard_modes, 4},
        {"billboard", "blend", billboard_blends, 3},
        {"particles", "blend", billboard_blends, 3},
        {"particles", "shape", particle_shapes, 3},
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; ++i) {
        if (strcmp(table[i].component, component) == 0 && strcmp(table[i].key, key) == 0) {
            *out_count = table[i].count;
            return table[i].choices;
        }
    }
    return NULL;
}

/* Optional asset references (null when there is none), by component and key. */
static bool wpe__is_asset_key(const char* component, const char* key) {
    static const char* const table[][2] = {
        {"mesh", "model"},         {"animator", "model"},  {"collider", "model"},
        {"audio_source", "sound"}, {"sprite", "image"},    {"text", "font"},
        {"billboard", "image"},    {"particles", "image"}, {"sky", "image"}};
    for (size_t i = 0; i < sizeof table / sizeof table[0]; ++i) {
        if (strcmp(table[i][0], component) == 0 && strcmp(table[i][1], key) == 0) return true;
    }
    return false;
}

/* Tracks edit gestures: a widget becoming active starts a new undo step. */
static void wpe__note_activation(WPE_EditorUi* ui) {
    if (igIsItemActivated()) ++ui->edit_serial;
}

static bool wpe__draw_value(WPE_EditorUi* ui, const char* component, const char* key, const WPE_JsonValue* v,
                            wpe__Edit* edit);

/* Draws one member of a container. Every member is drawn even after an earlier one changed (skipping the
 * rest would make them flicker); only the first change of a frame is kept. */
static bool wpe__draw_member(WPE_EditorUi* ui, const char* component, const char* key, const WPE_JsonValue* v,
                             wpe__Edit* edit, bool already_changed) {
    wpe__Edit member_edit;
    if (!wpe__draw_value(ui, component, key, v, &member_edit)) return false;
    if (!already_changed) *edit = member_edit;
    return true;
}

/* Draws an editable view of `v`; returns true (filling `edit`) when the user changed a leaf. Labels are
 * the keys, so callers give each member its own ID scope (a "mesh" field inside the "mesh" header). */
static bool wpe__draw_value(WPE_EditorUi* ui, const char* component, const char* key, const WPE_JsonValue* v,
                            wpe__Edit* edit) {
    bool changed = false;
    switch (wpe_json_type(v)) {
    case WPE_JSON_NUMBER: {
        float value = (float)wpe_json_number(v, 0.0);
        changed = igDragFloat(key, &value, 0.05f, 0.0f, 0.0f, "%.4g", 0);
        wpe__note_activation(ui);
        if (changed) *edit = (wpe__Edit){.target = v, .type = WPE_JSON_NUMBER, .number = value};
        break;
    }
    case WPE_JSON_BOOL: {
        bool value = wpe_json_bool(v, false);
        changed = igCheckbox(key, &value);
        ++ui->edit_serial;
        if (changed) *edit = (wpe__Edit){.target = v, .type = WPE_JSON_BOOL, .boolean = value};
        break;
    }
    case WPE_JSON_STRING: {
        uint32_t count = 0;
        const char* const* choices = wpe__choices(component, key, &count);
        if (choices && igBeginCombo(key, wpe_json_string(v), 0)) {
            for (uint32_t i = 0; i < count; ++i) {
                if (igSelectable_Bool(choices[i], strcmp(choices[i], wpe_json_string(v)) == 0, 0,
                                      (ImVec2_c){0, 0})) {
                    *edit = (wpe__Edit){.target = v, .type = WPE_JSON_STRING};
                    snprintf(edit->string, sizeof edit->string, "%s", choices[i]);
                    ++ui->edit_serial;
                    changed = true;
                }
            }
            igEndCombo();
        } else if (!choices) {
            char buffer[WPE__UI_PATH];
            snprintf(buffer, sizeof buffer, "%s", wpe_json_string(v));
            /* Applied on Enter or when the field loses focus, not per keystroke. */
            igInputText(key, buffer, sizeof buffer, 0, NULL, NULL);
            if (igIsItemDeactivatedAfterEdit()) {
                /* Clearing an optional asset path removes the asset (null). */
                const bool none = !buffer[0] && wpe__is_asset_key(component, key);
                *edit = (wpe__Edit){.target = v, .type = none ? WPE_JSON_NULL : WPE_JSON_STRING};
                snprintf(edit->string, sizeof edit->string, "%s", buffer);
                ++ui->edit_serial;
                changed = true;
            }
        }
        break;
    }
    case WPE_JSON_ARRAY: {
        const uint32_t count = wpe_json_count(v);
        bool numbers = count >= 2 && count <= 4;
        for (uint32_t i = 0; numbers && i < count; ++i)
            numbers = wpe_json_type(wpe_json_at(v, i)) == WPE_JSON_NUMBER;
        if (numbers) {
            float values[4] = {0};
            for (uint32_t i = 0; i < count; ++i)
                values[i] = (float)wpe_json_number(wpe_json_at(v, i), 0.0);
            changed = count == 2   ? igDragFloat2(key, values, 0.05f, 0, 0, "%.3g", 0)
                      : count == 3 ? igDragFloat3(key, values, 0.05f, 0, 0, "%.3g", 0)
                                   : igDragFloat4(key, values, 0.01f, 0, 0, "%.3g", 0);
            wpe__note_activation(ui);
            if (changed) {
                *edit = (wpe__Edit){.target = v, .type = WPE_JSON_ARRAY, .vector_count = count};
                memcpy(edit->vector, values, sizeof values);
            }
        } else if (igTreeNodeEx_Str(key, 0)) {
            /* Lists (joints, post effects): elements are edited in place. */
            for (uint32_t i = 0; i < count; ++i) {
                char label[16];
                snprintf(label, sizeof label, "[%u]", i);
                igPushID_Int((int)i);
                changed |= wpe__draw_member(ui, component, label, wpe_json_at(v, i), edit, changed);
                igPopID();
            }
            igTreePop();
        }
        break;
    }
    case WPE_JSON_OBJECT:
        if (igTreeNodeEx_Str(key, ImGuiTreeNodeFlags_DefaultOpen)) {
            /* The environment's objects ("fog", "sky") name their own fields' choices; list elements ("[0]")
             * keep the environment's. */
            const char* inner = strcmp(component, "environment") == 0 && key[0] != '[' ? key : component;
            for (uint32_t i = 0; i < wpe_json_count(v); ++i) {
                const char* member_key = NULL;
                const WPE_JsonValue* member = wpe_json_member(v, i, &member_key);
                igPushID_Int((int)i);
                changed |= wpe__draw_member(ui, inner, member_key, member, edit, changed);
                igPopID();
            }
            igTreePop();
        }
        break;
    case WPE_JSON_NULL:
        if (wpe__is_asset_key(component, key)) {
            /* No asset yet: typing a path sets one. */
            char buffer[WPE__UI_PATH] = "";
            igInputTextWithHint(key, "none (asset path)", buffer, sizeof buffer, 0, NULL, NULL);
            if (igIsItemDeactivatedAfterEdit() && buffer[0]) {
                *edit = (wpe__Edit){.target = v, .type = WPE_JSON_STRING};
                snprintf(edit->string, sizeof edit->string, "%s", buffer);
                ++ui->edit_serial;
                changed = true;
            }
        } else {
            igTextDisabled("%s: none", key);
        }
        break;
    }
    return changed;
}

/* ---- Viewport helpers ------------------------------------------------------------------------------ */

static WPE_Ecs* wpe__ui_ecs(const WPE_EditorUi* ui) {
    return wpe_engine_ecs(wpe_editor_engine(ui->editor));
}

/* View-projection of the camera rendering the viewport. */
static bool wpe__view_projection(const WPE_EditorUi* ui, WPE_Mat4* out) {
    WPE_Engine* engine = wpe_editor_engine(ui->editor);
    const WPE_ProjectSettings* p = wpe_engine_project(engine);
    WPE_Mat4 view, projection;
    if (!wpe_scene_camera_matrices(wpe__ui_ecs(ui), wpe_engine_camera(engine),
                                   (float)p->width / (float)p->height, &view, &projection))
        return false;
    *out = wpe_mat4_mul(&projection, &view);
    return true;
}

/* World point to screen (ImGui) coordinates; false when behind the camera. */
static bool wpe__to_screen(const WPE_EditorUi* ui, const WPE_Mat4* view_projection, WPE_Vec3 p,
                           ImVec2_c* out) {
    const WPE_Vec4 clip = wpe_mat4_mul_vec4(view_projection, wpe_vec4(p.x, p.y, p.z, 1.0f));
    if (!(clip.w > 1e-4f)) return false;
    out->x = ui->image_x + (clip.x / clip.w * 0.5f + 0.5f) * ui->image_w;
    out->y = ui->image_y + (0.5f - clip.y / clip.w * 0.5f) * ui->image_h;
    return true;
}

/* Ray through a screen point (world space). */
static bool wpe__ray(const WPE_EditorUi* ui, float sx, float sy, WPE_Vec3* out_origin,
                     WPE_Vec3* out_direction) {
    WPE_Mat4 view_projection, inverse;
    if (!wpe__view_projection(ui, &view_projection) || !wpe_mat4_inverse(&view_projection, &inverse))
        return false;
    const float nx = (sx - ui->image_x) / ui->image_w * 2.0f - 1.0f;
    const float ny = 1.0f - (sy - ui->image_y) / ui->image_h * 2.0f;
    const WPE_Vec4 a = wpe_mat4_mul_vec4(&inverse, wpe_vec4(nx, ny, -1.0f, 1.0f));
    const WPE_Vec4 b = wpe_mat4_mul_vec4(&inverse, wpe_vec4(nx, ny, 1.0f, 1.0f));
    if (fabsf(a.w) < 1e-12f || fabsf(b.w) < 1e-12f) return false;
    *out_origin = wpe_vec3(a.x / a.w, a.y / a.w, a.z / a.w);
    *out_direction = wpe_vec3_normalize(wpe_vec3_sub(wpe_vec3(b.x / b.w, b.y / b.w, b.z / b.w), *out_origin));
    return true;
}

/* Slab test of a local-space ray against a box; returns the entry parameter or -1. */
static float wpe__ray_box(WPE_Vec3 origin, WPE_Vec3 direction, WPE_Vec3 lo, WPE_Vec3 hi) {
    float t0 = 0.0f, t1 = 1e30f;
    for (int i = 0; i < 3; ++i) {
        if (fabsf(direction.v[i]) < 1e-12f) {
            if (origin.v[i] < lo.v[i] || origin.v[i] > hi.v[i]) return -1.0f;
            continue;
        }
        float a = (lo.v[i] - origin.v[i]) / direction.v[i], b = (hi.v[i] - origin.v[i]) / direction.v[i];
        if (a > b) {
            const float swap = a;
            a = b;
            b = swap;
        }
        t0 = a > t0 ? a : t0;
        t1 = b < t1 ? b : t1;
        if (t0 > t1) return -1.0f;
    }
    return t0;
}

/* The nearest mesh under a screen point; selects its outermost ancestor below a model root when the
 * mesh belongs to an instance (clicking a helmet selects the helmet, not its inner node). */
static WPE_Entity wpe__pick(WPE_EditorUi* ui, float sx, float sy) {
    WPE_Vec3 origin, direction;
    if (!wpe__ray(ui, sx, sy, &origin, &direction)) return WPE_ENTITY_NULL;
    WPE_Ecs* ecs = wpe__ui_ecs(ui);
    WPE_Entity best = WPE_ENTITY_NULL;
    float best_distance = 1e30f;
    WPE_EcsIter it = wpe_ecs_query(
        ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_MESH) | WPE_ECS_MASK(WPE_COMP_TRANSFORM)});
    while (wpe_ecs_iter_next(&it)) {
        const WPE_MeshRenderer* mesh = wpe_ecs_iter_get(&it, WPE_COMP_MESH);
        const WPE_Transform* t = wpe_ecs_iter_get(&it, WPE_COMP_TRANSFORM);
        if (!mesh->model || !mesh->visible || mesh->mesh >= mesh->model->mesh_count) continue;
        WPE_Mat4 inverse;
        if (!wpe_mat4_inverse(&t->world, &inverse)) continue;
        const WPE_Vec3 lo = wpe_mat4_transform_point(&inverse, origin);
        const WPE_Vec3 ld = wpe_mat4_transform_dir(&inverse, direction);
        const WPE_Aabb* bounds = &mesh->model->meshes[mesh->mesh].bounds;
        const float hit = wpe__ray_box(lo, ld, bounds->min, bounds->max);
        if (hit < 0.0f) continue;
        const WPE_Vec3 world_hit =
            wpe_mat4_transform_point(&t->world, wpe_vec3_add(lo, wpe_vec3_scale(ld, hit)));
        const float distance = wpe_vec3_length(wpe_vec3_sub(world_hit, origin));
        if (distance < best_distance) {
            best_distance = distance;
            best = it.entity;
        }
    }
    /* Billboards, as the camera sees them: the ray against each quad's plane, inside all four edges. */
    const WPE_Transform* eye =
        wpe_ecs_get_component(ecs, wpe_engine_camera(wpe_editor_engine(ui->editor)), WPE_COMP_TRANSFORM);
    it = wpe_ecs_query(
        ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_BILLBOARD) | WPE_ECS_MASK(WPE_COMP_TRANSFORM)});
    while (eye && wpe_ecs_iter_next(&it)) {
        const WPE_Billboard* b = wpe_ecs_iter_get(&it, WPE_COMP_BILLBOARD);
        const WPE_Transform* t = wpe_ecs_iter_get(&it, WPE_COMP_TRANSFORM);
        WPE_Vec3 c[4], n;
        if (!b->visible || !wpe_billboard_corners(b, &t->world, &eye->world, c, &n)) continue;
        const float facing = wpe_vec3_dot(direction, n);
        if (fabsf(facing) < 1e-6f) continue;
        const float distance = wpe_vec3_dot(wpe_vec3_sub(c[0], origin), n) / facing;
        if (!(distance > 0.0f) || distance >= best_distance) continue;
        const WPE_Vec3 p = wpe_vec3_add(origin, wpe_vec3_scale(direction, distance));
        bool inside = true;
        for (int i = 0; i < 4 && inside; ++i) {
            const WPE_Vec3 edge = wpe_vec3_sub(c[(i + 1) % 4], c[i]);
            inside = wpe_vec3_dot(wpe_vec3_cross(edge, wpe_vec3_sub(p, c[i])), n) >= 0.0f;
        }
        if (!inside) continue;
        best_distance = distance;
        best = it.entity;
    }
    /* Walk up past the "model" roots created by instances to the entity the scene file describes. */
    for (WPE_Entity e = best; e != WPE_ENTITY_NULL; e = wpe_scene_get_parent(ecs, e)) {
        const char* name = wpe_scene_get_name(ecs, e);
        if (name && strcmp(name, "model") == 0 && wpe_scene_get_parent(ecs, e) != WPE_ENTITY_NULL)
            return wpe_scene_get_parent(ecs, e);
    }
    return best;
}

/* ---- Fly camera ------------------------------------------------------------------------------------- */

static void wpe__ui_camera(WPE_EditorUi* ui, float dt) {
    WPE_Ecs* ecs = wpe__ui_ecs(ui);
    WPE_Transform* t = wpe_ecs_get_component(ecs, wpe_editor_camera(ui->editor), WPE_COMP_TRANSFORM);
    if (!t) return;
    if (!ui->camera_initialized) {
        ui->speed = 6.0f;
        ui->camera_initialized = true;
    }
    if (!ui->flying) {
        /* Commands (camera, focus) may have turned the camera: flying starts from where it looks. The angles
         * come from the view direction, not Euler angles, whose middle (Y) angle can't pass +-90 degrees. */
        const WPE_Vec3 forward = wpe_quat_rotate_vec3(t->rotation, wpe_vec3(0, 0, -1));
        const float up = forward.y < -1.0f ? -1.0f : forward.y > 1.0f ? 1.0f : forward.y;
        ui->pitch = asinf(up);
        ui->yaw = atan2f(-forward.x, -forward.z);
    }
    ImGuiIO* io = igGetIO_Nil();
    if (ui->view_hovered && igIsMouseClicked_Bool(ImGuiMouseButton_Right, false)) ui->flying = true;
    if (!igIsMouseDown_Nil(ImGuiMouseButton_Right)) ui->flying = false;
    if (ui->view_hovered && io->MouseWheel != 0.0f && !ui->flying) {
        t->position =
            wpe_vec3_add(t->position, wpe_vec3_scale(wpe_quat_rotate_vec3(t->rotation, wpe_vec3(0, 0, -1)),
                                                     io->MouseWheel * ui->speed * 0.25f));
    }
    if (!ui->flying) return;
    ui->yaw -= io->MouseDelta.x * 0.005f;
    ui->pitch -= io->MouseDelta.y * 0.005f;
    ui->pitch = ui->pitch < -1.55f ? -1.55f : ui->pitch > 1.55f ? 1.55f : ui->pitch;
    if (io->MouseWheel != 0.0f) ui->speed *= io->MouseWheel > 0.0f ? 1.25f : 0.8f;
    t->rotation = wpe_quat_mul(wpe_quat_from_axis_angle(wpe_vec3(0, 1, 0), ui->yaw),
                               wpe_quat_from_axis_angle(wpe_vec3(1, 0, 0), ui->pitch));
    WPE_Vec3 move = wpe_vec3(0, 0, 0);
    if (igIsKeyDown_Nil(ImGuiKey_W)) move.z -= 1.0f;
    if (igIsKeyDown_Nil(ImGuiKey_S)) move.z += 1.0f;
    if (igIsKeyDown_Nil(ImGuiKey_A)) move.x -= 1.0f;
    if (igIsKeyDown_Nil(ImGuiKey_D)) move.x += 1.0f;
    if (igIsKeyDown_Nil(ImGuiKey_E)) move.y += 1.0f;
    if (igIsKeyDown_Nil(ImGuiKey_Q)) move.y -= 1.0f;
    const float speed = ui->speed * (io->KeyShift ? 3.0f : 1.0f) * dt;
    t->position = wpe_vec3_add(t->position, wpe_vec3_scale(wpe_quat_rotate_vec3(t->rotation, move), speed));
}

/* ---- Gizmo ------------------------------------------------------------------------------------------ */

/* Unit vector along axis 0 (X), 1 (Y) or 2 (Z). */
static WPE_Vec3 wpe__unit(int axis) {
    return wpe_vec3(axis == 0 ? 1.0f : 0.0f, axis == 1 ? 1.0f : 0.0f, axis == 2 ? 1.0f : 0.0f);
}

static void wpe__send_transform(WPE_EditorUi* ui, WPE_Entity entity, const WPE_Transform* t) {
    WPE_JsonWriter* w = wpe__ui_begin("set_component");
    if (!w) return;
    wpe_json_write_key(w, "id");
    wpe_json_write_integer(w, entity);
    wpe_json_write_key(w, "component");
    wpe_json_write_string(w, "transform");
    wpe_json_write_key(w, "value");
    wpe_json_write_object_begin(w);
    const struct {
        const char* key;
        const float* v;
        int n;
    } parts[] = {{"position", t->position.v, 3}, {"rotation", t->rotation.v, 4}, {"scale", t->scale.v, 3}};
    for (size_t p = 0; p < 3; ++p) {
        wpe_json_write_key(w, parts[p].key);
        wpe_json_write_array_begin(w, true);
        for (int i = 0; i < parts[p].n; ++i)
            wpe_json_write_number(w, parts[p].v[i]);
        wpe_json_write_array_end(w);
    }
    wpe_json_write_object_end(w);
    char key[32];
    snprintf(key, sizeof key, "gizmo-%u", ui->gizmo_drag);
    wpe_json_write_key(w, "coalesce");
    wpe_json_write_string(w, key);
    wpe__ui_send(ui, w);
}

/* The nearest multiple of `step`. */
static float wpe__snap(float value, float step) {
    return step > 0.0f ? roundf(value / step) * step : value;
}

/* The inverse of a unit quaternion. */
static WPE_Quat wpe__quat_conjugate(WPE_Quat q) {
    return (WPE_Quat){.x = -q.x, .y = -q.y, .z = -q.z, .w = q.w};
}

/* Distance from `p` to the screen segment a..b; `out_s` receives the closest point's parameter (0..1). */
static float wpe__segment_distance(ImVec2_c a, ImVec2_c b, ImVec2_c p, float* out_s) {
    const float dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
    float s = len2 > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2 : 0;
    s = s < 0 ? 0 : s > 1 ? 1 : s;
    const float px = a.x + dx * s - p.x, py = a.y + dy * s - p.y;
    if (out_s) *out_s = s;
    return sqrtf(px * px + py * py);
}

/* Point `i` of the rotation ring around `axis` (unit length, world space). */
/* Where on the line `origin + s * axis` (axis of unit length) the mouse ray through a screen point
 * passes nearest: `s`. False when the ray runs within about 6 degrees of the axis, where `s` jumps
 * wildly with every pixel, or when that point is behind the camera or more than 1000 units out. */
static bool wpe__axis_param(const WPE_EditorUi* ui, WPE_Vec3 origin, WPE_Vec3 axis, float sx, float sy,
                            float* out_s) {
    WPE_Vec3 ray_origin, ray_dir;
    if (!wpe__ray(ui, sx, sy, &ray_origin, &ray_dir)) return false;
    const WPE_Vec3 w = wpe_vec3_sub(origin, ray_origin);
    const float b = wpe_vec3_dot(axis, ray_dir), d = wpe_vec3_dot(axis, w), e = wpe_vec3_dot(ray_dir, w);
    const float denom = 1.0f - b * b;
    if (denom < 0.01f) return false;
    const float s = (b * e - d) / denom, t = (e - b * d) / denom;
    if (!(t > 0.0f && t < 1000.0f)) return false;
    *out_s = s;
    return true;
}

static WPE_Vec3 wpe__ring_point(WPE_Vec3 center, WPE_Vec3 axis, float radius, int i) {
    /* Any vector not parallel to the axis gives the ring's plane basis. */
    const WPE_Vec3 seed = fabsf(axis.x) < 0.9f ? wpe_vec3(1, 0, 0) : wpe_vec3(0, 1, 0);
    const WPE_Vec3 u = wpe_vec3_normalize(wpe_vec3_cross(axis, seed));
    const WPE_Vec3 v = wpe_vec3_cross(axis, u);
    const float theta = 6.28318531f * (float)i / (float)WPE__RING_SEGMENTS;
    return wpe_vec3_add(center, wpe_vec3_add(wpe_vec3_scale(u, radius * cosf(theta)),
                                             wpe_vec3_scale(v, radius * sinf(theta))));
}

/* Draws the gizmo of the selection and handles dragging; returns true while it uses the mouse. Move and
 * scale have a handle per axis; rotate has a ring per axis, dragged along the ring where it was grabbed. */
static bool wpe__ui_gizmo(WPE_EditorUi* ui, ImDrawList* draw) {
    const WPE_Entity entity = wpe_editor_selection(ui->editor);
    WPE_Ecs* ecs = wpe__ui_ecs(ui);
    const WPE_Transform* t = wpe_ecs_get_component(ecs, entity, WPE_COMP_TRANSFORM);
    WPE_Mat4 view_projection;
    if (!t || wpe_editor_mode(ui->editor) != WPE_EDITOR_EDIT || !wpe__view_projection(ui, &view_projection)) {
        ui->gizmo_axis = -1;
        return false;
    }
    /* The transform is expressed in the parent's space; drags are converted into it. */
    WPE_Mat4 parent_world = wpe_mat4_identity();
    const WPE_Entity parent = wpe_scene_get_parent(ecs, entity);
    if (parent) wpe_scene_compute_world_matrix(ecs, parent, &parent_world);
    WPE_Mat4 world;
    wpe_scene_compute_world_matrix(ecs, entity, &world);
    const WPE_EditorView view = wpe_editor_view(ui->editor);
    /* Handle axes in world space: the entity's own (local space, and always for scaling, which acts
     * along them) or the world's. */
    const bool own_axes = view.gizmo_local || view.gizmo == WPE_EDITOR_GIZMO_SCALE;
    const WPE_Vec3 center = wpe_mat4_transform_point(&world, wpe_vec3(0, 0, 0));
    ImVec2_c c;
    if (!wpe__to_screen(ui, &view_projection, center, &c)) return false;
    /* Constant on-screen size: a world length that projects to about 70 pixels. */
    WPE_Vec3 eye_origin, eye_dir;
    if (!wpe__ray(ui, c.x, c.y, &eye_origin, &eye_dir)) return false;
    const float distance = wpe_vec3_length(wpe_vec3_sub(center, eye_origin));
    const float length = distance * 0.18f;
    static const uint32_t colors[3] = {WPE__COL(230, 70, 70, 255), WPE__COL(70, 210, 70, 255),
                                       WPE__COL(70, 110, 240, 255)};
    const bool rotate = view.gizmo == WPE_EDITOR_GIZMO_ROTATE;
    WPE_Vec3 axes[3];
    ImVec2_c ends[3] = {0};
    bool visible[3] = {false, false, false};
    ImVec2_c ring[3][WPE__RING_SEGMENTS] = {0};
    bool ring_visible[3][WPE__RING_SEGMENTS] = {0};
    const ImVec2_c mouse = igGetMousePos();
    int hover = -1, hover_segment = 0;
    float hover_distance = 8.0f;
    for (int a = 0; a < 3; ++a) {
        const WPE_Vec3 own = wpe_mat4_transform_dir(&world, wpe__unit(a));
        axes[a] = own_axes && wpe_vec3_length(own) > 1e-9f ? wpe_vec3_normalize(own) : wpe__unit(a);
        if (rotate) {
            for (int i = 0; i < WPE__RING_SEGMENTS; ++i)
                ring_visible[a][i] = wpe__to_screen(ui, &view_projection,
                                                    wpe__ring_point(center, axes[a], length, i), &ring[a][i]);
            for (int i = 0; i < WPE__RING_SEGMENTS; ++i) {
                const int j = (i + 1) % WPE__RING_SEGMENTS;
                if (!ring_visible[a][i] || !ring_visible[a][j]) continue;
                const float d = wpe__segment_distance(ring[a][i], ring[a][j], mouse, NULL);
                if (d < hover_distance) {
                    hover_distance = d;
                    hover = a;
                    hover_segment = i;
                }
            }
            continue;
        }
        visible[a] = wpe__to_screen(ui, &view_projection,
                                    wpe_vec3_add(center, wpe_vec3_scale(axes[a], length)), &ends[a]);
        if (!visible[a]) continue;
        /* A handle pointing nearly at the camera is too short to aim a drag with, so it can't be taken. */
        const float hx = ends[a].x - c.x, hy = ends[a].y - c.y;
        if (hx * hx + hy * hy < 12.0f * 12.0f) continue;
        float s = 0.0f;
        const float d = wpe__segment_distance(c, ends[a], mouse, &s);
        if (d < hover_distance && s > 0.15f) {
            hover_distance = d;
            hover = a;
        }
    }
    if (ui->gizmo_axis < 0) ui->gizmo_hover = ui->view_hovered ? hover : -1;
    for (int a = 0; a < 3; ++a) {
        const bool hot = a == ui->gizmo_axis || (ui->gizmo_axis < 0 && a == ui->gizmo_hover);
        const uint32_t color = hot ? WPE__COL(250, 220, 60, 255) : colors[a];
        if (rotate) {
            for (int i = 0; i < WPE__RING_SEGMENTS; ++i) {
                const int j = (i + 1) % WPE__RING_SEGMENTS;
                if (ring_visible[a][i] && ring_visible[a][j])
                    ImDrawList_AddLine(draw, ring[a][i], ring[a][j], color, hot ? 3.5f : 2.5f);
            }
            continue;
        }
        if (!visible[a]) continue;
        ImDrawList_AddLine(draw, c, ends[a], color, 3.0f);
        if (view.gizmo == WPE_EDITOR_GIZMO_SCALE) {
            ImDrawList_AddRectFilled(draw, (ImVec2_c){ends[a].x - 4, ends[a].y - 4},
                                     (ImVec2_c){ends[a].x + 4, ends[a].y + 4}, color, 0.0f, 0);
        } else {
            ImDrawList_AddCircleFilled(draw, ends[a], 4.0f, color, 12);
        }
    }
    ImDrawList_AddCircleFilled(draw, c, 3.0f, WPE__COL(255, 255, 255, 255), 8);

    if (ui->gizmo_axis < 0 && ui->gizmo_hover >= 0 && igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) {
        ui->gizmo_axis = ui->gizmo_hover;
        ui->gizmo_start = *t;
        ui->gizmo_mouse_x = mouse.x;
        ui->gizmo_mouse_y = mouse.y;
        ui->gizmo_start_center = center;
        ++ui->gizmo_drag;
        if (rotate) {
            /* The screen direction in which the grabbed ring point moves under a positive rotation. */
            const int a = ui->gizmo_axis;
            const WPE_Vec3 grabbed = wpe__ring_point(center, axes[a], length, hover_segment);
            const WPE_Vec3 tangent = wpe_vec3_cross(axes[a], wpe_vec3_sub(grabbed, center));
            ImVec2_c p0, p1;
            float tx = 0.0f, ty = 0.0f;
            if (wpe__to_screen(ui, &view_projection, grabbed, &p0) &&
                wpe__to_screen(ui, &view_projection, wpe_vec3_add(grabbed, wpe_vec3_scale(tangent, 0.1f)),
                               &p1)) {
                tx = p1.x - p0.x;
                ty = p1.y - p0.y;
            }
            const float tl = sqrtf(tx * tx + ty * ty);
            ui->gizmo_tangent_x = tl > 1e-3f ? tx / tl : 0.0f;
            ui->gizmo_tangent_y = tl > 1e-3f ? ty / tl : 0.0f;
        }
    }
    if (ui->gizmo_axis < 0) return false;
    if (!igIsMouseDown_Nil(ImGuiMouseButton_Left)) {
        ui->gizmo_axis = -1;
        return true;
    }
    const int a = ui->gizmo_axis;
    if (a > 2) return true;
    /* Ctrl inverts the snap setting for this drag. */
    const bool snap = view.snap != igGetIO_Nil()->KeyCtrl;
    const float mx = mouse.x - ui->gizmo_mouse_x, my = mouse.y - ui->gizmo_mouse_y;
    WPE_Transform moved = ui->gizmo_start;
    if (rotate) {
        /* Mouse movement along the grabbed point's tangent: 0.6 degrees per pixel. A world-space turn
         * about the axis, expressed in the parent's space: parent^-1 * turn * parent * rotation. */
        float angle = (mx * ui->gizmo_tangent_x + my * ui->gizmo_tangent_y) * 0.01f;
        if (snap) angle = wpe__snap(angle, view.snap_rotate * 0.0174532925f);
        WPE_Vec3 parent_position, parent_scale;
        WPE_Quat parent_rotation = wpe_quat_identity();
        if (!wpe_mat4_decompose(&parent_world, &parent_position, &parent_rotation, &parent_scale))
            parent_rotation = wpe_quat_identity();
        parent_rotation = wpe_quat_normalize(parent_rotation);
        const WPE_Quat turn = wpe_quat_from_axis_angle(axes[a], angle);
        const WPE_Quat in_parent =
            wpe_quat_mul(wpe__quat_conjugate(parent_rotation), wpe_quat_mul(turn, parent_rotation));
        moved.rotation = wpe_quat_normalize(wpe_quat_mul(in_parent, moved.rotation));
        char label[32];
        snprintf(label, sizeof label, "%.1f deg", (double)(angle * 57.2957795f));
        ImDrawList_AddText_Vec2(draw, (ImVec2_c){c.x + 12.0f, c.y - 22.0f}, WPE__COL(255, 255, 255, 230),
                                label, NULL);
        wpe__send_transform(ui, entity, &moved);
        return true;
    }
    if (!visible[a]) return true;
    const float dx = ends[a].x - c.x, dy = ends[a].y - c.y;
    const float screen_length = sqrtf(dx * dx + dy * dy);
    if (screen_length < 1e-3f) return true;
    /* Mouse movement along the handle's screen direction, in pixels. */
    const float along = (mx * dx + my * dy) / screen_length;
    char label[32];
    if (view.gizmo == WPE_EDITOR_GIZMO_MOVE) {
        /* World units along the axis: from the axis point nearest the mouse ray when the drag started to
         * the one nearest it now, then into the parent's space. */
        float start_s, now_s;
        if (!wpe__axis_param(ui, ui->gizmo_start_center, axes[a], ui->gizmo_mouse_x, ui->gizmo_mouse_y,
                             &start_s) ||
            !wpe__axis_param(ui, ui->gizmo_start_center, axes[a], mouse.x, mouse.y, &now_s))
            return true; /* No usable point (the axis points along the ray): the entity stays put. */
        float amount = now_s - start_s;
        if (snap && view.gizmo_local) {
            amount = wpe__snap(amount, view.snap_move); /* Whole steps along the entity's own axis. */
        } else if (snap) {
            /* World axes: the world coordinate lands on the grid of steps. */
            const float start = ui->gizmo_start_center.v[a];
            amount = wpe__snap(start + amount, view.snap_move) - start;
        }
        const WPE_Vec3 delta = wpe_vec3_scale(axes[a], amount);
        WPE_Mat4 to_parent;
        if (!wpe_mat4_inverse(&parent_world, &to_parent)) return true;
        moved.position = wpe_vec3_add(moved.position, wpe_mat4_transform_dir(&to_parent, delta));
        snprintf(label, sizeof label, "%+.3g", (double)amount);
    } else {
        float scale = moved.scale.v[a] * expf(along * 0.01f);
        if (snap) {
            /* On the step's multiples, never collapsing to zero. */
            const float snapped = wpe__snap(scale, view.snap_scale);
            scale = fabsf(snapped) >= view.snap_scale ? snapped
                    : scale < 0.0f                    ? -view.snap_scale
                                                      : view.snap_scale;
        }
        moved.scale.v[a] = scale;
        snprintf(label, sizeof label, "%.3g", (double)scale);
    }
    ImDrawList_AddText_Vec2(draw, (ImVec2_c){c.x + 12.0f, c.y - 22.0f}, WPE__COL(255, 255, 255, 230), label,
                            NULL);
    wpe__send_transform(ui, entity, &moved);
    return true;
}

/* ---- Light icons ------------------------------------------------------------------------------------ */

/* Draws one light's icon at `at`, in its color: a bulb for point lights, a cone along the light for spots,
 * and a sun with an arrow along the light for directional ones. */
static void wpe__draw_light_icon(ImDrawList* draw, const WPE_Light* light, ImVec2_c at, ImVec2_c aim,
                                 bool aimed, bool selected) {
    /* Normalized to its brightest channel, so dim lights stay visible. */
    const float peak = fmaxf(fmaxf(light->color.x, light->color.y), fmaxf(light->color.z, 1e-3f));
    const uint32_t color =
        WPE__COL((int)(light->color.x / peak * 255.0f), (int)(light->color.y / peak * 255.0f),
                 (int)(light->color.z / peak * 255.0f), 255);
    const uint32_t shadow = WPE__COL(15, 15, 18, 190);
    const float dx = aim.x - at.x, dy = aim.y - at.y;
    const float length = sqrtf(dx * dx + dy * dy);
    aimed = aimed && length > 12.0f;
    const float ux = aimed ? dx / length : 0.0f, uy = aimed ? dy / length : 0.0f;
    if (aimed && light->type == WPE_LIGHT_SPOT) {
        /* The cone's on-screen width follows its outer angle. */
        float half = length * tanf(fminf(light->outer_cone, 1.3f));
        half = half < 4.0f ? 4.0f : half > length ? length : half;
        const ImVec2_c left = {aim.x - uy * half, aim.y + ux * half},
                       right = {aim.x + uy * half, aim.y - ux * half};
        ImDrawList_AddLine(draw, at, left, shadow, 3.5f);
        ImDrawList_AddLine(draw, at, right, shadow, 3.5f);
        ImDrawList_AddLine(draw, left, right, shadow, 3.5f);
        ImDrawList_AddLine(draw, at, left, color, 1.5f);
        ImDrawList_AddLine(draw, at, right, color, 1.5f);
        ImDrawList_AddLine(draw, left, right, color, 1.5f);
    } else if (aimed) {
        const ImVec2_c head = {aim.x - ux * 8.0f, aim.y - uy * 8.0f};
        ImDrawList_AddLine(draw, at, head, shadow, 3.5f);
        ImDrawList_AddLine(draw, at, head, color, 1.5f);
        ImDrawList_AddTriangleFilled(draw, aim, (ImVec2_c){head.x - uy * 5.0f, head.y + ux * 5.0f},
                                     (ImVec2_c){head.x + uy * 5.0f, head.y - ux * 5.0f}, color);
    }
    ImDrawList_AddCircleFilled(draw, at, 10.0f, shadow, 20);
    if (light->type == WPE_LIGHT_DIRECTIONAL) {
        for (int i = 0; i < 8; ++i) {
            const float angle = (float)i * 0.785398f, cx = cosf(angle), cy = sinf(angle);
            ImDrawList_AddLine(draw, (ImVec2_c){at.x + cx * 6.0f, at.y + cy * 6.0f},
                               (ImVec2_c){at.x + cx * 9.0f, at.y + cy * 9.0f}, color, 1.5f);
        }
        ImDrawList_AddCircleFilled(draw, at, 4.0f, color, 12);
    } else if (light->type == WPE_LIGHT_POINT) {
        ImDrawList_AddCircle(draw, at, 7.5f, color, 20, 1.5f);
        ImDrawList_AddCircleFilled(draw, at, 4.0f, color, 12);
    } else {
        ImDrawList_AddCircleFilled(draw, at, 5.0f, color, 12);
    }
    if (selected) ImDrawList_AddCircle(draw, at, 12.5f, WPE__COL(237, 153, 56, 255), 24, 2.0f);
}

/* A particle emitter's icon: a four-pointed sparkle with small diagonal glints, in the emitter's starting
 * color (brightened like a light's, so dark smoke still shows). */
static void wpe__draw_particle_icon(ImDrawList* draw, const WPE_ParticleEmitter* emitter, ImVec2_c at,
                                    bool selected) {
    const WPE_Vec4 c = emitter->color_start;
    const float peak = fmaxf(fmaxf(c.x, c.y), fmaxf(c.z, 1e-3f));
    const float lift = peak < 0.6f ? 0.6f / peak : 1.0f / peak; /* Grey smoke reads as light grey. */
    const uint32_t color =
        WPE__COL((int)fminf(c.x * lift * 255.0f, 255.0f), (int)fminf(c.y * lift * 255.0f, 255.0f),
                 (int)fminf(c.z * lift * 255.0f, 255.0f), 255);
    ImDrawList_AddCircleFilled(draw, at, 10.0f, WPE__COL(15, 15, 18, 190), 20);
    const ImVec2_c star[8] = {{at.x, at.y - 8}, {at.x + 2, at.y - 2}, {at.x + 8, at.y}, {at.x + 2, at.y + 2},
                              {at.x, at.y + 8}, {at.x - 2, at.y + 2}, {at.x - 8, at.y}, {at.x - 2, at.y - 2}};
    ImDrawList_AddConvexPolyFilled(draw, star, 8, color);
    const float d = 5.0f;
    ImDrawList_AddLine(draw, (ImVec2_c){at.x + d, at.y - d}, (ImVec2_c){at.x + d + 2, at.y - d - 2}, color,
                       1.5f);
    ImDrawList_AddLine(draw, (ImVec2_c){at.x - d, at.y + d}, (ImVec2_c){at.x - d - 2, at.y + d + 2}, color,
                       1.5f);
    if (selected) ImDrawList_AddCircle(draw, at, 12.5f, WPE__COL(237, 153, 56, 255), 24, 2.0f);
}

/* Draws an icon at every light and particle emitter in edit mode (when the view shows them) and returns the
 * entity whose icon is under the mouse, if any. */
static WPE_Entity wpe__ui_light_icons(WPE_EditorUi* ui, ImDrawList* draw) {
    WPE_Mat4 view_projection;
    WPE_Vec3 eye, eye_dir;
    if (!wpe_editor_view(ui->editor).icons || wpe_editor_mode(ui->editor) != WPE_EDITOR_EDIT ||
        !wpe__view_projection(ui, &view_projection) ||
        !wpe__ray(ui, ui->image_x + ui->image_w * 0.5f, ui->image_y + ui->image_h * 0.5f, &eye, &eye_dir))
        return WPE_ENTITY_NULL;
    WPE_Ecs* ecs = wpe__ui_ecs(ui);
    const WPE_Entity selection = wpe_editor_selection(ui->editor);
    const ImVec2_c mouse = igGetMousePos();
    WPE_Entity hovered = WPE_ENTITY_NULL;
    float hovered_distance = 12.0f;
    ImDrawList_PushClipRect(draw, (ImVec2_c){ui->image_x, ui->image_y},
                            (ImVec2_c){ui->image_x + ui->image_w, ui->image_y + ui->image_h}, true);
    WPE_EcsIter it = wpe_ecs_query(
        ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_LIGHT) | WPE_ECS_MASK(WPE_COMP_TRANSFORM)});
    while (wpe_ecs_iter_next(&it)) {
        const WPE_Light* light = wpe_ecs_iter_get(&it, WPE_COMP_LIGHT);
        const WPE_Transform* t = wpe_ecs_iter_get(&it, WPE_COMP_TRANSFORM);
        const WPE_Vec3 position = wpe_mat4_transform_point(&t->world, wpe_vec3(0, 0, 0));
        ImVec2_c at, aim = {0};
        if (!wpe__to_screen(ui, &view_projection, position, &at)) continue;
        /* The direction line has a constant on-screen length, like the gizmo. */
        bool aimed = false;
        const WPE_Vec3 direction = wpe_mat4_transform_dir(&t->world, wpe_vec3(0, 0, -1));
        if (light->type != WPE_LIGHT_POINT && wpe_vec3_length(direction) > 1e-9f) {
            const float length = wpe_vec3_length(wpe_vec3_sub(position, eye)) * 0.12f;
            aimed = wpe__to_screen(
                ui, &view_projection,
                wpe_vec3_add(position, wpe_vec3_scale(wpe_vec3_normalize(direction), length)), &aim);
        }
        wpe__draw_light_icon(draw, light, at, aim, aimed, it.entity == selection);
        const float distance =
            sqrtf((mouse.x - at.x) * (mouse.x - at.x) + (mouse.y - at.y) * (mouse.y - at.y));
        if (ui->view_hovered && distance < hovered_distance) {
            hovered_distance = distance;
            hovered = it.entity;
        }
    }
    /* Particle emitters: a sparkle in their starting color, at the emitter. */
    it = wpe_ecs_query(
        ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_PARTICLES) | WPE_ECS_MASK(WPE_COMP_TRANSFORM)});
    while (wpe_ecs_iter_next(&it)) {
        const WPE_ParticleEmitter* emitter = wpe_ecs_iter_get(&it, WPE_COMP_PARTICLES);
        const WPE_Transform* t = wpe_ecs_iter_get(&it, WPE_COMP_TRANSFORM);
        ImVec2_c at;
        if (!wpe__to_screen(ui, &view_projection, wpe_mat4_transform_point(&t->world, wpe_vec3(0, 0, 0)),
                            &at))
            continue;
        wpe__draw_particle_icon(draw, emitter, at, it.entity == selection);
        const float distance =
            sqrtf((mouse.x - at.x) * (mouse.x - at.x) + (mouse.y - at.y) * (mouse.y - at.y));
        if (ui->view_hovered && distance < hovered_distance) {
            hovered_distance = distance;
            hovered = it.entity;
        }
    }
    ImDrawList_PopClipRect(draw);
    return hovered;
}

/* ---- 2D view ---------------------------------------------------------------------------------------- */

/* What a 2D drag changes: the element's edges, or all of it. */
enum {
    WPE__EDGE_LEFT = 1,
    WPE__EDGE_RIGHT = 2,
    WPE__EDGE_TOP = 4,
    WPE__EDGE_BOTTOM = 8,
    WPE__EDGE_MOVE = 16,
};

/* Framebuffer pixels to the viewport on screen. */
static ImVec2_c wpe__fb_to_screen(const WPE_EditorUi* ui, float x, float y) {
    return (ImVec2_c){ui->image_x + x * ui->image_w / (float)ui->view_width,
                      ui->image_y + y * ui->image_h / (float)ui->view_height};
}

/* The rectangle a UI element is placed in: its parent element's, or the screen's. */
static WPE_Rect wpe__ui_parent_rect(const WPE_EditorUi* ui, WPE_Entity entity) {
    WPE_Ecs* ecs = wpe__ui_ecs(ui);
    const WPE_UiRect* parent = wpe_ecs_get_component(ecs, wpe_scene_get_parent(ecs, entity), WPE_COMP_UI);
    return parent ? parent->rect : (WPE_Rect){0, 0, (float)ui->view_width, (float)ui->view_height};
}

static void wpe__write_pair(WPE_JsonWriter* w, const char* key, WPE_Vec2 v) {
    wpe_json_write_key(w, key);
    wpe_json_write_array_begin(w, true);
    wpe_json_write_number(w, v.x);
    wpe_json_write_number(w, v.y);
    wpe_json_write_array_end(w);
}

/* Replaces an element's "ui" component; drags share one undo step through `coalesce`. */
static void wpe__send_ui_rect(WPE_EditorUi* ui, WPE_Entity entity, const WPE_UiRect* r,
                              const char* coalesce) {
    WPE_JsonWriter* w = wpe__ui_begin("set_component");
    if (!w) return;
    wpe_json_write_key(w, "id");
    wpe_json_write_integer(w, entity);
    wpe_json_write_key(w, "component");
    wpe_json_write_string(w, "ui");
    wpe_json_write_key(w, "value");
    wpe_json_write_object_begin(w);
    wpe__write_pair(w, "anchor_min", r->anchor_min);
    wpe__write_pair(w, "anchor_max", r->anchor_max);
    wpe__write_pair(w, "offset_min", r->offset_min);
    wpe__write_pair(w, "offset_max", r->offset_max);
    wpe_json_write_key(w, "visible");
    wpe_json_write_bool(w, r->visible);
    wpe_json_write_object_end(w);
    wpe_json_write_key(w, "coalesce");
    wpe_json_write_string(w, coalesce);
    wpe__ui_send(ui, w);
}

/* A small diamond marking an anchor. */
static void wpe__draw_anchor(ImDrawList* draw, ImVec2_c p, uint32_t color) {
    const ImVec2_c points[4] = {{p.x, p.y - 5}, {p.x + 5, p.y}, {p.x, p.y + 5}, {p.x - 5, p.y}};
    ImDrawList_AddConvexPolyFilled(draw, points, 4, WPE__COL(15, 15, 18, 200));
    ImDrawList_AddPolyline(draw, points, 4, color, 1.5f, ImDrawFlags_Closed);
}

/* Starts dragging an element: `edges` are WPE__EDGE_* bits. */
static void wpe__canvas_begin(WPE_EditorUi* ui, WPE_Entity entity, const WPE_UiRect* element, int edges) {
    const ImVec2_c mouse = igGetMousePos();
    ui->canvas_entity = entity;
    ui->canvas_edges = edges;
    ui->canvas_start = *element;
    ui->canvas_mouse_x = mouse.x;
    ui->canvas_mouse_y = mouse.y;
    ++ui->canvas_drag;
}

/* The 2D view's overlay in edit mode: an outline around every shown UI element, and the selected element
 * with its anchors and handles. Clicking picks an element (and dragging moves it); the handles resize it.
 * Returns whether the mouse went to the overlay this frame. */
static bool wpe__ui_canvas(WPE_EditorUi* ui, ImDrawList* draw) {
    WPE_Ecs* ecs = wpe__ui_ecs(ui);
    if (wpe_editor_mode(ui->editor) != WPE_EDITOR_EDIT || ui->view_width == 0 || ui->view_height == 0) {
        ui->canvas_edges = 0;
        return false;
    }
    ImDrawList_PushClipRect(draw, (ImVec2_c){ui->image_x, ui->image_y},
                            (ImVec2_c){ui->image_x + ui->image_w, ui->image_y + ui->image_h}, true);
    WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_UI)});
    while (wpe_ecs_iter_next(&it)) {
        const WPE_UiRect* element = wpe_ecs_iter_get(&it, WPE_COMP_UI);
        if (!element->shown) continue;
        const WPE_Rect r = element->rect;
        ImDrawList_AddRect(draw, wpe__fb_to_screen(ui, r.x, r.y),
                           wpe__fb_to_screen(ui, r.x + r.width, r.y + r.height), WPE__COL(255, 255, 255, 45),
                           0.0f, 1.0f, 0);
    }
    /* The selected element: its rectangle, its anchors in the parent, and eight handles. */
    const WPE_Entity selection = wpe_editor_selection(ui->editor);
    const WPE_UiRect* selected = wpe_ecs_get_component(ecs, selection, WPE_COMP_UI);
    const ImVec2_c mouse = igGetMousePos();
    int hover = 0;
    if (selected) {
        const WPE_Rect r = selected->rect;
        const ImVec2_c a = wpe__fb_to_screen(ui, r.x, r.y),
                       b = wpe__fb_to_screen(ui, r.x + r.width, r.y + r.height);
        const uint32_t accent = WPE__COL(237, 153, 56, 255);
        const WPE_Rect p = wpe__ui_parent_rect(ui, selection);
        const ImVec2_c anchor_min = wpe__fb_to_screen(ui, p.x + p.width * selected->anchor_min.x,
                                                      p.y + p.height * selected->anchor_min.y);
        const ImVec2_c anchor_max = wpe__fb_to_screen(ui, p.x + p.width * selected->anchor_max.x,
                                                      p.y + p.height * selected->anchor_max.y);
        wpe__draw_anchor(draw, anchor_min, WPE__COL(120, 200, 255, 255));
        if (anchor_max.x != anchor_min.x || anchor_max.y != anchor_min.y)
            wpe__draw_anchor(draw, anchor_max, WPE__COL(120, 200, 255, 255));
        ImDrawList_AddRect(draw, a, b, accent, 0.0f, 2.0f, 0);
        const struct {
            float x, y;
            int edges;
        } handles[] = {
            {a.x, a.y, WPE__EDGE_LEFT | WPE__EDGE_TOP},    {b.x, a.y, WPE__EDGE_RIGHT | WPE__EDGE_TOP},
            {a.x, b.y, WPE__EDGE_LEFT | WPE__EDGE_BOTTOM}, {b.x, b.y, WPE__EDGE_RIGHT | WPE__EDGE_BOTTOM},
            {(a.x + b.x) * 0.5f, a.y, WPE__EDGE_TOP},      {(a.x + b.x) * 0.5f, b.y, WPE__EDGE_BOTTOM},
            {a.x, (a.y + b.y) * 0.5f, WPE__EDGE_LEFT},     {b.x, (a.y + b.y) * 0.5f, WPE__EDGE_RIGHT},
        };
        for (size_t i = 0; i < sizeof handles / sizeof handles[0]; ++i) {
            const bool hot = fabsf(mouse.x - handles[i].x) <= 6.0f && fabsf(mouse.y - handles[i].y) <= 6.0f;
            if (hot && !hover) hover = handles[i].edges;
            const int active = ui->canvas_edges ? ui->canvas_edges : hover;
            const uint32_t fill = active == handles[i].edges ? WPE__COL(250, 220, 60, 255) : accent;
            ImDrawList_AddRectFilled(draw, (ImVec2_c){handles[i].x - 4, handles[i].y - 4},
                                     (ImVec2_c){handles[i].x + 4, handles[i].y + 4}, fill, 0, 0);
        }
    }
    ImDrawList_PopClipRect(draw);
    if (!ui->view_hovered) hover = 0;
    const int shape = ui->canvas_edges ? ui->canvas_edges : hover;
    if (shape == (WPE__EDGE_LEFT | WPE__EDGE_TOP) || shape == (WPE__EDGE_RIGHT | WPE__EDGE_BOTTOM)) {
        igSetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
    } else if (shape == (WPE__EDGE_RIGHT | WPE__EDGE_TOP) || shape == (WPE__EDGE_LEFT | WPE__EDGE_BOTTOM)) {
        igSetMouseCursor(ImGuiMouseCursor_ResizeNESW);
    } else if (shape == WPE__EDGE_LEFT || shape == WPE__EDGE_RIGHT) {
        igSetMouseCursor(ImGuiMouseCursor_ResizeEW);
    } else if (shape == WPE__EDGE_TOP || shape == WPE__EDGE_BOTTOM) {
        igSetMouseCursor(ImGuiMouseCursor_ResizeNS);
    } else if (shape == WPE__EDGE_MOVE) {
        igSetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }

    /* A click: on a handle resizes the selection; elsewhere it picks the element there, which can then be
     * dragged right away. */
    if (ui->canvas_edges == 0 && ui->view_hovered && igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) {
        if (hover) {
            wpe__canvas_begin(ui, selection, selected, hover);
            return true;
        }
        const float fx = (mouse.x - ui->image_x) * (float)ui->view_width / ui->image_w;
        const float fy = (mouse.y - ui->image_y) * (float)ui->view_height / ui->image_h;
        const WPE_Entity picked = wpe_ui_pick(wpe_engine_ui(wpe_editor_engine(ui->editor)), fx, fy, true);
        if (picked == WPE_ENTITY_NULL) {
            WPE_JsonWriter* w = wpe__ui_begin("select");
            if (w) {
                wpe_json_write_key(w, "id");
                wpe_json_write_null(w);
                wpe__ui_send(ui, w);
            }
            return true;
        }
        if (picked != selection) wpe__ui_entity_command(ui, "select", picked);
        const WPE_UiRect* element = wpe_ecs_get_component(ecs, picked, WPE_COMP_UI);
        if (element) wpe__canvas_begin(ui, picked, element, WPE__EDGE_MOVE);
        return true;
    }
    if (ui->canvas_edges == 0) return false;
    if (!igIsMouseDown_Nil(ImGuiMouseButton_Left) ||
        !wpe_ecs_has_component(ecs, ui->canvas_entity, WPE_COMP_UI)) {
        ui->canvas_edges = 0;
        return true;
    }
    /* Offsets follow the mouse in framebuffer pixels: whole pixels, or steps when snapping (Ctrl inverts
     * the View menu's setting). */
    const WPE_EditorView view = wpe_editor_view(ui->editor);
    const float step = view.snap != igGetIO_Nil()->KeyCtrl ? view.snap_ui : 1.0f;
    const float dx = (mouse.x - ui->canvas_mouse_x) * (float)ui->view_width / ui->image_w;
    const float dy = (mouse.y - ui->canvas_mouse_y) * (float)ui->view_height / ui->image_h;
    if (dx == 0.0f && dy == 0.0f) return true; /* Not moved (yet): a click only selects. */
    const WPE_UiRect start = ui->canvas_start;
    WPE_UiRect moved = start;
    const int edges = ui->canvas_edges;
    if (edges == WPE__EDGE_MOVE) {
        const float shift_x = wpe__snap(start.offset_min.x + dx, step) - start.offset_min.x;
        const float shift_y = wpe__snap(start.offset_min.y + dy, step) - start.offset_min.y;
        moved.offset_min = (WPE_Vec2){.x = start.offset_min.x + shift_x, .y = start.offset_min.y + shift_y};
        moved.offset_max = (WPE_Vec2){.x = start.offset_max.x + shift_x, .y = start.offset_max.y + shift_y};
    } else {
        if (edges & WPE__EDGE_LEFT) moved.offset_min.x = wpe__snap(start.offset_min.x + dx, step);
        if (edges & WPE__EDGE_RIGHT) moved.offset_max.x = wpe__snap(start.offset_max.x + dx, step);
        if (edges & WPE__EDGE_TOP) moved.offset_min.y = wpe__snap(start.offset_min.y + dy, step);
        if (edges & WPE__EDGE_BOTTOM) moved.offset_max.y = wpe__snap(start.offset_max.y + dy, step);
    }
    char coalesce[32];
    snprintf(coalesce, sizeof coalesce, "ui2d-%u", ui->canvas_drag);
    wpe__send_ui_rect(ui, ui->canvas_entity, &moved, coalesce);
    const WPE_UiRect* now = wpe_ecs_get_component(ecs, ui->canvas_entity, WPE_COMP_UI);
    if (now) {
        char label[48];
        snprintf(label, sizeof label, "%.0f, %.0f  %.0f x %.0f", (double)now->rect.x, (double)now->rect.y,
                 (double)now->rect.width, (double)now->rect.height);
        ImDrawList_AddText_Vec2(draw, (ImVec2_c){mouse.x + 14, mouse.y + 4}, WPE__COL(255, 255, 255, 230),
                                label, NULL);
    }
    return true;
}

/* ---- Panels ----------------------------------------------------------------------------------------- */

/* Places a model or prefab 6 units in front of the editor camera. */
static void wpe__ui_create_model(WPE_EditorUi* ui, const char* path) {
    WPE_Vec3 position = wpe_vec3(0, 0, 0);
    const WPE_Transform* cam =
        wpe_ecs_get_component(wpe__ui_ecs(ui), wpe_editor_camera(ui->editor), WPE_COMP_TRANSFORM);
    if (cam)
        position = wpe_vec3_add(
            cam->position, wpe_vec3_scale(wpe_quat_rotate_vec3(cam->rotation, wpe_vec3(0, 0, -1)), 6.0f));
    const char* slash = strrchr(path, '/');
    char name[64];
    snprintf(name, sizeof name, "%s", slash ? slash + 1 : path);
    char* dot = strrchr(name, '.');
    if (dot) *dot = '\0';
    WPE_JsonWriter* w = wpe__ui_begin("create_entity");
    if (!w) return;
    wpe_json_write_key(w, "entity");
    wpe_json_write_object_begin(w);
    wpe_json_write_key(w, "name");
    wpe_json_write_string(w, name);
    wpe_json_write_key(w, "transform");
    wpe_json_write_object_begin(w);
    wpe_json_write_key(w, "position");
    wpe_json_write_array_begin(w, true);
    for (int i = 0; i < 3; ++i)
        wpe_json_write_number(w, roundf(position.v[i] * 10.0f) / 10.0f);
    wpe_json_write_array_end(w);
    wpe_json_write_object_end(w);
    /* A prefab becomes a linked instance; a model is copied in. */
    const bool prefab = wpe_prefab_is_path(path);
    wpe_json_write_key(w, prefab ? "prefab" : "instance");
    wpe_json_write_object_begin(w);
    wpe_json_write_key(w, prefab ? "path" : "model");
    wpe_json_write_string(w, path);
    wpe_json_write_object_end(w);
    wpe_json_write_object_end(w);
    wpe__ui_send(ui, w);
}

/* ---- New UI elements -------------------------------------------------------------------------------- */

static bool wpe__is_image_path(const char* path) {
    const size_t n = strlen(path);
    return (n > 4 && (strcmp(path + n - 4, ".png") == 0 || strcmp(path + n - 4, ".jpg") == 0)) ||
           (n > 5 && strcmp(path + n - 5, ".jpeg") == 0);
}

/* "ui": a width x height rectangle centered in its parent. */
static void wpe__write_centered_ui(WPE_JsonWriter* w, float width, float height) {
    const float hw = floorf(width * 0.5f), hh = floorf(height * 0.5f);
    wpe_json_write_key(w, "ui");
    wpe_json_write_object_begin(w);
    wpe__write_pair(w, "anchor_min", (WPE_Vec2){.x = 0.5f, .y = 0.5f});
    wpe__write_pair(w, "anchor_max", (WPE_Vec2){.x = 0.5f, .y = 0.5f});
    wpe__write_pair(w, "offset_min", (WPE_Vec2){.x = -hw, .y = -hh});
    wpe__write_pair(w, "offset_max", (WPE_Vec2){.x = width - hw, .y = height - hh});
    wpe_json_write_object_end(w);
}

static void wpe__write_color(WPE_JsonWriter* w, const char* key, float r, float g, float b, float a) {
    wpe_json_write_key(w, key);
    wpe_json_write_array_begin(w, true);
    const float c[4] = {r, g, b, a};
    for (int i = 0; i < 4; ++i)
        wpe_json_write_number(w, c[i]);
    wpe_json_write_array_end(w);
}

/* Starts a create_entity of a UI element: its name, and the selected UI element as its parent (so menus
 * are built by selecting the panel first). The caller writes the components and ends the entity object. */
static WPE_JsonWriter* wpe__begin_ui_element(WPE_EditorUi* ui, const char* name, WPE_Entity parent) {
    WPE_JsonWriter* w = wpe__ui_begin("create_entity");
    if (!w) return NULL;
    char coalesce[32];
    snprintf(coalesce, sizeof coalesce, "add-ui-%u", ui->canvas_drag);
    wpe_json_write_key(w, "coalesce");
    wpe_json_write_string(w, coalesce);
    wpe_json_write_key(w, "entity");
    wpe_json_write_object_begin(w);
    wpe_json_write_key(w, "name");
    wpe_json_write_string(w, name);
    if (wpe_ecs_has_component(wpe__ui_ecs(ui), parent, WPE_COMP_UI)) {
        wpe_json_write_key(w, "parent");
        wpe_json_write_integer(w, parent);
    }
    return w;
}

/* A billboard of an image (NULL: a plain quad) in front of the editor camera: one unit tall, `aspect`
 * (width / height) wide. */
static void wpe__ui_create_billboard(WPE_EditorUi* ui, const char* name, const char* image, float aspect) {
    WPE_Vec3 position = wpe_vec3(0, 0, 0);
    const WPE_Transform* cam =
        wpe_ecs_get_component(wpe__ui_ecs(ui), wpe_editor_camera(ui->editor), WPE_COMP_TRANSFORM);
    if (cam)
        position = wpe_vec3_add(
            cam->position, wpe_vec3_scale(wpe_quat_rotate_vec3(cam->rotation, wpe_vec3(0, 0, -1)), 6.0f));
    WPE_JsonWriter* w = wpe__ui_begin("create_entity");
    if (!w) return;
    wpe_json_write_key(w, "entity");
    wpe_json_write_object_begin(w);
    wpe_json_write_key(w, "name");
    wpe_json_write_string(w, name);
    wpe_json_write_key(w, "transform");
    wpe_json_write_object_begin(w);
    wpe_json_write_key(w, "position");
    wpe_json_write_array_begin(w, true);
    for (int i = 0; i < 3; ++i)
        wpe_json_write_number(w, roundf(position.v[i] * 10.0f) / 10.0f);
    wpe_json_write_array_end(w);
    wpe_json_write_object_end(w);
    wpe_json_write_key(w, "billboard");
    wpe_json_write_object_begin(w);
    if (image) {
        wpe_json_write_key(w, "image");
        wpe_json_write_string(w, image);
    }
    wpe__write_pair(w, "size", (WPE_Vec2){.x = aspect > 0.0f ? aspect : 1.0f, .y = 1.0f});
    wpe_json_write_object_end(w);
    wpe_json_write_object_end(w);
    wpe__ui_send(ui, w);
}

/* A particle emitter with the default settings, 6 units in front of the editor camera. */
static void wpe__ui_create_emitter(WPE_EditorUi* ui) {
    WPE_Vec3 position = wpe_vec3(0, 0, 0);
    const WPE_Transform* cam =
        wpe_ecs_get_component(wpe__ui_ecs(ui), wpe_editor_camera(ui->editor), WPE_COMP_TRANSFORM);
    if (cam)
        position = wpe_vec3_add(
            cam->position, wpe_vec3_scale(wpe_quat_rotate_vec3(cam->rotation, wpe_vec3(0, 0, -1)), 6.0f));
    WPE_JsonWriter* w = wpe__ui_begin("create_entity");
    if (!w) return;
    wpe_json_write_key(w, "entity");
    wpe_json_write_object_begin(w);
    wpe_json_write_key(w, "name");
    wpe_json_write_string(w, "particles");
    wpe_json_write_key(w, "transform");
    wpe_json_write_object_begin(w);
    wpe_json_write_key(w, "position");
    wpe_json_write_array_begin(w, true);
    for (int i = 0; i < 3; ++i)
        wpe_json_write_number(w, roundf(position.v[i] * 10.0f) / 10.0f);
    wpe_json_write_array_end(w);
    wpe_json_write_object_end(w);
    wpe_json_write_key(w, "particles");
    wpe_json_write_object_begin(w);
    wpe_json_write_object_end(w);
    wpe_json_write_object_end(w);
    wpe__ui_send(ui, w);
}

/* Adds a UI element: "panel", "text", "button" (with a label inside), "toggle", "slider" or an image's sprite
 * (`image` set, at the image's size). One undo step; it ends selected, and the editor switches to the 2D
 * view. In the 3D view an image becomes a billboard instead. */
static void wpe__ui_create_element(WPE_EditorUi* ui, const char* kind, const char* image) {
    ++ui->canvas_drag; /* A fresh undo step key for this element (and the button's label). */
    const WPE_Entity parent = wpe_editor_selection(ui->editor);
    WPE_JsonWriter* w = NULL;
    if (image) {
        WPE_AssetManager* assets = wpe_engine_assets(wpe_editor_engine(ui->editor));
        WPE_AssetHandle handle = WPE_ASSET_NONE;
        if (wpe_asset_load(assets, WPE_ASSET_IMAGE, image, &handle) != WPE_RESULT_OK) return; /* Logged. */
        const WPE_Texture* texture = wpe_asset_image(assets, handle);
        const float width = texture ? (float)texture->levels[0].width : 64.0f;
        const float height = texture ? (float)texture->levels[0].height : 64.0f;
        const char* slash = strrchr(image, '/');
        char name[64];
        snprintf(name, sizeof name, "%s", slash ? slash + 1 : image);
        char* dot = strrchr(name, '.');
        if (dot) *dot = '\0';
        if (!wpe_editor_view(ui->editor).view_2d) {
            /* The 3D view: a billboard one unit tall, 6 units in front of the camera. */
            wpe__ui_create_billboard(ui, name, image, width / height);
            wpe_asset_release(assets, handle);
            return;
        }
        w = wpe__begin_ui_element(ui, name, parent);
        if (w) {
            wpe__write_centered_ui(w, width, height);
            wpe_json_write_key(w, "sprite");
            wpe_json_write_object_begin(w);
            wpe_json_write_key(w, "image");
            wpe_json_write_string(w, image);
            wpe_json_write_object_end(w);
        }
        wpe_asset_release(assets, handle);
    } else if (strcmp(kind, "text") == 0) {
        w = wpe__begin_ui_element(ui, "text", parent);
        if (w) {
            wpe__write_centered_ui(w, 120, 16);
            wpe_json_write_key(w, "text");
            wpe_json_write_object_begin(w);
            wpe_json_write_key(w, "text");
            wpe_json_write_string(w, "Text");
            wpe_json_write_key(w, "align");
            wpe_json_write_string(w, "center");
            wpe_json_write_key(w, "valign");
            wpe_json_write_string(w, "middle");
            wpe_json_write_object_end(w);
        }
    } else if (strcmp(kind, "toggle") == 0 || strcmp(kind, "slider") == 0) {
        /* A check box (a dark box with the mark), or a slider (it draws its own track and handle). */
        const bool toggle = strcmp(kind, "toggle") == 0;
        w = wpe__begin_ui_element(ui, kind, parent);
        if (w) {
            wpe__write_centered_ui(w, toggle ? 20 : 120, toggle ? 20 : 14);
            if (toggle) {
                wpe_json_write_key(w, "sprite");
                wpe_json_write_object_begin(w);
                wpe__write_color(w, "color", 0.12f, 0.13f, 0.18f, 1);
                wpe_json_write_object_end(w);
            }
            wpe_json_write_key(w, "button");
            wpe_json_write_object_begin(w);
            wpe_json_write_object_end(w);
            wpe_json_write_key(w, kind);
            wpe_json_write_object_begin(w);
            if (toggle) {
                wpe_json_write_key(w, "on");
                wpe_json_write_bool(w, true);
                wpe__write_color(w, "mark_color", 0.93f, 0.6f, 0.22f, 1);
            } else {
                wpe_json_write_key(w, "value");
                wpe_json_write_number(w, 0.5);
                wpe__write_color(w, "fill_color", 0.93f, 0.6f, 0.22f, 1);
            }
            wpe_json_write_object_end(w);
        }
    } else {
        const bool button = strcmp(kind, "button") == 0;
        w = wpe__begin_ui_element(ui, button ? "button" : "panel", parent);
        if (w) {
            wpe__write_centered_ui(w, button ? 100 : 160, button ? 24 : 100);
            wpe_json_write_key(w, "sprite");
            wpe_json_write_object_begin(w);
            if (button) {
                wpe__write_color(w, "color", 0.93f, 0.6f, 0.22f, 1);
            } else {
                wpe__write_color(w, "color", 0.17f, 0.19f, 0.27f, 0.95f);
            }
            wpe_json_write_object_end(w);
            if (button) {
                wpe_json_write_key(w, "button");
                wpe_json_write_object_begin(w);
                wpe_json_write_object_end(w);
            }
        }
    }
    if (!w) return;
    wpe_json_write_object_end(w); /* entity */
    if (wpe__ui_send(ui, w) != WPE_RESULT_OK) return;
    if (strcmp(kind, "button") == 0) {
        /* Its label fills it; the button stays selected. */
        const WPE_Entity created = wpe_editor_selection(ui->editor);
        WPE_JsonWriter* label = wpe__begin_ui_element(ui, "label", created);
        if (label) {
            wpe_json_write_key(label, "ui");
            wpe_json_write_object_begin(label);
            wpe_json_write_object_end(label);
            wpe_json_write_key(label, "text");
            wpe_json_write_object_begin(label);
            wpe_json_write_key(label, "text");
            wpe_json_write_string(label, "Button");
            wpe__write_color(label, "color", 0.08f, 0.08f, 0.1f, 1);
            wpe_json_write_key(label, "align");
            wpe_json_write_string(label, "center");
            wpe_json_write_key(label, "valign");
            wpe_json_write_string(label, "middle");
            wpe_json_write_object_end(label);
            wpe_json_write_object_end(label); /* entity */
            if (wpe__ui_send(ui, label) == WPE_RESULT_OK) wpe__ui_entity_command(ui, "select", created);
        }
    }
    if (!wpe_editor_view(ui->editor).view_2d) wpe__ui_set_view(ui, "mode", "2d", 0, false);
}

static void wpe__ui_set_parent(WPE_EditorUi* ui, WPE_Entity child, WPE_Entity parent) {
    WPE_JsonWriter* w = wpe__ui_begin("set_parent");
    if (!w) return;
    wpe_json_write_key(w, "id");
    wpe_json_write_integer(w, child);
    wpe_json_write_key(w, "parent");
    if (parent) {
        wpe_json_write_integer(w, parent);
    } else {
        wpe_json_write_null(w);
    }
    wpe__ui_send(ui, w);
}

static void wpe__hierarchy_node(WPE_EditorUi* ui, WPE_Entity e) {
    WPE_Ecs* ecs = wpe__ui_ecs(ui);
    const WPE_Hierarchy* h = wpe_ecs_get_component(ecs, e, WPE_COMP_HIERARCHY);
    const char* name = wpe_scene_get_name(ecs, e);
    char label[96];
    snprintf(label, sizeof label, "%s##%u", name ? name : "(unnamed)", e);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (!h || h->child_count == 0) flags |= ImGuiTreeNodeFlags_Leaf;
    if (e == wpe_editor_selection(ui->editor)) flags |= ImGuiTreeNodeFlags_Selected;
    /* Prefab instances in blue: the root brighter than its members. */
    const WPE_Entity prefab_root = wpe_prefab_root(ecs, e);
    if (prefab_root != WPE_ENTITY_NULL)
        igPushStyleColor_U32(ImGuiCol_Text,
                             prefab_root == e ? WPE__COL(120, 180, 255, 255) : WPE__COL(150, 175, 210, 255));
    const bool open = igTreeNodeEx_Str(label, flags);
    if (prefab_root != WPE_ENTITY_NULL) igPopStyleColor(1);
    if (igIsItemClicked(ImGuiMouseButton_Left) && !igIsItemToggledOpen())
        wpe__ui_entity_command(ui, "select", e);
    /* Double-click: bring it into view. */
    if (igIsItemHovered(0) && igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left) &&
        wpe_editor_mode(ui->editor) == WPE_EDITOR_EDIT)
        wpe__ui_entity_command(ui, "focus", e);
    if (igBeginDragDropSource(0)) {
        igSetDragDropPayload(WPE__ENTITY_PAYLOAD, &e, sizeof e, 0);
        igText("%s", name ? name : "entity");
        igEndDragDropSource();
    }
    if (igBeginDragDropTarget()) {
        const ImGuiPayload* payload = igAcceptDragDropPayload(WPE__ENTITY_PAYLOAD, 0);
        if (payload && payload->DataSize == sizeof(WPE_Entity)) {
            WPE_Entity child;
            memcpy(&child, payload->Data, sizeof child);
            wpe__ui_set_parent(ui, child, e);
        }
        igEndDragDropTarget();
    }
    if (igBeginPopupContextItem(NULL, ImGuiPopupFlags_MouseButtonRight)) {
        if (igMenuItem_Bool("Duplicate", NULL, false, true))
            wpe__ui_entity_command(ui, "duplicate_entity", e);
        if (igMenuItem_Bool("Delete", NULL, false, true)) wpe__ui_entity_command(ui, "delete_entity", e);
        if (igMenuItem_Bool("Detach from parent", NULL, false,
                            wpe_scene_get_parent(ecs, e) != WPE_ENTITY_NULL &&
                                (prefab_root == WPE_ENTITY_NULL || prefab_root == e)))
            wpe__ui_set_parent(ui, e, WPE_ENTITY_NULL);
        igSeparator();
        /* A plain tree or an instance root can be saved as a (new) prefab; members can't on their own. */
        if (igMenuItem_Bool("Save as prefab...", NULL, false,
                            prefab_root == WPE_ENTITY_NULL || prefab_root == e)) {
            ui->prefab_entity = e;
            snprintf(ui->prefab_path, sizeof ui->prefab_path, "prefabs/%s.prefab", name ? name : "entity");
            ui->prefab_popup = true;
        }
        if (prefab_root != WPE_ENTITY_NULL) {
            if (igMenuItem_Bool("Apply to prefab", NULL, false, true))
                wpe__ui_entity_command(ui, "apply_prefab", e);
            if (igMenuItem_Bool("Revert to prefab", NULL, false, true))
                wpe__ui_entity_command(ui, "revert_prefab", e);
            if (igMenuItem_Bool("Unpack prefab", NULL, false, true))
                wpe__ui_entity_command(ui, "unpack_prefab", e);
        }
        igEndPopup();
    }
    if (!open) return;
    /* Children: the hierarchy may change while drawing (commands), so re-read the links per step. */
    for (WPE_Entity c = h ? h->first_child : WPE_ENTITY_NULL; c != WPE_ENTITY_NULL;) {
        const WPE_Hierarchy* ch = wpe_ecs_get_component(ecs, c, WPE_COMP_HIERARCHY);
        const WPE_Entity next = ch ? ch->next_sibling : WPE_ENTITY_NULL;
        if (wpe_ecs_is_alive(ecs, c)) wpe__hierarchy_node(ui, c);
        c = next;
    }
    igTreePop();
}

typedef struct wpe__Root {
    uint64_t serial;
    WPE_Entity entity;
} wpe__Root;

static int wpe__compare_roots(const void* a, const void* b) {
    const uint64_t x = ((const wpe__Root*)a)->serial, y = ((const wpe__Root*)b)->serial;
    return (x > y) - (x < y);
}

static void wpe__ui_hierarchy(WPE_EditorUi* ui) {
    if (!igBegin("Hierarchy", NULL, 0)) {
        igEnd();
        return;
    }
    if (igButton("+ Entity", (ImVec2_c){0, 0})) igOpenPopup_Str("add_entity", 0);
    if (igBeginPopup("add_entity", 0)) {
        if (igMenuItem_Bool("Empty", NULL, false, true)) {
            WPE_JsonWriter* w = wpe__ui_begin("create_entity");
            if (w) {
                wpe_json_write_key(w, "entity");
                wpe_json_write_object_begin(w);
                wpe_json_write_key(w, "name");
                wpe_json_write_string(w, "entity");
                wpe_json_write_key(w, "transform");
                wpe_json_write_object_begin(w);
                wpe_json_write_object_end(w);
                wpe_json_write_object_end(w);
                wpe__ui_send(ui, w);
            }
        }
        if (igMenuItem_Bool("Billboard", NULL, false, true))
            wpe__ui_create_billboard(ui, "billboard", NULL, 1.0f);
        if (igIsItemHovered(0))
            igSetTooltip("A 3D sprite; drop an image from Assets into the 3D view for one");
        if (igMenuItem_Bool("Particle emitter", NULL, false, true)) wpe__ui_create_emitter(ui);
        if (igIsItemHovered(0))
            igSetTooltip(
                "A fountain to tune; prefabs/fire, smoke and sparks in Assets are ready-made effects");
        igSeparator();
        igTextDisabled("UI (inside the selected UI element)");
        if (igMenuItem_Bool("Panel", NULL, false, true)) wpe__ui_create_element(ui, "panel", NULL);
        if (igMenuItem_Bool("Text", NULL, false, true)) wpe__ui_create_element(ui, "text", NULL);
        if (igMenuItem_Bool("Button", NULL, false, true)) wpe__ui_create_element(ui, "button", NULL);
        if (igMenuItem_Bool("Toggle", NULL, false, true)) wpe__ui_create_element(ui, "toggle", NULL);
        if (igMenuItem_Bool("Slider", NULL, false, true)) wpe__ui_create_element(ui, "slider", NULL);
        igEndPopup();
    }
    igSeparator();
    WPE_Ecs* ecs = wpe__ui_ecs(ui);
    const uint32_t count = wpe_ecs_entity_count(ecs);
    wpe__Root* roots = wpe_alloc((count ? count : 1) * sizeof *roots);
    uint32_t n = 0;
    WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){0});
    while (roots && wpe_ecs_iter_next(&it) && n < count) {
        if (wpe_scene_get_parent(ecs, it.entity) == WPE_ENTITY_NULL &&
            !wpe_editor_is_helper(ui->editor, it.entity))
            roots[n++] = (wpe__Root){.serial = wpe_ecs_spawn_serial(ecs, it.entity), .entity = it.entity};
    }
    if (roots) qsort(roots, n, sizeof *roots, wpe__compare_roots);
    for (uint32_t i = 0; i < n; ++i) {
        if (wpe_ecs_is_alive(ecs, roots[i].entity)) wpe__hierarchy_node(ui, roots[i].entity);
    }
    wpe_free(roots);
    /* Dropping on empty space makes an entity a root. */
    igInvisibleButton("##hierarchy_drop", (ImVec2_c){-1.0f, 40.0f}, 0);
    if (igBeginDragDropTarget()) {
        const ImGuiPayload* payload = igAcceptDragDropPayload(WPE__ENTITY_PAYLOAD, 0);
        if (payload && payload->DataSize == sizeof(WPE_Entity)) {
            WPE_Entity child;
            memcpy(&child, payload->Data, sizeof child);
            wpe__ui_set_parent(ui, child, WPE_ENTITY_NULL);
        }
        igEndDragDropTarget();
    }
    igEnd();
}

/* Applies an edit of one part ("name", a component key, or "environment") through a command. */
static void wpe__ui_apply_edit(WPE_EditorUi* ui, WPE_Entity entity, const char* key,
                               const WPE_JsonValue* value, const wpe__Edit* edit) {
    const bool environment = strcmp(key, "environment") == 0;
    WPE_JsonWriter* w = wpe__ui_begin(environment ? "set_environment" : "set_component");
    if (!w) return;
    if (!environment) {
        wpe_json_write_key(w, "id");
        wpe_json_write_integer(w, entity);
        wpe_json_write_key(w, "component");
        wpe_json_write_string(w, key);
    }
    wpe_json_write_key(w, "value");
    wpe__write_edited(w, value, edit);
    char coalesce[32];
    snprintf(coalesce, sizeof coalesce, "ui-%u", ui->edit_serial);
    wpe_json_write_key(w, "coalesce");
    wpe_json_write_string(w, coalesce);
    wpe__ui_send(ui, w);
}

/* ---- Script properties ----------------------------------------------------------------------------- */

#define WPE__MAX_PROPERTIES 64u

typedef struct wpe__Property {
    char name[64];
    WPE_ScriptValue value, fallback; /* fallback.type is NIL when the script doesn't declare it. */
    char string[WPE__UI_PATH], fallback_string[WPE__UI_PATH];
} wpe__Property;

typedef struct wpe__Properties {
    wpe__Property items[WPE__MAX_PROPERTIES];
    uint32_t count;
} wpe__Properties;

/* Copies a value whose string the host only lends. */
static void wpe__copy_script_value(WPE_ScriptValue* out, char* buffer, size_t size,
                                   const WPE_ScriptValue* v) {
    *out = *v;
    if (v->type != WPE_SCRIPT_VALUE_STRING) return;
    snprintf(buffer, size, "%s", v->string);
    out->string = buffer;
}

static wpe__Property* wpe__property_slot(wpe__Properties* list, const char* name) {
    for (uint32_t i = 0; i < list->count; ++i)
        if (strcmp(list->items[i].name, name) == 0) return NULL; /* Listed already (declared). */
    if (list->count == WPE__MAX_PROPERTIES) return NULL;
    wpe__Property* p = &list->items[list->count++];
    snprintf(p->name, sizeof p->name, "%s", name);
    p->fallback.type = WPE_SCRIPT_VALUE_NIL;
    return p;
}

static void wpe__collect_declared(const char* name, const WPE_ScriptValue* value,
                                  const WPE_ScriptValue* fallback, void* user_data) {
    wpe__Property* p = wpe__property_slot(user_data, name);
    if (!p) return;
    wpe__copy_script_value(&p->value, p->string, sizeof p->string, value);
    wpe__copy_script_value(&p->fallback, p->fallback_string, sizeof p->fallback_string, fallback);
}

static void wpe__collect_set(const char* name, const WPE_ScriptValue* value, void* user_data) {
    wpe__Property* p = wpe__property_slot(user_data, name);
    if (p) wpe__copy_script_value(&p->value, p->string, sizeof p->string, value);
}

static bool wpe__script_values_equal(const WPE_ScriptValue* a, const WPE_ScriptValue* b) {
    if (a->type != b->type) return false;
    switch (a->type) {
    case WPE_SCRIPT_VALUE_NUMBER:
        return a->number == b->number;
    case WPE_SCRIPT_VALUE_BOOL:
        return a->boolean == b->boolean;
    case WPE_SCRIPT_VALUE_STRING:
        return strcmp(a->string, b->string) == 0;
    case WPE_SCRIPT_VALUE_VECTOR:
        return a->vector.x == b->vector.x && a->vector.y == b->vector.y && a->vector.z == b->vector.z;
    case WPE_SCRIPT_VALUE_NIL:
        return true;
    }
    return false;
}

/* Sends set_script_property; NULL `value` removes the property (or resets a declared one). */
static void wpe__ui_set_property(WPE_EditorUi* ui, WPE_Entity entity, const char* name,
                                 const WPE_ScriptValue* value) {
    WPE_JsonWriter* w = wpe__ui_begin("set_script_property");
    if (!w) return;
    wpe_json_write_key(w, "id");
    wpe_json_write_integer(w, entity);
    wpe_json_write_key(w, "name");
    wpe_json_write_string(w, name);
    wpe_json_write_key(w, "value");
    const WPE_ScriptValue nil = {.type = WPE_SCRIPT_VALUE_NIL};
    if (!value) value = &nil;
    switch (value->type) {
    case WPE_SCRIPT_VALUE_NUMBER:
        wpe_json_write_double(w, value->number);
        break;
    case WPE_SCRIPT_VALUE_BOOL:
        wpe_json_write_bool(w, value->boolean);
        break;
    case WPE_SCRIPT_VALUE_STRING:
        wpe_json_write_string(w, value->string);
        break;
    case WPE_SCRIPT_VALUE_VECTOR:
        wpe_json_write_array_begin(w, true);
        for (int i = 0; i < 3; ++i)
            wpe_json_write_number(w, value->vector.v[i]);
        wpe_json_write_array_end(w);
        break;
    case WPE_SCRIPT_VALUE_NIL:
        wpe_json_write_null(w);
        break;
    }
    char coalesce[32];
    snprintf(coalesce, sizeof coalesce, "prop-%u", ui->edit_serial);
    wpe_json_write_key(w, "coalesce");
    wpe_json_write_string(w, coalesce);
    wpe__ui_send(ui, w);
}

/* The script's properties: the declared ones (a reset button when changed), then the others set on this
 * entity (a remove button), then a row to add one. Returns true when it sent a command. */
static bool wpe__ui_script_properties(WPE_EditorUi* ui, WPE_Entity entity) {
    WPE_ScriptHost* scripts = wpe_engine_scripts(wpe_editor_engine(ui->editor));
    wpe__Properties* list = wpe_alloc(sizeof *list);
    if (!list) return false;
    list->count = 0;
    (void)wpe_script_visit_declared(scripts, entity, wpe__collect_declared, list);
    (void)wpe_script_visit_properties(scripts, entity, wpe__collect_set, list);
    /* Drawn in full; the frame's change (at most one) is sent afterwards. */
    int changed = -1;
    bool removed = false;
    WPE_ScriptValue edit = {0};
    char edit_string[WPE__UI_PATH] = "";
    igTextDisabled(list->count ? "properties" : "properties: none (declare them in the script, or add one)");
    for (uint32_t i = 0; i < list->count; ++i) {
        wpe__Property* p = &list->items[i];
        const bool declared = p->fallback.type != WPE_SCRIPT_VALUE_NIL;
        igPushID_Str(p->name);
        bool edited = false;
        WPE_ScriptValue value = p->value;
        switch (p->value.type) {
        case WPE_SCRIPT_VALUE_NUMBER:
            edited = igDragScalar(p->name, ImGuiDataType_Double, &value.number, 0.05f, NULL, NULL, "%.6g", 0);
            wpe__note_activation(ui);
            break;
        case WPE_SCRIPT_VALUE_BOOL:
            edited = igCheckbox(p->name, &value.boolean);
            if (edited) ++ui->edit_serial;
            break;
        case WPE_SCRIPT_VALUE_STRING: {
            char buffer[WPE__UI_PATH];
            snprintf(buffer, sizeof buffer, "%s", p->value.string);
            igInputText(p->name, buffer, sizeof buffer, 0, NULL, NULL); /* Applied when the edit ends. */
            if (igIsItemDeactivatedAfterEdit()) {
                snprintf(edit_string, sizeof edit_string, "%s", buffer);
                value.string = edit_string;
                edited = true;
                ++ui->edit_serial;
            }
            break;
        }
        case WPE_SCRIPT_VALUE_VECTOR:
            edited = igDragFloat3(p->name, value.vector.v, 0.05f, 0, 0, "%.3g", 0);
            wpe__note_activation(ui);
            break;
        case WPE_SCRIPT_VALUE_NIL:
            break;
        }
        if (declared && !wpe__script_values_equal(&p->value, &p->fallback)) {
            igSameLine(0, -1);
            if (igSmallButton("Reset") && changed < 0) {
                changed = (int)i;
                removed = true;
            }
            if (igIsItemHovered(0)) igSetTooltip("Back to the script's default");
        } else if (!declared) {
            igSameLine(0, -1);
            if (igSmallButton("x") && changed < 0) {
                changed = (int)i;
                removed = true;
            }
            if (igIsItemHovered(0)) igSetTooltip("Remove the property (the script doesn't declare it)");
        }
        if (edited && changed < 0) {
            changed = (int)i;
            edit = value;
        }
        igPopID();
    }
    /* Adding one the script doesn't declare: a name and a type; the value is edited above afterwards. */
    static const char* const types[] = {"number", "bool", "string", "vector"};
    igSetNextItemWidth(igGetContentRegionAvail().x * 0.45f);
    igInputTextWithHint("##property_name", "new property", ui->add_property, sizeof ui->add_property, 0, NULL,
                        NULL);
    igSameLine(0, -1);
    igSetNextItemWidth(igGetContentRegionAvail().x * 0.55f);
    if (igBeginCombo("##property_type", types[ui->add_property_type], 0)) {
        for (int t = 0; t < 4; ++t)
            if (igSelectable_Bool(types[t], t == ui->add_property_type, 0, (ImVec2_c){0, 0}))
                ui->add_property_type = t;
        igEndCombo();
    }
    igSameLine(0, -1);
    const bool add = igSmallButton("Add") && ui->add_property[0];
    bool sent = false;
    if (changed >= 0) {
        wpe__ui_set_property(ui, entity, list->items[changed].name, removed ? NULL : &edit);
        sent = true;
    } else if (add) {
        static const WPE_ScriptValue initial[] = {
            {.type = WPE_SCRIPT_VALUE_NUMBER, .number = 0.0},
            {.type = WPE_SCRIPT_VALUE_BOOL, .boolean = false},
            {.type = WPE_SCRIPT_VALUE_STRING, .string = ""},
            {.type = WPE_SCRIPT_VALUE_VECTOR, .vector = {.v = {0, 0, 0}}},
        };
        ++ui->edit_serial;
        wpe__ui_set_property(ui, entity, ui->add_property, &initial[ui->add_property_type]);
        ui->add_property[0] = '\0';
        sent = true;
    }
    wpe_free(list);
    return sent;
}

static void wpe__ui_inspector(WPE_EditorUi* ui) {
    if (!igBegin("Inspector", NULL, 0)) {
        igEnd();
        return;
    }
    const WPE_Entity entity = wpe_editor_selection(ui->editor);
    if (entity == WPE_ENTITY_NULL) {
        igTextDisabled("Select an entity in the hierarchy or the viewport.");
        igEnd();
        return;
    }
    /* The entity as the scene-file schema describes it, read fresh every frame. */
    WPE_Editor* editor = ui->editor;
    const WPE_SceneContext context = wpe__editor_context(editor);
    WPE_JsonWriter* writer = NULL;
    WPE_JsonDocument* doc = NULL;
    const char* text = NULL;
    size_t size = 0;
    if (wpe_json_writer_create(&writer) == WPE_RESULT_OK &&
        wpe_scene_save_entity(&context, entity, writer) == WPE_RESULT_OK &&
        wpe_json_writer_finish(writer, &text, &size) == WPE_RESULT_OK)
        (void)wpe_json_parse(text, size, &doc, NULL);
    wpe_json_writer_destroy(writer);
    const WPE_JsonValue* root = wpe_json_root(doc);
    igText("Entity %u", entity);
    /* Prefab instances: where the entity comes from, and what to do with its changes. */
    const char* command = NULL; /* A prefab command for the whole instance, run after drawing. */
    const char* prefab = wpe_prefab_path(wpe__ui_ecs(ui), entity);
    if (prefab) {
        igTextColored((ImVec4_c){0.47f, 0.7f, 1.0f, 1.0f}, "Prefab %s%s", prefab,
                      wpe_prefab_root(wpe__ui_ecs(ui), entity) == entity ? "" : " (member)");
        if (igSmallButton("Apply")) command = "apply_prefab";
        if (igIsItemHovered(0))
            igSetTooltip("Write this instance's changes into the prefab (every instance gets them)");
        igSameLine(0, -1);
        if (igSmallButton("Revert all")) command = "revert_prefab";
        if (igIsItemHovered(0))
            igSetTooltip("Rebuild the instance from the prefab (its changes and added children go)");
        igSameLine(0, -1);
        if (igSmallButton("Unpack")) command = "unpack_prefab";
        if (igIsItemHovered(0))
            igSetTooltip("Make the instance plain entities, no longer linked to the prefab");
    }
    igPushItemWidth(-WPE__LABEL_WIDTH);
    /* The whole panel is drawn first and the frame's change (at most one) applied afterwards, since a
     * command changes the entity the document describes. */
    wpe__Edit edit;
    const char* edited_key = NULL;
    const WPE_JsonValue* edited_value = NULL;
    const char* removed_key = NULL;
    const char* reverted_key = NULL;
    bool property_sent = false; /* The script properties sent their own change. */
    for (uint32_t i = 0; doc && i < wpe_json_count(root); ++i) {
        const char* key = NULL;
        const WPE_JsonValue* value = wpe_json_member(root, i, &key);
        if (strcmp(key, "id") == 0 || strcmp(key, "parent") == 0 || strcmp(key, "prefab") == 0) continue;
        igPushID_Str(key);
        if (strcmp(key, "name") == 0) {
            if (wpe__draw_member(ui, key, "name", value, &edit, edited_key != NULL) && !edited_key) {
                edited_key = key;
                edited_value = value;
            }
        } else if (wpe_json_type(value) == WPE_JSON_OBJECT) {
            bool keep = true;
            /* Components that differ from the prefab are marked and can be reverted (right-click). The
             * "###" part keeps the header's id when the mark comes and goes. */
            const bool overridden = prefab && wpe_prefab_is_overridden(&context, entity, key);
            char header[96];
            snprintf(header, sizeof header, "%s%s###header", key, overridden ? "  (overridden)" : "");
            if (overridden) igPushStyleColor_U32(ImGuiCol_Text, WPE__COL(120, 180, 255, 255));
            const bool open = igCollapsingHeader_BoolPtr(header, &keep, ImGuiTreeNodeFlags_DefaultOpen);
            if (overridden) igPopStyleColor(1);
            if (overridden && igBeginPopupContextItem("revert", ImGuiPopupFlags_MouseButtonRight)) {
                if (igMenuItem_Bool("Revert to prefab", NULL, false, true)) reverted_key = key;
                igEndPopup();
            }
            if (open) {
                const bool script = strcmp(key, "script") == 0;
                for (uint32_t m = 0; m < wpe_json_count(value); ++m) {
                    const char* member_key = NULL;
                    const WPE_JsonValue* member = wpe_json_member(value, m, &member_key);
                    if (script && strcmp(member_key, "properties") == 0) continue; /* Drawn below. */
                    igPushID_Int((int)m);
                    if (wpe__draw_member(ui, key, member_key, member, &edit, edited_key != NULL) &&
                        !edited_key) {
                        edited_key = key;
                        edited_value = value;
                    }
                    igPopID();
                }
                if (script && wpe__ui_script_properties(ui, entity)) property_sent = true;
                if (strcmp(key, "collider") == 0 && igSmallButton("Fit to meshes")) command = "fit_collider";
                if (strcmp(key, "collider") == 0 && igIsItemHovered(0))
                    igSetTooltip(
                        "Size and center the collider around the meshes of this entity and its children");
            }
            if (!keep && !removed_key) removed_key = key;
        }
        igPopID();
    }
    igPopItemWidth();
    if (property_sent) {
        /* One change per frame: the property was it. */
    } else if (command) {
        wpe__ui_entity_command(ui, command, entity);
    } else if (reverted_key) {
        WPE_JsonWriter* w = wpe__ui_begin("revert_prefab");
        if (w) {
            wpe_json_write_key(w, "id");
            wpe_json_write_integer(w, entity);
            wpe_json_write_key(w, "component");
            wpe_json_write_string(w, reverted_key);
            wpe__ui_send(ui, w);
        }
    } else if (removed_key) {
        WPE_JsonWriter* w = wpe__ui_begin("remove_component");
        if (w) {
            wpe_json_write_key(w, "id");
            wpe_json_write_integer(w, entity);
            wpe_json_write_key(w, "component");
            wpe_json_write_string(w, removed_key);
            wpe__ui_send(ui, w);
        }
    } else if (edited_key) {
        wpe__ui_apply_edit(ui, entity, edited_key, edited_value, &edit);
    }
    wpe_json_free(doc);

    igSeparator();
    /* Something with meshes and no collider: one click gives it a box around them. */
    const WPE_Hierarchy* node = wpe_ecs_get_component(wpe__ui_ecs(ui), entity, WPE_COMP_HIERARCHY);
    if (!wpe_ecs_has_component(wpe__ui_ecs(ui), entity, WPE_COMP_PHYSICS_COLLIDER) &&
        (wpe_ecs_has_component(wpe__ui_ecs(ui), entity, WPE_COMP_MESH) || (node && node->first_child)) &&
        igButton("Add box collider fitted to meshes", (ImVec2_c){-1.0f, 0}))
        wpe__ui_entity_command(ui, "fit_collider", entity);
    /* Adding a component: those needing an asset take the path typed here. */
    static const char* const addable[] = {"transform", "mesh",      "camera",   "light",     "billboard",
                                          "particles", "rigidbody", "collider", "character", "audio_source",
                                          "ui",        "sprite",    "text",     "button",    "toggle",
                                          "slider",    "script"};
    igSetNextItemWidth(-1.0f);
    igInputText("##asset_path", ui->add_path, sizeof ui->add_path, 0, NULL, NULL);
    if (igIsItemHovered(0))
        igSetTooltip("Asset path for mesh (model), audio_source (sound), sprite (image) or script");
    if (igBeginCombo("##add", "Add component", 0)) {
        for (size_t i = 0; i < sizeof addable / sizeof addable[0]; ++i) {
            if (!igSelectable_Bool(addable[i], false, 0, (ImVec2_c){0, 0})) continue;
            WPE_JsonWriter* w = wpe__ui_begin("set_component");
            if (!w) break;
            wpe_json_write_key(w, "id");
            wpe_json_write_integer(w, entity);
            wpe_json_write_key(w, "component");
            wpe_json_write_string(w, addable[i]);
            wpe_json_write_key(w, "value");
            wpe_json_write_object_begin(w);
            const char* path_key =
                strcmp(addable[i], "mesh") == 0           ? "model"
                : strcmp(addable[i], "audio_source") == 0 ? "sound"
                : strcmp(addable[i], "script") == 0       ? "path"
                : (strcmp(addable[i], "sprite") == 0 || strcmp(addable[i], "billboard") == 0 ||
                   strcmp(addable[i], "particles") == 0) &&
                        wpe__is_image_path(ui->add_path)
                    ? "image"
                    : NULL;
            if (path_key && ui->add_path[0]) {
                wpe_json_write_key(w, path_key);
                wpe_json_write_string(w, ui->add_path);
            }
            if (strcmp(addable[i], "text") == 0) { /* Something to see. */
                wpe_json_write_key(w, "text");
                wpe_json_write_string(w, "Text");
            }
            wpe_json_write_object_end(w);
            wpe__ui_send(ui, w);
        }
        igEndCombo();
    }
    igEnd();
}

static void wpe__ui_environment(WPE_EditorUi* ui) {
    if (!igBegin("Environment", NULL, 0)) {
        igEnd();
        return;
    }
    WPE_JsonWriter* writer = NULL;
    WPE_JsonDocument* doc = NULL;
    const char* text = NULL;
    size_t size = 0;
    if (wpe_json_writer_create(&writer) == WPE_RESULT_OK &&
        wpe_scene_write_environment(wpe_engine_environment(wpe_editor_engine(ui->editor)), writer) ==
            WPE_RESULT_OK &&
        wpe_json_writer_finish(writer, &text, &size) == WPE_RESULT_OK)
        (void)wpe_json_parse(text, size, &doc, NULL);
    wpe_json_writer_destroy(writer);
    const WPE_JsonValue* root = wpe_json_root(doc);
    wpe__Edit edit;
    bool changed = false;
    igPushItemWidth(-WPE__LABEL_WIDTH);
    for (uint32_t i = 0; doc && i < wpe_json_count(root); ++i) {
        const char* key = NULL;
        const WPE_JsonValue* value = wpe_json_member(root, i, &key);
        igPushID_Int((int)i);
        changed |= wpe__draw_member(ui, "environment", key, value, &edit, changed);
        igPopID();
    }
    igPopItemWidth();
    if (changed) wpe__ui_apply_edit(ui, WPE_ENTITY_NULL, "environment", root, &edit);
    wpe_json_free(doc);
    igEnd();
}

static bool wpe__collect_asset(const char* path, void* user_data) {
    WPE_EditorUi* ui = user_data;
    if (ui->asset_count == ui->asset_capacity) {
        const uint32_t capacity = ui->asset_capacity ? ui->asset_capacity * 2 : 64;
        char** assets = wpe_realloc(ui->assets, capacity * sizeof *assets);
        if (!assets) return false;
        ui->assets = assets;
        ui->asset_capacity = capacity;
    }
    const size_t length = strlen(path);
    char* copy = wpe_alloc(length + 1);
    if (!copy) return false;
    memcpy(copy, path, length + 1);
    ui->assets[ui->asset_count++] = copy;
    return true;
}

static void wpe__clear_assets(WPE_EditorUi* ui) {
    for (uint32_t i = 0; i < ui->asset_count; ++i)
        wpe_free(ui->assets[i]);
    ui->asset_count = 0;
}

static bool wpe__has_suffix(const char* text, const char* suffix) {
    const size_t a = strlen(text), b = strlen(suffix);
    return a >= b && strcmp(text + a - b, suffix) == 0;
}

static void wpe__ui_assets(WPE_EditorUi* ui) {
    if (!igBegin("Assets", NULL, 0)) {
        igEnd();
        return;
    }
    if (ui->assets_stale) {
        wpe__clear_assets(ui);
        wpe_vfs_list(wpe_engine_vfs(wpe_editor_engine(ui->editor)), wpe__collect_asset, ui);
        ui->assets_stale = false;
    }
    if (igButton("Refresh", (ImVec2_c){0, 0})) ui->assets_stale = true;
    igSameLine(0, -1);
    igSetNextItemWidth(-1.0f);
    igInputText("##filter", ui->asset_filter, sizeof ui->asset_filter, 0, NULL, NULL);
    igTextDisabled("Double-click: open a scene / add a model, prefab or image (a UI sprite). Drag them into "
                   "the viewport.");
    igBeginChild_Str("##asset_list", (ImVec2_c){0, 0}, 0, 0);
    for (uint32_t i = 0; i < ui->asset_count; ++i) {
        const char* path = ui->assets[i];
        if (ui->asset_filter[0] && !strstr(path, ui->asset_filter)) continue;
        const bool model = wpe__has_suffix(path, ".glb") || wpe__has_suffix(path, ".gltf") ||
                           wpe__has_suffix(path, ".prim") || wpe_prefab_is_path(path);
        const bool scene = wpe__has_suffix(path, ".scene");
        const bool image = wpe__is_image_path(path);
        igSelectable_Bool(path, strcmp(path, wpe_editor_scene_path(ui->editor)) == 0,
                          ImGuiSelectableFlags_AllowDoubleClick, (ImVec2_c){0, 0});
        if (igIsItemHovered(0) && igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left)) {
            if (scene) {
                wpe__ui_guard(ui, WPE__PENDING_LOAD, path);
            } else if (model) {
                wpe__ui_create_model(ui, path);
            } else if (image) {
                wpe__ui_create_element(ui, "image", path);
            }
        }
        if ((model || image) && igBeginDragDropSource(0)) {
            igSetDragDropPayload(WPE__ASSET_PAYLOAD, path, strlen(path) + 1, 0);
            igText("%s", path);
            igEndDragDropSource();
        }
    }
    igEndChild();
    igEnd();
}

static void wpe__console_line(const wpe__LogLine* line, void* user_data) {
    (void)user_data;
    const ImVec4_c color = line->level >= WPE_LOG_LEVEL_ERROR  ? (ImVec4_c){1.0f, 0.45f, 0.4f, 1.0f}
                           : line->level >= WPE_LOG_LEVEL_WARN ? (ImVec4_c){1.0f, 0.85f, 0.4f, 1.0f}
                                                               : (ImVec4_c){0.8f, 0.82f, 0.85f, 1.0f};
    igTextColored(color, "%s", line->text);
}

static void wpe__ui_console(WPE_EditorUi* ui) {
    if (!igBegin("Console", NULL, 0)) {
        igEnd();
        return;
    }
    /* Commands typed here run exactly as an agent's would. */
    igSetNextItemWidth(-90.0f);
    const bool enter = igInputText("##command", ui->command, sizeof ui->command,
                                   ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL);
    igSameLine(0, -1);
    if ((igButton("Run", (ImVec2_c){80, 0}) || enter) && ui->command[0]) {
        wpe__ui_run(ui, ui->command);
        wpe_log_info("> %s", ui->command);
    }
    if (ui->reply[0]) igTextWrapped("%s", ui->reply);
    igSeparator();
    igBeginChild_Str("##log", (ImVec2_c){0, 0}, 0, 0);
    wpe__editor_log_since(ui->editor, 0, WPE__EDITOR_LOG_LINES, wpe__console_line, NULL);
    if (igGetScrollY() >= igGetScrollMaxY()) igSetScrollHereY(1.0f);
    igEndChild();
    igEnd();
}

static void wpe__ui_viewport(WPE_EditorUi* ui) {
    igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2_c){0, 0});
    const bool open =
        igBegin("Viewport", NULL, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    igPopStyleVar(1);
    if (!open) {
        ui->view_hovered = ui->view_focused = false;
        igEnd();
        return;
    }
    /* The game's framebuffer, scaled to fit while keeping its aspect ratio. */
    const ImVec2_c origin = igGetCursorScreenPos();
    const ImVec2_c avail = igGetContentRegionAvail();
    const float aspect = (float)ui->view_width / (float)ui->view_height;
    float w = avail.x, h = avail.x / aspect;
    if (h > avail.y) {
        h = avail.y;
        w = h * aspect;
    }
    w = w < 1 ? 1 : w;
    h = h < 1 ? 1 : h;
    ui->image_x = origin.x + (avail.x - w) * 0.5f;
    ui->image_y = origin.y + (avail.y - h) * 0.5f;
    ui->image_w = w;
    ui->image_h = h;
    igSetCursorScreenPos((ImVec2_c){ui->image_x, ui->image_y});
    igImage((ImTextureRef_c){._TexID = ui->view_image}, (ImVec2_c){w, h}, (ImVec2_c){0, 0}, (ImVec2_c){1, 1});
    ui->view_hovered = igIsItemHovered(0);
    ui->view_focused = igIsWindowFocused(0);
    if (igBeginDragDropTarget()) {
        const ImGuiPayload* payload = igAcceptDragDropPayload(WPE__ASSET_PAYLOAD, 0);
        if (payload && payload->DataSize > 1) {
            if (wpe__is_image_path(payload->Data)) {
                wpe__ui_create_element(ui, "image", payload->Data);
            } else {
                wpe__ui_create_model(ui, payload->Data);
            }
        }
        igEndDragDropTarget();
    }
    ImDrawList* draw = igGetWindowDrawList();
    const bool view_2d = wpe_editor_view(ui->editor).view_2d;
    /* The 2D view edits UI rectangles; the 3D view has the light icons and the transform gizmo. */
    const bool canvas = view_2d && wpe__ui_canvas(ui, draw);
    const WPE_Entity light = view_2d ? WPE_ENTITY_NULL : wpe__ui_light_icons(ui, draw);
    const bool gizmo = !view_2d && wpe__ui_gizmo(ui, draw);
    if (!view_2d) ui->canvas_edges = 0;
    if (!canvas && !view_2d && !gizmo && ui->view_hovered &&
        igIsMouseClicked_Bool(ImGuiMouseButton_Left, false) &&
        wpe_editor_mode(ui->editor) == WPE_EDITOR_EDIT) {
        /* Icons are drawn over the scene, so they are picked before it. */
        const ImVec2_c mouse = igGetMousePos();
        const WPE_Entity picked = light ? light : wpe__pick(ui, mouse.x, mouse.y);
        if (picked) {
            wpe__ui_entity_command(ui, "select", picked);
        } else {
            WPE_JsonWriter* sel = wpe__ui_begin("select");
            if (sel) {
                wpe_json_write_key(sel, "id");
                wpe_json_write_null(sel);
                wpe__ui_send(ui, sel);
            }
        }
    }
    static const char* const modes[] = {"EDIT", "PLAY", "PAUSED"};
    const WPE_EditorMode mode = wpe_editor_mode(ui->editor);
    ImDrawList_AddText_Vec2(draw, (ImVec2_c){ui->image_x + 6, ui->image_y + 4}, WPE__COL(255, 255, 255, 200),
                            mode == WPE_EDITOR_EDIT && view_2d ? "EDIT 2D" : modes[mode], NULL);
    if (wpe_editor_ui_mouse_captured(ui)) {
        static const char hint[] = "The game holds the mouse: Shift+F1 frees it, F5 stops";
        const ImVec2_c size = igCalcTextSize(hint, NULL, false, -1.0f);
        const ImVec2_c at = {ui->image_x + (ui->image_w - size.x) * 0.5f,
                             ui->image_y + ui->image_h - size.y - 6};
        ImDrawList_AddRectFilled(draw, (ImVec2_c){at.x - 6, at.y - 3},
                                 (ImVec2_c){at.x + size.x + 6, at.y + size.y + 3}, WPE__COL(0, 0, 0, 150),
                                 4.0f, 0);
        ImDrawList_AddText_Vec2(draw, at, WPE__COL(255, 255, 255, 220), hint, NULL);
    }
    igEnd();
}

static void wpe__ui_menu(WPE_EditorUi* ui) {
    const WPE_EditorMode mode = wpe_editor_mode(ui->editor);
    if (!igBeginMainMenuBar()) return;
    if (igBeginMenu("File", true)) {
        if (igMenuItem_Bool("New scene", NULL, false, true)) wpe__ui_guard(ui, WPE__PENDING_NEW, NULL);
        if (igMenuItem_Bool("Open scene...", NULL, false, true)) ui->open_scene_popup = true;
        if (igMenuItem_Bool("Save", "Ctrl+S", false, mode == WPE_EDITOR_EDIT)) {
            if (wpe_editor_scene_path(ui->editor)[0]) {
                wpe__ui_simple(ui, "save_scene");
            } else {
                ui->save_as_popup = true;
            }
        }
        if (igMenuItem_Bool("Save as...", NULL, false, mode == WPE_EDITOR_EDIT)) ui->save_as_popup = true;
        if (igMenuItem_Bool("Export game...", NULL, false, true)) ui->export_popup = true;
        igSeparator();
        if (igMenuItem_Bool("Quit", NULL, false, true)) wpe__ui_guard(ui, WPE__PENDING_QUIT, NULL);
        igEndMenu();
    }
    if (igBeginMenu("Edit", true)) {
        if (igMenuItem_Bool("Undo", "Ctrl+Z", false, wpe_editor_undo_count(ui->editor) > 0))
            wpe__ui_simple(ui, "undo");
        if (igMenuItem_Bool("Redo", "Ctrl+Y", false, wpe_editor_redo_count(ui->editor) > 0))
            wpe__ui_simple(ui, "redo");
        const WPE_Entity selection = wpe_editor_selection(ui->editor);
        if (igMenuItem_Bool("Duplicate", "Ctrl+D", false, selection != WPE_ENTITY_NULL))
            wpe__ui_entity_command(ui, "duplicate_entity", selection);
        if (igMenuItem_Bool("Delete", "Del", false, selection != WPE_ENTITY_NULL))
            wpe__ui_entity_command(ui, "delete_entity", selection);
        igSeparator();
        if (igMenuItem_Bool("Focus selection", "F", false,
                            selection != WPE_ENTITY_NULL && mode == WPE_EDITOR_EDIT))
            wpe__ui_simple(ui, "focus");
        igEndMenu();
    }
    if (igBeginMenu("View", true)) {
        const WPE_EditorView view = wpe_editor_view(ui->editor);
        if (igMenuItem_Bool("2D view (UI)", NULL, view.view_2d, true))
            wpe__ui_set_view(ui, "mode", view.view_2d ? "3d" : "2d", 0, false);
        igSeparator();
        if (igMenuItem_Bool("Grid", "G", view.grid, true)) wpe__ui_set_view(ui, "grid", NULL, 0, !view.grid);
        float spacing = view.grid_spacing;
        igSetNextItemWidth(120.0f);
        if (igDragFloat("Grid spacing", &spacing, 0.05f, 0.01f, 1000.0f, "%.2f",
                        ImGuiSliderFlags_AlwaysClamp))
            wpe__ui_set_view(ui, "grid_spacing", NULL, spacing, false);
        if (igMenuItem_Bool("Physics shapes", "P", view.physics, true))
            wpe__ui_set_view(ui, "physics", NULL, 0, !view.physics);
        if (igMenuItem_Bool("Icons (lights, particles)", "L", view.icons, true))
            wpe__ui_set_view(ui, "icons", NULL, 0, !view.icons);
        if (igMenuItem_Bool("Reload changed scripts", NULL, view.hot_reload, true))
            wpe__ui_set_view(ui, "hot_reload", NULL, 0, !view.hot_reload);
        igSeparator();
        if (igMenuItem_Bool("Snap", NULL, view.snap, true)) wpe__ui_set_view(ui, "snap", NULL, 0, !view.snap);
        const struct {
            const char* label;
            const char* key;
            float value, speed, min, max;
            const char* format;
        } steps[] = {{"Move step", "snap_move", view.snap_move, 0.05f, 0.001f, 1000.0f, "%.3g"},
                     {"Rotate step", "snap_rotate", view.snap_rotate, 0.5f, 0.1f, 180.0f, "%.3g deg"},
                     {"Scale step", "snap_scale", view.snap_scale, 0.01f, 0.001f, 100.0f, "%.3g"},
                     {"UI step", "snap_ui", view.snap_ui, 0.25f, 1.0f, 256.0f, "%.0f px"}};
        for (size_t i = 0; i < sizeof steps / sizeof steps[0]; ++i) {
            float value = steps[i].value;
            igSetNextItemWidth(120.0f);
            if (igDragFloat(steps[i].label, &value, steps[i].speed, steps[i].min, steps[i].max,
                            steps[i].format, ImGuiSliderFlags_AlwaysClamp))
                wpe__ui_set_view(ui, steps[i].key, NULL, value, false);
        }
        igEndMenu();
    }
    if (igBeginMenu("Window", true)) {
        if (igMenuItem_Bool("Reset layout", NULL, false, true)) ui->layout_built = false;
        igEndMenu();
    }
    /* Toolbar: play controls and gizmo modes. */
    igSeparator();
    if (mode == WPE_EDITOR_PLAY) {
        if (igMenuItem_Bool("Pause", "F6", false, true)) wpe__ui_simple(ui, "pause");
    } else if (igMenuItem_Bool(mode == WPE_EDITOR_PAUSED ? "Resume" : "Play", "F5", false, true)) {
        wpe__ui_simple(ui, "play");
    }
    if (igMenuItem_Bool("Stop", "Shift+F5", false, mode != WPE_EDITOR_EDIT)) wpe__ui_simple(ui, "stop");
    igSeparator();
    const WPE_EditorView view = wpe_editor_view(ui->editor);
    if (igMenuItem_Bool("Move", "W", view.gizmo == WPE_EDITOR_GIZMO_MOVE, true))
        wpe__ui_set_view(ui, "gizmo", "move", 0, false);
    if (igMenuItem_Bool("Rotate", "E", view.gizmo == WPE_EDITOR_GIZMO_ROTATE, true))
        wpe__ui_set_view(ui, "gizmo", "rotate", 0, false);
    if (igMenuItem_Bool("Scale", "R", view.gizmo == WPE_EDITOR_GIZMO_SCALE, true))
        wpe__ui_set_view(ui, "gizmo", "scale", 0, false);
    /* One item showing the current space; clicking switches it. */
    if (igMenuItem_Bool(view.gizmo_local ? "Local" : "World", "X", false, true))
        wpe__ui_set_view(ui, "gizmo_space", view.gizmo_local ? "world" : "local", 0, false);
    if (igIsItemHovered(0))
        igSetTooltip("Gizmo axes: the world's or the entity's own (scaling always uses its own)");
    if (igMenuItem_Bool("Snap", NULL, view.snap, true)) wpe__ui_set_view(ui, "snap", NULL, 0, !view.snap);
    if (igIsItemHovered(0))
        igSetTooltip("Snap gizmo drags to steps (View menu); hold Ctrl while dragging to invert");
    if (igMenuItem_Bool("2D", NULL, view.view_2d, true))
        wpe__ui_set_view(ui, "mode", view.view_2d ? "3d" : "2d", 0, false);
    if (igIsItemHovered(0))
        igSetTooltip("2D view: the game's camera and its UI, whose elements you pick, move and resize here");
    igSeparator();
    const char* path = wpe_editor_scene_path(ui->editor);
    igTextDisabled("%s%s", path[0] ? path : "(unsaved scene)", wpe_editor_dirty(ui->editor) ? " *" : "");
    igEndMainMenuBar();
}

/* A modal asking for one path (into `buffer`); the caller acts on WPE__POPUP_OK. */
static wpe__PopupResult wpe__ui_path_popup(bool* request, const char* title, const char* hint, char* buffer,
                                           size_t size) {
    if (*request) {
        igOpenPopup_Str(title, 0);
        *request = false;
    }
    if (!igBeginPopupModal(title, NULL, ImGuiWindowFlags_AlwaysAutoResize)) return WPE__POPUP_NONE;
    wpe__PopupResult result = WPE__POPUP_NONE;
    igTextUnformatted(hint, NULL);
    igSetNextItemWidth(420.0f);
    const bool enter = igInputText("##path", buffer, size, ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL);
    if ((igButton("OK", (ImVec2_c){120, 0}) || enter) && buffer[0]) {
        result = WPE__POPUP_OK;
        igCloseCurrentPopup();
    }
    igSameLine(0, -1);
    if (igButton("Cancel", (ImVec2_c){120, 0})) {
        result = WPE__POPUP_CANCEL;
        igCloseCurrentPopup();
    }
    igEndPopup();
    return result;
}

static void wpe__ui_popups(WPE_EditorUi* ui) {
    wpe__ui_unsaved_popup(ui);
    if (wpe__ui_path_popup(&ui->open_scene_popup, "Open scene", "Scene asset path:", ui->scene_path,
                           sizeof ui->scene_path) == WPE__POPUP_OK)
        wpe__ui_guard(ui, WPE__PENDING_LOAD, ui->scene_path);
    const wpe__PopupResult save = wpe__ui_path_popup(
        &ui->save_as_popup, "Save scene as", "Scene asset path:", ui->scene_path, sizeof ui->scene_path);
    if (save == WPE__POPUP_OK) {
        ui->assets_stale = true;
        const WPE_Result saved = wpe__ui_string_command(ui, "save_scene", "path", ui->scene_path, false);
        /* The unsaved-changes prompt's Save continues with what it was asked about. */
        if (ui->pending_after_save && saved == WPE_RESULT_OK) wpe__ui_perform(ui, false);
    }
    if (save != WPE__POPUP_NONE && ui->pending_after_save) {
        ui->pending = WPE__PENDING_NONE;
        ui->pending_after_save = false;
    }
    if (wpe__ui_path_popup(&ui->export_popup, "Export game", "Output executable (outside the project):",
                           ui->export_path, sizeof ui->export_path) == WPE__POPUP_OK)
        wpe__ui_string_command(ui, "export", "output", ui->export_path, false);
    if (wpe__ui_path_popup(&ui->prefab_popup, "Save as prefab", "Prefab asset path:", ui->prefab_path,
                           sizeof ui->prefab_path) == WPE__POPUP_OK) {
        WPE_JsonWriter* w = wpe__ui_begin("save_prefab");
        if (w) {
            wpe_json_write_key(w, "id");
            wpe_json_write_integer(w, ui->prefab_entity);
            wpe_json_write_key(w, "path");
            wpe_json_write_string(w, ui->prefab_path);
            wpe__ui_send(ui, w);
        }
        ui->assets_stale = true;
    }
}

static void wpe__ui_shortcuts(WPE_EditorUi* ui) {
    ImGuiIO* io = igGetIO_Nil();
    if (io->WantTextInput) return;
    const WPE_EditorMode mode = wpe_editor_mode(ui->editor);
    if (igIsKeyPressed_Bool(ImGuiKey_F5, false))
        wpe__ui_simple(ui, io->KeyShift || mode != WPE_EDITOR_EDIT ? "stop" : "play");
    if (igIsKeyPressed_Bool(ImGuiKey_F6, false))
        wpe__ui_simple(ui, mode == WPE_EDITOR_PLAY ? "pause" : "play");
    /* The way out of a game's mouse look that doesn't depend on the game's own scripts. */
    if (io->KeyShift && igIsKeyPressed_Bool(ImGuiKey_F1, false) && wpe_editor_ui_mouse_captured(ui))
        wpe__ui_simple(ui, "release_mouse");
    if (mode != WPE_EDITOR_EDIT) return; /* The game owns the keyboard while playing. */
    if (io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_Z, false)) wpe__ui_simple(ui, "undo");
    if (io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_Y, false)) wpe__ui_simple(ui, "redo");
    if (io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_S, false)) {
        if (wpe_editor_scene_path(ui->editor)[0]) {
            wpe__ui_simple(ui, "save_scene");
        } else {
            ui->save_as_popup = true;
        }
    }
    const WPE_Entity selection = wpe_editor_selection(ui->editor);
    if (io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_D, false) && selection)
        wpe__ui_entity_command(ui, "duplicate_entity", selection);
    if (igIsKeyPressed_Bool(ImGuiKey_Delete, false) && selection)
        wpe__ui_entity_command(ui, "delete_entity", selection);
    if (!ui->flying && !io->KeyCtrl) {
        const WPE_EditorView view = wpe_editor_view(ui->editor);
        if (igIsKeyPressed_Bool(ImGuiKey_F, false) && selection && !view.view_2d) wpe__ui_simple(ui, "focus");
        if (igIsKeyPressed_Bool(ImGuiKey_W, false)) wpe__ui_set_view(ui, "gizmo", "move", 0, false);
        if (igIsKeyPressed_Bool(ImGuiKey_E, false)) wpe__ui_set_view(ui, "gizmo", "rotate", 0, false);
        if (igIsKeyPressed_Bool(ImGuiKey_R, false)) wpe__ui_set_view(ui, "gizmo", "scale", 0, false);
        if (igIsKeyPressed_Bool(ImGuiKey_X, false))
            wpe__ui_set_view(ui, "gizmo_space", view.gizmo_local ? "world" : "local", 0, false);
        if (igIsKeyPressed_Bool(ImGuiKey_G, false)) wpe__ui_set_view(ui, "grid", NULL, 0, !view.grid);
        if (igIsKeyPressed_Bool(ImGuiKey_P, false)) wpe__ui_set_view(ui, "physics", NULL, 0, !view.physics);
        if (igIsKeyPressed_Bool(ImGuiKey_L, false)) wpe__ui_set_view(ui, "icons", NULL, 0, !view.icons);
    }
}

static void wpe__ui_layout(WPE_EditorUi* ui, ImGuiID dockspace, const ImGuiViewport* viewport) {
    igDockBuilderRemoveNode(dockspace);
    igDockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    igDockBuilderSetNodeSize(dockspace, viewport->WorkSize);
    ImGuiID center = dockspace, left = 0, right = 0, bottom = 0;
    left = igDockBuilderSplitNode(center, ImGuiDir_Left, 0.18f, NULL, &center);
    right = igDockBuilderSplitNode(center, ImGuiDir_Right, 0.28f, NULL, &center);
    bottom = igDockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, NULL, &center);
    igDockBuilderDockWindow("Hierarchy", left);
    igDockBuilderDockWindow("Inspector", right);
    igDockBuilderDockWindow("Environment", right);
    igDockBuilderDockWindow("Console", bottom);
    igDockBuilderDockWindow("Assets", bottom);
    igDockBuilderDockWindow("Viewport", center);
    igDockBuilderFinish(dockspace);
    ui->layout_built = true;
}

/* The editor's look: rounded, dark, with an amber accent. */
static void wpe__ui_style(void) {
    ImGuiStyle* style = igGetStyle();
    style->WindowRounding = 8.0f;
    style->ChildRounding = 6.0f;
    style->FrameRounding = 5.0f;
    style->PopupRounding = 7.0f;
    style->ScrollbarRounding = 8.0f;
    style->GrabRounding = 5.0f;
    style->WindowPadding = (ImVec2_c){12.0f, 12.0f};
    style->FramePadding = (ImVec2_c){10.0f, 6.0f};
    style->ItemSpacing = (ImVec2_c){8.0f, 8.0f};
    style->ItemInnerSpacing = (ImVec2_c){6.0f, 6.0f};
    style->ScrollbarSize = 12.0f;
    style->WindowBorderSize = 0.0f;
    style->FrameBorderSize = 0.0f;

    const ImVec4_c accent = {0.93f, 0.60f, 0.22f, 1.0f};
    const ImVec4_c accent_hover = {1.0f, 0.69f, 0.32f, 1.0f};
    const ImVec4_c accent_dim = {0.55f, 0.35f, 0.13f, 1.0f};
    ImVec4_c* colors = style->Colors;
    colors[ImGuiCol_Text] = (ImVec4_c){0.88f, 0.89f, 0.92f, 1.0f};
    colors[ImGuiCol_TextDisabled] = (ImVec4_c){0.46f, 0.48f, 0.53f, 1.0f};
    colors[ImGuiCol_WindowBg] = (ImVec4_c){0.075f, 0.08f, 0.095f, 1.0f};
    colors[ImGuiCol_ChildBg] = (ImVec4_c){0.075f, 0.08f, 0.095f, 1.0f};
    colors[ImGuiCol_PopupBg] = (ImVec4_c){0.10f, 0.11f, 0.13f, 0.98f};
    colors[ImGuiCol_Border] = (ImVec4_c){0.18f, 0.19f, 0.22f, 1.0f};
    colors[ImGuiCol_BorderShadow] = (ImVec4_c){0.0f, 0.0f, 0.0f, 0.0f};
    colors[ImGuiCol_FrameBg] = (ImVec4_c){0.14f, 0.15f, 0.18f, 1.0f};
    colors[ImGuiCol_FrameBgHovered] = (ImVec4_c){0.20f, 0.21f, 0.25f, 1.0f};
    colors[ImGuiCol_FrameBgActive] = (ImVec4_c){0.24f, 0.25f, 0.30f, 1.0f};
    colors[ImGuiCol_TitleBg] = (ImVec4_c){0.075f, 0.08f, 0.095f, 1.0f};
    colors[ImGuiCol_TitleBgActive] = (ImVec4_c){0.10f, 0.11f, 0.13f, 1.0f};
    colors[ImGuiCol_MenuBarBg] = (ImVec4_c){0.065f, 0.07f, 0.085f, 1.0f};
    colors[ImGuiCol_ScrollbarBg] = (ImVec4_c){0.075f, 0.08f, 0.095f, 1.0f};
    colors[ImGuiCol_ScrollbarGrab] = (ImVec4_c){0.25f, 0.27f, 0.32f, 1.0f};
    colors[ImGuiCol_ScrollbarGrabHovered] = (ImVec4_c){0.33f, 0.35f, 0.41f, 1.0f};
    colors[ImGuiCol_ScrollbarGrabActive] = accent_dim;
    colors[ImGuiCol_CheckMark] = accent;
    colors[ImGuiCol_SliderGrab] = accent;
    colors[ImGuiCol_SliderGrabActive] = accent_hover;
    colors[ImGuiCol_Button] = (ImVec4_c){0.16f, 0.17f, 0.20f, 1.0f};
    colors[ImGuiCol_ButtonHovered] = (ImVec4_c){0.23f, 0.24f, 0.29f, 1.0f};
    colors[ImGuiCol_ButtonActive] = (ImVec4_c){0.30f, 0.31f, 0.37f, 1.0f};
    colors[ImGuiCol_Header] = (ImVec4_c){0.22f, 0.20f, 0.16f, 1.0f};
    colors[ImGuiCol_HeaderHovered] = (ImVec4_c){0.31f, 0.27f, 0.18f, 1.0f};
    colors[ImGuiCol_HeaderActive] = accent_dim;
    colors[ImGuiCol_Separator] = (ImVec4_c){0.17f, 0.18f, 0.21f, 1.0f};
    colors[ImGuiCol_SeparatorHovered] = accent_dim;
    colors[ImGuiCol_SeparatorActive] = accent;
    colors[ImGuiCol_ResizeGrip] = (ImVec4_c){0.0f, 0.0f, 0.0f, 0.0f};
    colors[ImGuiCol_Tab] = (ImVec4_c){0.12f, 0.13f, 0.15f, 1.0f};
    colors[ImGuiCol_TabHovered] = (ImVec4_c){0.24f, 0.22f, 0.18f, 1.0f};
    colors[ImGuiCol_TabSelected] = (ImVec4_c){0.22f, 0.20f, 0.16f, 1.0f};
    colors[ImGuiCol_PlotLines] = accent;
    colors[ImGuiCol_PlotHistogram] = accent;
    colors[ImGuiCol_ModalWindowDimBg] = (ImVec4_c){0.0f, 0.0f, 0.0f, 0.64f};
    /* Colors the dark style would otherwise leave blue. */
    colors[ImGuiCol_CheckboxSelectedBg] = colors[ImGuiCol_FrameBg];
    colors[ImGuiCol_ResizeGripHovered] = accent_dim;
    colors[ImGuiCol_ResizeGripActive] = accent;
    colors[ImGuiCol_TabSelectedOverline] = accent;
    colors[ImGuiCol_TabDimmed] = colors[ImGuiCol_Tab];
    colors[ImGuiCol_TabDimmedSelected] = colors[ImGuiCol_TabSelected];
    colors[ImGuiCol_TabDimmedSelectedOverline] = accent_dim;
    colors[ImGuiCol_DockingPreview] = (ImVec4_c){accent.x, accent.y, accent.z, 0.5f};
    colors[ImGuiCol_TextLink] = accent;
    colors[ImGuiCol_TextSelectedBg] = (ImVec4_c){accent.x, accent.y, accent.z, 0.35f};
    colors[ImGuiCol_DragDropTarget] = accent;
    colors[ImGuiCol_NavCursor] = accent;
}

/* ---- Public ----------------------------------------------------------------------------------------- */

WPE_Result wpe_editor_ui_create(WPE_Editor* editor, WPE_PlatformRenderer* gpu, WPE_EditorUi** out_ui) {
    if (!editor || !out_ui) return WPE_RESULT_ERROR_NULL_ARG;
    *out_ui = NULL;
    WPE_EditorUi* ui = wpe_alloc_zeroed(1, sizeof *ui);
    if (!ui) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    ui->editor = editor;
    ui->gizmo_axis = -1;
    ui->gizmo_hover = -1;
    ui->assets_stale = true;
    snprintf(ui->scene_path, sizeof ui->scene_path, "scenes/new.scene");
    snprintf(ui->export_path, sizeof ui->export_path, "game%s", WPE_EXE_SUFFIX);
    const WPE_Result result = wpe_ui_backend_create(gpu, &ui->backend);
    if (result != WPE_RESULT_OK) {
        wpe_free(ui);
        return result;
    }
    wpe__ui_style();
    *out_ui = ui;
    return WPE_RESULT_OK;
}

void wpe_editor_ui_destroy(WPE_EditorUi* ui) {
    if (!ui) return;
    wpe_ui_backend_destroy(ui->backend);
    wpe__clear_assets(ui);
    wpe_free(ui->assets);
    wpe_free(ui);
}

void wpe_editor_ui_handle_event(WPE_EditorUi* ui, const WPE_PlatformEvent* event) {
    if (!ui || !event) return;
    const SDL_Event* e = (const SDL_Event*)event;
    /* While the game holds the mouse (mouse look), the hidden cursor must not hover or click the editor's
     * panels: that would take the focus from the game view. The mouse goes to the game alone. */
    const bool mouse_event = e->type == SDL_EVENT_MOUSE_MOTION || e->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                             e->type == SDL_EVENT_MOUSE_BUTTON_UP || e->type == SDL_EVENT_MOUSE_WHEEL;
    if (!(mouse_event && wpe_editor_ui_mouse_captured(ui))) wpe_ui_backend_handle_event(ui->backend, event);
    WPE_Engine* engine = wpe_editor_engine(ui->editor);
    const bool playing = wpe_editor_mode(ui->editor) == WPE_EDITOR_PLAY;
    switch (e->type) {
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        wpe_engine_handle_event(engine,
                                event); /* Pads always reach the game (connections must be tracked). */
        break;
    case SDL_EVENT_KEY_DOWN:
        if (playing && ui->view_focused) wpe_engine_handle_event(engine, event);
        break;
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        /* Releases always reach the game, so a key let go after stopping or away from the view never
         * stays held. */
        wpe_engine_handle_event(engine, event);
        break;
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL: {
        if (!playing || !ui->view_focused) break;
        /* Mouse positions in the game's framebuffer pixels. */
        SDL_Event copy = *e;
        const float sx = (float)ui->view_width / (ui->image_w > 0 ? ui->image_w : 1.0f);
        const float sy = (float)ui->view_height / (ui->image_h > 0 ? ui->image_h : 1.0f);
        if (e->type == SDL_EVENT_MOUSE_MOTION) {
            copy.motion.x = (e->motion.x - ui->image_x) * sx;
            copy.motion.y = (e->motion.y - ui->image_y) * sy;
            copy.motion.xrel = e->motion.xrel * sx;
            copy.motion.yrel = e->motion.yrel * sy;
        } else if (e->type != SDL_EVENT_MOUSE_WHEEL) {
            copy.button.x = (e->button.x - ui->image_x) * sx;
            copy.button.y = (e->button.y - ui->image_y) * sy;
        }
        wpe_engine_handle_event(engine, (const WPE_PlatformEvent*)&copy);
        break;
    }
    default:
        break;
    }
}

/* Copies the engine's framebuffer into the image shown by the viewport. */
static WPE_Result wpe__ui_update_view(WPE_EditorUi* ui) {
    const WPE_Framebuffer* fb = wpe_renderer_framebuffer(wpe_engine_renderer(wpe_editor_engine(ui->editor)));
    const WPE_Result result =
        wpe_ui_backend_set_image(ui->backend, &ui->view_image, fb->width, fb->height, fb->color);
    if (result != WPE_RESULT_OK) return result;
    ui->view_width = fb->width;
    ui->view_height = fb->height;
    return WPE_RESULT_OK;
}

WPE_Result wpe_editor_ui_frame(WPE_EditorUi* ui, WPE_Renderer* renderer, float dt) {
    if (!ui) return WPE_RESULT_ERROR_NULL_ARG;
    const bool gpu = wpe_ui_backend_is_gpu(ui->backend);
    if (!gpu && !renderer) return WPE_RESULT_ERROR_NULL_ARG;
    if (wpe_editor_mode(ui->editor) == WPE_EDITOR_EDIT) {
        /* The 2D view shows the game's camera: the editor camera stays where it is. */
        if (wpe_editor_view(ui->editor).view_2d) {
            ui->flying = false;
        } else {
            wpe__ui_camera(ui, dt);
        }
    } else {
        ui->camera_initialized = false; /* The editor camera is recreated when play stops. */
    }
    WPE_Result result = wpe_editor_frame(ui->editor, dt);
    const WPE_Result view = wpe__ui_update_view(ui);
    if (result == WPE_RESULT_OK) result = view;

    uint32_t width = 0, height = 0;
    if (gpu) {
        wpe_ui_backend_output_size(ui->backend, &width, &height);
    } else {
        width = wpe_renderer_framebuffer(renderer)->width;
        height = wpe_renderer_framebuffer(renderer)->height;
    }
    wpe_ui_backend_new_frame(ui->backend, width, height, dt);
    const ImGuiViewport* viewport = igGetMainViewport();
    const ImGuiID dockspace = igDockSpaceOverViewport(0, viewport, 0, NULL);
    if (!ui->layout_built) wpe__ui_layout(ui, dockspace, viewport);
    wpe__ui_shortcuts(ui);
    wpe__ui_menu(ui);
    wpe__ui_viewport(ui);
    wpe__ui_hierarchy(ui);
    wpe__ui_inspector(ui);
    wpe__ui_environment(ui);
    wpe__ui_assets(ui);
    wpe__ui_console(ui);
    wpe__ui_popups(ui);

    if (gpu) {
        const WPE_Result presented = wpe_ui_backend_present(ui->backend, WPE__BACKGROUND);
        return result == WPE_RESULT_OK ? presented : result;
    }
    const WPE_Result begin = wpe_renderer_begin_frame(
        renderer,
        &(WPE_ClearDesc){.clear_color = true, .color = WPE__BACKGROUND, .clear_depth = true, .depth = 1.0f});
    if (begin != WPE_RESULT_OK) return begin;
    const WPE_Result drawn = wpe_ui_backend_render(ui->backend, renderer);
    const WPE_Result ended = wpe_renderer_end_frame(renderer);
    if (result == WPE_RESULT_OK) result = drawn;
    if (result == WPE_RESULT_OK) result = ended;
    return result;
}

WPE_Result wpe_editor_ui_capture(WPE_EditorUi* ui, WPE_Image* out_image) {
    if (!ui) return WPE_RESULT_ERROR_NULL_ARG;
    return wpe_ui_backend_capture(ui->backend, out_image);
}

void wpe_editor_ui_request_quit(WPE_EditorUi* ui) {
    if (ui && ui->pending == WPE__PENDING_NONE) wpe__ui_guard(ui, WPE__PENDING_QUIT, NULL);
}

bool wpe_editor_ui_quit_requested(const WPE_EditorUi* ui) {
    return ui && ui->quit;
}

bool wpe_editor_ui_mouse_captured(const WPE_EditorUi* ui) {
    return ui && wpe_editor_mode(ui->editor) == WPE_EDITOR_PLAY &&
           wpe_input_mouse_captured(wpe_engine_input(wpe_editor_engine(ui->editor)));
}
