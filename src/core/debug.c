#define WPE_LOG_MODULE "core"
#include "core/debug.h"

#include <stdatomic.h>
#include <stdlib.h>

#include "core/log.h"

static _Atomic(WPE_AssertHandlerFn) wpe__g_assert_handler = NULL;

WPE_AssertHandlerFn wpe_set_assert_handler(WPE_AssertHandlerFn handler) {
    return atomic_exchange(&wpe__g_assert_handler, handler);
}

void wpe_assert_fail(const char* expression, const char* file, int line) {
    wpe_log_write(WPE_LOG_LEVEL_CRITICAL, WPE_LOG_MODULE, file, line, "assertion failed: %s", expression);

    WPE_AssertHandlerFn handler = atomic_load(&wpe__g_assert_handler);
    if (handler) {
        handler(expression, file, line);
        wpe_log_write(WPE_LOG_LEVEL_CRITICAL, WPE_LOG_MODULE, file, line,
                      "assert handler returned; aborting");
    }
    abort();
}

bool wpe_check_report(bool passed, const char* expression, const char* file, int line) {
    if (!passed) {
        wpe_log_write(WPE_LOG_LEVEL_ERROR, WPE_LOG_MODULE, file, line, "check failed: %s", expression);
    }
    return passed;
}
