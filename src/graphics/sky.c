/*
 * The sky: one triangle covering the screen at the far plane. Its corners carry the view ray through them
 * (rays are linear in screen position for any camera, so interpolating them is exact), and each pixel
 * colors its ray: a gradient by height or an equirectangular panorama, with the sun's disc on top.
 */
#include <math.h>

#include "graphics/sky_internal.h"

WPE_SkySettings wpe_sky_settings_default(void) {
    return (WPE_SkySettings){
        .mode = WPE_SKY_COLOR,
        .zenith = wpe_vec3(0.24f, 0.43f, 0.78f),
        .horizon = wpe_vec3(0.72f, 0.82f, 0.92f),
        .ground = wpe_vec3(0.32f, 0.31f, 0.3f),
        .curve = 0.5f,
        .sun_color = wpe_vec3(1.0f, 0.95f, 0.8f),
    };
}

typedef struct wpe__SkyUniforms {
    WPE_Mat4 inverse_view_projection;
    WPE_SkyMode mode;
    WPE_Vec3 zenith, horizon, ground;
    float curve;
    const WPE_Texture* panorama;
    float rotation; /* Fraction of a turn. */
    bool sun;
    WPE_Vec3 to_sun;
    float cos_edge, cos_core; /* The disc fades from its edge to 80 % of its radius. */
    WPE_Vec3 sun_color;
    float glow;
} wpe__SkyUniforms;

typedef struct wpe__SkyVertex {
    float x, y; /* Normalized device coordinates. */
} wpe__SkyVertex;

static const wpe__SkyVertex wpe__sky_vertices[3] = {{-1.0f, -1.0f}, {3.0f, -1.0f}, {-1.0f, 3.0f}};
static const uint32_t wpe__sky_indices[3] = {0, 1, 2};

static WPE_Vec3 wpe__unproject(const WPE_Mat4* inverse, float x, float y, float z) {
    const WPE_Vec4 p = wpe_mat4_mul_vec4(inverse, wpe_vec4(x, y, z, 1.0f));
    return wpe_vec3_scale(wpe_vec3(p.x, p.y, p.z), 1.0f / p.w);
}

/* Varyings: the view ray (unnormalized). */
static void wpe__sky_vs(const void* uniforms, const void* vertex, WPE_Vec4* out_position, float* out) {
    const wpe__SkyUniforms* u = uniforms;
    const wpe__SkyVertex* v = vertex;
    /* Just inside the far plane: exactly on it, interpolated depths rounding a hair past the cleared 1.0
     * would fail the depth test and leave holes. */
    *out_position = wpe_vec4(v->x, v->y, 0.99999f, 1.0f);
    const WPE_Vec3 ray = wpe_vec3_sub(wpe__unproject(&u->inverse_view_projection, v->x, v->y, 1.0f),
                                      wpe__unproject(&u->inverse_view_projection, v->x, v->y, -1.0f));
    out[0] = ray.x;
    out[1] = ray.y;
    out[2] = ray.z;
}

static float wpe__smoothstep(float edge0, float edge1, float x) {
    const float t = edge1 != edge0 ? (x - edge0) / (edge1 - edge0) : (x >= edge1 ? 1.0f : 0.0f);
    const float c = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    return c * c * (3.0f - 2.0f * c);
}

static const WPE_Sampler wpe__panorama_sampler = {.mag_filter = WPE_FILTER_NEAREST,
                                                  .min_filter = WPE_FILTER_NEAREST,
                                                  .wrap_u = WPE_WRAP_REPEAT,
                                                  .wrap_v = WPE_WRAP_CLAMP};

static bool wpe__sky_fs(const void* uniforms, const WPE_FragmentInput* in, WPE_Vec4* out_color) {
    const wpe__SkyUniforms* u = uniforms;
    const WPE_Vec3 d = wpe_vec3_normalize(wpe_vec3(in->varyings[0], in->varyings[1], in->varyings[2]));
    WPE_Vec3 color;
    if (u->mode == WPE_SKY_PANORAMA) {
        /* Equirectangular: longitude across (the image's center looks down -Z), latitude down. */
        const float up = d.y < -1.0f ? -1.0f : d.y > 1.0f ? 1.0f : d.y;
        const WPE_Vec2 uv = {.x = atan2f(d.x, -d.z) / (2.0f * WPE_PI) + 0.5f + u->rotation,
                             .y = acosf(up) / WPE_PI};
        const WPE_Vec4 t = wpe_texture_sample_lod(u->panorama, &wpe__panorama_sampler, uv, 0.0f);
        color = wpe_vec3(t.x, t.y, t.z);
    } else {
        const float height = d.y;
        const float amount = powf(fabsf(height), u->curve);
        color = wpe_vec3_lerp(u->horizon, height >= 0.0f ? u->zenith : u->ground, amount);
    }
    if (u->sun) {
        const float facing = wpe_vec3_dot(d, u->to_sun);
        if (u->glow > 0.0f && facing > 0.0f) {
            const float glow = u->glow * powf(facing, 48.0f);
            color = wpe_vec3_add(color, wpe_vec3_scale(u->sun_color, glow));
        }
        color = wpe_vec3_lerp(color, u->sun_color, wpe__smoothstep(u->cos_edge, u->cos_core, facing));
    }
    *out_color = wpe_vec4(color.x, color.y, color.z, 1.0f);
    return true;
}

static const WPE_ShaderProgram wpe__sky_program = {
    .vertex = wpe__sky_vs, .fragment = wpe__sky_fs, .varying_count = 3};

WPE_Result wpe__render_sky(WPE_Renderer* renderer, const WPE_SkySettings* sky,
                           const WPE_Mat4* view_projection, bool has_sun, WPE_Vec3 sun_direction) {
    if (sky->mode == WPE_SKY_COLOR || (sky->mode == WPE_SKY_PANORAMA && !sky->panorama)) return WPE_RESULT_OK;
    wpe__SkyUniforms u = {
        .mode = sky->mode,
        .zenith = sky->zenith,
        .horizon = sky->horizon,
        .ground = sky->ground,
        .curve = sky->curve > 0.0f ? sky->curve : 1.0f,
        .panorama = sky->panorama,
        .rotation = sky->rotation / 360.0f,
        .sun = has_sun && sky->sun_size > 0.0f,
        .to_sun = wpe_vec3_scale(wpe_vec3_normalize(sun_direction), -1.0f),
        .sun_color = sky->sun_color,
        .glow = sky->sun_glow,
    };
    if (!wpe_mat4_inverse(view_projection, &u.inverse_view_projection))
        return WPE_RESULT_OK; /* Degenerate. */
    const float radius = sky->sun_size * (WPE_PI / 180.0f);
    u.cos_edge = cosf(radius);
    u.cos_core = cosf(radius * 0.8f);
    /* Behind every opaque draw (and before the transparent ones, which have the top bit), so pixels geometry
     * already covered fail the depth test before shading. */
    WPE_RenderState state = wpe_render_state_default();
    state.cull = WPE_CULL_NONE;
    state.depth_func = WPE_DEPTH_LEQUAL;
    state.depth_write = false;
    return wpe_renderer_submit(renderer, &(WPE_DrawDesc){.program = &wpe__sky_program,
                                                         .uniforms = &u,
                                                         .uniforms_size = sizeof u,
                                                         .vertices = wpe__sky_vertices,
                                                         .vertex_stride = sizeof *wpe__sky_vertices,
                                                         .vertex_count = 3,
                                                         .indices = wpe__sky_indices,
                                                         .index_count = 3,
                                                         .state = state,
                                                         .sort_key = (UINT64_C(1) << 63) - 1});
}
