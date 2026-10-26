#include "wpe_test.h"

static void test_result_ok_is_zero(void) {
    WPE_TEST_ASSERT_EQ_INT(0, WPE_RESULT_OK);
}

static void test_result_to_string_names_every_code(void) {
    for (int i = 0; i < WPE_RESULT_COUNT; ++i) {
        const char* name = wpe_result_to_string((WPE_Result)i);
        WPE_TEST_ASSERT(name != NULL);
        WPE_TEST_ASSERT_MSG(strncmp(name, "WPE_RESULT_", 11) == 0, "bad name for %d: %s", i, name);
    }
    WPE_TEST_ASSERT_EQ_STR("WPE_RESULT_ERROR_NOT_FOUND", wpe_result_to_string(WPE_RESULT_ERROR_NOT_FOUND));
}

static void test_result_to_string_rejects_out_of_range(void) {
    WPE_TEST_ASSERT_EQ_STR("WPE_RESULT_<invalid>", wpe_result_to_string(WPE_RESULT_COUNT));
    WPE_TEST_ASSERT_EQ_STR("WPE_RESULT_<invalid>", wpe_result_to_string((WPE_Result)-1));
}

WPE_TEST_MAIN(
    WPE_TEST_CASE(test_result_ok_is_zero),
    WPE_TEST_CASE(test_result_to_string_names_every_code),
    WPE_TEST_CASE(test_result_to_string_rejects_out_of_range))
