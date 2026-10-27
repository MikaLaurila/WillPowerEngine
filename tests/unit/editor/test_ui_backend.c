#include "wpe_test.h"

#include <cimgui.h>

#include "editor/ui_backend.h"
#include "platform/platform.h"

/* Renders a small ImGui window with the software rasterizer. */
static void draw_frame(WPE_UiBackend* ui, WPE_Renderer* renderer, const WPE_Texture* image) {
    wpe_ui_new_frame(ui, 320, 200, 1.0f / 60.0f);
    igSetNextWindowPos((ImVec2_c){10, 10}, ImGuiCond_Always, (ImVec2_c){0, 0});
    igSetNextWindowSize((ImVec2_c){200, 150}, ImGuiCond_Always);
    igBegin("Hello", NULL, 0);
    igText("software rendered UI");
    igButton("Button", (ImVec2_c){0, 0});
    igImage((ImTextureRef_c){._TexID = wpe_ui_texture_id(ui, image)}, (ImVec2_c){32, 32}, (ImVec2_c){0, 0},
            (ImVec2_c){1, 1});
    igEnd();
    WPE_TEST_ASSERT_OK(wpe_renderer_begin_frame(
        renderer,
        &(WPE_ClearDesc){.clear_color = true, .color = 0xFF000000u, .clear_depth = true, .depth = 1}));
    WPE_TEST_ASSERT_OK(wpe_ui_render(ui, renderer));
    WPE_TEST_ASSERT_OK(wpe_renderer_end_frame(renderer));
}

static void test_ui_backend_renders_windows_and_images(void) {
    WPE_UiBackend* ui = NULL;
    WPE_TEST_ASSERT_OK(wpe_ui_create(&ui));
    WPE_UiBackend* second = NULL;
    wpe_log_set_stderr_enabled(false);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_STATE, wpe_ui_create(&second));
    wpe_log_set_stderr_enabled(true);
    WPE_Renderer* renderer = NULL;
    WPE_TEST_ASSERT_OK(wpe_renderer_create(&(WPE_RendererDesc){.width = 320, .height = 200}, &renderer));
    uint32_t texels[16];
    for (int i = 0; i < 16; ++i)
        texels[i] = 0xFFFF00FFu; /* Magenta. */
    WPE_Texture* image = NULL;
    WPE_TEST_ASSERT_OK(
        wpe_texture_create(&(WPE_TextureDesc){.width = 4, .height = 4, .texels = texels}, &image));

    for (int frame = 0; frame < 3; ++frame)
        draw_frame(ui, renderer, image);
    const WPE_Framebuffer* fb = wpe_renderer_framebuffer(renderer);
    WPE_TEST_ASSERT_OK(wpe_image_save_png("ui_backend.png", fb->width, fb->height, fb->color));
    /* Outside the window: untouched background; inside: the window, its text and the image. */
    WPE_TEST_ASSERT_EQ_UINT(0xFF000000u, fb->color[190 * 320 + 300]);
    uint32_t window = 0, bright = 0, magenta = 0;
    for (uint32_t y = 10; y < 160; ++y) {
        for (uint32_t x = 10; x < 210; ++x) {
            const WPE_Color c = fb->color[y * 320 + x];
            window += c != 0xFF000000u;
            bright += ((c >> 16) & 0xFF) > 200 && ((c >> 8) & 0xFF) > 200 && (c & 0xFF) > 200;
            magenta += c == 0xFFFF00FFu;
        }
    }
    WPE_TEST_ASSERT(window > 150 * 200 * 9 / 10); /* The window background covers its area. */
    WPE_TEST_ASSERT(bright > 50);                 /* Text glyphs. */
    WPE_TEST_ASSERT_EQ_UINT(32 * 32, magenta);    /* The image, exactly 32x32. */
    /* The same texture keeps its id. */
    WPE_TEST_ASSERT_EQ_UINT(wpe_ui_texture_id(ui, image), wpe_ui_texture_id(ui, image));

    wpe_ui_destroy(ui);
    wpe_texture_destroy(image);
    wpe_renderer_destroy(renderer);
    /* A new backend can be created after the first is gone. */
    WPE_TEST_ASSERT_OK(wpe_ui_create(&ui));
    wpe_ui_destroy(ui);
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_ui_backend_renders_windows_and_images))
