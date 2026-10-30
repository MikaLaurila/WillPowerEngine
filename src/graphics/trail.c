#define WPE_LOG_MODULE "graphics"
#include <string.h>

#include "core/log.h"
#include "graphics/billboard_internal.h"
#include "graphics/graphics.h"
#include "scene/scene.h"

WPE_Trail wpe_trail_default(void) {
    return (WPE_Trail){.base = WPE_ENTITY_NULL,
                       .tip = WPE_ENTITY_NULL,
                       .emitting = true,
                       .lifetime = 0.2f,
                       .color_start = wpe_vec4(0.75f, 0.9f, 1.0f, 0.9f),
                       .color_end = wpe_vec4(0.3f, 0.5f, 1.0f, 0.0f),
                       .width_end = 0.3f,
                       .smoothing = 4,
                       .blend = WPE_BILLBOARD_ADDITIVE,
                       .visible = true};
}

void wpe__trail_on_add(WPE_Ecs* ecs, WPE_Entity entity, void* component, void* user_data) {
    (void)ecs;
    (void)entity;
    (void)user_data;
    ((WPE_Trail*)component)->sample_count = 0; /* A copy starts without the original's places. */
}

/* Where an entity is in the world now (its cached world matrix), if it still exists. */
static bool wpe__world_point(const WPE_Ecs* ecs, WPE_Entity entity, WPE_Vec3* out) {
    const WPE_Transform* t = wpe_ecs_get_component((WPE_Ecs*)ecs, entity, WPE_COMP_TRANSFORM);
    if (!t) return false;
    *out = wpe_mat4_transform_point(&t->world, wpe_vec3(0, 0, 0));
    return true;
}

void wpe_trail_update(WPE_Ecs* ecs, float dt) {
    if (!ecs || !(dt >= 0.0f)) return;
    WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_TRAIL)});
    while (wpe_ecs_iter_next(&it)) {
        WPE_Trail* trail = wpe_ecs_iter_get(&it, WPE_COMP_TRAIL);
        /* Age, and drop what has outlived the trail (the oldest are last). */
        uint32_t kept = 0;
        for (uint32_t i = 0; i < trail->sample_count; ++i) {
            trail->samples[i].age += dt;
            if (trail->samples[i].age <= trail->lifetime) kept = i + 1;
        }
        trail->sample_count = kept;
        WPE_Vec3 base, tip;
        if (!trail->emitting || !wpe__world_point(ecs, trail->base, &base) ||
            !wpe__world_point(ecs, trail->tip, &tip))
            continue;
        /* The newest first: the rest move down (the oldest falls off a full trail). */
        const uint32_t moved =
            trail->sample_count < WPE_TRAIL_MAX_SAMPLES ? trail->sample_count : WPE_TRAIL_MAX_SAMPLES - 1;
        memmove(&trail->samples[1], &trail->samples[0], moved * sizeof trail->samples[0]);
        trail->samples[0] = (WPE_TrailSample){.base = base, .tip = tip, .age = 0.0f};
        trail->sample_count = moved + 1;
    }
}

void wpe_trail_clear(WPE_Ecs* ecs, WPE_Entity entity) {
    WPE_Trail* trail = ecs ? wpe_ecs_get_component(ecs, entity, WPE_COMP_TRAIL) : NULL;
    if (trail) trail->sample_count = 0;
}
