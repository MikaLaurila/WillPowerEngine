#define WPE_LOG_MODULE "graphics"
#include <math.h>

#include "graphics/graphics.h"

WPE_Water wpe_water_default(void) {
    return (WPE_Water){.amplitude = 0.1f, .wavelength = 3.0f, .speed = 1.0f, .shimmer = 0.2f};
}

/* Keeps `value` in [0, period) (0 for a non-positive or non-finite period). */
static float wpe__wrap(float value, float period) {
    if (!(period > 0.0f) || !isfinite(period) || !isfinite(value)) return 0.0f;
    value -= period * floorf(value / period);
    return value < period ? value : 0.0f;
}

void wpe_surface_update(WPE_Ecs* ecs, float dt) {
    if (!ecs || !(dt >= 0.0f)) return;
    WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_MESH)});
    while (wpe_ecs_iter_next(&it)) {
        WPE_MeshRenderer* mesh = wpe_ecs_iter_get(&it, WPE_COMP_MESH);
        if (mesh->uv_scroll.x == 0.0f && mesh->uv_scroll.y == 0.0f) continue;
        /* Repeat has period 1 and mirror period 2 (in texture coordinates), so wrapping by 2 changes neither.
         */
        mesh->uv_offset.x = wpe__wrap(mesh->uv_offset.x + mesh->uv_scroll.x * dt, 2.0f);
        mesh->uv_offset.y = wpe__wrap(mesh->uv_offset.y + mesh->uv_scroll.y * dt, 2.0f);
    }
    it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_WATER)});
    while (wpe_ecs_iter_next(&it)) {
        WPE_Water* water = wpe_ecs_iter_get(&it, WPE_COMP_WATER);
        /* The waves repeat every 3 wavelengths of travel (wpe_water_wave: the second wave's crests are 3/4
         * apart, the shimmer runs at twice the first wave's rate). */
        const float period = water->speed != 0.0f ? 3.0f * water->wavelength / fabsf(water->speed) : 0.0f;
        water->wpe__time = wpe__wrap(water->wpe__time + dt, period);
    }
}
