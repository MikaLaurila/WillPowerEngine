#include "wpe_test.h"

static int g_side_effects;

static bool count_and_return(bool value) {
    ++g_side_effects;
    return value;
}

static void test_debug_assert_passes_on_true(void) {
    wpe_assert(1 + 1 == 2);
}

static void test_debug_assert_fires_on_false(void) {
    WPE_TEST_SKIP_IF_NO_ASSERTS();
    wpe_log_set_stderr_enabled(false);
    WPE_TEST_EXPECT_ASSERT(wpe_assert(1 + 1 == 3));
    wpe_log_set_stderr_enabled(true);
}

static void test_debug_unreachable_always_fires(void) {
    wpe_log_set_stderr_enabled(false);
    WPE_TEST_EXPECT_ASSERT(wpe_unreachable());
    wpe_log_set_stderr_enabled(true);
}

static void test_debug_check_returns_condition_and_always_evaluates(void) {
    g_side_effects = 0;
    wpe_log_set_stderr_enabled(false);
    const bool passed = wpe_check(count_and_return(true));
    const bool failed = wpe_check(count_and_return(false));
    wpe_log_set_stderr_enabled(true);

    WPE_TEST_ASSERT(passed);
    WPE_TEST_ASSERT(!failed);
    WPE_TEST_ASSERT_EQ_INT(2, g_side_effects);
}

static void custom_handler(const char* expression, const char* file, int line) {
    (void)expression;
    (void)file;
    (void)line;
}

static void test_debug_set_assert_handler_returns_previous(void) {
    const WPE_AssertHandlerFn runner_handler = wpe_set_assert_handler(custom_handler);
    const WPE_AssertHandlerFn mine = wpe_set_assert_handler(runner_handler);
    WPE_TEST_ASSERT(runner_handler != NULL);
    WPE_TEST_ASSERT(mine == custom_handler);
}

WPE_TEST_MAIN(
    WPE_TEST_CASE(test_debug_assert_passes_on_true),
    WPE_TEST_CASE(test_debug_assert_fires_on_false),
    WPE_TEST_CASE(test_debug_unreachable_always_fires),
    WPE_TEST_CASE(test_debug_check_returns_condition_and_always_evaluates),
    WPE_TEST_CASE(test_debug_set_assert_handler_returns_previous))
