/**
 * @file bits.h
 * @brief Bit-level message packing: write values in exactly as many bits as they need, and read
 *        them back safely from data that came over the network.
 *
 * A network message is usually small but sent many times a second, so every bit counts: a flag
 * takes 1 bit, a player slot 2, a position 3 x 16 bits instead of 3 x 32 (quantization: a value in
 * a known range stored as one of 2^bits evenly spaced steps), and a rotation 2 + 3 x 15 bits instead
 * of 4 x 32 ("smallest three": only three of a unit quaternion's four components are sent).
 *
 * Bits are packed least significant first into little-endian bytes, the same on every platform.
 *
 * Errors are sticky and never undefined behaviour: a writer that runs out of room, or a reader that
 * runs past the end or meets malformed data, sets its `error` flag; later writes are ignored and
 * later reads return zeros. Check the flag once at the end instead of after every call.
 *
 * - Dependencies: core, math
 * - Thread affinity: a writer or reader belongs to one thread; separate ones may run concurrently
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_NETWORK_BITS_H
#define WPE_NETWORK_BITS_H

#include <stddef.h>
#include <stdint.h>

#include "math/wpe_math.h"

typedef struct WPE_BitWriter {
    uint8_t* data;   /* Borrowed. */
    size_t capacity; /* Bytes. */
    size_t bits;     /* Written so far. */
    bool error;      /* Ran out of room (or a bad argument). */
} WPE_BitWriter;

typedef struct WPE_BitReader {
    const uint8_t* data; /* Borrowed. */
    size_t size;         /* Bytes. */
    size_t bits;         /* Read so far. */
    bool error;          /* Read past the end, or malformed data. */
} WPE_BitReader;

/** A writer over `capacity` bytes of `buffer` (borrowed until done). @thread_safety Any thread */
WPE_BitWriter wpe_bits_writer(void* buffer, size_t capacity);

/** A reader over `size` bytes of `data` (borrowed until done). @thread_safety Any thread */
WPE_BitReader wpe_bits_reader(const void* data, size_t size);

/** Bytes used so far (a partial last byte counts). @thread_safety Owning thread */
size_t wpe_bits_writer_size(const WPE_BitWriter* writer);

/** Bits left to read. @thread_safety Owning thread */
size_t wpe_bits_reader_remaining(const WPE_BitReader* reader);

/**
 * Writes the low `count` bits of `value` (count 1..32; others set the error flag).
 *
 * @thread_safety Owning thread
 */
void wpe_bits_write(WPE_BitWriter* writer, uint32_t value, unsigned count);

/** Reads `count` bits (1..32). @thread_safety Owning thread */
uint32_t wpe_bits_read(WPE_BitReader* reader, unsigned count);

/** One bit. @thread_safety Owning thread */
void wpe_bits_write_bool(WPE_BitWriter* writer, bool value);
bool wpe_bits_read_bool(WPE_BitReader* reader);

/**
 * An unsigned integer in 1..5 bytes' worth of 7-bit groups: small numbers are short (0..127 take
 * 8 bits). A reader rejects encodings over 5 groups or above UINT32_MAX.
 *
 * @thread_safety Owning thread
 */
void wpe_bits_write_varint(WPE_BitWriter* writer, uint32_t value);
uint32_t wpe_bits_read_varint(WPE_BitReader* reader);

/** A signed integer as a varint, small magnitudes short either side of zero. @thread_safety Owning thread */
void wpe_bits_write_signed(WPE_BitWriter* writer, int32_t value);
int32_t wpe_bits_read_signed(WPE_BitReader* reader);

/** A float, exactly (32 bits). @thread_safety Owning thread */
void wpe_bits_write_float(WPE_BitWriter* writer, float value);
float wpe_bits_read_float(WPE_BitReader* reader);

/** A double, exactly (64 bits). @thread_safety Owning thread */
void wpe_bits_write_double(WPE_BitWriter* writer, double value);
double wpe_bits_read_double(WPE_BitReader* reader);

/**
 * A float in [min, max] as one of 2^bits evenly spaced steps (bits 1..32): the error is at most
 * half a step, (max - min) / (2^bits - 1) / 2. Values outside the range are clamped; NaN becomes
 * `min`. Reads always return a value in [min, max].
 *
 * @thread_safety Owning thread
 */
void wpe_bits_write_quantized(WPE_BitWriter* writer, float value, float min, float max, unsigned bits);
float wpe_bits_read_quantized(WPE_BitReader* reader, float min, float max, unsigned bits);

/** A vector, each component quantized in [min, max]. @thread_safety Owning thread */
void wpe_bits_write_vec3(WPE_BitWriter* writer, WPE_Vec3 value, float min, float max, unsigned bits);
WPE_Vec3 wpe_bits_read_vec3(WPE_BitReader* reader, float min, float max, unsigned bits);

/**
 * A rotation as "smallest three": the index of the largest component (2 bits) and the other three,
 * each quantized in bits_per_component (2..32) over [-1/sqrt(2), 1/sqrt(2)]. The largest one is
 * rebuilt on reading from the unit length. q and -q are the same rotation, so the sign is chosen
 * to make the largest component positive. `value` should be normalized; it is normalized first
 * (identity if zero or not finite). 15 bits per component give an error well under 0.01 degrees.
 *
 * @thread_safety Owning thread
 */
void wpe_bits_write_quat(WPE_BitWriter* writer, WPE_Quat value, unsigned bits_per_component);
WPE_Quat wpe_bits_read_quat(WPE_BitReader* reader, unsigned bits_per_component);

/** Pads with zero bits to the next byte boundary. @thread_safety Owning thread */
void wpe_bits_write_align(WPE_BitWriter* writer);
void wpe_bits_read_align(WPE_BitReader* reader);

/**
 * Aligns, then copies `size` bytes.
 *
 * @thread_safety Owning thread
 */
void wpe_bits_write_bytes(WPE_BitWriter* writer, const void* data, size_t size);

/**
 * Aligns, then returns a pointer to the next `size` bytes inside the reader's data (no copy), or
 * NULL (with the error flag) when fewer remain.
 *
 * @thread_safety Owning thread
 */
const uint8_t* wpe_bits_read_bytes(WPE_BitReader* reader, size_t size);

#endif /* WPE_NETWORK_BITS_H */
