#define WPE_LOG_MODULE "engine"
#include "engine/json_read.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define WPE__DEG_PER_RAD (180.0 / 3.14159265358979323846)

bool wpe__json_fail(wpe__JsonReader* r, const WPE_JsonValue* at, WPE_Result code, const char* format, ...) {
    if (r->result != WPE_RESULT_OK) return false;
    r->result = code;
    char message[384];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof message, format, args);
    va_end(args);
    wpe_log_error("%s '%s', line %u: %s", r->kind, r->name, wpe_json_line(at), message);
    return false;
}

bool wpe__json_check_object(wpe__JsonReader* r, const WPE_JsonValue* value, const char* what,
                            const char* const* allowed, uint32_t allowed_count) {
    if (r->result != WPE_RESULT_OK) return false;
    if (wpe_json_type(value) != WPE_JSON_OBJECT)
        return wpe__json_fail(r, value, WPE_RESULT_ERROR_PARSE, "%s must be an object", what);
    for (uint32_t i = 0; i < wpe_json_count(value); ++i) {
        const char* key = NULL;
        const WPE_JsonValue* member = wpe_json_member(value, i, &key);
        bool known = false;
        for (uint32_t k = 0; k < allowed_count && !known; ++k)
            known = strcmp(key, allowed[k]) == 0;
        if (!known)
            return wpe__json_fail(r, member, WPE_RESULT_ERROR_PARSE, "%s: unknown key \"%s\"", what, key);
    }
    return true;
}

bool wpe__json_read_double(wpe__JsonReader* r, const WPE_JsonValue* object, const char* key, double min,
                           double max, double* io) {
    if (r->result != WPE_RESULT_OK) return false;
    const WPE_JsonValue* v = wpe_json_get(object, key);
    if (!v) return true;
    if (wpe_json_type(v) != WPE_JSON_NUMBER)
        return wpe__json_fail(r, v, WPE_RESULT_ERROR_PARSE, "\"%s\" must be a number", key);
    const double number = wpe_json_number(v, 0.0);
    if (number < min || number > max)
        return wpe__json_fail(r, v, WPE_RESULT_ERROR_PARSE, "\"%s\" must be between %g and %g", key, min,
                              max);
    *io = number;
    return true;
}

bool wpe__json_read_float(wpe__JsonReader* r, const WPE_JsonValue* object, const char* key, double min,
                          double max, float* io) {
    double value = *io;
    if (!wpe__json_read_double(r, object, key, min, max, &value)) return false;
    *io = (float)value;
    return true;
}

bool wpe__json_read_degrees(wpe__JsonReader* r, const WPE_JsonValue* object, const char* key,
                            double min_degrees, double max_degrees, float* io_radians) {
    double degrees = (double)*io_radians * WPE__DEG_PER_RAD;
    if (!wpe__json_read_double(r, object, key, min_degrees, max_degrees, &degrees)) return false;
    *io_radians = (float)(degrees / WPE__DEG_PER_RAD);
    return true;
}

bool wpe__json_read_uint(wpe__JsonReader* r, const WPE_JsonValue* object, const char* key, uint32_t min,
                         uint32_t max, uint32_t* io) {
    if (r->result != WPE_RESULT_OK) return false;
    const WPE_JsonValue* v = wpe_json_get(object, key);
    if (!v) return true;
    const double number = wpe_json_number(v, -1.0);
    if (wpe_json_type(v) != WPE_JSON_NUMBER || number != floor(number) || number < min || number > max)
        return wpe__json_fail(r, v, WPE_RESULT_ERROR_PARSE, "\"%s\" must be a whole number from %u to %u",
                              key, min, max);
    *io = (uint32_t)number;
    return true;
}

bool wpe__json_read_bool(wpe__JsonReader* r, const WPE_JsonValue* object, const char* key, bool* io) {
    if (r->result != WPE_RESULT_OK) return false;
    const WPE_JsonValue* v = wpe_json_get(object, key);
    if (!v) return true;
    if (wpe_json_type(v) != WPE_JSON_BOOL)
        return wpe__json_fail(r, v, WPE_RESULT_ERROR_PARSE, "\"%s\" must be true or false", key);
    *io = wpe_json_bool(v, false);
    return true;
}

bool wpe__json_read_floats(wpe__JsonReader* r, const WPE_JsonValue* value, const char* key, uint32_t count,
                           float* out) {
    if (r->result != WPE_RESULT_OK) return false;
    if (wpe_json_type(value) != WPE_JSON_ARRAY || wpe_json_count(value) != count)
        return wpe__json_fail(r, value, WPE_RESULT_ERROR_PARSE, "\"%s\" must be an array of %u numbers", key,
                              count);
    for (uint32_t i = 0; i < count; ++i) {
        const WPE_JsonValue* item = wpe_json_at(value, i);
        const double number = wpe_json_number(item, 0.0);
        if (wpe_json_type(item) != WPE_JSON_NUMBER || fabs(number) > 3.0e38)
            return wpe__json_fail(r, item, WPE_RESULT_ERROR_PARSE,
                                  "\"%s\" must contain only (float-sized) numbers", key);
        out[i] = (float)number;
    }
    return true;
}

bool wpe__json_read_vec3(wpe__JsonReader* r, const WPE_JsonValue* object, const char* key, WPE_Vec3* io) {
    if (r->result != WPE_RESULT_OK) return false;
    const WPE_JsonValue* v = wpe_json_get(object, key);
    return !v || wpe__json_read_floats(r, v, key, 3, io->v);
}

bool wpe__json_read_enum(wpe__JsonReader* r, const WPE_JsonValue* object, const char* key,
                         const char* const* names, uint32_t count, uint32_t* io) {
    if (r->result != WPE_RESULT_OK) return false;
    const WPE_JsonValue* v = wpe_json_get(object, key);
    if (!v) return true;
    const char* text = wpe_json_string(v);
    for (uint32_t i = 0; text && i < count; ++i) {
        if (strcmp(text, names[i]) == 0) {
            *io = i;
            return true;
        }
    }
    char options[192] = "";
    for (uint32_t i = 0; i < count; ++i) {
        const size_t used = strlen(options);
        snprintf(options + used, sizeof options - used, "%s\"%s\"", i ? ", " : "", names[i]);
    }
    return wpe__json_fail(r, v, WPE_RESULT_ERROR_PARSE, "\"%s\" must be one of %s", key, options);
}

bool wpe__json_read_string(wpe__JsonReader* r, const WPE_JsonValue* object, const char* key, char* out,
                           size_t capacity) {
    if (r->result != WPE_RESULT_OK) return false;
    const WPE_JsonValue* v = wpe_json_get(object, key);
    if (!v) return true;
    const char* text = wpe_json_string(v);
    if (!text) return wpe__json_fail(r, v, WPE_RESULT_ERROR_PARSE, "\"%s\" must be a string", key);
    if (strlen(text) >= capacity)
        return wpe__json_fail(r, v, WPE_RESULT_ERROR_PARSE, "\"%s\" is longer than %zu bytes", key,
                              capacity - 1);
    memcpy(out, text, strlen(text) + 1);
    return true;
}
