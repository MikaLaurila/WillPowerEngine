#include "network/bits.h"

#include <math.h>
#include <string.h>

#define WPE__QUAT_RANGE 0.70710678118654752f /* 1/sqrt(2): no smaller component exceeds it. */

WPE_BitWriter wpe_bits_writer(void* buffer, size_t capacity) {
    return (WPE_BitWriter){.data = buffer, .capacity = buffer ? capacity : 0};
}

WPE_BitReader wpe_bits_reader(const void* data, size_t size) {
    return (WPE_BitReader){.data = data, .size = data ? size : 0};
}

size_t wpe_bits_writer_size(const WPE_BitWriter* writer) {
    return (writer->bits + 7) / 8;
}

size_t wpe_bits_reader_remaining(const WPE_BitReader* reader) {
    return reader->error ? 0 : reader->size * 8 - reader->bits;
}

/* ---- Raw bits -------------------------------------------------------------------------------- */

void wpe_bits_write(WPE_BitWriter* writer, uint32_t value, unsigned count) {
    if (writer->error) return;
    if (count == 0 || count > 32 || writer->bits + count > writer->capacity * 8) {
        writer->error = true;
        return;
    }
    uint64_t rest = count == 32 ? value : value & ((1u << count) - 1u);
    while (count > 0) {
        const size_t byte = writer->bits / 8;
        const unsigned offset = (unsigned)(writer->bits % 8);
        const unsigned take = 8 - offset < count ? 8 - offset : count;
        const unsigned keep = (1u << offset) - 1u; /* Bits already written in this byte. */
        writer->data[byte] =
            (uint8_t)((writer->data[byte] & keep) | ((rest & ((1u << take) - 1u)) << offset));
        rest >>= take;
        count -= take;
        writer->bits += take;
    }
}

uint32_t wpe_bits_read(WPE_BitReader* reader, unsigned count) {
    if (reader->error) return 0;
    if (count == 0 || count > 32 || reader->bits + count > reader->size * 8) {
        reader->error = true;
        return 0;
    }
    uint64_t value = 0;
    unsigned done = 0;
    while (done < count) {
        const size_t byte = reader->bits / 8;
        const unsigned offset = (unsigned)(reader->bits % 8);
        const unsigned take = 8 - offset < count - done ? 8 - offset : count - done;
        const uint64_t part = ((uint64_t)reader->data[byte] >> offset) & ((1u << take) - 1u);
        value |= part << done;
        done += take;
        reader->bits += take;
    }
    return (uint32_t)value;
}

void wpe_bits_write_bool(WPE_BitWriter* writer, bool value) {
    wpe_bits_write(writer, value ? 1u : 0u, 1);
}

bool wpe_bits_read_bool(WPE_BitReader* reader) {
    return wpe_bits_read(reader, 1) != 0;
}

/* ---- Integers -------------------------------------------------------------------------------- */

void wpe_bits_write_varint(WPE_BitWriter* writer, uint32_t value) {
    do {
        const uint32_t group = value & 0x7Fu;
        value >>= 7;
        wpe_bits_write(writer, group | (value ? 0x80u : 0u), 8);
    } while (value != 0);
}

uint32_t wpe_bits_read_varint(WPE_BitReader* reader) {
    uint32_t value = 0;
    for (unsigned group = 0; group < 5; ++group) {
        const uint32_t byte = wpe_bits_read(reader, 8);
        if (reader->error) return 0;
        if (group == 4 && byte > 0x0Fu) break; /* Above UINT32_MAX, or a sixth group. */
        value |= (byte & 0x7Fu) << (7 * group);
        if (!(byte & 0x80u)) return value;
    }
    reader->error = true;
    return 0;
}

void wpe_bits_write_signed(WPE_BitWriter* writer, int32_t value) {
    /* Zigzag: 0, -1, 1, -2, 2... become 0, 1, 2, 3, 4... */
    const uint32_t u = (uint32_t)value;
    wpe_bits_write_varint(writer, (u << 1) ^ (value < 0 ? UINT32_MAX : 0u));
}

int32_t wpe_bits_read_signed(WPE_BitReader* reader) {
    const uint32_t u = wpe_bits_read_varint(reader);
    const uint32_t bits = (u >> 1) ^ (u & 1u ? UINT32_MAX : 0u);
    int32_t value;
    memcpy(&value, &bits, sizeof value); /* Two's complement, without implementation-defined casts. */
    return value;
}

/* ---- Floats ---------------------------------------------------------------------------------- */

void wpe_bits_write_float(WPE_BitWriter* writer, float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    wpe_bits_write(writer, bits, 32);
}

float wpe_bits_read_float(WPE_BitReader* reader) {
    const uint32_t bits = wpe_bits_read(reader, 32);
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

void wpe_bits_write_double(WPE_BitWriter* writer, double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof bits);
    wpe_bits_write(writer, (uint32_t)bits, 32);
    wpe_bits_write(writer, (uint32_t)(bits >> 32), 32);
}

double wpe_bits_read_double(WPE_BitReader* reader) {
    const uint64_t low = wpe_bits_read(reader, 32);
    const uint64_t high = wpe_bits_read(reader, 32);
    const uint64_t bits = low | high << 32;
    double value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static bool wpe__range_valid(float min, float max, unsigned bits) {
    return bits >= 1 && bits <= 32 && isfinite(min) && isfinite(max) && max > min;
}

static double wpe__steps(unsigned bits) {
    return bits == 32 ? 4294967295.0 : (double)((1u << bits) - 1u);
}

void wpe_bits_write_quantized(WPE_BitWriter* writer, float value, float min, float max, unsigned bits) {
    if (!wpe__range_valid(min, max, bits)) {
        writer->error = true;
        return;
    }
    double t = ((double)value - min) / ((double)max - min);
    if (!(t > 0.0)) t = 0.0; /* Also NaN. */
    if (t > 1.0) t = 1.0;
    wpe_bits_write(writer, (uint32_t)(t * wpe__steps(bits) + 0.5), bits);
}

float wpe_bits_read_quantized(WPE_BitReader* reader, float min, float max, unsigned bits) {
    if (!wpe__range_valid(min, max, bits)) {
        reader->error = true;
        return min;
    }
    const double t = wpe_bits_read(reader, bits) / wpe__steps(bits);
    const float value = (float)(min + t * ((double)max - min));
    return value > max ? max : value < min ? min : value;
}

void wpe_bits_write_vec3(WPE_BitWriter* writer, WPE_Vec3 value, float min, float max, unsigned bits) {
    for (int i = 0; i < 3; ++i)
        wpe_bits_write_quantized(writer, value.v[i], min, max, bits);
}

WPE_Vec3 wpe_bits_read_vec3(WPE_BitReader* reader, float min, float max, unsigned bits) {
    WPE_Vec3 value;
    for (int i = 0; i < 3; ++i)
        value.v[i] = wpe_bits_read_quantized(reader, min, max, bits);
    return value;
}

/* ---- Rotations ------------------------------------------------------------------------------- */

void wpe_bits_write_quat(WPE_BitWriter* writer, WPE_Quat value, unsigned bits_per_component) {
    if (bits_per_component < 2 || bits_per_component > 32) {
        writer->error = true;
        return;
    }
    const float length = sqrtf(value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w);
    if (!(length > 0.0f) || !isfinite(length)) {
        value = (WPE_Quat){.x = 0, .y = 0, .z = 0, .w = 1};
    } else {
        for (int i = 0; i < 4; ++i)
            value.v[i] /= length;
    }
    unsigned largest = 0;
    for (unsigned i = 1; i < 4; ++i)
        if (fabsf(value.v[i]) > fabsf(value.v[largest])) largest = i;
    const float sign = value.v[largest] < 0.0f ? -1.0f : 1.0f;
    wpe_bits_write(writer, largest, 2);
    for (unsigned i = 0; i < 4; ++i)
        if (i != largest)
            wpe_bits_write_quantized(writer, sign * value.v[i], -WPE__QUAT_RANGE, WPE__QUAT_RANGE,
                                     bits_per_component);
}

WPE_Quat wpe_bits_read_quat(WPE_BitReader* reader, unsigned bits_per_component) {
    const WPE_Quat identity = {.x = 0, .y = 0, .z = 0, .w = 1};
    if (bits_per_component < 2 || bits_per_component > 32) {
        reader->error = true;
        return identity;
    }
    const unsigned largest = wpe_bits_read(reader, 2);
    WPE_Quat q = {0};
    float sum = 0.0f;
    for (unsigned i = 0; i < 4; ++i) {
        if (i == largest) continue;
        q.v[i] = wpe_bits_read_quantized(reader, -WPE__QUAT_RANGE, WPE__QUAT_RANGE, bits_per_component);
        sum += q.v[i] * q.v[i];
    }
    if (reader->error) return identity;
    /* Malformed input can make the three exceed unit length; the result is normalized anyway. */
    q.v[largest] = sum < 1.0f ? sqrtf(1.0f - sum) : 0.0f;
    const float length = sqrtf(sum + q.v[largest] * q.v[largest]);
    for (int i = 0; i < 4; ++i)
        q.v[i] /= length; /* Never 0: 1 when sum < 1, else sqrt(sum) >= 1. */
    return q;
}

/* ---- Bytes ----------------------------------------------------------------------------------- */

void wpe_bits_write_align(WPE_BitWriter* writer) {
    const unsigned pad = (unsigned)((8 - writer->bits % 8) % 8);
    if (pad > 0) wpe_bits_write(writer, 0, pad);
}

void wpe_bits_read_align(WPE_BitReader* reader) {
    const unsigned pad = (unsigned)((8 - reader->bits % 8) % 8);
    if (pad > 0) (void)wpe_bits_read(reader, pad);
}

void wpe_bits_write_bytes(WPE_BitWriter* writer, const void* data, size_t size) {
    wpe_bits_write_align(writer);
    if (writer->error || size == 0) return;
    if (!data || size > writer->capacity - writer->bits / 8) {
        writer->error = true;
        return;
    }
    memcpy(writer->data + writer->bits / 8, data, size);
    writer->bits += size * 8;
}

const uint8_t* wpe_bits_read_bytes(WPE_BitReader* reader, size_t size) {
    static const uint8_t empty[1] = {0};
    wpe_bits_read_align(reader);
    if (reader->error) return NULL;
    if (size > reader->size - reader->bits / 8) {
        reader->error = true;
        return NULL;
    }
    if (!reader->data) return empty;                     /* Size 0 over no data. */
    const uint8_t* at = reader->data + reader->bits / 8; /* At most one past the end (size 0). */
    reader->bits += size * 8;
    return at;
}
