/**
 * @file platform.h
 * @brief OS integration: window creation, event pumping, presenting framebuffers, timing and
 *        PNG image I/O. Wraps SDL3; no SDL types appear in this interface.
 *
 * - Dependencies: core, renderer (WPE_Framebuffer / WPE_Color), SDL3 (private)
 * - Thread affinity: windows and events are main thread only; image I/O and time are any thread
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_PLATFORM_PLATFORM_H
#define WPE_PLATFORM_PLATFORM_H

#include <stdint.h>

#include "core/result.h"
#include "renderer/renderer.h"

typedef struct WPE_Window WPE_Window;

typedef struct WPE_WindowDesc {
    const char* title; /* Required; copied by SDL. */
    uint32_t width;    /* Initial client size in pixels. */
    uint32_t height;
    bool resizable;
    bool vsync;
} WPE_WindowDesc;

/**
 * Opens a window that can present WPE_Framebuffers (scaled with nearest filtering and letterboxed
 * to preserve the framebuffer's aspect ratio).
 *
 * @param out_window Receives the window; owned by the caller, release with wpe_window_destroy().
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_INVALID_ARG,
 *         WPE_RESULT_ERROR_OUT_OF_MEMORY or WPE_RESULT_ERROR (SDL failure; an ERROR is logged).
 *
 * @thread_safety Main thread only
 */
[[nodiscard]] WPE_Result wpe_window_create(const WPE_WindowDesc* desc, WPE_Window** out_window);

/** Closes the window (ownership consumed); NULL is a no-op. @thread_safety Main thread only */
void wpe_window_destroy(WPE_Window* window);

/**
 * Processes pending OS events.
 *
 * @return false once the user has requested to close the window (it stays false afterwards).
 *
 * @thread_safety Main thread only
 */
bool wpe_window_poll_events(WPE_Window* window);

/**
 * Displays a framebuffer's color target.
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or WPE_RESULT_ERROR (SDL failure; logged).
 *
 * @thread_safety Main thread only
 */
WPE_Result wpe_window_present(WPE_Window* window, const WPE_Framebuffer* framebuffer);

/** @return Monotonic time in seconds since an unspecified point. @thread_safety Any thread */
double wpe_platform_time_seconds(void);

/** An image in WPE_Color (0xAARRGGBB) pixels, rows tightly packed. */
typedef struct WPE_Image {
    uint32_t width;
    uint32_t height;
    WPE_Color* pixels; /* Owned; release with wpe_image_free(). */
} WPE_Image;

/**
 * Writes pixels to a PNG file (alpha preserved).
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_INVALID_ARG or
 *         WPE_RESULT_ERROR_IO (logged).
 *
 * @thread_safety Any thread
 */
WPE_Result wpe_image_save_png(const char* path, uint32_t width, uint32_t height, const WPE_Color* pixels);

/**
 * Reads a PNG file into memory.
 *
 * @param out_image Receives the image; free with wpe_image_free().
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_IO (missing/unreadable file;
 *         logged) or WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Any thread
 */
WPE_Result wpe_image_load_png(const char* path, WPE_Image* out_image);

/** Releases an image's pixels and zeroes it; NULL is a no-op. @thread_safety Any thread */
void wpe_image_free(WPE_Image* image);

#endif /* WPE_PLATFORM_PLATFORM_H */
