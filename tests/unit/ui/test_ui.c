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

    WPE_TEST_ASSERT_EQ_UINT(child, wpe_ui_pick(f.ui, 15, 15, false)); /* Children over their parent. */
    WPE_TEST_ASSERT_EQ_UINT(back, wpe_ui_pick(f.ui, 5, 40, false));
    WPE_TEST_ASSERT_EQ_UINT(caption,
                            wpe_ui_pick(f.ui, 5, 5, true)); /* Editors pick text-only elements too. */
    WPE_TEST_ASSERT_EQ_UINT(back, wpe_ui_pick(f.ui, 5, 5, false));
    WPE_TEST_ASSERT_EQ_UINT(front, wpe_ui_pick(f.ui, 60, 60, false)); /* Later top-level elements on top. */
    WPE_TEST_ASSERT_EQ_UINT(WPE_ENTITY_NULL, wpe_ui_pick(f.ui, 150, 50, false));
    WPE_TEST_ASSERT_EQ_UINT(WPE_ENTITY_NULL, wpe_ui_pick(f.ui, 100, 10, false)); /* Right edge is outside. */
    ((WPE_UiRect*)wpe_ecs_get_component(f.ecs, front, WPE_COMP_UI))->visible = false;
    WPE_TEST_ASSERT_OK(wpe_ui_layout(f.ui, SCREEN_W, SCREEN_H));
    WPE_TEST_ASSERT_EQ_UINT(back, wpe_ui_pick(f.ui, 60, 60, false));
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

/* Draws the UI and, with plain 2D drawing, `copies` of the text at (x + dx, y + dy) in their colors; the
 * two frames must match pixel for pixel. */
static void assert_copies(Fixture* f, WPE_Renderer* renderer, const WPE_BitmapFont* font, const char* string,
                          float x, float y, float scale, const float (*offsets)[2], const WPE_Color* colors,
                          int copies) {
    static WPE_Color expected[128 * 64];
    const WPE_Framebuffer* fb = draw(f, renderer);
    memcpy(expected, fb->color, (size_t)fb->width * fb->height * sizeof *expected);
    WPE_Draw2D* draw2d = NULL;
    WPE_TEST_ASSERT_OK(wpe_draw2d_create(&draw2d));
    WPE_TEST_ASSERT_OK(
        wpe_renderer_begin_frame(renderer, &(WPE_ClearDesc){.clear_color = true, .color = 0xFF000000u}));
    wpe_draw2d_begin(draw2d, fb->width, fb->height);
    for (int i = 0; i < copies; ++i)
        wpe_draw2d_text(draw2d, font, x + offsets[i][0], y + offsets[i][1], string, colors[i], scale);
    WPE_TEST_ASSERT_OK(wpe_draw2d_submit(draw2d, renderer));
    WPE_TEST_ASSERT_OK(wpe_renderer_end_frame(renderer));
    uint32_t different = 0;
    for (uint32_t i = 0; i < fb->width * fb->height; ++i)
        different += fb->color[i] != expected[i];
    WPE_TEST_ASSERT_EQ_UINT(0, different);
    wpe_draw2d_destroy(draw2d);
}

static void test_ui_text_shadow_and_outline(void) {
    Fixture f = make_fixture(false);
    WPE_Renderer* renderer = NULL;
    WPE_TEST_ASSERT_OK(wpe_renderer_create(&(WPE_RendererDesc){.width = 128, .height = 64}, &renderer));
    const WPE_BitmapFont* font = wpe_asset_font(f.t.assets, f.font);
    const WPE_Entity label = add_element(&f, WPE_ENTITY_NULL, fixed(10, 10, 100, 40));
    WPE_Text text = wpe_text_default();
    char string[] = "HI";
    text.text = string;
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(f.ecs, label, WPE_COMP_TEXT, &text, NULL));
    const WPE_Color white = 0xFFFFFFFFu, red = 0xFFFF0000u, blue = 0xFF0000FFu;

    /* Off by default: the text alone. */
    assert_copies(&f, renderer, font, "HI", 10, 10, 1.0f, (const float[][2]){{0, 0}}, &white, 1);

    /* A shadow: one copy under the text, moved by the offset times the scale. */
    WPE_Text* live = wpe_ecs_get_component(f.ecs, label, WPE_COMP_TEXT);
    live->shadow_color = wpe_vec4(1, 0, 0, 1);
    live->shadow_offset = (WPE_Vec2){.x = 1, .y = 2};
    live->scale = 2.0f;
    assert_copies(&f, renderer, font, "HI", 10, 10, 2.0f, (const float[][2]){{2, 4}, {0, 0}},
                  (const WPE_Color[]){red, white}, 2);

    /* An outline and a shadow at scale 1: the shadow of the outlined shape (9 copies), the 8 outline
     * copies, then the text. */
    live->scale = 1.0f;
    live->outline_color = wpe_vec4(0, 0, 1, 1);
    float offsets[18][2];
    WPE_Color colors[18];
    int n = 0;
    for (int pass = 0; pass < 2; ++pass) {
        for (int j = -1; j <= 1; ++j) {
            for (int i = -1; i <= 1; ++i) {
                if (pass == 1 && i == 0 && j == 0) continue;
                offsets[n][0] = (float)i + (pass == 0 ? 1.0f : 0.0f);
                offsets[n][1] = (float)j + (pass == 0 ? 2.0f : 0.0f);
                colors[n++] = pass == 0 ? red : blue;
            }
        }
    }
    offsets[n][0] = offsets[n][1] = 0.0f;
    colors[n++] = white;
    assert_copies(&f, renderer, font, "HI", 10, 10, 1.0f, (const float (*)[2])offsets, colors, n);

    /* Width 0 turns the outline off (the shadow is then one copy again); widths past the cap are capped. */
    live->outline_width = 0;
    assert_copies(&f, renderer, font, "HI", 10, 10, 1.0f, (const float[][2]){{1, 2}, {0, 0}},
                  (const WPE_Color[]){red, white}, 2);
    live->shadow_color.w = 0.0f;
    live->outline_width = 99;
    const WPE_Framebuffer* fb = draw(&f, renderer);
    uint32_t outlined = count_color(fb, blue);
    live->outline_width = WPE_TEXT_MAX_OUTLINE;
    fb = draw(&f, renderer);
    WPE_TEST_ASSERT(outlined > 0);
    WPE_TEST_ASSERT_EQ_UINT(outlined, count_color(fb, blue));

    /* Button tints apply to the shadow and outline too. */
    live->outline_width = 1;
    WPE_Button button = wpe_button_default();
    button.interactable = false;
    button.disabled_tint = wpe_vec4(1, 1, 1, 0);
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(f.ecs, label, WPE_COMP_BUTTON, &button, NULL));
    fb = draw(&f, renderer);
    WPE_TEST_ASSERT_EQ_UINT(128 * 64, count_color(fb, 0xFF000000u));
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

/* An options list whose rows' buttons don't line up (sliders in the middle, toggles at the row's end, wide
 * value texts): up and down go row by row, and left/right find nothing (nothing is wholly to either side). */
static void test_ui_navigation_uses_edges(void) {
    Fixture f = make_fixture(true);
    const float row = 13.0f;
    WPE_Entity buttons[6];
    const float x0[] = {14, 14, 54, 54, 0, 0}, x1[] = {64, 64, 64, 64, 64, 64};
    for (int i = 0; i < 6; ++i) {
        buttons[i] = add_button(&f, 0, 0);
        *(WPE_UiRect*)wpe_ecs_get_component(f.ecs, buttons[i], WPE_COMP_UI) =
            fixed(x0[i] + 60, 10 + row * (float)i, x1[i] - x0[i], 10);
    }
    wpe_input_begin_frame(f.input);
    frame(&f, 0.016f);
    WPE_TEST_ASSERT(wpe_ui_focus(f.ui, buttons[4])); /* Scaling (wide) under Fullscreen (a toggle). */
    tap(&f, SDL_SCANCODE_UP);
    WPE_TEST_ASSERT_EQ_UINT(buttons[3], wpe_ui_focused(f.ui));
    for (int i = 2; i >= 0; --i) {
        tap(&f, SDL_SCANCODE_UP);
        WPE_TEST_ASSERT_EQ_UINT(buttons[i], wpe_ui_focused(f.ui));
    }
    for (int i = 1; i < 6; ++i) {
        tap(&f, SDL_SCANCODE_DOWN);
        WPE_TEST_ASSERT_EQ_UINT(buttons[i], wpe_ui_focused(f.ui));
    }
    for (int i = 0; i < 6; ++i) {
        WPE_TEST_ASSERT(wpe_ui_focus(f.ui, buttons[i]));
        tap(&f, SDL_SCANCODE_LEFT);
        WPE_TEST_ASSERT_EQ_UINT(buttons[i], wpe_ui_focused(f.ui));
        tap(&f, SDL_SCANCODE_RIGHT);
        WPE_TEST_ASSERT_EQ_UINT(buttons[i], wpe_ui_focused(f.ui));
    }
    destroy_fixture(&f);
}

/* A menu shown by a script can take the focus at once, before the next layout marks it shown; a hidden one
 * can't. */
static void test_ui_focus_follows_visibility_now(void) {
    Fixture f = make_fixture(false);
    const WPE_Entity menu = add_element(&f, WPE_ENTITY_NULL, fixed(0, 0, 100, 100));
    const WPE_Entity entry = add_button(&f, 10, 10);
    WPE_TEST_ASSERT_OK(wpe_scene_set_parent(f.ecs, entry, menu, false));
    ((WPE_UiRect*)wpe_ecs_get_component(f.ecs, menu, WPE_COMP_UI))->visible = false;
    WPE_TEST_ASSERT_OK(wpe_ui_layout(f.ui, SCREEN_W, SCREEN_H));
    WPE_TEST_ASSERT(!wpe_ui_focus(f.ui, entry));

    ((WPE_UiRect*)wpe_ecs_get_component(f.ecs, menu, WPE_COMP_UI))->visible = true; /* No layout since. */
    WPE_TEST_ASSERT(!shown(&f, entry));
    WPE_TEST_ASSERT(wpe_ui_focus(f.ui, entry));
    WPE_TEST_ASSERT_EQ_UINT(entry, wpe_ui_focused(f.ui));
    WPE_TEST_ASSERT_OK(wpe_ui_update(f.ui, SCREEN_W, SCREEN_H, 0.016f)); /* Laid out: it keeps the focus. */
    WPE_TEST_ASSERT_EQ_UINT(entry, wpe_ui_focused(f.ui));

    ((WPE_UiRect*)wpe_ecs_get_component(f.ecs, menu, WPE_COMP_UI))->visible = false;
    WPE_TEST_ASSERT(!wpe_ui_focus(f.ui, entry));
    destroy_fixture(&f);
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

static uint32_t change_count(const Fixture* f) {
    uint32_t count = 0;
    wpe_ui_changes(f->ui, &count);
    return count;
}

static const WPE_Toggle* toggle_of(const Fixture* f, WPE_Entity e) {
    return wpe_ecs_get_component(f->ecs, e, WPE_COMP_TOGGLE);
}

static float slider_value(const Fixture* f, WPE_Entity e) {
    return ((const WPE_Slider*)wpe_ecs_get_component(f->ecs, e, WPE_COMP_SLIDER))->value;
}

static void click_at(Fixture* f, float x, float y) {
    push_mouse_move(x, y);
    push_mouse_button(true, x, y);
    frame(f, 0.016f);
    push_mouse_button(false, x, y);
    frame(f, 0.016f);
}

static void test_ui_toggles_and_sliders(void) {
    Fixture f = make_fixture(true);
    const WPE_Entity toggle = add_element(&f, WPE_ENTITY_NULL, fixed(10, 10, 20, 20));
    const WPE_Button button = wpe_button_default();
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(f.ecs, toggle, WPE_COMP_BUTTON, &button, NULL));
    const WPE_Toggle off = wpe_toggle_default();
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(f.ecs, toggle, WPE_COMP_TOGGLE, &off, NULL));
    /* 106 wide with a 6-pixel handle: the handle's center travels 100 pixels, from x = 53 to 153. */
    const WPE_Entity slider = add_element(&f, WPE_ENTITY_NULL, fixed(50, 10, 106, 10));
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(f.ecs, slider, WPE_COMP_BUTTON, &button, NULL));
    WPE_Slider settings = wpe_slider_default();
    settings.max = 10.0f;
    settings.step = 1.0f;
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(f.ecs, slider, WPE_COMP_SLIDER, &settings, NULL));
    wpe_input_begin_frame(f.input);
    frame(&f, 0.016f);

    /* A click flips the toggle and reports the change (and the click). */
    push_mouse_move(20, 20);
    push_mouse_button(true, 20, 20);
    frame(&f, 0.016f);
    WPE_TEST_ASSERT(!toggle_of(&f, toggle)->on);
    push_mouse_button(false, 20, 20);
    WPE_TEST_ASSERT_EQ_UINT(1, frame(&f, 0.016f));
    WPE_TEST_ASSERT(toggle_of(&f, toggle)->on);
    uint32_t count = 0;
    WPE_TEST_ASSERT(change_count(&f) == 1 && wpe_ui_changes(f.ui, &count)[0] == toggle);
    click_at(&f, 20, 20);
    WPE_TEST_ASSERT(!toggle_of(&f, toggle)->on);
    /* Return on the focused toggle flips it too. */
    tap(&f, SDL_SCANCODE_RETURN);
    WPE_TEST_ASSERT(toggle_of(&f, toggle)->on);

    /* Pressing on the slider jumps there; dragging follows the mouse, even past the end. */
    push_mouse_move(103, 15);
    push_mouse_button(true, 103, 15);
    frame(&f, 0.016f);
    WPE_TEST_ASSERT_NEAR(5.0, slider_value(&f, slider), 0);
    WPE_TEST_ASSERT_EQ_UINT(1, change_count(&f));
    push_mouse_move(124, 60); /* 7.1 snaps to 7. */
    frame(&f, 0.016f);
    WPE_TEST_ASSERT_NEAR(7.0, slider_value(&f, slider), 0);
    push_mouse_move(190, 60);
    frame(&f, 0.016f);
    WPE_TEST_ASSERT_NEAR(10.0, slider_value(&f, slider), 0);
    push_mouse_move(191, 60);
    frame(&f, 0.016f);
    WPE_TEST_ASSERT_EQ_UINT(0, change_count(&f)); /* Unchanged: nothing reported. */
    push_mouse_button(false, 191, 60);
    frame(&f, 0.016f);
    push_mouse_move(0, 60); /* Released: moving no longer slides it. */
    frame(&f, 0.016f);
    WPE_TEST_ASSERT_NEAR(10.0, slider_value(&f, slider), 0);

    /* Focused, left and right step it (holding repeats), while up and down still move the focus. */
    WPE_TEST_ASSERT(wpe_ui_focus(f.ui, slider));
    tap(&f, SDL_SCANCODE_LEFT);
    WPE_TEST_ASSERT_NEAR(9.0, slider_value(&f, slider), 0);
    WPE_TEST_ASSERT_EQ_UINT(slider, wpe_ui_focused(f.ui));
    push_key(SDL_SCANCODE_LEFT, true);
    frame(&f, 0.016f);
    frame(&f, 0.3f);
    frame(&f, 0.15f);
    push_key(SDL_SCANCODE_LEFT, false);
    frame(&f, 0.016f);
    WPE_TEST_ASSERT_NEAR(7.0, slider_value(&f, slider), 0);
    tap(&f, SDL_SCANCODE_RIGHT);
    WPE_TEST_ASSERT_NEAR(8.0, slider_value(&f, slider), 0);

    /* Without a step, keys move 1/20 of the range. Values clamp and snap. */
    ((WPE_Slider*)wpe_ecs_get_component(f.ecs, slider, WPE_COMP_SLIDER))->step = 0.0f;
    tap(&f, SDL_SCANCODE_RIGHT);
    WPE_TEST_ASSERT_NEAR(8.5, slider_value(&f, slider), 1e-5);
    WPE_TEST_ASSERT_NEAR(10.0, wpe_slider_constrain(&settings, 12.0f), 0);
    WPE_TEST_ASSERT_NEAR(0.0, wpe_slider_constrain(&settings, -3.0f), 0);
    WPE_TEST_ASSERT_NEAR(3.0, wpe_slider_constrain(&settings, 3.4f), 0);
    WPE_TEST_ASSERT_NEAR(0.0, wpe_slider_constrain(&settings, NAN), 0);
    const WPE_Slider reversed = {.min = 1.0f, .max = -1.0f};
    WPE_TEST_ASSERT_NEAR(-1.0, wpe_slider_constrain(&reversed, -5.0f), 0);

    /* Without a button they don't react. */
    WPE_TEST_ASSERT_OK(wpe_ecs_remove_component(f.ecs, toggle, WPE_COMP_BUTTON));
    click_at(&f, 20, 20);
    WPE_TEST_ASSERT(toggle_of(&f, toggle)->on);
    destroy_fixture(&f);
}

static void test_ui_draws_toggles_and_sliders(void) {
    Fixture f = make_fixture(false);
    WPE_Renderer* renderer = NULL;
    WPE_TEST_ASSERT_OK(wpe_renderer_create(&(WPE_RendererDesc){.width = 64, .height = 48}, &renderer));
    const WPE_Entity toggle = add_element(&f, WPE_ENTITY_NULL, fixed(2, 2, 10, 10));
    WPE_Toggle check = {.on = true, .mark_color = wpe_vec4(1, 0, 0, 1), .mark_inset = 2.0f};
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(f.ecs, toggle, WPE_COMP_TOGGLE, &check, NULL));
    /* 40 x 9 at (2, 20): a 3-pixel track at y 23..25, the handle (4 wide) centered on x = 22. */
    const WPE_Entity slider = add_element(&f, WPE_ENTITY_NULL, fixed(2, 20, 40, 9));
    const WPE_Slider bar = {.value = 0.5f,
                            .max = 1.0f,
                            .track_color = wpe_vec4(0, 0, 1, 1),
                            .fill_color = wpe_vec4(0, 1, 0, 1),
                            .handle_color = wpe_vec4(1, 1, 1, 1),
                            .handle_width = 4.0f};
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(f.ecs, slider, WPE_COMP_SLIDER, &bar, NULL));

    const WPE_Framebuffer* fb = draw(&f, renderer);
    WPE_TEST_ASSERT_EQ_UINT(36, count_color(fb, 0xFFFF0000u)); /* The 6 x 6 mark. */
    WPE_TEST_ASSERT_EQ_UINT(36, count_color(fb, 0xFFFFFFFFu)); /* Handle: 4 x 9. */
    WPE_TEST_ASSERT_EQ_UINT(54,
                            count_color(fb, 0xFF00FF00u)); /* Fill: x 2..21, less the handle's 2 columns. */
    WPE_TEST_ASSERT_EQ_UINT(54, count_color(fb, 0xFF0000FFu)); /* Track: x 22..41, likewise. */
    ((WPE_Toggle*)wpe_ecs_get_component(f.ecs, toggle, WPE_COMP_TOGGLE))->on = false;
    ((WPE_Slider*)wpe_ecs_get_component(f.ecs, slider, WPE_COMP_SLIDER))->value = 0.0f;
    fb = draw(&f, renderer);
    WPE_TEST_ASSERT_EQ_UINT(0, count_color(fb, 0xFFFF0000u));
    WPE_TEST_ASSERT_EQ_UINT(0, count_color(fb, 0xFF00FF00u)); /* Nothing filled at the minimum. */
    wpe_renderer_destroy(renderer);
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

/* ---- Text fields ----------------------------------------------------------------------------------- */

static void push_text(const char* text) {
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.text = text; /* A literal: it outlives the queue. */
    WPE_TEST_ASSERT(SDL_PushEvent(&e));
}

static void type_text(Fixture* f, const char* text) {
    push_text(text);
    frame(f, 0.016f);
}

/* Taps `key` with `modifier` held. */
static void chord(Fixture* f, SDL_Scancode modifier, SDL_Scancode key) {
    push_key(modifier, true);
    push_key(key, true);
    frame(f, 0.016f);
    push_key(key, false);
    push_key(modifier, false);
    frame(f, 0.016f);
}

static WPE_Entity add_field(Fixture* f, float x, float y, const char* value) {
    const WPE_Entity e = add_element(f, WPE_ENTITY_NULL, fixed(x, y, 100, 12));
    const WPE_Button button = wpe_button_default();
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(f->ecs, e, WPE_COMP_BUTTON, &button, NULL));
    WPE_TextInput field = wpe_text_input_default();
    field.value = (char*)value; /* The add hook copies it. */
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(f->ecs, e, WPE_COMP_TEXT_INPUT, &field, NULL));
    return e;
}

static const char* field_text(const Fixture* f, WPE_Entity e) {
    return wpe_ui_input_text(f->ecs, e);
}

static uint32_t submit_count(const Fixture* f) {
    uint32_t count = 0;
    wpe_ui_submits(f->ui, &count);
    return count;
}

static void test_ui_text_field_editing(void) {
    Fixture f = make_fixture(true);
    const WPE_Entity field = add_field(&f, 10, 10, "ab");
    const WPE_Entity below = add_button(&f, 10, 40);
    wpe_input_begin_frame(f.input);
    frame(&f, 0.016f);
    WPE_TEST_ASSERT_EQ_UINT(WPE_ENTITY_NULL, wpe_ui_editing(f.ui));

    /* A click past the text edits with the cursor at its end; typing inserts and reports each change. */
    click_at(&f, 100, 15);
    WPE_TEST_ASSERT_EQ_UINT(field, wpe_ui_editing(f.ui));
    WPE_TEST_ASSERT(
        ((const WPE_TextInput*)wpe_ecs_get_component(f.ecs, field, WPE_COMP_TEXT_INPUT))->editing);
    push_text("cd");
    frame(&f, 0.016f);
    WPE_TEST_ASSERT_EQ_STR("abcd", field_text(&f, field));
    WPE_TEST_ASSERT_EQ_UINT(1, change_count(&f));
    tap(&f, SDL_SCANCODE_BACKSPACE);
    WPE_TEST_ASSERT_EQ_STR("abc", field_text(&f, field));
    tap(&f, SDL_SCANCODE_LEFT);
    tap(&f, SDL_SCANCODE_LEFT);
    type_text(&f, "X");
    WPE_TEST_ASSERT_EQ_STR("aXbc", field_text(&f, field));
    tap(&f, SDL_SCANCODE_DELETE);
    WPE_TEST_ASSERT_EQ_STR("aXc", field_text(&f, field));
    /* Home, then Shift+End selects everything; typing replaces the selection. */
    tap(&f, SDL_SCANCODE_HOME);
    chord(&f, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_END);
    type_text(&f, "z");
    WPE_TEST_ASSERT_EQ_STR("z", field_text(&f, field));
    /* Ctrl+A and Backspace empty it; Ctrl+Backspace deletes a word. */
    chord(&f, SDL_SCANCODE_LCTRL, SDL_SCANCODE_A);
    tap(&f, SDL_SCANCODE_BACKSPACE);
    WPE_TEST_ASSERT_EQ_STR("", field_text(&f, field));
    type_text(&f, "one two");
    chord(&f, SDL_SCANCODE_LCTRL, SDL_SCANCODE_BACKSPACE);
    WPE_TEST_ASSERT_EQ_STR("one ", field_text(&f, field));
    /* Keys typed with Ctrl held type nothing (the layout's text event is ignored). */
    push_key(SDL_SCANCODE_LCTRL, true);
    push_text("q");
    frame(&f, 0.016f);
    push_key(SDL_SCANCODE_LCTRL, false);
    frame(&f, 0.016f);
    WPE_TEST_ASSERT_EQ_STR("one ", field_text(&f, field));

    /* Escape puts back the text from before editing and stops. */
    tap(&f, SDL_SCANCODE_ESCAPE);
    WPE_TEST_ASSERT_EQ_STR("ab", field_text(&f, field));
    WPE_TEST_ASSERT_EQ_UINT(WPE_ENTITY_NULL, wpe_ui_editing(f.ui));
    WPE_TEST_ASSERT_EQ_UINT(field, wpe_ui_focused(f.ui));
    /* Typing no longer edits. */
    type_text(&f, "q");
    WPE_TEST_ASSERT_EQ_STR("ab", field_text(&f, field));

    /* Return on the focused field edits; Return again submits (not a click). */
    tap(&f, SDL_SCANCODE_RETURN);
    WPE_TEST_ASSERT_EQ_UINT(field, wpe_ui_editing(f.ui));
    type_text(&f, "1");
    push_key(SDL_SCANCODE_RETURN, true);
    WPE_TEST_ASSERT_EQ_UINT(0, frame(&f, 0.016f));
    WPE_TEST_ASSERT_EQ_UINT(1, submit_count(&f));
    uint32_t count = 0;
    WPE_TEST_ASSERT_EQ_UINT(field, wpe_ui_submits(f.ui, &count)[0]);
    push_key(SDL_SCANCODE_RETURN, false);
    WPE_TEST_ASSERT_EQ_UINT(0, frame(&f, 0.016f)); /* The release doesn't click the field either. */
    WPE_TEST_ASSERT_EQ_UINT(0, submit_count(&f));
    WPE_TEST_ASSERT_EQ_STR("ab1", field_text(&f, field));
    WPE_TEST_ASSERT_EQ_UINT(WPE_ENTITY_NULL, wpe_ui_editing(f.ui));

    /* Down leaves the field for the button below; a click elsewhere stops editing too. */
    WPE_TEST_ASSERT(wpe_ui_edit(f.ui, field));
    tap(&f, SDL_SCANCODE_DOWN);
    WPE_TEST_ASSERT_EQ_UINT(WPE_ENTITY_NULL, wpe_ui_editing(f.ui));
    WPE_TEST_ASSERT_EQ_UINT(below, wpe_ui_focused(f.ui));
    WPE_TEST_ASSERT(wpe_ui_edit(f.ui, field));
    click_at(&f, 150, 90);
    WPE_TEST_ASSERT_EQ_UINT(WPE_ENTITY_NULL, wpe_ui_editing(f.ui));
    /* Tab moves on as well. */
    WPE_TEST_ASSERT(wpe_ui_edit(f.ui, field));
    tap(&f, SDL_SCANCODE_TAB);
    WPE_TEST_ASSERT_EQ_UINT(below, wpe_ui_focused(f.ui));
    WPE_TEST_ASSERT_EQ_UINT(WPE_ENTITY_NULL, wpe_ui_editing(f.ui));

    /* Only fields edit; a hidden field stops editing. */
    WPE_TEST_ASSERT(!wpe_ui_edit(f.ui, below));
    WPE_TEST_ASSERT(wpe_ui_edit(f.ui, field));
    ((WPE_UiRect*)wpe_ecs_get_component(f.ecs, field, WPE_COMP_UI))->visible = false;
    frame(&f, 0.016f);
    WPE_TEST_ASSERT_EQ_UINT(WPE_ENTITY_NULL, wpe_ui_editing(f.ui));
    destroy_fixture(&f);
}

static void test_ui_text_field_filters_and_limits(void) {
    Fixture f = make_fixture(true);
    const WPE_Entity field = add_field(&f, 10, 10, "");
    WPE_TextInput* settings = wpe_ecs_get_component(f.ecs, field, WPE_COMP_TEXT_INPUT);
    settings->filter = WPE_TEXT_FILTER_DIGITS;
    settings->max_length = 3;
    wpe_input_begin_frame(f.input);
    frame(&f, 0.016f);
    WPE_TEST_ASSERT(wpe_ui_edit(f.ui, field));
    type_text(&f, "a1b2");
    WPE_TEST_ASSERT_EQ_STR("12", field_text(&f, field));
    type_text(&f, "345");
    WPE_TEST_ASSERT_EQ_STR("123", field_text(&f, field));
    /* Non-ASCII (UTF-8) characters are dropped: the fonts have no glyphs for them. */
    settings = wpe_ecs_get_component(f.ecs, field, WPE_COMP_TEXT_INPUT);
    settings->filter = WPE_TEXT_FILTER_ANY;
    settings->max_length = 0;
    chord(&f, SDL_SCANCODE_LCTRL, SDL_SCANCODE_A);
    type_text(&f, "caf\xc3\xa9!");
    WPE_TEST_ASSERT_EQ_STR("caf!", field_text(&f, field));
    /* Numbers: one leading '-', one '.'. */
    settings = wpe_ecs_get_component(f.ecs, field, WPE_COMP_TEXT_INPUT);
    settings->filter = WPE_TEXT_FILTER_NUMBER;
    chord(&f, SDL_SCANCODE_LCTRL, SDL_SCANCODE_A);
    type_text(&f, "-1.5.-2");
    WPE_TEST_ASSERT_EQ_STR("-1.52", field_text(&f, field));
    tap(&f, SDL_SCANCODE_HOME);
    type_text(&f, "-");
    WPE_TEST_ASSERT_EQ_STR("-1.52", field_text(&f, field));
    /* A rejected character leaves a selection alone. */
    chord(&f, SDL_SCANCODE_LCTRL, SDL_SCANCODE_A);
    type_text(&f, "x");
    WPE_TEST_ASSERT_EQ_STR("-1.52", field_text(&f, field));
    /* Names and addresses. */
    settings = wpe_ecs_get_component(f.ecs, field, WPE_COMP_TEXT_INPUT);
    settings->filter = WPE_TEXT_FILTER_NAME;
    type_text(&f, "Ada L_1!");
    WPE_TEST_ASSERT_EQ_STR("Ada L_1", field_text(&f, field));
    settings = wpe_ecs_get_component(f.ecs, field, WPE_COMP_TEXT_INPUT);
    settings->filter = WPE_TEXT_FILTER_ADDRESS;
    chord(&f, SDL_SCANCODE_LCTRL, SDL_SCANCODE_A);
    type_text(&f, "192.168.0.1:47 ");
    WPE_TEST_ASSERT_EQ_STR("192.168.0.1:47", field_text(&f, field));

    /* Setting the text from code cuts it at the limit and moves the cursor to the end. */
    settings = wpe_ecs_get_component(f.ecs, field, WPE_COMP_TEXT_INPUT);
    settings->max_length = 4;
    WPE_TEST_ASSERT_OK(wpe_ui_set_input_text(f.ecs, field, "abcdef"));
    WPE_TEST_ASSERT_EQ_STR("abcd", field_text(&f, field));
    WPE_TEST_ASSERT_EQ_UINT(
        4, ((const WPE_TextInput*)wpe_ecs_get_component(f.ecs, field, WPE_COMP_TEXT_INPUT))->cursor);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND,
                           wpe_ui_set_input_text(f.ecs, wpe_ecs_spawn(f.ecs), "x"));
    WPE_TEST_ASSERT(wpe_ui_input_text(f.ecs, wpe_ecs_spawn(f.ecs)) == NULL);

    /* A copied entity owns its own strings. */
    const WPE_Entity copy = add_field(&f, 10, 40, field_text(&f, field));
    WPE_TEST_ASSERT_OK(wpe_ui_set_input_text(f.ecs, field, "zz"));
    WPE_TEST_ASSERT_EQ_STR("abcd", field_text(&f, copy));
    destroy_fixture(&f); /* Despawning an editing field frees its strings (the leak check). */
}

static void test_ui_draws_text_fields(void) {
    Fixture f = make_fixture(false);
    WPE_Renderer* renderer = NULL;
    WPE_TEST_ASSERT_OK(wpe_renderer_create(&(WPE_RendererDesc){.width = 120, .height = 32}, &renderer));
    const WPE_Entity field = add_field(&f, 2, 2, "");
    WPE_TextInput* settings = wpe_ecs_get_component(f.ecs, field, WPE_COMP_TEXT_INPUT);
    settings->cursor_color = wpe_vec4(1, 0, 0, 1);
    settings->placeholder_color = wpe_vec4(0, 1, 0, 1);
    WPE_TEST_ASSERT_OK(wpe_ui_set_input_text(f.ecs, field, ""));
    /* The placeholder shows while empty and not editing. */
    char* hint = wpe_alloc(5);
    memcpy(hint, "name", 5);
    wpe_free(settings->placeholder);
    settings->placeholder = hint;
    const WPE_Framebuffer* fb = draw(&f, renderer);
    WPE_TEST_ASSERT(count_color(fb, 0xFF00FF00u) > 0);
    WPE_TEST_ASSERT_EQ_UINT(0, count_color(fb, 0xFFFF0000u));
    /* Editing: no placeholder, a cursor. */
    WPE_TEST_ASSERT_OK(wpe_ui_layout(f.ui, 120, 32));
    WPE_TEST_ASSERT(wpe_ui_edit(f.ui, field));
    fb = draw(&f, renderer);
    WPE_TEST_ASSERT_EQ_UINT(0, count_color(fb, 0xFF00FF00u));
    WPE_TEST_ASSERT(count_color(fb, 0xFFFF0000u) > 0);
    /* A password draws stars: the same pixels for any text of one length. */
    settings = wpe_ecs_get_component(f.ecs, field, WPE_COMP_TEXT_INPUT);
    settings->password = true;
    wpe_ui_edit(f.ui, WPE_ENTITY_NULL);
    WPE_TEST_ASSERT_OK(wpe_ui_set_input_text(f.ecs, field, "abc"));
    const uint32_t white = count_color(draw(&f, renderer), 0xFFFFFFFFu);
    WPE_TEST_ASSERT(white > 0);
    WPE_TEST_ASSERT_OK(wpe_ui_set_input_text(f.ecs, field, "xyz"));
    WPE_TEST_ASSERT_EQ_UINT(white, count_color(draw(&f, renderer), 0xFFFFFFFFu));
    wpe_renderer_destroy(renderer);
    destroy_fixture(&f);
}
/* "Image size": a sprite element takes its image's (or region's) size times a scale, centered where it was,
 * its anchors met in one point (so the size stays on any screen), its corners on whole pixels. */
static void test_ui_fit_image(void) {
    Fixture f = make_fixture(false);
    uint32_t texels[30 * 12];
    for (size_t i = 0; i < sizeof texels / sizeof texels[0]; ++i)
        texels[i] = 0xFFFFFFFFu;
    WPE_Texture* texture = NULL;
    WPE_TEST_ASSERT_OK(
        wpe_texture_create(&(WPE_TextureDesc){.width = 30, .height = 12, .texels = texels}, &texture));
    WPE_AssetHandle image = WPE_ASSET_NONE;
    WPE_TEST_ASSERT_OK(wpe_asset_add(f.t.assets, WPE_ASSET_IMAGE, "test/logo", texture, &image));
    WPE_Sprite sprite = wpe_sprite_default();
    sprite.image = image;

    /* A logo stretched across the screen's top half (anchors apart), centered at (100, 25). */
    WPE_UiRect stretched = wpe_ui_rect_default();
    stretched.anchor_max = (WPE_Vec2){.x = 1, .y = 0.5f};
    const WPE_Entity logo = add_element(&f, WPE_ENTITY_NULL, stretched);
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(f.ecs, logo, WPE_COMP_SPRITE, &sprite, NULL));
    WPE_UiRect fitted;
    WPE_TEST_ASSERT_OK(wpe_ui_fit_image_rect(f.ui, logo, 1.0f, SCREEN_W, SCREEN_H, &fitted));
    assert_rect((WPE_Rect){85, 19, 30, 12}, fitted.rect);
    WPE_TEST_ASSERT_NEAR(0.5, fitted.anchor_min.x, 0.0); /* The middle of the old anchors... */
    WPE_TEST_ASSERT_NEAR(0.25, fitted.anchor_min.y, 0.0);
    WPE_TEST_ASSERT(fitted.anchor_max.x == fitted.anchor_min.x && fitted.anchor_max.y == fitted.anchor_min.y);
    /* ...and nothing changed yet. */
    WPE_TEST_ASSERT_NEAR(
        1.0, ((const WPE_UiRect*)wpe_ecs_get_component(f.ecs, logo, WPE_COMP_UI))->anchor_max.x, 0.0);

    /* Applied at 2x: twice as big, same center; a bigger screen keeps the size. */
    WPE_TEST_ASSERT_OK(wpe_ui_fit_image(f.ui, logo, 2.0f, SCREEN_W, SCREEN_H));
    assert_rect((WPE_Rect){70, 13, 60, 24}, rect_of(&f, logo));
    WPE_TEST_ASSERT_OK(wpe_ui_layout(f.ui, 400, 300));
    WPE_TEST_ASSERT_NEAR(60.0, rect_of(&f, logo).width, 0.0);
    WPE_TEST_ASSERT_NEAR(24.0, rect_of(&f, logo).height, 0.0);

    /* A source region counts instead of the whole image; an odd size still lands on whole pixels; a child is
     * placed in its parent. */
    const WPE_Entity panel = add_element(&f, WPE_ENTITY_NULL, fixed(10, 10, 100, 50));
    const WPE_Entity icon = add_element(&f, panel, fixed(0, 0, 40, 40));
    sprite.source = (WPE_Rect){0, 0, 7, 5};
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(f.ecs, icon, WPE_COMP_SPRITE, &sprite, NULL));
    WPE_TEST_ASSERT_OK(wpe_ui_fit_image(f.ui, icon, 1.0f, SCREEN_W, SCREEN_H));
    const WPE_Rect r = rect_of(&f, icon);
    WPE_TEST_ASSERT(r.width == 7.0f && r.height == 5.0f && r.x == floorf(r.x) && r.y == floorf(r.y));
    WPE_TEST_ASSERT(fabsf(r.x + 3.5f - 30.0f) <= 0.5f &&
                    fabsf(r.y + 2.5f - 30.0f) <= 0.5f); /* Was centered at 30. */

    /* Not a sprite with an image; a bad scale. */
    wpe_log_set_stderr_enabled(false);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND,
                           wpe_ui_fit_image(f.ui, panel, 1.0f, SCREEN_W, SCREEN_H));
    wpe_log_set_stderr_enabled(true);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG,
                           wpe_ui_fit_image(f.ui, logo, 0.0f, SCREEN_W, SCREEN_H));
    wpe_asset_release(f.t.assets, image);
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

WPE_TEST_MAIN(
    WPE_TEST_CASE(test_ui_layout_anchors_and_visibility), WPE_TEST_CASE(test_ui_pick_follows_drawing_order),
    WPE_TEST_CASE(test_ui_draws_sprites_with_button_tints), WPE_TEST_CASE(test_ui_nine_slice_keeps_borders),
    WPE_TEST_CASE(test_ui_text_alignment_and_wrapping), WPE_TEST_CASE(test_ui_text_shadow_and_outline),
    WPE_TEST_CASE(test_ui_keyboard_navigation_and_clicks), WPE_TEST_CASE(test_ui_navigation_uses_edges),
    WPE_TEST_CASE(test_ui_focus_follows_visibility_now), WPE_TEST_CASE(test_ui_mouse_focus_and_clicks),
    WPE_TEST_CASE(test_ui_toggles_and_sliders), WPE_TEST_CASE(test_ui_draws_toggles_and_sliders),
    WPE_TEST_CASE(test_ui_navigation_uses_project_actions), WPE_TEST_CASE(test_ui_text_field_editing),
    WPE_TEST_CASE(test_ui_text_field_filters_and_limits), WPE_TEST_CASE(test_ui_draws_text_fields),
    WPE_TEST_CASE(test_ui_fit_image), WPE_TEST_CASE(test_ui_create_validates))
