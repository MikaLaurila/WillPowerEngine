/**
 * @file log.h
 * @brief Leveled, module-tagged logging with pluggable sinks.
 *
 * Each translation unit that logs defines its module name before including this header:
 *
 * @code
 * #define WPE_LOG_MODULE "ecs"
 * #include "core/log.h"
 *
 * wpe_log_error("entity %u has no transform", id);
 * @endcode
 *
 * Messages are formatted on the calling thread, then dispatched to every registered sink under
 * an internal lock, so sinks never observe interleaved messages. A default sink writing to
 * stderr is installed until wpe_log_set_stderr_enabled(false) is called.
 *
 * - Dependencies: none
 * - Thread affinity: logging is safe from any thread; sink registration is main thread only
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_CORE_LOG_H
#define WPE_CORE_LOG_H

#include <stdarg.h>
#include <stdint.h>

#include "core/result.h"

/* Messages longer than this (including the terminator) are truncated. */
#define WPE_LOG_MESSAGE_MAX 1024
/* Maximum number of sinks registered at once, excluding the built-in stderr sink. */
#define WPE_LOG_MAX_SINKS 8

/* MinGW GCC defaults `printf` checking to the legacy MSVCRT dialect; UCRT is C99-conformant. */
#if defined(__MINGW32__) && !defined(__clang__)
#define WPE_PRINTF_LIKE(fmt_index, args_index) [[gnu::format(gnu_printf, fmt_index, args_index)]]
#elif defined(__GNUC__) || defined(__clang__)
#define WPE_PRINTF_LIKE(fmt_index, args_index) [[gnu::format(printf, fmt_index, args_index)]]
#else
#define WPE_PRINTF_LIKE(fmt_index, args_index)
#endif

typedef enum WPE_LogLevel {
    WPE_LOG_LEVEL_TRACE = 0,
    WPE_LOG_LEVEL_DEBUG,
    WPE_LOG_LEVEL_INFO,
    WPE_LOG_LEVEL_WARN,
    WPE_LOG_LEVEL_ERROR,
    WPE_LOG_LEVEL_CRITICAL,
    WPE_LOG_LEVEL_COUNT
} WPE_LogLevel;

/** A single formatted log record. All pointers are only valid during the sink callback. */
typedef struct WPE_LogMessage {
    WPE_LogLevel level;
    const char* module;  /* Never NULL. */
    const char* file;    /* Never NULL. */
    int line;
    const char* text;    /* Formatted message, NUL-terminated, without trailing newline. */
} WPE_LogMessage;

/**
 * Sink callback. Called with the log lock held: it must not log, and should return quickly.
 *
 * @param message The record; borrowed for the duration of the call.
 * @param user_data The pointer given at registration.
 */
typedef void (*WPE_LogSinkFn)(const WPE_LogMessage* message, void* user_data);

/** Opaque identifier for a registered sink. Zero is never a valid id. */
typedef uint32_t WPE_LogSinkId;

/**
 * Formats and dispatches a message. Prefer the wpe_log_* macros, which fill in module/file/line.
 *
 * @param level Severity; messages below the minimum level are discarded cheaply.
 * @param module Module tag; NULL is logged as "?".
 * @param file Source file; NULL is logged as "?".
 * @param line Source line.
 * @param fmt printf-style format string; must not be NULL.
 *
 * @thread_safety Any thread
 */
WPE_PRINTF_LIKE(5, 6)
void wpe_log_write(WPE_LogLevel level, const char* module, const char* file, int line,
                   const char* fmt, ...);

/** va_list variant of wpe_log_write(). @thread_safety Any thread */
WPE_PRINTF_LIKE(5, 0)
void wpe_log_write_v(WPE_LogLevel level, const char* module, const char* file, int line,
                     const char* fmt, va_list args);

/**
 * Sets the minimum level that is dispatched. Defaults to DEBUG in debug builds, INFO otherwise.
 *
 * @thread_safety Any thread (atomic)
 */
void wpe_log_set_min_level(WPE_LogLevel level);

/** @return The current minimum level. @thread_safety Any thread (atomic) */
WPE_LogLevel wpe_log_get_min_level(void);

/** Enables or disables the built-in stderr sink. @thread_safety Any thread (atomic) */
void wpe_log_set_stderr_enabled(bool enabled);

/**
 * Registers a sink that receives every dispatched message.
 *
 * @param fn Callback; must not be NULL.
 * @param user_data Opaque pointer passed back to fn; the caller retains ownership.
 * @param out_id Receives the sink id for later removal; must not be NULL.
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, or WPE_RESULT_ERROR_CAPACITY when
 *         WPE_LOG_MAX_SINKS sinks are already registered.
 *
 * @thread_safety Main thread only
 */
[[nodiscard]] WPE_Result wpe_log_add_sink(WPE_LogSinkFn fn, void* user_data, WPE_LogSinkId* out_id);

/**
 * Unregisters a sink. After this returns the callback is guaranteed not to be running.
 *
 * @param id A sink id from wpe_log_add_sink().
 * @return WPE_RESULT_OK or WPE_RESULT_ERROR_NOT_FOUND.
 *
 * @thread_safety Main thread only
 */
WPE_Result wpe_log_remove_sink(WPE_LogSinkId id);

/** @return Upper-case level name, e.g. "WARN"; "?" for invalid values. @thread_safety Any thread */
const char* wpe_log_level_name(WPE_LogLevel level);

#ifndef WPE_LOG_MODULE
#define WPE_LOG_MODULE "?"
#endif

#define wpe_log_trace(...) wpe_log_write(WPE_LOG_LEVEL_TRACE, WPE_LOG_MODULE, __FILE__, __LINE__, __VA_ARGS__)
#define wpe_log_debug(...) wpe_log_write(WPE_LOG_LEVEL_DEBUG, WPE_LOG_MODULE, __FILE__, __LINE__, __VA_ARGS__)
#define wpe_log_info(...) wpe_log_write(WPE_LOG_LEVEL_INFO, WPE_LOG_MODULE, __FILE__, __LINE__, __VA_ARGS__)
#define wpe_log_warn(...) wpe_log_write(WPE_LOG_LEVEL_WARN, WPE_LOG_MODULE, __FILE__, __LINE__, __VA_ARGS__)
#define wpe_log_error(...) wpe_log_write(WPE_LOG_LEVEL_ERROR, WPE_LOG_MODULE, __FILE__, __LINE__, __VA_ARGS__)
#define wpe_log_critical(...) wpe_log_write(WPE_LOG_LEVEL_CRITICAL, WPE_LOG_MODULE, __FILE__, __LINE__, __VA_ARGS__)

#endif /* WPE_CORE_LOG_H */
