/*
 * UI tests: layout, picking, drawing (exact pixels), and focus navigation driven by real SDL input
 * events pushed into SDL's queue.
 */
#include <SDL3/SDL.h>

#include "wpe_test.h"
#include "wpe_test_assets.h"

#include "scene/scene.h"
#include "ui/ui.h"

#define SCREEN_W 200u
#define SCREEN_H 100u

typedef struct Fixture {
    WPE_TestAssets t;
    WPE_Ecs* ecs;
    WPE_Input* input;
    WPE_AssetHandle font;
    WPE_Ui* ui;
} Fixture;

static Fixture make_fixture(bool with_input) {
    Fixture f = {.t = wpe_test_assets_create(WPE_TEST_ASSETS_DIR, 0)};
    WPE_TEST_ASSERT_OK(wpe_ecs_create(&f.ecs));
    WPE_TEST_ASSERT_OK(wpe_scene_register_components(f.ecs));
    WPE_TEST_ASSERT_OK(wpe_asset_load(f.t.assets, WPE_ASSET_FONT, "fonts/small.png", &f.font));
    if (with_input) {
        WPE_TEST_ASSERT_OK(wpe_input_create(NULL, &f.input)); /* Initializes SDL's event queue. */
        wpe_platform_poll_events(NULL, NULL);                 /* Drop events left over by earlier cases. */
    }
    WPE_TEST_ASSERT_OK(wpe_ui_create(
        &(WPE_UiDesc){.ecs = f.ecs, .assets = f.t.assets, .input = f.input, .font = f.font}, &f.ui));
    return f;
}

static void destroy_fixture(Fixture* f) {
    wpe_ecs_destroy(f->ecs); /* Sprites and texts release their assets. */
    wpe_ui_destroy(f->ui);
    wpe_input_destroy(f->input);
    wpe_asset_release(f->t.assets, f->font);
    wpe_test_assets_destroy(&f->t);
}

/* A fixed-size element at (x, y) of its parent's top-left corner. */
static WPE_UiRect fixed(float x, float y, float w, float h) {
    WPE_UiRect r = wpe_ui_rect_default();
    r.anchor_max = (WPE_Vec2){.x = 0, .y = 0};
    r.offset_min = (WPE_Vec2){.x = x, .y = y};
    r.offset_max = (WPE_Vec2){.x = x + w, .y = y + h};
    return r;
}

static WPE_Entity add_element(Fixture* f, WPE_Entity parent, WPE_UiRect rect) {
    const WPE_Entity e = wpe_ecs_spawn(f->ecs);
    WPE_TEST_ASSERT(e != WPE_ENTITY_NULL);
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(f->ecs, e, WPE_COMP_UI, &rect, NULL));
    if (parent != WPE_ENTITY_NULL) WPE_TEST_ASSERT_OK(wpe_scene_set_parent(f->ecs, e, parent, false));
    return e;
}

static void add_sprite(Fixture* f, WPE_Entity e, WPE_Vec4 color) {
    WPE_Sprite sprite = wpe_sprite_default();
    sprite.color = color;
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(f->ecs, e, WPE_COMP_SPRITE, &sprite, NULL));
}

static WPE_Entity add_button(Fixture* f, float x, float y) {
    const WPE_Entity e = add_element(f, WPE_ENTITY_NULL, fixed(x, y, 40, 20));
    add_sprite(f, e, wpe_vec4(0.5f, 0.5f, 0.5f, 1));
    const WPE_Button button = wpe_button_default();
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(f->ecs, e, WPE_COMP_BUTTON, &button, NULL));
    return e;
}

static WPE_Rect rect_of(const Fixture* f, WPE_Entity e) {
    return ((const WPE_UiRect*)wpe_ecs_get_component(f->ecs, e, WPE_COMP_UI))->rect;
}

static bool shown(const Fixture* f, WPE_Entity e) {
    return ((const WPE_UiRect*)wpe_ecs_get_component(f->ecs, e, WPE_COMP_UI))->shown;
}

static void assert_rect(WPE_Rect expected, WPE_Rect actual) {
    WPE_TEST_ASSERT_NEAR(expected.x, actual.x, 1e-4);
    WPE_TEST_ASSERT_NEAR(expected.y, actual.y, 1e-4);
    WPE_TEST_ASSERT_NEAR(expected.width, actual.width, 1e-4);
    WPE_TEST_ASSERT_NEAR(expected.height, actual.height, 1e-4);
}

/* ---- Layout and picking ---------------------------------------------------------------------------- */

static void test_ui_layout_anchors_and_visibility(void) {
    Fixture f = make_fixture(false);
    const WPE_Entity panel = add_element(&f, WPE_ENTITY_NULL, fixed(10, 10, 100, 50));
    /* The default fills the parent. */
    const WPE_Entity fill = add_element(&f, panel, wpe_ui_rect_default());
    /* Stretched with a 5-pixel margin. */
    WPE_UiRect stretch = wpe_ui_rect_default();
    stretch.offset_min = (WPE_Vec2){.x = 5, .y = 5};
    stretch.offset_max = (WPE_Vec2){.x = -5, .y = -5};
    const WPE_Entity inner = add_element(&f, panel, stretch);
    /* Centered, fixed size. */
    WPE_UiRect centered = wpe_ui_rect_default();
    centered.anchor_min = centered.anchor_max = (WPE_Vec2){.x = 0.5f, .y = 0.5f};
    centered.offset_min = (WPE_Vec2){.x = -10, .y = -5};
    centered.offset_max = (WPE_Vec2){.x = 10, .y = 5};
    const WPE_Entity label = add_element(&f, panel, centered);
    /* Bottom-right corner of the screen. */
    WPE_UiRect corner = wpe_ui_rect_default();
    corner.anchor_min = corner.anchor_max = (WPE_Vec2){.x = 1, .y = 1};
    corner.offset_min = (WPE_Vec2){.x = -30, .y = -20};
    corner.offset_max = (WPE_Vec2){.x = 0, .y = 0};
    const WPE_Entity badge = add_element(&f, WPE_ENTITY_NULL, corner);
    /* Offsets that cross collapse to an empty rectangle. */
    const WPE_Entity empty = add_element(&f, WPE_ENTITY_NULL, fixed(50, 50, -10, 5));

    WPE_TEST_ASSERT_OK(wpe_ui_layout(f.ui, SCREEN_W, SCREEN_H));
    assert_rect((WPE_Rect){10, 10, 100, 50}, rect_of(&f, panel));
    assert_rect((WPE_Rect){10, 10, 100, 50}, rect_of(&f, fill));
    assert_rect((WPE_Rect){15, 15, 90, 40}, rect_of(&f, inner));
    assert_rect((WPE_Rect){50, 30, 20, 10}, rect_of(&f, label));
    assert_rect((WPE_Rect){170, 80, 30, 20}, rect_of(&f, badge));
    WPE_TEST_ASSERT_NEAR(0.0, rect_of(&f, empty).width, 0.0);

    /* A larger screen moves what is anchored to its far side; a hidden panel hides its children. */
    ((WPE_UiRect*)wpe_ecs_get_component(f.ecs, panel, WPE_COMP_UI))->visible = false;
    WPE_TEST_ASSERT_OK(wpe_ui_layout(f.ui, 400, 300));
    assert_rect((WPE_Rect){370, 280, 30, 20}, rect_of(&f, badge));
    WPE_TEST_ASSERT(!shown(&f, panel) && !shown(&f, inner) && !shown(&f, label));
    WPE_TEST_ASSERT(shown(&f, badge));

    /* An element under a non-UI parent is placed on the screen. */
    const WPE_Entity holder = wpe_ecs_spawn(f.ecs);
    const WPE_Entity loose = add_element(&f, holder, fixed(1, 2, 3, 4));
    WPE_TEST_ASSERT_OK(wpe_ui_layout(f.ui, SCREEN_W, SCREEN_H));
    assert_rect((WPE_Rect){1, 2, 3, 4}, rect_of(&f, loose));
    destroy_fixture(&f);
}

static void test_ui_pick_follows_drawing_order(void) {
    Fixture f = make_fixture(false);
    const WPE_Entity back = add_element(&f, WPE_ENTITY_NULL, fixed(0, 0, 100, 100));
    add_sprite(&f, back, wpe_vec4(1, 1, 1, 1));
    const WPE_Entity child = add_element(&f, back, fixed(10, 10, 20, 20));
    add_sprite(&f, child, wpe_vec4(1, 0, 0, 1));
    const WPE_Entity front = add_element(&f, WPE_ENTITY_NULL, fixed(50, 50, 40, 40));
    add_sprite(&f, front, wpe_vec4(0, 1, 0, 1));
    /* Text alone does not catch the mouse. */
    const WPE_Entity caption = add_element(&f, WPE_ENTITY_NULL, fixed(0, 0, 30, 30));
    const WPE_Text text = wpe_text_default();
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(f.ecs, caption, WPE_COMP_TEXT, &text, NULL));
    WPE_TEST_ASSERT_OK(wpe_ui_layout(f.ui, SCREEN_W, SCREEN_H));

    WPE_TEST_ASSERT_EQ_UINT(child, wpe_ui_pick(f.ui, 15, 15)); /* Children over their parent. */
    WPE_TEST_ASSERT_EQ_UINT(back, wpe_ui_pick(f.ui, 5, 40));
    WPE_TEST_ASSERT_EQ_UINT(front, wpe_ui_pick(f.ui, 60, 60)); /* Later top-level elements on top. */
    WPE_TEST_ASSERT_EQ_UINT(WPE_ENTITY_NULL, wpe_ui_pick(f.ui, 150, 50));
    WPE_TEST_ASSERT_EQ_UINT(WPE_ENTITY_NULL, wpe_ui_pick(f.ui, 100, 10)); /* Right edge is outside. */
    ((WPE_UiRect*)wpe_ecs_get_component(f.ecs, front, WPE_COMP_UI))->visible = false;
    WPE_TEST_ASSERT_OK(wpe_ui_layout(f.ui, SCREEN_W, SCREEN_H));
    WPE_TEST_ASSERT_EQ_UINT(back, wpe_ui_pick(f.ui, 60, 60));
    destroy_fixture(&f);
}

/* ---- Drawing --------------------------------------------------------------------------------------- */

static uint32_t count_color(const WPE_Framebuffer* fb, WPE_Color color) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < fb->width * fb->height; ++i)
        count += fb->color[i] == color;
    return count;
}

static const WPE_Framebuffer* draw(Fixture* f, WPE_Renderer* renderer) {
    WPE_TEST_ASSERT_OK(
        wpe_renderer_begin_frame(renderer, &(WPE_ClearDesc){.clear_color = true, .color = 0xFF000000u}));
    WPE_TEST_ASSERT_OK(wpe_ui_draw(f->ui, renderer));
    WPE_TEST_ASSERT_OK(wpe_renderer_end_frame(renderer));
    return wpe_renderer_framebuffer(renderer);
}

static void test_ui_draws_sprites_with_button_tints(void) {
    Fixture f = make_fixture(false);
    WPE_Renderer* renderer = NULL;
    WPE_TEST_ASSERT_OK(wpe_renderer_create(&(WPE_RendererDesc){.width = 64, .height = 48}, &renderer));
    const WPE_Entity box = add_element(&f, WPE_ENTITY_NULL, fixed(4, 4, 10, 8));
    add_sprite(&f, box, wpe_vec4(1, 0, 0, 1));
    const WPE_Entity button = add_element(&f, WPE_ENTITY_NULL, fixed(20, 20, 10, 10));
    add_sprite(&f, button, wpe_vec4(0.4f, 0.4f, 0.4f, 1));
    WPE_Button settings = wpe_button_default();
    settings.focused_tint = wpe_vec4(2, 2, 2, 1);
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(f.ecs, button, WPE_COMP_BUTTON, &settings, NULL));
    /* A child of the button is tinted with it. */
    const WPE_Entity icon = add_element(&f, button, fixed(2, 2, 2, 2));
    add_sprite(&f, icon, wpe_vec4(0, 0, 0.5f, 1));

    const WPE_Framebuffer* fb = draw(&f, renderer);
    WPE_TEST_ASSERT_EQ_UINT(80, count_color(fb, 0xFFFF0000u));
    const WPE_Color gray = wpe_color_from_vec4(wpe_vec4(0.4f, 0.4f, 0.4f, 1));
    WPE_TEST_ASSERT_EQ_UINT(96, count_color(fb, gray));
    WPE_TEST_ASSERT_EQ_UINT(4, count_color(fb, wpe_color_from_vec4(wpe_vec4(0, 0, 0.5f, 1))));

    WPE_TEST_ASSERT(wpe_ui_focus(f.ui, button));
    WPE_TEST_ASSERT_EQ_UINT(button, wpe_ui_focused(f.ui));
    fb = draw(&f, renderer);
    WPE_TEST_ASSERT_EQ_UINT(0, count_color(fb, gray));
    WPE_TEST_ASSERT_EQ_UINT(96, count_color(fb, wpe_color_from_vec4(wpe_vec4(0.8f, 0.8f, 0.8f, 1))));
    WPE_TEST_ASSERT_EQ_UINT(4, count_color(fb, wpe_color_from_vec4(wpe_vec4(0, 0, 1, 1))));

    /* Disabled buttons can't keep or take the focus. */
    ((WPE_Button*)wpe_ecs_get_component(f.ecs, button, WPE_COMP_BUTTON))->interactable = false;
    WPE_TEST_ASSERT(!wpe_ui_focus(f.ui, button));
    WPE_TEST_ASSERT_OK(wpe_ui_update(f.ui, SCREEN_W, SCREEN_H, 0.016f));
    WPE_TEST_ASSERT_EQ_UINT(WPE_ENTITY_NULL, wpe_ui_focused(f.ui));
    WPE_TEST_ASSERT(!wpe_ui_focus(f.ui, box)); /* Not a button. */
    wpe_renderer_destroy(renderer);
    destroy_fixture(&f);
}

static void test_ui_nine_slice_keeps_borders(void) {
    Fixture f = make_fixture(false);
    WPE_Renderer* renderer = NULL;
    WPE_TEST_ASSERT_OK(wpe_renderer_create(&(WPE_RendererDesc){.width = 64, .height = 48}, &renderer));
    /* 4x4: a one-texel red frame around green. */
    uint32_t texels[16];
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x)
            texels[y * 4 + x] = (x == 0 || y == 0 || x == 3 || y == 3) ? 0xFFFF0000u : 0xFF00FF00u;
    }
    WPE_Texture* texture = NULL;
    WPE_TEST_ASSERT_OK(
        wpe_texture_create(&(WPE_TextureDesc){.width = 4, .height = 4, .texels = texels}, &texture));
    WPE_AssetHandle image = WPE_ASSET_NONE;
    WPE_TEST_ASSERT_OK(wpe_asset_add(f.t.assets, WPE_ASSET_IMAGE, "test/frame", texture, &image));

    const WPE_Entity panel = add_element(&f, WPE_ENTITY_NULL, fixed(2, 2, 20, 10));
    WPE_Sprite sprite = wpe_sprite_default();
    sprite.image = image;
    sprite.slice = wpe_vec4(1, 1, 1, 1);
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(f.ecs, panel, WPE_COMP_SPRITE, &sprite, NULL));
    const WPE_Framebuffer* fb = draw(&f, renderer);
    WPE_TEST_ASSERT_EQ_UINT(18 * 8, count_color(fb, 0xFF00FF00u));
    WPE_TEST_ASSERT_EQ_UINT(20 * 10 - 18 * 8, count_color(fb, 0xFFFF0000u));
    WPE_TEST_ASSERT_EQ_UINT(0xFFFF0000u, fb->color[2 * 64 + 2]);
    WPE_TEST_ASSERT_EQ_UINT(0xFF00FF00u, fb->color[3 * 64 + 3]);

    /* Without slicing the image stretches: 5x2.5 pixels per texel. */
    ((WPE_Sprite*)wpe_ecs_get_component(f.ecs, panel, WPE_COMP_SPRITE))->slice = wpe_vec4(0, 0, 0, 0);
    fb = draw(&f, renderer);
    WPE_TEST_ASSERT_EQ_UINT(10 * 5, count_color(fb, 0xFF00FF00u));

    /* The sprite holds its own reference to the image. */
    wpe_asset_release(f.t.assets, image);
    WPE_TEST_ASSERT(wpe_asset_is_valid(f.t.assets, image));
    WPE_TEST_ASSERT_OK(wpe_ecs_remove_component(f.ecs, panel, WPE_COMP_SPRITE));
    WPE_TEST_ASSERT(!wpe_asset_is_valid(f.t.assets, image));
    wpe_renderer_destroy(renderer);
    destroy_fixture(&f);
}

/* Draws the UI, then the same text with plain 2D drawing at the given line positions, and checks that
 * the two frames match pixel for pixel. */
static void assert_text_at(Fixture* f, WPE_Renderer* renderer, const WPE_BitmapFont* font,
                           const char* const* lines, const float* xs, const float* ys, int count,
                           float scale) {
    static WPE_Color expected[128 * 64];
    const WPE_Framebuffer* fb = draw(f, renderer);
    WPE_TEST_ASSERT(fb->width * fb->height <= 128 * 64);
    memcpy(expected, fb->color, (size_t)fb->width * fb->height * sizeof *expected);
    WPE_Draw2D* draw2d = NULL;
    WPE_TEST_ASSERT_OK(wpe_draw2d_create(&draw2d));
    WPE_TEST_ASSERT_OK(
        wpe_renderer_begin_frame(renderer, &(WPE_ClearDesc){.clear_color = true, .color = 0xFF000000u}));
    wpe_draw2d_begin(draw2d, fb->width, fb->height);
    for (int i = 0; i < count; ++i)
        wpe_draw2d_text(draw2d, font, xs[i], ys[i], lines[i], 0xFFFFFFFFu, scale);
    WPE_TEST_ASSERT_OK(wpe_draw2d_submit(draw2d, renderer));
    WPE_TEST_ASSERT_OK(wpe_renderer_end_frame(renderer));
    uint32_t ink = 0, different = 0;
    for (uint32_t i = 0; i < fb->width * fb->height; ++i) {
        ink += fb->color[i] != 0xFF000000u;
        different += fb->color[i] != expected[i];
    }
    WPE_TEST_ASSERT(ink > 0);
    WPE_TEST_ASSERT_EQ_UINT(0, different);
    wpe_draw2d_destroy(draw2d);
}

static void test_ui_text_alignment_and_wrapping(void) {
    Fixture f = make_fixture(false);
    WPE_Renderer* renderer = NULL;
    WPE_TEST_ASSERT_OK(wpe_renderer_create(&(WPE_RendererDesc){.width = 128, .height = 64}, &renderer));
    const WPE_BitmapFont* font = wpe_asset_font(f.t.assets, f.font);
    uint32_t width = 0, height = 0;
    wpe_font_measure(font, "HI", &width, &height);
    const float w = (float)width, line = (float)font->line_height;

    const WPE_Entity label = add_element(&f, WPE_ENTITY_NULL, fixed(0, 0, 128, 64));
    WPE_Text text = wpe_text_default();
    char buffer[8] = "HI";
    text.text = buffer;
    text.align_x = WPE_UI_ALIGN_END;
    text.align_y = WPE_UI_ALIGN_END;
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(f.ecs, label, WPE_COMP_TEXT, &text, NULL));
    buffer[0] = 'X'; /* The component keeps its own copy. */
    const WPE_Text* stored = wpe_ecs_get_component(f.ecs, label, WPE_COMP_TEXT);
    WPE_TEST_ASSERT_EQ_STR("HI", stored->text);

    const char* const one[] = {"HI"};
    assert_text_at(&f, renderer, font, one, (const float[]){128 - w}, (const float[]){64 - line}, 1, 1.0f);

    /* Centered at scale 2. */
    WPE_Text* live = wpe_ecs_get_component(f.ecs, label, WPE_COMP_TEXT);
    live->align_x = live->align_y = WPE_UI_ALIGN_CENTER;
    live->scale = 2.0f;
    assert_text_at(&f, renderer, font, one, (const float[]){64 - w}, (const float[]){32 - line}, 1, 2.0f);

    /* Wrapping breaks at the space when the line does not fit; centered lines center one by one. */
    WPE_TEST_ASSERT_OK(wpe_ui_set_text(f.ecs, label, "HI HI"));
    live = wpe_ecs_get_component(f.ecs, label, WPE_COMP_TEXT);
    live->align_y = WPE_UI_ALIGN_START;
    live->scale = 1.0f;
    live->wrap = true;
    WPE_UiRect* element = wpe_ecs_get_component(f.ecs, label, WPE_COMP_UI);
    element->offset_max.x = w + 4.0f;
    const char* const two[] = {"HI", "HI"};
    assert_text_at(&f, renderer, font, two, (const float[]){2, 2}, (const float[]){0, line}, 2, 1.0f);
    /* Unwrapped, the line runs past the rectangle; '\n' still breaks. */
    live->wrap = false;
    live->align_x = WPE_UI_ALIGN_START;
    const char* const whole[] = {"HI HI"};
    assert_text_at(&f, renderer, font, whole, (const float[]){0}, (const float[]){0}, 1, 1.0f);
    WPE_TEST_ASSERT_OK(wpe_ui_set_text(f.ecs, label, "HI\nHI"));
    assert_text_at(&f, renderer, font, two, (const float[]){0, 0}, (const float[]){0, line}, 2, 1.0f);

    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND, wpe_ui_set_text(f.ecs, wpe_ecs_spawn(f.ecs), "x"));
    wpe_renderer_destroy(renderer);
    destroy_fixture(&f);
}

/* ---- Interaction ----------------------------------------------------------------------------------- */

static void on_event(const WPE_PlatformEvent* event, void* user_data) {
    wpe_input_handle_event(user_data, event);
}

/* Events are zeroed whole: an initializer list would only set the union's first member, leaving fields such
 * as key.repeat undefined. */
static void push_key(SDL_Scancode key, bool down) {
    SDL_Event e;
    SDL_zero(e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.scancode = key;
    e.key.down = down;
    WPE_TEST_ASSERT(SDL_PushEvent(&e));
}

static void push_mouse_move(float x, float y) {
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.x = x;
    e.motion.y = y;
    e.motion.xrel = 1.0f;
    WPE_TEST_ASSERT(SDL_PushEvent(&e));
}

static void push_mouse_button(bool down, float x, float y) {
    SDL_Event e;
    SDL_zero(e);
    e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.button = SDL_BUTTON_LEFT;
    e.button.down = down;
    e.button.x = x;
    e.button.y = y;
    WPE_TEST_ASSERT(SDL_PushEvent(&e));
}

/* Delivers the queued events and runs one UI frame; returns the number of clicks. */
static uint32_t frame(Fixture* f, float dt) {
    wpe_platform_poll_events(on_event, f->input);
    wpe_input_update(f->input);
    WPE_TEST_ASSERT_OK(wpe_ui_update(f->ui, SCREEN_W, SCREEN_H, dt));
    uint32_t count = 0;
    wpe_ui_clicks(f->ui, &count);
    wpe_input_begin_frame(f->input);
    return count;
}

static void tap(Fixture* f, SDL_Scancode key) {
    push_key(key, true);
    frame(f, 0.016f);
    push_key(key, false);
    frame(f, 0.016f);
}

static void test_ui_keyboard_navigation_and_clicks(void) {
    Fixture f = make_fixture(true);
    const WPE_Entity top = add_button(&f, 10, 10);
    const WPE_Entity middle = add_button(&f, 10, 40);
    const WPE_Entity bottom = add_button(&f, 10, 70);
    const WPE_Entity side = add_button(&f, 100, 42);
    wpe_input_begin_frame(f.input);
    WPE_TEST_ASSERT_EQ_UINT(0, frame(&f, 0.016f));
    WPE_TEST_ASSERT_EQ_UINT(WPE_ENTITY_NULL, wpe_ui_focused(f.ui));

    /* The first direction focuses the first button; later ones move to the nearest in that direction. */
    tap(&f, SDL_SCANCODE_DOWN);
    WPE_TEST_ASSERT_EQ_UINT(top, wpe_ui_focused(f.ui));
    tap(&f, SDL_SCANCODE_DOWN);
    WPE_TEST_ASSERT_EQ_UINT(middle, wpe_ui_focused(f.ui));
    tap(&f, SDL_SCANCODE_RIGHT);
    WPE_TEST_ASSERT_EQ_UINT(side, wpe_ui_focused(f.ui));
    tap(&f, SDL_SCANCODE_RIGHT); /* Nothing further right: the focus stays. */
    WPE_TEST_ASSERT_EQ_UINT(side, wpe_ui_focused(f.ui));
    tap(&f, SDL_SCANCODE_LEFT);
    WPE_TEST_ASSERT_EQ_UINT(middle, wpe_ui_focused(f.ui));

    /* Return clicks on release; a tap within one frame clicks too. */
    push_key(SDL_SCANCODE_RETURN, true);
    WPE_TEST_ASSERT_EQ_UINT(0, frame(&f, 0.016f));
    push_key(SDL_SCANCODE_RETURN, false);
    WPE_TEST_ASSERT_EQ_UINT(1, frame(&f, 0.016f));
    push_key(SDL_SCANCODE_SPACE, true);
    push_key(SDL_SCANCODE_SPACE, false);
    WPE_TEST_ASSERT_EQ_UINT(1, frame(&f, 0.016f));
    uint32_t count = 0;
    WPE_TEST_ASSERT_EQ_UINT(middle, wpe_ui_clicks(f.ui, &count)[0]);

    /* Holding a direction repeats after a delay: down to the bottom, which is the last one. */
    WPE_TEST_ASSERT(wpe_ui_focus(f.ui, top));
    push_key(SDL_SCANCODE_DOWN, true);
    frame(&f, 0.016f);
    WPE_TEST_ASSERT_EQ_UINT(middle, wpe_ui_focused(f.ui));
    frame(&f, 0.3f);
    WPE_TEST_ASSERT_EQ_UINT(middle, wpe_ui_focused(f.ui));
    frame(&f, 0.15f);
    WPE_TEST_ASSERT_EQ_UINT(bottom, wpe_ui_focused(f.ui));
    push_key(SDL_SCANCODE_DOWN, false);
    frame(&f, 0.016f);

    /* Disabled buttons are skipped, and a hidden focused button loses the focus. */
    ((WPE_Button*)wpe_ecs_get_component(f.ecs, middle, WPE_COMP_BUTTON))->interactable = false;
    tap(&f, SDL_SCANCODE_UP);
    WPE_TEST_ASSERT_EQ_UINT(top, wpe_ui_focused(f.ui));
    ((WPE_UiRect*)wpe_ecs_get_component(f.ecs, top, WPE_COMP_UI))->visible = false;
    frame(&f, 0.016f);
    WPE_TEST_ASSERT_EQ_UINT(WPE_ENTITY_NULL, wpe_ui_focused(f.ui));
    destroy_fixture(&f);
}

static void test_ui_mouse_focus_and_clicks(void) {
    Fixture f = make_fixture(true);
    const WPE_Entity first = add_button(&f, 10, 10);
    const WPE_Entity second = add_button(&f, 10, 40);
    /* A label inside the second button belongs to it. */
    const WPE_Entity label = add_element(&f, second, fixed(5, 5, 10, 10));
    add_sprite(&f, label, wpe_vec4(1, 1, 1, 1));
    wpe_input_begin_frame(f.input);
    frame(&f, 0.016f);

    push_mouse_move(20, 20);
    frame(&f, 0.016f);
    WPE_TEST_ASSERT_EQ_UINT(first, wpe_ui_focused(f.ui));
    push_mouse_move(17, 47);
    frame(&f, 0.016f);
    WPE_TEST_ASSERT_EQ_UINT(second, wpe_ui_focused(f.ui));

    push_mouse_button(true, 17, 47);
    WPE_TEST_ASSERT_EQ_UINT(0, frame(&f, 0.016f));
    push_mouse_button(false, 17, 47);
    WPE_TEST_ASSERT_EQ_UINT(1, frame(&f, 0.016f));
    uint32_t count = 0;
    WPE_TEST_ASSERT_EQ_UINT(second, wpe_ui_clicks(f.ui, &count)[0]);

    /* Pressing on one button and releasing over another clicks neither. */
    push_mouse_move(20, 20);
    push_mouse_button(true, 20, 20);
    frame(&f, 0.016f);
    push_mouse_move(20, 50);
    push_mouse_button(false, 20, 50);
    WPE_TEST_ASSERT_EQ_UINT(0, frame(&f, 0.016f));
    /* Nor does releasing away from every button. */
    push_mouse_button(true, 20, 50);
    frame(&f, 0.016f);
    push_mouse_move(150, 90);
    push_mouse_button(false, 150, 90);
    WPE_TEST_ASSERT_EQ_UINT(0, frame(&f, 0.016f));
    destroy_fixture(&f);
}

static void test_ui_navigation_uses_project_actions(void) {
    Fixture f = make_fixture(true);
    WPE_InputActionDesc down = {.name = "ui_down", .binding_count = 1};
    down.bindings[0] = (WPE_InputBinding){.source = WPE_INPUT_SOURCE_KEY, .code = WPE_KEY_A + ('s' - 'a')};
    WPE_TEST_ASSERT_OK(wpe_input_set_actions(f.input, &down, 1));
    const WPE_Entity top = add_button(&f, 10, 10);
    const WPE_Entity below = add_button(&f, 10, 40);
    wpe_input_begin_frame(f.input);
    frame(&f, 0.016f);
    tap(&f, SDL_SCANCODE_S);
    WPE_TEST_ASSERT_EQ_UINT(top, wpe_ui_focused(f.ui));
    tap(&f, SDL_SCANCODE_DOWN); /* The action replaced the arrow key. */
    WPE_TEST_ASSERT_EQ_UINT(top, wpe_ui_focused(f.ui));
    tap(&f, SDL_SCANCODE_S);
    WPE_TEST_ASSERT_EQ_UINT(below, wpe_ui_focused(f.ui));
    tap(&f, SDL_SCANCODE_UP); /* Directions without an action keep their defaults. */
    WPE_TEST_ASSERT_EQ_UINT(top, wpe_ui_focused(f.ui));
    destroy_fixture(&f);
}

static void test_ui_create_validates(void) {
    WPE_Ui* ui = NULL;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NULL_ARG, wpe_ui_create(NULL, &ui));
    Fixture f = make_fixture(false);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_ALREADY_EXISTS,
                           wpe_ui_create(&(WPE_UiDesc){.ecs = f.ecs, .assets = f.t.assets}, &ui));
    WPE_TEST_ASSERT(ui == NULL);
    wpe_ui_destroy(NULL);
    destroy_fixture(&f);
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_ui_layout_anchors_and_visibility),
              WPE_TEST_CASE(test_ui_pick_follows_drawing_order),
              WPE_TEST_CASE(test_ui_draws_sprites_with_button_tints),
              WPE_TEST_CASE(test_ui_nine_slice_keeps_borders),
              WPE_TEST_CASE(test_ui_text_alignment_and_wrapping),
              WPE_TEST_CASE(test_ui_keyboard_navigation_and_clicks),
              WPE_TEST_CASE(test_ui_mouse_focus_and_clicks),
              WPE_TEST_CASE(test_ui_navigation_uses_project_actions), WPE_TEST_CASE(test_ui_create_validates))
