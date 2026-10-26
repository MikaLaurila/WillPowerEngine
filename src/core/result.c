#include "core/result.h"

static const char* const wpe__result_names[] = {
    [WPE_RESULT_OK] = "WPE_RESULT_OK",
    [WPE_RESULT_ERROR] = "WPE_RESULT_ERROR",
    [WPE_RESULT_ERROR_NULL_ARG] = "WPE_RESULT_ERROR_NULL_ARG",
    [WPE_RESULT_ERROR_INVALID_ARG] = "WPE_RESULT_ERROR_INVALID_ARG",
    [WPE_RESULT_ERROR_OUT_OF_MEMORY] = "WPE_RESULT_ERROR_OUT_OF_MEMORY",
    [WPE_RESULT_ERROR_INVALID_STATE] = "WPE_RESULT_ERROR_INVALID_STATE",
    [WPE_RESULT_ERROR_NOT_FOUND] = "WPE_RESULT_ERROR_NOT_FOUND",
    [WPE_RESULT_ERROR_ALREADY_EXISTS] = "WPE_RESULT_ERROR_ALREADY_EXISTS",
    [WPE_RESULT_ERROR_CAPACITY] = "WPE_RESULT_ERROR_CAPACITY",
    [WPE_RESULT_ERROR_IO] = "WPE_RESULT_ERROR_IO",
    [WPE_RESULT_ERROR_PARSE] = "WPE_RESULT_ERROR_PARSE",
    [WPE_RESULT_ERROR_UNSUPPORTED] = "WPE_RESULT_ERROR_UNSUPPORTED",
};

static_assert(sizeof(wpe__result_names) / sizeof(wpe__result_names[0]) == WPE_RESULT_COUNT,
              "every WPE_Result needs a name");

const char* wpe_result_to_string(WPE_Result result) {
    if ((unsigned)result >= WPE_RESULT_COUNT) return "WPE_RESULT_<invalid>";
    return wpe__result_names[result];
}
