/*
 * wpe_editor: the WillPowerEngine editor, driven by people (the window) and by agents (JSON commands;
 * reference in docs/editor.md).
 *
 *   wpe_editor <project-dir> [--scene <path>] [--port <n> | --no-server]
 *       Opens the editor window. Unless --no-server, it accepts commands from other processes on
 *       127.0.0.1:<port> (default 47800), one JSON object per line, one reply per line.
 *
 *   wpe_editor <project-dir> --run <commands-file> [--scene <path>]
 *   wpe_editor <project-dir> --command '<json>' [--command '<json>' ...] [--scene <path>]
 *       Headless: runs the commands (a JSON array, or one object per line) against the project and
 *       prints one reply per line. Exit code 0 when every command succeeded. Changes are kept only
 *       when a command saves them (save_scene).
 *
 *   wpe_editor --send '<json>' [--send '<json>' ...] [--port <n>]
 *       Sends commands to a running editor and prints the replies.
 *
 *   --init [--template <name>] [--name <title>] with a project directory creates a new project there first
 *       (the directory must be new or empty) from a built-in template (default: empty), then opens it as
 *       above. The name defaults to the directory's.
 */
#define WPE_LOG_MODULE "editor"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/core.h"
#include "editor/editor.h"
#include "editor/editor_ui.h"
#include "platform/net.h"
#include "platform/platform.h"

#define DEFAULT_PORT 47800
#define MAX_COMMANDS 64
#define MAX_CONNECTIONS 8
#define LINE_MAX_BYTES (1u << 20)
#define HEADLESS_WIDTH 1280u
#define HEADLESS_HEIGHT 720u

typedef struct Options {
    const char* project;
    const char* scene;
    const char* run;
    const char* commands[MAX_COMMANDS];
    uint32_t command_count;
    const char* sends[MAX_COMMANDS];
    uint32_t send_count;
    uint16_t port;
    bool server;
    bool init;
    const char* name;
    const char* template_name;
} Options;

static int usage(void) {
    fprintf(stderr,
            "usage:\n"
            "  wpe_editor <project-dir> [--scene <path>] [--port <n> | --no-server]\n"
            "  wpe_editor <new-project-dir> --init [--template <name>] [--name <title>] [any of the above]\n"
            "  wpe_editor <project-dir> --run <commands-file> [--scene <path>]\n"
            "  wpe_editor <project-dir> --command '<json>' [--command '<json>' ...]\n"
            "  wpe_editor --send '<json>' [--send '<json>' ...] [--port <n>]\n"
            "templates:");
    for (uint32_t i = 0; i < wpe_project_template_count(); ++i)
        fprintf(stderr, "%s %s", i ? "," : "", wpe_project_template_name(i));
    fputc('\n', stderr);
    return EXIT_FAILURE;
}

static bool parse_options(int argc, char** argv, Options* o) {
    *o = (Options){.port = DEFAULT_PORT, .server = true};
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        const char* value = i + 1 < argc ? argv[i + 1] : NULL;
        if (!arg) return false;
        if (strcmp(arg, "--no-server") == 0) {
            o->server = false;
        } else if (strcmp(arg, "--init") == 0) {
            o->init = true;
        } else if (value && strcmp(arg, "--name") == 0) {
            o->name = argv[++i];
        } else if (value && strcmp(arg, "--template") == 0) {
            o->template_name = argv[++i];
        } else if (value && strcmp(arg, "--scene") == 0) {
            o->scene = argv[++i];
        } else if (value && strcmp(arg, "--run") == 0) {
            o->run = argv[++i];
        } else if (value && strcmp(arg, "--command") == 0 && o->command_count < MAX_COMMANDS) {
            o->commands[o->command_count++] = argv[++i];
        } else if (value && strcmp(arg, "--send") == 0 && o->send_count < MAX_COMMANDS) {
            o->sends[o->send_count++] = argv[++i];
        } else if (value && strcmp(arg, "--port") == 0) {
            char* end = NULL;
            const unsigned long port = strtoul(value, &end, 10);
            if (*end != '\0' || port == 0 || port > 65535) return false;
            o->port = (uint16_t)port;
            ++i;
        } else if (arg[0] != '-' && !o->project) {
            o->project = arg;
        } else {
            fprintf(stderr, "invalid argument: %s\n", arg);
            return false;
        }
    }
    if (o->send_count > 0) return !o->project && !o->run && o->command_count == 0 && !o->init;
    if ((o->name || o->template_name) && !o->init) return false;
    return o->project != NULL && !(o->run && o->command_count > 0);
}

/* Runs one command and prints its reply on one line; returns whether it succeeded. */
static bool execute_and_print(WPE_Editor* editor, const WPE_JsonValue* command) {
    WPE_JsonWriter* reply = NULL;
    if (wpe_json_writer_create(&reply) != WPE_RESULT_OK) return false;
    wpe_json_writer_set_compact(reply, true);
    const bool ok = wpe_editor_execute_value(editor, command, reply) == WPE_RESULT_OK;
    const char* text = NULL;
    if (wpe_json_writer_finish(reply, &text, NULL) == WPE_RESULT_OK) fputs(text, stdout);
    fflush(stdout);
    wpe_json_writer_destroy(reply);
    return ok;
}

/* Runs a batch: a JSON array of commands, or one command object per line. */
static bool run_text(WPE_Editor* editor, const char* text, size_t size, const char* name) {
    WPE_JsonDocument* doc = NULL;
    WPE_JsonError error;
    if (wpe_json_parse(text, size, &doc, &error) == WPE_RESULT_OK) {
        const WPE_JsonValue* root = wpe_json_root(doc);
        bool ok = true;
        if (wpe_json_type(root) == WPE_JSON_ARRAY) {
            for (uint32_t i = 0; i < wpe_json_count(root); ++i)
                ok = execute_and_print(editor, wpe_json_at(root, i)) && ok;
        } else {
            ok = execute_and_print(editor, root);
        }
        wpe_json_free(doc);
        return ok;
    }
    /* Not one document: JSON lines. */
    bool ok = true;
    for (const char* line = text; line < text + size;) {
        const char* end = memchr(line, '\n', (size_t)(text + size - line));
        const size_t length = end ? (size_t)(end - line) : (size_t)(text + size - line);
        bool blank = true;
        for (size_t i = 0; i < length && blank; ++i)
            blank = line[i] == ' ' || line[i] == '\t' || line[i] == '\r';
        if (!blank) {
            WPE_JsonDocument* command = NULL;
            if (wpe_json_parse(line, length, &command, &error) != WPE_RESULT_OK) {
                fprintf(stderr, "%s: invalid JSON (%s)\n", name, error.message);
                ok = false;
            } else {
                ok = execute_and_print(editor, wpe_json_root(command)) && ok;
                wpe_json_free(command);
            }
        }
        line += length + 1;
    }
    return ok;
}

/* ---- Editor screenshots (the "editor" target of the screenshot command) ---------------------------- */

typedef struct Shot {
    WPE_EditorUi* ui;
    WPE_Renderer* renderer; /* Headless: the software UI's framebuffer. NULL: the window's last frame. */
} Shot;

static WPE_Result save_editor_image(void* user_data, const char* os_path) {
    Shot* shot = user_data;
    if (!shot->renderer) {
        /* The window: the frame on screen, read back from the GPU. */
        WPE_Image image;
        WPE_Result result = wpe_editor_ui_capture(shot->ui, &image);
        if (result == WPE_RESULT_OK)
            result = wpe_image_save_png(os_path, image.width, image.height, image.pixels);
        wpe_image_free(&image);
        return result;
    }
    /* Headless: a few frames first; windows appear and dock in the first, tab bars settle after. */
    for (int i = 0; i < 4; ++i) {
        const WPE_Result result = wpe_editor_ui_frame(shot->ui, shot->renderer, 1.0f / 60.0f);
        if (result != WPE_RESULT_OK) return result;
    }
    const WPE_Framebuffer* fb = wpe_renderer_framebuffer(shot->renderer);
    return wpe_image_save_png(os_path, fb->width, fb->height, fb->color);
}

static int run_headless(const Options* o) {
    WPE_Editor* editor = NULL;
    WPE_EditorUi* ui = NULL;
    WPE_Renderer* renderer = NULL;
    WPE_Result result = wpe_editor_create(
        &(WPE_EditorDesc){.project_dir = o->project, .scene = o->scene, .no_audio_device = true}, &editor);
    if (result == WPE_RESULT_OK) result = wpe_editor_ui_create(editor, NULL, &ui);
    if (result == WPE_RESULT_OK)
        result = wpe_renderer_create(&(WPE_RendererDesc){.width = HEADLESS_WIDTH, .height = HEADLESS_HEIGHT},
                                     &renderer);
    Shot shot = {.ui = ui, .renderer = renderer};
    if (result == WPE_RESULT_OK) wpe_editor_set_screenshot_hook(editor, save_editor_image, &shot);
    bool ok = result == WPE_RESULT_OK;
    if (ok && o->run) {
        void* data = NULL;
        size_t size = 0;
        ok = wpe_file_read_all(o->run, &data, &size) == WPE_RESULT_OK && run_text(editor, data, size, o->run);
        wpe_free(data);
    }
    for (uint32_t i = 0; ok && i < o->command_count; ++i)
        ok = run_text(editor, o->commands[i], strlen(o->commands[i]), "--command") && ok;
    wpe_renderer_destroy(renderer);
    wpe_editor_ui_destroy(ui);
    wpe_editor_destroy(editor);
    if (result != WPE_RESULT_OK)
        fprintf(stderr, "cannot open '%s': %s\n", o->project, wpe_result_to_string(result));
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

/* ---- Client --------------------------------------------------------------------------------------- */

static int run_client(const Options* o) {
    WPE_Socket* socket = NULL;
    if (wpe_net_connect(o->port, 120000, &socket) != WPE_RESULT_OK) {
        fprintf(stderr, "no editor is listening on port %u\n", o->port);
        return EXIT_FAILURE;
    }
    bool ok = true;
    char* line = wpe_alloc(LINE_MAX_BYTES);
    for (uint32_t i = 0; line && i < o->send_count; ++i) {
        /* One line per command: JSON allows line breaks only as whitespace, so they become spaces. */
        const size_t length = strlen(o->sends[i]);
        if (length + 2 > LINE_MAX_BYTES) {
            ok = false;
            break;
        }
        for (size_t c = 0; c < length; ++c)
            line[c] = o->sends[i][c] == '\n' || o->sends[i][c] == '\r' ? ' ' : o->sends[i][c];
        line[length] = '\n';
        if (wpe_net_send(socket, line, length + 1) != WPE_RESULT_OK) {
            ok = false;
            break;
        }
        size_t got = 0;
        while (got == 0 || line[got - 1] != '\n') {
            size_t n = 0;
            if (got + 1 >= LINE_MAX_BYTES ||
                wpe_net_receive(socket, line + got, LINE_MAX_BYTES - 1 - got, &n) != WPE_RESULT_OK) {
                fprintf(stderr, "the editor did not answer\n");
                ok = false;
                break;
            }
            got += n;
        }
        if (!ok) break;
        fwrite(line, 1, got, stdout);
        ok = strstr(line, "\"ok\": true") != NULL && ok;
    }
    wpe_free(line);
    wpe_net_close(socket);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

/* ---- Window --------------------------------------------------------------------------------------- */

typedef struct Connection {
    WPE_Socket* socket;
    char* buffer;
    size_t used;
} Connection;

/* Serves queued commands from connected clients (one JSON object per line). */
static void serve(WPE_Editor* editor, WPE_Socket* listener, Connection* connections) {
    WPE_Socket* incoming = NULL;
    while (wpe_net_accept(listener, &incoming) == WPE_RESULT_OK) {
        Connection* free_slot = NULL;
        for (uint32_t i = 0; i < MAX_CONNECTIONS && !free_slot; ++i)
            free_slot = connections[i].socket ? NULL : &connections[i];
        char* buffer = free_slot ? wpe_alloc(LINE_MAX_BYTES) : NULL;
        if (!buffer) {
            wpe_net_close(incoming);
            continue;
        }
        *free_slot = (Connection){.socket = incoming, .buffer = buffer};
    }
    for (uint32_t i = 0; i < MAX_CONNECTIONS; ++i) {
        Connection* c = &connections[i];
        if (!c->socket) continue;
        size_t n = 0;
        const WPE_Result received =
            wpe_net_receive(c->socket, c->buffer + c->used, LINE_MAX_BYTES - c->used, &n);
        c->used += n;
        char* newline = c->used ? memchr(c->buffer, '\n', c->used) : NULL;
        while (newline) {
            const size_t length = (size_t)(newline - c->buffer);
            WPE_JsonWriter* reply = NULL;
            if (wpe_json_writer_create(&reply) == WPE_RESULT_OK) {
                wpe_json_writer_set_compact(reply, true);
                wpe_editor_execute(editor, c->buffer, length, reply);
                const char* text = NULL;
                size_t size = 0;
                if (wpe_json_writer_finish(reply, &text, &size) == WPE_RESULT_OK)
                    wpe_net_send(c->socket, text, size);
                wpe_json_writer_destroy(reply);
            }
            memmove(c->buffer, newline + 1, c->used - length - 1);
            c->used -= length + 1;
            newline = c->used ? memchr(c->buffer, '\n', c->used) : NULL;
        }
        /* Closed, failed, or a line longer than the buffer: drop the connection. */
        if (received != WPE_RESULT_OK || c->used == LINE_MAX_BYTES) {
            wpe_net_close(c->socket);
            wpe_free(c->buffer);
            *c = (Connection){0};
        }
    }
}

static void on_event(const WPE_PlatformEvent* event, void* user_data) {
    wpe_editor_ui_handle_event(user_data, event);
}

static int run_window(const Options* o) {
    WPE_Editor* editor = NULL;
    WPE_EditorUi* ui = NULL;
    WPE_Window* window = NULL;
    WPE_Socket* listener = NULL;
    Connection connections[MAX_CONNECTIONS] = {0};
    char title[512];
    snprintf(title, sizeof title, "WillPowerEngine Editor - %s", o->project);
    WPE_Result result =
        wpe_editor_create(&(WPE_EditorDesc){.project_dir = o->project, .scene = o->scene}, &editor);
    if (result == WPE_RESULT_OK)
        result = wpe_window_create(
            &(WPE_WindowDesc){.title = title, .width = 1600, .height = 900, .resizable = true, .vsync = true},
            &window);
    /* The UI is drawn on the GPU through the window's renderer; only the game view is software. */
    if (result == WPE_RESULT_OK) result = wpe_editor_ui_create(editor, wpe_window_renderer(window), &ui);
    if (result == WPE_RESULT_OK && o->server && wpe_net_listen(o->port, &listener) == WPE_RESULT_OK)
        wpe_log_info("accepting commands on 127.0.0.1:%u", o->port);
    if (result != WPE_RESULT_OK) {
        fprintf(stderr, "the editor failed to start: %s\n", wpe_result_to_string(result));
    } else {
        wpe_window_set_event_handler(window, on_event, ui);
        wpe_window_set_text_input(window, true);
        Shot shot = {.ui = ui};
        wpe_editor_set_screenshot_hook(editor, save_editor_image, &shot);
        bool captured = false;
        double previous = wpe_platform_time_seconds();
        while (result == WPE_RESULT_OK) {
            if (!wpe_window_poll_events(window)) {
                /* The close button: the UI asks about unsaved changes before quitting. */
                wpe_window_cancel_close(window);
                wpe_editor_ui_request_quit(ui);
            }
            if (wpe_editor_ui_quit_requested(ui) || wpe_editor_quit_requested(editor)) break;
            const double now = wpe_platform_time_seconds();
            const float dt = (float)(now - previous);
            previous = now;
            if (listener) serve(editor, listener, connections);
            result = wpe_editor_ui_frame(ui, NULL, dt);
            const bool want_capture = wpe_editor_ui_mouse_captured(ui);
            if (want_capture != captured &&
                wpe_window_set_relative_mouse(window, want_capture) == WPE_RESULT_OK)
                captured = want_capture;
        }
        if (result != WPE_RESULT_OK)
            fprintf(stderr, "the editor stopped: %s\n", wpe_result_to_string(result));
    }
    for (uint32_t i = 0; i < MAX_CONNECTIONS; ++i) {
        wpe_net_close(connections[i].socket);
        wpe_free(connections[i].buffer);
    }
    wpe_net_close(listener);
    wpe_editor_ui_destroy(ui);
    wpe_editor_destroy(editor);
    wpe_window_destroy(window);
    return result == WPE_RESULT_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}

/* --init: a new project in the project directory, named after it unless --name says otherwise. */
static bool init_project(const Options* o) {
    char name[WPE_PROJECT_NAME_MAX];
    if (o->name) {
        snprintf(name, sizeof name, "%s", o->name);
    } else {
        /* The last path component, ignoring trailing separators. */
        size_t end = strlen(o->project);
        while (end > 1 && (o->project[end - 1] == '/' || o->project[end - 1] == '\\'))
            --end;
        size_t start = end;
        while (start > 0 && o->project[start - 1] != '/' && o->project[start - 1] != '\\')
            --start;
        const size_t length = end - start < sizeof name - 1 ? end - start : sizeof name - 1;
        memcpy(name, o->project + start, length);
        name[length] = '\0';
    }
    const WPE_Result result = wpe_project_create(o->project, name[0] ? name : "New game", o->template_name);
    if (result != WPE_RESULT_OK)
        fprintf(stderr, "cannot create the project: %s\n", wpe_result_to_string(result));
    return result == WPE_RESULT_OK;
}

int main(int argc, char** argv) {
    wpe_platform_attach_console();
    Options o;
    if (!parse_options(argc, argv, &o)) return usage();
    if (o.send_count > 0) return run_client(&o);
    if (o.init && !init_project(&o)) return EXIT_FAILURE;
    if (o.run || o.command_count > 0) return run_headless(&o);
    return run_window(&o);
}
