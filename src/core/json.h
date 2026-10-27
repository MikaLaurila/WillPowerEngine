/**
 * @file json.h
 * @brief Strict JSON (RFC 8259) reading into an immutable document, and pretty-printed writing.
 *
 * Used for human- and tool-editable engine files (scenes, project settings). The reader is
 * strict so mistakes are reported instead of silently ignored: duplicate object keys, trailing
 * commas, comments, invalid escapes or UTF-8, "\u0000" (strings are C strings) and nesting deeper
 * than WPE_JSON_MAX_DEPTH are errors that report the line and column. Object members keep their
 * file order.
 *
 * Numbers are parsed with strtod and written with "%.9g" (round-trips every float exactly);
 * both assume the process uses the default "C" numeric locale (the engine never changes it).
 *
 * @code
 * WPE_JsonDocument* doc = NULL;
 * WPE_JsonError error;
 * if (wpe_json_parse(text, size, &doc, &error) == WPE_RESULT_OK) {
 *     const WPE_JsonValue* width = wpe_json_get(wpe_json_root(doc), "width");
 *     double w = wpe_json_number(width, 320.0);
 *     wpe_json_free(doc);
 * }
 * @endcode
 *
 * - Dependencies: core (alloc, arena)
 * - Thread affinity: a document may be read from any number of threads; a writer belongs to one
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_CORE_JSON_H
#define WPE_CORE_JSON_H

#include <stddef.h>
#include <stdint.h>

#include "core/result.h"

/** Maximum nesting of arrays and objects accepted by the reader and writer. */
#define WPE_JSON_MAX_DEPTH 64u

typedef enum WPE_JsonType {
    WPE_JSON_NULL = 0,
    WPE_JSON_BOOL,
    WPE_JSON_NUMBER,
    WPE_JSON_STRING,
    WPE_JSON_ARRAY,
    WPE_JSON_OBJECT,
} WPE_JsonType;

typedef struct WPE_JsonDocument WPE_JsonDocument;
typedef struct WPE_JsonValue WPE_JsonValue;

typedef struct WPE_JsonError {
    uint32_t line;     /* 1-based; 0 when the error has no position (e.g. out of memory). */
    uint32_t column;   /* 1-based, in bytes. */
    char message[128]; /* Empty on success. */
} WPE_JsonError;

/**
 * Parses a complete JSON text (one value, optionally surrounded by whitespace).
 *
 * @param text UTF-8 text of `size` bytes (need not be NUL-terminated; a leading UTF-8 BOM is skipped).
 * @param out_doc Receives the document; free with wpe_json_free().
 * @param out_error Optional; receives the position and description of a syntax error.
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_PARSE (not logged, see
 *         out_error); WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Any thread
 */
[[nodiscard]] WPE_Result wpe_json_parse(const char* text, size_t size, WPE_JsonDocument** out_doc,
                                        WPE_JsonError* out_error);

/** Frees a document and every value in it (ownership consumed); NULL is a no-op. @thread_safety Any thread */
void wpe_json_free(WPE_JsonDocument* doc);

/** @return The document's top-level value (borrowed from the document). @thread_safety Any thread */
const WPE_JsonValue* wpe_json_root(const WPE_JsonDocument* doc);

/** @return The value's type; WPE_JSON_NULL for a NULL value pointer. @thread_safety Any thread */
WPE_JsonType wpe_json_type(const WPE_JsonValue* value);

/** @return Line (1-based) where the value starts in the parsed text, or 0 for NULL. @thread_safety Any thread
 */
uint32_t wpe_json_line(const WPE_JsonValue* value);

/** @return The boolean, or `fallback` if the value is not a boolean. @thread_safety Any thread */
bool wpe_json_bool(const WPE_JsonValue* value, bool fallback);

/** @return The number, or `fallback` if the value is not a number. @thread_safety Any thread */
double wpe_json_number(const WPE_JsonValue* value, double fallback);

/** @return The NUL-terminated string (borrowed from the document), or NULL if not a string. @thread_safety
 * Any thread */
const char* wpe_json_string(const WPE_JsonValue* value);

/** @return Number of elements of an array or members of an object; 0 for other values. @thread_safety Any
 * thread */
uint32_t wpe_json_count(const WPE_JsonValue* value);

/** @return Element `index` of an array, or NULL if out of range or not an array. @thread_safety Any thread */
const WPE_JsonValue* wpe_json_at(const WPE_JsonValue* value, uint32_t index);

/** @return The member value named `key` of an object, or NULL. O(n). @thread_safety Any thread */
const WPE_JsonValue* wpe_json_get(const WPE_JsonValue* object, const char* key);

/**
 * Accesses object members in file order.
 *
 * @param out_key Optional; receives the member name (borrowed from the document).
 * @return The member value, or NULL if out of range or not an object.
 *
 * @thread_safety Any thread
 */
const WPE_JsonValue* wpe_json_member(const WPE_JsonValue* object, uint32_t index, const char** out_key);

/* ---- Writing ------------------------------------------------------------------------------------- */

typedef struct WPE_JsonWriter WPE_JsonWriter;

/** @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or WPE_RESULT_ERROR_OUT_OF_MEMORY. @thread_safety Any
 * thread */
[[nodiscard]] WPE_Result wpe_json_writer_create(WPE_JsonWriter** out_writer);

/** Frees a writer and its text (ownership consumed); NULL is a no-op. @thread_safety Owning thread */
void wpe_json_writer_destroy(WPE_JsonWriter* writer);

/*
 * Value emitters. Inside an object every value must be preceded by wpe_json_write_key(). Misuse
 * (a value without a key in an object, unbalanced end calls, nesting deeper than
 * WPE_JSON_MAX_DEPTH, non-finite numbers, invalid UTF-8) and allocation failures put the writer
 * into a failed state reported by wpe_json_writer_finish(); further calls are ignored.
 *
 * Arrays started with `inline_items` print on one line ("[1, 2, 3]"); use it for short vectors.
 */
void wpe_json_write_object_begin(WPE_JsonWriter* writer);
void wpe_json_write_object_end(WPE_JsonWriter* writer);
void wpe_json_write_array_begin(WPE_JsonWriter* writer, bool inline_items);
void wpe_json_write_array_end(WPE_JsonWriter* writer);
void wpe_json_write_key(WPE_JsonWriter* writer, const char* key);
void wpe_json_write_string(WPE_JsonWriter* writer, const char* value);
void wpe_json_write_number(WPE_JsonWriter* writer, double value); /* "%.9g": exact for floats. */
void wpe_json_write_double(WPE_JsonWriter* writer, double value); /* "%.17g": exact for doubles. */
/* Rounded to `significant_digits` (1..17), for derived values that should stay readable ("60" degrees
 * rather than "59.9999986"); reading and rewriting such a value reproduces the same text. */
void wpe_json_write_rounded(WPE_JsonWriter* writer, double value, int significant_digits);
void wpe_json_write_integer(WPE_JsonWriter* writer, int64_t value);
void wpe_json_write_bool(WPE_JsonWriter* writer, bool value);
void wpe_json_write_null(WPE_JsonWriter* writer);

/**
 * Completes the text (exactly one top-level value must have been written).
 *
 * @param out_text Receives the NUL-terminated text, ending with a newline (borrowed from the writer
 *                 until it is destroyed).
 * @param out_size Optional; receives the length in bytes without the terminator.
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_INVALID_STATE (misuse or an
 *         incomplete document; logged); WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Owning thread
 */
WPE_Result wpe_json_writer_finish(WPE_JsonWriter* writer, const char** out_text, size_t* out_size);

#endif /* WPE_CORE_JSON_H */
