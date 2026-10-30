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
                       .visible = true,
                       .playing = false};
}

WPE_Vec3 wpe_slash_point(const WPE_Slash* slash, float t) {
    const float u = 1.0f - t;
    const float w[4] = {u * u * u, 3.0f * u * u * t, 3.0f * u * t * t, t * t * t};
    WPE_Vec3 p = wpe_vec3(0, 0, 0);
    for (int i = 0; i < 4; ++i)
        p = wpe_vec3_add(p, wpe_vec3_scale(slash->points[i], w[i]));
    return p;
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
            follower->position = wpe_slash_point(slash, head);
    }
}
