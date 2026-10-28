#define WPE_LOG_MODULE "json"
#include "core/json.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/alloc.h"
#include "core/arena.h"
#include "core/log.h"

struct WPE_JsonValue {
    WPE_JsonType type;
    uint32_t line;
    uint32_t count; /* Array elements or object members. */
    bool boolean;
    double number;
    const char* string;
    const WPE_JsonValue* items; /* Array elements or object member values (count). */
    const char* const* keys;    /* Object member names (count). */
};

struct WPE_JsonDocument {
    WPE_Arena arena;
    WPE_JsonValue root;
};

/* ---- UTF-8 ----------------------------------------------------------------------------------------- */

/* Length of the valid UTF-8 sequence starting at text[0] (at most `available` bytes), or 0 if invalid
 * (overlong forms, surrogates and code points above U+10FFFF are invalid). */
static size_t wpe__utf8_sequence(const unsigned char* text, size_t available) {
    const unsigned char c = text[0];
    if (c < 0x80) return 1;
    size_t length = 0;
    uint32_t code = 0, minimum = 0;
    if ((c & 0xE0) == 0xC0) {
        length = 2;
        code = c & 0x1Fu;
        minimum = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
        length = 3;
        code = c & 0x0Fu;
        minimum = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
        length = 4;
        code = c & 0x07u;
        minimum = 0x10000;
    } else {
        return 0;
    }
    if (available < length) return 0;
    for (size_t i = 1; i < length; ++i) {
        if ((text[i] & 0xC0) != 0x80) return 0;
        code = (code << 6) | (text[i] & 0x3Fu);
    }
    if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) return 0;
    return length;
}

/* ---- Reading -------------------------------------------------------------------------------------- */

typedef struct wpe__JsonParser {
    const char* text;
    size_t size;
    size_t at;
    uint32_t line;
    size_t line_start;
    WPE_Arena* arena;
    WPE_JsonError* error;
    bool failed;

    /* Elements and keys of the containers being parsed; copied into the arena when one closes. */
    WPE_JsonValue* values;
    uint32_t value_count, value_capacity;
    const char** keys;
    uint32_t key_count, key_capacity;
    char* scratch; /* Decoded string under construction. */
    size_t scratch_capacity;
} wpe__JsonParser;

WPE_PRINTF_LIKE(2, 3)
static void wpe__json_fail(wpe__JsonParser* p, const char* format, ...) {
    if (p->failed) return;
    p->failed = true;
    p->error->line = p->line;
    p->error->column = (uint32_t)(p->at - p->line_start) + 1;
    va_list args;
    va_start(args, format);
    vsnprintf(p->error->message, sizeof p->error->message, format, args);
    va_end(args);
}

static void wpe__json_out_of_memory(wpe__JsonParser* p) {
    if (p->failed) return;
    p->failed = true;
    *p->error = (WPE_JsonError){0};
    snprintf(p->error->message, sizeof p->error->message, "out of memory");
}

static void wpe__skip_whitespace(wpe__JsonParser* p) {
    while (p->at < p->size) {
        const char c = p->text[p->at];
        if (c == '\n') {
            ++p->line;
            p->line_start = p->at + 1;
        } else if (c != ' ' && c != '\t' && c != '\r') {
            return;
        }
        ++p->at;
    }
}

static bool wpe__match_literal(wpe__JsonParser* p, const char* literal) {
    const size_t length = strlen(literal);
    if (p->size - p->at < length || memcmp(p->text + p->at, literal, length) != 0) return false;
    p->at += length;
    return true;
}

static bool wpe__scratch_reserve(wpe__JsonParser* p, size_t needed) {
    if (needed <= p->scratch_capacity) return true;
    size_t capacity = p->scratch_capacity ? p->scratch_capacity : 256;
    while (capacity < needed)
        capacity *= 2;
    char* scratch = wpe_realloc(p->scratch, capacity);
    if (!scratch) {
        wpe__json_out_of_memory(p);
        return false;
    }
    p->scratch = scratch;
    p->scratch_capacity = capacity;
    return true;
}

static uint32_t wpe__hex4(const char* text) {
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        const char c = text[i];
        uint32_t digit = 0;
        if (c >= '0' && c <= '9') {
            digit = (uint32_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = (uint32_t)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = (uint32_t)(c - 'A' + 10);
        } else {
            return UINT32_MAX;
        }
        value = (value << 4) | digit;
    }
    return value;
}

static size_t wpe__utf8_encode(uint32_t code, char* out) {
    if (code < 0x80) {
        out[0] = (char)code;
        return 1;
    }
    if (code < 0x800) {
        out[0] = (char)(0xC0 | (code >> 6));
        out[1] = (char)(0x80 | (code & 0x3F));
        return 2;
    }
    if (code < 0x10000) {
        out[0] = (char)(0xE0 | (code >> 12));
        out[1] = (char)(0x80 | ((code >> 6) & 0x3F));
        out[2] = (char)(0x80 | (code & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (code >> 18));
    out[1] = (char)(0x80 | ((code >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((code >> 6) & 0x3F));
    out[3] = (char)(0x80 | (code & 0x3F));
    return 4;
}

/* Parses a string starting at the opening quote; returns an arena copy or NULL on failure. */
static const char* wpe__parse_string(wpe__JsonParser* p) {
    ++p->at; /* Opening quote. */
    size_t length = 0;
    for (;;) {
        if (p->at >= p->size) {
            wpe__json_fail(p, "unterminated string");
            return NULL;
        }
        if (!wpe__scratch_reserve(p, length + 5)) return NULL;
        const unsigned char c = (unsigned char)p->text[p->at];
        if (c == '"') {
            ++p->at;
            break;
        }
        if (c < 0x20) {
            wpe__json_fail(p, "control character in string (escape it)");
            return NULL;
        }
        if (c != '\\') {
            const size_t sequence =
                wpe__utf8_sequence((const unsigned char*)p->text + p->at, p->size - p->at);
            if (sequence == 0) {
                wpe__json_fail(p, "invalid UTF-8 in string");
                return NULL;
            }
            memcpy(p->scratch + length, p->text + p->at, sequence);
            length += sequence;
            p->at += sequence;
            continue;
        }
        if (p->size - p->at < 2) {
            wpe__json_fail(p, "unterminated escape sequence");
            return NULL;
        }
        const char escape = p->text[p->at + 1];
        static const char simple_from[] = "\"\\/bfnrt";
        static const char simple_to[] = "\"\\/\b\f\n\r\t";
        const char* simple = strchr(simple_from, escape);
        if (simple && escape != '\0') {
            p->scratch[length++] = simple_to[simple - simple_from];
            p->at += 2;
            continue;
        }
        if (escape != 'u') {
            wpe__json_fail(p, "invalid escape sequence '\\%c'", escape);
            return NULL;
        }
        uint32_t code = p->size - p->at >= 6 ? wpe__hex4(p->text + p->at + 2) : UINT32_MAX;
        if (code == UINT32_MAX) {
            wpe__json_fail(p, "invalid \\u escape");
            return NULL;
        }
        p->at += 6;
        if (code >= 0xD800 && code <= 0xDBFF) {
            const uint32_t low = p->size - p->at >= 6 && p->text[p->at] == '\\' && p->text[p->at + 1] == 'u'
                                     ? wpe__hex4(p->text + p->at + 2)
                                     : UINT32_MAX;
            if (low < 0xDC00 || low > 0xDFFF) {
                wpe__json_fail(p, "unpaired UTF-16 surrogate in \\u escape");
                return NULL;
            }
            p->at += 6;
            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
        } else if (code >= 0xDC00 && code <= 0xDFFF) {
            wpe__json_fail(p, "unpaired UTF-16 surrogate in \\u escape");
            return NULL;
        } else if (code == 0) {
            wpe__json_fail(p, "\\u0000 is not allowed in strings");
            return NULL;
        }
        length += wpe__utf8_encode(code, p->scratch + length);
    }
    char* copy = wpe_arena_alloc(p->arena, length + 1, 1);
    if (!copy) {
        wpe__json_out_of_memory(p);
        return NULL;
    }
    memcpy(copy, p->scratch, length);
    copy[length] = '\0';
    return copy;
}

static bool wpe__is_digit(char c) {
    return c >= '0' && c <= '9';
}

static void wpe__parse_number(wpe__JsonParser* p, WPE_JsonValue* out) {
    const size_t start = p->at;
    size_t at = p->at;
    if (at < p->size && p->text[at] == '-') ++at;
    if (at < p->size && p->text[at] == '0') {
        ++at;
    } else if (at < p->size && wpe__is_digit(p->text[at])) {
        while (at < p->size && wpe__is_digit(p->text[at]))
            ++at;
    } else {
        wpe__json_fail(p, "invalid number");
        return;
    }
    if (at < p->size && p->text[at] == '.') {
        ++at;
        if (at >= p->size || !wpe__is_digit(p->text[at])) {
            wpe__json_fail(p, "invalid number: digits must follow '.'");
            return;
        }
        while (at < p->size && wpe__is_digit(p->text[at]))
            ++at;
    }
    if (at < p->size && (p->text[at] == 'e' || p->text[at] == 'E')) {
        ++at;
        if (at < p->size && (p->text[at] == '+' || p->text[at] == '-')) ++at;
        if (at >= p->size || !wpe__is_digit(p->text[at])) {
            wpe__json_fail(p, "invalid number: digits must follow the exponent");
            return;
        }
        while (at < p->size && wpe__is_digit(p->text[at]))
            ++at;
    }
    char buffer[64];
    if (at - start >= sizeof buffer) {
        wpe__json_fail(p, "number has too many digits");
        return;
    }
    memcpy(buffer, p->text + start, at - start);
    buffer[at - start] = '\0';
    const double value = strtod(buffer, NULL);
    if (!isfinite(value)) {
        wpe__json_fail(p, "number out of range");
        return;
    }
    out->type = WPE_JSON_NUMBER;
    out->number = value;
    p->at = at;
}

static bool wpe__push_value(wpe__JsonParser* p, const WPE_JsonValue* value) {
    if (p->value_count == p->value_capacity) {
        const uint32_t capacity = p->value_capacity ? p->value_capacity * 2 : 64;
        WPE_JsonValue* values = wpe_realloc(p->values, (size_t)capacity * sizeof *values);
        if (!values) {
            wpe__json_out_of_memory(p);
            return false;
        }
        p->values = values;
        p->value_capacity = capacity;
    }
    p->values[p->value_count++] = *value;
    return true;
}

static bool wpe__push_key(wpe__JsonParser* p, const char* key) {
    if (p->key_count == p->key_capacity) {
        const uint32_t capacity = p->key_capacity ? p->key_capacity * 2 : 64;
        const char** keys = wpe_realloc(p->keys, (size_t)capacity * sizeof *keys);
        if (!keys) {
            wpe__json_out_of_memory(p);
            return false;
        }
        p->keys = keys;
        p->key_capacity = capacity;
    }
    p->keys[p->key_count++] = key;
    return true;
}

/* Moves the top `count` pending values (and keys for objects) into the arena as `out`'s children. */
static void wpe__close_container(wpe__JsonParser* p, WPE_JsonValue* out, uint32_t count, bool object) {
    out->count = count;
    if (count == 0) return;
    WPE_JsonValue* items = wpe_arena_alloc(p->arena, (size_t)count * sizeof *items, alignof(WPE_JsonValue));
    const char** keys =
        object ? wpe_arena_alloc(p->arena, (size_t)count * sizeof *keys, alignof(const char*)) : NULL;
    if (!items || (object && !keys)) {
        wpe__json_out_of_memory(p);
        return;
    }
    memcpy(items, p->values + p->value_count - count, (size_t)count * sizeof *items);
    p->value_count -= count;
    if (object) {
        memcpy(keys, p->keys + p->key_count - count, (size_t)count * sizeof *keys);
        p->key_count -= count;
        out->keys = keys;
    }
    out->items = items;
}

static void wpe__parse_value(wpe__JsonParser* p, WPE_JsonValue* out, uint32_t depth);

static void wpe__parse_array(wpe__JsonParser* p, WPE_JsonValue* out, uint32_t depth) {
    ++p->at; /* '[' */
    out->type = WPE_JSON_ARRAY;
    uint32_t count = 0;
    wpe__skip_whitespace(p);
    if (p->at < p->size && p->text[p->at] == ']') {
        ++p->at;
        return;
    }
    for (;;) {
        WPE_JsonValue element = {0};
        wpe__parse_value(p, &element, depth + 1);
        if (p->failed || !wpe__push_value(p, &element)) return;
        ++count;
        wpe__skip_whitespace(p);
        if (p->at < p->size && p->text[p->at] == ',') {
            ++p->at;
            wpe__skip_whitespace(p);
            if (p->at < p->size && p->text[p->at] == ']') {
                wpe__json_fail(p, "trailing comma in array");
                return;
            }
            continue;
        }
        if (p->at < p->size && p->text[p->at] == ']') {
            ++p->at;
            wpe__close_container(p, out, count, false);
            return;
        }
        wpe__json_fail(p, "expected ',' or ']' in array");
        return;
    }
}

static void wpe__parse_object(wpe__JsonParser* p, WPE_JsonValue* out, uint32_t depth) {
    ++p->at; /* '{' */
    out->type = WPE_JSON_OBJECT;
    uint32_t count = 0;
    wpe__skip_whitespace(p);
    if (p->at < p->size && p->text[p->at] == '}') {
        ++p->at;
        return;
    }
    for (;;) {
        wpe__skip_whitespace(p);
        if (p->at >= p->size || p->text[p->at] != '"') {
            wpe__json_fail(p, count > 0 && p->at < p->size && p->text[p->at] == '}'
                                  ? "trailing comma in object"
                                  : "expected a member name string");
            return;
        }
        const char* key = wpe__parse_string(p);
        if (!key) return;
        for (uint32_t i = 0; i < count; ++i) {
            if (strcmp(p->keys[p->key_count - count + i], key) == 0) {
                wpe__json_fail(p, "duplicate member name \"%.64s\"", key);
                return;
            }
        }
        wpe__skip_whitespace(p);
        if (p->at >= p->size || p->text[p->at] != ':') {
            wpe__json_fail(p, "expected ':' after member name");
            return;
        }
        ++p->at;
        WPE_JsonValue member = {0};
        wpe__parse_value(p, &member, depth + 1);
        if (p->failed || !wpe__push_key(p, key) || !wpe__push_value(p, &member)) return;
        ++count;
        wpe__skip_whitespace(p);
        if (p->at < p->size && p->text[p->at] == ',') {
            ++p->at;
            continue;
        }
        if (p->at < p->size && p->text[p->at] == '}') {
            ++p->at;
            wpe__close_container(p, out, count, true);
            return;
        }
        wpe__json_fail(p, "expected ',' or '}' in object");
        return;
    }
}

static void wpe__parse_value(wpe__JsonParser* p, WPE_JsonValue* out, uint32_t depth) {
    wpe__skip_whitespace(p);
    out->line = p->line;
    if (p->at >= p->size) {
        wpe__json_fail(p, "unexpected end of input");
        return;
    }
    const char c = p->text[p->at];
    if ((c == '[' || c == '{') && depth >= WPE_JSON_MAX_DEPTH) {
        wpe__json_fail(p, "nesting deeper than %u levels", WPE_JSON_MAX_DEPTH);
        return;
    }
    switch (c) {
    case '{':
        wpe__parse_object(p, out, depth);
        return;
    case '[':
        wpe__parse_array(p, out, depth);
        return;
    case '"':
        out->type = WPE_JSON_STRING;
        out->string = wpe__parse_string(p);
        return;
    default:
        break;
    }
    if (wpe__match_literal(p, "true")) {
        out->type = WPE_JSON_BOOL;
        out->boolean = true;
    } else if (wpe__match_literal(p, "false")) {
        out->type = WPE_JSON_BOOL;
    } else if (wpe__match_literal(p, "null")) {
        out->type = WPE_JSON_NULL;
    } else if (c == '-' || wpe__is_digit(c)) {
        wpe__parse_number(p, out);
    } else {
        wpe__json_fail(p, "unexpected character '%c'", c >= 0x20 && c < 0x7F ? c : '?');
    }
}

WPE_Result wpe_json_parse(const char* text, size_t size, WPE_JsonDocument** out_doc,
                          WPE_JsonError* out_error) {
    WPE_JsonError ignored;
    WPE_JsonError* error = out_error ? out_error : &ignored;
    *error = (WPE_JsonError){0};
    if (!text || !out_doc) return WPE_RESULT_ERROR_NULL_ARG;
    *out_doc = NULL;
    WPE_JsonDocument* doc = wpe_alloc_zeroed(1, sizeof *doc);
    if (!doc) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    wpe_arena_init(&doc->arena, 0);

    wpe__JsonParser p = {.text = text, .size = size, .line = 1, .arena = &doc->arena, .error = error};
    if (size >= 3 && memcmp(text, "\xEF\xBB\xBF", 3) == 0) p.at = p.line_start = 3;
    wpe__parse_value(&p, &doc->root, 0);
    if (!p.failed) {
        wpe__skip_whitespace(&p);
        if (p.at != p.size) wpe__json_fail(&p, "unexpected data after the top-level value");
    }
    const bool out_of_memory = p.failed && error->line == 0;
    wpe_free(p.values);
    wpe_free(p.keys);
    wpe_free(p.scratch);
    if (p.failed) {
        wpe_json_free(doc);
        return out_of_memory ? WPE_RESULT_ERROR_OUT_OF_MEMORY : WPE_RESULT_ERROR_PARSE;
    }
    *out_doc = doc;
    return WPE_RESULT_OK;
}

void wpe_json_free(WPE_JsonDocument* doc) {
    if (!doc) return;
    wpe_arena_release(&doc->arena);
    wpe_free(doc);
}

const WPE_JsonValue* wpe_json_root(const WPE_JsonDocument* doc) {
    return doc ? &doc->root : NULL;
}

WPE_JsonType wpe_json_type(const WPE_JsonValue* value) {
    return value ? value->type : WPE_JSON_NULL;
}

uint32_t wpe_json_line(const WPE_JsonValue* value) {
    return value ? value->line : 0;
}

bool wpe_json_bool(const WPE_JsonValue* value, bool fallback) {
    return value && value->type == WPE_JSON_BOOL ? value->boolean : fallback;
}

double wpe_json_number(const WPE_JsonValue* value, double fallback) {
    return value && value->type == WPE_JSON_NUMBER ? value->number : fallback;
}

const char* wpe_json_string(const WPE_JsonValue* value) {
    return value && value->type == WPE_JSON_STRING ? value->string : NULL;
}

uint32_t wpe_json_count(const WPE_JsonValue* value) {
    return value && (value->type == WPE_JSON_ARRAY || value->type == WPE_JSON_OBJECT) ? value->count : 0;
}

const WPE_JsonValue* wpe_json_at(const WPE_JsonValue* value, uint32_t index) {
    return value && value->type == WPE_JSON_ARRAY && index < value->count ? &value->items[index] : NULL;
}

const WPE_JsonValue* wpe_json_get(const WPE_JsonValue* object, const char* key) {
    if (!object || object->type != WPE_JSON_OBJECT || !key) return NULL;
    for (uint32_t i = 0; i < object->count; ++i) {
        if (strcmp(object->keys[i], key) == 0) return &object->items[i];
    }
    return NULL;
}

const WPE_JsonValue* wpe_json_member(const WPE_JsonValue* object, uint32_t index, const char** out_key) {
    if (!object || object->type != WPE_JSON_OBJECT || index >= object->count) return NULL;
    if (out_key) *out_key = object->keys[index];
    return &object->items[index];
}

bool wpe_json_equal(const WPE_JsonValue* a, const WPE_JsonValue* b) {
    if (!a || !b) return a == b;
    if (a->type != b->type || a->count != b->count) return false;
    switch (a->type) {
    case WPE_JSON_NULL:
        return true;
    case WPE_JSON_BOOL:
        return a->boolean == b->boolean;
    case WPE_JSON_NUMBER:
        return a->number == b->number;
    case WPE_JSON_STRING:
        return strcmp(a->string, b->string) == 0;
    case WPE_JSON_ARRAY:
        for (uint32_t i = 0; i < a->count; ++i) {
            if (!wpe_json_equal(&a->items[i], &b->items[i])) return false;
        }
        return true;
    case WPE_JSON_OBJECT:
        for (uint32_t i = 0; i < a->count; ++i) {
            if (!wpe_json_equal(&a->items[i], wpe_json_get(b, a->keys[i]))) return false;
        }
        return true;
    }
    return false;
}

/* ---- Writing -------------------------------------------------------------------------------------- */

typedef struct wpe__JsonFrame {
    bool object;
    bool inline_items;
    bool key_pending; /* Objects: a key was written and awaits its value. */
    uint32_t count;
} wpe__JsonFrame;

struct WPE_JsonWriter {
    char* text;
    size_t length;
    size_t capacity;
    wpe__JsonFrame frames[WPE_JSON_MAX_DEPTH];
    uint32_t depth;
    bool root_written;
    bool compact;  /* One line: no newlines or indentation. */
    bool finished; /* Terminating newline appended; further values are misuse. */
    bool failed;
    bool out_of_memory;
    const char* misuse; /* First misuse, reported by finish. */
};

WPE_Result wpe_json_writer_create(WPE_JsonWriter** out_writer) {
    if (!out_writer) return WPE_RESULT_ERROR_NULL_ARG;
    *out_writer = wpe_alloc_zeroed(1, sizeof **out_writer);
    return *out_writer ? WPE_RESULT_OK : WPE_RESULT_ERROR_OUT_OF_MEMORY;
}

void wpe_json_writer_set_compact(WPE_JsonWriter* writer, bool compact) {
    if (writer) writer->compact = compact;
}

void wpe_json_writer_destroy(WPE_JsonWriter* writer) {
    if (!writer) return;
    wpe_free(writer->text);
    wpe_free(writer);
}

static void wpe__writer_misuse(WPE_JsonWriter* w, const char* reason) {
    if (w->failed) return;
    w->failed = true;
    w->misuse = reason;
}

static void wpe__append(WPE_JsonWriter* w, const char* data, size_t size) {
    if (w->failed) return;
    if (w->length + size + 1 > w->capacity) {
        size_t capacity = w->capacity ? w->capacity : 1024;
        while (capacity < w->length + size + 1)
            capacity *= 2;
        char* text = wpe_realloc(w->text, capacity);
        if (!text) {
            w->failed = true;
            w->out_of_memory = true;
            return;
        }
        w->text = text;
        w->capacity = capacity;
    }
    memcpy(w->text + w->length, data, size);
    w->length += size;
    w->text[w->length] = '\0';
}

static void wpe__append_str(WPE_JsonWriter* w, const char* text) {
    wpe__append(w, text, strlen(text));
}

static void wpe__newline(WPE_JsonWriter* w, uint32_t depth) {
    if (w->compact) {
        wpe__append(w, " ", 1);
        return;
    }
    wpe__append(w, "\n", 1);
    for (uint32_t i = 0; i < depth; ++i)
        wpe__append(w, "  ", 2);
}

static bool wpe__inline(const WPE_JsonWriter* w) {
    return w->depth > 0 && w->frames[w->depth - 1].inline_items;
}

/* Separators and indentation before a value; false (and failed) on misuse. */
static bool wpe__begin_value(WPE_JsonWriter* w) {
    if (w->failed) return false;
    if (w->depth == 0) {
        if (w->root_written) {
            wpe__writer_misuse(w, "more than one top-level value");
            return false;
        }
        w->root_written = true;
        return true;
    }
    wpe__JsonFrame* frame = &w->frames[w->depth - 1];
    if (frame->object) {
        if (!frame->key_pending) {
            wpe__writer_misuse(w, "object member value without a key");
            return false;
        }
        frame->key_pending = false;
        return true;
    }
    if (frame->count > 0) wpe__append(w, frame->inline_items ? ", " : ",", frame->inline_items ? 2 : 1);
    if (!frame->inline_items) wpe__newline(w, w->depth);
    ++frame->count;
    return true;
}

static void wpe__write_escaped(WPE_JsonWriter* w, const char* text) {
    wpe__append(w, "\"", 1);
    const unsigned char* at = (const unsigned char*)text;
    while (*at) {
        const unsigned char c = *at;
        if (c == '"' || c == '\\') {
            const char escaped[2] = {'\\', (char)c};
            wpe__append(w, escaped, 2);
            ++at;
        } else if (c < 0x20) {
            char escaped[8];
            switch (c) {
            case '\n':
                snprintf(escaped, sizeof escaped, "\\n");
                break;
            case '\t':
                snprintf(escaped, sizeof escaped, "\\t");
                break;
            case '\r':
                snprintf(escaped, sizeof escaped, "\\r");
                break;
            default:
                snprintf(escaped, sizeof escaped, "\\u%04x", c);
                break;
            }
            wpe__append_str(w, escaped);
            ++at;
        } else {
            const size_t sequence = wpe__utf8_sequence(at, strlen((const char*)at));
            if (sequence == 0) {
                wpe__writer_misuse(w, "string is not valid UTF-8");
                return;
            }
            wpe__append(w, (const char*)at, sequence);
            at += sequence;
        }
    }
    wpe__append(w, "\"", 1);
}

static void wpe__push_frame(WPE_JsonWriter* w, bool object, bool inline_items) {
    if (!wpe__begin_value(w)) return;
    if (w->depth == WPE_JSON_MAX_DEPTH) {
        wpe__writer_misuse(w, "nesting too deep");
        return;
    }
    /* Containers inside an inline array print inline too. */
    const bool parent_inline = wpe__inline(w);
    w->frames[w->depth] = (wpe__JsonFrame){.object = object, .inline_items = inline_items || parent_inline};
    ++w->depth;
    wpe__append(w, object ? "{" : "[", 1);
}

static void wpe__pop_frame(WPE_JsonWriter* w, bool object) {
    if (w->failed) return;
    if (w->depth == 0 || w->frames[w->depth - 1].object != object || w->frames[w->depth - 1].key_pending) {
        wpe__writer_misuse(w, object ? "unbalanced object end" : "unbalanced array end");
        return;
    }
    const wpe__JsonFrame frame = w->frames[--w->depth];
    if (frame.count > 0 && !frame.inline_items) wpe__newline(w, w->depth);
    wpe__append(w, object ? "}" : "]", 1);
}

void wpe_json_write_object_begin(WPE_JsonWriter* writer) {
    if (writer) wpe__push_frame(writer, true, false);
}

void wpe_json_write_object_end(WPE_JsonWriter* writer) {
    if (writer) wpe__pop_frame(writer, true);
}

void wpe_json_write_array_begin(WPE_JsonWriter* writer, bool inline_items) {
    if (writer) wpe__push_frame(writer, false, inline_items);
}

void wpe_json_write_array_end(WPE_JsonWriter* writer) {
    if (writer) wpe__pop_frame(writer, false);
}

void wpe_json_write_key(WPE_JsonWriter* writer, const char* key) {
    if (!writer || writer->failed) return;
    if (!key || writer->depth == 0 || !writer->frames[writer->depth - 1].object ||
        writer->frames[writer->depth - 1].key_pending) {
        wpe__writer_misuse(writer, "key outside an object or without a value");
        return;
    }
    wpe__JsonFrame* frame = &writer->frames[writer->depth - 1];
    if (frame->count > 0) wpe__append(writer, frame->inline_items ? ", " : ",", frame->inline_items ? 2 : 1);
    if (!frame->inline_items) wpe__newline(writer, writer->depth);
    wpe__write_escaped(writer, key);
    wpe__append(writer, ": ", 2);
    frame->key_pending = true;
    ++frame->count;
}

void wpe_json_write_string(WPE_JsonWriter* writer, const char* value) {
    if (!writer) return;
    if (!value) {
        wpe__writer_misuse(writer, "NULL string");
        return;
    }
    if (wpe__begin_value(writer)) wpe__write_escaped(writer, value);
}

static void wpe__write_formatted_number(WPE_JsonWriter* w, double value, int precision) {
    if (!isfinite(value)) {
        wpe__writer_misuse(w, "non-finite number");
        return;
    }
    if (!wpe__begin_value(w)) return;
    char buffer[40];
    snprintf(buffer, sizeof buffer, "%.*g", precision, value);
    wpe__append_str(w, buffer);
}

/* The fewest significant digits that read back as exactly `value`. */
static int wpe__shortest_precision(double value) {
    for (int precision = 1; precision < 17; ++precision) {
        char buffer[40];
        snprintf(buffer, sizeof buffer, "%.*g", precision, value);
        if (strtod(buffer, NULL) == value) return precision;
    }
    return 17;
}

void wpe_json_write_value(WPE_JsonWriter* writer, const WPE_JsonValue* value) {
    if (!writer) return;
    if (!value) {
        wpe__writer_misuse(writer, "NULL value");
        return;
    }
    switch (value->type) {
    case WPE_JSON_NULL:
        wpe_json_write_null(writer);
        break;
    case WPE_JSON_BOOL:
        wpe_json_write_bool(writer, value->boolean);
        break;
    case WPE_JSON_NUMBER:
        /* Whole numbers as integers: "%g" with few digits would write 10 as "1e+01". */
        if (value->number == floor(value->number) && fabs(value->number) < 1e15) {
            wpe_json_write_integer(writer, (int64_t)value->number);
        } else if (isfinite(value->number)) {
            wpe__write_formatted_number(writer, value->number, wpe__shortest_precision(value->number));
        } else {
            wpe__writer_misuse(writer, "non-finite number");
        }
        break;
    case WPE_JSON_STRING:
        wpe_json_write_string(writer, value->string);
        break;
    case WPE_JSON_ARRAY: {
        bool scalars = true;
        for (uint32_t i = 0; i < value->count && scalars; ++i)
            scalars = value->items[i].type != WPE_JSON_ARRAY && value->items[i].type != WPE_JSON_OBJECT;
        wpe_json_write_array_begin(writer, scalars);
        for (uint32_t i = 0; i < value->count; ++i)
            wpe_json_write_value(writer, &value->items[i]);
        wpe_json_write_array_end(writer);
        break;
    }
    case WPE_JSON_OBJECT:
        wpe_json_write_object_begin(writer);
        for (uint32_t i = 0; i < value->count; ++i) {
            wpe_json_write_key(writer, value->keys[i]);
            wpe_json_write_value(writer, &value->items[i]);
        }
        wpe_json_write_object_end(writer);
        break;
    }
}

/* The fewest significant digits that read back as the same float as `value` (0.06f is "0.06", not
 * "0.0599999987"); 0 for whole numbers that print exactly with "%.0f" ("2000", not "2e+03"). */
static int wpe__shortest_float_precision(double value) {
    const float f = (float)value;
    if (f == floorf(f) && fabsf(f) < 1e15f) return 0;
    for (int precision = 1; precision < 9; ++precision) {
        char buffer[40];
        snprintf(buffer, sizeof buffer, "%.*g", precision, (double)f);
        if (strtof(buffer, NULL) == f) return precision;
    }
    return 9;
}

void wpe_json_write_number(WPE_JsonWriter* writer, double value) {
    if (!writer) return;
    if (!isfinite(value)) {
        wpe__writer_misuse(writer, "non-finite number");
        return;
    }
    const int precision = wpe__shortest_float_precision(value);
    if (precision > 0) {
        wpe__write_formatted_number(writer, (double)(float)value, precision);
        return;
    }
    if (!wpe__begin_value(writer)) return;
    char buffer[40];
    snprintf(buffer, sizeof buffer, "%.0f", (double)(float)value);
    wpe__append_str(writer, buffer);
}

void wpe_json_write_double(WPE_JsonWriter* writer, double value) {
    if (writer) wpe__write_formatted_number(writer, value, 17);
}

void wpe_json_write_rounded(WPE_JsonWriter* writer, double value, int significant_digits) {
    if (!writer) return;
    if (significant_digits < 1 || significant_digits > 17) {
        wpe__writer_misuse(writer, "significant digits out of range");
        return;
    }
    wpe__write_formatted_number(writer, value, significant_digits);
}

void wpe_json_write_integer(WPE_JsonWriter* writer, int64_t value) {
    if (!writer || !wpe__begin_value(writer)) return;
    char buffer[24];
    snprintf(buffer, sizeof buffer, "%lld", (long long)value);
    wpe__append_str(writer, buffer);
}

void wpe_json_write_bool(WPE_JsonWriter* writer, bool value) {
    if (writer && wpe__begin_value(writer)) wpe__append_str(writer, value ? "true" : "false");
}

void wpe_json_write_null(WPE_JsonWriter* writer) {
    if (writer && wpe__begin_value(writer)) wpe__append_str(writer, "null");
}

WPE_Result wpe_json_writer_finish(WPE_JsonWriter* writer, const char** out_text, size_t* out_size) {
    if (!writer || !out_text) return WPE_RESULT_ERROR_NULL_ARG;
    *out_text = NULL;
    if (!writer->failed && !writer->finished) {
        if (writer->depth != 0 || !writer->root_written) wpe__writer_misuse(writer, "incomplete document");
        wpe__append(writer, "\n", 1);
        writer->finished = !writer->failed;
    }
    if (writer->out_of_memory) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    if (writer->failed) {
        wpe_log_error("invalid JSON writer use: %s", writer->misuse);
        return WPE_RESULT_ERROR_INVALID_STATE;
    }
    *out_text = writer->text;
    if (out_size) *out_size = writer->length;
    return WPE_RESULT_OK;
}
