/**
 * @file wpe_test.h
 * @brief Minimal unit test framework used by all WillPowerEngine test suites.
 *
 * Each test executable lists its cases and hands them to the runner:
 *
 * @code
 * static void test_ecs_spawn(void) {
 *     WPE_TEST_ASSERT(wpe_ecs_spawn(ecs) != WPE_ENTITY_NULL);
 * }
 *
 * WPE_TEST_MAIN(WPE_TEST_CASE(test_ecs_spawn))
 * @endcode
 *
 * Runner guarantees, per test case:
 * - A failed WPE_TEST_ASSERT* aborts the case (later cases still run).
 * - A failed wpe_assert() inside engine code fails the case instead of aborting the process.
 * - Any change in live wpe_alloc() allocations fails the case as a leak.
 *
 * Command line: `<exe> [--filter <substring>] [--list]`. Exit code is 0 only if every selected
 * case passed.
 *
 * - Dependencies: core
 * - Thread affinity: assertions must only be used on the thread running the test
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_TEST_H
#define WPE_TEST_H

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "core/core.h"

typedef struct WPE_TestCase {
    const char* name;
    void (*fn)(void);
} WPE_TestCase;

/* Braced initializer (not a compound literal) so the case table can be a constant static array. */
#define WPE_TEST_CASE(fn) {#fn, fn}

/** Runs the given cases; returns the process exit code. Use WPE_TEST_MAIN instead. */
int wpe_test_run(const WPE_TestCase* cases, size_t count, int argc, char** argv);

#define WPE_TEST_MAIN(...)                                                                                   \
    int main(int argc, char** argv) {                                                                        \
        static const WPE_TestCase wpe__test_cases[] = {__VA_ARGS__};                                         \
        return wpe_test_run(wpe__test_cases, sizeof wpe__test_cases / sizeof wpe__test_cases[0], argc,       \
                            argv);                                                                           \
    }

/* Internal: records a failure and unwinds to the runner. Never returns. */
[[noreturn]] WPE_PRINTF_LIKE(3, 4) void wpe_test_fail(const char* file, int line, const char* fmt, ...);

/* Internal: jump buffer used by WPE_TEST_EXPECT_ASSERT. */
jmp_buf* wpe_test_push_expect_assert(void);
void wpe_test_pop_expect_assert(void);

#define WPE_TEST_ASSERT(cond)                                                                                \
    ((cond) ? (void)0 : wpe_test_fail(__FILE__, __LINE__, "assertion failed: %s", #cond))

#define WPE_TEST_ASSERT_MSG(cond, ...) ((cond) ? (void)0 : wpe_test_fail(__FILE__, __LINE__, __VA_ARGS__))

#define WPE_TEST_ASSERT_EQ_INT(expected, actual)                                                             \
    do {                                                                                                     \
        const long long wpe__e = (long long)(expected);                                                      \
        const long long wpe__a = (long long)(actual);                                                        \
        if (wpe__e != wpe__a)                                                                                \
            wpe_test_fail(__FILE__, __LINE__, "%s == %s: expected %lld, got %lld", #expected, #actual,       \
                          wpe__e, wpe__a);                                                                   \
    } while (0)

#define WPE_TEST_ASSERT_EQ_UINT(expected, actual)                                                            \
    do {                                                                                                     \
        const unsigned long long wpe__e = (unsigned long long)(expected);                                    \
        const unsigned long long wpe__a = (unsigned long long)(actual);                                      \
        if (wpe__e != wpe__a)                                                                                \
            wpe_test_fail(__FILE__, __LINE__, "%s == %s: expected %llu, got %llu", #expected, #actual,       \
                          wpe__e, wpe__a);                                                                   \
    } while (0)

#define WPE_TEST_ASSERT_NEAR(expected, actual, tolerance)                                                    \
    do {                                                                                                     \
        const double wpe__e = (double)(expected);                                                            \
        const double wpe__a = (double)(actual);                                                              \
        const double wpe__d = wpe__e > wpe__a ? wpe__e - wpe__a : wpe__a - wpe__e;                           \
        if (!(wpe__d <= (double)(tolerance)))                                                                \
            wpe_test_fail(__FILE__, __LINE__, "%s ~= %s: expected %.9g, got %.9g (tol %.3g)", #expected,     \
                          #actual, wpe__e, wpe__a, (double)(tolerance));                                     \
    } while (0)

#define WPE_TEST_ASSERT_EQ_STR(expected, actual)                                                             \
    do {                                                                                                     \
        const char* wpe__e = (expected);                                                                     \
        const char* wpe__a = (actual);                                                                       \
        if (!wpe__e || !wpe__a || strcmp(wpe__e, wpe__a) != 0)                                               \
            wpe_test_fail(__FILE__, __LINE__, "%s == %s: expected \"%s\", got \"%s\"", #expected, #actual,   \
                          wpe__e ? wpe__e : "(null)", wpe__a ? wpe__a : "(null)");                           \
    } while (0)

#define WPE_TEST_ASSERT_OK(expr)                                                                             \
    do {                                                                                                     \
        const WPE_Result wpe__r = (expr);                                                                    \
        if (wpe__r != WPE_RESULT_OK)                                                                         \
            wpe_test_fail(__FILE__, __LINE__, "%s returned %s", #expr, wpe_result_to_string(wpe__r));        \
    } while (0)

/*
 * Asserts that executing `stmt` triggers a wpe_assert() failure. Only meaningful when WPE_DEBUG is
 * defined; use WPE_TEST_SKIP_IF_NO_ASSERTS() first in such tests. `stmt` is abandoned at the
 * failing assertion, so it must not leave allocations behind.
 */
#define WPE_TEST_EXPECT_ASSERT(stmt)                                                                         \
    do {                                                                                                     \
        if (setjmp(*wpe_test_push_expect_assert()) == 0) {                                                   \
            stmt;                                                                                            \
            wpe_test_pop_expect_assert();                                                                    \
            wpe_test_fail(__FILE__, __LINE__, "expected assertion from: %s", #stmt);                         \
        }                                                                                                    \
        wpe_test_pop_expect_assert();                                                                        \
    } while (0)

/* Internal: marks the running case as skipped and unwinds to the runner. */
[[noreturn]] void wpe_test_skip(const char* reason);

#if defined(WPE_DEBUG)
#define WPE_TEST_SKIP_IF_NO_ASSERTS() ((void)0)
#else
#define WPE_TEST_SKIP_IF_NO_ASSERTS() wpe_test_skip("wpe_assert disabled in this build type")
#endif

#endif /* WPE_TEST_H */
