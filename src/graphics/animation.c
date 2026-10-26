#include "graphics/animation.h"

#include <math.h>

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

void wpe_animation_apply(WPE_Ecs* ecs, const WPE_Animator* animator) {
    if (!ecs || !animator || !animator->model || animator->animation >= animator->model->animation_count)
        return;
    const WPE_Model* model = animator->model;
    const WPE_Animation* animation = &model->animations[animator->animation];

    for (uint32_t c = 0; c < animation->channel_count; ++c) {
        const WPE_AnimationChannel* channel = &model->channels[animation->first_channel + c];
        if (channel->node >= animator->node_count) continue;
        WPE_Transform* t =
            wpe_ecs_get_component(ecs, animator->node_entities[channel->node], WPE_COMP_TRANSFORM);
        if (!t) continue; /* Node entity was despawned or lost its transform. */

        float value[4];
        wpe_animation_sample_channel(channel, animator->time, value);
        switch (channel->path) {
        case WPE_ANIM_TRANSLATION:
            t->position = wpe_vec3(value[0], value[1], value[2]);
            break;
        case WPE_ANIM_ROTATION:
            t->rotation = (WPE_Quat){.x = value[0], .y = value[1], .z = value[2], .w = value[3]};
            break;
        case WPE_ANIM_SCALE:
            t->scale = wpe_vec3(value[0], value[1], value[2]);
            break;
        }
    }
}

WPE_Result wpe_animation_update(WPE_Ecs* ecs, float dt) {
    if (!ecs) return WPE_RESULT_ERROR_NULL_ARG;
    WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_ANIMATOR)});
    while (wpe_ecs_iter_next(&it)) {
        WPE_Animator* animator = wpe_ecs_iter_get(&it, WPE_COMP_ANIMATOR);
        if (!animator->playing || !animator->model || animator->animation >= animator->model->animation_count)
            continue;

        const float duration = animator->model->animations[animator->animation].duration;
        float time = animator->time + dt * animator->speed;
        if (!isfinite(time)) time = 0.0f;
        if (duration <= 0.0f) {
            time = 0.0f;
        } else if (animator->loop) {
            time = fmodf(time, duration);
            if (time < 0.0f) time += duration;
        } else if (time >= duration && animator->speed > 0.0f) {
            time = duration;
            animator->playing = false;
        } else if (time <= 0.0f && animator->speed < 0.0f) {
            time = 0.0f;
            animator->playing = false;
        } else {
            time = time < 0.0f ? 0.0f : (time > duration ? duration : time);
        }
        animator->time = time;
        wpe_animation_apply(ecs, animator);
    }
    return WPE_RESULT_OK;
}
