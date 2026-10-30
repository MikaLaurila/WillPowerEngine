#define WPE_LOG_MODULE "graphics"
#include "core/log.h"
#include "graphics/graphics.h"
#include "scene/scene.h"

WPE_Slash wpe_slash_default(void) {
    /* A crescent from left to right, bulging upward. */
    return (WPE_Slash){.points = {{{-0.6f, 0.0f, 0.0f}},
                                  {{-0.3f, 0.45f, 0.0f}},
                                  {{0.3f, 0.45f, 0.0f}},
                                  {{0.6f, 0.0f, 0.0f}}},
                       .width = 0.25f,
                       .length = 0.5f,
                       .duration = 0.3f,
                       .color_head = wpe_vec4(0.85f, 0.95f, 1.0f, 1.0f),
                       .color_tail = wpe_vec4(0.3f, 0.5f, 1.0f, 0.0f),
                       .blend = WPE_BILLBOARD_ADDITIVE,
                       .segments = 24,
                       .head = WPE_ENTITY_NULL,
                       .even = true,
                       .visible = true,
                       .playing = false,
                       .distortion = 0.0f,
                       .distortion_width = 10.0f,
                       .distortion_tint = wpe_vec4(1.0f, 1.0f, 1.0f, 0.0f)};
}

WPE_Vec3 wpe_slash_point(const WPE_Slash* slash, float t) {
    const float u = 1.0f - t;
    const float w[4] = {u * u * u, 3.0f * u * u * t, 3.0f * u * t * t, t * t * t};
    WPE_Vec3 p = wpe_vec3(0, 0, 0);
    for (int i = 0; i < 4; ++i)
        p = wpe_vec3_add(p, wpe_vec3_scale(slash->points[i], w[i]));
    return p;
}

#define WPE__SLASH_LENGTH_SAMPLES 32

WPE_Vec3 wpe_slash_point_along(const WPE_Slash* slash, float along) {
    along = along < 0.0f ? 0.0f : (along > 1.0f ? 1.0f : along);
    if (!slash->even) return wpe_slash_point(slash, along);
    /* The curve's length at evenly spaced parameters, then the parameter where `along` of it is reached. */
    float lengths[WPE__SLASH_LENGTH_SAMPLES + 1];
    lengths[0] = 0.0f;
    WPE_Vec3 previous = slash->points[0];
    for (int i = 1; i <= WPE__SLASH_LENGTH_SAMPLES; ++i) {
        const WPE_Vec3 p = wpe_slash_point(slash, (float)i / WPE__SLASH_LENGTH_SAMPLES);
        lengths[i] = lengths[i - 1] + wpe_vec3_length(wpe_vec3_sub(p, previous));
        previous = p;
    }
    const float goal = along * lengths[WPE__SLASH_LENGTH_SAMPLES];
    if (!(lengths[WPE__SLASH_LENGTH_SAMPLES] > 1e-9f)) return wpe_slash_point(slash, along);
    int i = 1;
    while (i < WPE__SLASH_LENGTH_SAMPLES && lengths[i] < goal)
        ++i;
    const float span = lengths[i] - lengths[i - 1];
    const float f = span > 1e-9f ? (goal - lengths[i - 1]) / span : 0.0f;
    return wpe_slash_point(slash, ((float)(i - 1) + f) / WPE__SLASH_LENGTH_SAMPLES);
}
void wpe_slash_play(WPE_Ecs* ecs, WPE_Entity entity) {
    WPE_Slash* slash = ecs ? wpe_ecs_get_component(ecs, entity, WPE_COMP_SLASH) : NULL;
    if (!slash) return;
    slash->playing = true;
    slash->time = 0.0f;
}

void wpe_slash_update(WPE_Ecs* ecs, float dt) {
    if (!ecs || !(dt >= 0.0f)) return;
    WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_SLASH)});
    while (wpe_ecs_iter_next(&it)) {
        WPE_Slash* slash = wpe_ecs_iter_get(&it, WPE_COMP_SLASH);
        if (!slash->playing) continue;
        slash->time += dt;
        const float duration = slash->duration > 1e-4f ? slash->duration : 1e-4f;
        if (slash->time >= duration) {
            slash->playing = false;
            continue;
        }
        /* The head runs ahead of the tail by `length`, both covering the curve over the duration. */
        const float head = fminf(slash->time / duration * (1.0f + slash->length), 1.0f);
        WPE_Transform* follower = wpe_ecs_get_component(ecs, slash->head, WPE_COMP_TRANSFORM);
        if (follower && wpe_scene_get_parent(ecs, slash->head) == it.entity)
            follower->position = wpe_slash_point_along(slash, head);
    }
}

uint32_t wpe_slash_distortions(WPE_Ecs* ecs, const WPE_Mat4* view_projection, uint32_t width, uint32_t height,
                               WPE_CurveDistortParams* out_params, float* out_points, uint32_t max) {
    if (!ecs || !view_projection || !out_params || !out_points) return 0;
    uint32_t count = 0;
    WPE_EcsIter it = wpe_ecs_query(
        ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_SLASH) | WPE_ECS_MASK(WPE_COMP_TRANSFORM)});
    while (count < max && wpe_ecs_iter_next(&it)) {
        const WPE_Slash* slash = wpe_ecs_iter_get(&it, WPE_COMP_SLASH);
        const WPE_Transform* t = wpe_ecs_iter_get(&it, WPE_COMP_TRANSFORM);
        if (!slash->visible || !slash->playing || !(slash->distortion != 0.0f)) continue;
        /* The visible piece, as wpe_scene_render() draws it. */
        const float duration = slash->duration > 1e-4f ? slash->duration : 1e-4f;
        const float sweep = slash->time / duration * (1.0f + slash->length);
        const float head = fminf(sweep, 1.0f), tail = fmaxf(sweep - slash->length, 0.0f);
        if (!(head > tail)) continue;
        float* points = &out_points[(size_t)count * WPE_SLASH_DISTORTION_POINTS * 3];
        uint32_t n = 0;
        for (uint32_t k = 0; k < WPE_SLASH_DISTORTION_POINTS; ++k) {
            const float along = tail + (head - tail) * (float)k / (float)(WPE_SLASH_DISTORTION_POINTS - 1);
            const WPE_Vec3 world = wpe_mat4_transform_point(&t->world, wpe_slash_point_along(slash, along));
            const WPE_Vec4 clip =
                wpe_mat4_mul_vec4(view_projection, wpe_vec4(world.x, world.y, world.z, 1.0f));
            if (!(clip.w > 1e-4f)) break;
            float* out = &points[(size_t)n * 3];
            out[0] = (clip.x / clip.w * 0.5f + 0.5f) * (float)width;
            out[1] = (0.5f - clip.y / clip.w * 0.5f) * (float)height;
            out[2] = clip.z / clip.w * 0.5f + 0.5f;
            ++n;
        }
        if (n < 2) continue;
        out_params[count++] = (WPE_CurveDistortParams){
            .points = points,
            .point_count = n,
            .amplitude = slash->distortion,
            .width = slash->distortion_width,
            .taper = 0.8f,
            .smear_taps = 5,
            .smear_spread = 0.6f,
            .prism = fabsf(slash->distortion) * 0.4f,
            .ripple = fabsf(slash->distortion) * 0.15f,
            .phase = slash->time,
            .tint = slash->distortion_tint,
            .depth_fade = 0.7f,
            .strength = fminf((1.0f - slash->time / duration) * 5.0f, 1.0f),
        };
    }
    return count;
}
