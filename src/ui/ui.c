#define WPE_LOG_MODULE "ui"
#include "ui/ui.h"

#include <stdlib.h>
#include <string.h>

#include "core/alloc.h"
#include "core/debug.h"
#include "core/log.h"
#include "platform/platform.h"
#include "scene/scene.h"

#define WPE__NAV_DELAY 0.4f   /* Seconds a held direction waits before repeating. */
#define WPE__NAV_REPEAT 0.12f /* Seconds between repeats. */
#define WPE__STICK_PRESS 0.5f /* Stick deflection that counts as a direction. */

typedef enum wpe__Direction {
    WPE__DIR_NONE = -1,
    WPE__DIR_UP = 0,
    WPE__DIR_DOWN,
    WPE__DIR_LEFT,
    WPE__DIR_RIGHT,
} wpe__Direction;

/* One line of laid-out text: [start, end) and its width in font pixels. */
typedef struct wpe__Line {
    const char* start;
    const char* end;
    float width;
} wpe__Line;

struct WPE_Ui {
    WPE_Ecs* ecs;
    WPE_AssetManager* assets;
    WPE_Input* input;
    WPE_AssetHandle font;
    WPE_Draw2D* draw2d;
    WPE_Entity* order; /* Every element of the last layout, in drawing order. */
    uint32_t order_count, order_capacity;
    WPE_Entity* roots; /* Scratch for the layout. */
    uint32_t root_capacity;
    WPE_Entity* clicks;
    uint32_t click_count, click_capacity;
    WPE_Entity* changes; /* Toggles, sliders and text fields the player changed this update. */
    uint32_t change_count, change_capacity;
    WPE_Entity* submits; /* Text fields submitted (Return) this update. */
    uint32_t submit_count, submit_capacity;
    WPE_Entity editing;   /* The text field being edited. */
    WPE_Entity selecting; /* The text field the mouse drags a selection in. */
    wpe__Line* lines;     /* Scratch for text. */
    uint32_t line_capacity;
    WPE_Entity focus;
    WPE_Entity mouse_target;  /* The button the left mouse button went down on. */
    WPE_Entity submit_target; /* The button the submit input went down on. */
    WPE_Entity pressed;       /* Drawn pressed. */
    wpe__Direction held;      /* Navigation direction held, and time until it repeats. */
    float repeat_timer;
};

/* ---- Defaults ------------------------------------------------------------------------------------ */

WPE_UiRect wpe_ui_rect_default(void) {
    return (WPE_UiRect){.anchor_max = {.x = 1.0f, .y = 1.0f}, .visible = true};
}

WPE_Sprite wpe_sprite_default(void) {
    return (WPE_Sprite){.color = wpe_vec4(1, 1, 1, 1)};
}

WPE_Text wpe_text_default(void) {
    return (WPE_Text){.color = wpe_vec4(1, 1, 1, 1), .scale = 1.0f};
}

WPE_Button wpe_button_default(void) {
    return (WPE_Button){.interactable = true,
                        .normal_tint = wpe_vec4(1, 1, 1, 1),
                        .focused_tint = wpe_vec4(1.25f, 1.25f, 1.25f, 1),
                        .pressed_tint = wpe_vec4(0.75f, 0.75f, 0.75f, 1),
                        .disabled_tint = wpe_vec4(0.6f, 0.6f, 0.6f, 0.6f)};
}

WPE_Toggle wpe_toggle_default(void) {
    return (WPE_Toggle){.mark_color = wpe_vec4(1, 1, 1, 1), .mark_inset = 3.0f};
}

WPE_TextInput wpe_text_input_default(void) {
    return (WPE_TextInput){.placeholder_color = wpe_vec4(1, 1, 1, 0.45f),
                           .cursor_color = wpe_vec4(1, 1, 1, 1),
                           .selection_color = wpe_vec4(0.3f, 0.5f, 1.0f, 0.55f),
                           .padding = 3.0f};
}

WPE_Slider wpe_slider_default(void) {
    return (WPE_Slider){.max = 1.0f,
                        .track_color = wpe_vec4(0.3f, 0.3f, 0.35f, 1),
                        .fill_color = wpe_vec4(0.85f, 0.85f, 0.9f, 1),
                        .handle_color = wpe_vec4(1, 1, 1, 1),
                        .handle_width = 6.0f};
}

float wpe_slider_constrain(const WPE_Slider* slider, float value) {
    if (!slider) return 0.0f;
    const float lo = slider->min < slider->max ? slider->min : slider->max;
    const float hi = slider->min < slider->max ? slider->max : slider->min;
    if (!isfinite(value)) value = lo;
    if (slider->step > 0.0f && isfinite(slider->step))
        value = lo + roundf((value - lo) / slider->step) * slider->step;
    return value < lo ? lo : value > hi ? hi : value;
}

/* ---- Hooks ---------------------------------------------------------------------------------------- */

static void wpe__sprite_on_add(WPE_Ecs* ecs, WPE_Entity entity, void* component, void* user_data) {
    (void)ecs;
    WPE_Sprite* sprite = component;
    sprite->texture = wpe_asset_image(user_data, sprite->image);
    if (sprite->texture) {
        wpe_asset_retain(user_data, sprite->image);
    } else if (sprite->image != WPE_ASSET_NONE) {
        wpe_log_error("the sprite of entity 0x%08x references 0x%08x, which is not a loaded image", entity,
                      sprite->image);
    }
}

static void wpe__sprite_on_remove(WPE_Ecs* ecs, WPE_Entity entity, void* component, void* user_data) {
    (void)ecs;
    (void)entity;
    WPE_Sprite* sprite = component;
    if (sprite->texture) wpe_asset_release(user_data, sprite->image);
    sprite->texture = NULL;
}

static char* wpe__copy_string(const char* text) {
    const size_t length = strlen(text);
    char* copy = wpe_alloc(length + 1);
    if (copy) memcpy(copy, text, length + 1);
    return copy;
}

static void wpe__text_on_add(WPE_Ecs* ecs, WPE_Entity entity, void* component, void* user_data) {
    (void)ecs;
    WPE_Text* text = component;
    text->text = wpe__copy_string(text->text ? text->text : "");
    if (!text->text) wpe_log_error("out of memory copying the text of entity 0x%08x", entity);
    text->glyphs = wpe_asset_font(user_data, text->font);
    if (text->glyphs) {
        wpe_asset_retain(user_data, text->font);
    } else if (text->font != WPE_ASSET_NONE) {
        wpe_log_error("the text of entity 0x%08x references 0x%08x, which is not a loaded font", entity,
                      text->font);
    }
}

static void wpe__text_on_remove(WPE_Ecs* ecs, WPE_Entity entity, void* component, void* user_data) {
    (void)ecs;
    (void)entity;
    WPE_Text* text = component;
    wpe_free(text->text);
    text->text = NULL;
    if (text->glyphs) wpe_asset_release(user_data, text->font);
    text->glyphs = NULL;
}

static void wpe__text_input_on_add(WPE_Ecs* ecs, WPE_Entity entity, void* component, void* user_data) {
    (void)ecs;
    (void)user_data;
    WPE_TextInput* field = component;
    field->value = wpe__copy_string(field->value ? field->value : "");
    field->placeholder = wpe__copy_string(field->placeholder ? field->placeholder : "");
    if (!field->value || !field->placeholder)
        wpe_log_error("out of memory copying the text field of 0x%08x", entity);
    /* A copy of another field starts fresh: not editing, the cursor at the end. */
    field->editing = false;
    field->cursor = field->anchor = field->value ? (uint32_t)strlen(field->value) : 0;
    field->scroll = 0.0f;
    field->blink = 0.0f;
    field->wpe__before = NULL;
}

static void wpe__text_input_on_remove(WPE_Ecs* ecs, WPE_Entity entity, void* component, void* user_data) {
    (void)ecs;
    (void)entity;
    (void)user_data;
    WPE_TextInput* field = component;
    wpe_free(field->value);
    wpe_free(field->placeholder);
    wpe_free(field->wpe__before);
    field->value = field->placeholder = field->wpe__before = NULL;
}

/* ---- Lifecycle ------------------------------------------------------------------------------------ */

WPE_Result wpe_ui_create(const WPE_UiDesc* desc, WPE_Ui** out_ui) {
    if (!desc || !desc->ecs || !desc->assets || !out_ui) return WPE_RESULT_ERROR_NULL_ARG;
    *out_ui = NULL;
    const struct {
        WPE_ComponentType type;
        WPE_ComponentDesc desc;
    } components[] = {
        {WPE_COMP_UI, {.name = "UiRect", .size = sizeof(WPE_UiRect), .alignment = alignof(WPE_UiRect)}},
        {WPE_COMP_SPRITE,
         {.name = "Sprite",
          .size = sizeof(WPE_Sprite),
          .alignment = alignof(WPE_Sprite),
          .on_add = wpe__sprite_on_add,
          .on_remove = wpe__sprite_on_remove,
          .hook_user_data = desc->assets}},
        {WPE_COMP_TEXT,
         {.name = "Text",
          .size = sizeof(WPE_Text),
          .alignment = alignof(WPE_Text),
          .on_add = wpe__text_on_add,
          .on_remove = wpe__text_on_remove,
          .hook_user_data = desc->assets}},
        {WPE_COMP_BUTTON, {.name = "Button", .size = sizeof(WPE_Button), .alignment = alignof(WPE_Button)}},
        {WPE_COMP_TOGGLE, {.name = "Toggle", .size = sizeof(WPE_Toggle), .alignment = alignof(WPE_Toggle)}},
        {WPE_COMP_SLIDER, {.name = "Slider", .size = sizeof(WPE_Slider), .alignment = alignof(WPE_Slider)}},
        {WPE_COMP_TEXT_INPUT,
         {.name = "TextInput",
          .size = sizeof(WPE_TextInput),
          .alignment = alignof(WPE_TextInput),
          .on_add = wpe__text_input_on_add,
          .on_remove = wpe__text_input_on_remove}},
    };
    for (size_t i = 0; i < sizeof components / sizeof components[0]; ++i) {
        const WPE_Result result =
            wpe_ecs_register_component(desc->ecs, components[i].type, &components[i].desc);
        if (result != WPE_RESULT_OK) {
            wpe_log_error("failed to register component '%s': %s", components[i].desc.name,
                          wpe_result_to_string(result));
            for (size_t j = 0; j < i; ++j)
                wpe_ecs_unregister_component(desc->ecs, components[j].type);
            return result;
        }
    }
    WPE_Ui* ui = wpe_alloc_zeroed(1, sizeof *ui);
    WPE_Result result = ui ? wpe_draw2d_create(&ui->draw2d) : WPE_RESULT_ERROR_OUT_OF_MEMORY;
    if (result != WPE_RESULT_OK) {
        wpe_free(ui);
        for (size_t i = 0; i < sizeof components / sizeof components[0]; ++i)
            wpe_ecs_unregister_component(desc->ecs, components[i].type);
        return result;
    }
    ui->ecs = desc->ecs;
    ui->assets = desc->assets;
    ui->input = desc->input;
    ui->held = WPE__DIR_NONE;
    if (wpe_asset_font(desc->assets, desc->font)) {
        ui->font = desc->font;
        wpe_asset_retain(desc->assets, desc->font);
    } else if (desc->font != WPE_ASSET_NONE) {
        wpe_log_warn("the UI's default font 0x%08x is not a loaded font; texts need their own", desc->font);
    }
    *out_ui = ui;
    return WPE_RESULT_OK;
}

void wpe_ui_destroy(WPE_Ui* ui) {
    if (!ui) return;
    wpe_asset_release(ui->assets, ui->font);
    wpe_draw2d_destroy(ui->draw2d);
    wpe_free(ui->order);
    wpe_free(ui->roots);
    wpe_free(ui->clicks);
    wpe_free(ui->changes);
    wpe_free(ui->submits);
    wpe_free(ui->lines);
    wpe_free(ui);
}

/* ---- Layout --------------------------------------------------------------------------------------- */

/* Grows a buffer of `size`-byte items to hold at least `needed`. */
static bool wpe__reserve(void** buffer, uint32_t* capacity, uint32_t needed, size_t size) {
    if (needed <= *capacity) return true;
    uint32_t grown = *capacity ? *capacity * 2 : 64;
    while (grown < needed)
        grown *= 2;
    void* items = wpe_realloc(*buffer, (size_t)grown * size);
    if (!items) return false;
    *buffer = items;
    *capacity = grown;
    return true;
}

static WPE_UiRect* wpe__element(const WPE_Ui* ui, WPE_Entity entity) {
    return wpe_ecs_get_component(ui->ecs, entity, WPE_COMP_UI);
}

/* The element's parent when that is a UI element, else WPE_ENTITY_NULL (placed on the screen). */
static WPE_Entity wpe__ui_parent(const WPE_Ui* ui, WPE_Entity entity) {
    const WPE_Entity parent = wpe_scene_get_parent(ui->ecs, entity);
    return parent != WPE_ENTITY_NULL && wpe__element(ui, parent) ? parent : WPE_ENTITY_NULL;
}

/* The first UI element among `first` and its following siblings. */
static WPE_Entity wpe__ui_sibling(const WPE_Ui* ui, WPE_Entity first) {
    for (WPE_Entity e = first; e != WPE_ENTITY_NULL;) {
        if (wpe__element(ui, e)) return e;
        const WPE_Hierarchy* node = wpe_ecs_get_component(ui->ecs, e, WPE_COMP_HIERARCHY);
        e = node ? node->next_sibling : WPE_ENTITY_NULL;
    }
    return WPE_ENTITY_NULL;
}

static WPE_Entity wpe__first_child(const WPE_Ui* ui, WPE_Entity entity) {
    const WPE_Hierarchy* node = wpe_ecs_get_component(ui->ecs, entity, WPE_COMP_HIERARCHY);
    return node ? wpe__ui_sibling(ui, node->first_child) : WPE_ENTITY_NULL;
}

static WPE_Entity wpe__next_sibling(const WPE_Ui* ui, WPE_Entity entity) {
    const WPE_Hierarchy* node = wpe_ecs_get_component(ui->ecs, entity, WPE_COMP_HIERARCHY);
    return node ? wpe__ui_sibling(ui, node->next_sibling) : WPE_ENTITY_NULL;
}

/* Places one element in its parent's rectangle (or the screen's). */
static void wpe__place(WPE_Ui* ui, WPE_Entity entity, WPE_Rect screen) {
    WPE_UiRect* element = wpe__element(ui, entity);
    const WPE_Entity parent = wpe__ui_parent(ui, entity);
    const WPE_UiRect* outer = parent != WPE_ENTITY_NULL ? wpe__element(ui, parent) : NULL;
    const WPE_Rect p = outer ? outer->rect : screen;
    const float x0 = p.x + p.width * element->anchor_min.x + element->offset_min.x;
    const float y0 = p.y + p.height * element->anchor_min.y + element->offset_min.y;
    const float x1 = p.x + p.width * element->anchor_max.x + element->offset_max.x;
    const float y1 = p.y + p.height * element->anchor_max.y + element->offset_max.y;
    element->rect = (WPE_Rect){x0, y0, x1 > x0 ? x1 - x0 : 0.0f, y1 > y0 ? y1 - y0 : 0.0f};
    element->shown = element->visible && (!outer || outer->shown);
}

/* Insertion sort by spawn serial, so top-level elements draw in creation order (qsort has no context
 * argument to reach the world). Top-level elements are few. */
static void wpe__sort_roots(WPE_Ui* ui, uint32_t count) {
    for (uint32_t i = 1; i < count; ++i) {
        const WPE_Entity key = ui->roots[i];
        const uint64_t serial = wpe_ecs_spawn_serial(ui->ecs, key);
        uint32_t j = i;
        while (j > 0 && wpe_ecs_spawn_serial(ui->ecs, ui->roots[j - 1]) > serial) {
            ui->roots[j] = ui->roots[j - 1];
            --j;
        }
        ui->roots[j] = key;
    }
}

WPE_Result wpe_ui_layout(WPE_Ui* ui, uint32_t width, uint32_t height) {
    if (!ui) return WPE_RESULT_ERROR_NULL_ARG;
    ui->order_count = 0;
    const uint32_t total = wpe_ecs_component_count(ui->ecs, WPE_COMP_UI);
    if (!wpe__reserve((void**)&ui->order, &ui->order_capacity, total, sizeof *ui->order) ||
        !wpe__reserve((void**)&ui->roots, &ui->root_capacity, total, sizeof *ui->roots))
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    uint32_t root_count = 0;
    WPE_EcsIter it = wpe_ecs_query(ui->ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_UI)});
    while (wpe_ecs_iter_next(&it)) {
        if (wpe__ui_parent(ui, it.entity) == WPE_ENTITY_NULL && root_count < total)
            ui->roots[root_count++] = it.entity;
    }
    wpe__sort_roots(ui, root_count);
    const WPE_Rect screen = {0.0f, 0.0f, (float)width, (float)height};
    /* Depth first, parents before children: walks the sibling links without a stack. */
    for (uint32_t r = 0; r < root_count; ++r) {
        const WPE_Entity root = ui->roots[r];
        WPE_Entity e = root;
        while (e != WPE_ENTITY_NULL && ui->order_count < total) {
            wpe__place(ui, e, screen);
            ui->order[ui->order_count++] = e;
            const WPE_Entity child = wpe__first_child(ui, e);
            if (child != WPE_ENTITY_NULL) {
                e = child;
                continue;
            }
            WPE_Entity next = WPE_ENTITY_NULL;
            while (e != root && next == WPE_ENTITY_NULL) {
                next = wpe__next_sibling(ui, e);
                if (next == WPE_ENTITY_NULL) e = wpe__ui_parent(ui, e);
            }
            e = next;
        }
    }
    return WPE_RESULT_OK;
}

/* ---- Picking and focus ---------------------------------------------------------------------------- */

static bool wpe__contains(WPE_Rect r, float x, float y) {
    return x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height;
}

WPE_Entity wpe_ui_pick(const WPE_Ui* ui, float x, float y, bool any_element) {
    if (!ui) return WPE_ENTITY_NULL;
    for (uint32_t i = ui->order_count; i-- > 0;) {
        const WPE_Entity e = ui->order[i];
        const WPE_UiRect* element = wpe__element(ui, e);
        if (!element || !element->shown || !wpe__contains(element->rect, x, y)) continue;
        if (any_element || wpe_ecs_has_component(ui->ecs, e, WPE_COMP_SPRITE) ||
            wpe_ecs_has_component(ui->ecs, e, WPE_COMP_BUTTON))
            return e;
    }
    return WPE_ENTITY_NULL;
}

/* Text fields (defined with their section, below). */
static void wpe__stop_editing(WPE_Ui* ui);
static void wpe__start_editing(WPE_Ui* ui, WPE_Entity entity);
static void wpe__field_mouse(WPE_Ui* ui, WPE_Entity hovered);
static void wpe__field_keys(WPE_Ui* ui, float dt);
static WPE_TextInput* wpe__field(const WPE_Ui* ui, WPE_Entity entity);

/* Whether a button can take the focus: alive, shown by the last layout and interactable. */
static bool wpe__focusable(const WPE_Ui* ui, WPE_Entity entity) {
    const WPE_Button* button = wpe_ecs_get_component(ui->ecs, entity, WPE_COMP_BUTTON);
    const WPE_UiRect* element = wpe__element(ui, entity);
    return button && element && button->interactable && element->shown;
}

/* The button an element belongs to: itself or its nearest UI ancestor with a button. */
static WPE_Entity wpe__owning_button(const WPE_Ui* ui, WPE_Entity entity) {
    for (WPE_Entity e = entity; e != WPE_ENTITY_NULL; e = wpe__ui_parent(ui, e)) {
        if (wpe_ecs_has_component(ui->ecs, e, WPE_COMP_BUTTON)) return e;
    }
    return WPE_ENTITY_NULL;
}

bool wpe_ui_focus(WPE_Ui* ui, WPE_Entity button) {
    if (!ui) return false;
    if (button != WPE_ENTITY_NULL && !wpe__focusable(ui, button)) return false;
    if (button != ui->editing) wpe__stop_editing(ui);
    ui->focus = button;
    return true;
}

WPE_Entity wpe_ui_focused(const WPE_Ui* ui) {
    return ui ? ui->focus : WPE_ENTITY_NULL;
}

static WPE_Entity wpe__first_focusable(const WPE_Ui* ui) {
    for (uint32_t i = 0; i < ui->order_count; ++i) {
        if (wpe__focusable(ui, ui->order[i])) return ui->order[i];
    }
    return WPE_ENTITY_NULL;
}

/* The focusable button nearest to the focused one in a direction: ahead of it, favoring buttons in
 * line with it (sideways distance counts double). */
static WPE_Entity wpe__neighbor(const WPE_Ui* ui, WPE_Entity from, wpe__Direction direction) {
    const WPE_Rect r = wpe__element(ui, from)->rect;
    const float cx = r.x + r.width * 0.5f, cy = r.y + r.height * 0.5f;
    static const float dx[] = {0, 0, -1, 1}, dy[] = {-1, 1, 0, 0};
    WPE_Entity best = WPE_ENTITY_NULL;
    float best_score = 0.0f;
    for (uint32_t i = 0; i < ui->order_count; ++i) {
        const WPE_Entity e = ui->order[i];
        if (e == from || !wpe__focusable(ui, e)) continue;
        const WPE_Rect o = wpe__element(ui, e)->rect;
        const float vx = o.x + o.width * 0.5f - cx, vy = o.y + o.height * 0.5f - cy;
        const float ahead = vx * dx[direction] + vy * dy[direction];
        const float side = fabsf(vx * dy[direction] - vy * dx[direction]);
        if (ahead < 1.0f) continue;
        const float score = ahead + 2.0f * side;
        if (best == WPE_ENTITY_NULL || score < best_score) {
            best = e;
            best_score = score;
        }
    }
    return best;
}

/* ---- Interaction ---------------------------------------------------------------------------------- */

/* Records that the player changed a toggle or slider (once per update). */
static void wpe__changed(WPE_Ui* ui, WPE_Entity entity) {
    for (uint32_t i = 0; i < ui->change_count; ++i)
        if (ui->changes[i] == entity) return;
    if (!wpe__reserve((void**)&ui->changes, &ui->change_capacity, ui->change_count + 1,
                      sizeof *ui->changes)) {
        wpe_log_error("out of memory: a change of 0x%08x was not reported", entity);
        return;
    }
    ui->changes[ui->change_count++] = entity;
}

/* Clicks a button; a toggle flips. */
static void wpe__click(WPE_Ui* ui, WPE_Entity button) {
    WPE_Toggle* toggle = wpe_ecs_get_component(ui->ecs, button, WPE_COMP_TOGGLE);
    if (toggle) {
        toggle->on = !toggle->on;
        wpe__changed(ui, button);
    }
    if (!wpe__reserve((void**)&ui->clicks, &ui->click_capacity, ui->click_count + 1, sizeof *ui->clicks)) {
        wpe_log_error("out of memory: a click on 0x%08x was dropped", button);
        return;
    }
    ui->clicks[ui->click_count++] = button;
}

/* Sets a slider's value as the player moved it, reporting a change. */
static void wpe__slide_to(WPE_Ui* ui, WPE_Entity entity, WPE_Slider* slider, float value) {
    value = wpe_slider_constrain(slider, value);
    if (value == slider->value) return;
    slider->value = value;
    wpe__changed(ui, entity);
}

/* A slider's step for keys and the pad. */
static float wpe__slider_step(const WPE_Slider* slider) {
    return slider->step > 0.0f ? slider->step : fabsf(slider->max - slider->min) / 20.0f;
}

/* The slider value at a screen x: the handle's center follows the mouse. */
static float wpe__slider_value_at(const WPE_Slider* slider, WPE_Rect rect, float x) {
    const float half = slider->handle_width * 0.5f;
    const float span = rect.width - slider->handle_width;
    const float t = span > 0.0f ? (x - rect.x - half) / span : 0.0f;
    const float clamped = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    return slider->min + (slider->max - slider->min) * clamped;
}

/* A navigation input: the project's action of that name when there is one, else the defaults. */
typedef struct wpe__NavInput {
    bool down, pressed, released;
} wpe__NavInput;

static wpe__NavInput wpe__action(const WPE_Input* input, WPE_InputAction action) {
    return (wpe__NavInput){.down = wpe_input_action_down(input, action, WPE_INPUT_ANY_PLAYER),
                           .pressed = wpe_input_action_pressed(input, action, WPE_INPUT_ANY_PLAYER),
                           .released = wpe_input_action_released(input, action, WPE_INPUT_ANY_PLAYER)};
}

static bool wpe__direction_down(const WPE_Input* input, wpe__Direction direction) {
    static const char* const names[] = {"ui_up", "ui_down", "ui_left", "ui_right"};
    const WPE_InputAction action = wpe_input_find_action(input, names[direction]);
    if (action != WPE_INPUT_ACTION_NONE) return wpe__action(input, action).down;
    static const WPE_Key keys[] = {WPE_KEY_UP, WPE_KEY_DOWN, WPE_KEY_LEFT, WPE_KEY_RIGHT};
    static const WPE_GamepadButton pads[] = {WPE_PAD_DPAD_UP, WPE_PAD_DPAD_DOWN, WPE_PAD_DPAD_LEFT,
                                             WPE_PAD_DPAD_RIGHT};
    const float stick = wpe_input_gamepad_axis(input, WPE_INPUT_ANY_PLAYER,
                                               direction <= WPE__DIR_DOWN ? WPE_PAD_LEFT_Y : WPE_PAD_LEFT_X);
    const bool positive = direction == WPE__DIR_DOWN || direction == WPE__DIR_RIGHT;
    return wpe_input_key_down(input, keys[direction]) ||
           wpe_input_gamepad_down(input, WPE_INPUT_ANY_PLAYER, pads[direction]) ||
           (positive ? stick >= WPE__STICK_PRESS : stick <= -WPE__STICK_PRESS);
}

static wpe__NavInput wpe__submit(const WPE_Input* input) {
    const WPE_InputAction action = wpe_input_find_action(input, "ui_submit");
    if (action != WPE_INPUT_ACTION_NONE) return wpe__action(input, action);
    const WPE_Key keys[] = {WPE_KEY_RETURN, WPE_KEY_SPACE};
    wpe__NavInput nav = {.down = wpe_input_gamepad_down(input, WPE_INPUT_ANY_PLAYER, WPE_PAD_SOUTH),
                         .pressed = wpe_input_gamepad_pressed(input, WPE_INPUT_ANY_PLAYER, WPE_PAD_SOUTH),
                         .released = wpe_input_gamepad_released(input, WPE_INPUT_ANY_PLAYER, WPE_PAD_SOUTH)};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; ++i) {
        nav.down = nav.down || wpe_input_key_down(input, keys[i]);
        nav.pressed = nav.pressed || wpe_input_key_pressed(input, keys[i]);
        nav.released = nav.released || wpe_input_key_released(input, keys[i]);
    }
    return nav;
}

/* Directions move the focus at once, then repeat while held. */
static void wpe__navigate(WPE_Ui* ui, float dt) {
    wpe__Direction held = WPE__DIR_NONE;
    for (int d = WPE__DIR_UP; d <= WPE__DIR_RIGHT && held == WPE__DIR_NONE; ++d) {
        if (wpe__direction_down(ui->input, (wpe__Direction)d)) held = (wpe__Direction)d;
    }
    bool step = false;
    if (held != ui->held) {
        step = held != WPE__DIR_NONE;
        ui->repeat_timer = WPE__NAV_DELAY;
    } else if (held != WPE__DIR_NONE) {
        ui->repeat_timer -= dt;
        if (ui->repeat_timer <= 0.0f) {
            step = true;
            ui->repeat_timer += WPE__NAV_REPEAT;
            if (ui->repeat_timer <= 0.0f) ui->repeat_timer = WPE__NAV_REPEAT;
        }
    }
    ui->held = held;
    if (!step) return;
    if (ui->focus == WPE_ENTITY_NULL) {
        ui->focus = wpe__first_focusable(ui);
        return;
    }
    /* Left and right move a focused slider instead of the focus. */
    WPE_Slider* slider = wpe_ecs_get_component(ui->ecs, ui->focus, WPE_COMP_SLIDER);
    if (slider && (held == WPE__DIR_LEFT || held == WPE__DIR_RIGHT)) {
        const float direction = (held == WPE__DIR_RIGHT) == (slider->max >= slider->min) ? 1.0f : -1.0f;
        wpe__slide_to(ui, ui->focus, slider, slider->value + direction * wpe__slider_step(slider));
        return;
    }
    const WPE_Entity next = wpe__neighbor(ui, ui->focus, held);
    if (next != WPE_ENTITY_NULL) ui->focus = next;
}

static void wpe__submit_input(WPE_Ui* ui) {
    const wpe__NavInput submit = wpe__submit(ui->input);
    if (submit.pressed) {
        if (ui->focus == WPE_ENTITY_NULL) {
            ui->focus = wpe__first_focusable(ui);
        } else {
            ui->submit_target = ui->focus;
        }
    }
    /* A press and release within one frame still clicks. */
    if (submit.released || (submit.pressed && !submit.down)) {
        if (ui->submit_target != WPE_ENTITY_NULL && ui->submit_target == ui->focus &&
            wpe__focusable(ui, ui->submit_target))
            wpe__click(ui, ui->submit_target);
        ui->submit_target = WPE_ENTITY_NULL;
    }
}

static WPE_Entity wpe__mouse_input(WPE_Ui* ui) {
    if (wpe_input_mouse_captured(ui->input)) {
        ui->mouse_target = WPE_ENTITY_NULL;
        return WPE_ENTITY_NULL;
    }
    float x, y, dx, dy;
    wpe_input_mouse_position(ui->input, &x, &y);
    wpe_input_mouse_delta(ui->input, &dx, &dy);
    WPE_Entity hovered = wpe__owning_button(ui, wpe_ui_pick(ui, x, y, false));
    if (hovered != WPE_ENTITY_NULL && !wpe__focusable(ui, hovered)) hovered = WPE_ENTITY_NULL;
    const bool pressed = wpe_input_mouse_pressed(ui->input, WPE_MOUSE_LEFT);
    /* While a field edits, only a press moves the focus (passing over buttons doesn't). */
    if (hovered != WPE_ENTITY_NULL && (dx != 0.0f || dy != 0.0f || pressed) &&
        (ui->editing == WPE_ENTITY_NULL || pressed))
        ui->focus = hovered;
    if (pressed) ui->mouse_target = hovered;
    if (wpe_input_mouse_released(ui->input, WPE_MOUSE_LEFT)) {
        if (ui->mouse_target != WPE_ENTITY_NULL && ui->mouse_target == hovered) wpe__click(ui, hovered);
        ui->mouse_target = WPE_ENTITY_NULL;
    }
    return hovered;
}

WPE_Result wpe_ui_update(WPE_Ui* ui, uint32_t width, uint32_t height, float dt) {
    if (!ui) return WPE_RESULT_ERROR_NULL_ARG;
    ui->click_count = 0;
    ui->change_count = 0;
    ui->submit_count = 0;
    const WPE_Result result = wpe_ui_layout(ui, width, height);
    if (result != WPE_RESULT_OK) return result;
    /* A field that went away, hid or lost the focus stops editing. */
    if (ui->editing != WPE_ENTITY_NULL &&
        (!wpe__focusable(ui, ui->editing) || !wpe__field(ui, ui->editing) || ui->focus != ui->editing))
        wpe__stop_editing(ui);
    /* Buttons that were removed, hidden or disabled let go of the focus and of presses. */
    if (ui->focus != WPE_ENTITY_NULL && !wpe__focusable(ui, ui->focus)) ui->focus = WPE_ENTITY_NULL;
    if (ui->mouse_target != WPE_ENTITY_NULL && !wpe__focusable(ui, ui->mouse_target))
        ui->mouse_target = WPE_ENTITY_NULL;
    if (ui->submit_target != WPE_ENTITY_NULL && !wpe__focusable(ui, ui->submit_target))
        ui->submit_target = WPE_ENTITY_NULL;
    ui->pressed = WPE_ENTITY_NULL;
    if (!ui->input) return WPE_RESULT_OK;
    const WPE_Entity hovered = wpe__mouse_input(ui);
    /* A slider pressed with the mouse follows it until the button is let go, even off the slider. */
    WPE_Slider* dragged = ui->mouse_target != WPE_ENTITY_NULL
                              ? wpe_ecs_get_component(ui->ecs, ui->mouse_target, WPE_COMP_SLIDER)
                              : NULL;
    if (dragged && wpe_input_mouse_down(ui->input, WPE_MOUSE_LEFT)) {
        float x, y;
        wpe_input_mouse_position(ui->input, &x, &y);
        wpe__slide_to(ui, ui->mouse_target, dragged,
                      wpe__slider_value_at(dragged, wpe__element(ui, ui->mouse_target)->rect, x));
    }
    const WPE_Entity was_editing = ui->editing;
    wpe__field_mouse(ui, hovered);
    if (ui->editing != WPE_ENTITY_NULL) {
        /* The keyboard types. A field that just started editing takes keys from the next frame. */
        if (ui->editing == was_editing) wpe__field_keys(ui, dt);
    } else if (wpe__field(ui, ui->focus) && wpe__submit(ui->input).pressed) {
        wpe__start_editing(ui, ui->focus); /* Submitting a field edits it (no click). */
    } else {
        wpe__navigate(ui, dt);
        wpe__submit_input(ui);
    }
    if (ui->submit_target != WPE_ENTITY_NULL) {
        ui->pressed = ui->submit_target;
    } else if (ui->mouse_target != WPE_ENTITY_NULL && ui->mouse_target == hovered &&
               wpe_input_mouse_down(ui->input, WPE_MOUSE_LEFT)) {
        ui->pressed = ui->mouse_target;
    }
    return WPE_RESULT_OK;
}

const WPE_Entity* wpe_ui_clicks(const WPE_Ui* ui, uint32_t* out_count) {
    if (out_count) *out_count = ui ? ui->click_count : 0;
    return ui ? ui->clicks : NULL;
}

const WPE_Entity* wpe_ui_changes(const WPE_Ui* ui, uint32_t* out_count) {
    if (out_count) *out_count = ui ? ui->change_count : 0;
    return ui ? ui->changes : NULL;
}

/* ---- Text fields ---------------------------------------------------------------------------------- */

#define WPE__FIELD_MAX 256u /* Characters when max_length is 0. */
#define WPE__FIELD_LIMIT 4096u
#define WPE__BLINK 1.0f /* Seconds per cursor blink (shown the first half). */

static WPE_TextInput* wpe__field(const WPE_Ui* ui, WPE_Entity entity) {
    return entity != WPE_ENTITY_NULL ? wpe_ecs_get_component(ui->ecs, entity, WPE_COMP_TEXT_INPUT) : NULL;
}

static const char* wpe__field_value(const WPE_TextInput* field) {
    return field->value ? field->value : "";
}

static uint32_t wpe__field_max(const WPE_TextInput* field) {
    const uint32_t max = field->max_length ? field->max_length : WPE__FIELD_MAX;
    return max < WPE__FIELD_LIMIT ? max : WPE__FIELD_LIMIT;
}

static bool wpe__is_alnum(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* Whether `c` may go in at `at` (number filters look at the rest of the text). */
static bool wpe__accepts(const WPE_TextInput* field, const char* value, uint32_t at, char c) {
    if (c < 32 || c > 126) return false; /* The fonts' glyphs. */
    switch (field->filter) {
    case WPE_TEXT_FILTER_DIGITS:
        return c >= '0' && c <= '9';
    case WPE_TEXT_FILTER_NUMBER:
        if (c >= '0' && c <= '9') return true;
        if (c == '-') return at == 0 && value[0] != '-';
        return c == '.' && !strchr(value, '.');
    case WPE_TEXT_FILTER_NAME:
        return wpe__is_alnum(c) || c == ' ' || c == '_' || c == '-' || c == '.';
    case WPE_TEXT_FILTER_ADDRESS:
        return wpe__is_alnum(c) || c == '.' || c == ':' || c == '-' || c == '[' || c == ']';
    case WPE_TEXT_FILTER_ANY:
        break;
    }
    return true;
}

/* Replaces [from, to) with `insert` (n bytes); the cursor ends after it. @return false when out of memory. */
static bool wpe__replace(WPE_TextInput* field, uint32_t from, uint32_t to, const char* insert, size_t n) {
    const char* value = wpe__field_value(field);
    const size_t length = strlen(value);
    char* next = wpe_alloc(length - (to - from) + n + 1);
    if (!next) {
        wpe_log_error("out of memory editing a text field");
        return false;
    }
    memcpy(next, value, from);
    memcpy(next + from, insert, n);
    memcpy(next + from + n, value + to, length - to + 1);
    wpe_free(field->value);
    field->value = next;
    field->cursor = field->anchor = from + (uint32_t)n;
    return true;
}

static uint32_t wpe__selection_lo(const WPE_TextInput* f) {
    return f->cursor < f->anchor ? f->cursor : f->anchor;
}

static uint32_t wpe__selection_hi(const WPE_TextInput* f) {
    return f->cursor < f->anchor ? f->anchor : f->cursor;
}

/* Inserts typed or pasted text where the cursor is (over the selection), keeping what the filter accepts and
 * what fits. @return Whether the text changed. */
static bool wpe__type(WPE_TextInput* field, const char* text) {
    const uint32_t lo = wpe__selection_lo(field), hi = wpe__selection_hi(field);
    const char* value = wpe__field_value(field);
    const size_t length = strlen(value) - (hi - lo);
    char accepted[WPE__FIELD_LIMIT];
    size_t n = 0;
    for (const char* c = text; *c && length + n < wpe__field_max(field); ++c) {
        /* The text as it would be so far (for the number filter's one '-' and one '.'). */
        char sample[2 * WPE__FIELD_LIMIT + 1];
        const size_t tail = strlen(value + hi);
        memcpy(sample, value, lo);
        memcpy(sample + lo, accepted, n);
        memcpy(sample + lo + n, value + hi, tail + 1);
        if (wpe__accepts(field, sample, lo + (uint32_t)n, *c)) accepted[n++] = *c;
    }
    if (n == 0) return false; /* Nothing accepted: a selection stays, too. */
    return wpe__replace(field, lo, hi, accepted, n);
}

/* Where the cursor lands moving by a word: the next word's start going right, the current one's going left.
 */
static uint32_t wpe__word(const char* value, uint32_t from, int direction) {
    uint32_t at = from;
    const uint32_t length = (uint32_t)strlen(value);
    if (direction < 0) {
        while (at > 0 && !wpe__is_alnum(value[at - 1]))
            --at;
        while (at > 0 && wpe__is_alnum(value[at - 1]))
            --at;
    } else {
        while (at < length && wpe__is_alnum(value[at]))
            ++at;
        while (at < length && !wpe__is_alnum(value[at]))
            ++at;
    }
    return at;
}

/* The font and scale a field draws with: its entity's text, else the UI's default font at scale 1. */
static const WPE_BitmapFont* wpe__field_font(const WPE_Ui* ui, WPE_Entity entity, float* out_scale) {
    const WPE_Text* text = wpe_ecs_get_component(ui->ecs, entity, WPE_COMP_TEXT);
    *out_scale = text && text->scale > 0.0f ? text->scale : 1.0f;
    return text && text->glyphs ? text->glyphs : wpe_asset_font(ui->assets, ui->font);
}

static char wpe__shown_char(const WPE_TextInput* field, const char* value, uint32_t i) {
    return field->password ? '*' : value[i];
}

/* The x of the gap before character `index`, in pixels from the text's start. */
static float wpe__caret_x(const WPE_BitmapFont* font, float scale, const WPE_TextInput* field,
                          uint32_t index) {
    const char* value = wpe__field_value(field);
    float x = 0.0f;
    for (uint32_t i = 0; i < index && value[i]; ++i)
        x += (float)(wpe_font_glyph(font, wpe__shown_char(field, value, i))->width + font->glyph_spacing) *
             scale;
    return x;
}

/* The character gap nearest to `x` (pixels from the text's start). */
static uint32_t wpe__caret_at(const WPE_BitmapFont* font, float scale, const WPE_TextInput* field, float x) {
    const char* value = wpe__field_value(field);
    float left = 0.0f;
    uint32_t i = 0;
    for (; value[i]; ++i) {
        const float advance =
            (float)(wpe_font_glyph(font, wpe__shown_char(field, value, i))->width + font->glyph_spacing) *
            scale;
        if (x < left + advance * 0.5f) return i;
        left += advance;
    }
    return i;
}

/* Scrolls the field so its cursor stays inside it. */
static void wpe__scroll_to_cursor(const WPE_Ui* ui, WPE_Entity entity, WPE_TextInput* field) {
    float scale;
    const WPE_BitmapFont* font = wpe__field_font(ui, entity, &scale);
    const WPE_UiRect* element = wpe__element(ui, entity);
    if (!font || !element) return;
    const float inner = element->rect.width - 2.0f * field->padding;
    const float caret = wpe__caret_x(font, scale, field, field->cursor);
    const float total = wpe__caret_x(font, scale, field, (uint32_t)strlen(wpe__field_value(field)));
    if (caret - field->scroll > inner - scale) field->scroll = caret - inner + scale;
    if (caret - field->scroll < 0.0f) field->scroll = caret;
    /* No empty space past the end while text is hidden on the left. */
    const float most = total + scale - inner;
    if (field->scroll > most) field->scroll = most > 0.0f ? most : 0.0f;
}

static void wpe__stop_editing(WPE_Ui* ui) {
    WPE_TextInput* field = wpe__field(ui, ui->editing);
    if (field) {
        field->editing = false;
        field->scroll = 0.0f;
        field->anchor = field->cursor;
        wpe_free(field->wpe__before);
        field->wpe__before = NULL;
    }
    ui->editing = WPE_ENTITY_NULL;
    ui->selecting = WPE_ENTITY_NULL;
}

static void wpe__start_editing(WPE_Ui* ui, WPE_Entity entity) {
    if (ui->editing == entity) return;
    wpe__stop_editing(ui);
    WPE_TextInput* field = wpe__field(ui, entity);
    if (!field) return;
    ui->editing = entity;
    ui->focus = entity;
    field->editing = true;
    field->blink = 0.0f;
    field->cursor = field->anchor = (uint32_t)strlen(wpe__field_value(field));
    field->wpe__before = wpe__copy_string(wpe__field_value(field));
}

static void wpe__submitted(WPE_Ui* ui, WPE_Entity entity) {
    if (!wpe__reserve((void**)&ui->submits, &ui->submit_capacity, ui->submit_count + 1,
                      sizeof *ui->submits)) {
        wpe_log_error("out of memory: a submit of 0x%08x was not reported", entity);
        return;
    }
    ui->submits[ui->submit_count++] = entity;
}

/* The next (or previous) focusable element in drawing order, wrapping around. */
static WPE_Entity wpe__tab_order(const WPE_Ui* ui, WPE_Entity from, int direction) {
    if (ui->order_count == 0) return WPE_ENTITY_NULL;
    uint32_t start = 0;
    for (uint32_t i = 0; i < ui->order_count; ++i) {
        if (ui->order[i] == from) start = i;
    }
    for (uint32_t step = 1; step <= ui->order_count; ++step) {
        const uint32_t i = (start + ui->order_count + (uint32_t)((int)step * direction)) % ui->order_count;
        if (wpe__focusable(ui, ui->order[i])) return ui->order[i];
    }
    return WPE_ENTITY_NULL;
}

/* The mouse in text fields: a press starts editing with the cursor there, dragging selects, a press anywhere
 * else stops editing. */
static void wpe__field_mouse(WPE_Ui* ui, WPE_Entity hovered) {
    if (wpe_input_mouse_captured(ui->input)) return;
    float x, y;
    wpe_input_mouse_position(ui->input, &x, &y);
    const bool pressed = wpe_input_mouse_pressed(ui->input, WPE_MOUSE_LEFT);
    if (pressed) {
        if (wpe__field(ui, hovered)) {
            wpe__start_editing(ui, hovered);
            ui->selecting = hovered;
        } else {
            wpe__stop_editing(ui);
        }
    }
    WPE_TextInput* field = wpe__field(ui, ui->selecting);
    if (field && field->editing && wpe_input_mouse_down(ui->input, WPE_MOUSE_LEFT)) {
        float scale;
        const WPE_BitmapFont* font = wpe__field_font(ui, ui->selecting, &scale);
        if (font) {
            const float local = x - wpe__element(ui, ui->selecting)->rect.x - field->padding + field->scroll;
            field->cursor = wpe__caret_at(font, scale, field, local);
            if (pressed) field->anchor = field->cursor;
            field->blink = 0.0f;
        }
    } else {
        ui->selecting = WPE_ENTITY_NULL;
    }
}

/* The keyboard while a field edits. */
static void wpe__field_keys(WPE_Ui* ui, float dt) {
    const WPE_Entity entity = ui->editing;
    WPE_TextInput* field = wpe__field(ui, entity);
    if (!field) return;
    const WPE_Input* in = ui->input;
    const bool ctrl = wpe_input_key_down(in, WPE_KEY_LCTRL) || wpe_input_key_down(in, WPE_KEY_RCTRL);
    const bool shift = wpe_input_key_down(in, WPE_KEY_LSHIFT) || wpe_input_key_down(in, WPE_KEY_RSHIFT);
    bool changed = false, moved = false;
    /* Typed characters (the layout's; Ctrl shortcuts type nothing). */
    if (!ctrl && wpe_input_text(in)[0]) changed |= wpe__type(field, wpe_input_text(in));
    for (uint32_t n = wpe_input_key_typed(in, WPE_KEY_BACKSPACE); n > 0; --n) {
        const uint32_t lo = wpe__selection_lo(field), hi = wpe__selection_hi(field);
        const uint32_t from = lo != hi ? lo
                              : ctrl   ? wpe__word(wpe__field_value(field), lo, -1)
                              : lo > 0 ? lo - 1
                                       : 0;
        if (from != hi) changed |= wpe__replace(field, from, hi, "", 0);
    }
    for (uint32_t n = wpe_input_key_typed(in, WPE_KEY_DELETE); n > 0; --n) {
        const uint32_t lo = wpe__selection_lo(field), hi = wpe__selection_hi(field);
        const uint32_t length = (uint32_t)strlen(wpe__field_value(field));
        const uint32_t to = lo != hi      ? hi
                            : ctrl        ? wpe__word(wpe__field_value(field), hi, 1)
                            : hi < length ? hi + 1
                                          : hi;
        if (to != lo) changed |= wpe__replace(field, lo, to, "", 0);
    }
    const uint32_t length = (uint32_t)strlen(wpe__field_value(field));
    for (int dir = -1; dir <= 1; dir += 2) {
        const WPE_Key key = dir < 0 ? WPE_KEY_LEFT : WPE_KEY_RIGHT;
        for (uint32_t n = wpe_input_key_typed(in, key); n > 0; --n) {
            const uint32_t lo = wpe__selection_lo(field), hi = wpe__selection_hi(field);
            uint32_t to;
            if (ctrl) {
                to = wpe__word(wpe__field_value(field), field->cursor, dir);
            } else if (lo != hi && !shift) {
                to = dir < 0 ? lo : hi; /* Collapse the selection to its side. */
            } else {
                to = dir < 0 ? (field->cursor > 0 ? field->cursor - 1 : 0)
                             : (field->cursor < length ? field->cursor + 1 : length);
            }
            field->cursor = to;
            if (!shift) field->anchor = to;
            moved = true;
        }
    }
    if (wpe_input_key_typed(in, WPE_KEY_HOME) || wpe_input_key_typed(in, WPE_KEY_END)) {
        field->cursor = wpe_input_key_typed(in, WPE_KEY_HOME) ? 0 : length;
        if (!shift) field->anchor = field->cursor;
        moved = true;
    }
    if (ctrl && wpe_input_key_typed(in, WPE_KEY_A)) {
        field->anchor = 0;
        field->cursor = length;
        moved = true;
    }
    const uint32_t lo = wpe__selection_lo(field), hi = wpe__selection_hi(field);
    if (ctrl &&
        (wpe_input_key_typed(in, WPE_KEY_A + ('c' - 'a')) ||
         wpe_input_key_typed(in, WPE_KEY_A + ('x' - 'a'))) &&
        lo != hi && !field->password) {
        char copy[WPE__FIELD_LIMIT + 1];
        memcpy(copy, wpe__field_value(field) + lo, hi - lo);
        copy[hi - lo] = '\0';
        (void)wpe_clipboard_set_text(copy); /* Logged when there is no clipboard. */
        if (wpe_input_key_typed(in, WPE_KEY_A + ('x' - 'a'))) changed |= wpe__replace(field, lo, hi, "", 0);
    }
    if (ctrl && wpe_input_key_typed(in, WPE_KEY_A + ('v' - 'a'))) {
        char pasted[WPE__FIELD_LIMIT + 1];
        if (wpe_clipboard_text(pasted, sizeof pasted) > 0) changed |= wpe__type(field, pasted);
    }
    field->blink = changed || moved ? 0.0f : field->blink + dt;
    if (changed) wpe__changed(ui, entity);
    wpe__scroll_to_cursor(ui, entity, field);

    /* Leaving the field. */
    if (wpe_input_key_typed(in, WPE_KEY_RETURN) || wpe_input_key_typed(in, WPE_KEY_KP_ENTER)) {
        wpe__stop_editing(ui);
        wpe__submitted(ui, entity);
    } else if (wpe_input_key_typed(in, WPE_KEY_ESCAPE)) {
        if (field->wpe__before && strcmp(field->wpe__before, wpe__field_value(field)) != 0) {
            wpe_free(field->value);
            field->value = field->wpe__before;
            field->wpe__before = NULL;
            field->cursor = field->anchor = (uint32_t)strlen(field->value);
            wpe__changed(ui, entity);
        }
        wpe__stop_editing(ui);
    } else if (wpe_input_key_typed(in, WPE_KEY_TAB)) {
        wpe__stop_editing(ui);
        const WPE_Entity next = wpe__tab_order(ui, entity, shift ? -1 : 1);
        if (next != WPE_ENTITY_NULL) ui->focus = next;
    } else if (wpe_input_key_typed(in, WPE_KEY_UP) || wpe_input_key_typed(in, WPE_KEY_DOWN)) {
        wpe__stop_editing(ui);
        const WPE_Entity next =
            wpe__neighbor(ui, entity, wpe_input_key_typed(in, WPE_KEY_UP) ? WPE__DIR_UP : WPE__DIR_DOWN);
        if (next != WPE_ENTITY_NULL) ui->focus = next;
        ui->held = wpe_input_key_down(in, WPE_KEY_UP) ? WPE__DIR_UP : WPE__DIR_DOWN; /* No second step. */
        ui->repeat_timer = WPE__NAV_DELAY;
    }
}

const WPE_Entity* wpe_ui_submits(const WPE_Ui* ui, uint32_t* out_count) {
    if (out_count) *out_count = ui ? ui->submit_count : 0;
    return ui ? ui->submits : NULL;
}

WPE_Entity wpe_ui_editing(const WPE_Ui* ui) {
    return ui ? ui->editing : WPE_ENTITY_NULL;
}

bool wpe_ui_edit(WPE_Ui* ui, WPE_Entity field) {
    if (!ui) return false;
    if (field == WPE_ENTITY_NULL) {
        wpe__stop_editing(ui);
        return true;
    }
    if (!wpe__field(ui, field) || !wpe__focusable(ui, field)) return false;
    wpe__start_editing(ui, field);
    return true;
}

WPE_Result wpe_ui_set_input_text(WPE_Ecs* ecs, WPE_Entity entity, const char* text) {
    if (!ecs || !text) return WPE_RESULT_ERROR_NULL_ARG;
    WPE_TextInput* field = wpe_ecs_get_component(ecs, entity, WPE_COMP_TEXT_INPUT);
    if (!field) return WPE_RESULT_ERROR_NOT_FOUND;
    size_t length = strlen(text);
    if (length > wpe__field_max(field)) length = wpe__field_max(field);
    char* copy = wpe_alloc(length + 1);
    if (!copy) {
        wpe_log_error("out of memory setting the text field of entity 0x%08x", entity);
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }
    memcpy(copy, text, length);
    copy[length] = '\0';
    wpe_free(field->value);
    field->value = copy;
    field->cursor = field->anchor = (uint32_t)length;
    field->scroll = 0.0f;
    return WPE_RESULT_OK;
}

const char* wpe_ui_input_text(const WPE_Ecs* ecs, WPE_Entity entity) {
    const WPE_TextInput* field = ecs ? wpe_ecs_get_component(ecs, entity, WPE_COMP_TEXT_INPUT) : NULL;
    return field ? wpe__field_value(field) : NULL;
}

/* ---- Text ----------------------------------------------------------------------------------------- */

/* Finds the line starting at `p`: it ends at a newline, the end of the text or, when `max_width` is
 * positive, at the last space that keeps it within that many font pixels (mid-word for a word that
 * fits no line). Returns where the next line starts, or NULL after the last line. */
static const char* wpe__line(const WPE_BitmapFont* font, const char* p, float max_width, wpe__Line* out) {
    out->start = p;
    float width = 0.0f, space_width = 0.0f;
    const char* space = NULL;
    bool any = false;
    for (; *p && *p != '\n'; ++p) {
        const float advance =
            (float)wpe_font_glyph(font, *p)->width + (any ? (float)font->glyph_spacing : 0.0f);
        if (max_width > 0.0f && any && width + advance > max_width) {
            out->end = space ? space : p;
            out->width = space ? space_width : width;
            return space ? space + 1 : p;
        }
        if (*p == ' ') {
            space = p;
            space_width = width;
        }
        width += advance;
        any = true;
    }
    out->end = p;
    out->width = width;
    return *p == '\n' ? p + 1 : NULL;
}

static uint32_t wpe__break_lines(WPE_Ui* ui, const WPE_BitmapFont* font, const char* text, float max_width) {
    uint32_t count = 0;
    for (const char* p = text; p;) {
        if (!wpe__reserve((void**)&ui->lines, &ui->line_capacity, count + 1, sizeof *ui->lines)) {
            wpe_log_error("out of memory: text drawn without its last lines");
            break;
        }
        p = wpe__line(font, p, max_width, &ui->lines[count++]);
    }
    return count;
}

static float wpe__aligned(WPE_UiAlign align, float start, float space, float size) {
    return align == WPE_UI_ALIGN_START    ? start
           : align == WPE_UI_ALIGN_CENTER ? start + (space - size) * 0.5f
                                          : start + space - size;
}

static void wpe__draw_text(WPE_Ui* ui, const WPE_Text* text, WPE_Rect rect, WPE_Color color) {
    const WPE_BitmapFont* font = text->glyphs ? text->glyphs : wpe_asset_font(ui->assets, ui->font);
    if (!font || !text->text || !text->text[0] || !(text->scale > 0.0f)) return;
    const float scale = text->scale;
    const uint32_t count = wpe__break_lines(ui, font, text->text, text->wrap ? rect.width / scale : 0.0f);
    const float line_height = (float)font->line_height * scale;
    /* Whole pixels keep the glyphs crisp. */
    float y = floorf(wpe__aligned(text->align_y, rect.y, rect.height, line_height * (float)count) + 0.5f);
    for (uint32_t i = 0; i < count; ++i, y += line_height) {
        const wpe__Line* line = &ui->lines[i];
        float x = floorf(wpe__aligned(text->align_x, rect.x, rect.width, line->width * scale) + 0.5f);
        for (const char* p = line->start; p < line->end; ++p) {
            const WPE_FontGlyph* g = wpe_font_glyph(font, *p);
            const WPE_Rect source = {(float)g->x, (float)g->y, (float)g->width, (float)g->height};
            wpe_draw2d_image(ui->draw2d, font->atlas, source,
                             (WPE_Rect){x, y, (float)g->width * scale, (float)g->height * scale}, color);
            x += (float)(g->width + font->glyph_spacing) * scale;
        }
    }
}

/* ---- Drawing -------------------------------------------------------------------------------------- */

static WPE_Vec4 wpe__mul(WPE_Vec4 a, WPE_Vec4 b) {
    return wpe_vec4(a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w);
}

static WPE_Color wpe__color(WPE_Vec4 c) {
    for (int i = 0; i < 4; ++i)
        c.v[i] = c.v[i] < 0.0f ? 0.0f : c.v[i] > 1.0f ? 1.0f : c.v[i];
    return wpe_color_from_vec4(c);
}

/* The product of the state tints of the element's button and of its ancestors' buttons. */
static WPE_Vec4 wpe__tint(const WPE_Ui* ui, WPE_Entity entity) {
    WPE_Vec4 tint = wpe_vec4(1, 1, 1, 1);
    for (WPE_Entity e = entity; e != WPE_ENTITY_NULL; e = wpe__ui_parent(ui, e)) {
        const WPE_Button* button = wpe_ecs_get_component(ui->ecs, e, WPE_COMP_BUTTON);
        if (!button) continue;
        const WPE_Vec4 state = !button->interactable ? button->disabled_tint
                               : e == ui->pressed    ? button->pressed_tint
                               : e == ui->focus      ? button->focused_tint
                                                     : button->normal_tint;
        tint = wpe__mul(tint, state);
    }
    return tint;
}

/* An image region stretched over `dest`, or nine-sliced: corners keep their size, edges stretch along
 * one axis and the middle along both (borders shrink evenly when `dest` is smaller than them). */
static void wpe__draw_image(WPE_Draw2D* draw2d, const WPE_Texture* texture, WPE_Rect source, WPE_Vec4 slice,
                            WPE_Rect dest, WPE_Color color) {
    const float l = slice.x, t = slice.y, r = slice.z, b = slice.w;
    if (!(l > 0.0f || t > 0.0f || r > 0.0f || b > 0.0f)) {
        wpe_draw2d_image(draw2d, texture, source, dest, color);
        return;
    }
    const float fx = l + r > dest.width ? dest.width / (l + r) : 1.0f;
    const float fy = t + b > dest.height ? dest.height / (t + b) : 1.0f;
    const float sx[4] = {source.x, source.x + l, source.x + source.width - r, source.x + source.width};
    const float sy[4] = {source.y, source.y + t, source.y + source.height - b, source.y + source.height};
    const float dx[4] = {dest.x, dest.x + l * fx, dest.x + dest.width - r * fx, dest.x + dest.width};
    const float dy[4] = {dest.y, dest.y + t * fy, dest.y + dest.height - b * fy, dest.y + dest.height};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            const WPE_Rect s = {sx[column], sy[row], sx[column + 1] - sx[column], sy[row + 1] - sy[row]};
            const WPE_Rect d = {dx[column], dy[row], dx[column + 1] - dx[column], dy[row + 1] - dy[row]};
            if (s.width > 0.0f && s.height > 0.0f) wpe_draw2d_image(draw2d, texture, s, d, color);
        }
    }
}

static void wpe__draw_sprite(WPE_Ui* ui, const WPE_Sprite* sprite, WPE_Rect rect, WPE_Color color) {
    if (!sprite->texture) {
        wpe_draw2d_rect(ui->draw2d, rect, color);
        return;
    }
    WPE_Rect source = sprite->source;
    if (!(source.width > 0.0f) || !(source.height > 0.0f)) {
        source = (WPE_Rect){0.0f, 0.0f, (float)sprite->texture->levels[0].width,
                            (float)sprite->texture->levels[0].height};
    }
    wpe__draw_image(ui->draw2d, sprite->texture, source, sprite->slice, rect, color);
}

static void wpe__fill(WPE_Ui* ui, WPE_Rect rect, WPE_Vec4 color, WPE_Vec4 tint) {
    if (rect.width > 0.0f && rect.height > 0.0f && color.w > 0.0f)
        wpe_draw2d_rect(ui->draw2d, rect, wpe__color(wpe__mul(color, tint)));
}

static void wpe__draw_mark(WPE_Ui* ui, const WPE_Toggle* toggle, WPE_Rect rect, WPE_Vec4 tint) {
    const float inset = toggle->mark_inset > 0.0f ? toggle->mark_inset : 0.0f;
    wpe__fill(
        ui, (WPE_Rect){rect.x + inset, rect.y + inset, rect.width - 2.0f * inset, rect.height - 2.0f * inset},
        toggle->mark_color, tint);
}

/* The track across the middle third (at least 2 pixels), filled up to the value, and the handle
 * centered on it. Whole pixels keep the edges crisp. */
static void wpe__draw_slider(WPE_Ui* ui, const WPE_Slider* slider, WPE_Rect rect, WPE_Vec4 tint) {
    const float range = slider->max - slider->min;
    const float t = range != 0.0f ? (slider->value - slider->min) / range : 0.0f;
    const float fraction = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    const float handle = slider->handle_width > 0.0f ? slider->handle_width : 0.0f;
    const float center = floorf(rect.x + handle * 0.5f + (rect.width - handle) * fraction + 0.5f);
    const float thickness = floorf(rect.height / 3.0f) > 2.0f ? floorf(rect.height / 3.0f) : 2.0f;
    const float track_y = floorf(rect.y + (rect.height - thickness) * 0.5f + 0.5f);
    wpe__fill(ui, (WPE_Rect){rect.x, track_y, rect.width, thickness}, slider->track_color, tint);
    wpe__fill(ui, (WPE_Rect){rect.x, track_y, center - rect.x, thickness}, slider->fill_color, tint);
    if (handle > 0.0f)
        wpe__fill(ui, (WPE_Rect){floorf(center - handle * 0.5f + 0.5f), rect.y, handle, rect.height},
                  slider->handle_color, tint);
}

/* A text field: one line, vertically centered, scrolled and clipped to its rectangle (glyphs that don't fit
 * whole are left out), with the selection behind it and the blinking cursor while it edits. */
static void wpe__draw_field(WPE_Ui* ui, WPE_Entity entity, const WPE_TextInput* field, WPE_Rect rect,
                            WPE_Vec4 tint) {
    float scale;
    const WPE_BitmapFont* font = wpe__field_font(ui, entity, &scale);
    if (!font) return;
    const WPE_Text* text = wpe_ecs_get_component(ui->ecs, entity, WPE_COMP_TEXT);
    const char* value = wpe__field_value(field);
    const bool empty = value[0] == '\0';
    const bool placeholder = empty && !field->editing && field->placeholder && field->placeholder[0];
    const float line = (float)font->line_height * scale;
    const float left = rect.x + field->padding, right = rect.x + rect.width - field->padding;
    const float y = floorf(rect.y + (rect.height - line) * 0.5f + 0.5f);
    const float origin = floorf(left - (field->editing ? field->scroll : 0.0f) + 0.5f);
    if (field->editing && field->cursor != field->anchor) {
        const uint32_t lo = wpe__selection_lo(field), hi = wpe__selection_hi(field);
        float x0 = origin + wpe__caret_x(font, scale, field, lo),
              x1 = origin + wpe__caret_x(font, scale, field, hi);
        x0 = x0 < left ? left : x0;
        x1 = x1 > right ? right : x1;
        if (x1 > x0) wpe__fill(ui, (WPE_Rect){x0, y, x1 - x0, line}, field->selection_color, tint);
    }
    const WPE_Vec4 base = placeholder ? field->placeholder_color : text ? text->color : wpe_vec4(1, 1, 1, 1);
    const WPE_Color color = wpe__color(wpe__mul(base, tint));
    const char* shown = placeholder ? field->placeholder : value;
    float x = origin;
    for (uint32_t i = 0; shown[i]; ++i) {
        const char c = placeholder ? shown[i] : wpe__shown_char(field, value, i);
        const WPE_FontGlyph* g = wpe_font_glyph(font, c);
        const float w = (float)g->width * scale;
        if (x >= left - 0.01f && x + w <= right + 0.01f) {
            const WPE_Rect source = {(float)g->x, (float)g->y, (float)g->width, (float)g->height};
            wpe_draw2d_image(ui->draw2d, font->atlas, source, (WPE_Rect){x, y, w, (float)g->height * scale},
                             color);
        }
        x += (float)(g->width + font->glyph_spacing) * scale;
        if (x > right) break;
    }
    if (field->editing && fmodf(field->blink, WPE__BLINK) < WPE__BLINK * 0.5f) {
        const float caret =
            floorf(origin + wpe__caret_x(font, scale, field, field->cursor) - scale * 0.5f + 0.5f);
        if (caret >= left - scale && caret <= right)
            wpe__fill(ui, (WPE_Rect){caret, y, scale > 1.0f ? scale : 1.0f, line}, field->cursor_color, tint);
    }
}

WPE_Result wpe_ui_draw(WPE_Ui* ui, WPE_Renderer* renderer) {
    if (!ui || !renderer) return WPE_RESULT_ERROR_NULL_ARG;
    const WPE_Framebuffer* fb = wpe_renderer_framebuffer(renderer);
    WPE_Result result = wpe_ui_layout(ui, fb->width, fb->height);
    if (result != WPE_RESULT_OK) return result;
    wpe_draw2d_begin(ui->draw2d, fb->width, fb->height);
    for (uint32_t i = 0; i < ui->order_count; ++i) {
        const WPE_Entity e = ui->order[i];
        const WPE_UiRect* element = wpe__element(ui, e);
        if (!element->shown) continue;
        const WPE_Sprite* sprite = wpe_ecs_get_component(ui->ecs, e, WPE_COMP_SPRITE);
        const WPE_Text* text = wpe_ecs_get_component(ui->ecs, e, WPE_COMP_TEXT);
        const WPE_Toggle* toggle = wpe_ecs_get_component(ui->ecs, e, WPE_COMP_TOGGLE);
        const WPE_Slider* slider = wpe_ecs_get_component(ui->ecs, e, WPE_COMP_SLIDER);
        const WPE_TextInput* field = wpe__field(ui, e);
        if (!sprite && !text && !toggle && !slider && !field) continue;
        const WPE_Vec4 tint = wpe__tint(ui, e);
        if (sprite) wpe__draw_sprite(ui, sprite, element->rect, wpe__color(wpe__mul(sprite->color, tint)));
        if (toggle && toggle->on) wpe__draw_mark(ui, toggle, element->rect, tint);
        if (slider) wpe__draw_slider(ui, slider, element->rect, tint);
        if (field) {
            wpe__draw_field(ui, e, field, element->rect, tint); /* Its text component styles it. */
        } else if (text) {
            wpe__draw_text(ui, text, element->rect, wpe__color(wpe__mul(text->color, tint)));
        }
    }
    return wpe_draw2d_submit(ui->draw2d, renderer);
}

/* ---- Text editing --------------------------------------------------------------------------------- */

WPE_Result wpe_ui_set_text(WPE_Ecs* ecs, WPE_Entity entity, const char* string) {
    if (!ecs || !string) return WPE_RESULT_ERROR_NULL_ARG;
    WPE_Text* text = wpe_ecs_get_component(ecs, entity, WPE_COMP_TEXT);
    if (!text) return WPE_RESULT_ERROR_NOT_FOUND;
    char* copy = wpe__copy_string(string);
    if (!copy) {
        wpe_log_error("out of memory setting the text of entity 0x%08x", entity);
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }
    wpe_free(text->text);
    text->text = copy;
    return WPE_RESULT_OK;
}
