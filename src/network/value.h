/**
 * @file value.h
 * @brief Script-style values in network messages: nil, booleans, numbers, strings, vectors and
 *        tables, packed with the bit writer and read back with a reader that checks everything.
 *
 * This is the wire format of the values scripts send each other. The script module converts Luau
 * values to and from it; the format itself is plain C so it can be tested and fuzzed on its own.
 *
 * Encoding: a 3-bit tag, then
 * - nil, false, true: nothing more
 * - an integral number that fits in 32 bits: a zigzag varint (small numbers take 1 byte)
 * - any other number: 64-bit double
 * - string: varint length, then the bytes (byte aligned)
 * - vector: three 32-bit floats
 * - table: varint entry count, then each entry's key and value
 *
 * Writers do not check structure. The reader trusts nothing: it rejects tables nested deeper than
 * WPE_NETVALUE_MAX_DEPTH, more than WPE_NETVALUE_MAX_VALUES values in one reader, entry counts the
 * remaining data cannot hold (so a forged count cannot make a decoder allocate gigabytes), and keys
 * that are not strings, booleans or numbers other than NaN (as in Luau).
 *
 * - Dependencies: core, math, network/bits
 * - Thread affinity: a reader belongs to one thread
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_NETWORK_VALUE_H
#define WPE_NETWORK_VALUE_H

#include <stddef.h>
#include <stdint.h>

#include "network/bits.h"

#define WPE_NETVALUE_MAX_DEPTH 16u
#define WPE_NETVALUE_MAX_VALUES 65536u

typedef enum WPE_NetValueType {
    WPE_NETVALUE_NIL = 0,
    WPE_NETVALUE_BOOL,
    WPE_NETVALUE_NUMBER,
    WPE_NETVALUE_STRING,
    WPE_NETVALUE_VECTOR,
    WPE_NETVALUE_TABLE,
} WPE_NetValueType;

/** One value as read. Strings point into the reader's data and are not NUL-terminated. */
typedef struct WPE_NetValue {
    WPE_NetValueType type;
    bool boolean;
    double number;
    const char* string;
    size_t length;
    WPE_Vec3 vector;
    uint32_t entries; /* TABLE: the next 2 * entries values are its keys and values, alternating. */
} WPE_NetValue;

typedef struct WPE_NetValueReader {
    WPE_BitReader* bits;                          /* Borrowed. */
    uint32_t depth;                               /* Tables open. */
    uint32_t pending[WPE_NETVALUE_MAX_DEPTH + 1]; /* Values left in each open table. */
    uint32_t values;                              /* Read so far. */
} WPE_NetValueReader;

/** nil. @thread_safety Owning thread of the writer */
void wpe_netvalue_write_nil(WPE_BitWriter* writer);

/** A boolean. @thread_safety Owning thread of the writer */
void wpe_netvalue_write_bool(WPE_BitWriter* writer, bool value);

/** A number (integral ones in 32 bits are short; -0 and others are sent exactly). @thread_safety Owning
 * thread of the writer */
void wpe_netvalue_write_number(WPE_BitWriter* writer, double value);

/** A string of `length` bytes (any bytes; not NUL-terminated). @thread_safety Owning thread of the writer */
void wpe_netvalue_write_string(WPE_BitWriter* writer, const char* data, size_t length);

/** A vector. @thread_safety Owning thread of the writer */
void wpe_netvalue_write_vector(WPE_BitWriter* writer, WPE_Vec3 value);

/**
 * Starts a table: write 2 * entries values after it, key then value for each entry.
 *
 * @thread_safety Owning thread of the writer
 */
void wpe_netvalue_write_table(WPE_BitWriter* writer, uint32_t entries);

/** A reader of values from `bits` (borrowed). @thread_safety Any thread */
WPE_NetValueReader wpe_netvalue_reader(WPE_BitReader* bits);

/**
 * Reads the next value, checking it.
 *
 * @return true with *out_value set; false on malformed data or past the end (the bit reader's
 *         error flag is then set; later calls also fail).
 *
 * @thread_safety Owning thread
 */
bool wpe_netvalue_read(WPE_NetValueReader* reader, WPE_NetValue* out_value);

/**
 * Reads one whole value (a table with everything in it).
 *
 * @return false on malformed data.
 *
 * @thread_safety Owning thread
 */
bool wpe_netvalue_skip(WPE_NetValueReader* reader);

#endif /* WPE_NETWORK_VALUE_H */
