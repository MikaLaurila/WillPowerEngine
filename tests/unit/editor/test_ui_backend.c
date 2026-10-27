#include "wpe_test.h"

#include <SDL3/SDL.h>
#include <cimgui.h>
#include <stdlib.h>
#include <string.h>

#include "editor/ui_backend.h"
#include "platform/platform.h"

#define WIDTH 320u
#define HEIGHT 200u

/* Builds a small ImGui window with text, a button and a 4x4 magenta image shown at 32x32. */
static void build_frame(WPE_UiBackend* ui, uint64_t* image) {
    uint32_t texels[16];
    for (int i = 0; i < 16; ++i)
        texels[i] = 0xFFFF00FFu;
    WPE_TEST_ASSERT_OK(wpe_ui_set_image(ui, image, 4, 4, texels));
    wpe_ui_new_frame(ui, WIDTH, HEIGHT, 1.0f / 60.0f);
    igSetNextWindowPos((ImVec2_c){10, 10}, ImGuiCond_Always, (ImVec2_c){0, 0});
    igSetNextWindowSize((ImVec2_c){200, 150}, ImGuiCond_Always);
    igBegin("Hello", NULL, 0);
    igText("software rendered UI");
    igButton("Button", (ImVec2_c){0, 0});
    igImage((ImTextureRef_c){._TexID = *image}, (ImVec2_c){32, 32}, (ImVec2_c){0, 0}, (ImVec2_c){1, 1});
    igEnd();
}

/* Checks the frame: background outside the window; inside, its background, text and the image. */
static void check_frame(const WPE_Color* pixels) {
    WPE_TEST_ASSERT_EQ_UINT(0xFF000000u, pixels[190 * WIDTH + 300]);
    uint32_t window = 0, bright = 0, magenta = 0;
    for (uint32_t y = 10; y < 160; ++y) {
        for (uint32_t x = 10; x < 210; ++x) {
            const WPE_Color c = pixels[y * WIDTH + x];
            window += c != 0xFF000000u;
            bright += ((c >> 16) & 0xFF) > 200 && ((c >> 8) & 0xFF) > 200 && (c & 0xFF) > 200;
            magenta += c == 0xFFFF00FFu;
        }
    }
    WPE_TEST_ASSERT(window > 150 * 200 * 9 / 10); /* The window background covers its area. */
    WPE_TEST_ASSERT(bright > 50);                 /* Text glyphs. */
    WPE_TEST_ASSERT_EQ_UINT(32 * 32, magenta);    /* The image, exactly 32x32. */
}

/* Draws the test frame in software; the caller frees the pixels. */
static WPE_Color* render_software(void) {
    WPE_UiBackend* ui = NULL;
    WPE_TEST_ASSERT_OK(wpe_ui_create(NULL, &ui));
    WPE_TEST_ASSERT(!wpe_ui_is_gpu(ui));
    WPE_Renderer* renderer = NULL;
    WPE_TEST_ASSERT_OK(wpe_renderer_create(&(WPE_RendererDesc){.width = WIDTH, .height = HEIGHT}, &renderer));
    uint64_t image = 0;
    for (int frame = 0; frame < 3; ++frame) {
        build_frame(ui, &image);
        WPE_TEST_ASSERT_OK(wpe_renderer_begin_frame(
            renderer,
            &(WPE_ClearDesc){.clear_color = true, .color = 0xFF000000u, .clear_depth = true, .depth = 1}));
        WPE_TEST_ASSERT_OK(wpe_ui_render(ui, renderer));
        WPE_TEST_ASSERT_OK(wpe_renderer_end_frame(renderer));
    }
    const WPE_Framebuffer* fb = wpe_renderer_framebuffer(renderer);
    WPE_Color* pixels = malloc(sizeof(WPE_Color) * WIDTH * HEIGHT);
    WPE_TEST_ASSERT(pixels != NULL);
    memcpy(pixels, fb->color, sizeof(WPE_Color) * WIDTH * HEIGHT);
    wpe_ui_destroy(ui);
    wpe_renderer_destroy(renderer);
    return pixels;
}

static void test_ui_backend_renders_windows_and_images(void) {
    WPE_UiBackend* ui = NULL;
    WPE_TEST_ASSERT_OK(wpe_ui_create(NULL, &ui));
    WPE_UiBackend* second = NULL;
    wpe_log_set_stderr_enabled(false);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_STATE, wpe_ui_create(NULL, &second));
    /* Each backend draws one way only. */
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_STATE, wpe_ui_present(ui, 0xFF000000u));
    WPE_Image none;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_STATE, wpe_ui_capture(ui, &none));
    /* Images: unknown ids and empty sizes are rejected. */
    uint64_t bogus = 99;
    const WPE_Color texel = 0xFFFFFFFFu;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_ui_set_image(ui, &bogus, 1, 1, &texel));
    uint64_t empty = 0;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_ui_set_image(ui, &empty, 0, 1, &texel));
    wpe_log_set_stderr_enabled(true);
    wpe_ui_destroy(ui);

    WPE_Color* pixels = render_software();
    WPE_TEST_ASSERT_OK(wpe_image_save_png("ui_backend.png", WIDTH, HEIGHT, pixels));
    check_frame(pixels);
    free(pixels);
}

/* The GPU path, through SDL's software implementation of SDL_Renderer (no window or GPU needed): it
 * must draw the same frame as the engine's rasterizer, up to edge antialiasing. */
static void test_ui_backend_gpu_path_matches_software(void) {
    SDL_Surface* surface = SDL_CreateSurface((int)WIDTH, (int)HEIGHT, SDL_PIXELFORMAT_ARGB8888);
    WPE_TEST_ASSERT(surface != NULL);
    SDL_Renderer* renderer = SDL_CreateSoftwareRenderer(surface);
    WPE_TEST_ASSERT(renderer != NULL);
    WPE_UiBackend* ui = NULL;
    WPE_TEST_ASSERT_OK(wpe_ui_create((WPE_PlatformRenderer*)renderer, &ui));
    WPE_TEST_ASSERT(wpe_ui_is_gpu(ui));
    uint32_t width = 0, height = 0;
    wpe_ui_output_size(ui, &width, &height);
    WPE_TEST_ASSERT_EQ_UINT(WIDTH, width);
    WPE_TEST_ASSERT_EQ_UINT(HEIGHT, height);
    WPE_Image image;
    wpe_log_set_stderr_enabled(false);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_STATE, wpe_ui_capture(ui, &image)); /* Nothing shown. */
    WPE_Renderer* small = NULL;
    WPE_TEST_ASSERT_OK(wpe_renderer_create(&(WPE_RendererDesc){.width = 8, .height = 8}, &small));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_STATE, wpe_ui_render(ui, small));
    wpe_renderer_destroy(small);
    wpe_log_set_stderr_enabled(true);

    uint64_t id = 0;
    for (int frame = 0; frame < 3; ++frame) {
        build_frame(ui, &id);
        WPE_TEST_ASSERT_OK(wpe_ui_present(ui, 0xFF000000u));
    }
    WPE_TEST_ASSERT_OK(wpe_ui_capture(ui, &image));
    WPE_TEST_ASSERT_EQ_UINT(WIDTH, image.width);
    WPE_TEST_ASSERT_EQ_UINT(HEIGHT, image.height);
    WPE_TEST_ASSERT_OK(wpe_image_save_png("ui_backend_gpu.png", image.width, image.height, image.pixels));
    check_frame(image.pixels);
    /* The presented frame reached the window (here the surface) too. */
    const WPE_Color* shown = surface->pixels;
    WPE_TEST_ASSERT_EQ_UINT(image.pixels[60 * WIDTH + 60] & 0x00FFFFFFu,
                            shown[60 * WIDTH + 60] & 0x00FFFFFFu);
    wpe_ui_destroy(ui);
    SDL_DestroyRenderer(renderer);
    SDL_DestroySurface(surface);

    /* Same layout and colors as the software path: only antialiased edges may differ. */
    WPE_Color* software = render_software();
    uint32_t different = 0;
    for (uint32_t i = 0; i < WIDTH * HEIGHT; ++i) {
        int worst = 0;
        for (int shift = 0; shift < 24; shift += 8) {
            const int d =
                abs((int)((image.pixels[i] >> shift) & 0xFF) - (int)((software[i] >> shift) & 0xFF));
            worst = d > worst ? d : worst;
        }
        different += worst > 48;
    }
    WPE_TEST_ASSERT(different < WIDTH * HEIGHT / 50);
    free(software);
    wpe_image_free(&image);
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_ui_backend_renders_windows_and_images),
              WPE_TEST_CASE(test_ui_backend_gpu_path_matches_software))
