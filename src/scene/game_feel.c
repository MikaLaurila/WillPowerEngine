#define WPE_LOG_MODULE "scene"
#include "scene/game_feel.h"

#include <math.h>

#include "core/log.h"
#include "scene/scene.h"

WPE_GameFeel wpe_game_feel_default(void) {
    return (WPE_GameFeel){.base_scale = 1.0f, .slow_scale = 1.0f};
}

/* ---- Time scale ------------------------------------------------------------------------------------ */

/* Slow motion's scale with `remaining` real seconds left: its own, easing to the base over the fade. */
static float wpe__slow_scale(const WPE_GameFeel* feel, float remaining) {
    if (feel->slow_fade > 0.0f && remaining < feel->slow_fade) {
        const float t = remaining / feel->slow_fade; /* 1 as the fade starts, 0 at the end. */
        return feel->base_scale + (feel->slow_scale - feel->base_scale) * t;
    }
    return feel->slow_scale;
}

float wpe_game_feel_time_scale(const WPE_GameFeel* feel) {
    if (feel->stop_remaining > 0.0f) return 0.0f;
    if (feel->slow_remaining > 0.0f) return wpe__slow_scale(feel, feel->slow_remaining);
    return feel->base_scale;
}

double wpe_game_feel_advance(WPE_GameFeel* feel, double real_dt) {
    if (!(real_dt > 0.0) || !isfinite(real_dt)) real_dt = 0.0;
    feel->real_dt = (float)real_dt;
    float rest = (float)real_dt;
    /* A hit-stop freezes its share of the frame; slow motion's clock runs through it too. */
    const float frozen = fminf(feel->stop_remaining, rest);
    feel->stop_remaining -= frozen;
    feel->slow_remaining = fmaxf(feel->slow_remaining - frozen, 0.0f);
    rest -= frozen;
    double game_dt = 0.0;
    if (rest > 0.0f && feel->slow_remaining > 0.0f) {
        /* The slow share at the scale halfway through it (exact while not fading, close while fading). */
        const float part = fminf(rest, feel->slow_remaining);
        game_dt += (double)part * wpe__slow_scale(feel, feel->slow_remaining - part * 0.5f);
        feel->slow_remaining -= part;
        rest -= part;
    }
    return game_dt + (double)rest * feel->base_scale;
}

void wpe_game_feel_slow_motion(WPE_GameFeel* feel, float scale, float duration, float fade) {
    if (!(scale >= 0.0f) || !isfinite(scale) || !(duration > 0.0f) || !isfinite(duration)) return;
    feel->slow_scale = scale;
    feel->slow_remaining = duration;
    feel->slow_fade = fade > 0.0f ? fminf(fade, duration) : 0.0f;
}

void wpe_game_feel_hit_stop(WPE_GameFeel* feel, float duration) {
    if (duration > 0.0f && isfinite(duration)) feel->stop_remaining = fmaxf(feel->stop_remaining, duration);
}

/* ---- Shakes ---------------------------------------------------------------------------------------- */

/* A hashed value in [0, 1) for (seed, index). */
static float wpe__hash01(uint32_t seed, uint32_t index) {
    uint32_t h = seed * 0x9E3779B1u ^ (index + 1u) * 0x85EBCA77u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return (float)(h & 0xFFFFFFu) / 16777216.0f;
}

void wpe_shake_offset(const WPE_ShakeParams* params, uint32_t seed, float elapsed, WPE_Vec3* out_position,
                      WPE_Vec3* out_rotation) {
    *out_position = *out_rotation = wpe_vec3(0, 0, 0);
    if (!(params->duration > 0.0f) || !(elapsed >= 0.0f) || elapsed >= params->duration) return;
    const float fade = 1.0f - elapsed / params->duration;
    const float envelope = fade * fade;
    const float w = 2.0f * WPE_PI * params->frequency * elapsed;
    float channel[6];
    for (uint32_t c = 0; c < 6; ++c) {
        /* Two sines at an irrational ratio with random phases: smooth, and it never quite repeats. */
        const float p1 = 2.0f * WPE_PI * wpe__hash01(seed, 2 * c),
                    p2 = 2.0f * WPE_PI * wpe__hash01(seed, 2 * c + 1);
        channel[c] = envelope * (0.6f * sinf(w + p1) + 0.4f * sinf(1.618034f * w + p2));
    }
    const float radians = params->rotation * (WPE_PI / 180.0f);
    *out_position = wpe_vec3(channel[0] * params->amplitude, channel[1] * params->amplitude,
                             channel[2] * params->amplitude);
    *out_rotation = wpe_vec3(channel[3] * radians, channel[4] * radians, channel[5] * radians);
}

static WPE_Shake* wpe__find_shake(WPE_GameFeel* feel, WPE_Entity entity) {
    for (uint32_t i = 0; i < feel->shake_count; ++i) {
        if (feel->shakes[i].entity == entity) return &feel->shakes[i];
    }
    return NULL;
}

void wpe_game_feel_shake(WPE_GameFeel* feel, WPE_Entity entity, const WPE_ShakeParams* params) {
    if (!params || !(params->duration > 0.0f) || !isfinite(params->duration) ||
        !isfinite(params->amplitude) || !isfinite(params->rotation) || !isfinite(params->frequency))
        return;
    WPE_Shake* shake = wpe__find_shake(feel, entity);
    if (!shake) {
        if (feel->shake_count == WPE_MAX_SHAKES) {
            wpe_log_warn("too many shakes at once (%u); entity 0x%08x won't shake", WPE_MAX_SHAKES, entity);
            return;
        }
        shake = &feel->shakes[feel->shake_count++];
        *shake = (WPE_Shake){.entity = entity};
    }
    /* A shake on the transform now stays recorded, so the pose is still restored. */
    shake->params = *params;
    shake->elapsed = 0.0f;
    shake->seed = ++feel->next_seed;
}

void wpe_game_feel_stop_shake(WPE_GameFeel* feel, WPE_Entity entity) {
    WPE_Shake* shake = wpe__find_shake(feel, entity);
    if (shake) shake->params.duration = 0.0f; /* Finished: restored and dropped at the next apply. */
}

bool wpe_game_feel_is_shaking(const WPE_GameFeel* feel, WPE_Entity entity) {
    for (uint32_t i = 0; i < feel->shake_count; ++i) {
        if (feel->shakes[i].entity == entity)
            return feel->shakes[i].elapsed < feel->shakes[i].params.duration;
    }
    return false;
}

static bool wpe__same_quat(WPE_Quat a, WPE_Quat b) {
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

/* Takes one shake's offset off, unless something has moved the entity since it was put on. */
static void wpe__restore(WPE_Shake* shake, WPE_Ecs* ecs) {
    if (!shake->applied) return;
    shake->applied = false;
    WPE_Transform* t = ecs ? wpe_ecs_get_component(ecs, shake->entity, WPE_COMP_TRANSFORM) : NULL;
    if (!t) return;
    const WPE_Vec3 p = t->position, s = shake->shaken_position;
    if (p.x == s.x && p.y == s.y && p.z == s.z) t->position = shake->base_position;
    if (wpe__same_quat(t->rotation, shake->shaken_rotation)) t->rotation = shake->base_rotation;
}

void wpe_game_feel_restore_shakes(WPE_GameFeel* feel, WPE_Ecs* ecs) {
    for (uint32_t i = 0; i < feel->shake_count; ++i)
        wpe__restore(&feel->shakes[i], ecs);
}

void wpe_game_feel_apply_shakes(WPE_GameFeel* feel, WPE_Ecs* ecs, float real_dt) {
    if (!(real_dt >= 0.0f) || !isfinite(real_dt)) real_dt = 0.0f;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < feel->shake_count; ++i) {
        WPE_Shake shake = feel->shakes[i];
        wpe__restore(&shake, ecs);
        shake.elapsed += real_dt;
        WPE_Transform* t = wpe_ecs_get_component(ecs, shake.entity, WPE_COMP_TRANSFORM);
        if (!t || shake.elapsed >= shake.params.duration) continue; /* Gone or finished. */
        WPE_Vec3 offset, turn;
        wpe_shake_offset(&shake.params, shake.seed, shake.elapsed, &offset, &turn);
        shake.base_position = t->position;
        shake.base_rotation = t->rotation;
        t->position = wpe_vec3_add(t->position, offset);
        t->rotation = wpe_quat_normalize(wpe_quat_mul(t->rotation, wpe_quat_from_euler_xyz(turn)));
        shake.shaken_position = t->position;
        shake.shaken_rotation = t->rotation;
        shake.applied = true;
        feel->shakes[kept++] = shake;
    }
    feel->shake_count = kept;
}

void wpe_game_feel_reset(WPE_GameFeel* feel, WPE_Ecs* ecs) {
    if (ecs) wpe_game_feel_restore_shakes(feel, ecs);
    const uint32_t seed = feel->next_seed;
    *feel = wpe_game_feel_default();
    feel->next_seed = seed;
}
