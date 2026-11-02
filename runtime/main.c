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
 *     --headless            no window and no audio device (automated runs, dedicated servers)
 *     --frames <n>          quit after n frames
 *     --fixed-dt <seconds>  simulate this frame time instead of real time (reproducible runs)
 *     --max-fps <n>         sleep so frames don't come faster (headless hosts default to 60)
 *     --screenshot <png>    save the last frame
 *     --workers <n>         render worker threads (default: one per core)
 *     --save-dir <dir>      keep saved data in this existing directory instead of the player's
 *
 *   Multiplayer (docs/networking.md):
 *     --host [port]         host a game (default: the project's port); with --headless, a dedicated server
 *     --connect <address>   join the game at "a.b.c.d[:port]" or "name[:port]"
 *     --name <player>       the name the others see
 *     --local               this machine only (127.0.0.1): no firewall prompt, invisible on the LAN
 *     --net-sim <settings>  pretend the network is bad: "latency=120,jitter=30,loss=5" (ms, ms, percent)
 *     --net-stats           show the players, pings and traffic in the corner
 *
 * The network settings also apply to games that scripts host or join (wpe.net_host, wpe.net_join).
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
#define HEADLESS_HOST_FPS 60u

typedef struct Options {
    WPE_EngineDesc engine;
    WPE_RunOptions run;
    bool mute;
    bool host;
    WPE_EngineNetDesc net; /* join, player_name, port, local_only, conditions */
    bool net_stats;
} Options;

static int usage(void) {
    fprintf(stderr,
            "usage: wpe_runtime [--project <dir> | --pak <archive>] [--scene <path>] [--watch] [--mute]\n"
            "                   [--headless] [--frames <n>] [--fixed-dt <seconds>] [--max-fps <n>]\n"
            "                   [--screenshot <png>] [--workers <n>] [--save-dir <dir>]\n"
            "                   [--host [port] | --connect <address[:port]>] [--name <player>] [--local]\n"
            "                   [--net-sim latency=<ms>,jitter=<ms>,loss=<percent>] [--net-stats]\n");
    return EXIT_FAILURE;
}

static bool parse_number(const char* text, double min, double max, double* out) {
    char* end = NULL;
    const double value = strtod(text, &end);
    if (end == text || *end != '\0' || !(value >= min && value <= max)) return false;
    *out = value;
    return true;
}

/* "latency=120,jitter=30,loss=5": any of the three, in any order. */
static bool parse_net_sim(const char* text, WPE_NetConditions* out) {
    char buffer[128];
    if (strlen(text) >= sizeof buffer) return false;
    memcpy(buffer, text, strlen(text) + 1);
    for (char* item = buffer; item;) {
        char* comma = strchr(item, ',');
        if (comma) *comma = '\0';
        char* equals = strchr(item, '=');
        if (!equals) return false;
        *equals = '\0';
        double value = 0.0;
        if (strcmp(item, "latency") == 0 && parse_number(equals + 1, 0, 5000, &value)) {
            out->latency_ms = (uint32_t)value;
        } else if (strcmp(item, "jitter") == 0 && parse_number(equals + 1, 0, 5000, &value)) {
            out->jitter_ms = (uint32_t)value;
        } else if (strcmp(item, "loss") == 0 && parse_number(equals + 1, 0, 90, &value)) {
            out->loss = (float)(value / 100.0);
        } else {
            return false;
        }
        item = comma ? comma + 1 : NULL;
    }
    return true;
}

static bool parse_options(int argc, char** argv, Options* o) {
    *o = (Options){0};
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        const char* value = i + 1 < argc ? argv[i + 1] : NULL;
        double number = 0.0;
        if (!arg) return false; /* argv ends with NULL at argc; nothing earlier is NULL. */
        if (strcmp(arg, "--watch") == 0) {
            o->run.watch = true;
        } else if (strcmp(arg, "--mute") == 0) {
            o->mute = true;
        } else if (strcmp(arg, "--headless") == 0) {
            o->engine.headless = true;
        } else if (strcmp(arg, "--host") == 0) {
            o->host = true;
            if (value && value[0] != '-') { /* The port is optional. */
                if (!parse_number(value, 1, 65535, &number)) {
                    fprintf(stderr, "invalid port: %s\n", value);
                    return false;
                }
                o->net.port = (uint16_t)number;
                ++i;
            }
        } else if (strcmp(arg, "--local") == 0) {
            o->net.local_only = true;
        } else if (strcmp(arg, "--net-stats") == 0) {
            o->net_stats = true;
        } else if (value && strcmp(arg, "--connect") == 0) {
            o->net.join = argv[++i];
        } else if (value && strcmp(arg, "--name") == 0) {
            o->net.player_name = argv[++i];
        } else if (value && strcmp(arg, "--net-sim") == 0) {
            if (!parse_net_sim(value, &o->net.conditions)) {
                fprintf(stderr, "invalid --net-sim: %s (try latency=120,jitter=30,loss=5)\n", value);
                return false;
            }
            ++i;
        } else if (value && strcmp(arg, "--project") == 0) {
            o->engine.project_dir = argv[++i];
        } else if (value && strcmp(arg, "--pak") == 0) {
            o->engine.pak_path = argv[++i];
        } else if (value && strcmp(arg, "--scene") == 0) {
            o->engine.scene = argv[++i];
        } else if (value && strcmp(arg, "--screenshot") == 0) {
            o->run.screenshot = argv[++i];
        } else if (value && strcmp(arg, "--save-dir") == 0) {
            o->engine.save_dir = argv[++i];
        } else if (value && strcmp(arg, "--frames") == 0 && parse_number(value, 1, 1e9, &number)) {
            o->run.frames = (uint32_t)number;
            ++i;
        } else if (value && strcmp(arg, "--fixed-dt") == 0 && parse_number(value, 1e-4, 1.0, &number)) {
            o->run.fixed_dt = number;
            ++i;
        } else if (value && strcmp(arg, "--max-fps") == 0 && parse_number(value, 1, 1000, &number)) {
            o->run.max_fps = (uint32_t)number;
            ++i;
        } else if (value && strcmp(arg, "--workers") == 0 && parse_number(value, 1, 256, &number)) {
            o->engine.workers = (uint32_t)number;
            ++i;
        } else {
            fprintf(stderr, "invalid argument: %s\n", arg);
            return false;
        }
    }
    if (o->host && o->net.join) {
        fprintf(stderr, "--host and --connect can't be used together\n");
        return false;
    }
    /* A dedicated server has no window to pace it: without a limit it would use a whole core. */
    if (o->host && o->engine.headless && o->run.max_fps == 0 && o->run.fixed_dt == 0.0)
        o->run.max_fps = HEADLESS_HOST_FPS;
    return !(o->engine.project_dir && o->engine.pak_path);
}

/* --connect: whether this side got into the game (it may have left since). A game that refused it, or a
 * host that never answered, makes the run fail, so scripts and tests can tell. */
static bool joined_or_left(const WPE_Engine* engine) {
    const WPE_Session* session = wpe_engine_session(engine);
    if (!session || wpe_session_local_player(session) != WPE_SESSION_NO_PLAYER) return true;
    const uint32_t reason = wpe_session_end_reason(session);
    switch (reason) {
    case 0:
        wpe_log_error("the game did not answer in time");
        return false;
    case WPE_NET_REASON_CONNECT_FAILED:
    case WPE_SESSION_REASON_VERSION:
    case WPE_SESSION_REASON_WRONG_GAME:
    case WPE_SESSION_REASON_FULL:
    case WPE_SESSION_REASON_NOT_ACCEPTING:
    case WPE_SESSION_REASON_PROTOCOL:
        wpe_log_error("could not join the game: %s", wpe_session_reason_name(reason));
        return false;
    default:
        return true; /* It played, then the game ended (closed, timed out, kicked). */
    }
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
        wpe_engine_set_net_defaults(engine, &o.net); /* Games the scripts start get the same settings. */
        if (o.net_stats) {
            WPE_EngineDebugDraw debug = wpe_engine_debug_draw(engine);
            debug.network = true;
            wpe_engine_set_debug_draw(engine, &debug);
        }
        if (o.host || o.net.join) result = wpe_engine_net_start(engine, &o.net);
    }
    if (result == WPE_RESULT_OK) {
        result = wpe_engine_run(engine, &o.run);
        if (result == WPE_RESULT_OK && o.run.screenshot)
            wpe_log_info("saved screenshot to %s", o.run.screenshot);
    }
    if (result == WPE_RESULT_OK && o.net.join && !joined_or_left(engine)) result = WPE_RESULT_ERROR_NOT_FOUND;
    wpe_engine_destroy(engine);
    if (result != WPE_RESULT_OK) {
        char message[256];
        snprintf(message, sizeof message,
                 "The game stopped because of an error (%s).\nSee the log for details.",
                 wpe_result_to_string(result));
        if (!o.engine.headless) wpe_platform_show_error("Game error", message);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
