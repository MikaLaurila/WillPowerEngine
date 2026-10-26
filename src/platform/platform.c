#define WPE_LOG_MODULE "platform"
#include "platform/platform.h"

#include <SDL3/SDL.h>
#include <string.h>

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

    const SDL_WindowFlags flags = desc->resizable ? SDL_WINDOW_RESIZABLE : 0;
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
    }
    return !window->quit_requested;
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

void wpe_image_free(WPE_Image* image) {
    if (!image) return;
    wpe_free(image->pixels);
    *image = (WPE_Image){0};
}
