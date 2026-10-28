#define WPE_LOG_MODULE "ui"
#include "ui/ui.h"

#include <stdlib.h>
#include <string.h>

#include "core/alloc.h"
#include "core/debug.h"
#include "core/log.h"
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
    wpe__Line* lines; /* Scratch for text. */
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

WPE_Entity wpe_ui_pick(const WPE_Ui* ui, float x, float y) {
    if (!ui) return WPE_ENTITY_NULL;
    for (uint32_t i = ui->order_count; i-- > 0;) {
        const WPE_Entity e = ui->order[i];
        const WPE_UiRect* element = wpe__element(ui, e);
        if (!element || !element->shown || !wpe__contains(element->rect, x, y)) continue;
        if (wpe_ecs_has_component(ui->ecs, e, WPE_COMP_SPRITE) ||
            wpe_ecs_has_component(ui->ecs, e, WPE_COMP_BUTTON))
            return e;
    }
    return WPE_ENTITY_NULL;
}

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

static void wpe__click(WPE_Ui* ui, WPE_Entity button) {
    if (!wpe__reserve((void**)&ui->clicks, &ui->click_capacity, ui->click_count + 1, sizeof *ui->clicks)) {
        wpe_log_error("out of memory: a click on 0x%08x was dropped", button);
        return;
    }
    ui->clicks[ui->click_count++] = button;
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
    WPE_Entity hovered = wpe__owning_button(ui, wpe_ui_pick(ui, x, y));
    if (hovered != WPE_ENTITY_NULL && !wpe__focusable(ui, hovered)) hovered = WPE_ENTITY_NULL;
    const bool pressed = wpe_input_mouse_pressed(ui->input, WPE_MOUSE_LEFT);
    if (hovered != WPE_ENTITY_NULL && (dx != 0.0f || dy != 0.0f || pressed)) ui->focus = hovered;
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
    const WPE_Result result = wpe_ui_layout(ui, width, height);
    if (result != WPE_RESULT_OK) return result;
    /* Buttons that were removed, hidden or disabled let go of the focus and of presses. */
    if (ui->focus != WPE_ENTITY_NULL && !wpe__focusable(ui, ui->focus)) ui->focus = WPE_ENTITY_NULL;
    if (ui->mouse_target != WPE_ENTITY_NULL && !wpe__focusable(ui, ui->mouse_target))
        ui->mouse_target = WPE_ENTITY_NULL;
    if (ui->submit_target != WPE_ENTITY_NULL && !wpe__focusable(ui, ui->submit_target))
        ui->submit_target = WPE_ENTITY_NULL;
    ui->pressed = WPE_ENTITY_NULL;
    if (!ui->input) return WPE_RESULT_OK;
    const WPE_Entity hovered = wpe__mouse_input(ui);
    wpe__navigate(ui, dt);
    wpe__submit_input(ui);
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
        if (!sprite && !text) continue;
        const WPE_Vec4 tint = wpe__tint(ui, e);
        if (sprite) wpe__draw_sprite(ui, sprite, element->rect, wpe__color(wpe__mul(sprite->color, tint)));
        if (text) wpe__draw_text(ui, text, element->rect, wpe__color(wpe__mul(text->color, tint)));
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
