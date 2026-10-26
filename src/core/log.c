#include "core/log.h"

#include <stdatomic.h>
#include <stdio.h>

#if defined(WPE_DEBUG)
#define WPE__LOG_DEFAULT_MIN_LEVEL WPE_LOG_LEVEL_DEBUG
#else
#define WPE__LOG_DEFAULT_MIN_LEVEL WPE_LOG_LEVEL_INFO
#endif

typedef struct wpe__LogSink {
    WPE_LogSinkFn fn;  /* NULL marks a free slot. */
    void* user_data;
    WPE_LogSinkId id;
} wpe__LogSink;

/*
 * Shared state. The sink table and next_id are guarded by `lock`; the level and stderr flag are
 * atomics so the fast "is this level enabled" check never takes the lock.
 *
 * NOTE(limitation): `lock` is a spinlock because core has no threading dependency. Critical
 * sections are a handful of sink calls, so contention stays short; revisit if profiling shows
 * worker threads spinning here.
 */
static struct {
    atomic_flag lock;
    atomic_int min_level;
    atomic_bool stderr_enabled;
    wpe__LogSink sinks[WPE_LOG_MAX_SINKS];
    WPE_LogSinkId next_id;
} wpe__g_log = {
    .lock = ATOMIC_FLAG_INIT,
    .min_level = WPE__LOG_DEFAULT_MIN_LEVEL,
    .stderr_enabled = true,
    .next_id = 1,
};

static const char* const wpe__log_level_names[] = {
    [WPE_LOG_LEVEL_TRACE] = "TRACE",
    [WPE_LOG_LEVEL_DEBUG] = "DEBUG",
    [WPE_LOG_LEVEL_INFO] = "INFO",
    [WPE_LOG_LEVEL_WARN] = "WARN",
    [WPE_LOG_LEVEL_ERROR] = "ERROR",
    [WPE_LOG_LEVEL_CRITICAL] = "CRITICAL",
};

static_assert(sizeof(wpe__log_level_names) / sizeof(wpe__log_level_names[0]) == WPE_LOG_LEVEL_COUNT,
              "every WPE_LogLevel needs a name");

static void wpe__log_lock(void) {
    while (atomic_flag_test_and_set_explicit(&wpe__g_log.lock, memory_order_acquire)) {
    }
}

static void wpe__log_unlock(void) {
    atomic_flag_clear_explicit(&wpe__g_log.lock, memory_order_release);
}

static void wpe__log_write_stderr(const WPE_LogMessage* message) {
    if (message->level >= WPE_LOG_LEVEL_ERROR) {
        fprintf(stderr, "[%s][%s] %s (%s:%d)\n", wpe_log_level_name(message->level), message->module,
                message->text, message->file, message->line);
    } else {
        fprintf(stderr, "[%s][%s] %s\n", wpe_log_level_name(message->level), message->module,
                message->text);
    }
}

const char* wpe_log_level_name(WPE_LogLevel level) {
    if ((unsigned)level >= WPE_LOG_LEVEL_COUNT) return "?";
    return wpe__log_level_names[level];
}

void wpe_log_write(WPE_LogLevel level, const char* module, const char* file, int line,
                   const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    wpe_log_write_v(level, module, file, line, fmt, args);
    va_end(args);
}

void wpe_log_write_v(WPE_LogLevel level, const char* module, const char* file, int line,
                     const char* fmt, va_list args) {
    if ((unsigned)level >= WPE_LOG_LEVEL_COUNT) return;
    if ((int)level < atomic_load_explicit(&wpe__g_log.min_level, memory_order_relaxed)) return;

    char text[WPE_LOG_MESSAGE_MAX];
    if (fmt == NULL) {
        snprintf(text, sizeof text, "(null format)");
    } else if (vsnprintf(text, sizeof text, fmt, args) < 0) {
        snprintf(text, sizeof text, "(invalid format: %s)", fmt);
    }

    const WPE_LogMessage message = {
        .level = level,
        .module = module ? module : "?",
        .file = file ? file : "?",
        .line = line,
        .text = text,
    };

    wpe__log_lock();
    if (atomic_load_explicit(&wpe__g_log.stderr_enabled, memory_order_relaxed)) {
        wpe__log_write_stderr(&message);
    }
    for (int i = 0; i < WPE_LOG_MAX_SINKS; ++i) {
        const wpe__LogSink* sink = &wpe__g_log.sinks[i];
        if (sink->fn) sink->fn(&message, sink->user_data);
    }
    wpe__log_unlock();
}

void wpe_log_set_min_level(WPE_LogLevel level) {
    if ((unsigned)level >= WPE_LOG_LEVEL_COUNT) return;
    atomic_store_explicit(&wpe__g_log.min_level, (int)level, memory_order_relaxed);
}

WPE_LogLevel wpe_log_get_min_level(void) {
    return (WPE_LogLevel)atomic_load_explicit(&wpe__g_log.min_level, memory_order_relaxed);
}

void wpe_log_set_stderr_enabled(bool enabled) {
    atomic_store_explicit(&wpe__g_log.stderr_enabled, enabled, memory_order_relaxed);
}

WPE_Result wpe_log_add_sink(WPE_LogSinkFn fn, void* user_data, WPE_LogSinkId* out_id) {
    if (!fn || !out_id) return WPE_RESULT_ERROR_NULL_ARG;

    WPE_Result result = WPE_RESULT_ERROR_CAPACITY;
    wpe__log_lock();
    for (int i = 0; i < WPE_LOG_MAX_SINKS; ++i) {
        wpe__LogSink* sink = &wpe__g_log.sinks[i];
        if (sink->fn) continue;
        sink->fn = fn;
        sink->user_data = user_data;
        sink->id = wpe__g_log.next_id++;
        if (wpe__g_log.next_id == 0) wpe__g_log.next_id = 1;  /* Zero is reserved as invalid. */
        *out_id = sink->id;
        result = WPE_RESULT_OK;
        break;
    }
    wpe__log_unlock();
    return result;
}

WPE_Result wpe_log_remove_sink(WPE_LogSinkId id) {
    if (id == 0) return WPE_RESULT_ERROR_NOT_FOUND;

    WPE_Result result = WPE_RESULT_ERROR_NOT_FOUND;
    wpe__log_lock();
    for (int i = 0; i < WPE_LOG_MAX_SINKS; ++i) {
        wpe__LogSink* sink = &wpe__g_log.sinks[i];
        if (sink->fn && sink->id == id) {
            *sink = (wpe__LogSink){0};
            result = WPE_RESULT_OK;
            break;
        }
    }
    wpe__log_unlock();
    return result;
}
