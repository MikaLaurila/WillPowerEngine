#include "graphics/animation.h"

#include <math.h>
#include <string.h>

#include "core/debug.h"

/* Index of the last key whose time is <= t (0 when t precedes all keys). */
static uint32_t wpe__find_key(const float* times, uint32_t count, float t) {
    uint32_t lo = 0, hi = count - 1;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo + 1) / 2;
        if (times[mid] <= t) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return lo;
}

static void wpe__normalize4(float v[4]) {
    const WPE_Quat q = wpe_quat_normalize((WPE_Quat){.x = v[0], .y = v[1], .z = v[2], .w = v[3]});
    v[0] = q.x;
    v[1] = q.y;
    v[2] = q.z;
    v[3] = q.w;
}

void wpe_animation_sample_channel(const WPE_AnimationChannel* channel, float time, float out_value[4]) {
    wpe_assert(channel && out_value && channel->key_count > 0);
    const uint32_t components = channel->path == WPE_ANIM_ROTATION ? 4u : 3u;
    const bool cubic = channel->interpolation == WPE_INTERP_CUBIC_SPLINE;
    /* Cubic spline keys store (in-tangent, value, out-tangent); `stride` steps whole keys. */
    const uint32_t stride = components * (cubic ? 3u : 1u);
    const uint32_t value_offset = cubic ? components : 0u;
    const float* values = channel->values;
    const uint32_t last = channel->key_count - 1;

    uint32_t k = 0;
    float s = 0.0f;
    bool interpolate = false;
    if (!(time > channel->times[0])) {
        k = 0;
    } else if (time >= channel->times[last]) {
        k = last;
    } else {
        k = wpe__find_key(channel->times, channel->key_count, time);
        const float span = channel->times[k + 1] - channel->times[k];
        interpolate = span > 0.0f && channel->interpolation != WPE_INTERP_STEP;
        s = interpolate ? (time - channel->times[k]) / span : 0.0f;
    }

    const float* a = values + (size_t)k * stride + value_offset;
    if (!interpolate) {
        for (uint32_t c = 0; c < components; ++c)
            out_value[c] = a[c];
        if (components == 4) wpe__normalize4(out_value);
        return;
    }
    const float* b = values + (size_t)(k + 1) * stride + value_offset;

    if (cubic) {
        /* Hermite spline (glTF spec, Appendix C): tangents are scaled by the key interval. */
        const float span = channel->times[k + 1] - channel->times[k];
        const float* out_tangent_a = a + components;
        const float* in_tangent_b = b - components;
        const float s2 = s * s, s3 = s2 * s;
        const float h00 = 2 * s3 - 3 * s2 + 1, h10 = s3 - 2 * s2 + s, h01 = -2 * s3 + 3 * s2, h11 = s3 - s2;
        for (uint32_t c = 0; c < components; ++c) {
            out_value[c] =
                h00 * a[c] + h10 * span * out_tangent_a[c] + h01 * b[c] + h11 * span * in_tangent_b[c];
        }
        if (components == 4) wpe__normalize4(out_value);
        return;
    }

    if (components == 4) {
        const WPE_Quat qa = {.x = a[0], .y = a[1], .z = a[2], .w = a[3]};
        const WPE_Quat qb = {.x = b[0], .y = b[1], .z = b[2], .w = b[3]};
        const WPE_Quat q = wpe_quat_slerp(qa, qb, s);
        out_value[0] = q.x;
        out_value[1] = q.y;
        out_value[2] = q.z;
        out_value[3] = q.w;
        return;
    }
    for (uint32_t c = 0; c < components; ++c)
        out_value[c] = a[c] + (b[c] - a[c]) * s;
}

/* Writes an animation's pose at `time` into the node transforms, blended over what they hold with
 * `weight` (1: replaces it). */
static void wpe__apply_animation(WPE_Ecs* ecs, const WPE_Animator* animator, uint32_t index, float time,
                                 float weight) {
    const WPE_Model* model = animator->model;
    const WPE_Animation* animation = &model->animations[index];
    for (uint32_t c = 0; c < animation->channel_count; ++c) {
        const WPE_AnimationChannel* channel = &model->channels[animation->first_channel + c];
        if (channel->node >= animator->node_count) continue;
        WPE_Transform* t =
            wpe_ecs_get_component(ecs, animator->node_entities[channel->node], WPE_COMP_TRANSFORM);
        if (!t) continue; /* Node entity was despawned or lost its transform. */

        float value[4];
        wpe_animation_sample_channel(channel, time, value);
        const WPE_Vec3 v = wpe_vec3(value[0], value[1], value[2]);
        switch (channel->path) {
        case WPE_ANIM_TRANSLATION:
            t->position = weight >= 1.0f ? v : wpe_vec3_lerp(t->position, v, weight);
            break;
        case WPE_ANIM_ROTATION: {
            const WPE_Quat q = {.x = value[0], .y = value[1], .z = value[2], .w = value[3]};
            t->rotation = weight >= 1.0f ? q : wpe_quat_slerp(t->rotation, q, weight);
            break;
        }
        case WPE_ANIM_SCALE:
            t->scale = weight >= 1.0f ? v : wpe_vec3_lerp(t->scale, v, weight);
            break;
        }
    }
}

static bool wpe__fading(const WPE_Animator* animator) {
    return animator->fade_duration > 0.0f && animator->from_animation < animator->model->animation_count;
}

void wpe_animation_apply(WPE_Ecs* ecs, const WPE_Animator* animator) {
    if (!ecs || !animator || !animator->model || animator->animation >= animator->model->animation_count)
        return;
    if (!wpe__fading(animator)) {
        wpe__apply_animation(ecs, animator, animator->animation, animator->time, 1.0f);
        return;
    }
    /* The old animation in full, then the new one over it by its weight so far. */
    const float weight = animator->fade / animator->fade_duration;
    wpe__apply_animation(ecs, animator, animator->from_animation, animator->from_time, 1.0f);
    wpe__apply_animation(ecs, animator, animator->animation, animator->time, weight < 1.0f ? weight : 1.0f);
}

/* Advances a playback time: looping wraps; otherwise it clamps at the end it runs toward, and
 * `*out_ended` reports reaching it. */
static float wpe__advance(float time, float dt, float speed, float duration, bool loop, bool* out_ended) {
    *out_ended = false;
    time += dt * speed;
    if (!isfinite(time)) return 0.0f;
    if (duration <= 0.0f) return 0.0f;
    if (loop) {
        time = fmodf(time, duration);
        return time < 0.0f ? time + duration : time;
    }
    if (time >= duration && speed > 0.0f) {
        *out_ended = true;
        return duration;
    }
    if (time <= 0.0f && speed < 0.0f) {
        *out_ended = true;
        return 0.0f;
    }
    return time < 0.0f ? 0.0f : (time > duration ? duration : time);
}

WPE_Result wpe_animation_update(WPE_Ecs* ecs, float dt) {
    if (!ecs) return WPE_RESULT_ERROR_NULL_ARG;
    WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_ANIMATOR)});
    while (wpe_ecs_iter_next(&it)) {
        WPE_Animator* animator = wpe_ecs_iter_get(&it, WPE_COMP_ANIMATOR);
        if (!animator->playing || !animator->model || animator->animation >= animator->model->animation_count)
            continue;
        const WPE_Animation* animations = animator->model->animations;
        bool ended = false;
        animator->time = wpe__advance(animator->time, dt, animator->speed,
                                      animations[animator->animation].duration, animator->loop, &ended);
        if (ended) animator->playing = false;
        if (wpe__fading(animator)) {
            bool from_ended = false; /* The old animation holds its last pose once it ends. */
            animator->from_time =
                wpe__advance(animator->from_time, dt, animator->from_speed,
                             animations[animator->from_animation].duration, animator->from_loop, &from_ended);
            animator->fade += dt;
            if (!(animator->fade < animator->fade_duration)) animator->fade_duration = 0.0f; /* Done. */
        }
        wpe_animation_apply(ecs, animator);
    }
    return WPE_RESULT_OK;
}

WPE_Result wpe_animator_play(WPE_Animator* animator, uint32_t animation, float fade_seconds, bool loop) {
    if (!animator || !animator->model) return WPE_RESULT_ERROR_NULL_ARG;
    if (animation >= animator->model->animation_count || !isfinite(fade_seconds) || fade_seconds < 0.0f)
        return WPE_RESULT_ERROR_INVALID_ARG;
    const bool from_valid = animator->animation < animator->model->animation_count;
    if (fade_seconds > 0.0f && from_valid) {
        animator->from_animation = animator->animation;
        animator->from_time = animator->time;
        /* A paused animation fades out where it stopped. */
        animator->from_speed = animator->playing ? animator->speed : 0.0f;
        animator->from_loop = animator->loop;
        animator->fade = 0.0f;
        animator->fade_duration = fade_seconds;
    } else {
        animator->fade_duration = 0.0f;
    }
    animator->animation = animation;
    animator->time = animator->speed < 0.0f ? animator->model->animations[animation].duration : 0.0f;
    animator->loop = loop;
    animator->playing = true;
    return WPE_RESULT_OK;
}

uint32_t wpe_model_find_animation(const WPE_Model* model, const char* name) {
    if (!model || !name) return WPE_MODEL_NONE;
    for (uint32_t i = 0; i < model->animation_count; ++i)
        if (strcmp(model->animations[i].name, name) == 0) return i;
    return WPE_MODEL_NONE;
}
