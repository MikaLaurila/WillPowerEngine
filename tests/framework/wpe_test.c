#include "wpe_test.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* Maximum nesting of WPE_TEST_EXPECT_ASSERT blocks within one test case. */
#define WPE__TEST_MAX_EXPECT_DEPTH 4

typedef enum wpe__TestOutcome {
    WPE__TEST_OUTCOME_RUNNING = 0,  /* setjmp return value for the initial call. */
    WPE__TEST_OUTCOME_FAILED,
    WPE__TEST_OUTCOME_SKIPPED,
} wpe__TestOutcome;

/* Runner state. Tests run sequentially on the main thread, so no synchronization is needed. */
static struct {
    jmp_buf case_jump;
    jmp_buf expect_jumps[WPE__TEST_MAX_EXPECT_DEPTH];
    int expect_depth;
} wpe__g_test;

void wpe_test_fail(const char* file, int line, const char* fmt, ...) {
    fprintf(stderr, "    FAIL %s:%d: ", file, line);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    longjmp(wpe__g_test.case_jump, WPE__TEST_OUTCOME_FAILED);
}

void wpe_test_skip(const char* reason) {
    fprintf(stderr, "    SKIP: %s\n", reason);
    longjmp(wpe__g_test.case_jump, WPE__TEST_OUTCOME_SKIPPED);
}

jmp_buf* wpe_test_push_expect_assert(void) {
    if (wpe__g_test.expect_depth >= WPE__TEST_MAX_EXPECT_DEPTH) {
        wpe_test_fail(__FILE__, __LINE__, "WPE_TEST_EXPECT_ASSERT nested too deeply");
    }
    return &wpe__g_test.expect_jumps[wpe__g_test.expect_depth++];
}

void wpe_test_pop_expect_assert(void) {
    if (wpe__g_test.expect_depth > 0) --wpe__g_test.expect_depth;
}

/* Routes engine assertion failures back into the runner instead of aborting the process. */
static void wpe__test_assert_handler(const char* expression, const char* file, int line) {
    if (wpe__g_test.expect_depth > 0) {
        longjmp(wpe__g_test.expect_jumps[wpe__g_test.expect_depth - 1], 1);
    }
    wpe_test_fail(file, line, "engine assertion failed: %s", expression);
}

static bool wpe__test_selected(const char* name, const char* filter) {
    return filter == NULL || strstr(name, filter) != NULL;
}

static uint64_t wpe__test_live_allocations(void) {
    WPE_AllocStats stats;
    wpe_alloc_get_stats(&stats);
    return stats.live_allocations;
}

static wpe__TestOutcome wpe__test_run_case(const WPE_TestCase* test_case) {
    wpe__g_test.expect_depth = 0;
    const uint64_t live_before = wpe__test_live_allocations();

    /* setjmp may only appear in a controlling expression; failures/skips longjmp back here. */
    switch (setjmp(wpe__g_test.case_jump)) {
    case WPE__TEST_OUTCOME_RUNNING:
        test_case->fn();
        break;
    case WPE__TEST_OUTCOME_SKIPPED:
        return WPE__TEST_OUTCOME_SKIPPED;
    default:
        return WPE__TEST_OUTCOME_FAILED;
    }

    const uint64_t live_after = wpe__test_live_allocations();
    if (live_after != live_before) {
        fprintf(stderr, "    FAIL: leaked %lld allocation(s)\n",
                (long long)live_after - (long long)live_before);
        return WPE__TEST_OUTCOME_FAILED;
    }
    return WPE__TEST_OUTCOME_RUNNING;
}

int wpe_test_run(const WPE_TestCase* cases, size_t count, int argc, char** argv) {
    const char* filter = NULL;
    bool list_only = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--filter") == 0 && i + 1 < argc) {
            filter = argv[++i];
        } else if (strcmp(argv[i], "--list") == 0) {
            list_only = true;
        } else {
            fprintf(stderr, "usage: %s [--filter <substring>] [--list]\n", argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (list_only) {
        for (size_t i = 0; i < count; ++i) {
            if (wpe__test_selected(cases[i].name, filter)) printf("%s\n", cases[i].name);
        }
        return EXIT_SUCCESS;
    }

    const WPE_AssertHandlerFn previous_handler = wpe_set_assert_handler(wpe__test_assert_handler);

    size_t passed = 0, failed = 0, skipped = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!wpe__test_selected(cases[i].name, filter)) continue;

        fprintf(stderr, "[ RUN  ] %s\n", cases[i].name);
        switch (wpe__test_run_case(&cases[i])) {
        case WPE__TEST_OUTCOME_RUNNING:
            ++passed;
            fprintf(stderr, "[  OK  ] %s\n", cases[i].name);
            break;
        case WPE__TEST_OUTCOME_SKIPPED:
            ++skipped;
            fprintf(stderr, "[ SKIP ] %s\n", cases[i].name);
            break;
        case WPE__TEST_OUTCOME_FAILED:
            ++failed;
            fprintf(stderr, "[ FAIL ] %s\n", cases[i].name);
            break;
        }
    }

    wpe_set_assert_handler(previous_handler);

    fprintf(stderr, "%zu passed, %zu failed, %zu skipped\n", passed, failed, skipped);
    if (passed + failed + skipped == 0) {
        fprintf(stderr, "no test cases matched filter \"%s\"\n", filter ? filter : "");
        return EXIT_FAILURE;
    }
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
