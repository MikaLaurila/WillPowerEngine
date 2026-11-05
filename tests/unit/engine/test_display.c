/*
 * The player's display choices: the display file, an engine with a real window (SDL's "dummy" video driver,
 * so no screen is needed) opening as the player left it, wpe.set_display/get_display, and Alt+Enter.
 */
#include "wpe_test.h"

#include <SDL3/SDL.h>
#include <string.h>

#include "engine/engine.h"

/* Scratch files live below the test's working directory. */
#define ROOT "display_scratch"

static void quiet(bool on) {
    wpe_log_set_stderr_enabled(!on);
}

static void write_text(const char* path, const char* text) {
    WPE_File* file = NULL;
    WPE_TEST_ASSERT_OK(wpe_file_open(path, true, &file));
    WPE_TEST_ASSERT_OK(wpe_file_write(file, text, strlen(text)));
    WPE_TEST_ASSERT_OK(wpe_file_close(file));
}

static WPE_DisplaySettings parse(const char* text, WPE_DisplaySettings base, WPE_Result expected) {
    WPE_TEST_ASSERT_EQ_INT(expected, wpe_display_parse(text, strlen(text), "test", &base));
    return base;
}

static void test_display_file(void) {
    const WPE_DisplaySettings base = {.scaling = WPE_WINDOW_SCALING_INTEGER, .scale = 3, .vsync = true};
    /* Keys left out keep their values. */
    WPE_DisplaySettings d =
        parse("{\"format\": \"wpe.display\", \"version\": 1, \"fullscreen\": true, \"scaling\": \"stretch\"}",
              base, WPE_RESULT_OK);
    WPE_TEST_ASSERT(d.fullscreen && d.scaling == WPE_WINDOW_SCALING_STRETCH && d.scale == 3 && d.vsync);

    /* Written and read back. */
    d.scale = 5;
    d.vsync = false;
    WPE_JsonWriter* w = NULL;
    WPE_TEST_ASSERT_OK(wpe_json_writer_create(&w));
    WPE_TEST_ASSERT_OK(wpe_display_write(&d, w));
    const char* text = NULL;
    size_t size = 0;
    WPE_TEST_ASSERT_OK(wpe_json_writer_finish(w, &text, &size));
    WPE_DisplaySettings back = base;
    WPE_TEST_ASSERT_OK(wpe_display_parse(text, size, "written", &back));
    WPE_TEST_ASSERT(back.fullscreen && back.scaling == WPE_WINDOW_SCALING_STRETCH && back.scale == 5 &&
                    !back.vsync);
    wpe_json_writer_destroy(w);

    /* Bad documents change nothing. */
    quiet(true);
    d = parse("{\"format\": \"wpe.display\", \"version\": 1, \"fullscreen\": true, \"scale\": 0}", base,
              WPE_RESULT_ERROR_PARSE);
    WPE_TEST_ASSERT(!d.fullscreen && d.scale == 3);
    parse("{\"format\": \"wpe.display\", \"version\": 1, \"scaling\": \"crop\"}", base,
          WPE_RESULT_ERROR_PARSE);
    parse("{\"format\": \"wpe.display\", \"version\": 1, \"size\": 3}", base, WPE_RESULT_ERROR_PARSE);
    parse("{\"format\": \"wpe.bindings\", \"version\": 1}", base, WPE_RESULT_ERROR_PARSE);
    parse("{\"format\": \"wpe.display\", \"version\": 2}", base, WPE_RESULT_ERROR_UNSUPPORTED);
    parse("{\"format\": \"wpe.display\"", base, WPE_RESULT_ERROR_PARSE);
    quiet(false);
}

static WPE_Engine* create(bool headless) {
    WPE_Engine* engine = NULL;
    const WPE_EngineDesc desc = {.project_dir = ROOT "/game",
                                 .save_dir = ROOT "/saves",
                                 .headless = headless,
                                 .no_audio_device = true,
                                 .workers = 1,
                                 .player_display = true};
    WPE_TEST_ASSERT_OK(wpe_engine_create(&desc, &engine));
    return engine;
}

static void run(WPE_Engine* engine, const char* code) {
    WPE_TEST_ASSERT_MSG(wpe_script_host_run(wpe_engine_scripts(engine), "check", code) == WPE_RESULT_OK,
                        "script: %s", code);
}

static char* read_saved(void) {
    void* text = NULL;
    size_t size = 0;
    WPE_TEST_ASSERT_OK(wpe_file_read_all(ROOT "/saves/" WPE_DISPLAY_FILE, &text, &size));
    return text;
}

static void push_alt_enter(void) {
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.key = SDLK_RETURN;
    event.key.scancode = SDL_SCANCODE_RETURN;
    event.key.mod = SDL_KMOD_LALT;
    event.key.down = true;
    WPE_TEST_ASSERT(SDL_PushEvent(&event));
}

static void test_display_engine_window(void) {
    WPE_TEST_ASSERT(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
    WPE_TEST_ASSERT_OK(wpe_directory_create(ROOT "/game"));
    WPE_TEST_ASSERT_OK(wpe_directory_create(ROOT "/saves"));
    (void)wpe_path_remove(ROOT "/saves/" WPE_DISPLAY_FILE); /* Left by an earlier run. */
    write_text(ROOT "/game/project.json",
               "{\"format\": \"wpe.project\", \"version\": 1, \"name\": \"Display\",\n"
               " \"display\": {\"width\": 32, \"height\": 24, \"scale\": 2, \"vsync\": false}}\n");

    /* No file: the project's display. */
    WPE_Engine* engine = create(false);
    WPE_Window* window = wpe_engine_window(engine);
    WPE_TEST_ASSERT(window != NULL && !wpe_window_is_fullscreen(window));
    uint32_t width = 0, height = 0;
    wpe_window_size(window, &width, &height);
    WPE_TEST_ASSERT(width == 64 && height == 48);
    WPE_DisplaySettings d = wpe_engine_display(engine);
    WPE_TEST_ASSERT(!d.fullscreen && d.scale == 2 && !d.vsync && d.scaling == WPE_WINDOW_SCALING_INTEGER);
    run(engine, "local d = wpe.get_display()\n"
                "assert(d.window and not d.fullscreen and d.scale == 2 and d.scaling == 'integer')\n"
                "assert(d.max_scale >= 1 and d.vsync == false)\n");

    /* A script changes it: applied to the window and saved at once. */
    run(engine, "wpe.set_display({scale = 3, scaling = 'fit'})\n"
                "assert(not pcall(wpe.set_display, {scaling = 'crop'}))\n"
                "assert(not pcall(wpe.set_display, {scale = 0}))\n"
                "assert(not pcall(wpe.set_display, {scale = 2.5}))\n");
    wpe_window_size(window, &width, &height);
    WPE_TEST_ASSERT(width == 96 && height == 72);
    d = wpe_engine_display(engine);
    WPE_TEST_ASSERT(d.scale == 3 && d.scaling == WPE_WINDOW_SCALING_FIT && !d.fullscreen);
    char* saved = read_saved();
    WPE_TEST_ASSERT(strstr(saved, "\"scale\": 3") && strstr(saved, "\"fit\""));
    wpe_free(saved);

    run(engine, "wpe.set_display({fullscreen = true})\n");
    WPE_TEST_ASSERT_OK(wpe_engine_frame(engine, 0.016)); /* The switch may finish later on some systems. */
    WPE_TEST_ASSERT(wpe_window_is_fullscreen(window) && wpe_engine_display(engine).fullscreen);

    /* Alt+Enter switches back; the engine follows (and saves), and the game never sees the key. */
    push_alt_enter();
    WPE_TEST_ASSERT_OK(wpe_engine_frame(engine, 0.016));
    WPE_TEST_ASSERT_OK(wpe_engine_frame(engine, 0.016));
    WPE_TEST_ASSERT(!wpe_window_is_fullscreen(window));
    WPE_TEST_ASSERT(!wpe_engine_display(engine).fullscreen);
    WPE_TEST_ASSERT(!wpe_input_key_down(wpe_engine_input(engine), WPE_KEY_RETURN));
    saved = read_saved();
    WPE_TEST_ASSERT(strstr(saved, "\"fullscreen\": false") != NULL);
    wpe_free(saved);

    /* The scale changed while fullscreen is the window's size once it is windowed again. */
    run(engine, "wpe.set_display({fullscreen = true})\n");
    WPE_TEST_ASSERT_OK(wpe_engine_frame(engine, 0.016));
    run(engine, "wpe.set_display({scale = 1})\n");
    push_alt_enter();
    WPE_TEST_ASSERT_OK(wpe_engine_frame(engine, 0.016));
    WPE_TEST_ASSERT_OK(wpe_engine_frame(engine, 0.016));
    WPE_TEST_ASSERT(!wpe_window_is_fullscreen(window));
    wpe_window_size(window, &width, &height);
    WPE_TEST_ASSERT(width == 32 && height == 24);
    run(engine, "wpe.set_display({fullscreen = true, scale = 2})\n");
    wpe_engine_destroy(engine);

    /* The next run opens as the player left it. */
    engine = create(false);
    window = wpe_engine_window(engine);
    WPE_TEST_ASSERT(wpe_window_is_fullscreen(window));
    d = wpe_engine_display(engine);
    WPE_TEST_ASSERT(d.fullscreen && d.scale == 2 && d.scaling == WPE_WINDOW_SCALING_FIT);
    wpe_engine_destroy(engine);

    /* Without a window the settings are kept but apply to nothing; a damaged file leaves the project's. */
    quiet(true);
    write_text(ROOT "/saves/" WPE_DISPLAY_FILE,
               "{\"format\": \"wpe.display\", \"version\": 1, \"scale\": 99}");
    engine = create(true);
    quiet(false);
    WPE_TEST_ASSERT(wpe_engine_window(engine) == NULL);
    d = wpe_engine_display(engine);
    WPE_TEST_ASSERT(!d.fullscreen && d.scale == 2 && d.scaling == WPE_WINDOW_SCALING_INTEGER);
    run(engine, "wpe.set_display({fullscreen = true})\n"
                "local d = wpe.get_display()\n"
                "assert(d.fullscreen and not d.window and d.max_scale == 2)\n");
    WPE_TEST_ASSERT_OK(wpe_engine_load_display(engine)); /* The file the script just saved. */
    WPE_TEST_ASSERT(wpe_engine_display(engine).fullscreen);
    wpe_engine_destroy(engine);
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_display_file), WPE_TEST_CASE(test_display_engine_window))
