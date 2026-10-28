#define WPE_LOG_MODULE "graphics"
#include <math.h>

#include "graphics/billboard_internal.h"

/* ---- Component -------------------------------------------------------------------------------------- */

WPE_Billboard wpe_billboard_default(void) {
    return (WPE_Billboard){.size = {.x = 1.0f, .y = 1.0f},
                           .pivot = {.x = 0.5f, .y = 0.5f},
                           .color = wpe_vec4(1, 1, 1, 1),
                           .columns = 1,
                           .rows = 1,
                           .loop = true,
                           .mode = WPE_BILLBOARD_SCREEN_ALIGNED,
                           .visible = true};
}

static WPE_Vec3 wpe__column(const WPE_Mat4* m, int c) {
    return wpe_vec3(m->m[c][0], m->m[c][1], m->m[c][2]);
}

/* `v` normalized, or `fallback` when it is (nearly) zero. */
static WPE_Vec3 wpe__direction_or(WPE_Vec3 v, WPE_Vec3 fallback) {
    const float length = wpe_vec3_length(v);
    return length > 1e-6f ? wpe_vec3_scale(v, 1.0f / length) : fallback;
}

bool wpe_billboard_corners(const WPE_Billboard* b, const WPE_Mat4* world, const WPE_Mat4* camera_world,
                           WPE_Vec3 out_corners[4], WPE_Vec3* out_normal) {
    const WPE_Vec3 center = wpe__column(world, 3);
    const float width = b->size.x * wpe_vec3_length(wpe__column(world, 0));
    const float height = b->size.y * wpe_vec3_length(wpe__column(world, 1));
    if (!(width > 0.0f) || !(height > 0.0f)) return false;
    const WPE_Vec3 camera_right = wpe__direction_or(wpe__column(camera_world, 0), wpe_vec3(1, 0, 0));
    const WPE_Vec3 camera_up = wpe__direction_or(wpe__column(camera_world, 1), wpe_vec3(0, 1, 0));
    const WPE_Vec3 to_camera = wpe_vec3_sub(wpe__column(camera_world, 3), center);
    WPE_Vec3 right, up;
    switch (b->mode) {
    case WPE_BILLBOARD_FIXED:
        right = wpe__direction_or(wpe__column(world, 0), wpe_vec3(1, 0, 0));
        up = wpe__direction_or(wpe__column(world, 1), wpe_vec3(0, 1, 0));
        break;
    case WPE_BILLBOARD_AXIS_ALIGNED: {
        /* Faces the camera's position from any side, above included, and stays upright: its up is the
         * world's up as far as facing the camera allows (no tilt, no roll with the camera). Straight above
         * or below, the camera's right decides the turn. */
        const WPE_Vec3 facing =
            wpe__direction_or(to_camera, wpe__direction_or(wpe__column(camera_world, 2), wpe_vec3(0, 0, 1)));
        right = wpe__direction_or(wpe_vec3_cross(wpe_vec3(0, 1, 0), facing), camera_right);
        up = wpe__direction_or(wpe_vec3_cross(facing, right), camera_up);
        right = wpe_vec3_cross(up, facing); /* Square again: the fallback right needn't be perpendicular. */
        break;
    }
    case WPE_BILLBOARD_AXIS_ALIGNED_Y:
        /* Upright along world Y, turned about it so the quad faces the camera as much as the axis allows. A
         * camera right on the axis leaves no turn to make: the camera's right is used. */
        up = wpe_vec3(0, 1, 0);
        right = wpe__direction_or(wpe_vec3_cross(up, to_camera), camera_right);
        break;
    case WPE_BILLBOARD_SCREEN_ALIGNED:
    default:
        right = camera_right;
        up = camera_up;
        break;
    }
    WPE_Vec3 normal = wpe__direction_or(wpe_vec3_cross(right, up), wpe_vec3(0, 0, 1));
    if (b->rotation != 0.0f) {
        const float c = cosf(b->rotation), s = sinf(b->rotation);
        const WPE_Vec3 r = right;
        right = wpe_vec3_add(wpe_vec3_scale(r, c), wpe_vec3_scale(up, s));
        up = wpe_vec3_sub(wpe_vec3_scale(up, c), wpe_vec3_scale(r, s));
    }
    const float x0 = -b->pivot.x * width, x1 = (1.0f - b->pivot.x) * width;
    const float y0 = -b->pivot.y * height, y1 = (1.0f - b->pivot.y) * height;
    const float xs[4] = {x0, x1, x1, x0}, ys[4] = {y0, y0, y1, y1};
    for (int i = 0; i < 4; ++i)
        out_corners[i] =
            wpe_vec3_add(center, wpe_vec3_add(wpe_vec3_scale(right, xs[i]), wpe_vec3_scale(up, ys[i])));
    if (out_normal) *out_normal = normal;
    return true;
}

static uint32_t wpe__frame_count(const WPE_Billboard* b) {
    const uint32_t grid = (b->columns ? b->columns : 1) * (b->rows ? b->rows : 1);
    return b->frame_count && b->frame_count < grid ? b->frame_count : grid;
}

void wpe_billboard_update(WPE_Ecs* ecs, float dt) {
    if (!ecs || !(dt > 0.0f)) return;
    WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_BILLBOARD)});
    while (wpe_ecs_iter_next(&it)) {
        WPE_Billboard* b = wpe_ecs_iter_get(&it, WPE_COMP_BILLBOARD);
        if (!(b->fps > 0.0f)) continue;
        const uint32_t count = wpe__frame_count(b);
        const float step = 1.0f / b->fps;
        b->frame_time += dt;
        const float steps = floorf(b->frame_time / step);
        if (!(steps >= 1.0f)) continue;
        b->frame_time -= steps * step;
        if (b->loop) {
            b->frame = (uint32_t)fmodf((float)b->frame + steps, (float)count);
        } else {
            const float last = (float)(count - 1);
            b->frame = (float)b->frame + steps >= last ? count - 1 : b->frame + (uint32_t)steps;
        }
    }
}

/* ---- Shader ----------------------------------------------------------------------------------------- */

/* Varying layout. */
enum {
    WPE__BV_UV = 0,       /* 2 */
    WPE__BV_COLOR = 2,    /* 4 */
    WPE__BV_DISTANCE = 6, /* 1: view distance (fog) */
    WPE__BV_LIGHT = 7,    /* 3: ambient and point/spot lights (1 when unlit) */
    WPE__BV_SUN = 10,     /* 1: the sun's wrapped N.L */
    WPE__BV_SHADOW = 11,  /* 3: shadow-map coordinates */
    WPE__BV_COUNT = 14,
};

static const WPE_Sampler wpe__billboard_sampler = {.mag_filter = WPE_FILTER_NEAREST,
                                                   .min_filter = WPE_FILTER_NEAREST,
                                                   .wrap_u = WPE_WRAP_CLAMP,
                                                   .wrap_v = WPE_WRAP_CLAMP};

/* Sprites are lit softly ("half Lambert"): a quad turned away from a light is dimmer, not black. */
static float wpe__wrap(float ndl) {
    return ndl * 0.5f + 0.5f;
}

static void wpe__billboard_vs(const void* uniforms, const void* vertex, WPE_Vec4* out_position, float* out) {
    const wpe__BillboardUniforms* u = uniforms;
    const wpe__BillboardVertex* v = vertex;
    const WPE_Vec3 p = v->position;
    *out_position = wpe_mat4_mul_vec4(&u->view_projection, wpe_vec4(p.x, p.y, p.z, 1.0f));
    out[WPE__BV_UV + 0] = v->u;
    out[WPE__BV_UV + 1] = v->v;
    for (int i = 0; i < 4; ++i)
        out[WPE__BV_COLOR + i] = v->color[i];
    const WPE_Vec3 to_camera = wpe_vec3_sub(u->camera_position, p);
    out[WPE__BV_DISTANCE] = wpe_vec3_length(to_camera);
    for (int i = 0; i < 3; ++i)
        out[WPE__BV_LIGHT + i] = 1.0f;
    out[WPE__BV_SUN] = 0.0f;
    for (int i = 0; i < 3; ++i)
        out[WPE__BV_SHADOW + i] = 0.0f;
    if (!u->lit) return;

    /* Lit from the side facing the camera. */
    WPE_Vec3 n = v->normal;
    if (wpe_vec3_dot(n, to_camera) < 0.0f) n = wpe_vec3_scale(n, -1.0f);
    WPE_Vec3 light = u->ambient;
    for (uint32_t l = 0; l < u->light_count; ++l) {
        const WPE_ShaderLight* s = &u->lights[l];
        const WPE_Vec3 to_light = wpe_vec3_sub(s->position, p);
        const float distance = wpe_vec3_length(to_light);
        float attenuation = wpe_light_attenuation(distance, s->range);
        if (!(attenuation > 0.0f) || !(distance > 1e-6f)) continue;
        const WPE_Vec3 l_dir = wpe_vec3_scale(to_light, 1.0f / distance);
        if (s->spot) {
            const float span = s->cos_inner - s->cos_outer;
            const float cos_angle = -wpe_vec3_dot(l_dir, s->direction);
            float cone =
                span > 1e-6f ? (cos_angle - s->cos_outer) / span : (cos_angle >= s->cos_outer ? 1.0f : 0.0f);
            cone = cone < 0.0f ? 0.0f : (cone > 1.0f ? 1.0f : cone);
            attenuation *= cone * cone * (3.0f - 2.0f * cone);
        }
        light =
            wpe_vec3_add(light, wpe_vec3_scale(s->color, attenuation * wpe__wrap(wpe_vec3_dot(n, l_dir))));
    }
    for (int i = 0; i < 3; ++i)
        out[WPE__BV_LIGHT + i] = light.v[i];
    if (u->has_sun) out[WPE__BV_SUN] = wpe__wrap(-wpe_vec3_dot(n, u->sun_direction));
    if (u->sun_shadow) {
        const WPE_Vec3 s = wpe_mat4_transform_point(&u->shadow_matrix, p);
        out[WPE__BV_SHADOW + 0] = s.x;
        out[WPE__BV_SHADOW + 1] = s.y;
        out[WPE__BV_SHADOW + 2] = s.z;
    }
}

static bool wpe__billboard_fs(const void* uniforms, const WPE_FragmentInput* in, WPE_Vec4* out_color) {
    const wpe__BillboardUniforms* u = uniforms;
    const float* v = in->varyings;
    WPE_Vec4 color =
        wpe_vec4(v[WPE__BV_COLOR], v[WPE__BV_COLOR + 1], v[WPE__BV_COLOR + 2], v[WPE__BV_COLOR + 3]);
    if (u->texture) {
        const WPE_Vec4 t =
            wpe_texture_sample_lod(u->texture, &wpe__billboard_sampler,
                                   (WPE_Vec2){.x = v[WPE__BV_UV], .y = v[WPE__BV_UV + 1]}, 0.0f);
        color = wpe_vec4(color.x * t.x, color.y * t.y, color.z * t.z, color.w * t.w);
    }
    if (u->cutout ? !(color.w >= 0.5f) : !(color.w > 0.0f)) return false;
    if (u->lit) {
        const float shadow = u->sun_shadow
                                 ? wpe_shadow_visibility(u->sun_shadow, v[WPE__BV_SHADOW],
                                                         v[WPE__BV_SHADOW + 1], v[WPE__BV_SHADOW + 2])
                                 : 1.0f;
        for (int i = 0; i < 3; ++i)
            color.v[i] *= v[WPE__BV_LIGHT + i] + u->sun_color.v[i] * v[WPE__BV_SUN] * shadow;
    }
    const float fog = wpe_fog_factor(&u->fog, v[WPE__BV_DISTANCE]);
    if (fog > 0.0f) {
        for (int i = 0; i < 3; ++i)
            color.v[i] =
                u->additive ? color.v[i] * (1.0f - fog) : color.v[i] + (u->fog.color.v[i] - color.v[i]) * fog;
    }
    if (u->cutout) color.w = 1.0f;
    *out_color = color;
    return true;
}

static const WPE_ShaderProgram wpe__billboard_program = {
    .vertex = wpe__billboard_vs, .fragment = wpe__billboard_fs, .varying_count = WPE__BV_COUNT};

const WPE_ShaderProgram* wpe__shader_billboard(void) {
    return &wpe__billboard_program;
}
