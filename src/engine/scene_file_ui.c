/*
 * Scene files: the sprite-like components: the UI ("ui", "sprite", "text", "button") and 3D sprites
 * ("billboard"); format in docs/scenes.md.
 */
#define WPE_LOG_MODULE "scene"
#include <string.h>

#include "engine/scene_file_internal.h"
#include "graphics/graphics.h"
#include "ui/ui.h"

static const char* const wpe__align_x[] = {"left", "center", "right"};
static const char* const wpe__align_y[] = {"top", "middle", "bottom"};
static const char* const wpe__billboard_modes[] = {"fixed", "axis_aligned", "axis_aligned_y",
                                                   "screen_aligned"};
static const char* const wpe__billboard_blends[] = {"cutout", "alpha", "additive"};

#define WPE__COUNT(array) ((uint32_t)(sizeof(array) / sizeof((array)[0])))
#define WPE__DEGREES_PER_RADIAN (180.0 / 3.14159265358979323846)

static_assert(WPE__COUNT(wpe__align_x) == WPE_UI_ALIGN_END + 1, "horizontal alignment names");
static_assert(WPE__COUNT(wpe__align_y) == WPE_UI_ALIGN_END + 1, "vertical alignment names");
static_assert(WPE__COUNT(wpe__billboard_modes) == WPE_BILLBOARD_SCREEN_ALIGNED + 1, "billboard mode names");
static_assert(WPE__COUNT(wpe__billboard_blends) == WPE_BILLBOARD_ADDITIVE + 1, "billboard blend names");

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
    const WPE_Toggle* toggle = wpe_ecs_get_component(ecs, e, WPE_COMP_TOGGLE);
    if (toggle) {
        wpe_json_write_key(w, "toggle");
        wpe_json_write_object_begin(w);
        wpe_json_write_key(w, "on");
        wpe_json_write_bool(w, toggle->on);
        wpe__write_floats(w, "mark_color", toggle->mark_color.v, 4);
        wpe_json_write_key(w, "mark_inset");
        wpe_json_write_number(w, toggle->mark_inset);
        wpe_json_write_object_end(w);
    }
    const WPE_Slider* slider = wpe_ecs_get_component(ecs, e, WPE_COMP_SLIDER);
    if (slider) {
        wpe_json_write_key(w, "slider");
        wpe_json_write_object_begin(w);
        const struct {
            const char* key;
            float value;
        } numbers[] = {
            {"value", slider->value}, {"min", slider->min}, {"max", slider->max}, {"step", slider->step}};
        for (size_t i = 0; i < sizeof numbers / sizeof numbers[0]; ++i) {
            wpe_json_write_key(w, numbers[i].key);
            wpe_json_write_number(w, numbers[i].value);
        }
        wpe__write_floats(w, "track_color", slider->track_color.v, 4);
        wpe__write_floats(w, "fill_color", slider->fill_color.v, 4);
        wpe__write_floats(w, "handle_color", slider->handle_color.v, 4);
        wpe_json_write_key(w, "handle_width");
        wpe_json_write_number(w, slider->handle_width);
        wpe_json_write_object_end(w);
    }
}

void wpe__save_billboard(wpe__Saver* s, WPE_Entity e) {
    const WPE_Billboard* b = wpe_ecs_get_component(s->context->ecs, e, WPE_COMP_BILLBOARD);
    if (!b) return;
    WPE_JsonWriter* w = s->w;
    wpe_json_write_key(w, "billboard");
    wpe_json_write_object_begin(w);
    wpe__write_asset(s, "image", b->image);
    wpe__write_floats(w, "size", b->size.v, 2);
    wpe__write_floats(w, "pivot", b->pivot.v, 2);
    wpe_json_write_key(w, "rotation");
    wpe_json_write_rounded(w, (double)b->rotation * WPE__DEGREES_PER_RADIAN, 6);
    wpe__write_floats(w, "color", b->color.v, 4);
    wpe__write_rect(w, "source", b->source);
    const struct {
        const char* key;
        uint32_t value;
    } counts[] = {
        {"columns", b->columns}, {"rows", b->rows}, {"frame_count", b->frame_count}, {"frame", b->frame}};
    for (size_t i = 0; i < sizeof counts / sizeof counts[0]; ++i) {
        wpe_json_write_key(w, counts[i].key);
        wpe_json_write_integer(w, counts[i].value);
    }
    wpe_json_write_key(w, "fps");
    wpe_json_write_number(w, b->fps);
    wpe_json_write_key(w, "loop");
    wpe_json_write_bool(w, b->loop);
    wpe_json_write_key(w, "mode");
    wpe_json_write_string(w, wpe__billboard_modes[b->mode]);
    wpe_json_write_key(w, "blend");
    wpe_json_write_string(w, wpe__billboard_blends[b->blend]);
    wpe_json_write_key(w, "lit");
    wpe_json_write_bool(w, b->lit);
    wpe_json_write_key(w, "visible");
    wpe_json_write_bool(w, b->visible);
    wpe_json_write_object_end(w);
}

static const char* const wpe__particle_shapes[] = {"point", "sphere", "box"};
static_assert(WPE__COUNT(wpe__particle_shapes) == WPE_PARTICLES_BOX + 1, "particle shape names");

static void wpe__write_pair_of(WPE_JsonWriter* w, const char* key, float a, float b) {
    const float values[2] = {a, b};
    wpe__write_floats(w, key, values, 2);
}

void wpe__save_particles(wpe__Saver* s, WPE_Entity e) {
    const WPE_ParticleEmitter* p = wpe_ecs_get_component(s->context->ecs, e, WPE_COMP_PARTICLES);
    if (!p) return;
    WPE_JsonWriter* w = s->w;
    wpe_json_write_key(w, "particles");
    wpe_json_write_object_begin(w);
    wpe__write_asset(s, "image", p->image);
    const struct {
        const char* key;
        uint32_t value;
    } counts[] = {{"columns", p->columns},
                  {"rows", p->rows},
                  {"frame_count", p->frame_count},
                  {"max_particles", p->max_particles},
                  {"burst", p->burst},
                  {"seed", p->seed}};
    for (size_t i = 0; i < sizeof counts / sizeof counts[0]; ++i) {
        wpe_json_write_key(w, counts[i].key);
        wpe_json_write_integer(w, counts[i].value);
    }
    wpe_json_write_key(w, "blend");
    wpe_json_write_string(w, wpe__billboard_blends[p->blend]);
    const struct {
        const char* key;
        bool value;
    } flags[] = {{"lit", p->lit},
                 {"playing", p->playing},
                 {"loop", p->loop},
                 {"world_space", p->world_space},
                 {"random_rotation", p->random_rotation}};
    for (size_t i = 0; i < sizeof flags / sizeof flags[0]; ++i) {
        wpe_json_write_key(w, flags[i].key);
        wpe_json_write_bool(w, flags[i].value);
    }
    wpe__write_floats(w, "color_start", p->color_start.v, 4);
    wpe__write_floats(w, "color_end", p->color_end.v, 4);
    wpe__write_pair_of(w, "size", p->size_start, p->size_end);
    wpe_json_write_key(w, "duration");
    wpe_json_write_number(w, p->duration);
    wpe_json_write_key(w, "rate");
    wpe_json_write_number(w, p->rate);
    wpe_json_write_key(w, "shape");
    wpe_json_write_string(w, wpe__particle_shapes[p->shape]);
    wpe__write_floats(w, "shape_size", p->shape_size.v, 3);
    wpe__write_pair_of(w, "lifetime", p->lifetime_min, p->lifetime_max);
    wpe__write_pair_of(w, "speed", p->speed_min, p->speed_max);
    wpe__write_floats(w, "direction", p->direction.v, 3);
    wpe_json_write_key(w, "spread");
    wpe_json_write_rounded(w, (double)p->spread * WPE__DEGREES_PER_RADIAN, 6);
    wpe__write_floats(w, "gravity", p->gravity.v, 3);
    wpe_json_write_key(w, "drag");
    wpe_json_write_number(w, p->drag);
    wpe_json_write_key(w, "spin");
    wpe_json_write_array_begin(w, true);
    wpe_json_write_rounded(w, (double)p->spin_min * WPE__DEGREES_PER_RADIAN, 6);
    wpe_json_write_rounded(w, (double)p->spin_max * WPE__DEGREES_PER_RADIAN, 6);
    wpe_json_write_array_end(w);
    wpe_json_write_object_end(w);
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

bool wpe__load_billboard(wpe__Loader* l, WPE_Entity e, const WPE_JsonValue* v) {
    WPE_Billboard b = wpe_billboard_default();
    float source[4] = {0};
    uint32_t mode = b.mode, blend = b.blend;
    if (!WPE__JSON_CHECK_OBJECT(&l->r, v, "billboard", "image", "size", "pivot", "rotation", "color",
                                "source", "columns", "rows", "frame_count", "frame", "fps", "loop", "mode",
                                "blend", "lit", "visible") ||
        !wpe__read_ranged(l, v, "size", 2, 0.0, 1e6, b.size.v) ||
        !wpe__read_ranged(l, v, "pivot", 2, -10.0, 10.0, b.pivot.v) ||
        !wpe__json_read_degrees(&l->r, v, "rotation", -36000.0, 36000.0, &b.rotation) ||
        !wpe__read_ranged(l, v, "color", 4, 0.0, 1.0, b.color.v) ||
        !wpe__read_ranged(l, v, "source", 4, 0.0, WPE__PIXELS, source) ||
        !wpe__json_read_uint(&l->r, v, "columns", 1, 256, &b.columns) ||
        !wpe__json_read_uint(&l->r, v, "rows", 1, 256, &b.rows) ||
        !wpe__json_read_uint(&l->r, v, "frame_count", 0, 65536, &b.frame_count) ||
        !wpe__json_read_uint(&l->r, v, "frame", 0, 65535, &b.frame) ||
        !wpe__json_read_float(&l->r, v, "fps", 0.0, 1000.0, &b.fps) ||
        !wpe__json_read_bool(&l->r, v, "loop", &b.loop) ||
        !wpe__json_read_enum(&l->r, v, "mode", wpe__billboard_modes, WPE__COUNT(wpe__billboard_modes),
                             &mode) ||
        !wpe__json_read_enum(&l->r, v, "blend", wpe__billboard_blends, WPE__COUNT(wpe__billboard_blends),
                             &blend) ||
        !wpe__json_read_bool(&l->r, v, "lit", &b.lit) ||
        !wpe__json_read_bool(&l->r, v, "visible", &b.visible))
        return false;
    const uint32_t grid = b.columns * b.rows;
    if (b.frame_count > grid)
        return wpe__json_fail(&l->r, v, WPE_RESULT_ERROR_PARSE,
                              "\"frame_count\" (%u) is more than the %u x %u grid holds", b.frame_count,
                              b.columns, b.rows);
    if (b.frame >= (b.frame_count ? b.frame_count : grid))
        return wpe__json_fail(&l->r, v, WPE_RESULT_ERROR_PARSE, "\"frame\" %u is past the last frame",
                              b.frame);
    b.source = (WPE_Rect){source[0], source[1], source[2], source[3]};
    b.mode = (WPE_BillboardMode)mode;
    b.blend = (WPE_BillboardBlend)blend;
    if (!wpe__read_asset(l, v, "image", WPE_ASSET_IMAGE, &b.image)) return false;
    const bool ok = wpe__add(l, v, e, WPE_COMP_BILLBOARD, &b, "billboard");
    wpe_asset_release(l->context->assets, b.image); /* The component holds its own reference. */
    return ok;
}

/* An optional [low, high] pair within [min, max] with low <= high. */
static bool wpe__read_range(wpe__Loader* l, const WPE_JsonValue* object, const char* key, double min,
                            double max, float* io_low, float* io_high) {
    float pair[2] = {*io_low, *io_high};
    if (!wpe__read_ranged(l, object, key, 2, min, max, pair)) return false;
    if (pair[0] > pair[1])
        return wpe__json_fail(&l->r, wpe_json_get(object, key), WPE_RESULT_ERROR_PARSE,
                              "\"%s\" is [low, high]: the first must not exceed the second", key);
    *io_low = pair[0];
    *io_high = pair[1];
    return true;
}

bool wpe__load_particles(wpe__Loader* l, WPE_Entity e, const WPE_JsonValue* v) {
    WPE_ParticleEmitter p = wpe_particle_emitter_default();
    uint32_t blend = p.blend, shape = p.shape;
    float size[2] = {p.size_start, p.size_end}, spin[2] = {0, 0};
    if (!WPE__JSON_CHECK_OBJECT(&l->r, v, "particles", "image", "columns", "rows", "frame_count", "blend",
                                "lit", "color_start", "color_end", "size", "playing", "duration", "loop",
                                "rate", "burst", "max_particles", "world_space", "seed", "shape",
                                "shape_size", "lifetime", "speed", "direction", "spread", "gravity", "drag",
                                "spin", "random_rotation") ||
        !wpe__json_read_uint(&l->r, v, "columns", 1, 256, &p.columns) ||
        !wpe__json_read_uint(&l->r, v, "rows", 1, 256, &p.rows) ||
        !wpe__json_read_uint(&l->r, v, "frame_count", 0, 65536, &p.frame_count) ||
        !wpe__json_read_enum(&l->r, v, "blend", wpe__billboard_blends, WPE__COUNT(wpe__billboard_blends),
                             &blend) ||
        !wpe__json_read_bool(&l->r, v, "lit", &p.lit) ||
        !wpe__read_ranged(l, v, "color_start", 4, 0.0, 1.0, p.color_start.v) ||
        !wpe__read_ranged(l, v, "color_end", 4, 0.0, 1.0, p.color_end.v) ||
        !wpe__read_ranged(l, v, "size", 2, 0.0, 1e4, size) ||
        !wpe__json_read_bool(&l->r, v, "playing", &p.playing) ||
        !wpe__json_read_float(&l->r, v, "duration", 0.0, 1e6, &p.duration) ||
        !wpe__json_read_bool(&l->r, v, "loop", &p.loop) ||
        !wpe__json_read_float(&l->r, v, "rate", 0.0, 1e6, &p.rate) ||
        !wpe__json_read_uint(&l->r, v, "burst", 0, 100000, &p.burst) ||
        !wpe__json_read_uint(&l->r, v, "max_particles", 1, 100000, &p.max_particles) ||
        !wpe__json_read_bool(&l->r, v, "world_space", &p.world_space) ||
        !wpe__json_read_uint(&l->r, v, "seed", 0, UINT32_MAX, &p.seed) ||
        !wpe__json_read_enum(&l->r, v, "shape", wpe__particle_shapes, WPE__COUNT(wpe__particle_shapes),
                             &shape) ||
        !wpe__read_ranged(l, v, "shape_size", 3, 0.0, 1e6, p.shape_size.v) ||
        !wpe__read_range(l, v, "lifetime", 0.001, 1e4, &p.lifetime_min, &p.lifetime_max) ||
        !wpe__read_range(l, v, "speed", -1e4, 1e4, &p.speed_min, &p.speed_max) ||
        !wpe__json_read_vec3(&l->r, v, "direction", &p.direction) ||
        !wpe__json_read_degrees(&l->r, v, "spread", 0.0, 180.0, &p.spread) ||
        !wpe__json_read_vec3(&l->r, v, "gravity", &p.gravity) ||
        !wpe__json_read_float(&l->r, v, "drag", 0.0, 1e3, &p.drag) ||
        !wpe__read_range(l, v, "spin", -1e6, 1e6, &spin[0], &spin[1]) ||
        !wpe__json_read_bool(&l->r, v, "random_rotation", &p.random_rotation))
        return false;
    if (p.frame_count > p.columns * p.rows)
        return wpe__json_fail(&l->r, v, WPE_RESULT_ERROR_PARSE,
                              "\"frame_count\" (%u) is more than the %u x %u grid holds", p.frame_count,
                              p.columns, p.rows);
    p.size_start = size[0];
    p.size_end = size[1];
    p.spin_min = (float)(spin[0] / WPE__DEGREES_PER_RADIAN);
    p.spin_max = (float)(spin[1] / WPE__DEGREES_PER_RADIAN);
    p.blend = (WPE_BillboardBlend)blend;
    p.shape = (WPE_ParticleShape)shape;
    if (!wpe__read_asset(l, v, "image", WPE_ASSET_IMAGE, &p.image)) return false;
    const bool ok = wpe__add(l, v, e, WPE_COMP_PARTICLES, &p, "particles");
    wpe_asset_release(l->context->assets, p.image); /* The component holds its own reference. */
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

bool wpe__load_toggle(wpe__Loader* l, WPE_Entity e, const WPE_JsonValue* v) {
    WPE_Toggle toggle = wpe_toggle_default();
    if (!WPE__JSON_CHECK_OBJECT(&l->r, v, "toggle", "on", "mark_color", "mark_inset") ||
        !wpe__json_read_bool(&l->r, v, "on", &toggle.on) ||
        !wpe__read_ranged(l, v, "mark_color", 4, 0.0, 1.0, toggle.mark_color.v) ||
        !wpe__json_read_float(&l->r, v, "mark_inset", 0.0f, 1e4f, &toggle.mark_inset))
        return false;
    return wpe__add(l, v, e, WPE_COMP_TOGGLE, &toggle, "toggle");
}

bool wpe__load_slider(wpe__Loader* l, WPE_Entity e, const WPE_JsonValue* v) {
    WPE_Slider slider = wpe_slider_default();
    if (!WPE__JSON_CHECK_OBJECT(&l->r, v, "slider", "value", "min", "max", "step", "track_color",
                                "fill_color", "handle_color", "handle_width") ||
        !wpe__json_read_float(&l->r, v, "min", -1e9f, 1e9f, &slider.min) ||
        !wpe__json_read_float(&l->r, v, "max", -1e9f, 1e9f, &slider.max) ||
        !wpe__json_read_float(&l->r, v, "step", 0.0f, 1e9f, &slider.step) ||
        !wpe__json_read_float(&l->r, v, "value", -1e9f, 1e9f, &slider.value) ||
        !wpe__read_ranged(l, v, "track_color", 4, 0.0, 1.0, slider.track_color.v) ||
        !wpe__read_ranged(l, v, "fill_color", 4, 0.0, 1.0, slider.fill_color.v) ||
        !wpe__read_ranged(l, v, "handle_color", 4, 0.0, 1.0, slider.handle_color.v) ||
        !wpe__json_read_float(&l->r, v, "handle_width", 0.0f, 1e4f, &slider.handle_width))
        return false;
    if (slider.min == slider.max)
        return wpe__json_fail(&l->r, v, WPE_RESULT_ERROR_PARSE, "a slider's \"min\" and \"max\" must differ");
    /* Kept as written when it's in range, so loading and saving round-trips. */
    if (wpe_slider_constrain(&slider, slider.value) != slider.value)
        return wpe__json_fail(&l->r, v, WPE_RESULT_ERROR_PARSE,
                              "a slider's \"value\" must be within min..max (and on its step)");
    return wpe__add(l, v, e, WPE_COMP_SLIDER, &slider, "slider");
}
