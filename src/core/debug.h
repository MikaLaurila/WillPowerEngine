/**
 * @file debug.h
 * @brief Assertions (programmer errors) and checks (runtime validation).
 *
 * - wpe_assert(cond): a violated invariant is a bug. Enabled when WPE_DEBUG is defined (Debug and
 *   RelWithDebInfo builds) and compiled out otherwise, so `cond` must not have side effects.
 *   On failure it logs a CRITICAL message and calls the assert handler, which aborts by default.
 * - wpe_check(cond): validation that may legitimately fail in production (bad input data, etc.).
 *   Always evaluated; on failure logs an ERROR and yields false so the caller can bail out:
 *
 * @code
 * if (!wpe_check(header.version == WPE_PAK_VERSION)) return WPE_RESULT_ERROR_UNSUPPORTED;
 * @endcode
 *
 * - Dependencies: core/log
 * - Thread affinity: any thread; wpe_set_assert_handler() main thread only
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_CORE_DEBUG_H
#define WPE_CORE_DEBUG_H

/**
 * Called when an assertion fails. Handlers must not return normally (the test framework longjmps
 * out); if one does, the process aborts, so code after a failed assertion never runs.
 *
 * @param expression Stringified condition. @param file Source file. @param line Source line.
 */
typedef void (*WPE_AssertHandlerFn)(const char* expression, const char* file, int line);

/**
 * Replaces the assert handler.
 *
 * @param handler New handler, or NULL to restore the default (log + abort).
 * @return The previously installed handler (NULL if it was the default).
 *
 * @thread_safety Main thread only; must not race with failing assertions on other threads
 */
WPE_AssertHandlerFn wpe_set_assert_handler(WPE_AssertHandlerFn handler);

/** Implementation of wpe_assert(); call through the macro. @thread_safety Any thread */
[[noreturn]] void wpe_assert_fail(const char* expression, const char* file, int line);

/**
 * Implementation of wpe_check(); call through the macro.
 *
 * @return `passed`, after logging an ERROR when it is false.
 * @thread_safety Any thread
 */
bool wpe_check_report(bool passed, const char* expression, const char* file, int line);

#if defined(WPE_DEBUG)
#define wpe_assert(cond) ((cond) ? (void)0 : wpe_assert_fail(#cond, __FILE__, __LINE__))
#else
#define wpe_assert(cond) ((void)0)
#endif

#define wpe_check(cond) wpe_check_report((cond) ? true : false, #cond, __FILE__, __LINE__)

/* Marks code paths that must never execute. Active in all build types. */
#define wpe_unreachable() wpe_assert_fail("unreachable", __FILE__, __LINE__)

#endif /* WPE_CORE_DEBUG_H */
