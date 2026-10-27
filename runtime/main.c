/*
 * wpe_runtime: plays a WillPowerEngine game.
 *
 * An exported game is this executable with the game's archive appended (see engine/export.h); run
 * without arguments it plays that game. During development it plays a project directory or an
 * archive instead:
 *
 *   wpe_runtime [--project <dir> | --pak <archive>] [options]
 *     --scene <path>        start with this scene instead of the project's start scene
 *     --watch               hot-reload changed scripts (project directories)
 *     --mute                silence audio
 *     --headless            no window and no audio device (automated runs)
 *     --frames <n>          quit after n frames
 *     --fixed-dt <seconds>  simulate this frame time instead of real time (reproducible runs)
 *     --screenshot <png>    save the last frame
 *     --workers <n>         render worker threads (default: one per core)
 *
 * Exit code 0 when the game ran and quit normally.
 */
#define WPE_LOG_MODULE "runtime"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/core.h"
#include "engine/engine.h"
#include "platform/platform.h"
#include "vfs/pak.h"

#define OS_PATH_MAX 4096

typedef struct Options {
    WPE_EngineDesc engine;
    WPE_RunOptions run;
    bool mute;
} Options;

static int usage(void) {
    fprintf(stderr, "usage: wpe_runtime [--project <dir> | --pak <archive>] [--scene <path>] [--watch] [--mute]\n"
                    "                   [--headless] [--frames <n>] [--fixed-dt <seconds>] [--screenshot <png>]\n"
                    "                   [--workers <n>]\n");
    return EXIT_FAILURE;
}

static bool parse_number(const char* text, double min, double max, double* out) {
    char* end = NULL;
    const double value = strtod(text, &end);
    if (end == text || *end != '\0' || !(value >= min && value <= max)) return false;
    *out = value;
    return true;
}

static bool parse_options(int argc, char** argv, Options* o) {
    *o = (Options){0};
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        const char* value = i + 1 < argc ? argv[i + 1] : NULL;
        double number = 0.0;
        if (strcmp(arg, "--watch") == 0) {
            o->run.watch = true;
        } else if (strcmp(arg, "--mute") == 0) {
            o->mute = true;
        } else if (strcmp(arg, "--headless") == 0) {
            o->engine.headless = true;
        } else if (value && strcmp(arg, "--project") == 0) {
            o->engine.project_dir = argv[++i];
        } else if (value && strcmp(arg, "--pak") == 0) {
            o->engine.pak_path = argv[++i];
        } else if (value && strcmp(arg, "--scene") == 0) {
            o->engine.scene = argv[++i];
        } else if (value && strcmp(arg, "--screenshot") == 0) {
            o->run.screenshot = argv[++i];
        } else if (value && strcmp(arg, "--frames") == 0 && parse_number(value, 1, 1e9, &number)) {
            o->run.frames = (uint32_t)number;
            ++i;
        } else if (value && strcmp(arg, "--fixed-dt") == 0 && parse_number(value, 1e-4, 1.0, &number)) {
            o->run.fixed_dt = number;
            ++i;
        } else if (value && strcmp(arg, "--workers") == 0 && parse_number(value, 1, 256, &number)) {
            o->engine.workers = (uint32_t)number;
            ++i;
        } else {
            fprintf(stderr, "invalid argument: %s\n", arg);
            return false;
        }
    }
    return !(o->engine.project_dir && o->engine.pak_path);
}

int main(int argc, char** argv) {
    wpe_platform_attach_console();
    Options o;
    if (!parse_options(argc, argv, &o)) return usage();

    /* No game given: play the one embedded in this executable. */
    static char self[OS_PATH_MAX];
    if (!o.engine.project_dir && !o.engine.pak_path) {
        if (wpe_platform_executable_path(self, sizeof self) == WPE_RESULT_OK && wpe_pak_probe(self)) {
            o.engine.pak_path = self;
        } else {
            fprintf(stderr, "this runtime carries no game: give one with --project or --pak\n");
            return usage();
        }
    }

    WPE_Engine* engine = NULL;
    WPE_Result result = wpe_engine_create(&o.engine, &engine);
    if (result == WPE_RESULT_OK) {
        if (o.mute) wpe_audio_set_volume(wpe_engine_audio(engine), WPE_AUDIO_BUS_MASTER, 0.0f);
        result = wpe_engine_run(engine, &o.run);
        if (result == WPE_RESULT_OK && o.run.screenshot) wpe_log_info("saved screenshot to %s", o.run.screenshot);
    }
    wpe_engine_destroy(engine);
    if (result != WPE_RESULT_OK) {
        char message[256];
        snprintf(message, sizeof message, "The game stopped because of an error (%s).\nSee the log for details.",
                 wpe_result_to_string(result));
        if (!o.engine.headless) wpe_platform_show_error("Game error", message);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
