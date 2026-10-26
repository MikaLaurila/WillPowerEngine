/**
 * @file wpe_math.h
 * @brief Vector, quaternion and matrix types and operations for the engine.
 *
 * Conventions:
 * - Right-handed coordinates, +Y up. Angles are radians.
 * - WPE_Mat4 is column-major (`m[column][row]`) and transforms column vectors: p' = M * p.
 * - WPE_Quat stores (x, y, z, w) with w the scalar part; rotations use unit quaternions.
 * - Composition: wpe_mat4_mul(a, b) applies b first, then a.
 *
 * Trivial vector arithmetic is inline here. Matrix/quaternion work is implemented in wpe_math.c on
 * top of cglm, which is an implementation detail and must not be included by public headers.
 * The file is named wpe_math.h (not math/math.h) to avoid any confusion with <math.h>.
 *
 * - Dependencies: C standard library (cglm privately)
 * - Thread affinity: any thread (pure functions)
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_MATH_WPE_MATH_H
#define WPE_MATH_WPE_MATH_H

#include <math.h>

#define WPE_PI 3.14159265358979323846f

typedef union WPE_Vec2 {
    struct {
        float x, y;
    };
    float v[2];
} WPE_Vec2;

typedef union WPE_Vec3 {
    struct {
        float x, y, z;
    };
    float v[3];
} WPE_Vec3;

/* 16-byte aligned so it can be handed to SIMD code directly. */
typedef union WPE_Vec4 {
    struct {
        float x, y, z, w;
    };
    alignas(16) float v[4];
} WPE_Vec4;

typedef union WPE_Quat {
    struct {
        float x, y, z, w;
    };
    alignas(16) float v[4];
} WPE_Quat;

typedef union WPE_Mat4 {
    alignas(16) float m[4][4]; /* m[column][row] */
    WPE_Vec4 cols[4];
} WPE_Mat4;

static_assert(sizeof(WPE_Vec3) == 3 * sizeof(float), "WPE_Vec3 must be tightly packed");
static_assert(sizeof(WPE_Mat4) == 16 * sizeof(float), "WPE_Mat4 must be tightly packed");

/* ---- Vector arithmetic (inline) ---------------------------------------------------------- */

static inline WPE_Vec3 wpe_vec3(float x, float y, float z) {
    return (WPE_Vec3){.x = x, .y = y, .z = z};
}

static inline WPE_Vec3 wpe_vec3_add(WPE_Vec3 a, WPE_Vec3 b) {
    return wpe_vec3(a.x + b.x, a.y + b.y, a.z + b.z);
}

static inline WPE_Vec3 wpe_vec3_sub(WPE_Vec3 a, WPE_Vec3 b) {
    return wpe_vec3(a.x - b.x, a.y - b.y, a.z - b.z);
}

static inline WPE_Vec3 wpe_vec3_scale(WPE_Vec3 v, float s) {
    return wpe_vec3(v.x * s, v.y * s, v.z * s);
}

/** Component-wise product. */
static inline WPE_Vec3 wpe_vec3_mul(WPE_Vec3 a, WPE_Vec3 b) {
    return wpe_vec3(a.x * b.x, a.y * b.y, a.z * b.z);
}

static inline float wpe_vec3_dot(WPE_Vec3 a, WPE_Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static inline WPE_Vec3 wpe_vec3_cross(WPE_Vec3 a, WPE_Vec3 b) {
    return wpe_vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

static inline float wpe_vec3_length_sq(WPE_Vec3 v) {
    return wpe_vec3_dot(v, v);
}

static inline float wpe_vec3_length(WPE_Vec3 v) {
    return sqrtf(wpe_vec3_dot(v, v));
}

/** Returns the unit vector in v's direction, or the zero vector when v has (near) zero length. */
static inline WPE_Vec3 wpe_vec3_normalize(WPE_Vec3 v) {
    const float len = wpe_vec3_length(v);
    if (!(len > 1e-20f)) return wpe_vec3(0.0f, 0.0f, 0.0f);
    return wpe_vec3_scale(v, 1.0f / len);
}

static inline WPE_Vec3 wpe_vec3_lerp(WPE_Vec3 a, WPE_Vec3 b, float t) {
    return wpe_vec3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t);
}

static inline WPE_Vec4 wpe_vec4(float x, float y, float z, float w) {
    return (WPE_Vec4){.x = x, .y = y, .z = z, .w = w};
}

static inline WPE_Quat wpe_quat_identity(void) {
    return (WPE_Quat){.x = 0.0f, .y = 0.0f, .z = 0.0f, .w = 1.0f};
}

static inline WPE_Mat4 wpe_mat4_identity(void) {
    return (WPE_Mat4){.m = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
}

/* ---- Quaternions --------------------------------------------------------------------------- */

/** Rotation of `radians` around `axis` (normalized internally; zero axis yields identity). */
WPE_Quat wpe_quat_from_axis_angle(WPE_Vec3 axis, float radians);

/** Rotation from Euler angles applied X first, then Y, then Z: q = qz * qy * qx. */
WPE_Quat wpe_quat_from_euler_xyz(WPE_Vec3 radians);

/** Hamilton product: the result applies b first, then a. */
WPE_Quat wpe_quat_mul(WPE_Quat a, WPE_Quat b);

/** Unit quaternion in q's direction; a (near) zero quaternion yields identity. */
WPE_Quat wpe_quat_normalize(WPE_Quat q);

/** Rotates v by unit quaternion q. */
WPE_Vec3 wpe_quat_rotate_vec3(WPE_Quat q, WPE_Vec3 v);

/** Shortest-path spherical interpolation between unit quaternions, t in [0, 1]. */
WPE_Quat wpe_quat_slerp(WPE_Quat a, WPE_Quat b, float t);

/* ---- Matrices ------------------------------------------------------------------------------ */

/** Returns a * b (b applied first). */
WPE_Mat4 wpe_mat4_mul(const WPE_Mat4* a, const WPE_Mat4* b);

/**
 * Builds translation * rotation * scale. The rotation is normalized first, so non-unit input
 * cannot introduce shear.
 */
WPE_Mat4 wpe_mat4_from_trs(WPE_Vec3 translation, WPE_Quat rotation, WPE_Vec3 scale);

/**
 * Inverts a matrix.
 *
 * @param m Matrix to invert; must not be NULL.
 * @param out Receives the inverse; must not be NULL. Unmodified when false is returned.
 * @return false when m is singular (|determinant| below 1e-12).
 */
bool wpe_mat4_inverse(const WPE_Mat4* m, WPE_Mat4* out);

/** Transforms a point (w = 1); translation applies. */
WPE_Vec3 wpe_mat4_transform_point(const WPE_Mat4* m, WPE_Vec3 p);

/** Transforms a direction (w = 0); translation does not apply. */
WPE_Vec3 wpe_mat4_transform_dir(const WPE_Mat4* m, WPE_Vec3 d);

/**
 * Splits an affine matrix into translation, rotation and (positive) scale.
 *
 * NOTE(limitation): matrices containing shear or a negative determinant (mirroring) cannot be
 * represented exactly as TRS; mirroring is folded into the rotation/scale best-effort.
 *
 * @return false when any scale axis is (near) zero; outputs are then unmodified.
 */
bool wpe_mat4_decompose(const WPE_Mat4* m, WPE_Vec3* out_translation, WPE_Quat* out_rotation,
                        WPE_Vec3* out_scale);

#endif /* WPE_MATH_WPE_MATH_H */
