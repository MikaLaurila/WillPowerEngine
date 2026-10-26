#include "graphics/material_shader.h"

#include <math.h>

#include "renderer/texture.h"

/* Varying layout. */
enum {
    WPE__V_UV0 = 0,       /* 2 */
    WPE__V_UV1 = 2,       /* 2 */
    WPE__V_COLOR = 4,     /* 4: vertex color */
    WPE__V_DIFFUSE = 8,   /* 3: diffuse light (ambient + directional) */
    WPE__V_SPECULAR = 11, /* 3: specular light */
    WPE__V_COUNT = 14,
};

float wpe_material_shininess(float glossiness) {
    const float g = glossiness < 0.0f ? 0.0f : (glossiness > 1.0f ? 1.0f : glossiness);
    return exp2f(1.0f + 10.0f * g);
}

/* ---- Vertex stage --------------------------------------------------------------------------------- */

static void wpe__material_vs(const void* uniforms, const void* vertex, WPE_Vec4* out_position, float* out) {
    const WPE_MaterialUniforms* u = uniforms;
    const WPE_MeshVertex* v = vertex;

    WPE_Vec3 position = v->position;
    WPE_Vec3 normal = v->normal;
    if (u->joint_palette) {
        WPE_Vec3 skinned_p = wpe_vec3(0, 0, 0), skinned_n = wpe_vec3(0, 0, 0);
        float total = 0.0f;
        for (int i = 0; i < 4; ++i) {
            const float w = v->weights[i];
            if (!(w > 0.0f) || v->joints[i] >= u->joint_count) continue;
            const WPE_Mat4* m = &u->joint_palette[v->joints[i]];
            skinned_p = wpe_vec3_add(skinned_p, wpe_vec3_scale(wpe_mat4_transform_point(m, v->position), w));
            skinned_n = wpe_vec3_add(skinned_n, wpe_vec3_scale(wpe_mat4_transform_dir(m, v->normal), w));
            total += w;
        }
        if (total > 0.0f) {
            position = wpe_vec3_scale(skinned_p, 1.0f / total);
            normal = skinned_n;
        }
    }

    *out_position = wpe_mat4_mul_vec4(&u->mvp, wpe_vec4(position.x, position.y, position.z, 1.0f));
    out[WPE__V_UV0 + 0] = v->uv0.x;
    out[WPE__V_UV0 + 1] = v->uv0.y;
    out[WPE__V_UV1 + 0] = v->uv1.x;
    out[WPE__V_UV1 + 1] = v->uv1.y;
    for (int i = 0; i < 4; ++i)
        out[WPE__V_COLOR + i] = v->color[i];

    if (u->material->workflow == WPE_MATERIAL_UNLIT) {
        for (int i = 0; i < 3; ++i) {
            out[WPE__V_DIFFUSE + i] = 1.0f;
            out[WPE__V_SPECULAR + i] = 0.0f;
        }
        return;
    }

    const WPE_Vec3 world_position = wpe_mat4_transform_point(&u->world, position);
    const WPE_Vec3 n = wpe_vec3_normalize(wpe_mat4_transform_dir(&u->normal_matrix, normal));
    const WPE_Vec3 l = wpe_vec3_normalize(wpe_vec3_scale(u->light.direction, -1.0f));
    float ndl = wpe_vec3_dot(n, l);
    if (u->material->double_sided) ndl = fabsf(ndl);
    const float diffuse = ndl > 0.0f ? ndl : 0.0f;

    float specular = 0.0f;
    if (diffuse > 0.0f) {
        const WPE_Vec3 view = wpe_vec3_normalize(wpe_vec3_sub(u->camera_position, world_position));
        const WPE_Vec3 h = wpe_vec3_normalize(wpe_vec3_add(l, view));
        float ndh = wpe_vec3_dot(n, h);
        if (u->material->double_sided) ndh = fabsf(ndh);
        if (ndh > 0.0f) specular = powf(ndh, u->shininess);
    }
    for (int i = 0; i < 3; ++i) {
        out[WPE__V_DIFFUSE + i] = u->light.ambient.v[i] + u->light.color.v[i] * diffuse;
        out[WPE__V_SPECULAR + i] = u->light.color.v[i] * specular;
    }
}

/* ---- Fragment stage ------------------------------------------------------------------------------- */

/* Samples a material texture reference (KHR_texture_transform applied), or returns `fallback`. */
static WPE_Vec4 wpe__sample_ref(const WPE_Model* model, const WPE_TextureRef* ref,
                                const WPE_FragmentInput* in, WPE_Vec4 fallback) {
    if (ref->texture == WPE_MODEL_NONE || ref->texture >= model->texture_count) return fallback;
    const WPE_Texture* texture = model->textures[ref->texture];
    if (!texture) return fallback;

    const float* uv_in = in->varyings + (ref->texcoord == 1 ? WPE__V_UV1 : WPE__V_UV0);
    WPE_Vec2 uv = {.x = uv_in[0] * ref->scale.x, .y = uv_in[1] * ref->scale.y};
    WPE_Vec2 dx = {.x = in->uv_dx.x * ref->scale.x, .y = in->uv_dx.y * ref->scale.y};
    WPE_Vec2 dy = {.x = in->uv_dy.x * ref->scale.x, .y = in->uv_dy.y * ref->scale.y};
    if (ref->rotation != 0.0f) {
        /* KHR_texture_transform: uv' = T * R * S * uv with R = [cos sin; -sin cos]. */
        const float c = cosf(ref->rotation), s = sinf(ref->rotation);
        uv = (WPE_Vec2){.x = c * uv.x + s * uv.y, .y = -s * uv.x + c * uv.y};
        dx = (WPE_Vec2){.x = c * dx.x + s * dx.y, .y = -s * dx.x + c * dx.y};
        dy = (WPE_Vec2){.x = c * dy.x + s * dy.y, .y = -s * dy.x + c * dy.y};
    }
    uv.x += ref->offset.x;
    uv.y += ref->offset.y;
    return wpe_texture_sample(texture, &ref->sampler, uv, dx, dy);
}

static bool wpe__material_fs(const void* uniforms, const WPE_FragmentInput* in, WPE_Vec4* out_color) {
    const WPE_MaterialUniforms* u = uniforms;
    const WPE_Material* m = u->material;
    const WPE_Model* model = u->model;
    const float* v = in->varyings;
    const WPE_Vec4 one = wpe_vec4(1, 1, 1, 1);

    const WPE_Vec4 tex = wpe__sample_ref(model, &m->diffuse_texture, in, one);
    WPE_Vec4 base = wpe_vec4(
        m->diffuse_factor.x * tex.x * v[WPE__V_COLOR + 0], m->diffuse_factor.y * tex.y * v[WPE__V_COLOR + 1],
        m->diffuse_factor.z * tex.z * v[WPE__V_COLOR + 2], m->diffuse_factor.w * tex.w * v[WPE__V_COLOR + 3]);

    switch (m->alpha_mode) {
    case WPE_ALPHA_MASK:
        if (!(base.w >= m->alpha_cutoff)) return false;
        base.w = 1.0f;
        break;
    case WPE_ALPHA_OPAQUE:
        base.w = 1.0f;
        break;
    case WPE_ALPHA_BLEND:
        break;
    }

    if (m->workflow == WPE_MATERIAL_UNLIT) {
        *out_color = base;
        return true;
    }

    WPE_Vec3 diffuse, specular;
    float gloss;
    if (m->workflow == WPE_MATERIAL_METALLIC_ROUGHNESS) {
        const WPE_Vec4 mr = wpe__sample_ref(model, &m->metallic_roughness_texture, in, one);
        const float metallic = m->metallic_factor * mr.z;
        const float roughness = m->roughness_factor * mr.y;
        diffuse = wpe_vec3_scale(wpe_vec3(base.x, base.y, base.z), 1.0f - metallic);
        specular = wpe_vec3(0.04f + (base.x - 0.04f) * metallic, 0.04f + (base.y - 0.04f) * metallic,
                            0.04f + (base.z - 0.04f) * metallic);
        gloss = 1.0f - roughness;
    } else {
        const WPE_Vec4 sg = wpe__sample_ref(model, &m->specular_glossiness_texture, in, one);
        diffuse = wpe_vec3(base.x, base.y, base.z);
        specular =
            wpe_vec3(m->specular_factor.x * sg.x, m->specular_factor.y * sg.y, m->specular_factor.z * sg.z);
        gloss = m->glossiness_factor * sg.w;
    }

    const float ao =
        1.0f + m->occlusion_strength * (wpe__sample_ref(model, &m->occlusion_texture, in, one).x - 1.0f);
    const WPE_Vec4 emissive_tex = wpe__sample_ref(model, &m->emissive_texture, in, one);

    /*
     * Ambient light is treated as a uniform environment: diffuse surfaces scatter it (included in
     * the diffuse varying) and specular surfaces reflect it, which keeps metals from going black
     * without environment maps.
     */
    WPE_Vec4 color = {.w = base.w};
    for (int i = 0; i < 3; ++i) {
        const float direct_specular = specular.v[i] * v[WPE__V_SPECULAR + i] * gloss;
        const float ambient_specular = specular.v[i] * u->light.ambient.v[i];
        color.v[i] = (diffuse.v[i] * v[WPE__V_DIFFUSE + i] + ambient_specular) * ao + direct_specular +
                     m->emissive_factor.v[i] * emissive_tex.v[i];
    }
    *out_color = color;
    return true;
}

static const WPE_ShaderProgram wpe__material_program = {
    .vertex = wpe__material_vs,
    .fragment = wpe__material_fs,
    .varying_count = WPE__V_COUNT,
    .uv_derivatives = true,
    .uv_varying = WPE__V_UV0,
};

const WPE_ShaderProgram* wpe_shader_material(void) {
    return &wpe__material_program;
}
