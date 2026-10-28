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

#include <stddef.h>
#include <stdint.h>

#include "core/result.h"
#include "renderer/renderer.h"

typedef struct WPE_Window WPE_Window;

/**
 * An OS event as read by the event pump. Opaque to the engine: only modules allowed to use SDL
 * (input, and later the editor) interpret it, as a `const SDL_Event*`.
 */
typedef struct WPE_PlatformEvent WPE_PlatformEvent;

/**
 * A window's GPU-accelerated 2D renderer. Opaque like WPE_PlatformEvent: only modules allowed to use
 * SDL (the editor) interpret it, as an `SDL_Renderer*`.
 */
typedef struct WPE_PlatformRenderer WPE_PlatformRenderer;

/** Receives each event read by the pump; `event` is valid only during the call. */
typedef void (*WPE_PlatformEventFn)(const WPE_PlatformEvent* event, void* user_data);

typedef struct WPE_WindowDesc {
    const char* title; /* Required; copied by SDL. */
    uint32_t width;    /* Initial client size in pixels. */
    uint32_t height;
    bool resizable;
    bool vsync;
    bool fullscreen; /* Fullscreen on the current display (the framebuffer is letterboxed). */
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
 * Processes pending OS events, passing each to the window's event handler (if set). Mouse
 * coordinates in the events are converted to framebuffer pixels of the last presented frame.
 *
 * @return false once the user has requested to close the window (it stays false afterwards).
 *
 * @thread_safety Main thread only
 */
bool wpe_window_poll_events(WPE_Window* window);

/**
 * Withdraws the user's request to close the window, so wpe_window_poll_events() returns true again
 * (for hosts that ask first, such as an editor with unsaved changes).
 *
 * @thread_safety Main thread only
 */
void wpe_window_cancel_close(WPE_Window* window);

/** Sets the function receiving the events read by wpe_window_poll_events() (NULL removes it). @thread_safety
 * Main thread only */
void wpe_window_set_event_handler(WPE_Window* window, WPE_PlatformEventFn fn, void* user_data);

/**
 * Event pump for programs without a window (headless runs, tools, tests): reads every pending event
 * and passes it to `fn` (may be NULL to discard them). Only subsystems that were initialized
 * produce events (for example the input module's gamepad support).
 *
 * @return false if an application quit request was among the events.
 *
 * @thread_safety Main thread only
 */
bool wpe_platform_poll_events(WPE_PlatformEventFn fn, void* user_data);

/**
 * Captures the mouse: the cursor is hidden and confined, and motion arrives as relative deltas
 * (for mouse look).
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or WPE_RESULT_ERROR (SDL failure; logged).
 *
 * @thread_safety Main thread only
 */
WPE_Result wpe_window_set_relative_mouse(WPE_Window* window, bool enabled);

/** Gets the window's client size in pixels (0 x 0 for NULL or on failure). @thread_safety Main thread only */
void wpe_window_size(const WPE_Window* window, uint32_t* out_width, uint32_t* out_height);

/**
 * Turns delivery of text input events (typed characters, with keyboard layout and IME applied) on or
 * off, e.g. while a text field has focus.
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or WPE_RESULT_ERROR (SDL failure; logged).
 *
 * @thread_safety Main thread only
 */
WPE_Result wpe_window_set_text_input(WPE_Window* window, bool enabled);

/** Changes the window title (copied). @thread_safety Main thread only */
void wpe_window_set_title(WPE_Window* window, const char* title);

/**
 * Displays a framebuffer's color target.
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or WPE_RESULT_ERROR (SDL failure; logged).
 *
 * @thread_safety Main thread only
 */
WPE_Result wpe_window_present(WPE_Window* window, const WPE_Framebuffer* framebuffer);

/**
 * The renderer that presents the window, for hosts that draw the window themselves instead of
 * presenting framebuffers (the editor draws its interface on the GPU). Such a host never calls
 * wpe_window_present(), and event mouse coordinates are then the renderer's output pixels.
 *
 * @return The renderer, owned by the window (valid until wpe_window_destroy()), or NULL for NULL.
 *
 * @thread_safety Main thread only
 */
WPE_PlatformRenderer* wpe_window_renderer(WPE_Window* window);

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

/**
 * Reads a whole file into memory. Paths are UTF-8 on every platform.
 *
 * @param path File path; must not be NULL.
 * @param out_data Receives a wpe_alloc() buffer owned by the caller (release with wpe_free()). A
 *                 terminating NUL byte is appended (not counted in out_size) so text can be
 *                 parsed in place.
 * @param out_size Receives the file size in bytes.
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_IO (logged) or
 *         WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Any thread
 */
WPE_Result wpe_file_read_all(const char* path, void** out_data, size_t* out_size);

/**
 * Reads a file's last modification time (for change detection such as hot reload).
 *
 * @param out_time Receives an opaque, monotonically comparable timestamp (nanoseconds).
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or WPE_RESULT_ERROR_NOT_FOUND (no such file;
 *         not logged, since polling for missing files is normal).
 *
 * @thread_safety Any thread
 */
WPE_Result wpe_file_modified_time(const char* path, int64_t* out_time);

/** An open file stream (binary). */
typedef struct WPE_File WPE_File;

/**
 * Opens a file for reading, or for writing (created or truncated).
 *
 * @param out_file Receives the stream; close with wpe_file_close().
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_NOT_FOUND (reading a missing
 *         file; not logged), WPE_RESULT_ERROR_IO (logged) or WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Any thread; one stream must not be used by two threads at once
 */
[[nodiscard]] WPE_Result wpe_file_open(const char* path, bool write, WPE_File** out_file);

/**
 * Closes a stream (ownership consumed); NULL is a no-op.
 *
 * @return WPE_RESULT_OK, or WPE_RESULT_ERROR_IO when flushing written data failed (logged).
 *
 * @thread_safety Any thread
 */
WPE_Result wpe_file_close(WPE_File* file);

/** @return Bytes read (less than `size` only at end of file or on error). @thread_safety See wpe_file_open()
 */
size_t wpe_file_read(WPE_File* file, void* buffer, size_t size);

/** @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or WPE_RESULT_ERROR_IO (short write; logged).
 * @thread_safety See wpe_file_open() */
WPE_Result wpe_file_write(WPE_File* file, const void* data, size_t size);

/** Seeks to an absolute byte offset. @return WPE_RESULT_OK or WPE_RESULT_ERROR_IO. @thread_safety See
 * wpe_file_open() */
WPE_Result wpe_file_seek(WPE_File* file, uint64_t offset);

/** @return The current byte offset, or UINT64_MAX on error. @thread_safety See wpe_file_open() */
uint64_t wpe_file_tell(WPE_File* file);

/** @return The file size in bytes, or UINT64_MAX on error. @thread_safety See wpe_file_open() */
uint64_t wpe_file_size(WPE_File* file);

/**
 * Called for every regular file below a directory by wpe_directory_walk().
 *
 * @param relative_path Path relative to the walked directory, with '/' separators (borrowed for
 *                      the duration of the call).
 * @return false to stop the walk.
 */
typedef bool (*WPE_DirectoryWalkFn)(const char* relative_path, void* user_data);

/**
 * Recursively lists the regular files below a directory, in no particular order.
 *
 * @return WPE_RESULT_OK (also when the callback stopped the walk), WPE_RESULT_ERROR_NULL_ARG,
 *         WPE_RESULT_ERROR_NOT_FOUND (not a directory), WPE_RESULT_ERROR_CAPACITY (nesting deeper
 *         than 32 levels or a path longer than 1023 bytes; logged) or WPE_RESULT_ERROR_IO (logged).
 *
 * @thread_safety Any thread
 */
WPE_Result wpe_directory_walk(const char* directory, WPE_DirectoryWalkFn fn, void* user_data);

/** @return Whether `path` names an existing directory. @thread_safety Any thread */
bool wpe_directory_exists(const char* path);

/**
 * Creates a directory and any missing parents (succeeds if it already exists).
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or WPE_RESULT_ERROR_IO (logged).
 *
 * @thread_safety Any thread
 */
WPE_Result wpe_directory_create(const char* path);

/**
 * Deletes a file or an empty directory.
 *
 * @return WPE_RESULT_OK (also if nothing existed), WPE_RESULT_ERROR_NULL_ARG or WPE_RESULT_ERROR_IO
 *         (for example a non-empty directory; logged).
 *
 * @thread_safety Any thread
 */
WPE_Result wpe_path_remove(const char* path);

/**
 * Renames (moves) a file or directory, replacing an existing file at `to`.
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or WPE_RESULT_ERROR_IO (logged).
 *
 * @thread_safety Any thread
 */
WPE_Result wpe_path_rename(const char* from, const char* to);

/**
 * Gets the absolute path of the running executable (UTF-8).
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_CAPACITY (buffer too small),
 *         WPE_RESULT_ERROR_OUT_OF_MEMORY or WPE_RESULT_ERROR_IO (logged).
 *
 * @thread_safety Any thread
 */
WPE_Result wpe_platform_executable_path(char* out_path, size_t capacity);

/**
 * Gets (creating it if needed) the per-user directory where an application may write its data:
 * `%APPDATA%\<org>\<app>\` on Windows, `~/.local/share/<org>/<app>/` on Linux. The path ends with a
 * separator. Names should hold only letters, digits and spaces.
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_CAPACITY (buffer too small) or
 *         WPE_RESULT_ERROR_IO (logged).
 *
 * @thread_safety Any thread
 */
WPE_Result wpe_platform_save_directory(const char* org, const char* app, char* out_path, size_t capacity);

/**
 * Programs built as GUI applications (the game runtime on Windows) have no console; when started
 * from a terminal, this attaches stdout/stderr to it so logs stay visible. Does nothing elsewhere
 * or when output is already redirected.
 *
 * @thread_safety Main thread, at startup
 */
void wpe_platform_attach_console(void);

/**
 * Shows a modal error message box (blocking until dismissed); logs instead when no display is
 * available. For fatal errors of interactive programs only (never in headless runs).
 *
 * @thread_safety Main thread only
 */
void wpe_platform_show_error(const char* title, const char* message);

/**
 * Marks a file as executable (POSIX: adds execute permission wherever read permission is granted;
 * Windows: nothing to do, executability follows the extension).
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_NOT_FOUND or WPE_RESULT_ERROR_IO
 *         (logged).
 *
 * @thread_safety Any thread
 */
WPE_Result wpe_file_set_executable(const char* path);

#endif /* WPE_PLATFORM_PLATFORM_H */
