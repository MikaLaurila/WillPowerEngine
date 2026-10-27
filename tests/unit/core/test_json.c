#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "wpe_test.h"

static WPE_JsonDocument* parse_ok(const char* text) {
    WPE_JsonDocument* doc = NULL;
    WPE_JsonError error;
    const WPE_Result result = wpe_json_parse(text, strlen(text), &doc, &error);
    WPE_TEST_ASSERT_MSG(result == WPE_RESULT_OK, "parse failed at %u:%u: %s", error.line, error.column,
                        error.message);
    return doc;
}

/* Expects a syntax error at the given line and column. */
static void expect_error(const char* text, uint32_t line, uint32_t column) {
    WPE_JsonDocument* doc = NULL;
    WPE_JsonError error;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_PARSE, wpe_json_parse(text, strlen(text), &doc, &error));
    WPE_TEST_ASSERT(doc == NULL);
    WPE_TEST_ASSERT(error.message[0] != '\0');
    WPE_TEST_ASSERT_MSG(error.line == line && error.column == column, "'%s': expected %u:%u, got %u:%u (%s)",
                        text, line, column, error.line, error.column, error.message);
}

static void test_json_parses_all_value_types(void) {
    WPE_JsonDocument* doc =
        parse_ok("{\"n\": -12.5e1, \"t\": true, \"f\": false, \"z\": null, \"s\": \"hi\",\n"
                 " \"a\": [1, [2], {}], \"o\": {\"x\": 0}}");
    const WPE_JsonValue* root = wpe_json_root(doc);
    WPE_TEST_ASSERT_EQ_INT(WPE_JSON_OBJECT, wpe_json_type(root));
    WPE_TEST_ASSERT_EQ_UINT(7, wpe_json_count(root));
    WPE_TEST_ASSERT_NEAR(-125.0, wpe_json_number(wpe_json_get(root, "n"), 0.0), 0.0);
    WPE_TEST_ASSERT(wpe_json_bool(wpe_json_get(root, "t"), false));
    WPE_TEST_ASSERT(!wpe_json_bool(wpe_json_get(root, "f"), true));
    WPE_TEST_ASSERT_EQ_INT(WPE_JSON_NULL, wpe_json_type(wpe_json_get(root, "z")));
    WPE_TEST_ASSERT(wpe_json_get(root, "z") != NULL); /* Present, unlike a missing key. */
    WPE_TEST_ASSERT(wpe_json_get(root, "missing") == NULL);
    WPE_TEST_ASSERT_EQ_STR("hi", wpe_json_string(wpe_json_get(root, "s")));
    const WPE_JsonValue* a = wpe_json_get(root, "a");
    WPE_TEST_ASSERT_EQ_UINT(3, wpe_json_count(a));
    WPE_TEST_ASSERT_NEAR(2.0, wpe_json_number(wpe_json_at(wpe_json_at(a, 1), 0), 0.0), 0.0);
    WPE_TEST_ASSERT_EQ_INT(WPE_JSON_OBJECT, wpe_json_type(wpe_json_at(a, 2)));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_json_count(wpe_json_at(a, 2)));
    WPE_TEST_ASSERT(wpe_json_at(a, 3) == NULL);
    WPE_TEST_ASSERT_EQ_UINT(2, wpe_json_line(a));

    /* Members keep file order. */
    const char* key = NULL;
    WPE_TEST_ASSERT(wpe_json_member(root, 0, &key) != NULL);
    WPE_TEST_ASSERT_EQ_STR("n", key);
    WPE_TEST_ASSERT(wpe_json_member(root, 6, &key) != NULL);
    WPE_TEST_ASSERT_EQ_STR("o", key);
    WPE_TEST_ASSERT(wpe_json_member(root, 7, &key) == NULL);

    /* Accessors are type-checked and NULL-safe. */
    WPE_TEST_ASSERT_NEAR(7.0, wpe_json_number(wpe_json_get(root, "s"), 7.0), 0.0);
    WPE_TEST_ASSERT(wpe_json_string(wpe_json_get(root, "n")) == NULL);
    WPE_TEST_ASSERT(wpe_json_get(a, "x") == NULL);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_json_count(NULL));
    wpe_json_free(doc);
}

static void test_json_decodes_escapes_and_utf8(void) {
    WPE_JsonDocument* doc =
        parse_ok("[\"a\\\"b\\\\c\\/d\\b\\f\\n\\r\\t\", \"\\u00e4\\u20AC\\ud83d\\ude00\", \"\xc3\xa4\"]");
    const WPE_JsonValue* root = wpe_json_root(doc);
    WPE_TEST_ASSERT_EQ_STR("a\"b\\c/d\b\f\n\r\t", wpe_json_string(wpe_json_at(root, 0)));
    WPE_TEST_ASSERT_EQ_STR("\xc3\xa4\xe2\x82\xac\xf0\x9f\x98\x80", wpe_json_string(wpe_json_at(root, 1)));
    WPE_TEST_ASSERT_EQ_STR("\xc3\xa4", wpe_json_string(wpe_json_at(root, 2)));
    wpe_json_free(doc);

    /* A UTF-8 byte order mark is skipped. */
    doc = parse_ok("\xEF\xBB\xBF 5");
    WPE_TEST_ASSERT_NEAR(5.0, wpe_json_number(wpe_json_root(doc), 0.0), 0.0);
    wpe_json_free(doc);
}

static void test_json_reports_errors_with_positions(void) {
    expect_error("", 1, 1);
    expect_error("{\"a\": 1,}", 1, 9);           /* Trailing comma. */
    expect_error("[1, 2,]", 1, 7);               /* Trailing comma. */
    expect_error("{\"a\": 1, \"a\": 2}", 1, 13); /* Duplicate key. */
    expect_error("{\n  \"a\": tru\n}", 2, 8);
    expect_error("[01]", 1, 3);
    expect_error("[1.]", 1, 2);
    expect_error("[1e]", 1, 2);
    expect_error("1e999", 1, 1);
    expect_error("\"a\nb\"", 1, 3);         /* Raw control character. */
    expect_error("\"\\x\"", 1, 2);          /* Invalid escape. */
    expect_error("\"\\u0000\"", 1, 8);      /* NUL cannot be represented in C strings. */
    expect_error("\"\\ud800\"", 1, 8);      /* Lone surrogate. */
    expect_error("\"\xc0\xaf\"", 1, 2);     /* Overlong UTF-8. */
    expect_error("\"\xed\xa0\x80\"", 1, 2); /* Encoded surrogate. */
    expect_error("// comment\n1", 1, 1);
    expect_error("1 2", 1, 3);
    expect_error("{\"a\" 1}", 1, 6);
    expect_error("\"abc", 1, 5);

    /* Nesting is limited. */
    char deep[WPE_JSON_MAX_DEPTH * 2 + 8];
    size_t n = 0;
    for (uint32_t i = 0; i <= WPE_JSON_MAX_DEPTH; ++i)
        deep[n++] = '[';
    for (uint32_t i = 0; i <= WPE_JSON_MAX_DEPTH; ++i)
        deep[n++] = ']';
    deep[n] = '\0';
    expect_error(deep, 1, WPE_JSON_MAX_DEPTH + 1);
    deep[WPE_JSON_MAX_DEPTH] = ' '; /* One level less is fine. */
    deep[WPE_JSON_MAX_DEPTH + 1] = ' ';
    WPE_JsonDocument* doc = parse_ok(deep);
    wpe_json_free(doc);
}

static void test_json_parse_rejects_null_arguments(void) {
    WPE_JsonDocument* doc = NULL;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NULL_ARG, wpe_json_parse(NULL, 0, &doc, NULL));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NULL_ARG, wpe_json_parse("1", 1, NULL, NULL));
    /* Text need not be NUL-terminated: only `size` bytes are read. */
    WPE_TEST_ASSERT_OK(wpe_json_parse("12345", 2, &doc, NULL));
    WPE_TEST_ASSERT_NEAR(12.0, wpe_json_number(wpe_json_root(doc), 0.0), 0.0);
    wpe_json_free(doc);
    wpe_json_free(NULL);
}

static void test_json_writer_pretty_prints(void) {
    WPE_JsonWriter* w = NULL;
    WPE_TEST_ASSERT_OK(wpe_json_writer_create(&w));
    wpe_json_write_object_begin(w);
    wpe_json_write_key(w, "name");
    wpe_json_write_string(w, "q\"\n\x01\xc3\xa4");
    wpe_json_write_key(w, "v");
    wpe_json_write_array_begin(w, true);
    wpe_json_write_number(w, 1.0);
    wpe_json_write_number(w, 0.1f);
    wpe_json_write_integer(w, -3);
    wpe_json_write_array_end(w);
    wpe_json_write_key(w, "list");
    wpe_json_write_array_begin(w, false);
    wpe_json_write_bool(w, true);
    wpe_json_write_null(w);
    wpe_json_write_object_begin(w);
    wpe_json_write_object_end(w);
    wpe_json_write_array_end(w);
    wpe_json_write_key(w, "empty");
    wpe_json_write_array_begin(w, false);
    wpe_json_write_array_end(w);
    wpe_json_write_object_end(w);
    const char* text = NULL;
    size_t size = 0;
    WPE_TEST_ASSERT_OK(wpe_json_writer_finish(w, &text, &size));
    WPE_TEST_ASSERT_EQ_STR("{\n"
                           "  \"name\": \"q\\\"\\n\\u0001\xc3\xa4\",\n"
                           "  \"v\": [1, 0.100000001, -3],\n"
                           "  \"list\": [\n"
                           "    true,\n"
                           "    null,\n"
                           "    {}\n"
                           "  ],\n"
                           "  \"empty\": []\n"
                           "}\n",
                           text);
    WPE_TEST_ASSERT_EQ_UINT(strlen(text), size);
    /* Finishing again returns the same text. */
    const char* again = NULL;
    WPE_TEST_ASSERT_OK(wpe_json_writer_finish(w, &again, NULL));
    WPE_TEST_ASSERT(again == text);
    wpe_json_writer_destroy(w);
}

static void test_json_writer_compact(void) {
    WPE_JsonWriter* w = NULL;
    WPE_TEST_ASSERT_OK(wpe_json_writer_create(&w));
    wpe_json_writer_set_compact(w, true);
    wpe_json_write_object_begin(w);
    wpe_json_write_key(w, "a");
    wpe_json_write_array_begin(w, false);
    wpe_json_write_integer(w, 1);
    wpe_json_write_string(w, "x\ny");
    wpe_json_write_array_end(w);
    wpe_json_write_object_end(w);
    const char* text = NULL;
    WPE_TEST_ASSERT_OK(wpe_json_writer_finish(w, &text, NULL));
    WPE_TEST_ASSERT_EQ_STR("{ \"a\": [ 1, \"x\\ny\" ] }\n", text);
    wpe_json_writer_destroy(w);
}

static void test_json_writer_round_trips_floats(void) {
    WPE_JsonWriter* w = NULL;
    WPE_TEST_ASSERT_OK(wpe_json_writer_create(&w));
    const float values[] = {0.1f, 1.0f / 3.0f, -1e-30f, 3.4028235e38f, 1e-45f, 16777217.0f, -0.0f};
    const double precise = 0.1 + 0.2;
    wpe_json_write_array_begin(w, true);
    for (size_t i = 0; i < sizeof values / sizeof values[0]; ++i)
        wpe_json_write_number(w, values[i]);
    wpe_json_write_double(w, precise);
    wpe_json_write_array_end(w);
    const char* text = NULL;
    WPE_TEST_ASSERT_OK(wpe_json_writer_finish(w, &text, NULL));
    WPE_JsonDocument* doc = parse_ok(text);
    for (uint32_t i = 0; i < sizeof values / sizeof values[0]; ++i) {
        const float parsed = (float)wpe_json_number(wpe_json_at(wpe_json_root(doc), i), NAN);
        WPE_TEST_ASSERT_MSG(memcmp(&parsed, &values[i], sizeof parsed) == 0, "value %u changed", i);
    }
    WPE_TEST_ASSERT(wpe_json_number(wpe_json_at(wpe_json_root(doc), 7), 0.0) == precise);
    wpe_json_free(doc);
    wpe_json_writer_destroy(w);
}

static void test_json_writer_detects_misuse(void) {
    struct {
        const char* name;
        int scenario;
    } cases[] = {{"value without key", 0}, {"unbalanced", 1}, {"two roots", 2},    {"incomplete", 3},
                 {"non-finite", 4},        {"bad utf8", 5},   {"key in array", 6}, {"nothing", 7}};
    wpe_log_set_stderr_enabled(false);
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        WPE_JsonWriter* w = NULL;
        WPE_TEST_ASSERT_OK(wpe_json_writer_create(&w));
        switch (cases[i].scenario) {
        case 0:
            wpe_json_write_object_begin(w);
            wpe_json_write_integer(w, 1);
            wpe_json_write_object_end(w);
            break;
        case 1:
            wpe_json_write_array_begin(w, false);
            wpe_json_write_object_end(w);
            break;
        case 2:
            wpe_json_write_null(w);
            wpe_json_write_null(w);
            break;
        case 3:
            wpe_json_write_array_begin(w, false);
            break;
        case 4:
            wpe_json_write_number(w, INFINITY);
            break;
        case 5:
            wpe_json_write_string(w, "\xff");
            break;
        case 6:
            wpe_json_write_array_begin(w, false);
            wpe_json_write_key(w, "k");
            wpe_json_write_array_end(w);
            break;
        default:
            break;
        }
        const char* text = NULL;
        WPE_TEST_ASSERT_MSG(wpe_json_writer_finish(w, &text, NULL) == WPE_RESULT_ERROR_INVALID_STATE,
                            "%s not detected", cases[i].name);
        WPE_TEST_ASSERT(text == NULL);
        wpe_json_writer_destroy(w);
    }
    wpe_log_set_stderr_enabled(true);
}

static void test_json_large_document(void) {
    /* Many nested containers exercise the pending-value stack growth. */
    WPE_JsonWriter* w = NULL;
    WPE_TEST_ASSERT_OK(wpe_json_writer_create(&w));
    wpe_json_write_array_begin(w, false);
    for (int i = 0; i < 2000; ++i) {
        wpe_json_write_object_begin(w);
        wpe_json_write_key(w, "id");
        wpe_json_write_integer(w, i);
        wpe_json_write_key(w, "v");
        wpe_json_write_array_begin(w, true);
        for (int j = 0; j < 3; ++j)
            wpe_json_write_number(w, i * 3 + j);
        wpe_json_write_array_end(w);
        wpe_json_write_object_end(w);
    }
    wpe_json_write_array_end(w);
    const char* text = NULL;
    size_t size = 0;
    WPE_TEST_ASSERT_OK(wpe_json_writer_finish(w, &text, &size));
    WPE_JsonDocument* doc = NULL;
    WPE_TEST_ASSERT_OK(wpe_json_parse(text, size, &doc, NULL));
    const WPE_JsonValue* root = wpe_json_root(doc);
    WPE_TEST_ASSERT_EQ_UINT(2000, wpe_json_count(root));
    for (uint32_t i = 0; i < 2000; i += 199) {
        const WPE_JsonValue* item = wpe_json_at(root, i);
        WPE_TEST_ASSERT_NEAR(i, wpe_json_number(wpe_json_get(item, "id"), -1), 0.0);
        WPE_TEST_ASSERT_NEAR(i * 3 + 2, wpe_json_number(wpe_json_at(wpe_json_get(item, "v"), 2), -1), 0.0);
    }
    wpe_json_free(doc);
    wpe_json_writer_destroy(w);
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_json_parses_all_value_types),
              WPE_TEST_CASE(test_json_decodes_escapes_and_utf8),
              WPE_TEST_CASE(test_json_reports_errors_with_positions),
              WPE_TEST_CASE(test_json_parse_rejects_null_arguments),
              WPE_TEST_CASE(test_json_writer_pretty_prints),
              WPE_TEST_CASE(test_json_writer_round_trips_floats), WPE_TEST_CASE(test_json_writer_compact),
              WPE_TEST_CASE(test_json_writer_detects_misuse), WPE_TEST_CASE(test_json_large_document))
