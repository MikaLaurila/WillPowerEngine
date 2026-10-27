#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L /* readlink, chmod */
#endif
#define WPE_LOG_MODULE "platform"
#include "platform/platform.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "core/alloc.h"
#include "core/debug.h"
#include "core/log.h"

struct WPE_Window {
    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_Texture* texture;
    uint32_t texture_width;
    uint32_t texture_height;
    bool quit_requested;
    WPE_PlatformEventFn event_fn;
    void* event_user_data;
};

static_assert(sizeof(WPE_Color) == 4, "presenting assumes 32-bit packed pixels");

/* ---- Window --------------------------------------------------------------------------------------- */

WPE_Result wpe_window_create(const WPE_WindowDesc* desc, WPE_Window** out_window) {
    if (!desc || !out_window) return WPE_RESULT_ERROR_NULL_ARG;
    *out_window = NULL;
    if (!desc->title || desc->width == 0 || desc->height == 0 || desc->width > INT32_MAX ||
        desc->height > INT32_MAX) {
        return WPE_RESULT_ERROR_INVALID_ARG;
    }

    /* SDL reference-counts subsystem initialization, so each window holds one reference. */
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        wpe_log_error("SDL video initialization failed: %s", SDL_GetError());
        return WPE_RESULT_ERROR;
    }

    WPE_Window* window = wpe_alloc_zeroed(1, sizeof *window);
    if (!window) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }

    const SDL_WindowFlags flags =
        (desc->resizable ? SDL_WINDOW_RESIZABLE : 0) | (desc->fullscreen ? SDL_WINDOW_FULLSCREEN : 0);
    if (!SDL_CreateWindowAndRenderer(desc->title, (int)desc->width, (int)desc->height, flags, &window->window,
                                     &window->renderer)) {
        wpe_log_error("window creation failed: %s", SDL_GetError());
        wpe_free(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return WPE_RESULT_ERROR;
    }
    if (desc->vsync && !SDL_SetRenderVSync(window->renderer, 1)) {
        wpe_log_warn("vsync unavailable: %s", SDL_GetError());
    }

    *out_window = window;
    return WPE_RESULT_OK;
}

void wpe_window_destroy(WPE_Window* window) {
    if (!window) return;
    if (window->texture) SDL_DestroyTexture(window->texture);
    SDL_DestroyRenderer(window->renderer);
    SDL_DestroyWindow(window->window);
    wpe_free(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

bool wpe_window_poll_events(WPE_Window* window) {
    if (!wpe_check(window != NULL)) return false;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            window->quit_requested = true;
        }
        /* Mouse coordinates in framebuffer pixels (undoing the letterboxed scaling). */
        SDL_ConvertEventToRenderCoordinates(window->renderer, &event);
        if (window->event_fn) window->event_fn((const WPE_PlatformEvent*)&event, window->event_user_data);
    }
    return !window->quit_requested;
}

void wpe_window_cancel_close(WPE_Window* window) {
    if (wpe_check(window != NULL)) window->quit_requested = false;
}

void wpe_window_set_event_handler(WPE_Window* window, WPE_PlatformEventFn fn, void* user_data) {
    if (!wpe_check(window != NULL)) return;
    window->event_fn = fn;
    window->event_user_data = user_data;
}

bool wpe_platform_poll_events(WPE_PlatformEventFn fn, void* user_data) {
    bool quit = false;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) quit = true;
        if (fn) fn((const WPE_PlatformEvent*)&event, user_data);
    }
    return !quit;
}

WPE_Result wpe_window_set_relative_mouse(WPE_Window* window, bool enabled) {
    if (!window) return WPE_RESULT_ERROR_NULL_ARG;
    if (SDL_SetWindowRelativeMouseMode(window->window, enabled)) return WPE_RESULT_OK;
    wpe_log_error("cannot %s relative mouse mode: %s", enabled ? "enable" : "disable", SDL_GetError());
    return WPE_RESULT_ERROR;
}

void wpe_window_size(const WPE_Window* window, uint32_t* out_width, uint32_t* out_height) {
    int w = 0, h = 0;
    if (window && !SDL_GetWindowSizeInPixels(window->window, &w, &h)) w = h = 0;
    if (out_width) *out_width = w > 0 ? (uint32_t)w : 0;
    if (out_height) *out_height = h > 0 ? (uint32_t)h : 0;
}

WPE_Result wpe_window_set_text_input(WPE_Window* window, bool enabled) {
    if (!window) return WPE_RESULT_ERROR_NULL_ARG;
    const bool ok = enabled ? SDL_StartTextInput(window->window) : SDL_StopTextInput(window->window);
    if (ok) return WPE_RESULT_OK;
    wpe_log_error("cannot %s text input: %s", enabled ? "start" : "stop", SDL_GetError());
    return WPE_RESULT_ERROR;
}

void wpe_window_set_title(WPE_Window* window, const char* title) {
    if (!wpe_check(window != NULL && title != NULL)) return;
    if (!SDL_SetWindowTitle(window->window, title))
        wpe_log_warn("cannot set the window title: %s", SDL_GetError());
}

WPE_Result wpe_window_present(WPE_Window* window, const WPE_Framebuffer* framebuffer) {
    if (!window || !framebuffer || !framebuffer->color) return WPE_RESULT_ERROR_NULL_ARG;

    if (!window->texture || window->texture_width != framebuffer->width ||
        window->texture_height != framebuffer->height) {
        if (window->texture) SDL_DestroyTexture(window->texture);
        window->texture =
            SDL_CreateTexture(window->renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING,
                              (int)framebuffer->width, (int)framebuffer->height);
        if (!window->texture) {
            wpe_log_error("texture creation failed: %s", SDL_GetError());
            return WPE_RESULT_ERROR;
        }
        SDL_SetTextureScaleMode(window->texture, SDL_SCALEMODE_NEAREST);
        SDL_SetRenderLogicalPresentation(window->renderer, (int)framebuffer->width, (int)framebuffer->height,
                                         SDL_LOGICAL_PRESENTATION_LETTERBOX);
        window->texture_width = framebuffer->width;
        window->texture_height = framebuffer->height;
    }

    if (!SDL_UpdateTexture(window->texture, NULL, framebuffer->color,
                           (int)(framebuffer->width * sizeof(WPE_Color))) ||
        !SDL_SetRenderDrawColor(window->renderer, 0, 0, 0, 255) || !SDL_RenderClear(window->renderer) ||
        !SDL_RenderTexture(window->renderer, window->texture, NULL, NULL) ||
        !SDL_RenderPresent(window->renderer)) {
        wpe_log_error("present failed: %s", SDL_GetError());
        return WPE_RESULT_ERROR;
    }
    return WPE_RESULT_OK;
}

WPE_PlatformRenderer* wpe_window_renderer(WPE_Window* window) {
    return window ? (WPE_PlatformRenderer*)window->renderer : NULL;
}

double wpe_platform_time_seconds(void) {
    return (double)SDL_GetTicksNS() / 1e9;
}

/* ---- Images --------------------------------------------------------------------------------------- */

WPE_Result wpe_image_save_png(const char* path, uint32_t width, uint32_t height, const WPE_Color* pixels) {
    if (!path || !pixels) return WPE_RESULT_ERROR_NULL_ARG;
    if (width == 0 || height == 0 || width > INT32_MAX / 4 || height > INT32_MAX)
        return WPE_RESULT_ERROR_INVALID_ARG;

    /* SDL only reads from the surface while saving; the cast does not lead to writes. */
    SDL_Surface* surface = SDL_CreateSurfaceFrom((int)width, (int)height, SDL_PIXELFORMAT_ARGB8888,
                                                 (void*)pixels, (int)(width * sizeof(WPE_Color)));
    if (!surface) {
        wpe_log_error("cannot wrap pixels for PNG export: %s", SDL_GetError());
        return WPE_RESULT_ERROR;
    }
    const bool saved = SDL_SavePNG(surface, path);
    SDL_DestroySurface(surface);
    if (!saved) {
        wpe_log_error("failed to write PNG '%s': %s", path, SDL_GetError());
        return WPE_RESULT_ERROR_IO;
    }
    return WPE_RESULT_OK;
}

WPE_Result wpe_image_load_png(const char* path, WPE_Image* out_image) {
    if (!path || !out_image) return WPE_RESULT_ERROR_NULL_ARG;
    *out_image = (WPE_Image){0};

    SDL_Surface* loaded = SDL_LoadPNG(path);
    if (!loaded) {
        wpe_log_error("failed to read PNG '%s': %s", path, SDL_GetError());
        return WPE_RESULT_ERROR_IO;
    }
    SDL_Surface* converted = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_ARGB8888);
    SDL_DestroySurface(loaded);
    if (!converted) {
        wpe_log_error("failed to convert PNG '%s': %s", path, SDL_GetError());
        return WPE_RESULT_ERROR_IO;
    }

    const uint32_t width = (uint32_t)converted->w;
    const uint32_t height = (uint32_t)converted->h;
    WPE_Color* pixels = wpe_alloc((size_t)width * height * sizeof *pixels);
    if (!pixels) {
        SDL_DestroySurface(converted);
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }
    for (uint32_t y = 0; y < height; ++y) {
        memcpy(pixels + (size_t)y * width,
               (const unsigned char*)converted->pixels + (size_t)y * (size_t)converted->pitch,
               width * sizeof *pixels);
    }
    SDL_DestroySurface(converted);

    *out_image = (WPE_Image){.width = width, .height = height, .pixels = pixels};
    return WPE_RESULT_OK;
}

/* ---- Files ---------------------------------------------------------------------------------------- */

WPE_Result wpe_file_read_all(const char* path, void** out_data, size_t* out_size) {
    if (!path || !out_data || !out_size) return WPE_RESULT_ERROR_NULL_ARG;
    *out_data = NULL;
    *out_size = 0;

    SDL_IOStream* stream = SDL_IOFromFile(path, "rb");
    if (!stream) {
        wpe_log_error("cannot open '%s': %s", path, SDL_GetError());
        return WPE_RESULT_ERROR_IO;
    }
    const Sint64 size = SDL_GetIOSize(stream);
    if (size < 0 || (uint64_t)size >= SIZE_MAX) {
        wpe_log_error("cannot determine size of '%s': %s", path, SDL_GetError());
        SDL_CloseIO(stream);
        return WPE_RESULT_ERROR_IO;
    }

    unsigned char* data = wpe_alloc((size_t)size + 1);
    if (!data) {
        SDL_CloseIO(stream);
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }
    const size_t read = size > 0 ? SDL_ReadIO(stream, data, (size_t)size) : 0;
    const bool closed = SDL_CloseIO(stream);
    if (read != (size_t)size || !closed) {
        wpe_log_error("failed to read '%s': %s", path, SDL_GetError());
        wpe_free(data);
        return WPE_RESULT_ERROR_IO;
    }
    data[size] = '\0';
    *out_data = data;
    *out_size = (size_t)size;
    return WPE_RESULT_OK;
}

WPE_Result wpe_file_modified_time(const char* path, int64_t* out_time) {
    if (!path || !out_time) return WPE_RESULT_ERROR_NULL_ARG;
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info) || info.type != SDL_PATHTYPE_FILE) return WPE_RESULT_ERROR_NOT_FOUND;
    *out_time = (int64_t)info.modify_time;
    return WPE_RESULT_OK;
}

struct WPE_File {
    SDL_IOStream* io;
};

WPE_Result wpe_file_open(const char* path, bool write, WPE_File** out_file) {
    if (!path || !out_file) return WPE_RESULT_ERROR_NULL_ARG;
    *out_file = NULL;
    if (!write) {
        SDL_PathInfo info;
        if (!SDL_GetPathInfo(path, &info) || info.type != SDL_PATHTYPE_FILE)
            return WPE_RESULT_ERROR_NOT_FOUND;
    }
    WPE_File* file = wpe_alloc(sizeof *file);
    if (!file) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    file->io = SDL_IOFromFile(path, write ? "wb" : "rb");
    if (!file->io) {
        wpe_log_error("cannot open '%s' for %s: %s", path, write ? "writing" : "reading", SDL_GetError());
        wpe_free(file);
        return WPE_RESULT_ERROR_IO;
    }
    *out_file = file;
    return WPE_RESULT_OK;
}

WPE_Result wpe_file_close(WPE_File* file) {
    if (!file) return WPE_RESULT_OK;
    const bool closed = SDL_CloseIO(file->io);
    wpe_free(file);
    if (closed) return WPE_RESULT_OK;
    wpe_log_error("failed to close a file: %s", SDL_GetError());
    return WPE_RESULT_ERROR_IO;
}

size_t wpe_file_read(WPE_File* file, void* buffer, size_t size) {
    if (!file || !buffer || size == 0) return 0;
    return SDL_ReadIO(file->io, buffer, size);
}

WPE_Result wpe_file_write(WPE_File* file, const void* data, size_t size) {
    if (!file || (!data && size > 0)) return WPE_RESULT_ERROR_NULL_ARG;
    if (size == 0) return WPE_RESULT_OK;
    if (SDL_WriteIO(file->io, data, size) == size) return WPE_RESULT_OK;
    wpe_log_error("failed to write %zu bytes: %s", size, SDL_GetError());
    return WPE_RESULT_ERROR_IO;
}

WPE_Result wpe_file_seek(WPE_File* file, uint64_t offset) {
    if (!file) return WPE_RESULT_ERROR_NULL_ARG;
    if (offset > (uint64_t)INT64_MAX) return WPE_RESULT_ERROR_IO;
    return SDL_SeekIO(file->io, (Sint64)offset, SDL_IO_SEEK_SET) >= 0 ? WPE_RESULT_OK : WPE_RESULT_ERROR_IO;
}

uint64_t wpe_file_tell(WPE_File* file) {
    const Sint64 offset = file ? SDL_TellIO(file->io) : -1;
    return offset < 0 ? UINT64_MAX : (uint64_t)offset;
}

uint64_t wpe_file_size(WPE_File* file) {
    const Sint64 size = file ? SDL_GetIOSize(file->io) : -1;
    return size < 0 ? UINT64_MAX : (uint64_t)size;
}

/* ---- Directories ---------------------------------------------------------------------------------- */

#define WPE__WALK_PATH_MAX 1024u
#define WPE__WALK_DEPTH_MAX 32u

typedef struct wpe__Walk {
    WPE_DirectoryWalkFn fn;
    void* user_data;
    char relative[WPE__WALK_PATH_MAX]; /* Path of the directory being enumerated, relative to the root. */
    size_t relative_length;
    uint32_t depth;
    WPE_Result result;
    bool stopped;
} wpe__Walk;

static SDL_EnumerationResult wpe__walk_entry(void* user_data, const char* dirname, const char* fname) {
    wpe__Walk* walk = user_data;
    if (strcmp(fname, ".") == 0 || strcmp(fname, "..") == 0) return SDL_ENUM_CONTINUE;

    /* SDL passes `dirname` with a trailing separator. */
    char full[WPE__WALK_PATH_MAX];
    const size_t name_length = strlen(fname);
    const int written = SDL_snprintf(full, sizeof full, "%s%s", dirname, fname);
    if (written < 0 || (size_t)written >= sizeof full ||
        walk->relative_length + name_length + 2 > sizeof walk->relative) {
        wpe_log_error("path too long while walking '%s'", dirname);
        walk->result = WPE_RESULT_ERROR_CAPACITY;
        return SDL_ENUM_FAILURE;
    }
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(full, &info)) return SDL_ENUM_CONTINUE; /* Vanished meanwhile. */

    const size_t saved = walk->relative_length;
    memcpy(walk->relative + saved, fname, name_length + 1);
    walk->relative_length = saved + name_length;
    SDL_EnumerationResult next = SDL_ENUM_CONTINUE;
    if (info.type == SDL_PATHTYPE_FILE) {
        if (!walk->fn(walk->relative, walk->user_data)) {
            walk->stopped = true;
            next = SDL_ENUM_SUCCESS;
        }
    } else if (info.type == SDL_PATHTYPE_DIRECTORY) {
        if (walk->depth + 1 > WPE__WALK_DEPTH_MAX) {
            wpe_log_error("directory nesting deeper than %u levels at '%s'", WPE__WALK_DEPTH_MAX, full);
            walk->result = WPE_RESULT_ERROR_CAPACITY;
            next = SDL_ENUM_FAILURE;
        } else {
            walk->relative[walk->relative_length++] = '/';
            walk->relative[walk->relative_length] = '\0';
            ++walk->depth;
            const bool ok = SDL_EnumerateDirectory(full, wpe__walk_entry, walk);
            --walk->depth;
            if (walk->stopped) {
                next = SDL_ENUM_SUCCESS;
            } else if (!ok) {
                if (walk->result == WPE_RESULT_OK) {
                    wpe_log_error("cannot list '%s': %s", full, SDL_GetError());
                    walk->result = WPE_RESULT_ERROR_IO;
                }
                next = SDL_ENUM_FAILURE;
            }
        }
    }
    walk->relative_length = saved;
    walk->relative[saved] = '\0';
    return next;
}

WPE_Result wpe_directory_walk(const char* directory, WPE_DirectoryWalkFn fn, void* user_data) {
    if (!directory || !fn) return WPE_RESULT_ERROR_NULL_ARG;
    if (!wpe_directory_exists(directory)) return WPE_RESULT_ERROR_NOT_FOUND;
    wpe__Walk* walk = wpe_alloc_zeroed(1, sizeof *walk); /* Large; keep it off the stack. */
    if (!walk) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    walk->fn = fn;
    walk->user_data = user_data;
    const bool ok = SDL_EnumerateDirectory(directory, wpe__walk_entry, walk);
    WPE_Result result = walk->result;
    if (!ok && !walk->stopped && result == WPE_RESULT_OK) {
        wpe_log_error("cannot list '%s': %s", directory, SDL_GetError());
        result = WPE_RESULT_ERROR_IO;
    }
    wpe_free(walk);
    return result;
}

bool wpe_directory_exists(const char* path) {
    SDL_PathInfo info;
    return path && SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_DIRECTORY;
}

WPE_Result wpe_directory_create(const char* path) {
    if (!path) return WPE_RESULT_ERROR_NULL_ARG;
    if (SDL_CreateDirectory(path)) return WPE_RESULT_OK; /* Creates parents; true if it exists. */
    wpe_log_error("cannot create directory '%s': %s", path, SDL_GetError());
    return WPE_RESULT_ERROR_IO;
}

WPE_Result wpe_path_remove(const char* path) {
    if (!path) return WPE_RESULT_ERROR_NULL_ARG;
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info)) return WPE_RESULT_OK;
    if (SDL_RemovePath(path)) return WPE_RESULT_OK;
    wpe_log_error("cannot remove '%s': %s", path, SDL_GetError());
    return WPE_RESULT_ERROR_IO;
}

WPE_Result wpe_path_rename(const char* from, const char* to) {
    if (!from || !to) return WPE_RESULT_ERROR_NULL_ARG;
    if (SDL_RenamePath(from, to)) return WPE_RESULT_OK; /* Replaces an existing `to`. */
    wpe_log_error("cannot rename '%s' to '%s': %s", from, to, SDL_GetError());
    return WPE_RESULT_ERROR_IO;
}

void wpe_image_free(WPE_Image* image) {
    if (!image) return;
    wpe_free(image->pixels);
    *image = (WPE_Image){0};
}

/* ---- Process ------------------------------------------------------------------------------------- */

WPE_Result wpe_platform_executable_path(char* out_path, size_t capacity) {
    if (!out_path) return WPE_RESULT_ERROR_NULL_ARG;
    if (capacity == 0) return WPE_RESULT_ERROR_CAPACITY;
    out_path[0] = '\0';
#if defined(_WIN32)
    enum { WIDE_MAX = 32768 }; /* The longest path Windows supports. */
    wchar_t* wide = wpe_alloc(WIDE_MAX * sizeof *wide);
    if (!wide) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    const DWORD length = GetModuleFileNameW(NULL, wide, WIDE_MAX);
    WPE_Result result = WPE_RESULT_OK;
    if (length == 0 || length >= WIDE_MAX) {
        wpe_log_error("cannot determine the executable path (error %lu)", (unsigned long)GetLastError());
        result = WPE_RESULT_ERROR_IO;
    }
    const int needed =
        result == WPE_RESULT_OK ? WideCharToMultiByte(CP_UTF8, 0, wide, (int)length, NULL, 0, NULL, NULL) : 0;
    if (result == WPE_RESULT_OK && needed <= 0) result = WPE_RESULT_ERROR_IO;
    if (result == WPE_RESULT_OK && (size_t)needed >= capacity) result = WPE_RESULT_ERROR_CAPACITY;
    if (result == WPE_RESULT_OK) {
        WideCharToMultiByte(CP_UTF8, 0, wide, (int)length, out_path, needed, NULL, NULL);
        out_path[needed] = '\0';
    }
    wpe_free(wide);
    if (result != WPE_RESULT_OK) return result;
#else
    const ssize_t length = readlink("/proc/self/exe", out_path, capacity);
    if (length < 0) {
        wpe_log_error("cannot determine the executable path");
        return WPE_RESULT_ERROR_IO;
    }
    if ((size_t)length >= capacity) {
        out_path[0] = '\0';
        return WPE_RESULT_ERROR_CAPACITY;
    }
    out_path[length] = '\0';
#endif
    return WPE_RESULT_OK;
}

void wpe_platform_attach_console(void) {
#if defined(_WIN32)
    /* Output redirected by the parent (pipes, files) already works; only an interactive parent
     * console needs attaching. */
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != NULL && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN) return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return; /* Started without a console (e.g. double-clicked). */
    if (!freopen("CONOUT$", "w", stdout) || !freopen("CONOUT$", "w", stderr)) return;
#endif
}

void wpe_platform_show_error(const char* title, const char* message) {
    if (!title || !message) return;
    if (!SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, message, NULL))
        wpe_log_error("%s: %s", title, message); /* No display available: the log has to do. */
}

WPE_Result wpe_file_set_executable(const char* path) {
    if (!path) return WPE_RESULT_ERROR_NULL_ARG;
#if defined(_WIN32)
    /* Windows decides executability by extension. */
    SDL_PathInfo info;
    return SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_FILE ? WPE_RESULT_OK
                                                                          : WPE_RESULT_ERROR_NOT_FOUND;
#else
    struct stat status;
    if (stat(path, &status) != 0) return WPE_RESULT_ERROR_NOT_FOUND;
    /* Executable for everyone who may read it. */
    const mode_t mode = status.st_mode | ((status.st_mode & (S_IRUSR | S_IRGRP | S_IROTH)) >> 2);
    if (chmod(path, mode) == 0) return WPE_RESULT_OK;
    wpe_log_error("cannot make '%s' executable", path);
    return WPE_RESULT_ERROR_IO;
#endif
}
