/*
 * Scene files: the UI components ("ui", "sprite", "text", "button"; format in docs/scenes.md).
 */
#define WPE_LOG_MODULE "scene"
#include <string.h>

#include "engine/scene_file_internal.h"
#include "ui/ui.h"

static const char* const wpe__align_x[] = {"left", "center", "right"};
static const char* const wpe__align_y[] = {"top", "middle", "bottom"};

#define WPE__COUNT(array) ((uint32_t)(sizeof(array) / sizeof((array)[0])))

static_assert(WPE__COUNT(wpe__align_x) == WPE_UI_ALIGN_END + 1, "horizontal alignment names");
static_assert(WPE__COUNT(wpe__align_y) == WPE_UI_ALIGN_END + 1, "vertical alignment names");

/* ---- Saving --------------------------------------------------------------------------------------- */

static void wpe__write_floats(WPE_JsonWriter* w, const char* key, const float* values, int count) {
    wpe_json_write_key(w, key);
    wpe_json_write_array_begin(w, true);
    for (int i = 0; i < count; ++i)
        wpe_json_write_number(w, values[i]);
    wpe_json_write_array_end(w);
}

static void wpe__write_rect(WPE_JsonWriter* w, const char* key, WPE_Rect r) {
    const float values[4] = {r.x, r.y, r.width, r.height};
    wpe__write_floats(w, key, values, 4);
}

void wpe__save_ui_components(wpe__Saver* s, WPE_Entity e) {
    WPE_Ecs* ecs = s->context->ecs;
    WPE_JsonWriter* w = s->w;
    const WPE_UiRect* element = wpe_ecs_get_component(ecs, e, WPE_COMP_UI);
    if (element) {
        wpe_json_write_key(w, "ui");
        wpe_json_write_object_begin(w);
        wpe__write_floats(w, "anchor_min", element->anchor_min.v, 2);
        wpe__write_floats(w, "anchor_max", element->anchor_max.v, 2);
        wpe__write_floats(w, "offset_min", element->offset_min.v, 2);
        wpe__write_floats(w, "offset_max", element->offset_max.v, 2);
        wpe_json_write_key(w, "visible");
        wpe_json_write_bool(w, element->visible);
        wpe_json_write_object_end(w);
    }
    const WPE_Sprite* sprite = wpe_ecs_get_component(ecs, e, WPE_COMP_SPRITE);
    if (sprite) {
        wpe_json_write_key(w, "sprite");
        wpe_json_write_object_begin(w);
        wpe__write_asset(s, "image", sprite->image);
        wpe__write_floats(w, "color", sprite->color.v, 4);
        wpe__write_rect(w, "source", sprite->source);
        wpe__write_floats(w, "slice", sprite->slice.v, 4);
        wpe_json_write_object_end(w);
    }
    const WPE_Text* text = wpe_ecs_get_component(ecs, e, WPE_COMP_TEXT);
    if (text) {
        wpe_json_write_key(w, "text");
        wpe_json_write_object_begin(w);
        wpe_json_write_key(w, "text");
        wpe_json_write_string(w, text->text ? text->text : "");
        wpe__write_asset(s, "font", text->font);
        wpe__write_floats(w, "color", text->color.v, 4);
        wpe_json_write_key(w, "scale");
        wpe_json_write_number(w, text->scale);
        wpe_json_write_key(w, "align");
        wpe_json_write_string(w, wpe__align_x[text->align_x]);
        wpe_json_write_key(w, "valign");
        wpe_json_write_string(w, wpe__align_y[text->align_y]);
        wpe_json_write_key(w, "wrap");
        wpe_json_write_bool(w, text->wrap);
        wpe_json_write_object_end(w);
    }
    const WPE_Button* button = wpe_ecs_get_component(ecs, e, WPE_COMP_BUTTON);
    if (button) {
        wpe_json_write_key(w, "button");
        wpe_json_write_object_begin(w);
        wpe_json_write_key(w, "interactable");
        wpe_json_write_bool(w, button->interactable);
        wpe__write_floats(w, "normal", button->normal_tint.v, 4);
        wpe__write_floats(w, "focused", button->focused_tint.v, 4);
        wpe__write_floats(w, "pressed", button->pressed_tint.v, 4);
        wpe__write_floats(w, "disabled", button->disabled_tint.v, 4);
        wpe_json_write_object_end(w);
    }
}

/* ---- Loading -------------------------------------------------------------------------------------- */

/* Optional array of `count` numbers in [min, max]. */
static bool wpe__read_ranged(wpe__Loader* l, const WPE_JsonValue* object, const char* key, uint32_t count,
                             double min, double max, float* io) {
    const WPE_JsonValue* v = wpe_json_get(object, key);
    if (!v) return l->r.result == WPE_RESULT_OK;
    float values[4];
    if (!wpe__json_read_floats(&l->r, v, key, count, values)) return false;
    for (uint32_t i = 0; i < count; ++i) {
        if (!(values[i] >= min && values[i] <= max))
            return wpe__json_fail(&l->r, wpe_json_at(v, i), WPE_RESULT_ERROR_PARSE,
                                  "\"%s\" values must be %g..%g", key, min, max);
    }
    memcpy(io, values, count * sizeof *values);
    return true;
}

#define WPE__PIXELS 1e6 /* Offsets and sizes in pixels: generous, but finite. */

bool wpe__load_ui(wpe__Loader* l, WPE_Entity e, const WPE_JsonValue* v) {
    WPE_UiRect element = wpe_ui_rect_default();
    if (!WPE__JSON_CHECK_OBJECT(&l->r, v, "ui", "anchor_min", "anchor_max", "offset_min", "offset_max",
                                "visible") ||
        !wpe__read_ranged(l, v, "anchor_min", 2, -10.0, 10.0, element.anchor_min.v) ||
        !wpe__read_ranged(l, v, "anchor_max", 2, -10.0, 10.0, element.anchor_max.v) ||
        !wpe__read_ranged(l, v, "offset_min", 2, -WPE__PIXELS, WPE__PIXELS, element.offset_min.v) ||
        !wpe__read_ranged(l, v, "offset_max", 2, -WPE__PIXELS, WPE__PIXELS, element.offset_max.v) ||
        !wpe__json_read_bool(&l->r, v, "visible", &element.visible))
        return false;
    return wpe__add(l, v, e, WPE_COMP_UI, &element, "ui");
}

bool wpe__load_sprite(wpe__Loader* l, WPE_Entity e, const WPE_JsonValue* v) {
    WPE_Sprite sprite = wpe_sprite_default();
    float source[4] = {0};
    if (!WPE__JSON_CHECK_OBJECT(&l->r, v, "sprite", "image", "color", "source", "slice") ||
        !wpe__read_ranged(l, v, "color", 4, 0.0, 1.0, sprite.color.v) ||
        !wpe__read_ranged(l, v, "source", 4, 0.0, WPE__PIXELS, source) ||
        !wpe__read_ranged(l, v, "slice", 4, 0.0, WPE__PIXELS, sprite.slice.v))
        return false;
    sprite.source = (WPE_Rect){source[0], source[1], source[2], source[3]};
    if (!wpe__read_asset(l, v, "image", WPE_ASSET_IMAGE, &sprite.image)) return false;
    const bool ok = wpe__add(l, v, e, WPE_COMP_SPRITE, &sprite, "sprite");
    wpe_asset_release(l->context->assets, sprite.image); /* The component holds its own reference. */
    return ok;
}

bool wpe__load_text(wpe__Loader* l, WPE_Entity e, const WPE_JsonValue* v) {
    WPE_Text text = wpe_text_default();
    uint32_t align_x = text.align_x, align_y = text.align_y;
    if (!WPE__JSON_CHECK_OBJECT(&l->r, v, "text", "text", "font", "color", "scale", "align", "valign",
                                "wrap") ||
        !wpe__read_ranged(l, v, "color", 4, 0.0, 1.0, text.color.v) ||
        !wpe__json_read_float(&l->r, v, "scale", 0.1, 64.0, &text.scale) ||
        !wpe__json_read_enum(&l->r, v, "align", wpe__align_x, WPE__COUNT(wpe__align_x), &align_x) ||
        !wpe__json_read_enum(&l->r, v, "valign", wpe__align_y, WPE__COUNT(wpe__align_y), &align_y) ||
        !wpe__json_read_bool(&l->r, v, "wrap", &text.wrap))
        return false;
    const WPE_JsonValue* string = wpe_json_get(v, "text");
    if (string && !wpe_json_string(string))
        return wpe__json_fail(&l->r, string, WPE_RESULT_ERROR_PARSE, "\"text\" must be a string");
    text.text = (char*)(string ? wpe_json_string(string) : ""); /* The add hook copies it. */
    text.align_x = (WPE_UiAlign)align_x;
    text.align_y = (WPE_UiAlign)align_y;
    if (!wpe__read_asset(l, v, "font", WPE_ASSET_FONT, &text.font)) return false;
    const bool ok = wpe__add(l, v, e, WPE_COMP_TEXT, &text, "text");
    wpe_asset_release(l->context->assets, text.font);
    return ok;
}

bool wpe__load_button(wpe__Loader* l, WPE_Entity e, const WPE_JsonValue* v) {
    WPE_Button button = wpe_button_default();
    if (!WPE__JSON_CHECK_OBJECT(&l->r, v, "button", "interactable", "normal", "focused", "pressed",
                                "disabled") ||
        !wpe__json_read_bool(&l->r, v, "interactable", &button.interactable) ||
        !wpe__read_ranged(l, v, "normal", 4, 0.0, 2.0, button.normal_tint.v) ||
        !wpe__read_ranged(l, v, "focused", 4, 0.0, 2.0, button.focused_tint.v) ||
        !wpe__read_ranged(l, v, "pressed", 4, 0.0, 2.0, button.pressed_tint.v) ||
        !wpe__read_ranged(l, v, "disabled", 4, 0.0, 2.0, button.disabled_tint.v))
        return false;
    return wpe__add(l, v, e, WPE_COMP_BUTTON, &button, "button");
}
