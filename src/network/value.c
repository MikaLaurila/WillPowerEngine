#include "network/value.h"

#include <math.h>

typedef enum wpe__Tag {
    WPE__TAG_NIL = 0,
    WPE__TAG_FALSE = 1,
    WPE__TAG_TRUE = 2,
    WPE__TAG_INT = 3,
    WPE__TAG_DOUBLE = 4,
    WPE__TAG_STRING = 5,
    WPE__TAG_VECTOR = 6,
    WPE__TAG_TABLE = 7,
} wpe__Tag;

/* The fewest bits any value takes (a tag): bounds how many entries the data left can hold. */
#define WPE__MIN_VALUE_BITS 3u

void wpe_netvalue_write_nil(WPE_BitWriter* writer) {
    wpe_bits_write(writer, WPE__TAG_NIL, 3);
}

void wpe_netvalue_write_bool(WPE_BitWriter* writer, bool value) {
    wpe_bits_write(writer, value ? WPE__TAG_TRUE : WPE__TAG_FALSE, 3);
}

void wpe_netvalue_write_number(WPE_BitWriter* writer, double value) {
    const bool integral = value >= -2147483648.0 && value <= 2147483647.0 && value == floor(value) &&
                          !(value == 0.0 && signbit(value));
    if (integral) {
        wpe_bits_write(writer, WPE__TAG_INT, 3);
        wpe_bits_write_signed(writer, (int32_t)value);
    } else {
        wpe_bits_write(writer, WPE__TAG_DOUBLE, 3);
        wpe_bits_write_double(writer, value);
    }
}

void wpe_netvalue_write_string(WPE_BitWriter* writer, const char* data, size_t length) {
    if (length > UINT32_MAX) {
        writer->error = true;
        return;
    }
    wpe_bits_write(writer, WPE__TAG_STRING, 3);
    wpe_bits_write_varint(writer, (uint32_t)length);
    wpe_bits_write_bytes(writer, data, length);
}

void wpe_netvalue_write_vector(WPE_BitWriter* writer, WPE_Vec3 value) {
    wpe_bits_write(writer, WPE__TAG_VECTOR, 3);
    for (int i = 0; i < 3; ++i)
        wpe_bits_write_float(writer, value.v[i]);
}

void wpe_netvalue_write_table(WPE_BitWriter* writer, uint32_t entries) {
    wpe_bits_write(writer, WPE__TAG_TABLE, 3);
    wpe_bits_write_varint(writer, entries);
}

WPE_NetValueReader wpe_netvalue_reader(WPE_BitReader* bits) {
    return (WPE_NetValueReader){.bits = bits};
}

static bool wpe__fail(WPE_NetValueReader* reader) {
    reader->bits->error = true;
    return false;
}

bool wpe_netvalue_read(WPE_NetValueReader* reader, WPE_NetValue* out_value) {
    WPE_BitReader* bits = reader->bits;
    *out_value = (WPE_NetValue){0};
    if (bits->error) return false;
    if (reader->values >= WPE_NETVALUE_MAX_VALUES) return wpe__fail(reader);
    /* Inside a table, values alternate key, value: a key comes when an even number is left. */
    const bool is_key = reader->depth > 0 && reader->pending[reader->depth] % 2 == 0;
    WPE_NetValue v = {0};
    const wpe__Tag tag = (wpe__Tag)wpe_bits_read(bits, 3);
    switch (tag) {
    case WPE__TAG_NIL:
        v.type = WPE_NETVALUE_NIL;
        break;
    case WPE__TAG_FALSE:
    case WPE__TAG_TRUE:
        v.type = WPE_NETVALUE_BOOL;
        v.boolean = tag == WPE__TAG_TRUE;
        break;
    case WPE__TAG_INT:
        v.type = WPE_NETVALUE_NUMBER;
        v.number = wpe_bits_read_signed(bits);
        break;
    case WPE__TAG_DOUBLE:
        v.type = WPE_NETVALUE_NUMBER;
        v.number = wpe_bits_read_double(bits);
        break;
    case WPE__TAG_STRING: {
        v.type = WPE_NETVALUE_STRING;
        v.length = wpe_bits_read_varint(bits);
        v.string = (const char*)wpe_bits_read_bytes(bits, v.length);
        break;
    }
    case WPE__TAG_VECTOR:
        v.type = WPE_NETVALUE_VECTOR;
        for (int i = 0; i < 3; ++i)
            v.vector.v[i] = wpe_bits_read_float(bits);
        break;
    case WPE__TAG_TABLE:
        v.type = WPE_NETVALUE_TABLE;
        v.entries = wpe_bits_read_varint(bits);
        if (!bits->error && (uint64_t)v.entries * 2u * WPE__MIN_VALUE_BITS > wpe_bits_reader_remaining(bits))
            return wpe__fail(reader); /* More entries than the data could hold. */
        if (v.entries > 0 && reader->depth + 1 > WPE_NETVALUE_MAX_DEPTH) return wpe__fail(reader);
        break;
    }
    if (bits->error) return false;
    if (is_key) {
        const bool valid_key = v.type == WPE_NETVALUE_STRING || v.type == WPE_NETVALUE_BOOL ||
                               (v.type == WPE_NETVALUE_NUMBER && !isnan(v.number));
        if (!valid_key) return wpe__fail(reader);
    }
    ++reader->values;
    if (reader->depth > 0) --reader->pending[reader->depth];
    if (v.type == WPE_NETVALUE_TABLE && v.entries > 0) {
        ++reader->depth;
        reader->pending[reader->depth] = v.entries * 2u; /* Fits: entries <= data bits / 6. */
    }
    while (reader->depth > 0 && reader->pending[reader->depth] == 0)
        --reader->depth;
    *out_value = v;
    return true;
}

bool wpe_netvalue_skip(WPE_NetValueReader* reader) {
    const uint32_t depth = reader->depth;
    WPE_NetValue value;
    if (!wpe_netvalue_read(reader, &value)) return false;
    /* A non-empty table opened a level: read until it (and everything in it) is closed. */
    while (reader->depth > depth)
        if (!wpe_netvalue_read(reader, &value)) return false;
    return true;
}
