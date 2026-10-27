/**
 * @file json_read.h
 * @brief Private helpers for strict reading of engine JSON files (scenes, projects): typed,
 *        range-checked field access whose first error is logged with the file name and line.
 *
 * Every function returns false after recording an error (and does nothing once an error has been
 * recorded), so loaders chain them with &&. Optional fields that are absent leave the output
 * untouched, so outputs are pre-filled with defaults.
 *
 * - Dependencies: core (JSON, log)
 * - Thread affinity: one reader per thread
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_ENGINE_JSON_READ_H
#define WPE_ENGINE_JSON_READ_H

#include <stdint.h>

#include "core/json.h"
#include "core/log.h"
#include "core/result.h"
#include "math/wpe_math.h"

typedef struct wpe__JsonReader {
    const char* name;  /* File name for messages. */
    const char* kind;  /* "scene", "project", ... */
    WPE_Result result; /* First error (WPE_RESULT_OK while reading succeeds). */
} wpe__JsonReader;

/* Records the first error (logging "<kind> '<name>', line N: message") and returns false. */
WPE_PRINTF_LIKE(4, 5)
bool wpe__json_fail(wpe__JsonReader* r, const WPE_JsonValue* at, WPE_Result code, const char* format, ...);

/* Checks that `value` is an object whose keys are all in `allowed`. */
bool wpe__json_check_object(wpe__JsonReader* r, const WPE_JsonValue* value, const char* what,
                            const char* const* allowed, uint32_t allowed_count);

#define WPE__JSON_CHECK_OBJECT(r, value, what, ...)                                                          \
    wpe__json_check_object((r), (value), (what), (const char* const[]){__VA_ARGS__},                         \
                           (uint32_t)(sizeof((const char* const[]){__VA_ARGS__}) / sizeof(const char*)))

/* Optional numbers in [min, max]. */
bool wpe__json_read_double(wpe__JsonReader* r, const WPE_JsonValue* object, const char* key, double min,
                           double max, double* io);
bool wpe__json_read_float(wpe__JsonReader* r, const WPE_JsonValue* object, const char* key, double min,
                          double max, float* io);
/* Degrees in the file, radians in memory. */
bool wpe__json_read_degrees(wpe__JsonReader* r, const WPE_JsonValue* object, const char* key,
                            double min_degrees, double max_degrees, float* io_radians);
/* Optional whole number in [min, max]. */
bool wpe__json_read_uint(wpe__JsonReader* r, const WPE_JsonValue* object, const char* key, uint32_t min,
                         uint32_t max, uint32_t* io);
bool wpe__json_read_bool(wpe__JsonReader* r, const WPE_JsonValue* object, const char* key, bool* io);
/* Exactly `count` numbers from an array value (not a key). */
bool wpe__json_read_floats(wpe__JsonReader* r, const WPE_JsonValue* value, const char* key, uint32_t count,
                           float* out);
bool wpe__json_read_vec3(wpe__JsonReader* r, const WPE_JsonValue* object, const char* key, WPE_Vec3* io);
/* Optional string naming one of `names`; *io receives its index. */
bool wpe__json_read_enum(wpe__JsonReader* r, const WPE_JsonValue* object, const char* key,
                         const char* const* names, uint32_t count, uint32_t* io);
/* Optional string copied into `out` (capacity bytes); too long is an error. */
bool wpe__json_read_string(wpe__JsonReader* r, const WPE_JsonValue* object, const char* key, char* out,
                           size_t capacity);

#endif /* WPE_ENGINE_JSON_READ_H */
