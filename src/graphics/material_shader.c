#include "graphics/material_shader.h"

#include <math.h>
#include <string.h>

#include "renderer/texture.h"

/* Varying layout. */
enum {
    WPE__V_UV0 = 0,       /* 2 */
    WPE__V_UV1 = 2,       /* 2 */
    WPE__V_COLOR = 4,     /* 4: vertex color */
    WPE__V_DIFFUSE = 8,   /* 3: ambient + point/spot diffuse light */
    WPE__V_SPECULAR = 11, /* 3: point/spot specular light */
    WPE__V_SUN_NDL = 14,  /* 1: per-vertex sun diffuse term (unshadowed) */
    WPE__V_SUN_SPEC = 15, /* 1: per-vertex sun specular term (unshadowed) */
    WPE__V_SHADOW = 16,   /* 3: shadow map (u, v, depth) */
    WPE__V_DISTANCE = 19, /* 1: view distance (fog) */
    WPE__V_SUN_TS = 20,   /* 3: sun direction in tangent space (normal-mapped only) */
    WPE__V_HALF_TS = 23,  /* 3: sun half vector in tangent space (normal-mapped only) */
    WPE__V_COUNT = 26,
};

static_assert(WPE__V_COUNT <= WPE_MAX_VARYINGS, "material varyings exceed the renderer limit");

static const char* const wpe__filtering_names[] = {"material", "nearest", "bilinear", "three_point"};
static_assert(sizeof wpe__filtering_names / sizeof wpe__filtering_names[0] ==
                  WPE_TEXTURE_FILTERING_THREE_POINT + 1,
              "texture filtering names out of sync with WPE_TextureFiltering");

const char* wpe_texture_filtering_name(WPE_TextureFiltering filtering) {
    return (uint32_t)filtering <= WPE_TEXTURE_FILTERING_THREE_POINT ? wpe__filtering_names[filtering] : NULL;
}

bool wpe_texture_filtering_from_name(const char* name, WPE_TextureFiltering* out_filtering) {
    for (uint32_t i = 0; name && i <= WPE_TEXTURE_FILTERING_THREE_POINT; ++i) {
        if (strcmp(wpe__filtering_names[i], name) == 0) {
            *out_filtering = (WPE_TextureFiltering)i;
            return true;
        }
    }
    return false;
}

float wpe_material_shininess(float glossiness) {
    const float g = glossiness < 0.0f ? 0.0f : (glossiness > 1.0f ? 1.0f : glossiness);
    return exp2f(1.0f + 10.0f * g);
}

float wpe_light_attenuation(float distance, float range) {
    if (!(range > 0.0f) || !(distance < range)) return 0.0f;
    const float ratio = distance / range;
    const float window = 1.0f - ratio * ratio * ratio * ratio;
    /* 1 / (1 + (d/r * 4)^2): inverse-square shaped but finite at d = 0 and scale-independent. */
    const float scaled = ratio * 4.0f;
    return window * window / (1.0f + scaled * scaled);
}

float wpe_shadow_visibility(const WPE_ShadowSampler* shadow, float u, float v, float depth) {
    if (!shadow || !shadow->depth || shadow->size == 0) return 1.0f;
    if (!(u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f && depth <= 1.0f)) return 1.0f;
    const int32_t size = (int32_t)shadow->size;
    const int32_t cx = (int32_t)(u * (float)size), cy = (int32_t)(v * (float)size);
    const float receiver = depth - shadow->bias;
    int lit = 0;
    for (int32_t dy = -1; dy <= 1; ++dy) {
        for (int32_t dx = -1; dx <= 1; ++dx) {
            int32_t x = cx + dx, y = cy + dy;
            x = x < 0 ? 0 : (x >= size ? size - 1 : x);
            y = y < 0 ? 0 : (y >= size ? size - 1 : y);
            lit += receiver <= shadow->depth[(size_t)y * shadow->size + (size_t)x];
        }
    }
    return (float)lit / 9.0f;
}

/* ---- Vertex stage --------------------------------------------------------------------------------- */

/* Skins position, normal and tangent with the palette; returns false if no joint applied. */
static bool wpe__skin(const WPE_MaterialUniforms* u, const WPE_MeshVertex* v, WPE_Vec3* position,
                      WPE_Vec3* normal, WPE_Vec3* tangent) {
    WPE_Vec3 p = wpe_vec3(0, 0, 0), n = wpe_vec3(0, 0, 0), t = wpe_vec3(0, 0, 0);
    const WPE_Vec3 tangent_in = wpe_vec3(v->tangent[0], v->tangent[1], v->tangent[2]);
    float total = 0.0f;
    for (int i = 0; i < 4; ++i) {
        const float w = v->weights[i];
        if (!(w > 0.0f) || v->joints[i] >= u->joint_count) continue;
        const WPE_Mat4* m = &u->joint_palette[v->joints[i]];
        p = wpe_vec3_add(p, wpe_vec3_scale(wpe_mat4_transform_point(m, v->position), w));
        n = wpe_vec3_add(n, wpe_vec3_scale(wpe_mat4_transform_dir(m, v->normal), w));
        t = wpe_vec3_add(t, wpe_vec3_scale(wpe_mat4_transform_dir(m, tangent_in), w));
        total += w;
    }
    if (!(total > 0.0f)) return false;
    *position = wpe_vec3_scale(p, 1.0f / total);
    *normal = n;
    *tangent = t;
    return true;
}

static bool wpe__normal_mapped(const WPE_MaterialUniforms* u) {
    return u->material->normal_texture.texture != WPE_MODEL_NONE && u->has_sun;
}

static void wpe__material_vs(const void* uniforms, const void* vertex, WPE_Vec4* out_position, float* out) {
    const WPE_MaterialUniforms* u = uniforms;
    const WPE_MeshVertex* v = vertex;

    WPE_Vec3 position = v->position, normal = v->normal;
    WPE_Vec3 tangent = wpe_vec3(v->tangent[0], v->tangent[1], v->tangent[2]);
    if (u->joint_palette) wpe__skin(u, v, &position, &normal, &tangent);

    *out_position = wpe_mat4_mul_vec4(&u->mvp, wpe_vec4(position.x, position.y, position.z, 1.0f));
    out[WPE__V_UV0 + 0] = v->uv0.x;
    out[WPE__V_UV0 + 1] = v->uv0.y;
    out[WPE__V_UV1 + 0] = v->uv1.x;
    out[WPE__V_UV1 + 1] = v->uv1.y;
    for (int i = 0; i < 4; ++i)
        out[WPE__V_COLOR + i] = v->color[i];
    for (int i = WPE__V_DIFFUSE; i < WPE__V_COUNT; ++i)
        out[i] = 0.0f;

    const WPE_Vec3 world_position = wpe_mat4_transform_point(&u->world, position);
    const WPE_Vec3 to_camera = wpe_vec3_sub(u->camera_position, world_position);
    out[WPE__V_DISTANCE] = wpe_vec3_length(to_camera);

    if (u->sun_shadow) {
        const WPE_Vec3 s = wpe_mat4_transform_point(&u->shadow_matrix, world_position);
        out[WPE__V_SHADOW + 0] = s.x;
        out[WPE__V_SHADOW + 1] = s.y;
        out[WPE__V_SHADOW + 2] = s.z;
    }

    if (u->material->workflow == WPE_MATERIAL_UNLIT || u->unlit) {
        for (int i = 0; i < 3; ++i)
            out[WPE__V_DIFFUSE + i] = 1.0f;
        return;
    }

    const bool two_sided = u->material->double_sided || u->double_sided;
    const WPE_Vec3 n = wpe_vec3_normalize(wpe_mat4_transform_dir(&u->normal_matrix, normal));
    const WPE_Vec3 view = wpe_vec3_normalize(to_camera);
    WPE_Vec3 diffuse = u->ambient, specular = wpe_vec3(0, 0, 0);

    for (uint32_t l = 0; l < u->light_count; ++l) {
        const WPE_ShaderLight* light = &u->lights[l];
        const WPE_Vec3 to_light = wpe_vec3_sub(light->position, world_position);
        const float distance = wpe_vec3_length(to_light);
        float attenuation = wpe_light_attenuation(distance, light->range);
        if (!(attenuation > 0.0f) || !(distance > 1e-6f)) continue;
        const WPE_Vec3 l_dir = wpe_vec3_scale(to_light, 1.0f / distance);
        if (light->spot) {
            const float cos_angle = -wpe_vec3_dot(l_dir, light->direction);
            const float span = light->cos_inner - light->cos_outer;
            float cone = span > 1e-6f ? (cos_angle - light->cos_outer) / span
                                      : (cos_angle >= light->cos_outer ? 1.0f : 0.0f);
            cone = cone < 0.0f ? 0.0f : (cone > 1.0f ? 1.0f : cone);
            attenuation *= cone * cone * (3.0f - 2.0f * cone); /* smoothstep */
            if (!(attenuation > 0.0f)) continue;
        }
        float ndl = wpe_vec3_dot(n, l_dir);
        if (two_sided) ndl = fabsf(ndl);
        if (!(ndl > 0.0f)) continue;
        const WPE_Vec3 h = wpe_vec3_normalize(wpe_vec3_add(l_dir, view));
        float ndh = wpe_vec3_dot(n, h);
        if (two_sided) ndh = fabsf(ndh);
        const float spec = ndh > 0.0f ? powf(ndh, u->shininess) : 0.0f;
        diffuse = wpe_vec3_add(diffuse, wpe_vec3_scale(light->color, ndl * attenuation));
        specular = wpe_vec3_add(specular, wpe_vec3_scale(light->color, spec * attenuation));
    }
    for (int i = 0; i < 3; ++i) {
        out[WPE__V_DIFFUSE + i] = diffuse.v[i];
        out[WPE__V_SPECULAR + i] = specular.v[i];
    }

    if (!u->has_sun) return;
    const WPE_Vec3 l = wpe_vec3_scale(u->sun_direction, -1.0f);
    const WPE_Vec3 h = wpe_vec3_normalize(wpe_vec3_add(l, view));
    float ndl = wpe_vec3_dot(n, l), ndh = wpe_vec3_dot(n, h);
    if (two_sided) {
        ndl = fabsf(ndl);
        ndh = fabsf(ndh);
    }
    out[WPE__V_SUN_NDL] = ndl > 0.0f ? ndl : 0.0f;
    out[WPE__V_SUN_SPEC] = (ndl > 0.0f && ndh > 0.0f) ? powf(ndh, u->shininess) : 0.0f;

    /* Tangent-space sun and half vectors for per-pixel normal mapping (needs a valid tangent). */
    if (wpe__normal_mapped(u) && v->tangent[3] != 0.0f) {
        WPE_Vec3 t = wpe_mat4_transform_dir(&u->world, tangent);
        t = wpe_vec3_normalize(wpe_vec3_sub(t, wpe_vec3_scale(n, wpe_vec3_dot(n, t)))); /* Gram-Schmidt */
        const WPE_Vec3 b = wpe_vec3_scale(wpe_vec3_cross(n, t), v->tangent[3] < 0.0f ? -1.0f : 1.0f);
        out[WPE__V_SUN_TS + 0] = wpe_vec3_dot(l, t);
        out[WPE__V_SUN_TS + 1] = wpe_vec3_dot(l, b);
        out[WPE__V_SUN_TS + 2] = wpe_vec3_dot(l, n);
        out[WPE__V_HALF_TS + 0] = wpe_vec3_dot(h, t);
        out[WPE__V_HALF_TS + 1] = wpe_vec3_dot(h, b);
        out[WPE__V_HALF_TS + 2] = wpe_vec3_dot(h, n);
    }
}

/* Subdivision support: interpolates every attribute; skin weights are merged from both endpoints. */
static void wpe__material_lerp(const void* a_ptr, const void* b_ptr, float t, void* out_ptr) {
    const WPE_MeshVertex* a = a_ptr;
    const WPE_MeshVertex* b = b_ptr;
    WPE_MeshVertex* out = out_ptr;
    out->position = wpe_vec3_lerp(a->position, b->position, t);
    out->normal = wpe_vec3_lerp(a->normal, b->normal, t);
    out->uv0 =
        (WPE_Vec2){.x = a->uv0.x + (b->uv0.x - a->uv0.x) * t, .y = a->uv0.y + (b->uv0.y - a->uv0.y) * t};
    out->uv1 =
        (WPE_Vec2){.x = a->uv1.x + (b->uv1.x - a->uv1.x) * t, .y = a->uv1.y + (b->uv1.y - a->uv1.y) * t};
    for (int i = 0; i < 4; ++i) {
        out->tangent[i] = a->tangent[i] + (b->tangent[i] - a->tangent[i]) * t;
        out->color[i] = a->color[i] + (b->color[i] - a->color[i]) * t;
    }
    out->tangent[3] = a->tangent[3]; /* Handedness is not interpolated. */

    /* Merge the weighted joint sets of both endpoints and keep the four strongest influences. */
    uint16_t joints[8];
    float weights[8];
    int count = 0;
    for (int e = 0; e < 2; ++e) {
        const WPE_MeshVertex* src = e == 0 ? a : b;
        const float scale = e == 0 ? 1.0f - t : t;
        for (int i = 0; i < 4; ++i) {
            if (!(src->weights[i] > 0.0f)) continue;
            int k = 0;
            while (k < count && joints[k] != src->joints[i])
                ++k;
            if (k == count) {
                joints[count] = src->joints[i];
                weights[count++] = 0.0f;
            }
            weights[k] += src->weights[i] * scale;
        }
    }
    for (int i = 0; i < 4; ++i) {
        int best = -1;
        for (int k = 0; k < count; ++k) {
            if (weights[k] > 0.0f && (best < 0 || weights[k] > weights[best])) best = k;
        }
        out->joints[i] = best >= 0 ? joints[best] : 0;
        out->weights[i] = best >= 0 ? weights[best] : 0.0f;
        if (best >= 0) weights[best] = 0.0f;
    }
    const float sum = out->weights[0] + out->weights[1] + out->weights[2] + out->weights[3];
    if (sum > 0.0f) {
        for (int i = 0; i < 4; ++i)
            out->weights[i] /= sum;
    }
}

/* ---- Fragment stage ------------------------------------------------------------------------------- */

/* Samples a material texture reference (KHR_texture_transform applied, the filter override of `u` too), or
 * returns `fallback`. */
static WPE_Vec4 wpe__sample_ref(const WPE_MaterialUniforms* u, const WPE_TextureRef* ref,
                                const WPE_FragmentInput* in, WPE_Vec4 fallback) {
    const WPE_Model* model = u->model;
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
    if (u->texture_filtering == WPE_TEXTURE_FILTERING_MATERIAL)
        return wpe_texture_sample(texture, &ref->sampler, uv, dx, dy);
    static const WPE_TextureFilter filters[] = {
        [WPE_TEXTURE_FILTERING_NEAREST] = WPE_FILTER_NEAREST,
        [WPE_TEXTURE_FILTERING_BILINEAR] = WPE_FILTER_BILINEAR,
        [WPE_TEXTURE_FILTERING_THREE_POINT] = WPE_FILTER_THREE_POINT,
    };
    WPE_Sampler sampler = ref->sampler;
    sampler.mag_filter = sampler.min_filter = filters[u->texture_filtering];
    /* Nearest stays crisp when minified too: one mip level, not a blend of two (glTF's default is trilinear).
     */
    if (u->texture_filtering == WPE_TEXTURE_FILTERING_NEAREST && sampler.mip_filter == WPE_MIP_LINEAR)
        sampler.mip_filter = WPE_MIP_NEAREST;
    return wpe_texture_sample(texture, &sampler, uv, dx, dy);
}

/* Applies the alpha mode; returns false to discard. */
static bool wpe__resolve_alpha(const WPE_Material* m, float* alpha) {
    switch (m->alpha_mode) {
    case WPE_ALPHA_MASK:
        if (!(*alpha >= m->alpha_cutoff)) return false;
        *alpha = 1.0f;
        return true;
    case WPE_ALPHA_OPAQUE:
        *alpha = 1.0f;
        return true;
    case WPE_ALPHA_BLEND:
    default:
        return true;
    }
}

float wpe_fog_factor(const WPE_FogParams* fog, float distance) {
    switch (fog->mode) {
    case WPE_FOG_LINEAR: {
        const float span = fog->end - fog->start;
        const float f = span > 0.0f ? (distance - fog->start) / span : (distance >= fog->end ? 1.0f : 0.0f);
        return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
    }
    case WPE_FOG_EXPONENTIAL:
        return 1.0f - expf(-fog->density * distance);
    case WPE_FOG_NONE:
    default:
        return 0.0f;
    }
}

static bool wpe__material_fs(const void* uniforms, const WPE_FragmentInput* in, WPE_Vec4* out_color) {
    const WPE_MaterialUniforms* u = uniforms;
    const WPE_Material* m = u->material;
    const float* v = in->varyings;
    const WPE_Vec4 one = wpe_vec4(1, 1, 1, 1);

    const WPE_Vec4 tex = wpe__sample_ref(u, &m->diffuse_texture, in, one);
    WPE_Vec4 base = wpe_vec4(
        m->diffuse_factor.x * tex.x * v[WPE__V_COLOR + 0], m->diffuse_factor.y * tex.y * v[WPE__V_COLOR + 1],
        m->diffuse_factor.z * tex.z * v[WPE__V_COLOR + 2], m->diffuse_factor.w * tex.w * v[WPE__V_COLOR + 3]);
    if (!wpe__resolve_alpha(m, &base.w)) return false;

    WPE_Vec3 color;
    if (m->workflow == WPE_MATERIAL_UNLIT || u->unlit) {
        color = wpe_vec3(base.x, base.y, base.z);
        /* A mesh made unlit still takes the sun's shadow: shadowed parts fall to the ambient light, as lit
         * surfaces do. */
        if (u->unlit && u->sun_shadow) {
            const float shadow = wpe_shadow_visibility(u->sun_shadow, v[WPE__V_SHADOW], v[WPE__V_SHADOW + 1],
                                                       v[WPE__V_SHADOW + 2]);
            for (int i = 0; i < 3; ++i) {
                const float floor_light = fminf(fmaxf(u->ambient.v[i], 0.0f), 1.0f);
                color.v[i] *= floor_light + (1.0f - floor_light) * shadow;
            }
        }
    } else {
        WPE_Vec3 diffuse, specular;
        float gloss;
        if (m->workflow == WPE_MATERIAL_METALLIC_ROUGHNESS) {
            const WPE_Vec4 mr = wpe__sample_ref(u, &m->metallic_roughness_texture, in, one);
            const float metallic = m->metallic_factor * mr.z;
            const float roughness = m->roughness_factor * mr.y;
            diffuse = wpe_vec3_scale(wpe_vec3(base.x, base.y, base.z), 1.0f - metallic);
            specular = wpe_vec3(0.04f + (base.x - 0.04f) * metallic, 0.04f + (base.y - 0.04f) * metallic,
                                0.04f + (base.z - 0.04f) * metallic);
            gloss = 1.0f - roughness;
        } else {
            const WPE_Vec4 sg = wpe__sample_ref(u, &m->specular_glossiness_texture, in, one);
            diffuse = wpe_vec3(base.x, base.y, base.z);
            specular = wpe_vec3(m->specular_factor.x * sg.x, m->specular_factor.y * sg.y,
                                m->specular_factor.z * sg.z);
            gloss = m->glossiness_factor * sg.w;
        }

        /* Sun: per-vertex terms, replaced by per-pixel normal-mapped terms when available. */
        float sun_ndl = v[WPE__V_SUN_NDL], sun_spec = v[WPE__V_SUN_SPEC];
        const WPE_Vec3 sun_ts = wpe_vec3(v[WPE__V_SUN_TS], v[WPE__V_SUN_TS + 1], v[WPE__V_SUN_TS + 2]);
        if (wpe__normal_mapped(u) && wpe_vec3_length_sq(sun_ts) > 1e-8f) {
            const WPE_Vec4 nt = wpe__sample_ref(u, &m->normal_texture, in, wpe_vec4(0.5f, 0.5f, 1.0f, 1.0f));
            const WPE_Vec3 nm =
                wpe_vec3_normalize(wpe_vec3((nt.x * 2.0f - 1.0f) * m->normal_scale,
                                            (nt.y * 2.0f - 1.0f) * m->normal_scale, nt.z * 2.0f - 1.0f));
            const WPE_Vec3 l = wpe_vec3_normalize(sun_ts);
            const WPE_Vec3 h =
                wpe_vec3_normalize(wpe_vec3(v[WPE__V_HALF_TS], v[WPE__V_HALF_TS + 1], v[WPE__V_HALF_TS + 2]));
            float ndl = wpe_vec3_dot(nm, l), ndh = wpe_vec3_dot(nm, h);
            if ((m->double_sided || u->double_sided) && !in->front_facing) {
                ndl = -ndl;
                ndh = -ndh;
            }
            sun_ndl = ndl > 0.0f ? ndl : 0.0f;
            sun_spec = (ndl > 0.0f && ndh > 0.0f) ? powf(ndh, u->shininess) : 0.0f;
        }
        const float shadow = u->sun_shadow ? wpe_shadow_visibility(u->sun_shadow, v[WPE__V_SHADOW],
                                                                   v[WPE__V_SHADOW + 1], v[WPE__V_SHADOW + 2])
                                           : 1.0f;

        const float ao =
            1.0f + m->occlusion_strength * (wpe__sample_ref(u, &m->occlusion_texture, in, one).x - 1.0f);
        for (int i = 0; i < 3; ++i) {
            const float diffuse_light = v[WPE__V_DIFFUSE + i] + u->sun_color.v[i] * sun_ndl * shadow;
            const float specular_light = v[WPE__V_SPECULAR + i] + u->sun_color.v[i] * sun_spec * shadow;
            /* Ambient as a uniform environment: scattered diffusely and reflected specularly. */
            const float ambient_specular = specular.v[i] * u->ambient.v[i];
            color.v[i] = (diffuse.v[i] * diffuse_light + ambient_specular) * ao +
                         specular.v[i] * specular_light * gloss;
        }
    }

    const WPE_Vec4 emissive_tex = wpe__sample_ref(u, &m->emissive_texture, in, one);
    for (int i = 0; i < 3; ++i)
        color.v[i] += m->emissive_factor.v[i] * emissive_tex.v[i];

    const float fog = wpe_fog_factor(&u->fog, v[WPE__V_DISTANCE]);
    if (fog > 0.0f) color = wpe_vec3_lerp(color, u->fog.color, fog);

    *out_color = wpe_vec4(color.x, color.y, color.z, base.w);
    return true;
}

/* ---- Depth-only (shadow casters) ------------------------------------------------------------------ */

static void wpe__depth_vs(const void* uniforms, const void* vertex, WPE_Vec4* out_position, float* out) {
    const WPE_MaterialUniforms* u = uniforms;
    const WPE_MeshVertex* v = vertex;
    WPE_Vec3 position = v->position, normal = v->normal, tangent = wpe_vec3(0, 0, 0);
    if (u->joint_palette) wpe__skin(u, v, &position, &normal, &tangent);
    *out_position = wpe_mat4_mul_vec4(&u->mvp, wpe_vec4(position.x, position.y, position.z, 1.0f));
    out[WPE__V_UV0 + 0] = v->uv0.x;
    out[WPE__V_UV0 + 1] = v->uv0.y;
    out[WPE__V_UV1 + 0] = v->uv1.x;
    out[WPE__V_UV1 + 1] = v->uv1.y;
    for (int i = 0; i < 4; ++i)
        out[WPE__V_COLOR + i] = v->color[i];
}

/* Alpha-tested casters: discard where the material would be cut out. */
static bool wpe__depth_mask_fs(const void* uniforms, const WPE_FragmentInput* in, WPE_Vec4* out_color) {
    const WPE_MaterialUniforms* u = uniforms;
    const WPE_Material* m = u->material;
    const WPE_Vec4 tex = wpe__sample_ref(u, &m->diffuse_texture, in, wpe_vec4(1, 1, 1, 1));
    float alpha = m->diffuse_factor.w * tex.w * in->varyings[WPE__V_COLOR + 3];
    *out_color = wpe_vec4(0, 0, 0, 1);
    return wpe__resolve_alpha(m, &alpha);
}

static const WPE_ShaderProgram wpe__material_program = {
    .vertex = wpe__material_vs,
    .fragment = wpe__material_fs,
    .varying_count = WPE__V_COUNT,
    .uv_derivatives = true,
    .uv_varying = WPE__V_UV0,
    .vertex_lerp = wpe__material_lerp,
};

static const WPE_ShaderProgram wpe__depth_program = {
    .vertex = wpe__depth_vs,
    .fragment = NULL, /* Opaque casters only write depth. */
    .varying_count = WPE__V_COLOR + 4,
    .vertex_lerp = wpe__material_lerp,
};

static const WPE_ShaderProgram wpe__depth_masked_program = {
    .vertex = wpe__depth_vs,
    .fragment = wpe__depth_mask_fs,
    .varying_count = WPE__V_COLOR + 4,
    .uv_derivatives = true,
    .uv_varying = WPE__V_UV0,
    .vertex_lerp = wpe__material_lerp,
};

const WPE_ShaderProgram* wpe_shader_material(void) {
    return &wpe__material_program;
}

const WPE_ShaderProgram* wpe_shader_material_depth(bool alpha_masked) {
    return alpha_masked ? &wpe__depth_masked_program : &wpe__depth_program;
}
