#include "wpe_test.h"

#include <math.h>

#include "math/easing.h"
#include "math/wpe_math.h"

#define EPS 1e-5

static void assert_vec3_near(WPE_Vec3 expected, WPE_Vec3 actual, double tolerance) {
    WPE_TEST_ASSERT_NEAR(expected.x, actual.x, tolerance);
    WPE_TEST_ASSERT_NEAR(expected.y, actual.y, tolerance);
    WPE_TEST_ASSERT_NEAR(expected.z, actual.z, tolerance);
}

static void assert_mat4_near(const WPE_Mat4* expected, const WPE_Mat4* actual, double tolerance) {
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r)
            WPE_TEST_ASSERT_NEAR(expected->m[c][r], actual->m[c][r], tolerance);
    }
}

/* Quaternions q and -q are the same rotation. */
static void assert_quat_same_rotation(WPE_Quat a, WPE_Quat b) {
    const double dot = (double)a.x * b.x + (double)a.y * b.y + (double)a.z * b.z + (double)a.w * b.w;
    WPE_TEST_ASSERT_NEAR(1.0, dot < 0 ? -dot : dot, 1e-4);
}

static void test_math_vec3_basic_ops(void) {
    const WPE_Vec3 a = wpe_vec3(1, 2, 3);
    const WPE_Vec3 b = wpe_vec3(4, -5, 6);
    assert_vec3_near(wpe_vec3(5, -3, 9), wpe_vec3_add(a, b), EPS);
    assert_vec3_near(wpe_vec3(-3, 7, -3), wpe_vec3_sub(a, b), EPS);
    assert_vec3_near(wpe_vec3(2, 4, 6), wpe_vec3_scale(a, 2), EPS);
    assert_vec3_near(wpe_vec3(4, -10, 18), wpe_vec3_mul(a, b), EPS);
    WPE_TEST_ASSERT_NEAR(12.0, wpe_vec3_dot(a, b), EPS);
    assert_vec3_near(wpe_vec3(27, 6, -13), wpe_vec3_cross(a, b), EPS);
    WPE_TEST_ASSERT_NEAR(5.0, wpe_vec3_length(wpe_vec3(3, 4, 0)), EPS);
    assert_vec3_near(wpe_vec3(2.5f, -1.5f, 4.5f), wpe_vec3_lerp(a, b, 0.5f), EPS);
}

static void test_math_vec3_normalize_handles_zero(void) {
    assert_vec3_near(wpe_vec3(0, 0, 1), wpe_vec3_normalize(wpe_vec3(0, 0, 7)), EPS);
    assert_vec3_near(wpe_vec3(0, 0, 0), wpe_vec3_normalize(wpe_vec3(0, 0, 0)), 0);
}

static void test_math_quat_axis_angle_rotates_vector(void) {
    const WPE_Quat q = wpe_quat_from_axis_angle(wpe_vec3(0, 0, 1), WPE_PI / 2);
    assert_vec3_near(wpe_vec3(0, 1, 0), wpe_quat_rotate_vec3(q, wpe_vec3(1, 0, 0)), EPS);

    const WPE_Quat zero_axis = wpe_quat_from_axis_angle(wpe_vec3(0, 0, 0), 1.0f);
    assert_quat_same_rotation(wpe_quat_identity(), zero_axis);
}

static void test_math_quat_mul_applies_right_operand_first(void) {
    const WPE_Quat rx = wpe_quat_from_axis_angle(wpe_vec3(1, 0, 0), WPE_PI / 2);
    const WPE_Quat rz = wpe_quat_from_axis_angle(wpe_vec3(0, 0, 1), WPE_PI / 2);
    /* rz * rx: first X (y -> z), then Z (z stays z). */
    const WPE_Vec3 v = wpe_quat_rotate_vec3(wpe_quat_mul(rz, rx), wpe_vec3(0, 1, 0));
    assert_vec3_near(wpe_vec3(0, 0, 1), v, EPS);
}

static void test_math_quat_from_euler_matches_zyx_product(void) {
    const WPE_Vec3 angles = wpe_vec3(0.3f, -0.7f, 1.1f);
    const WPE_Quat qx = wpe_quat_from_axis_angle(wpe_vec3(1, 0, 0), angles.x);
    const WPE_Quat qy = wpe_quat_from_axis_angle(wpe_vec3(0, 1, 0), angles.y);
    const WPE_Quat qz = wpe_quat_from_axis_angle(wpe_vec3(0, 0, 1), angles.z);
    const WPE_Quat expected = wpe_quat_mul(qz, wpe_quat_mul(qy, qx));
    assert_quat_same_rotation(expected, wpe_quat_from_euler_xyz(angles));
}

static void test_math_quat_normalize_degenerate_is_identity(void) {
    assert_quat_same_rotation(wpe_quat_identity(),
                              wpe_quat_normalize((WPE_Quat){.x = 0, .y = 0, .z = 0, .w = 0}));
    assert_quat_same_rotation(wpe_quat_identity(),
                              wpe_quat_normalize((WPE_Quat){.x = NAN, .y = 0, .z = 0, .w = 1}));
    const WPE_Quat n = wpe_quat_normalize((WPE_Quat){.x = 0, .y = 0, .z = 0, .w = 5});
    WPE_TEST_ASSERT_NEAR(1.0, n.w, EPS);
}

static void test_math_quat_slerp_takes_shortest_path(void) {
    const WPE_Quat a = wpe_quat_identity();
    const WPE_Quat b = wpe_quat_from_axis_angle(wpe_vec3(0, 1, 0), WPE_PI / 2);
    const WPE_Quat neg_b = {.x = -b.x, .y = -b.y, .z = -b.z, .w = -b.w};
    const WPE_Quat half = wpe_quat_from_axis_angle(wpe_vec3(0, 1, 0), WPE_PI / 4);
    assert_quat_same_rotation(half, wpe_quat_slerp(a, b, 0.5f));
    assert_quat_same_rotation(half, wpe_quat_slerp(a, neg_b, 0.5f));
}

static void test_math_mat4_trs_transforms_point(void) {
    const WPE_Mat4 m = wpe_mat4_from_trs(
        wpe_vec3(10, 0, 0), wpe_quat_from_axis_angle(wpe_vec3(0, 0, 1), WPE_PI / 2), wpe_vec3(2, 2, 2));
    /* Scale (1,0,0) -> (2,0,0), rotate -> (0,2,0), translate -> (10,2,0). */
    assert_vec3_near(wpe_vec3(10, 2, 0), wpe_mat4_transform_point(&m, wpe_vec3(1, 0, 0)), EPS);
    assert_vec3_near(wpe_vec3(0, 2, 0), wpe_mat4_transform_dir(&m, wpe_vec3(1, 0, 0)), EPS);
}

static void test_math_mat4_trs_normalizes_rotation(void) {
    const WPE_Quat scaled = {.x = 0, .y = 0, .z = 0, .w = 3};
    const WPE_Mat4 m = wpe_mat4_from_trs(wpe_vec3(0, 0, 0), scaled, wpe_vec3(1, 1, 1));
    const WPE_Mat4 identity = wpe_mat4_identity();
    assert_mat4_near(&identity, &m, EPS);
}

static void test_math_mat4_mul_applies_right_operand_first(void) {
    const WPE_Mat4 translate = wpe_mat4_from_trs(wpe_vec3(5, 0, 0), wpe_quat_identity(), wpe_vec3(1, 1, 1));
    const WPE_Mat4 scale = wpe_mat4_from_trs(wpe_vec3(0, 0, 0), wpe_quat_identity(), wpe_vec3(3, 3, 3));
    const WPE_Mat4 m = wpe_mat4_mul(&translate, &scale);
    assert_vec3_near(wpe_vec3(8, 0, 0), wpe_mat4_transform_point(&m, wpe_vec3(1, 0, 0)), EPS);
}

static void test_math_mat4_inverse_roundtrip_and_singular(void) {
    const WPE_Mat4 m = wpe_mat4_from_trs(
        wpe_vec3(1, 2, 3), wpe_quat_from_euler_xyz(wpe_vec3(0.4f, 0.2f, -0.9f)), wpe_vec3(2, 0.5f, 4));
    WPE_Mat4 inv;
    WPE_TEST_ASSERT(wpe_mat4_inverse(&m, &inv));
    const WPE_Mat4 product = wpe_mat4_mul(&m, &inv);
    const WPE_Mat4 identity = wpe_mat4_identity();
    assert_mat4_near(&identity, &product, 1e-4);

    const WPE_Mat4 singular = wpe_mat4_from_trs(wpe_vec3(0, 0, 0), wpe_quat_identity(), wpe_vec3(1, 0, 1));
    WPE_Mat4 untouched = identity;
    WPE_TEST_ASSERT(!wpe_mat4_inverse(&singular, &untouched));
    assert_mat4_near(&identity, &untouched, 0);
}

static void test_math_mat4_decompose_roundtrip(void) {
    const WPE_Vec3 t = wpe_vec3(-4, 7.5f, 1000);
    const WPE_Quat r = wpe_quat_from_euler_xyz(wpe_vec3(1.2f, -0.4f, 2.9f));
    const WPE_Vec3 s = wpe_vec3(0.25f, 3, 12);
    const WPE_Mat4 m = wpe_mat4_from_trs(t, r, s);

    WPE_Vec3 out_t, out_s;
    WPE_Quat out_r;
    WPE_TEST_ASSERT(wpe_mat4_decompose(&m, &out_t, &out_r, &out_s));
    assert_vec3_near(t, out_t, 1e-3);
    assert_vec3_near(s, out_s, 1e-4);
    assert_quat_same_rotation(r, out_r);
}

static void test_math_mat4_decompose_mirrored_reconstructs(void) {
    const WPE_Mat4 m = wpe_mat4_from_trs(
        wpe_vec3(1, 2, 3), wpe_quat_from_euler_xyz(wpe_vec3(0.5f, 0.1f, 0.3f)), wpe_vec3(-2, 1, 1));
    WPE_Vec3 t, s;
    WPE_Quat r;
    WPE_TEST_ASSERT(wpe_mat4_decompose(&m, &t, &r, &s));
    const WPE_Mat4 rebuilt = wpe_mat4_from_trs(t, r, s);
    assert_mat4_near(&m, &rebuilt, 1e-4);
}

static void test_math_mat4_decompose_rejects_zero_scale(void) {
    const WPE_Mat4 m = wpe_mat4_from_trs(wpe_vec3(1, 1, 1), wpe_quat_identity(), wpe_vec3(1, 0, 1));
    WPE_Vec3 t = wpe_vec3(9, 9, 9), s = wpe_vec3(9, 9, 9);
    WPE_Quat r = wpe_quat_identity();
    WPE_TEST_ASSERT(!wpe_mat4_decompose(&m, &t, &r, &s));
    assert_vec3_near(wpe_vec3(9, 9, 9), t, 0);
}

static void test_math_extreme_values_stay_finite(void) {
    const WPE_Mat4 m = wpe_mat4_from_trs(wpe_vec3(1e30f, -1e30f, 1e30f),
                                         wpe_quat_from_euler_xyz(wpe_vec3(1000.0f, -2000.0f, 3000.0f)),
                                         wpe_vec3(1e-20f, 1e20f, 1));
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r)
            WPE_TEST_ASSERT(isfinite(m.m[c][r]));
    }
}

static void test_math_perspective_maps_near_far_to_clip_range(void) {
    const WPE_Mat4 p = wpe_mat4_perspective(WPE_PI / 2, 1.0f, 0.5f, 50.0f);
    const WPE_Vec4 near_pt = wpe_mat4_mul_vec4(&p, wpe_vec4(0, 0, -0.5f, 1));
    const WPE_Vec4 far_pt = wpe_mat4_mul_vec4(&p, wpe_vec4(0, 0, -50.0f, 1));
    WPE_TEST_ASSERT_NEAR(-1.0, near_pt.z / near_pt.w, 1e-5);
    WPE_TEST_ASSERT_NEAR(1.0, far_pt.z / far_pt.w, 1e-4);
    /* 90 degree FOV: a point at 45 degrees up lands on the top edge. */
    const WPE_Vec4 edge = wpe_mat4_mul_vec4(&p, wpe_vec4(0, 2, -2, 1));
    WPE_TEST_ASSERT_NEAR(1.0, edge.y / edge.w, 1e-5);
}

static void test_math_orthographic_and_look_at(void) {
    const WPE_Mat4 o = wpe_mat4_orthographic(0, 320, 0, 240, -1, 1);
    const WPE_Vec4 corner = wpe_mat4_mul_vec4(&o, wpe_vec4(320, 240, 0, 1));
    WPE_TEST_ASSERT_NEAR(1.0, corner.x, EPS);
    WPE_TEST_ASSERT_NEAR(1.0, corner.y, EPS);

    const WPE_Mat4 view = wpe_mat4_look_at(wpe_vec3(0, 0, 5), wpe_vec3(0, 0, 0), wpe_vec3(0, 1, 0));
    assert_vec3_near(wpe_vec3(0, 0, -5), wpe_mat4_transform_point(&view, wpe_vec3(0, 0, 0)), EPS);
    const WPE_Mat4 side = wpe_mat4_look_at(wpe_vec3(5, 0, 0), wpe_vec3(0, 0, 0), wpe_vec3(0, 1, 0));
    assert_vec3_near(wpe_vec3(0, 0, -5), wpe_mat4_transform_point(&side, wpe_vec3(0, 0, 0)), EPS);
}

static void test_math_easing(void) {
    for (int e = 0; e < WPE_EASE_COUNT; ++e) {
        const WPE_Ease ease = (WPE_Ease)e;
        /* Every curve starts at 0 and ends at 1, clamps outside, and is continuous at the ends. */
        WPE_TEST_ASSERT_NEAR(0.0, wpe_ease(ease, 0.0f), 0);
        WPE_TEST_ASSERT_NEAR(1.0, wpe_ease(ease, 1.0f), 0);
        WPE_TEST_ASSERT_NEAR(0.0, wpe_ease(ease, -3.0f), 0);
        WPE_TEST_ASSERT_NEAR(1.0, wpe_ease(ease, 7.0f), 0);
        WPE_TEST_ASSERT_NEAR(0.0, wpe_ease(ease, NAN), 0);
        WPE_TEST_ASSERT_MSG(fabsf(wpe_ease(ease, 1e-4f)) < 0.01f, "%s jumps at 0", wpe_ease_name(ease));
        WPE_TEST_ASSERT_MSG(fabsf(wpe_ease(ease, 1.0f - 1e-4f) - 1.0f) < 0.01f, "%s jumps at 1",
                            wpe_ease_name(ease));
        /* Names round-trip. */
        WPE_Ease found = WPE_EASE_LINEAR;
        WPE_TEST_ASSERT(wpe_ease_from_name(wpe_ease_name(ease), &found) && found == ease);
        /* Only back and elastic leave [0, 1]; in_out curves pass through the middle. */
        const bool overshoots = (ease >= WPE_EASE_IN_BACK && ease <= WPE_EASE_IN_OUT_ELASTIC);
        bool outside = false;
        for (int i = 0; i <= 1000; ++i) {
            const float v = wpe_ease(ease, (float)i / 1000.0f);
            outside |= v < -1e-6f || v > 1.0f + 1e-6f;
        }
        WPE_TEST_ASSERT_MSG(outside == overshoots, "%s", wpe_ease_name(ease));
    }
    WPE_TEST_ASSERT_NEAR(0.5, wpe_ease(WPE_EASE_LINEAR, 0.5f), EPS);
    WPE_TEST_ASSERT_NEAR(0.25, wpe_ease(WPE_EASE_IN_QUAD, 0.5f), EPS);
    WPE_TEST_ASSERT_NEAR(0.75, wpe_ease(WPE_EASE_OUT_QUAD, 0.5f), EPS);
    WPE_TEST_ASSERT_NEAR(0.5, wpe_ease(WPE_EASE_IN_OUT_CUBIC, 0.5f), EPS);
    WPE_TEST_ASSERT_NEAR(0.5, wpe_ease(WPE_EASE_SMOOTH, 0.5f), EPS);
    WPE_Ease unused;
    WPE_TEST_ASSERT(!wpe_ease_from_name("wobbly", &unused) && !wpe_ease_from_name(NULL, &unused));
    WPE_TEST_ASSERT(wpe_ease_name((WPE_Ease)99) == NULL);
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_math_easing),
              WPE_TEST_CASE(test_math_perspective_maps_near_far_to_clip_range),
              WPE_TEST_CASE(test_math_orthographic_and_look_at), WPE_TEST_CASE(test_math_vec3_basic_ops),
              WPE_TEST_CASE(test_math_vec3_normalize_handles_zero),
              WPE_TEST_CASE(test_math_quat_axis_angle_rotates_vector),
              WPE_TEST_CASE(test_math_quat_mul_applies_right_operand_first),
              WPE_TEST_CASE(test_math_quat_from_euler_matches_zyx_product),
              WPE_TEST_CASE(test_math_quat_normalize_degenerate_is_identity),
              WPE_TEST_CASE(test_math_quat_slerp_takes_shortest_path),
              WPE_TEST_CASE(test_math_mat4_trs_transforms_point),
              WPE_TEST_CASE(test_math_mat4_trs_normalizes_rotation),
              WPE_TEST_CASE(test_math_mat4_mul_applies_right_operand_first),
              WPE_TEST_CASE(test_math_mat4_inverse_roundtrip_and_singular),
              WPE_TEST_CASE(test_math_mat4_decompose_roundtrip),
              WPE_TEST_CASE(test_math_mat4_decompose_mirrored_reconstructs),
              WPE_TEST_CASE(test_math_mat4_decompose_rejects_zero_scale),
              WPE_TEST_CASE(test_math_extreme_values_stay_finite))
