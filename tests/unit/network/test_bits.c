#include <math.h>

#include "network/network.h"
#include "wpe_test.h"

/* A small deterministic generator (tests must not depend on rand()'s platform sequence). */
static uint64_t g_rng = 1;
static uint32_t next_u32(void) {
    g_rng = g_rng * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)(g_rng >> 32);
}
static double next_unit(void) {
    return next_u32() / 4294967296.0;
}

/* ---- Bits ------------------------------------------------------------------------------------ */

static void test_bits_round_trip_any_width(void) {
    static uint8_t buffer[4096];
    uint32_t values[1000];
    unsigned widths[1000];
    g_rng = 11;
    WPE_BitWriter w = wpe_bits_writer(buffer, sizeof buffer);
    size_t total = 0;
    for (int i = 0; i < 1000; ++i) {
        widths[i] = 1 + next_u32() % 32;
        values[i] = widths[i] == 32 ? next_u32() : next_u32() & ((1u << widths[i]) - 1u);
        wpe_bits_write(&w, values[i] | (widths[i] < 32 ? 0xFFFFFFFFu << widths[i] : 0u),
                       widths[i]); /* High bits ignored. */
        total += widths[i];
    }
    WPE_TEST_ASSERT(!w.error);
    WPE_TEST_ASSERT_EQ_UINT((total + 7) / 8, wpe_bits_writer_size(&w));
    WPE_BitReader r = wpe_bits_reader(buffer, wpe_bits_writer_size(&w));
    for (int i = 0; i < 1000; ++i)
        WPE_TEST_ASSERT_EQ_UINT(values[i], wpe_bits_read(&r, widths[i]));
    WPE_TEST_ASSERT(!r.error);
    WPE_TEST_ASSERT(wpe_bits_reader_remaining(&r) < 8);
}

static void test_bits_errors_are_sticky(void) {
    uint8_t buffer[2] = {0};
    WPE_BitWriter w = wpe_bits_writer(buffer, sizeof buffer);
    wpe_bits_write(&w, 0x3FF, 10);
    wpe_bits_write(&w, 0x7F, 7); /* 17 bits: does not fit. */
    WPE_TEST_ASSERT(w.error);
    wpe_bits_write(&w, 1, 1); /* Ignored. */
    WPE_TEST_ASSERT_EQ_UINT(10, w.bits);
    WPE_BitWriter bad = wpe_bits_writer(buffer, sizeof buffer);
    wpe_bits_write(&bad, 0, 0);
    WPE_TEST_ASSERT(bad.error);
    WPE_BitWriter none = wpe_bits_writer(NULL, 100);
    wpe_bits_write_bool(&none, true);
    WPE_TEST_ASSERT(none.error);

    WPE_BitReader r = wpe_bits_reader(buffer, sizeof buffer);
    WPE_TEST_ASSERT_EQ_UINT(0x3FF, wpe_bits_read(&r, 10));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_bits_read(&r, 7)); /* Past the end. */
    WPE_TEST_ASSERT(r.error);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_bits_read(&r, 1));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_bits_reader_remaining(&r));
    WPE_BitReader empty = wpe_bits_reader(NULL, 5);
    WPE_TEST_ASSERT(!wpe_bits_read_bool(&empty) && empty.error);
}

static void test_bits_integers(void) {
    uint8_t buffer[256];
    const uint32_t unsigned_values[] = {0,       1,       127,       128,       16383,     16384,
                                        2097151, 2097152, 268435455, 268435456, UINT32_MAX};
    const size_t sizes[] = {1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5};
    for (size_t i = 0; i < sizeof unsigned_values / sizeof unsigned_values[0]; ++i) {
        WPE_BitWriter w = wpe_bits_writer(buffer, sizeof buffer);
        wpe_bits_write_varint(&w, unsigned_values[i]);
        WPE_TEST_ASSERT_EQ_UINT(sizes[i], wpe_bits_writer_size(&w));
        WPE_BitReader r = wpe_bits_reader(buffer, wpe_bits_writer_size(&w));
        WPE_TEST_ASSERT_EQ_UINT(unsigned_values[i], wpe_bits_read_varint(&r));
        WPE_TEST_ASSERT(!r.error);
    }
    const int32_t signed_values[] = {0, -1, 1, -64, 63, -65, INT32_MIN, INT32_MAX};
    WPE_BitWriter w = wpe_bits_writer(buffer, sizeof buffer);
    for (size_t i = 0; i < sizeof signed_values / sizeof signed_values[0]; ++i)
        wpe_bits_write_signed(&w, signed_values[i]);
    WPE_TEST_ASSERT_EQ_UINT(1 + 1 + 1 + 1 + 1 + 2 + 5 + 5, wpe_bits_writer_size(&w));
    WPE_BitReader r = wpe_bits_reader(buffer, wpe_bits_writer_size(&w));
    for (size_t i = 0; i < sizeof signed_values / sizeof signed_values[0]; ++i)
        WPE_TEST_ASSERT_EQ_INT(signed_values[i], wpe_bits_read_signed(&r));

    /* Malformed varints: too many groups, or more than 32 bits. */
    const uint8_t six[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01};
    WPE_BitReader bad = wpe_bits_reader(six, sizeof six);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_bits_read_varint(&bad));
    WPE_TEST_ASSERT(bad.error);
    const uint8_t big[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x10};
    bad = wpe_bits_reader(big, sizeof big);
    (void)wpe_bits_read_varint(&bad);
    WPE_TEST_ASSERT(bad.error);
    const uint8_t cut[] = {0x80};
    bad = wpe_bits_reader(cut, sizeof cut);
    (void)wpe_bits_read_varint(&bad);
    WPE_TEST_ASSERT(bad.error);
}

static void test_bits_floats_are_exact(void) {
    uint8_t buffer[64];
    const float floats[] = {0.0f, -0.0f, 1.5f, -3.25e-30f, INFINITY, NAN};
    const double doubles[] = {0.1, -1e300, 5e-324};
    WPE_BitWriter w = wpe_bits_writer(buffer, sizeof buffer);
    wpe_bits_write_bool(&w, true); /* Unaligned on purpose. */
    for (size_t i = 0; i < 6; ++i)
        wpe_bits_write_float(&w, floats[i]);
    for (size_t i = 0; i < 3; ++i)
        wpe_bits_write_double(&w, doubles[i]);
    WPE_BitReader r = wpe_bits_reader(buffer, wpe_bits_writer_size(&w));
    WPE_TEST_ASSERT(wpe_bits_read_bool(&r));
    for (size_t i = 0; i < 6; ++i) {
        const float f = wpe_bits_read_float(&r);
        WPE_TEST_ASSERT(memcmp(&f, &floats[i], sizeof f) == 0);
    }
    for (size_t i = 0; i < 3; ++i)
        WPE_TEST_ASSERT(wpe_bits_read_double(&r) == doubles[i]);
    WPE_TEST_ASSERT(!r.error);
}

static void test_bits_quantized(void) {
    uint8_t buffer[16];
    g_rng = 3;
    for (unsigned bits = 1; bits <= 32; ++bits) {
        const double step = 200.0 / (bits == 32 ? 4294967295.0 : (double)((1u << bits) - 1u));
        for (int i = 0; i < 200; ++i) {
            const float value = (float)(next_unit() * 200.0 - 100.0);
            WPE_BitWriter w = wpe_bits_writer(buffer, sizeof buffer);
            wpe_bits_write_quantized(&w, value, -100.0f, 100.0f, bits);
            WPE_TEST_ASSERT_EQ_UINT(bits, w.bits);
            WPE_BitReader r = wpe_bits_reader(buffer, sizeof buffer);
            const float back = wpe_bits_read_quantized(&r, -100.0f, 100.0f, bits);
            WPE_TEST_ASSERT_NEAR(value, back, step / 2 + 1e-4);
        }
    }
    /* Clamping, NaN, the ends of the range, and bad ranges. */
    WPE_BitWriter w = wpe_bits_writer(buffer, sizeof buffer);
    wpe_bits_write_quantized(&w, 1000.0f, 0.0f, 1.0f, 8);
    wpe_bits_write_quantized(&w, -5.0f, 0.0f, 1.0f, 8);
    wpe_bits_write_quantized(&w, NAN, 2.0f, 3.0f, 8);
    WPE_BitReader r = wpe_bits_reader(buffer, sizeof buffer);
    WPE_TEST_ASSERT(wpe_bits_read_quantized(&r, 0.0f, 1.0f, 8) == 1.0f);
    WPE_TEST_ASSERT(wpe_bits_read_quantized(&r, 0.0f, 1.0f, 8) == 0.0f);
    WPE_TEST_ASSERT(wpe_bits_read_quantized(&r, 2.0f, 3.0f, 8) == 2.0f);
    wpe_bits_write_quantized(&w, 1.0f, 1.0f, 1.0f, 8);
    WPE_TEST_ASSERT(w.error);
    WPE_BitReader rr = wpe_bits_reader(buffer, sizeof buffer);
    (void)wpe_bits_read_quantized(&rr, 0.0f, 1.0f, 33);
    WPE_TEST_ASSERT(rr.error);

    const WPE_Vec3 position = {.x = 12.25f, .y = -0.5f, .z = 499.0f};
    w = wpe_bits_writer(buffer, sizeof buffer);
    wpe_bits_write_vec3(&w, position, -512.0f, 512.0f, 20);
    WPE_TEST_ASSERT_EQ_UINT(60, w.bits);
    r = wpe_bits_reader(buffer, sizeof buffer);
    const WPE_Vec3 got = wpe_bits_read_vec3(&r, -512.0f, 512.0f, 20);
    for (int i = 0; i < 3; ++i)
        WPE_TEST_ASSERT_NEAR(position.v[i], got.v[i], 0.001);
}

static WPE_Quat random_quat(void) {
    WPE_Quat q;
    float length = 0.0f;
    do {
        for (int i = 0; i < 4; ++i)
            q.v[i] = (float)(next_unit() * 2.0 - 1.0);
        length = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    } while (length < 0.1f);
    for (int i = 0; i < 4; ++i)
        q.v[i] /= length;
    return q;
}

/* The rotation angle between a and b, from the chord between them on the unit sphere (acos of a dot
 * product near 1 would lose most of its precision). q and -q are the same rotation. */
static double angle_between_degrees(WPE_Quat a, WPE_Quat b) {
    double same = 0.0;
    double opposite = 0.0;
    for (int i = 0; i < 4; ++i) {
        same += ((double)a.v[i] - b.v[i]) * ((double)a.v[i] - b.v[i]);
        opposite += ((double)a.v[i] + b.v[i]) * ((double)a.v[i] + b.v[i]);
    }
    const double chord = sqrt(same < opposite ? same : opposite);
    return 4.0 * asin(chord / 2.0 > 1.0 ? 1.0 : chord / 2.0) * 180.0 / 3.14159265358979323846;
}

static void test_bits_quaternions(void) {
    uint8_t buffer[16];
    g_rng = 21;
    double worst = 0.0;
    for (int i = 0; i < 5000; ++i) {
        const WPE_Quat q = random_quat();
        WPE_BitWriter w = wpe_bits_writer(buffer, sizeof buffer);
        wpe_bits_write_quat(&w, q, 15);
        WPE_TEST_ASSERT_EQ_UINT(2 + 3 * 15, w.bits);
        WPE_BitReader r = wpe_bits_reader(buffer, sizeof buffer);
        const WPE_Quat back = wpe_bits_read_quat(&r, 15);
        const double angle = angle_between_degrees(q, back);
        if (angle > worst) worst = angle;
    }
    WPE_TEST_ASSERT_MSG(worst < 0.01, "worst error %.6f degrees", worst);

    /* q and -q are the same rotation; zero and NaN become the identity. */
    const WPE_Quat q = random_quat();
    const WPE_Quat negated = {.x = -q.x, .y = -q.y, .z = -q.z, .w = -q.w};
    const WPE_Quat odd[] = {negated, {.x = 0, .y = 0, .z = 0, .w = 0}, {.x = NAN, .y = 0, .z = 0, .w = 1}};
    const WPE_Quat expected[] = {q, {.w = 1}, {.w = 1}};
    for (int i = 0; i < 3; ++i) {
        WPE_BitWriter w = wpe_bits_writer(buffer, sizeof buffer);
        wpe_bits_write_quat(&w, odd[i], 12);
        WPE_BitReader r = wpe_bits_reader(buffer, sizeof buffer);
        WPE_TEST_ASSERT(angle_between_degrees(expected[i], wpe_bits_read_quat(&r, 12)) < 0.1);
    }
    /* Any bits at all read as a unit rotation. */
    for (int i = 0; i < 2000; ++i) {
        for (size_t b = 0; b < sizeof buffer; ++b)
            buffer[b] = (uint8_t)next_u32();
        WPE_BitReader r = wpe_bits_reader(buffer, sizeof buffer);
        const WPE_Quat back = wpe_bits_read_quat(&r, 2 + (unsigned)(i % 31));
        const float length = sqrtf(back.x * back.x + back.y * back.y + back.z * back.z + back.w * back.w);
        WPE_TEST_ASSERT(isfinite(length));
        WPE_TEST_ASSERT_NEAR(1.0, length, 1e-5);
    }
}

static void test_bits_bytes(void) {
    uint8_t buffer[16];
    WPE_BitWriter w = wpe_bits_writer(buffer, sizeof buffer);
    wpe_bits_write(&w, 5, 3);
    wpe_bits_write_bytes(&w, "hello", 5);
    WPE_TEST_ASSERT_EQ_UINT(6 * 8, w.bits);
    wpe_bits_write_bytes(&w, "0123456789abcdef", 11); /* Does not fit. */
    WPE_TEST_ASSERT(w.error);
    WPE_BitReader r = wpe_bits_reader(buffer, 6);
    WPE_TEST_ASSERT_EQ_UINT(5, wpe_bits_read(&r, 3));
    const uint8_t* at = wpe_bits_read_bytes(&r, 5);
    WPE_TEST_ASSERT(at == buffer + 1 && memcmp(at, "hello", 5) == 0);
    WPE_TEST_ASSERT(wpe_bits_read_bytes(&r, 0) != NULL);
    WPE_TEST_ASSERT(wpe_bits_read_bytes(&r, 1) == NULL && r.error);
}

/* ---- Values ---------------------------------------------------------------------------------- */

static void test_netvalue_round_trip(void) {
    uint8_t buffer[512];
    WPE_BitWriter w = wpe_bits_writer(buffer, sizeof buffer);
    /* {name = "car\0x", lap = 3, time = 61.25, pos = vector(1,2,3), [1] = true, [-0.5] = {nil, false}} */
    wpe_netvalue_write_table(&w, 6);
    wpe_netvalue_write_string(&w, "name", 4);
    wpe_netvalue_write_string(&w, "car\0x", 5);
    wpe_netvalue_write_string(&w, "lap", 3);
    wpe_netvalue_write_number(&w, 3);
    wpe_netvalue_write_string(&w, "time", 4);
    wpe_netvalue_write_number(&w, 61.25);
    wpe_netvalue_write_string(&w, "pos", 3);
    wpe_netvalue_write_vector(&w, (WPE_Vec3){.x = 1, .y = 2, .z = 3});
    wpe_netvalue_write_number(&w, 1);
    wpe_netvalue_write_bool(&w, true);
    wpe_netvalue_write_number(&w, -0.5);
    wpe_netvalue_write_table(&w, 1);
    wpe_netvalue_write_number(&w, 2);
    wpe_netvalue_write_bool(&w, false);
    wpe_netvalue_write_number(&w, -0.0); /* A second top-level value. */
    WPE_TEST_ASSERT(!w.error);

    WPE_BitReader bits = wpe_bits_reader(buffer, wpe_bits_writer_size(&w));
    WPE_NetValueReader r = wpe_netvalue_reader(&bits);
    WPE_NetValue v;
    WPE_TEST_ASSERT(wpe_netvalue_read(&r, &v) && v.type == WPE_NETVALUE_TABLE && v.entries == 6);
    WPE_TEST_ASSERT_EQ_UINT(1, r.depth);
    WPE_TEST_ASSERT(wpe_netvalue_read(&r, &v) && v.type == WPE_NETVALUE_STRING && v.length == 4);
    WPE_TEST_ASSERT(memcmp(v.string, "name", 4) == 0);
    WPE_TEST_ASSERT(wpe_netvalue_read(&r, &v) && v.length == 5 && memcmp(v.string, "car\0x", 5) == 0);
    WPE_TEST_ASSERT(wpe_netvalue_skip(&r)); /* "lap" */
    WPE_TEST_ASSERT(wpe_netvalue_read(&r, &v) && v.type == WPE_NETVALUE_NUMBER && v.number == 3);
    WPE_TEST_ASSERT(wpe_netvalue_skip(&r));
    WPE_TEST_ASSERT(wpe_netvalue_read(&r, &v) && v.number == 61.25);
    WPE_TEST_ASSERT(wpe_netvalue_skip(&r));
    WPE_TEST_ASSERT(wpe_netvalue_read(&r, &v) && v.type == WPE_NETVALUE_VECTOR && v.vector.z == 3);
    WPE_TEST_ASSERT(wpe_netvalue_read(&r, &v) && v.number == 1);
    WPE_TEST_ASSERT(wpe_netvalue_read(&r, &v) && v.type == WPE_NETVALUE_BOOL && v.boolean);
    WPE_TEST_ASSERT(wpe_netvalue_read(&r, &v) && v.number == -0.5);
    WPE_TEST_ASSERT(wpe_netvalue_skip(&r)); /* The inner table, whole. */
    WPE_TEST_ASSERT_EQ_UINT(0, r.depth);
    WPE_TEST_ASSERT(wpe_netvalue_read(&r, &v) && v.number == 0.0 && signbit(v.number));
    WPE_TEST_ASSERT(!wpe_netvalue_read(&r, &v)); /* Nothing left. */
}

static void test_netvalue_numbers_are_compact(void) {
    uint8_t buffer[32];
    const double cases[] = {0, 100, -100, 2147483647.0, -2147483648.0, 2147483648.0, 0.5};
    const size_t bits[] = {3 + 8, 3 + 16, 3 + 16, 3 + 40, 3 + 40, 3 + 64, 3 + 64};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        WPE_BitWriter w = wpe_bits_writer(buffer, sizeof buffer);
        wpe_netvalue_write_number(&w, cases[i]);
        WPE_TEST_ASSERT_MSG(w.bits == bits[i], "%g took %zu bits", cases[i], w.bits);
        WPE_BitReader b = wpe_bits_reader(buffer, sizeof buffer);
        WPE_NetValueReader r = wpe_netvalue_reader(&b);
        WPE_NetValue v;
        WPE_TEST_ASSERT(wpe_netvalue_read(&r, &v) && v.number == cases[i]);
    }
    /* The example in docs/networking.md: {lap = 3, best = 61.25} takes 22 bytes. */
    WPE_BitWriter w = wpe_bits_writer(buffer, sizeof buffer);
    wpe_netvalue_write_table(&w, 2);
    wpe_netvalue_write_string(&w, "lap", 3);
    wpe_netvalue_write_number(&w, 3);
    wpe_netvalue_write_string(&w, "best", 4);
    wpe_netvalue_write_number(&w, 61.25);
    WPE_TEST_ASSERT_EQ_UINT(22, wpe_bits_writer_size(&w));
}

/* Encodes a value and reports whether it reads back whole. */
typedef void (*Encoder)(WPE_BitWriter* w);
static bool reads_back(Encoder encode) {
    static uint8_t buffer[4096];
    WPE_BitWriter w = wpe_bits_writer(buffer, sizeof buffer);
    encode(&w);
    WPE_TEST_ASSERT(!w.error);
    WPE_BitReader b = wpe_bits_reader(buffer, wpe_bits_writer_size(&w));
    WPE_NetValueReader r = wpe_netvalue_reader(&b);
    return wpe_netvalue_skip(&r);
}

static void nil_key(WPE_BitWriter* w) {
    wpe_netvalue_write_table(w, 1);
    wpe_netvalue_write_nil(w);
    wpe_netvalue_write_number(w, 1);
}
static void nan_key(WPE_BitWriter* w) {
    wpe_netvalue_write_table(w, 1);
    wpe_netvalue_write_number(w, NAN);
    wpe_netvalue_write_number(w, 1);
}
static void table_key(WPE_BitWriter* w) {
    wpe_netvalue_write_table(w, 1);
    wpe_netvalue_write_table(w, 0);
    wpe_netvalue_write_number(w, 1);
}
static void vector_key(WPE_BitWriter* w) {
    wpe_netvalue_write_table(w, 1);
    wpe_netvalue_write_vector(w, (WPE_Vec3){0});
    wpe_netvalue_write_number(w, 1);
}
static void nan_value(WPE_BitWriter* w) {
    wpe_netvalue_write_table(w, 1);
    wpe_netvalue_write_bool(w, true);
    wpe_netvalue_write_number(w, NAN);
}
static void nested(WPE_BitWriter* w, unsigned levels) {
    for (unsigned i = 0; i < levels; ++i) {
        wpe_netvalue_write_table(w, 1);
        wpe_netvalue_write_number(w, 1);
    }
    wpe_netvalue_write_nil(w);
}
static void nested_16(WPE_BitWriter* w) {
    nested(w, WPE_NETVALUE_MAX_DEPTH);
}
static void nested_17(WPE_BitWriter* w) {
    nested(w, WPE_NETVALUE_MAX_DEPTH + 1);
}
static void forged_count(WPE_BitWriter* w) {
    wpe_netvalue_write_table(w, 4000000000u); /* Claims 4 billion entries, has 2. */
    for (int i = 0; i < 4; ++i)
        wpe_netvalue_write_number(w, i);
}
static void empty_tables(WPE_BitWriter* w) {
    wpe_netvalue_write_table(w, 2);
    wpe_netvalue_write_string(w, "", 0);
    wpe_netvalue_write_table(w, 0);
    wpe_netvalue_write_bool(w, false);
    wpe_netvalue_write_table(w, 0);
}

static void test_netvalue_rejects_bad_structure(void) {
    WPE_TEST_ASSERT(!reads_back(nil_key));
    WPE_TEST_ASSERT(!reads_back(nan_key));
    WPE_TEST_ASSERT(!reads_back(table_key));
    WPE_TEST_ASSERT(!reads_back(vector_key));
    WPE_TEST_ASSERT(reads_back(nan_value));
    WPE_TEST_ASSERT(reads_back(nested_16));
    WPE_TEST_ASSERT(!reads_back(nested_17));
    WPE_TEST_ASSERT(!reads_back(forged_count));
    WPE_TEST_ASSERT(reads_back(empty_tables));

    /* No more than WPE_NETVALUE_MAX_VALUES values from one reader. */
    static uint8_t buffer[WPE_NETVALUE_MAX_VALUES];
    WPE_BitWriter w = wpe_bits_writer(buffer, sizeof buffer);
    for (uint32_t i = 0; i <= WPE_NETVALUE_MAX_VALUES; ++i)
        wpe_netvalue_write_nil(&w);
    WPE_BitReader b = wpe_bits_reader(buffer, wpe_bits_writer_size(&w));
    WPE_NetValueReader r = wpe_netvalue_reader(&b);
    uint32_t read = 0;
    while (wpe_netvalue_skip(&r))
        ++read;
    WPE_TEST_ASSERT_EQ_UINT(WPE_NETVALUE_MAX_VALUES, read);
}

/* ---- Fuzzing --------------------------------------------------------------------------------- */

/* Reads everything a reader gives; strings must lie inside the data. Returns values read. */
static uint32_t consume(const uint8_t* data, size_t size) {
    WPE_BitReader b = wpe_bits_reader(data, size);
    WPE_NetValueReader r = wpe_netvalue_reader(&b);
    WPE_NetValue v;
    uint32_t count = 0;
    while (wpe_netvalue_read(&r, &v)) {
        ++count;
        WPE_TEST_ASSERT(count <= WPE_NETVALUE_MAX_VALUES);
        WPE_TEST_ASSERT(r.depth <= WPE_NETVALUE_MAX_DEPTH);
        if (v.type == WPE_NETVALUE_STRING)
            WPE_TEST_ASSERT((const uint8_t*)v.string >= data &&
                            (const uint8_t*)v.string + v.length <= data + size);
        if (v.type == WPE_NETVALUE_TABLE) WPE_TEST_ASSERT((uint64_t)v.entries * 6u <= size * 8u);
    }
    return count;
}

/* A random valid value (depth-limited) written to `w`. */
static void random_value(WPE_BitWriter* w, unsigned depth, bool key) {
    const uint32_t kind = next_u32() % (key ? 3 : (depth < 6 ? 7 : 6));
    switch (kind) {
    case 0:
        wpe_netvalue_write_number(w, ((double)next_u32() - 2147483648.0) / (next_u32() % 2 ? 1.0 : 1024.0));
        break;
    case 1: {
        unsigned char text[24];
        const size_t length = next_u32() % sizeof text;
        for (size_t i = 0; i < length; ++i)
            text[i] = (unsigned char)next_u32();
        wpe_netvalue_write_string(w, (const char*)text, length);
        break;
    }
    case 2:
        wpe_netvalue_write_bool(w, next_u32() % 2);
        break;
    case 3:
        wpe_netvalue_write_nil(w);
        break;
    case 4:
        wpe_netvalue_write_vector(w, (WPE_Vec3){.x = (float)next_unit(), .y = -1, .z = 1e9f});
        break;
    case 5:
        wpe_netvalue_write_number(w, next_unit());
        break;
    default: {
        const uint32_t entries = next_u32() % 5;
        wpe_netvalue_write_table(w, entries);
        for (uint32_t i = 0; i < entries; ++i) {
            random_value(w, depth + 1, true);
            random_value(w, depth + 1, false);
        }
    }
    }
}

/* Writes one value as a reader gives it back: a valid encoding must come out identical. */
static size_t reencode(const uint8_t* data, size_t size, uint8_t* out, size_t capacity) {
    WPE_BitReader b = wpe_bits_reader(data, size);
    WPE_NetValueReader r = wpe_netvalue_reader(&b);
    WPE_BitWriter w = wpe_bits_writer(out, capacity);
    memset(out, 0, capacity);
    WPE_NetValue v;
    do {
        WPE_TEST_ASSERT(wpe_netvalue_read(&r, &v));
        switch (v.type) {
        case WPE_NETVALUE_NIL:
            wpe_netvalue_write_nil(&w);
            break;
        case WPE_NETVALUE_BOOL:
            wpe_netvalue_write_bool(&w, v.boolean);
            break;
        case WPE_NETVALUE_NUMBER:
            wpe_netvalue_write_number(&w, v.number);
            break;
        case WPE_NETVALUE_STRING:
            wpe_netvalue_write_string(&w, v.string, v.length);
            break;
        case WPE_NETVALUE_VECTOR:
            wpe_netvalue_write_vector(&w, v.vector);
            break;
        case WPE_NETVALUE_TABLE:
            wpe_netvalue_write_table(&w, v.entries);
            break;
        }
    } while (r.depth > 0);
    WPE_TEST_ASSERT(!b.error && !w.error);
    return wpe_bits_writer_size(&w);
}

static void test_netvalue_fuzz_random_bytes(void) {
    static uint8_t data[256];
    g_rng = 1234;
    uint64_t values = 0;
    for (int i = 0; i < 20000; ++i) {
        const size_t size = next_u32() % sizeof data;
        for (size_t b = 0; b < size; ++b)
            data[b] = (uint8_t)next_u32();
        values += consume(data, size);
    }
    WPE_TEST_ASSERT(values > 1000); /* Random data does decode partly; it must just never misbehave. */
}

static void test_netvalue_fuzz_mutations(void) {
    static uint8_t data[8192];
    static uint8_t again[8192];
    g_rng = 77;
    for (int i = 0; i < 3000; ++i) {
        WPE_BitWriter w = wpe_bits_writer(data, sizeof data);
        random_value(&w, 0, false);
        WPE_TEST_ASSERT(!w.error);
        const size_t size = wpe_bits_writer_size(&w);
        /* Valid: it reads back, and writing what was read gives the same bytes. */
        WPE_BitReader b = wpe_bits_reader(data, size);
        WPE_NetValueReader r = wpe_netvalue_reader(&b);
        WPE_TEST_ASSERT(wpe_netvalue_skip(&r));
        WPE_TEST_ASSERT_EQ_UINT(0, r.depth);
        WPE_TEST_ASSERT_EQ_UINT(size, reencode(data, size, again, sizeof again));
        WPE_TEST_ASSERT(memcmp(data, again, size) == 0);
        /* Broken: flipped bits, then a cut. */
        for (int flips = 1 + (int)(next_u32() % 4); flips > 0; --flips) {
            const size_t bit = next_u32() % (size * 8);
            data[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        }
        (void)consume(data, size);
        (void)consume(data, next_u32() % (size + 1));
    }
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_bits_round_trip_any_width), WPE_TEST_CASE(test_bits_errors_are_sticky),
              WPE_TEST_CASE(test_bits_integers), WPE_TEST_CASE(test_bits_floats_are_exact),
              WPE_TEST_CASE(test_bits_quantized), WPE_TEST_CASE(test_bits_quaternions),
              WPE_TEST_CASE(test_bits_bytes), WPE_TEST_CASE(test_netvalue_round_trip),
              WPE_TEST_CASE(test_netvalue_numbers_are_compact),
              WPE_TEST_CASE(test_netvalue_rejects_bad_structure),
              WPE_TEST_CASE(test_netvalue_fuzz_random_bytes), WPE_TEST_CASE(test_netvalue_fuzz_mutations))
