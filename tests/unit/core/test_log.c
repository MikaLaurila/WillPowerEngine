#define WPE_LOG_MODULE "test"
#include "wpe_test.h"

#include <stdio.h>

#define CAPTURE_MAX 8

typedef struct Capture {
    int count;
    WPE_LogLevel levels[CAPTURE_MAX];
    char modules[CAPTURE_MAX][32];
    char texts[CAPTURE_MAX][WPE_LOG_MESSAGE_MAX];
    int lines[CAPTURE_MAX];
} Capture;

static void capture_sink(const WPE_LogMessage* message, void* user_data) {
    Capture* capture = user_data;
    if (capture->count >= CAPTURE_MAX) return;
    const int i = capture->count++;
    capture->levels[i] = message->level;
    capture->lines[i] = message->line;
    snprintf(capture->modules[i], sizeof capture->modules[i], "%s", message->module);
    snprintf(capture->texts[i], sizeof capture->texts[i], "%s", message->text);
}

/* Installs a capturing sink with stderr muted; every test pairs this with capture_end(). */
static WPE_LogSinkId capture_begin(Capture* capture) {
    *capture = (Capture){0};
    wpe_log_set_stderr_enabled(false);
    wpe_log_set_min_level(WPE_LOG_LEVEL_TRACE);
    WPE_LogSinkId id = 0;
    WPE_TEST_ASSERT_OK(wpe_log_add_sink(capture_sink, capture, &id));
    return id;
}

static void capture_end(WPE_LogSinkId id) {
    WPE_TEST_ASSERT_OK(wpe_log_remove_sink(id));
    wpe_log_set_stderr_enabled(true);
    wpe_log_set_min_level(WPE_LOG_LEVEL_DEBUG);
}

static void test_log_dispatches_formatted_message_with_module(void) {
    Capture capture;
    const WPE_LogSinkId id = capture_begin(&capture);

    const int expected_line = __LINE__ + 1;
    wpe_log_warn("value=%d name=%s", 42, "box");

    capture_end(id);
    WPE_TEST_ASSERT_EQ_INT(1, capture.count);
    WPE_TEST_ASSERT_EQ_INT(WPE_LOG_LEVEL_WARN, capture.levels[0]);
    WPE_TEST_ASSERT_EQ_STR("test", capture.modules[0]);
    WPE_TEST_ASSERT_EQ_STR("value=42 name=box", capture.texts[0]);
    WPE_TEST_ASSERT_EQ_INT(expected_line, capture.lines[0]);
}

static void test_log_filters_below_min_level(void) {
    Capture capture;
    const WPE_LogSinkId id = capture_begin(&capture);
    wpe_log_set_min_level(WPE_LOG_LEVEL_ERROR);

    wpe_log_trace("dropped");
    wpe_log_debug("dropped");
    wpe_log_info("dropped");
    wpe_log_warn("dropped");
    wpe_log_error("kept");
    wpe_log_critical("kept");

    capture_end(id);
    WPE_TEST_ASSERT_EQ_INT(2, capture.count);
    WPE_TEST_ASSERT_EQ_INT(WPE_LOG_LEVEL_ERROR, capture.levels[0]);
    WPE_TEST_ASSERT_EQ_INT(WPE_LOG_LEVEL_CRITICAL, capture.levels[1]);
}

static void test_log_truncates_long_messages(void) {
    Capture capture;
    const WPE_LogSinkId id = capture_begin(&capture);

    char long_text[WPE_LOG_MESSAGE_MAX * 2];
    memset(long_text, 'x', sizeof long_text - 1);
    long_text[sizeof long_text - 1] = '\0';
    wpe_log_info("%s", long_text);

    capture_end(id);
    WPE_TEST_ASSERT_EQ_INT(1, capture.count);
    WPE_TEST_ASSERT_EQ_UINT(WPE_LOG_MESSAGE_MAX - 1, strlen(capture.texts[0]));
}

static void test_log_removed_sink_receives_nothing(void) {
    Capture capture;
    const WPE_LogSinkId id = capture_begin(&capture);
    capture_end(id);

    wpe_log_set_stderr_enabled(false);
    wpe_log_error("after removal");
    wpe_log_set_stderr_enabled(true);

    WPE_TEST_ASSERT_EQ_INT(0, capture.count);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND, wpe_log_remove_sink(id));
}

static void test_log_add_sink_validates_arguments(void) {
    WPE_LogSinkId id = 0;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NULL_ARG, wpe_log_add_sink(NULL, NULL, &id));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NULL_ARG, wpe_log_add_sink(capture_sink, NULL, NULL));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND, wpe_log_remove_sink(0));
}

static void test_log_sink_capacity_is_enforced(void) {
    Capture capture = {0};
    WPE_LogSinkId ids[WPE_LOG_MAX_SINKS];
    for (int i = 0; i < WPE_LOG_MAX_SINKS; ++i) {
        WPE_TEST_ASSERT_OK(wpe_log_add_sink(capture_sink, &capture, &ids[i]));
    }

    WPE_LogSinkId extra = 0;
    const WPE_Result overflow = wpe_log_add_sink(capture_sink, &capture, &extra);

    for (int i = 0; i < WPE_LOG_MAX_SINKS; ++i) {
        WPE_TEST_ASSERT_OK(wpe_log_remove_sink(ids[i]));
    }
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_CAPACITY, overflow);
}

static void test_log_ids_are_unique(void) {
    Capture capture = {0};
    WPE_LogSinkId a = 0, b = 0;
    WPE_TEST_ASSERT_OK(wpe_log_add_sink(capture_sink, &capture, &a));
    WPE_TEST_ASSERT_OK(wpe_log_remove_sink(a));
    WPE_TEST_ASSERT_OK(wpe_log_add_sink(capture_sink, &capture, &b));
    WPE_TEST_ASSERT_OK(wpe_log_remove_sink(b));
    WPE_TEST_ASSERT(a != 0 && b != 0 && a != b);
}

static void test_log_level_names(void) {
    WPE_TEST_ASSERT_EQ_STR("TRACE", wpe_log_level_name(WPE_LOG_LEVEL_TRACE));
    WPE_TEST_ASSERT_EQ_STR("CRITICAL", wpe_log_level_name(WPE_LOG_LEVEL_CRITICAL));
    WPE_TEST_ASSERT_EQ_STR("?", wpe_log_level_name(WPE_LOG_LEVEL_COUNT));
}

static void test_log_min_level_ignores_invalid_values(void) {
    const WPE_LogLevel before = wpe_log_get_min_level();
    wpe_log_set_min_level(WPE_LOG_LEVEL_COUNT);
    WPE_TEST_ASSERT_EQ_INT(before, wpe_log_get_min_level());
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_log_dispatches_formatted_message_with_module),
              WPE_TEST_CASE(test_log_filters_below_min_level),
              WPE_TEST_CASE(test_log_truncates_long_messages),
              WPE_TEST_CASE(test_log_removed_sink_receives_nothing),
              WPE_TEST_CASE(test_log_add_sink_validates_arguments),
              WPE_TEST_CASE(test_log_sink_capacity_is_enforced), WPE_TEST_CASE(test_log_ids_are_unique),
              WPE_TEST_CASE(test_log_level_names), WPE_TEST_CASE(test_log_min_level_ignores_invalid_values))
