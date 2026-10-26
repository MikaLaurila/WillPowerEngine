/**
 * @file result.h
 * @brief Engine-wide status codes.
 *
 * Every fallible public function returns a WPE_Result. WPE_RESULT_OK is zero so results can be
 * tested with `if (result != WPE_RESULT_OK)`.
 *
 * - Dependencies: none
 * - Thread affinity: any thread
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_CORE_RESULT_H
#define WPE_CORE_RESULT_H

typedef enum WPE_Result {
    WPE_RESULT_OK = 0,
    WPE_RESULT_ERROR,                /* Unspecified failure; prefer a specific code. */
    WPE_RESULT_ERROR_NULL_ARG,       /* A required pointer argument was NULL. */
    WPE_RESULT_ERROR_INVALID_ARG,    /* An argument was outside its valid range. */
    WPE_RESULT_ERROR_OUT_OF_MEMORY,  /* An allocation failed. */
    WPE_RESULT_ERROR_INVALID_STATE,  /* The call is not valid in the object's current state. */
    WPE_RESULT_ERROR_NOT_FOUND,      /* A requested item does not exist. */
    WPE_RESULT_ERROR_ALREADY_EXISTS, /* An item with the same identity already exists. */
    WPE_RESULT_ERROR_CAPACITY,       /* A fixed capacity limit was reached. */
    WPE_RESULT_ERROR_IO,             /* A file or stream operation failed. */
    WPE_RESULT_ERROR_PARSE,          /* Input data was malformed. */
    WPE_RESULT_ERROR_UNSUPPORTED,    /* The feature or format is not supported. */
    WPE_RESULT_COUNT
} WPE_Result;

/**
 * Returns a static, human-readable name for a result code.
 *
 * @param result Any value; out-of-range values yield "WPE_RESULT_<invalid>".
 * @return Pointer to a string literal; never NULL. Caller must not free it.
 *
 * @thread_safety Any thread
 */
const char* wpe_result_to_string(WPE_Result result);

#endif /* WPE_CORE_RESULT_H */
