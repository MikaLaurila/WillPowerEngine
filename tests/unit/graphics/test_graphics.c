#include "wpe_test.h"
#include "wpe_test_assets.h"

#include "assets/font.h"
#include "assets/procedural.h"
#include "graphics/animation.h"
#include "graphics/debug_lines.h"
#include "graphics/draw2d.h"
#include "graphics/graphics.h"
#include "graphics/material_shader.h"
#include "graphics/scene_render.h"

#define ASSET(path) WPE_TEST_ASSETS_DIR "/" path

static WPE_Ecs* make_world(WPE_AssetManager* assets) {
    WPE_Ecs* ecs = NULL;
    WPE_TEST_ASSERT_OK(wpe_ecs_create(&ecs));
    WPE_TEST_ASSERT_OK(wpe_scene_register_components(ecs));
    WPE_TEST_ASSERT_OK(wpe_graphics_register_components(ecs, assets));
    return ecs;
}

/* ---- Animation sampling ---------------------------------------------------------------------------- */

static void test_graphics_sample_linear_step_and_clamp(void) {
    const float times[3] = {0.0f, 1.0f, 3.0f};
    const float values[9] = {0, 0, 0, 10, 20, 30, 10, 20, 50};
    WPE_AnimationChannel ch = {.path = WPE_ANIM_TRANSLATION,
                               .interpolation = WPE_INTERP_LINEAR,
                               .key_count = 3,
                               .times = times,
                               .values = values};
    float out[4];
    wpe_animation_sample_channel(&ch, 0.5f, out);
    WPE_TEST_ASSERT_NEAR(5.0, out[0], 1e-6);
    WPE_TEST_ASSERT_NEAR(15.0, out[2], 1e-6);
    wpe_animation_sample_channel(&ch, 2.0f, out);
    WPE_TEST_ASSERT_NEAR(40.0, out[2], 1e-5);
    wpe_animation_sample_channel(&ch, -1.0f, out); /* before first key */
    WPE_TEST_ASSERT_NEAR(0.0, out[0], 0);
    wpe_animation_sample_channel(&ch, 99.0f, out); /* after last key */
    WPE_TEST_ASSERT_NEAR(50.0, out[2], 0);
    wpe_animation_sample_channel(&ch, 1.0f, out); /* exactly on a key */
    WPE_TEST_ASSERT_NEAR(30.0, out[2], 0);

    ch.interpolation = WPE_INTERP_STEP;
    wpe_animation_sample_channel(&ch, 0.99f, out);
    WPE_TEST_ASSERT_NEAR(0.0, out[0], 0);
    wpe_animation_sample_channel(&ch, 2.5f, out);
    WPE_TEST_ASSERT_NEAR(30.0, out[2], 0);

    /* A single key is constant. */
    ch = (WPE_AnimationChannel){.path = WPE_ANIM_SCALE, .key_count = 1, .times = times, .values = values + 3};
    wpe_animation_sample_channel(&ch, 5.0f, out);
    WPE_TEST_ASSERT_NEAR(20.0, out[1], 0);
}

static void test_graphics_sample_rotation_slerps_shortest_path(void) {
    const WPE_Quat a = wpe_quat_identity();
    const WPE_Quat b = wpe_quat_from_axis_angle(wpe_vec3(0, 1, 0), WPE_PI / 2);
    /* Store b negated: same rotation, opposite hemisphere; interpolation must still take the short arc. */
    const float times[2] = {0, 1};
    const float values[8] = {a.x, a.y, a.z, a.w, -b.x, -b.y, -b.z, -b.w};
    const WPE_AnimationChannel ch = {
        .path = WPE_ANIM_ROTATION, .key_count = 2, .times = times, .values = values};
    float out[4];
    wpe_animation_sample_channel(&ch, 0.5f, out);
    const WPE_Quat half = wpe_quat_from_axis_angle(wpe_vec3(0, 1, 0), WPE_PI / 4);
    const float dot = out[0] * half.x + out[1] * half.y + out[2] * half.z + out[3] * half.w;
    WPE_TEST_ASSERT_NEAR(1.0, fabsf(dot), 1e-4);
    WPE_TEST_ASSERT_NEAR(1.0, sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]),
                         1e-5);
}

static void test_graphics_sample_cubic_spline(void) {
    /* Hermite from 0 to 1 over [0, 2] with zero tangents: value at the midpoint is exactly 0.5. */
    const float times[2] = {0, 2};
    /* Per key: in-tangent(3), value(3), out-tangent(3). */
    const float values[18] = {0, 0, 0, 0, 0, 0, 0, 0, 0, /**/ 0, 0, 0, 1, 1, 1, 0, 0, 0};
    WPE_AnimationChannel ch = {.path = WPE_ANIM_TRANSLATION,
                               .interpolation = WPE_INTERP_CUBIC_SPLINE,
                               .key_count = 2,
                               .times = times,
                               .values = values};
    float out[4];
    wpe_animation_sample_channel(&ch, 1.0f, out);
    WPE_TEST_ASSERT_NEAR(0.5, out[0], 1e-6);
    wpe_animation_sample_channel(&ch, 0.5f, out); /* s = 0.25: 3s^2 - 2s^3 = 0.15625 */
    WPE_TEST_ASSERT_NEAR(0.15625, out[1], 1e-6);
    wpe_animation_sample_channel(&ch, 2.0f, out);
    WPE_TEST_ASSERT_NEAR(1.0, out[2], 0);

    /* Tangents are scaled by the key interval: out-tangent 1 on key 0 adds h10 * span. */
    const float tangent_values[18] = {0, 0, 0, 0, 0, 0, 1, 0, 0, /**/ 0, 0, 0, 0, 0, 0, 0, 0, 0};
    ch.values = tangent_values;
    wpe_animation_sample_channel(&ch, 1.0f, out); /* s = 0.5: h10 = 0.125, span = 2 */
    WPE_TEST_ASSERT_NEAR(0.25, out[0], 1e-6);
}

/* ---- Crossfades ------------------------------------------------------------------------------------- */

static float quat_alignment(WPE_Quat a, const float b[4]) {
    return fabsf(a.x * b[0] + a.y * b[1] + a.z * b[2] + a.w * b[3]);
}

static void test_graphics_animation_crossfade(void) {
    WPE_TestAssets t = wpe_test_assets_create(WPE_TEST_ASSETS_DIR, 0);
    const WPE_AssetHandle handle = wpe_test_load_model(t.assets, "meshes/RiggedFigure.glb");
    const WPE_Model* model = wpe_asset_model(t.assets, handle);
    WPE_Ecs* ecs = make_world(t.assets);
    WPE_Entity root = WPE_ENTITY_NULL;
    WPE_TEST_ASSERT_OK(wpe_model_instantiate(ecs, t.assets, handle,
                                             &(WPE_InstantiateOptions){.animation = 0, .play = true}, &root));
    WPE_Animator* animator = wpe_ecs_get_component(ecs, root, WPE_COMP_ANIMATOR);
    const float duration = model->animations[0].duration;
    WPE_TEST_ASSERT(duration > 1.2f);

    /* A rotation channel whose two sample times below differ, to see the blend. */
    const WPE_AnimationChannel* channel = NULL;
    for (uint32_t c = 0; c < model->animations[0].channel_count && !channel; ++c) {
        const WPE_AnimationChannel* candidate = &model->channels[model->animations[0].first_channel + c];
        float a[4], b[4];
        wpe_animation_sample_channel(candidate, 0.8f, a);
        wpe_animation_sample_channel(candidate, 0.5f, b);
        const WPE_Quat qa = {.x = a[0], .y = a[1], .z = a[2], .w = a[3]};
        if (candidate->path == WPE_ANIM_ROTATION && quat_alignment(qa, b) < 0.999f) channel = candidate;
    }
    WPE_TEST_ASSERT(channel != NULL);
    const WPE_Entity node = animator->node_entities[channel->node];

    /* From 0.3 s into the animation, crossfade over 1 s into the same animation from its start. */
    animator->time = 0.3f;
    WPE_TEST_ASSERT_OK(wpe_animator_play(animator, 0, 1.0f, true));
    WPE_TEST_ASSERT(animator->fade_duration > 0.0f && animator->time == 0.0f);
    WPE_TEST_ASSERT_NEAR(0.3, animator->from_time, 1e-6);

    /* Halfway: the old animation at 0.8 s and the new one at 0.5 s, half each. */
    WPE_TEST_ASSERT_OK(wpe_animation_update(ecs, 0.5f));
    float from[4], to[4];
    wpe_animation_sample_channel(channel, 0.8f, from);
    wpe_animation_sample_channel(channel, 0.5f, to);
    const WPE_Quat expected =
        wpe_quat_slerp((WPE_Quat){.x = from[0], .y = from[1], .z = from[2], .w = from[3]},
                       (WPE_Quat){.x = to[0], .y = to[1], .z = to[2], .w = to[3]}, 0.5f);
    const WPE_Quat blended = ((WPE_Transform*)wpe_ecs_get_component(ecs, node, WPE_COMP_TRANSFORM))->rotation;
    WPE_TEST_ASSERT_NEAR(1.0, quat_alignment(blended, expected.v), 1e-5);
    WPE_TEST_ASSERT(quat_alignment(blended, to) < 0.9999f); /* Not the new pose alone yet. */

    /* Done: the new animation alone. */
    WPE_TEST_ASSERT_OK(wpe_animation_update(ecs, 0.6f));
    WPE_TEST_ASSERT_NEAR(0.0, animator->fade_duration, 0);
    wpe_animation_sample_channel(channel, 1.1f, to);
    const WPE_Quat after = ((WPE_Transform*)wpe_ecs_get_component(ecs, node, WPE_COMP_TRANSFORM))->rotation;
    WPE_TEST_ASSERT_NEAR(1.0, quat_alignment(after, to), 1e-5);

    /* A cut (no fade); a paused animation fades out where it stopped. */
    WPE_TEST_ASSERT_OK(wpe_animator_play(animator, 0, 0.0f, false));
    WPE_TEST_ASSERT(animator->fade_duration == 0.0f && animator->time == 0.0f && !animator->loop);
    animator->playing = false;
    animator->time = 0.4f;
    WPE_TEST_ASSERT_OK(wpe_animator_play(animator, 0, 0.5f, true));
    WPE_TEST_ASSERT_NEAR(0.0, animator->from_speed, 0);
    WPE_TEST_ASSERT_OK(wpe_animation_update(ecs, 0.25f));
    WPE_TEST_ASSERT_NEAR(0.4, animator->from_time, 1e-6);

    /* Arguments, and animations by name. */
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_animator_play(animator, 99, 0.0f, true));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_animator_play(animator, 0, -1.0f, true));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NULL_ARG, wpe_animator_play(NULL, 0, 0.0f, true));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_model_find_animation(model, model->animations[0].name));
    WPE_TEST_ASSERT_EQ_UINT(WPE_MODEL_NONE, wpe_model_find_animation(model, "no such animation"));

    wpe_ecs_destroy(ecs);
    wpe_asset_release(t.assets, handle);
    wpe_test_assets_destroy(&t);
}

/* The channel of `animation` that moves `node` along `path`, or NULL. */
static const WPE_AnimationChannel* find_channel(const WPE_Model* model, uint32_t animation, uint32_t node,
                                                WPE_AnimationPath path) {
    const WPE_Animation* a = &model->animations[animation];
    for (uint32_t c = 0; c < a->channel_count; ++c) {
        const WPE_AnimationChannel* channel = &model->channels[a->first_channel + c];
        if (channel->node == node && channel->path == path) return channel;
    }
    return NULL;
}

static void test_graphics_animation_crossfade_between_clips(void) {
    /* Beowulf: 24 clips, some a single held pose (duration 0). */
    WPE_TestAssets t = wpe_test_assets_create(WPE_TEST_ASSETS_DIR, 0);
    const WPE_AssetHandle handle = wpe_test_load_model(t.assets, "meshes/Beowulf.glb");
    const WPE_Model* model = wpe_asset_model(t.assets, handle);
    WPE_TEST_ASSERT_EQ_UINT(24, model->animation_count);
    const uint32_t run = wpe_model_find_animation(model, "Run");
    const uint32_t idle = wpe_model_find_animation(model, "Idle");
    const uint32_t slash = wpe_model_find_animation(model, "Slash0");
    WPE_TEST_ASSERT(run != WPE_MODEL_NONE && idle != WPE_MODEL_NONE && slash != WPE_MODEL_NONE);
    WPE_TEST_ASSERT_NEAR(0.0, model->animations[idle].duration, 0);
    WPE_TEST_ASSERT(model->animations[run].duration > 0.5f);

    WPE_Ecs* ecs = make_world(t.assets);
    WPE_Entity root = WPE_ENTITY_NULL;
    WPE_TEST_ASSERT_OK(wpe_model_instantiate(
        ecs, t.assets, handle, &(WPE_InstantiateOptions){.animation = run, .play = true}, &root));
    WPE_Animator* animator = wpe_ecs_get_component(ecs, root, WPE_COMP_ANIMATOR);

    /* A node both clips rotate, posed differently by them. */
    const WPE_AnimationChannel* run_channel = NULL;
    const WPE_AnimationChannel* idle_channel = NULL;
    for (uint32_t node = 0; node < model->node_count && !run_channel; ++node) {
        const WPE_AnimationChannel* a = find_channel(model, run, node, WPE_ANIM_ROTATION);
        const WPE_AnimationChannel* b = find_channel(model, idle, node, WPE_ANIM_ROTATION);
        if (!a || !b) continue;
        float va[4], vb[4];
        wpe_animation_sample_channel(a, 0.4f, va);
        wpe_animation_sample_channel(b, 0.0f, vb);
        if (quat_alignment((WPE_Quat){.x = va[0], .y = va[1], .z = va[2], .w = va[3]}, vb) < 0.999f) {
            run_channel = a;
            idle_channel = b;
        }
    }
    WPE_TEST_ASSERT(run_channel != NULL);
    const WPE_Entity node = animator->node_entities[run_channel->node];
    const WPE_Transform* transform = wpe_ecs_get_component(ecs, node, WPE_COMP_TRANSFORM);

    /* Running 0.2 s, then a 0.4 s fade into the held Idle pose: at 0.2 s into it, half of each. */
    WPE_TEST_ASSERT_OK(wpe_animation_update(ecs, 0.2f));
    WPE_TEST_ASSERT_OK(wpe_animator_play(animator, idle, 0.4f, true));
    WPE_TEST_ASSERT_OK(wpe_animation_update(ecs, 0.2f));
    float from[4], to[4];
    wpe_animation_sample_channel(run_channel, 0.4f, from);
    wpe_animation_sample_channel(idle_channel, 0.0f, to);
    const WPE_Quat half = wpe_quat_slerp((WPE_Quat){.x = from[0], .y = from[1], .z = from[2], .w = from[3]},
                                         (WPE_Quat){.x = to[0], .y = to[1], .z = to[2], .w = to[3]}, 0.5f);
    WPE_TEST_ASSERT_NEAR(1.0, quat_alignment(transform->rotation, half.v), 1e-4);

    /* After it, the held pose: time stays 0 and it keeps "playing". */
    WPE_TEST_ASSERT_OK(wpe_animation_update(ecs, 0.3f));
    WPE_TEST_ASSERT_NEAR(0.0, animator->fade_duration, 0);
    WPE_TEST_ASSERT_NEAR(0.0, animator->time, 0);
    WPE_TEST_ASSERT(animator->playing);
    WPE_TEST_ASSERT_NEAR(1.0, quat_alignment(transform->rotation, to), 1e-5);

    /* A one-shot slash ends on its last frame and stops. */
    WPE_TEST_ASSERT_OK(wpe_animator_play(animator, slash, 0.1f, false));
    const float length = model->animations[slash].duration;
    WPE_TEST_ASSERT_OK(wpe_animation_update(ecs, length * 0.5f));
    WPE_TEST_ASSERT(animator->playing);
    WPE_TEST_ASSERT_OK(wpe_animation_update(ecs, length));
    WPE_TEST_ASSERT(!animator->playing);
    WPE_TEST_ASSERT_NEAR(length, animator->time, 0);

    wpe_ecs_destroy(ecs);
    wpe_asset_release(t.assets, handle);
    wpe_test_assets_destroy(&t);
}

/* ---- Instantiation and animation playback --------------------------------------------------------- */

static void test_graphics_instantiate_rigged_figure(void) {
    WPE_TestAssets t = wpe_test_assets_create(WPE_TEST_ASSETS_DIR, 0);
    const WPE_AssetHandle handle = wpe_test_load_model(t.assets, "meshes/RiggedFigure.glb");
    const WPE_Model* model = wpe_asset_model(t.assets, handle);
    WPE_Ecs* ecs = make_world(t.assets);

    WPE_Entity root = WPE_ENTITY_NULL;
    WPE_TEST_ASSERT_OK(wpe_model_instantiate(ecs, t.assets, handle,
                                             &(WPE_InstantiateOptions){.animation = 0, .play = true}, &root));
    WPE_TEST_ASSERT_EQ_UINT(model->node_count + 1, wpe_ecs_entity_count(ecs));
    /* The caller's reference plus one per mesh renderer and one for the animator. */
    WPE_TEST_ASSERT_EQ_UINT(3, wpe_asset_ref_count(t.assets, handle));
    WPE_TEST_ASSERT_EQ_STR("model", wpe_scene_get_name(ecs, root));

    WPE_EcsPoolView meshes;
    WPE_TEST_ASSERT_OK(wpe_ecs_pool_view(ecs, WPE_COMP_MESH, &meshes));
    WPE_TEST_ASSERT_EQ_UINT(1, meshes.count);
    const WPE_MeshRenderer* mesh = meshes.data;
    WPE_TEST_ASSERT_EQ_UINT(0, mesh->skin);
    WPE_TEST_ASSERT_EQ_UINT(19, mesh->joint_count);
    for (uint32_t j = 0; j < mesh->joint_count; ++j)
        WPE_TEST_ASSERT(wpe_ecs_is_alive(ecs, mesh->joints[j]));

    /* Every node entity sits under the root. */
    WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_TRANSFORM)});
    while (wpe_ecs_iter_next(&it)) {
        if (it.entity != root) WPE_TEST_ASSERT(wpe_scene_is_ancestor(ecs, root, it.entity));
    }

    /* Playback moves the joints; looping wraps the time. */
    const WPE_Entity joint = mesh->joints[5];
    WPE_TEST_ASSERT_OK(wpe_animation_update(ecs, 0.0f));
    const WPE_Quat before = ((WPE_Transform*)wpe_ecs_get_component(ecs, joint, WPE_COMP_TRANSFORM))->rotation;
    WPE_TEST_ASSERT_OK(wpe_animation_update(ecs, 0.4f));
    const WPE_Quat after = ((WPE_Transform*)wpe_ecs_get_component(ecs, joint, WPE_COMP_TRANSFORM))->rotation;
    WPE_TEST_ASSERT(fabsf(before.x - after.x) + fabsf(before.y - after.y) + fabsf(before.z - after.z) >
                    1e-4f);
    WPE_Animator* animator = wpe_ecs_get_component(ecs, root, WPE_COMP_ANIMATOR);
    const float duration = model->animations[0].duration;
    WPE_TEST_ASSERT_OK(wpe_animation_update(ecs, duration * 2.5f));
    WPE_TEST_ASSERT(animator->time >= 0.0f && animator->time < duration);
    WPE_TEST_ASSERT(animator->playing);

    /* Non-looping playback clamps at the end and stops; backwards stops at zero. */
    animator->loop = false;
    animator->time = 0.0f;
    WPE_TEST_ASSERT_OK(wpe_animation_update(ecs, duration + 1.0f));
    WPE_TEST_ASSERT_NEAR(duration, animator->time, 0);
    WPE_TEST_ASSERT(!animator->playing);
    animator->playing = true;
    animator->speed = -1.0f;
    WPE_TEST_ASSERT_OK(wpe_animation_update(ecs, duration * 3.0f));
    WPE_TEST_ASSERT_NEAR(0.0, animator->time, 0);
    WPE_TEST_ASSERT(!animator->playing);

    /* Destroying the instance releases joint/node arrays and model references through the hooks. */
    WPE_TEST_ASSERT_OK(wpe_scene_destroy_recursive(ecs, root));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_ecs_entity_count(ecs));
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_asset_ref_count(t.assets, handle));
    wpe_ecs_destroy(ecs);
    wpe_asset_release(t.assets, handle);
    WPE_TEST_ASSERT(!wpe_asset_is_valid(t.assets, handle)); /* The last reference frees the model. */
    wpe_test_assets_destroy(&t);
}

static void test_graphics_instantiate_validates_and_cleans_up(void) {
    WPE_TestAssets t = wpe_test_assets_create(WPE_TEST_ASSETS_DIR, 0);
    const WPE_AssetHandle handle = wpe_test_load_model(t.assets, "meshes/RiggedFigure.glb");
    const WPE_Model* model = wpe_asset_model(t.assets, handle);
    WPE_Ecs* ecs = make_world(t.assets);
    WPE_Entity root = WPE_ENTITY_NULL;
    wpe_log_set_stderr_enabled(false);
    WPE_TEST_ASSERT_EQ_INT(
        WPE_RESULT_ERROR_INVALID_ARG,
        wpe_model_instantiate(ecs, t.assets, handle, &(WPE_InstantiateOptions){.animation = 7}, &root));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG,
                           wpe_model_instantiate(ecs, t.assets, WPE_ASSET_NONE, NULL, &root));
    wpe_log_set_stderr_enabled(true);
    WPE_TEST_ASSERT(root == WPE_ENTITY_NULL);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_ecs_entity_count(ecs));
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_asset_ref_count(t.assets, handle)); /* Failed attempts hold nothing. */
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NULL_ARG, wpe_model_instantiate(ecs, NULL, handle, NULL, &root));

    /* Instances are independent; destroying the world with live instances releases everything, and
     * the caller may drop its reference while instances still use the model. */
    WPE_TEST_ASSERT_OK(wpe_model_instantiate(ecs, t.assets, handle, NULL, &root));
    WPE_TEST_ASSERT_OK(
        wpe_model_instantiate(ecs, t.assets, handle, &(WPE_InstantiateOptions){.animation = 0}, &root));
    WPE_TEST_ASSERT_EQ_UINT(2 * (model->node_count + 1), wpe_ecs_entity_count(ecs));
    wpe_asset_release(t.assets, handle);
    WPE_TEST_ASSERT(wpe_asset_is_valid(t.assets, handle));
    wpe_ecs_destroy(ecs);
    WPE_TEST_ASSERT(!wpe_asset_is_valid(t.assets, handle));
    wpe_test_assets_destroy(&t);
}

/* ---- Skinning (vertex shader) --------------------------------------------------------------------- */

static void test_graphics_material_vertex_shader_skinning(void) {
    const WPE_Material material = {.workflow = WPE_MATERIAL_UNLIT, .diffuse_factor = {{1, 1, 1, 1}}};
    WPE_Mat4 palette[2] = {wpe_mat4_identity(),
                           wpe_mat4_from_trs(wpe_vec3(10, 0, 0), wpe_quat_identity(), wpe_vec3(1, 1, 1))};
    WPE_MaterialUniforms u = {.mvp = wpe_mat4_identity(),
                              .world = wpe_mat4_identity(),
                              .normal_matrix = wpe_mat4_identity(),
                              .material = &material,
                              .joint_palette = palette,
                              .joint_count = 2};
    WPE_MeshVertex v = {.position = {{1, 2, 3}},
                        .normal = {{0, 1, 0}},
                        .joints = {0, 1, 0, 0},
                        .weights = {0.25f, 0.75f, 0, 0}};
    WPE_Vec4 clip;
    float varyings[WPE_MAX_VARYINGS];
    wpe_shader_material()->vertex(&u, &v, &clip, varyings);
    WPE_TEST_ASSERT_NEAR(1 + 7.5, clip.x, 1e-5); /* 0.25 * 1 + 0.75 * 11 */
    WPE_TEST_ASSERT_NEAR(2.0, clip.y, 1e-6);

    /* Out-of-range joint indices are ignored rather than read out of bounds. */
    v.joints[1] = 9;
    wpe_shader_material()->vertex(&u, &v, &clip, varyings);
    WPE_TEST_ASSERT_NEAR(1.0, clip.x, 1e-6);

    /* Without a palette the vertex is untouched. */
    u.joint_palette = NULL;
    wpe_shader_material()->vertex(&u, &v, &clip, varyings);
    WPE_TEST_ASSERT_NEAR(1.0, clip.x, 1e-6);
    WPE_TEST_ASSERT_NEAR(3.0, clip.z, 1e-6);
}

static void test_graphics_shininess_range(void) {
    WPE_TEST_ASSERT_NEAR(2.0, wpe_material_shininess(0.0f), 1e-4);
    WPE_TEST_ASSERT_NEAR(2048.0, wpe_material_shininess(1.0f), 1e-1);
    WPE_TEST_ASSERT_NEAR(2.0, wpe_material_shininess(-5.0f), 1e-4);
}

/* ---- Scene rendering ------------------------------------------------------------------------------ */

static WPE_Entity spawn_camera(WPE_Ecs* ecs, WPE_Vec3 position, WPE_Quat rotation) {
    const WPE_Entity camera = wpe_ecs_spawn(ecs);
    WPE_Transform t = wpe_transform_identity();
    t.position = position;
    t.rotation = rotation;
    const WPE_Camera c = wpe_camera_default();
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, camera, WPE_COMP_TRANSFORM, &t, NULL));
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, camera, WPE_COMP_CAMERA, &c, NULL));
    return camera;
}

/* ---- Mesh options ---------------------------------------------------------------------------------------
 */

static uint32_t count_color(const WPE_Framebuffer* fb, WPE_Color color);
static const WPE_Framebuffer* render_scene(WPE_Renderer* renderer, WPE_SceneRenderer* sr, WPE_Ecs* ecs,
                                           WPE_Entity camera, WPE_SceneRenderStats* stats);

static WPE_Entity spawn_mesh(WPE_Ecs* ecs, WPE_AssetHandle model, WPE_Vec3 position) {
    const WPE_Entity e = wpe_ecs_spawn(ecs);
    WPE_Transform t = wpe_transform_identity();
    t.position = position;
    const WPE_MeshRenderer mesh = wpe_mesh_renderer_default(model, 0);
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, e, WPE_COMP_TRANSFORM, &t, NULL));
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, e, WPE_COMP_MESH, &mesh, NULL));
    return e;
}

/* The framebuffer pixel showing a world point (the point must be in view). */
static WPE_Color pixel_at(const WPE_Framebuffer* fb, const WPE_Ecs* ecs, WPE_Entity camera, WPE_Vec3 p) {
    WPE_Mat4 view, projection;
    WPE_TEST_ASSERT(
        wpe_scene_camera_matrices(ecs, camera, (float)fb->width / (float)fb->height, &view, &projection));
    const WPE_Mat4 vp = wpe_mat4_mul(&projection, &view);
    const WPE_Vec4 clip = wpe_mat4_mul_vec4(&vp, wpe_vec4(p.x, p.y, p.z, 1.0f));
    const uint32_t x = (uint32_t)((clip.x / clip.w * 0.5f + 0.5f) * (float)fb->width);
    const uint32_t y = (uint32_t)((0.5f - clip.y / clip.w * 0.5f) * (float)fb->height);
    WPE_TEST_ASSERT(x < fb->width && y < fb->height);
    return fb->color[y * fb->width + x];
}

static WPE_MeshRenderer* mesh_of(WPE_Ecs* ecs, WPE_Entity e) {
    return wpe_ecs_get_component(ecs, e, WPE_COMP_MESH);
}

static void test_graphics_mesh_shadow_unlit_and_sides(void) {
    WPE_TestAssets t = wpe_test_assets_create(NULL, 0);
    const WPE_Material gray = wpe_material_solid(wpe_vec4(0.8f, 0.8f, 0.8f, 1), 0.0f, 1.0f);
    WPE_Model* plane = NULL;
    WPE_Model* cube = NULL;
    WPE_TEST_ASSERT_OK(wpe_model_create_plane(12.0f, 1, &gray, &plane));
    WPE_TEST_ASSERT_OK(wpe_model_create_box(wpe_vec3(1, 1, 1), &gray, &cube));
    const WPE_AssetHandle plane_asset = wpe_test_add_model(t.assets, "test/plane", plane);
    const WPE_AssetHandle cube_asset = wpe_test_add_model(t.assets, "test/cube", cube);
    WPE_Ecs* ecs = make_world(t.assets);
    const WPE_Entity ground = spawn_mesh(ecs, plane_asset, wpe_vec3(0, 0, 0));
    const WPE_Entity box = spawn_mesh(ecs, cube_asset, wpe_vec3(0, 2, 0));
    /* A sun going down and toward +X: the box's shadow falls about two units right of it. */
    const WPE_Entity sun = wpe_ecs_spawn(ecs);
    WPE_Transform st = wpe_transform_identity();
    const WPE_Quat down = wpe_quat_from_axis_angle(wpe_vec3(1, 0, 0), -WPE_PI * 0.5f);
    st.rotation = wpe_quat_mul(wpe_quat_from_axis_angle(wpe_vec3(0, 0, 1), WPE_PI * 0.25f), down);
    WPE_Light light = wpe_light_default(WPE_LIGHT_DIRECTIONAL);
    light.cast_shadows = true;
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, sun, WPE_COMP_TRANSFORM, &st, NULL));
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, sun, WPE_COMP_LIGHT, &light, NULL));
    const WPE_Entity camera = spawn_camera(ecs, wpe_vec3(0, 9, 0), down); /* Looking straight down. */

    WPE_Renderer* renderer = NULL;
    WPE_TEST_ASSERT_OK(wpe_renderer_create(&(WPE_RendererDesc){.width = 64, .height = 48}, &renderer));
    WPE_SceneRenderer* sr = NULL;
    WPE_TEST_ASSERT_OK(wpe_scene_renderer_create(&sr));
    WPE_SceneRenderStats stats;
    const WPE_Vec3 shadowed = wpe_vec3(2, 0, 0), open = wpe_vec3(-2.5f, 0, 0);

    const WPE_Framebuffer* fb = render_scene(renderer, sr, ecs, camera, &stats);
    const WPE_Color lit = pixel_at(fb, ecs, camera, open);
    WPE_TEST_ASSERT(pixel_at(fb, ecs, camera, shadowed) != lit); /* The box's shadow. */
    /* A box that casts no shadow, or a ground that receives none: no shadow there. */
    mesh_of(ecs, box)->cast_shadows = false;
    fb = render_scene(renderer, sr, ecs, camera, &stats);
    WPE_TEST_ASSERT_EQ_UINT(lit, pixel_at(fb, ecs, camera, shadowed));
    mesh_of(ecs, box)->cast_shadows = true;
    mesh_of(ecs, ground)->receive_shadows = false;
    fb = render_scene(renderer, sr, ecs, camera, &stats);
    WPE_TEST_ASSERT_EQ_UINT(lit, pixel_at(fb, ecs, camera, shadowed));

    /* Unlit: exactly the material's color in the open, still shadowed where the box's shadow falls. */
    mesh_of(ecs, ground)->receive_shadows = true;
    mesh_of(ecs, ground)->unlit = true;
    fb = render_scene(renderer, sr, ecs, camera, &stats);
    const WPE_Color base = wpe_color_from_vec4(wpe_vec4(0.8f, 0.8f, 0.8f, 1));
    WPE_TEST_ASSERT_EQ_UINT(base, pixel_at(fb, ecs, camera, open));
    const WPE_Color dark = pixel_at(fb, ecs, camera, shadowed);
    WPE_TEST_ASSERT(dark != base && (dark & 0xFF) < (base & 0xFF));
    mesh_of(ecs, ground)->receive_shadows = false;
    fb = render_scene(renderer, sr, ecs, camera, &stats);
    WPE_TEST_ASSERT_EQ_UINT(base, pixel_at(fb, ecs, camera, shadowed));

    /* From below, a single-sided ground is culled; a double-sided one is drawn. */
    ((WPE_Transform*)wpe_ecs_get_component(ecs, camera, WPE_COMP_TRANSFORM))->position = wpe_vec3(0, -9, 0);
    ((WPE_Transform*)wpe_ecs_get_component(ecs, camera, WPE_COMP_TRANSFORM))->rotation =
        wpe_quat_from_axis_angle(wpe_vec3(1, 0, 0), WPE_PI * 0.5f);
    mesh_of(ecs, box)->visible = false;
    fb = render_scene(renderer, sr, ecs, camera, &stats);
    WPE_TEST_ASSERT_EQ_UINT(0, count_color(fb, base));
    mesh_of(ecs, ground)->double_sided = true;
    fb = render_scene(renderer, sr, ecs, camera, &stats);
    WPE_TEST_ASSERT(count_color(fb, base) > 1000);

    wpe_scene_renderer_destroy(sr);
    wpe_renderer_destroy(renderer);
    wpe_ecs_destroy(ecs);
    wpe_asset_release(t.assets, plane_asset);
    wpe_asset_release(t.assets, cube_asset);
    wpe_test_assets_destroy(&t);
}

/* ---- Billboards ------------------------------------------------------------------------------------- */

static WPE_Mat4 translation(WPE_Vec3 v) {
    return wpe_mat4_from_trs(v, wpe_quat_identity(), wpe_vec3(1, 1, 1));
}

static void assert_vec3(WPE_Vec3 expected, WPE_Vec3 actual) {
    for (int i = 0; i < 3; ++i)
        WPE_TEST_ASSERT_NEAR(expected.v[i], actual.v[i], 1e-5);
}

static void test_graphics_billboard_corners(void) {
    const WPE_Mat4 world = translation(wpe_vec3(1, 2, 0));
    const WPE_Mat4 camera = translation(wpe_vec3(1, 2, 5)); /* Looking down -Z at it. */
    WPE_Billboard b = wpe_billboard_default();
    WPE_Vec3 c[4], n;
    /* Screen-aligned, centered: counter-clockwise from the bottom-left, facing the camera. */
    WPE_TEST_ASSERT(wpe_billboard_corners(&b, &world, &camera, c, &n));
    assert_vec3(wpe_vec3(0.5f, 1.5f, 0), c[0]);
    assert_vec3(wpe_vec3(1.5f, 1.5f, 0), c[1]);
    assert_vec3(wpe_vec3(1.5f, 2.5f, 0), c[2]);
    assert_vec3(wpe_vec3(0.5f, 2.5f, 0), c[3]);
    assert_vec3(wpe_vec3(0, 0, 1), n);
    /* The pivot is the point at the entity: (0.5, 0) stands the quad on it. Size scales with the entity. */
    b.pivot = (WPE_Vec2){.x = 0.5f, .y = 0.0f};
    b.size = (WPE_Vec2){.x = 2.0f, .y = 1.0f};
    const WPE_Mat4 scaled =
        wpe_mat4_mul(&world, &(WPE_Mat4){.m = {{1, 0, 0, 0}, {0, 3, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}});
    WPE_TEST_ASSERT(wpe_billboard_corners(&b, &scaled, &camera, c, &n));
    assert_vec3(wpe_vec3(0, 2, 0), c[0]);
    assert_vec3(wpe_vec3(2, 5, 0), c[2]);

    /* Upright about world Y: seen from +X (and from above) it stays vertical and faces +X. */
    b = wpe_billboard_default();
    b.mode = WPE_BILLBOARD_AXIS_ALIGNED_Y;
    const WPE_Mat4 high = translation(wpe_vec3(6, 12, 0));
    WPE_TEST_ASSERT(wpe_billboard_corners(&b, &world, &high, c, &n));
    assert_vec3(wpe_vec3(1, 1.5f, 0.5f), c[0]); /* Its right points -Z for a viewer at +X. */
    assert_vec3(wpe_vec3(1, 2.5f, -0.5f), c[2]);
    assert_vec3(wpe_vec3(1, 0, 0), n);
    /* Screen-aligned instead follows the camera's own axes. */
    b.mode = WPE_BILLBOARD_SCREEN_ALIGNED;
    const WPE_Mat4 turned =
        wpe_mat4_mul(&high, &(WPE_Mat4){.m = {{0, 0, -1, 0}, {0, 1, 0, 0}, {1, 0, 0, 0}, {0, 0, 0, 1}}});
    WPE_TEST_ASSERT(wpe_billboard_corners(&b, &world, &turned, c, &n));
    assert_vec3(wpe_vec3(1, 1.5f, 0.5f), c[0]);
    /* Axis-aligned: faces the camera's position, upright, whatever the entity's rotation. Straight ahead it
     * matches the screen-aligned quad even for a tilted entity; from above and in front it tips back to face
     * the camera, its right staying level. */
    b.mode = WPE_BILLBOARD_AXIS_ALIGNED;
    const WPE_Mat4 tilted =
        wpe_mat4_mul(&world, &(WPE_Mat4){.m = {{0, 1, 0, 0}, {-1, 0, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}});
    WPE_TEST_ASSERT(wpe_billboard_corners(&b, &tilted, &camera, c, &n));
    assert_vec3(wpe_vec3(0.5f, 1.5f, 0), c[0]);
    assert_vec3(wpe_vec3(1.5f, 2.5f, 0), c[2]);
    const WPE_Mat4 above = translation(wpe_vec3(1, 7, 5));
    WPE_TEST_ASSERT(wpe_billboard_corners(&b, &tilted, &above, c, &n));
    const float h = 0.35355339f; /* Half a unit along a 45-degree diagonal. */
    assert_vec3(wpe_vec3(0.5f, 2.0f - h, h), c[0]);
    assert_vec3(wpe_vec3(1.5f, 2.0f + h, -h), c[2]);
    assert_vec3(wpe_vec3(0, 0.70710678f, 0.70710678f), n);
    /* Straight above, the camera's right decides the turn and the quad lies flat, facing up. */
    const WPE_Mat4 overhead = translation(wpe_vec3(1, 9, 0));
    WPE_TEST_ASSERT(wpe_billboard_corners(&b, &world, &overhead, c, &n));
    assert_vec3(wpe_vec3(0, 1, 0), n);
    /* Fixed: the entity's XY plane, whatever the camera does; rotation turns it within its plane. */
    b.mode = WPE_BILLBOARD_FIXED;
    WPE_TEST_ASSERT(wpe_billboard_corners(&b, &world, &high, c, &n));
    assert_vec3(wpe_vec3(0.5f, 1.5f, 0), c[0]);
    assert_vec3(wpe_vec3(0, 0, 1), n);
    b.rotation = WPE_PI * 0.5f;
    WPE_TEST_ASSERT(wpe_billboard_corners(&b, &world, &high, c, &n));
    assert_vec3(wpe_vec3(1.5f, 1.5f, 0), c[0]); /* A quarter turn counter-clockwise. */
    /* Empty quads have no corners. */
    b.size.x = 0.0f;
    WPE_TEST_ASSERT(!wpe_billboard_corners(&b, &world, &camera, c, &n));
}

static void test_graphics_billboard_animation(void) {
    WPE_TestAssets t = wpe_test_assets_create(NULL, 0);
    WPE_Ecs* ecs = make_world(t.assets);
    WPE_Billboard b = wpe_billboard_default();
    b.columns = 4;
    b.fps = 10.0f;
    const WPE_Entity looping = wpe_ecs_spawn(ecs);
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, looping, WPE_COMP_BILLBOARD, &b, NULL));
    b.loop = false;
    b.frame_count = 3;
    const WPE_Entity once = wpe_ecs_spawn(ecs);
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, once, WPE_COMP_BILLBOARD, &b, NULL));
    b.fps = 0.0f;
    const WPE_Entity still = wpe_ecs_spawn(ecs);
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, still, WPE_COMP_BILLBOARD, &b, NULL));

    wpe_billboard_update(ecs, 0.25f); /* 2.5 frames: two whole frames, half of the next one kept. */
    const WPE_Billboard* l = wpe_ecs_get_component(ecs, looping, WPE_COMP_BILLBOARD);
    WPE_TEST_ASSERT_EQ_UINT(2, l->frame);
    WPE_TEST_ASSERT_NEAR(0.05, l->frame_time, 1e-5);
    wpe_billboard_update(ecs, 0.2f); /* 4 and a half frames in all: the loop wraps to frame 0. */
    WPE_TEST_ASSERT_EQ_UINT(
        0, ((const WPE_Billboard*)wpe_ecs_get_component(ecs, looping, WPE_COMP_BILLBOARD))->frame);
    /* Without looping it stops on its last frame (frame_count 3 of the 4 cells). */
    WPE_TEST_ASSERT_EQ_UINT(
        2, ((const WPE_Billboard*)wpe_ecs_get_component(ecs, once, WPE_COMP_BILLBOARD))->frame);
    WPE_TEST_ASSERT_EQ_UINT(
        0, ((const WPE_Billboard*)wpe_ecs_get_component(ecs, still, WPE_COMP_BILLBOARD))->frame);
    wpe_ecs_destroy(ecs);
    wpe_test_assets_destroy(&t);
}

static uint32_t count_color(const WPE_Framebuffer* fb, WPE_Color color);

/* Renders the world from `camera` into a black frame; returns the frame. */
static const WPE_Framebuffer* render_scene(WPE_Renderer* renderer, WPE_SceneRenderer* sr, WPE_Ecs* ecs,
                                           WPE_Entity camera, WPE_SceneRenderStats* stats) {
    WPE_TEST_ASSERT_OK(wpe_scene_update_transforms(ecs));
    WPE_TEST_ASSERT_OK(wpe_renderer_begin_frame(
        renderer,
        &(WPE_ClearDesc){.clear_color = true, .color = 0xFF000000u, .clear_depth = true, .depth = 1.0f}));
    WPE_TEST_ASSERT_OK(wpe_scene_render(sr, renderer, ecs, camera, NULL, stats));
    WPE_TEST_ASSERT_OK(wpe_renderer_end_frame(renderer));
    return wpe_renderer_framebuffer(renderer);
}

/* ---- Sky -------------------------------------------------------------------------------------------- */

static const WPE_Framebuffer* render_sky(WPE_Renderer* renderer, WPE_SceneRenderer* sr, WPE_Ecs* ecs,
                                         WPE_Entity camera, const WPE_SceneRenderSettings* settings) {
    WPE_TEST_ASSERT_OK(wpe_scene_update_transforms(ecs));
    WPE_TEST_ASSERT_OK(wpe_renderer_begin_frame(
        renderer,
        &(WPE_ClearDesc){.clear_color = true, .color = 0xFF000000u, .clear_depth = true, .depth = 1.0f}));
    WPE_TEST_ASSERT_OK(wpe_scene_render(sr, renderer, ecs, camera, settings, NULL));
    WPE_TEST_ASSERT_OK(wpe_renderer_end_frame(renderer));
    return wpe_renderer_framebuffer(renderer);
}

static WPE_Color center_pixel(const WPE_Framebuffer* fb) {
    return fb->color[(fb->height / 2) * fb->width + fb->width / 2];
}

/* Within 2 per channel (rays through pixel centers are not exactly level). */
static bool near_color(WPE_Color a, WPE_Vec3 expected) {
    const WPE_Color b = wpe_color_from_vec4(wpe_vec4(expected.x, expected.y, expected.z, 1.0f));
    for (int shift = 0; shift < 32; shift += 8) {
        const int d = (int)((a >> shift) & 0xFFu) - (int)((b >> shift) & 0xFFu);
        if (d < -2 || d > 2) return false;
    }
    return true;
}

static void test_graphics_sky_gradient_sun_and_occlusion(void) {
    WPE_TestAssets t = wpe_test_assets_create(NULL, 0);
    WPE_Ecs* ecs = make_world(t.assets);
    /* An odd height puts the middle row's pixel centers exactly level. */
    WPE_Renderer* renderer = NULL;
    WPE_TEST_ASSERT_OK(wpe_renderer_create(&(WPE_RendererDesc){.width = 64, .height = 47}, &renderer));
    WPE_SceneRenderer* sr = NULL;
    WPE_TEST_ASSERT_OK(wpe_scene_renderer_create(&sr));
    WPE_SceneRenderSettings settings = wpe_scene_render_settings_default();
    settings.sky.mode = WPE_SKY_GRADIENT;
    settings.sky.zenith = wpe_vec3(0.0f, 0.0f, 1.0f);
    settings.sky.horizon = wpe_vec3(0.0f, 1.0f, 0.0f);
    settings.sky.ground = wpe_vec3(1.0f, 0.0f, 0.0f);
    settings.default_sun_direction = wpe_vec3(0.0f, -1.0f, 0.0f); /* Straight down: out of the level views. */

    /* Level: the horizon at the middle, the zenith color above, ground below. */
    const WPE_Entity camera = spawn_camera(ecs, wpe_vec3(0, 0, 0), wpe_quat_identity());
    const WPE_Framebuffer* fb = render_sky(renderer, sr, ecs, camera, &settings);
    WPE_TEST_ASSERT(near_color(center_pixel(fb), settings.sky.horizon));
    const WPE_Color top = fb->color[fb->width / 2],
                    bottom = fb->color[(fb->height - 1) * fb->width + fb->width / 2];
    WPE_TEST_ASSERT(((top >> 0) & 0xFFu) > 0x40u && ((top >> 16) & 0xFFu) == 0);       /* Blue, no red. */
    WPE_TEST_ASSERT(((bottom >> 16) & 0xFFu) > 0x40u && ((bottom >> 0) & 0xFFu) == 0); /* Red, no blue. */
    /* Every pixel of a game-sized frame, at any angle: no holes where depths round past the far plane. */
    WPE_Renderer* large = NULL;
    WPE_TEST_ASSERT_OK(wpe_renderer_create(&(WPE_RendererDesc){.width = 320, .height = 240}, &large));
    for (int degrees = 0; degrees <= 90; degrees += 10) {
        ((WPE_Transform*)wpe_ecs_get_component(ecs, camera, WPE_COMP_TRANSFORM))->rotation =
            wpe_quat_mul(wpe_quat_from_axis_angle(wpe_vec3(0, 1, 0), (float)degrees * 0.02f),
                         wpe_quat_from_axis_angle(wpe_vec3(1, 0, 0), (float)degrees * (WPE_PI / 180.0f)));
        WPE_TEST_ASSERT_MSG(count_color(render_sky(large, sr, ecs, camera, &settings), 0xFF000000u) == 0,
                            "holes at %d degrees", degrees);
    }
    wpe_renderer_destroy(large);
    /* Straight up the zenith, straight down the ground. */
    ((WPE_Transform*)wpe_ecs_get_component(ecs, camera, WPE_COMP_TRANSFORM))->rotation =
        wpe_quat_from_axis_angle(wpe_vec3(1, 0, 0), WPE_PI * 0.5f);
    WPE_TEST_ASSERT(
        near_color(center_pixel(render_sky(renderer, sr, ecs, camera, &settings)), settings.sky.zenith));

    /* The sun's disc where its light comes from (here, the sun shining down from overhead). */
    settings.sky.sun_size = 5.0f;
    settings.sky.sun_color = wpe_vec3(1.0f, 0.0f, 1.0f);
    WPE_TEST_ASSERT(
        near_color(center_pixel(render_sky(renderer, sr, ecs, camera, &settings)), settings.sky.sun_color));
    settings.default_sun = false; /* No sun, no disc. */
    WPE_TEST_ASSERT(
        near_color(center_pixel(render_sky(renderer, sr, ecs, camera, &settings)), settings.sky.zenith));
    settings.default_sun = true;
    ((WPE_Transform*)wpe_ecs_get_component(ecs, camera, WPE_COMP_TRANSFORM))->rotation =
        wpe_quat_from_axis_angle(wpe_vec3(1, 0, 0), -WPE_PI * 0.5f);
    WPE_TEST_ASSERT(
        near_color(center_pixel(render_sky(renderer, sr, ecs, camera, &settings)), settings.sky.ground));

    /* Geometry covers the sky: a white unlit box in front of a level view. */
    const WPE_Material white = wpe_material_solid(wpe_vec4(1, 1, 1, 1), 0.0f, 1.0f);
    WPE_Model* cube = NULL;
    WPE_TEST_ASSERT_OK(wpe_model_create_box(wpe_vec3(1, 1, 1), &white, &cube));
    const WPE_Entity box =
        spawn_mesh(ecs, wpe_test_add_model(t.assets, "test/cube", cube), wpe_vec3(0, 0, -4));
    mesh_of(ecs, box)->unlit = true;
    ((WPE_Transform*)wpe_ecs_get_component(ecs, camera, WPE_COMP_TRANSFORM))->rotation = wpe_quat_identity();
    fb = render_sky(renderer, sr, ecs, camera, &settings);
    WPE_TEST_ASSERT_EQ_UINT(0xFFFFFFFFu, center_pixel(fb));
    WPE_TEST_ASSERT(near_color(fb->color[(fb->height / 2) * fb->width + 1], settings.sky.horizon));

    /* The color mode draws nothing: the clear color stays. */
    settings.sky.mode = WPE_SKY_COLOR;
    fb = render_sky(renderer, sr, ecs, camera, &settings);
    WPE_TEST_ASSERT_EQ_UINT(0xFF000000u, fb->color[(fb->height / 2) * fb->width + 1]);

    wpe_scene_renderer_destroy(sr);
    wpe_renderer_destroy(renderer);
    wpe_ecs_destroy(ecs);
    wpe_test_assets_destroy(&t);
}

static void test_graphics_sky_panorama(void) {
    WPE_TestAssets t = wpe_test_assets_create(NULL, 0);
    WPE_Ecs* ecs = make_world(t.assets);
    WPE_Renderer* renderer = NULL;
    WPE_TEST_ASSERT_OK(wpe_renderer_create(&(WPE_RendererDesc){.width = 64, .height = 48}, &renderer));
    WPE_SceneRenderer* sr = NULL;
    WPE_TEST_ASSERT_OK(wpe_scene_renderer_create(&sr));
    /* 3 x 3 texels: longitude across (the middle column straight ahead, -Z), latitude down. */
    const uint32_t texels[9] = {0xFF000010u, 0xFF000020u, 0xFF000030u, 0xFFFF0000u, 0xFF00FF00u,
                                0xFF0000FFu, 0xFF100000u, 0xFF200000u, 0xFF300000u};
    WPE_Texture* panorama = NULL;
    WPE_TEST_ASSERT_OK(
        wpe_texture_create(&(WPE_TextureDesc){.width = 3, .height = 3, .texels = texels}, &panorama));
    WPE_SceneRenderSettings settings = wpe_scene_render_settings_default();
    settings.sky.mode = WPE_SKY_PANORAMA;
    settings.sky.panorama = panorama;
    const WPE_Entity camera = spawn_camera(ecs, wpe_vec3(0, 0, 0), wpe_quat_identity());
    WPE_TEST_ASSERT_EQ_UINT(0xFF00FF00u, center_pixel(render_sky(renderer, sr, ecs, camera, &settings)));
    /* Looking toward +X: a quarter turn further along the image. */
    WPE_Transform* transform = wpe_ecs_get_component(ecs, camera, WPE_COMP_TRANSFORM);
    transform->rotation = wpe_quat_from_axis_angle(wpe_vec3(0, 1, 0), -WPE_PI * 0.5f);
    WPE_TEST_ASSERT_EQ_UINT(0xFF0000FFu, center_pixel(render_sky(renderer, sr, ecs, camera, &settings)));
    /* Turned a third of a turn, the image moves with it. */
    transform = wpe_ecs_get_component(ecs, camera, WPE_COMP_TRANSFORM);
    transform->rotation = wpe_quat_identity();
    settings.sky.rotation = 120.0f;
    WPE_TEST_ASSERT_EQ_UINT(0xFF0000FFu, center_pixel(render_sky(renderer, sr, ecs, camera, &settings)));
    /* Up the top row, down the bottom one. */
    settings.sky.rotation = 0.0f;
    transform = wpe_ecs_get_component(ecs, camera, WPE_COMP_TRANSFORM);
    transform->rotation = wpe_quat_from_axis_angle(wpe_vec3(1, 0, 0), WPE_PI * 0.45f);
    WPE_TEST_ASSERT_EQ_UINT(0xFF000020u, center_pixel(render_sky(renderer, sr, ecs, camera, &settings)));
    /* Without its image a panorama draws nothing. */
    settings.sky.panorama = NULL;
    WPE_TEST_ASSERT_EQ_UINT(0xFF000000u, center_pixel(render_sky(renderer, sr, ecs, camera, &settings)));

    wpe_texture_destroy(panorama);
    wpe_scene_renderer_destroy(sr);
    wpe_renderer_destroy(renderer);
    wpe_ecs_destroy(ecs);
    wpe_test_assets_destroy(&t);
}

static void test_graphics_billboard_draws_frames_and_blends(void) {
    WPE_TestAssets t = wpe_test_assets_create(NULL, 0);
    /* Two frames side by side: opaque red, then fully transparent. */
    const uint32_t texels[2] = {0xFFFF0000u, 0x00000000u};
    WPE_Texture* texture = NULL;
    WPE_TEST_ASSERT_OK(
        wpe_texture_create(&(WPE_TextureDesc){.width = 2, .height = 1, .texels = texels}, &texture));
    WPE_AssetHandle image = WPE_ASSET_NONE;
    WPE_TEST_ASSERT_OK(wpe_asset_add(t.assets, WPE_ASSET_IMAGE, "test/frames", texture, &image));
    WPE_Ecs* ecs = make_world(t.assets);
    const WPE_Entity camera = spawn_camera(ecs, wpe_vec3(0, 0, 5), wpe_quat_identity());
    WPE_Billboard settings = wpe_billboard_default();
    settings.image = image;
    settings.columns = 2;
    settings.size = (WPE_Vec2){.x = 2, .y = 2};
    const WPE_Entity sprite = wpe_ecs_spawn(ecs);
    const WPE_Transform at = wpe_transform_identity();
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, sprite, WPE_COMP_TRANSFORM, &at, NULL));
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, sprite, WPE_COMP_BILLBOARD, &settings, NULL));
    wpe_asset_release(t.assets, image); /* The billboard holds its own reference. */
    WPE_TEST_ASSERT(wpe_asset_is_valid(t.assets, image));

    WPE_Renderer* renderer = NULL;
    WPE_TEST_ASSERT_OK(wpe_renderer_create(&(WPE_RendererDesc){.width = 64, .height = 48}, &renderer));
    WPE_SceneRenderer* sr = NULL;
    WPE_TEST_ASSERT_OK(wpe_scene_renderer_create(&sr));
    WPE_SceneRenderStats stats;

    /* Frame 0, unlit cutout: exactly the image's red, in the middle of the view. */
    const WPE_Framebuffer* fb = render_scene(renderer, sr, ecs, camera, &stats);
    WPE_TEST_ASSERT_EQ_UINT(1, stats.billboards_submitted);
    const uint32_t red = count_color(fb, 0xFFFF0000u);
    WPE_TEST_ASSERT(red > 100);
    WPE_TEST_ASSERT_EQ_UINT(0xFFFF0000u, fb->color[24 * 64 + 32]);
    /* Frame 1 is transparent: cut out entirely. */
    WPE_Billboard* live = wpe_ecs_get_component(ecs, sprite, WPE_COMP_BILLBOARD);
    live->frame = 1;
    fb = render_scene(renderer, sr, ecs, camera, &stats);
    WPE_TEST_ASSERT_EQ_UINT(0, count_color(fb, 0xFFFF0000u));
    /* Additive: half the tint added to the black background over the same area. */
    live = wpe_ecs_get_component(ecs, sprite, WPE_COMP_BILLBOARD);
    live->frame = 0;
    live->blend = WPE_BILLBOARD_ADDITIVE;
    live->color = wpe_vec4(1, 1, 1, 0.5f);
    fb = render_scene(renderer, sr, ecs, camera, &stats);
    const WPE_Color half = fb->color[24 * 64 + 32];
    WPE_TEST_ASSERT(((half >> 16) & 0xFF) >= 126 && ((half >> 16) & 0xFF) <= 129 && (half & 0xFFFF) == 0);
    /* Lit: the default sun (the world has no lights) brightens it above ambient, never above the image. */
    live = wpe_ecs_get_component(ecs, sprite, WPE_COMP_BILLBOARD);
    live->blend = WPE_BILLBOARD_CUTOUT;
    live->color = wpe_vec4(1, 1, 1, 1);
    live->lit = true;
    fb = render_scene(renderer, sr, ecs, camera, &stats);
    const uint32_t lit_red = (fb->color[24 * 64 + 32] >> 16) & 0xFF;
    WPE_TEST_ASSERT(lit_red > 64 && lit_red < 255);
    /* Behind the camera: culled. Hidden: skipped. */
    live = wpe_ecs_get_component(ecs, sprite, WPE_COMP_BILLBOARD);
    live->lit = false;
    ((WPE_Transform*)wpe_ecs_get_component(ecs, sprite, WPE_COMP_TRANSFORM))->position = wpe_vec3(0, 0, 10);
    render_scene(renderer, sr, ecs, camera, &stats);
    WPE_TEST_ASSERT(stats.billboards_submitted == 0 && stats.billboards_culled == 1);
    live->visible = false;
    render_scene(renderer, sr, ecs, camera, &stats);
    WPE_TEST_ASSERT(stats.billboards_submitted == 0 && stats.billboards_culled == 0);

    wpe_scene_renderer_destroy(sr);
    wpe_renderer_destroy(renderer);
    wpe_ecs_destroy(ecs);
    WPE_TEST_ASSERT(!wpe_asset_is_valid(t.assets, image));
    wpe_test_assets_destroy(&t);
}

/* ---- Particles -------------------------------------------------------------------------------------- */

static WPE_Entity spawn_emitter(WPE_Ecs* ecs, WPE_Vec3 position, const WPE_ParticleEmitter* settings) {
    const WPE_Entity e = wpe_ecs_spawn(ecs);
    WPE_Transform t = wpe_transform_identity();
    t.position = position;
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, e, WPE_COMP_TRANSFORM, &t, NULL));
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, e, WPE_COMP_PARTICLES, settings, NULL));
    WPE_TEST_ASSERT_OK(wpe_scene_update_transforms(ecs));
    return e;
}

static const WPE_ParticleEmitter* emitter_of(WPE_Ecs* ecs, WPE_Entity e) {
    return wpe_ecs_get_component(ecs, e, WPE_COMP_PARTICLES);
}

static void test_graphics_particles_emit_and_expire(void) {
    WPE_TestAssets t = wpe_test_assets_create(NULL, 0);
    WPE_Ecs* ecs = make_world(t.assets);
    WPE_ParticleEmitter settings = wpe_particle_emitter_default();
    settings.rate = 10.0f;
    const WPE_Entity steady = spawn_emitter(ecs, wpe_vec3(0, 0, 0), &settings);
    /* A one-shot: a burst of 4, half a second of emission, no loop. */
    settings.rate = 0.0f;
    settings.burst = 4;
    settings.duration = 0.5f;
    settings.loop = false;
    const WPE_Entity once = spawn_emitter(ecs, wpe_vec3(0, 0, 0), &settings);
    settings.loop = true;
    const WPE_Entity cycles = spawn_emitter(ecs, wpe_vec3(0, 0, 0), &settings);
    settings.max_particles = 3;
    settings.burst = 10;
    const WPE_Entity capped = spawn_emitter(ecs, wpe_vec3(0, 0, 0), &settings);

    WPE_TEST_ASSERT_OK(wpe_particles_update(ecs, NULL, 0.25f));
    WPE_TEST_ASSERT_EQ_UINT(2, emitter_of(ecs, steady)->count); /* 2.5 owed: 2 now, half carried over. */
    WPE_TEST_ASSERT_EQ_UINT(4, emitter_of(ecs, once)->count);
    WPE_TEST_ASSERT_EQ_UINT(4, emitter_of(ecs, cycles)->count);
    WPE_TEST_ASSERT_EQ_UINT(3, emitter_of(ecs, capped)->count); /* At most max_particles. */
    WPE_TEST_ASSERT_OK(wpe_particles_update(ecs, NULL, 0.3f));
    WPE_TEST_ASSERT_EQ_UINT(5, emitter_of(ecs, steady)->count);
    /* At 0.55 seconds the one-shot stopped (its particles live on) and the loop burst again. */
    WPE_TEST_ASSERT(!emitter_of(ecs, once)->playing);
    WPE_TEST_ASSERT_EQ_UINT(4, emitter_of(ecs, once)->count);
    WPE_TEST_ASSERT_EQ_UINT(8, emitter_of(ecs, cycles)->count);
    /* A second later the first particles (1 second of life) are gone. */
    WPE_TEST_ASSERT_OK(wpe_particles_update(ecs, NULL, 0.5f));
    WPE_TEST_ASSERT_OK(wpe_particles_update(ecs, NULL, 0.5f));
    WPE_TEST_ASSERT_EQ_UINT(0, emitter_of(ecs, once)->count);
    WPE_TEST_ASSERT_EQ_UINT(10, emitter_of(ecs, steady)->count); /* A steady state of rate x lifetime. */

    /* Emitting on demand, clearing, restarting. */
    WPE_TEST_ASSERT_EQ_UINT(2, wpe_particles_emit(ecs, once, 2));
    WPE_TEST_ASSERT_EQ_UINT(2, emitter_of(ecs, once)->count);
    wpe_particles_clear(ecs, once);
    WPE_TEST_ASSERT(emitter_of(ecs, once)->count == 0 && !emitter_of(ecs, once)->playing);
    wpe_particles_restart(ecs, once);
    WPE_TEST_ASSERT_OK(wpe_particles_update(ecs, NULL, 0.1f));
    WPE_TEST_ASSERT(emitter_of(ecs, once)->playing && emitter_of(ecs, once)->count == 4);
    /* The editor's preview plays a stopped emitter without changing its settings. */
    wpe_particles_clear(ecs, once);
    ((WPE_ParticleEmitter*)wpe_ecs_get_component(ecs, once, WPE_COMP_PARTICLES))->playing = false;
    WPE_TEST_ASSERT_OK(wpe_particles_preview(ecs, once, 0.1f));
    WPE_TEST_ASSERT_EQ_UINT(4, emitter_of(ecs, once)->count);
    WPE_TEST_ASSERT(!emitter_of(ecs, once)->playing && !emitter_of(ecs, once)->loop);
    /* An emitter copied from a live one shares none of its particles. */
    const WPE_Entity copy = spawn_emitter(ecs, wpe_vec3(0, 0, 0), emitter_of(ecs, steady));
    WPE_TEST_ASSERT(emitter_of(ecs, copy)->count == 0 && emitter_of(ecs, copy)->particles == NULL);
    wpe_ecs_destroy(ecs);
    wpe_test_assets_destroy(&t);
}

static void test_graphics_particles_move(void) {
    WPE_TestAssets t = wpe_test_assets_create(NULL, 0);
    WPE_Ecs* ecs = make_world(t.assets);
    WPE_ParticleEmitter settings = wpe_particle_emitter_default();
    settings.rate = 0.0f;
    settings.speed_min = settings.speed_max = 2.0f;
    settings.spread = 0.0f;
    settings.direction = wpe_vec3(0, 1, 0);
    /* World space, from an emitter at (5, 0, 0) turned a quarter about Z: "up" leaves along -X. */
    const WPE_Entity turned = wpe_ecs_spawn(ecs);
    WPE_Transform tr = wpe_transform_identity();
    tr.position = wpe_vec3(5, 0, 0);
    tr.rotation = wpe_quat_from_axis_angle(wpe_vec3(0, 0, 1), WPE_PI * 0.5f);
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, turned, WPE_COMP_TRANSFORM, &tr, NULL));
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, turned, WPE_COMP_PARTICLES, &settings, NULL));
    WPE_TEST_ASSERT_OK(wpe_scene_update_transforms(ecs));
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_particles_emit(ecs, turned, 1));
    const WPE_Particle* p = &emitter_of(ecs, turned)->particles[0];
    assert_vec3(wpe_vec3(5, 0, 0), p->position);
    assert_vec3(wpe_vec3(-2, 0, 0), p->velocity);
    /* Gravity and drag: velocity first, then position, per step. */
    ((WPE_ParticleEmitter*)wpe_ecs_get_component(ecs, turned, WPE_COMP_PARTICLES))->gravity =
        wpe_vec3(0, -10, 0);
    WPE_TEST_ASSERT_OK(wpe_particles_update(ecs, NULL, 0.1f));
    p = &emitter_of(ecs, turned)->particles[0];
    assert_vec3(wpe_vec3(-2, -1, 0), p->velocity);
    assert_vec3(wpe_vec3(4.8f, -0.1f, 0), p->position);

    /* Local space: particles keep their place relative to the emitter, so moving it moves them. */
    settings.world_space = false;
    const WPE_Entity local = spawn_emitter(ecs, wpe_vec3(0, 3, 0), &settings);
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_particles_emit(ecs, local, 1));
    assert_vec3(wpe_vec3(0, 0, 0), emitter_of(ecs, local)->particles[0].position);
    assert_vec3(wpe_vec3(0, 2, 0), emitter_of(ecs, local)->particles[0].velocity);
    /* Spawn shapes keep to their volume. */
    settings.shape = WPE_PARTICLES_BOX;
    settings.shape_size = wpe_vec3(2, 0, 4);
    const WPE_Entity box = spawn_emitter(ecs, wpe_vec3(0, 0, 0), &settings);
    WPE_TEST_ASSERT_EQ_UINT(100, wpe_particles_emit(ecs, box, 100));
    for (uint32_t i = 0; i < 100; ++i) {
        const WPE_Vec3 at = emitter_of(ecs, box)->particles[i].position;
        WPE_TEST_ASSERT(fabsf(at.x) <= 1.0f && at.y == 0.0f && fabsf(at.z) <= 2.0f);
    }
    wpe_ecs_destroy(ecs);
    wpe_test_assets_destroy(&t);
}

/* The same seed gives the same particles, and worker threads change nothing. */
static void test_graphics_particles_are_deterministic(void) {
    WPE_TestAssets t = wpe_test_assets_create(NULL, 0);
    WPE_JobSystem* jobs = NULL;
    WPE_TEST_ASSERT_OK(wpe_jobs_create(4, &jobs));
    WPE_ParticleEmitter settings = wpe_particle_emitter_default();
    settings.max_particles = 20000;
    settings.rate = 20000.0f;
    settings.burst = 10000;
    settings.lifetime_min = 0.2f;
    settings.lifetime_max = 2.0f;
    settings.spread = 1.0f;
    settings.gravity = wpe_vec3(0, -9.8f, 0);
    settings.drag = 0.3f;
    settings.seed = 42;
    WPE_Ecs* a = make_world(t.assets);
    WPE_Ecs* b = make_world(t.assets);
    const WPE_Entity ea = spawn_emitter(a, wpe_vec3(1, 2, 3), &settings);
    const WPE_Entity eb = spawn_emitter(b, wpe_vec3(1, 2, 3), &settings);
    for (int step = 0; step < 10; ++step) {
        WPE_TEST_ASSERT_OK(wpe_particles_update(a, NULL, 1.0f / 30.0f));
        WPE_TEST_ASSERT_OK(wpe_particles_update(b, jobs, 1.0f / 30.0f));
    }
    const WPE_ParticleEmitter* pa = emitter_of(a, ea);
    const WPE_ParticleEmitter* pb = emitter_of(b, eb);
    WPE_TEST_ASSERT(pa->count > 10000);
    WPE_TEST_ASSERT_EQ_UINT(pa->count, pb->count);
    WPE_TEST_ASSERT(memcmp(pa->particles, pb->particles, (size_t)pa->count * sizeof *pa->particles) == 0);
    wpe_ecs_destroy(a);
    wpe_ecs_destroy(b);
    wpe_jobs_destroy(jobs);
    wpe_test_assets_destroy(&t);
}

static void test_graphics_particles_draw(void) {
    WPE_TestAssets t = wpe_test_assets_create(NULL, 0);
    WPE_Ecs* ecs = make_world(t.assets);
    const WPE_Entity camera = spawn_camera(ecs, wpe_vec3(0, 0, 5), wpe_quat_identity());
    WPE_ParticleEmitter settings = wpe_particle_emitter_default();
    settings.rate = 0.0f;
    settings.speed_min = settings.speed_max = 0.0f;
    settings.size_start = settings.size_end = 1.0f;
    settings.color_start = settings.color_end = wpe_vec4(1, 0, 0, 1);
    settings.shape = WPE_PARTICLES_SPHERE;
    settings.shape_size = wpe_vec3(1, 1, 1);
    const WPE_Entity e = spawn_emitter(ecs, wpe_vec3(0, 0, 0), &settings);
    WPE_TEST_ASSERT_EQ_UINT(50, wpe_particles_emit(ecs, e, 50));

    WPE_Renderer* renderer = NULL;
    WPE_TEST_ASSERT_OK(wpe_renderer_create(&(WPE_RendererDesc){.width = 64, .height = 48}, &renderer));
    WPE_SceneRenderer* sr = NULL;
    WPE_TEST_ASSERT_OK(wpe_scene_renderer_create(&sr));
    WPE_SceneRenderStats stats;
    /* Alpha blended (sorted back to front): every particle in one draw, red over black. */
    const WPE_Framebuffer* fb = render_scene(renderer, sr, ecs, camera, &stats);
    WPE_TEST_ASSERT_EQ_UINT(50, stats.particles_drawn);
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_renderer_stats(renderer).draws);
    WPE_TEST_ASSERT(count_color(fb, 0xFFFF0000u) > 100);
    /* Cutout and additive draw the same particles. */
    ((WPE_ParticleEmitter*)wpe_ecs_get_component(ecs, e, WPE_COMP_PARTICLES))->blend = WPE_BILLBOARD_ADDITIVE;
    fb = render_scene(renderer, sr, ecs, camera, &stats);
    WPE_TEST_ASSERT(stats.particles_drawn == 50 && count_color(fb, 0xFFFF0000u) > 100);
    /* Out of view: the whole emitter is culled. */
    ((WPE_Transform*)wpe_ecs_get_component(ecs, camera, WPE_COMP_TRANSFORM))->rotation =
        wpe_quat_from_axis_angle(wpe_vec3(0, 1, 0), WPE_PI);
    render_scene(renderer, sr, ecs, camera, &stats);
    WPE_TEST_ASSERT(stats.particles_drawn == 0 && stats.particle_emitters_culled == 1);

    wpe_scene_renderer_destroy(sr);
    wpe_renderer_destroy(renderer);
    wpe_ecs_destroy(ecs);
    wpe_test_assets_destroy(&t);
}

/* ---- Trails ---------------------------------------------------------------------------------------- */

static WPE_Entity spawn_point(WPE_Ecs* ecs, WPE_Vec3 position) {
    const WPE_Entity e = wpe_ecs_spawn(ecs);
    WPE_Transform t = wpe_transform_identity();
    t.position = position;
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, e, WPE_COMP_TRANSFORM, &t, NULL));
    return e;
}

/* A trail records where its edges are each update while emitting, keeps a place for `lifetime`, and draws a
 * ribbon between the edges through the recent places; a copy starts with none. */
static void test_graphics_trail_records_and_draws(void) {
    WPE_TestAssets t = wpe_test_assets_create(NULL, 0);
    WPE_Ecs* ecs = make_world(t.assets);
    const WPE_Entity camera = spawn_camera(ecs, wpe_vec3(0, 0, 5), wpe_quat_identity());
    const WPE_Entity base = spawn_point(ecs, wpe_vec3(-1.5f, 0, 0));
    const WPE_Entity tip = spawn_point(ecs, wpe_vec3(-1.5f, 1.5f, 0));
    WPE_Trail settings = wpe_trail_default();
    settings.base = base;
    settings.tip = tip;
    settings.lifetime = 0.5f;
    settings.width_end = 1.0f;
    settings.color_start = settings.color_end = wpe_vec4(0, 1, 0, 1);
    settings.blend = WPE_BILLBOARD_CUTOUT;
    const WPE_Entity holder = spawn_point(ecs, wpe_vec3(0, 0, 0));
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, holder, WPE_COMP_TRAIL, &settings, NULL));

    /* The edges sweep to the right over ten updates: ten places, newest first. */
    for (int i = 0; i < 10; ++i) {
        ((WPE_Transform*)wpe_ecs_get_component(ecs, base, WPE_COMP_TRANSFORM))->position.x =
            -1.5f + 0.3f * (float)i;
        ((WPE_Transform*)wpe_ecs_get_component(ecs, tip, WPE_COMP_TRANSFORM))->position.x =
            -1.5f + 0.3f * (float)i;
        WPE_TEST_ASSERT_OK(wpe_scene_update_transforms(ecs));
        wpe_trail_update(ecs, 1.0f / 60.0f);
    }
    const WPE_Trail* trail = wpe_ecs_get_component(ecs, holder, WPE_COMP_TRAIL);
    WPE_TEST_ASSERT_EQ_UINT(10, trail->sample_count);
    WPE_TEST_ASSERT_NEAR(1.2, trail->samples[0].base.x, 1e-4);
    WPE_TEST_ASSERT_NEAR(-1.5, trail->samples[9].base.x, 1e-4);
    WPE_TEST_ASSERT(trail->samples[0].age < trail->samples[9].age);

    /* Drawn: one draw covering the swept band between y = 0 and 1.5, green. */
    WPE_Renderer* renderer = NULL;
    WPE_TEST_ASSERT_OK(wpe_renderer_create(&(WPE_RendererDesc){.width = 64, .height = 48}, &renderer));
    WPE_SceneRenderer* sr = NULL;
    WPE_TEST_ASSERT_OK(wpe_scene_renderer_create(&sr));
    WPE_SceneRenderStats stats;
    const WPE_Framebuffer* fb = render_scene(renderer, sr, ecs, camera, &stats);
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_renderer_stats(renderer).draws);
    WPE_TEST_ASSERT(count_color(fb, 0xFF00FF00u) > 100);

    /* Not emitting, it ages away: nothing left after its lifetime, and nothing drawn. */
    ((WPE_Trail*)wpe_ecs_get_component(ecs, holder, WPE_COMP_TRAIL))->emitting = false;
    wpe_trail_update(ecs, 0.4f);
    WPE_TEST_ASSERT(trail->sample_count > 0 && trail->sample_count < 10); /* The oldest went first. */
    wpe_trail_update(ecs, 0.3f);
    WPE_TEST_ASSERT_EQ_UINT(0, trail->sample_count);
    fb = render_scene(renderer, sr, ecs, camera, &stats);
    WPE_TEST_ASSERT_EQ_UINT(0, count_color(fb, 0xFF00FF00u));

    /* A full trail drops its oldest place; a copy (a duplicate entity) starts empty; clear forgets. */
    WPE_Trail* live = wpe_ecs_get_component(ecs, holder, WPE_COMP_TRAIL);
    live->emitting = true;
    live->lifetime = 60.0f;
    for (uint32_t i = 0; i < WPE_TRAIL_MAX_SAMPLES + 5; ++i)
        wpe_trail_update(ecs, 0.001f);
    WPE_TEST_ASSERT_EQ_UINT(WPE_TRAIL_MAX_SAMPLES, live->sample_count);
    const WPE_Entity copy = spawn_point(ecs, wpe_vec3(0, 0, 0));
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, copy, WPE_COMP_TRAIL, live, NULL));
    WPE_TEST_ASSERT_EQ_UINT(0, ((WPE_Trail*)wpe_ecs_get_component(ecs, copy, WPE_COMP_TRAIL))->sample_count);
    wpe_trail_clear(ecs, holder);
    WPE_TEST_ASSERT_EQ_UINT(0,
                            ((WPE_Trail*)wpe_ecs_get_component(ecs, holder, WPE_COMP_TRAIL))->sample_count);

    wpe_scene_renderer_destroy(sr);
    wpe_renderer_destroy(renderer);
    wpe_ecs_destroy(ecs);
    wpe_test_assets_destroy(&t);
}

/* A slash sweeps along its curve when played: drawn while playing (a ribbon, thin at both ends), its head
 * entity following the head, and gone at the end. */
static void test_graphics_slash_sweeps(void) {
    WPE_TestAssets t = wpe_test_assets_create(NULL, 0);
    WPE_Ecs* ecs = make_world(t.assets);
    const WPE_Entity camera = spawn_camera(ecs, wpe_vec3(0, 0, 5), wpe_quat_identity());
    const WPE_Entity e = spawn_point(ecs, wpe_vec3(0, 0, 0));
    const WPE_Entity sparks = spawn_point(ecs, wpe_vec3(0, 0, 0));
    WPE_TEST_ASSERT_OK(wpe_scene_set_parent(ecs, sparks, e, false));
    WPE_Slash settings = wpe_slash_default();
    settings.points[0] = wpe_vec3(-2, 0, 0);
    settings.points[1] = wpe_vec3(-1, 1, 0);
    settings.points[2] = wpe_vec3(1, 1, 0);
    settings.points[3] = wpe_vec3(2, 0, 0);
    settings.width = 0.6f;
    settings.duration = 1.0f;
    settings.length = 0.5f;
    settings.color_head = settings.color_tail = wpe_vec4(1, 0, 1, 1);
    settings.blend = WPE_BILLBOARD_CUTOUT;
    settings.head = sparks;
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, e, WPE_COMP_SLASH, &settings, NULL));
    /* The curve's ends and middle. */
    WPE_TEST_ASSERT_NEAR(-2.0, wpe_slash_point(&settings, 0.0f).x, 1e-5);
    WPE_TEST_ASSERT_NEAR(0.75, wpe_slash_point(&settings, 0.5f).y, 1e-5);

    WPE_Renderer* renderer = NULL;
    WPE_TEST_ASSERT_OK(wpe_renderer_create(&(WPE_RendererDesc){.width = 64, .height = 48}, &renderer));
    WPE_SceneRenderer* sr = NULL;
    WPE_TEST_ASSERT_OK(wpe_scene_renderer_create(&sr));
    WPE_SceneRenderStats stats;
    /* Not playing: nothing. */
    WPE_TEST_ASSERT_OK(wpe_scene_update_transforms(ecs));
    const WPE_Framebuffer* fb = render_scene(renderer, sr, ecs, camera, &stats);
    WPE_TEST_ASSERT_EQ_UINT(0, count_color(fb, 0xFFFF00FFu));
    /* Playing, a third of the way: the head (at 0.5 of the curve) leads, and the sparks are there. */
    wpe_slash_play(ecs, e);
    wpe_slash_update(ecs, 1.0f / 3.0f);
    const WPE_Vec3 head = ((WPE_Transform*)wpe_ecs_get_component(ecs, sparks, WPE_COMP_TRANSFORM))->position;
    WPE_TEST_ASSERT_NEAR(0.0, head.x, 1e-4);
    WPE_TEST_ASSERT_NEAR(0.75, head.y, 1e-4);
    WPE_TEST_ASSERT_OK(wpe_scene_update_transforms(ecs));
    fb = render_scene(renderer, sr, ecs, camera, &stats);
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_renderer_stats(renderer).draws);
    WPE_TEST_ASSERT(count_color(fb, 0xFFFF00FFu) > 20);
    /* Done after its duration: stopped and not drawn. */
    wpe_slash_update(ecs, 0.7f);
    WPE_TEST_ASSERT(!((WPE_Slash*)wpe_ecs_get_component(ecs, e, WPE_COMP_SLASH))->playing);
    fb = render_scene(renderer, sr, ecs, camera, &stats);
    WPE_TEST_ASSERT_EQ_UINT(0, count_color(fb, 0xFFFF00FFu));

    wpe_scene_renderer_destroy(sr);
    wpe_renderer_destroy(renderer);
    wpe_ecs_destroy(ecs);
    wpe_test_assets_destroy(&t);
}

static void test_graphics_scene_render_frustum_culling(void) {
    WPE_TestAssets t = wpe_test_assets_create(WPE_TEST_ASSETS_DIR, 32);
    const WPE_AssetHandle model = wpe_test_load_model(t.assets, "meshes/damaged_helmet.glb");
    WPE_Ecs* ecs = make_world(t.assets);
    WPE_Entity root;
    WPE_TEST_ASSERT_OK(wpe_model_instantiate(ecs, t.assets, model, NULL, &root));
    const WPE_Entity camera = spawn_camera(ecs, wpe_vec3(0, 0, 5), wpe_quat_identity());
    WPE_TEST_ASSERT_OK(wpe_scene_update_transforms(ecs));

    WPE_Renderer* renderer = NULL;
    WPE_TEST_ASSERT_OK(wpe_renderer_create(&(WPE_RendererDesc){.width = 64, .height = 48}, &renderer));
    WPE_SceneRenderer* sr = NULL;
    WPE_TEST_ASSERT_OK(wpe_scene_renderer_create(&sr));

    /* Looking at the helmet: one primitive drawn. */
    WPE_SceneRenderStats stats;
    WPE_TEST_ASSERT_OK(wpe_renderer_begin_frame(renderer, NULL));
    WPE_TEST_ASSERT_OK(wpe_scene_render(sr, renderer, ecs, camera, NULL, &stats));
    WPE_TEST_ASSERT_OK(wpe_renderer_end_frame(renderer));
    WPE_TEST_ASSERT_EQ_UINT(1, stats.primitives_submitted);
    WPE_TEST_ASSERT_EQ_UINT(0, stats.primitives_culled);
    WPE_TEST_ASSERT(wpe_renderer_stats(renderer).fragments_shaded > 100);

    /* Turned around: culled before reaching the rasterizer. */
    ((WPE_Transform*)wpe_ecs_get_component(ecs, camera, WPE_COMP_TRANSFORM))->rotation =
        wpe_quat_from_axis_angle(wpe_vec3(0, 1, 0), WPE_PI);
    WPE_TEST_ASSERT_OK(wpe_scene_update_transforms(ecs));
    WPE_TEST_ASSERT_OK(wpe_renderer_begin_frame(renderer, NULL));
    WPE_TEST_ASSERT_OK(wpe_scene_render(sr, renderer, ecs, camera, NULL, &stats));
    WPE_TEST_ASSERT_OK(wpe_renderer_end_frame(renderer));
    WPE_TEST_ASSERT_EQ_UINT(0, stats.primitives_submitted);
    WPE_TEST_ASSERT_EQ_UINT(1, stats.primitives_culled);

    /* Invisible mesh renderers are skipped; invalid cameras are rejected. */
    WPE_EcsPoolView meshes;
    WPE_TEST_ASSERT_OK(wpe_ecs_pool_view(ecs, WPE_COMP_MESH, &meshes));
    ((WPE_MeshRenderer*)meshes.data)->visible = false;
    WPE_TEST_ASSERT_OK(wpe_renderer_begin_frame(renderer, NULL));
    WPE_TEST_ASSERT_OK(wpe_scene_render(sr, renderer, ecs, camera, NULL, &stats));
    wpe_log_set_stderr_enabled(false);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG,
                           wpe_scene_render(sr, renderer, ecs, root, NULL, &stats));
    wpe_log_set_stderr_enabled(true);
    WPE_TEST_ASSERT_OK(wpe_renderer_end_frame(renderer));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_renderer_stats(renderer).draws);

    wpe_scene_renderer_destroy(sr);
    wpe_renderer_destroy(renderer);
    wpe_ecs_destroy(ecs);
    wpe_asset_release(t.assets, model);
    wpe_test_assets_destroy(&t);
}

static void test_graphics_camera_matrices_validation(void) {
    WPE_TestAssets t = wpe_test_assets_create(NULL, 0);
    WPE_Ecs* ecs = make_world(t.assets);
    const WPE_Entity camera = spawn_camera(ecs, wpe_vec3(0, 0, 0), wpe_quat_identity());
    WPE_TEST_ASSERT_OK(wpe_scene_update_transforms(ecs));
    WPE_Mat4 view, proj;
    WPE_TEST_ASSERT(wpe_scene_camera_matrices(ecs, camera, 1.5f, &view, &proj));
    WPE_TEST_ASSERT(!wpe_scene_camera_matrices(ecs, camera, 0.0f, &view, &proj));

    WPE_Camera* c = wpe_ecs_get_component(ecs, camera, WPE_COMP_CAMERA);
    c->near_z = 0.0f;
    WPE_TEST_ASSERT(!wpe_scene_camera_matrices(ecs, camera, 1.0f, &view, &proj));
    *c = wpe_camera_default();
    c->orthographic = true;
    c->ortho_height = 4.0f;
    WPE_TEST_ASSERT(wpe_scene_camera_matrices(ecs, camera, 2.0f, &view, &proj));
    /* Orthographic: the top edge of a 4-unit-tall view maps to NDC y = 1. */
    const WPE_Vec4 top = wpe_mat4_mul_vec4(&proj, wpe_vec4(0, 2, -1, 1));
    WPE_TEST_ASSERT_NEAR(1.0, top.y, 1e-5);

    /* A camera with zero scale has no valid view. */
    ((WPE_Transform*)wpe_ecs_get_component(ecs, camera, WPE_COMP_TRANSFORM))->scale = wpe_vec3(0, 0, 0);
    WPE_TEST_ASSERT_OK(wpe_scene_update_transforms(ecs));
    WPE_TEST_ASSERT(!wpe_scene_camera_matrices(ecs, camera, 1.0f, &view, &proj));
    wpe_ecs_destroy(ecs);
    wpe_test_assets_destroy(&t);
}

/* ---- 2D ------------------------------------------------------------------------------------------- */

static void test_graphics_draw2d_text_is_pixel_exact(void) {
    WPE_BitmapFont* font = NULL;
    WPE_TEST_ASSERT_OK(wpe_font_load(ASSET("fonts/small.png"), &font));
    WPE_Renderer* renderer = NULL;
    WPE_TEST_ASSERT_OK(wpe_renderer_create(&(WPE_RendererDesc){.width = 40, .height = 20}, &renderer));
    WPE_Draw2D* d = NULL;
    WPE_TEST_ASSERT_OK(wpe_draw2d_create(&d));

    WPE_TEST_ASSERT_OK(
        wpe_renderer_begin_frame(renderer, &(WPE_ClearDesc){.clear_color = true, .color = 0xFF000000u}));
    wpe_draw2d_begin(d, 40, 20);
    wpe_draw2d_text(d, font, 3, 4, "AB", 0xFFFFFFFFu, 1.0f);
    WPE_TEST_ASSERT_OK(wpe_draw2d_submit(d, renderer));
    WPE_TEST_ASSERT_OK(wpe_renderer_end_frame(renderer));

    /* Every framebuffer pixel equals the atlas glyph texel it should show (ink white, rest black). */
    const WPE_Framebuffer* fb = wpe_renderer_framebuffer(renderer);
    const WPE_TextureLevel* atlas = &font->atlas->levels[0];
    const WPE_FontGlyph* glyphs[2] = {wpe_font_glyph(font, 'A'), wpe_font_glyph(font, 'B')};
    uint32_t pen = 3, ink = 0;
    for (int g = 0; g < 2; ++g) {
        for (uint32_t y = 0; y < glyphs[g]->height; ++y) {
            for (uint32_t x = 0; x < glyphs[g]->width; ++x) {
                const uint32_t texel = atlas->texels[(glyphs[g]->y + y) * atlas->width + glyphs[g]->x + x];
                const WPE_Color expected = (texel >> 24) ? 0xFFFFFFFFu : 0xFF000000u;
                WPE_TEST_ASSERT_EQ_UINT(expected, fb->color[(4 + y) * 40 + pen + x]);
                ink += expected == 0xFFFFFFFFu;
            }
        }
        pen += glyphs[g]->width + font->glyph_spacing;
    }
    WPE_TEST_ASSERT(ink > 10);
    /* Nothing outside the text area was touched. */
    for (uint32_t x = 0; x < 40; ++x)
        WPE_TEST_ASSERT_EQ_UINT(0xFF000000u, fb->color[x]);

    /* Rectangles blend over 3D content and respect alpha. */
    WPE_TEST_ASSERT_OK(
        wpe_renderer_begin_frame(renderer, &(WPE_ClearDesc){.clear_color = true, .color = 0xFF0000FFu}));
    wpe_draw2d_begin(d, 40, 20);
    wpe_draw2d_rect(d, (WPE_Rect){0, 0, 10, 10}, wpe_color_rgba8(255, 0, 0, 128));
    wpe_draw2d_rect(d, (WPE_Rect){20, 0, 0, 10}, 0xFFFFFFFFu); /* zero-size: ignored */
    WPE_TEST_ASSERT_OK(wpe_draw2d_submit(d, renderer));
    WPE_TEST_ASSERT_OK(wpe_renderer_end_frame(renderer));
    WPE_TEST_ASSERT_EQ_UINT(wpe_color_rgba8(128, 0, 127, 255), fb->color[5 * 40 + 5]);
    WPE_TEST_ASSERT_EQ_UINT(0xFF0000FFu, fb->color[5 * 40 + 15]);
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_renderer_stats(renderer).draws);

    wpe_draw2d_destroy(d);
    wpe_renderer_destroy(renderer);
    wpe_font_destroy(font);
}

/* ---- Debug lines ------------------------------------------------------------------------------------ */

static uint32_t count_color(const WPE_Framebuffer* fb, WPE_Color color) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < fb->width * fb->height; ++i)
        count += fb->color[i] == color;
    return count;
}

static void test_graphics_debug_lines(void) {
    WPE_Renderer* renderer = NULL;
    WPE_TEST_ASSERT_OK(wpe_renderer_create(&(WPE_RendererDesc){.width = 64, .height = 48}, &renderer));
    WPE_DebugLines* lines = NULL;
    WPE_TEST_ASSERT_OK(wpe_debug_lines_create(&lines));
    const WPE_Mat4 identity = wpe_mat4_identity(); /* World coordinates are clip coordinates. */
    const WPE_Framebuffer* fb = wpe_renderer_framebuffer(renderer);
    const WPE_ClearDesc clear = {
        .clear_color = true, .color = 0xFF000000u, .clear_depth = true, .depth = 1.0f};

    /* A horizontal line across half the screen covers one pixel row of 32 pixels. */
    WPE_TEST_ASSERT_OK(wpe_renderer_begin_frame(renderer, &clear));
    wpe_debug_lines_begin(lines, &identity, 64, 48, 1.0f);
    wpe_debug_lines_add(lines, wpe_vec3(-0.5f, 0.01f, 0.0f), wpe_vec3(0.5f, 0.01f, 0.0f), 0xFFFF0000u);
    /* Entirely behind the near plane: rejected. */
    wpe_debug_lines_add(lines, wpe_vec3(0.0f, 0.0f, -2.0f), wpe_vec3(0.5f, 0.5f, -3.0f), 0xFF00FF00u);
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_debug_lines_count(lines));
    WPE_TEST_ASSERT_OK(wpe_debug_lines_submit(lines, renderer, true));
    WPE_TEST_ASSERT_OK(wpe_renderer_end_frame(renderer));
    WPE_TEST_ASSERT_EQ_UINT(32, count_color(fb, 0xFFFF0000u));
    WPE_TEST_ASSERT_EQ_UINT(0xFFFF0000u, fb->color[23 * 64 + 32]);
    WPE_TEST_ASSERT_NEAR(1.0, fb->depth[23 * 64 + 32], 0); /* No depth writes. */

    /* A wider vertical line: 3 pixel columns over half the height. */
    WPE_TEST_ASSERT_OK(wpe_renderer_begin_frame(renderer, &clear));
    wpe_debug_lines_begin(lines, &identity, 64, 48, 3.0f);
    wpe_debug_lines_add(lines, wpe_vec3(0.01f, -0.5f, 0.0f), wpe_vec3(0.01f, 0.5f, 0.0f), 0xFF00FF00u);
    WPE_TEST_ASSERT_OK(wpe_debug_lines_submit(lines, renderer, true));
    WPE_TEST_ASSERT_OK(wpe_renderer_end_frame(renderer));
    WPE_TEST_ASSERT_EQ_UINT(3 * 24, count_color(fb, 0xFF00FF00u));

    /* Depth testing hides lines behind the stored depth unless disabled. */
    const WPE_ClearDesc near_depth = {
        .clear_color = true, .color = 0xFF000000u, .clear_depth = true, .depth = 0.25f};
    for (int test = 0; test < 2; ++test) {
        WPE_TEST_ASSERT_OK(wpe_renderer_begin_frame(renderer, &near_depth));
        wpe_debug_lines_begin(lines, &identity, 64, 48, 1.0f);
        wpe_debug_lines_add(lines, wpe_vec3(-0.5f, 0.01f, 0.0f), wpe_vec3(0.5f, 0.01f, 0.0f), 0xFFFF0000u);
        WPE_TEST_ASSERT_OK(wpe_debug_lines_submit(lines, renderer, test == 0));
        WPE_TEST_ASSERT_OK(wpe_renderer_end_frame(renderer));
        WPE_TEST_ASSERT_EQ_UINT(test == 0 ? 0u : 32u, count_color(fb, 0xFFFF0000u));
    }
    wpe_debug_lines_destroy(lines);
    wpe_renderer_destroy(renderer);
}

WPE_TEST_MAIN(
    WPE_TEST_CASE(test_graphics_sample_linear_step_and_clamp),
    WPE_TEST_CASE(test_graphics_sample_rotation_slerps_shortest_path),
    WPE_TEST_CASE(test_graphics_sample_cubic_spline), WPE_TEST_CASE(test_graphics_instantiate_rigged_figure),
    WPE_TEST_CASE(test_graphics_animation_crossfade),
    WPE_TEST_CASE(test_graphics_animation_crossfade_between_clips),
    WPE_TEST_CASE(test_graphics_sky_gradient_sun_and_occlusion), WPE_TEST_CASE(test_graphics_sky_panorama),
    WPE_TEST_CASE(test_graphics_instantiate_validates_and_cleans_up),
    WPE_TEST_CASE(test_graphics_material_vertex_shader_skinning),
    WPE_TEST_CASE(test_graphics_shininess_range), WPE_TEST_CASE(test_graphics_scene_render_frustum_culling),
    WPE_TEST_CASE(test_graphics_billboard_corners), WPE_TEST_CASE(test_graphics_billboard_animation),
    WPE_TEST_CASE(test_graphics_billboard_draws_frames_and_blends),
    WPE_TEST_CASE(test_graphics_mesh_shadow_unlit_and_sides),
    WPE_TEST_CASE(test_graphics_particles_emit_and_expire), WPE_TEST_CASE(test_graphics_particles_move),
    WPE_TEST_CASE(test_graphics_particles_are_deterministic), WPE_TEST_CASE(test_graphics_particles_draw),
    WPE_TEST_CASE(test_graphics_trail_records_and_draws), WPE_TEST_CASE(test_graphics_slash_sweeps),
    WPE_TEST_CASE(test_graphics_camera_matrices_validation),
    WPE_TEST_CASE(test_graphics_draw2d_text_is_pixel_exact), WPE_TEST_CASE(test_graphics_debug_lines))
