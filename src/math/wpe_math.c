#include "math/wpe_math.h"

#include <cglm/cglm.h>

#include "core/debug.h"

/*
 * cglm takes mutable array parameters even for inputs it only reads. These casts drop const for
 * read-only inputs; cglm never writes through them.
 */
#define WPE__CGLM_MAT4(mat_ptr) ((vec4*)(mat_ptr)->m)

static_assert(alignof(WPE_Mat4) >= 16, "cglm mat4 requires 16-byte alignment");
static_assert(alignof(WPE_Quat) >= 16, "cglm versor requires 16-byte alignment");

WPE_Quat wpe_quat_from_axis_angle(WPE_Vec3 axis, float radians) {
    WPE_Vec3 unit = wpe_vec3_normalize(axis); /* Non-const: cglm takes mutable arrays. */
    if (wpe_vec3_length_sq(unit) == 0.0f) return wpe_quat_identity();

    WPE_Quat q;
    glm_quatv(q.v, radians, unit.v);
    return q;
}

WPE_Quat wpe_quat_from_euler_xyz(WPE_Vec3 radians) {
    /* Composed explicitly: cglm's euler helpers use a different application order. */
    const WPE_Quat qx = wpe_quat_from_axis_angle(wpe_vec3(1.0f, 0.0f, 0.0f), radians.x);
    const WPE_Quat qy = wpe_quat_from_axis_angle(wpe_vec3(0.0f, 1.0f, 0.0f), radians.y);
    const WPE_Quat qz = wpe_quat_from_axis_angle(wpe_vec3(0.0f, 0.0f, 1.0f), radians.z);
    return wpe_quat_mul(qz, wpe_quat_mul(qy, qx));
}

WPE_Quat wpe_quat_mul(WPE_Quat a, WPE_Quat b) {
    WPE_Quat q;
    glm_quat_mul(a.v, b.v, q.v);
    return q;
}

WPE_Quat wpe_quat_normalize(WPE_Quat q) {
    const float len_sq = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    if (!(len_sq > 1e-20f) || !isfinite(len_sq)) return wpe_quat_identity();

    const float inv = 1.0f / sqrtf(len_sq);
    return (WPE_Quat){.x = q.x * inv, .y = q.y * inv, .z = q.z * inv, .w = q.w * inv};
}

WPE_Vec3 wpe_quat_rotate_vec3(WPE_Quat q, WPE_Vec3 v) {
    WPE_Vec3 out;
    glm_quat_rotatev(q.v, v.v, out.v);
    return out;
}

WPE_Quat wpe_quat_slerp(WPE_Quat a, WPE_Quat b, float t) {
    /* cglm's slerp does not pick the shortest arc; flip b into a's hemisphere first. */
    const float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (dot < 0.0f) b = (WPE_Quat){.x = -b.x, .y = -b.y, .z = -b.z, .w = -b.w};

    WPE_Quat q;
    glm_quat_slerp(a.v, b.v, t, q.v);
    return wpe_quat_normalize(q);
}

WPE_Mat4 wpe_mat4_mul(const WPE_Mat4* a, const WPE_Mat4* b) {
    wpe_assert(a && b);
    WPE_Mat4 out;
    glm_mat4_mul(WPE__CGLM_MAT4(a), WPE__CGLM_MAT4(b), out.m);
    return out;
}

WPE_Mat4 wpe_mat4_from_trs(WPE_Vec3 translation, WPE_Quat rotation, WPE_Vec3 scale) {
    const WPE_Quat r = wpe_quat_normalize(rotation);
    WPE_Mat4 out;
    glm_quat_mat4((float*)r.v, out.m);

    for (int row = 0; row < 3; ++row) {
        out.m[0][row] *= scale.x;
        out.m[1][row] *= scale.y;
        out.m[2][row] *= scale.z;
    }
    out.m[3][0] = translation.x;
    out.m[3][1] = translation.y;
    out.m[3][2] = translation.z;
    out.m[3][3] = 1.0f;
    return out;
}

bool wpe_mat4_inverse(const WPE_Mat4* m, WPE_Mat4* out) {
    wpe_assert(m && out);
    const float det = glm_mat4_det(WPE__CGLM_MAT4(m));
    if (!(fabsf(det) > 1e-12f)) return false;

    glm_mat4_inv(WPE__CGLM_MAT4(m), out->m);
    return true;
}

WPE_Vec3 wpe_mat4_transform_point(const WPE_Mat4* m, WPE_Vec3 p) {
    wpe_assert(m);
    WPE_Vec3 out;
    glm_mat4_mulv3(WPE__CGLM_MAT4(m), p.v, 1.0f, out.v);
    return out;
}

WPE_Vec3 wpe_mat4_transform_dir(const WPE_Mat4* m, WPE_Vec3 d) {
    wpe_assert(m);
    WPE_Vec3 out;
    glm_mat4_mulv3(WPE__CGLM_MAT4(m), d.v, 0.0f, out.v);
    return out;
}

bool wpe_mat4_decompose(const WPE_Mat4* m, WPE_Vec3* out_translation, WPE_Quat* out_rotation,
                        WPE_Vec3* out_scale) {
    wpe_assert(m && out_translation && out_rotation && out_scale);

    const WPE_Vec3 axes[3] = {
        wpe_vec3(m->m[0][0], m->m[0][1], m->m[0][2]),
        wpe_vec3(m->m[1][0], m->m[1][1], m->m[1][2]),
        wpe_vec3(m->m[2][0], m->m[2][1], m->m[2][2]),
    };
    WPE_Vec3 scale = wpe_vec3(wpe_vec3_length(axes[0]), wpe_vec3_length(axes[1]), wpe_vec3_length(axes[2]));
    if (!(scale.x > 1e-12f && scale.y > 1e-12f && scale.z > 1e-12f)) return false;

    /* A negative determinant means mirroring; fold it into the X scale so rotation stays proper. */
    if (wpe_vec3_dot(wpe_vec3_cross(axes[0], axes[1]), axes[2]) < 0.0f) scale.x = -scale.x;

    WPE_Mat4 rotation = wpe_mat4_identity();
    for (int row = 0; row < 3; ++row) {
        rotation.m[0][row] = axes[0].v[row] / scale.x;
        rotation.m[1][row] = axes[1].v[row] / scale.y;
        rotation.m[2][row] = axes[2].v[row] / scale.z;
    }

    WPE_Quat q;
    glm_mat4_quat(rotation.m, q.v);

    *out_translation = wpe_vec3(m->m[3][0], m->m[3][1], m->m[3][2]);
    *out_rotation = wpe_quat_normalize(q);
    *out_scale = scale;
    return true;
}
