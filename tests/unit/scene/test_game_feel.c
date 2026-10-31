#include <math.h>

#include "wpe_test.h"

#include "core/log.h"
#include "scene/game_feel.h"
#include "scene/scene.h"

#define EPS 1e-5

/* ---- Time scale ------------------------------------------------------------------------------------ */

static void test_game_feel_base_scale_and_hit_stop(void) {
    WPE_GameFeel feel = wpe_game_feel_default();
    WPE_TEST_ASSERT_NEAR(0.1, wpe_game_feel_advance(&feel, 0.1), EPS);
    WPE_TEST_ASSERT_NEAR(0.1, feel.real_dt, EPS);
    feel.base_scale = 0.5f;
    WPE_TEST_ASSERT_NEAR(0.05, wpe_game_feel_advance(&feel, 0.1), EPS);
    feel.base_scale = 1.0f;

    /* A hit-stop freezes whole frames, then only its share of the frame it ends in. */
    wpe_game_feel_hit_stop(&feel, 0.25f);
    WPE_TEST_ASSERT_NEAR(0.0, wpe_game_feel_time_scale(&feel), EPS);
    WPE_TEST_ASSERT_NEAR(0.0, wpe_game_feel_advance(&feel, 0.1), EPS);
    WPE_TEST_ASSERT_NEAR(0.0, wpe_game_feel_advance(&feel, 0.1), EPS);
    WPE_TEST_ASSERT_NEAR(0.05, wpe_game_feel_advance(&feel, 0.1), EPS);
    WPE_TEST_ASSERT_NEAR(0.1, wpe_game_feel_advance(&feel, 0.1), EPS);
    WPE_TEST_ASSERT_NEAR(1.0, wpe_game_feel_time_scale(&feel), EPS);

    /* The longer of two hit-stops wins; nonsense is ignored. */
    wpe_game_feel_hit_stop(&feel, 0.5f);
    wpe_game_feel_hit_stop(&feel, 0.1f);
    wpe_game_feel_hit_stop(&feel, NAN);
    wpe_game_feel_hit_stop(&feel, -1.0f);
    WPE_TEST_ASSERT_NEAR(0.5, feel.stop_remaining, EPS);
    WPE_TEST_ASSERT_NEAR(0.0, wpe_game_feel_advance(&feel, -1.0), EPS);
    WPE_TEST_ASSERT_NEAR(0.0, wpe_game_feel_advance(&feel, INFINITY), EPS);
    WPE_TEST_ASSERT_NEAR(0.5, feel.stop_remaining, EPS);
}

static void test_game_feel_slow_motion(void) {
    WPE_GameFeel feel = wpe_game_feel_default();
    wpe_game_feel_slow_motion(&feel, 0.25f, 1.0f, 0.0f);
    WPE_TEST_ASSERT_NEAR(0.25, wpe_game_feel_time_scale(&feel), EPS);
    WPE_TEST_ASSERT_NEAR(0.125, wpe_game_feel_advance(&feel, 0.5), EPS);
    /* It ends mid-frame: half slow, half at full speed. */
    WPE_TEST_ASSERT_NEAR(0.5 * 0.25 + 0.5, wpe_game_feel_advance(&feel, 1.0), EPS);
    WPE_TEST_ASSERT_NEAR(1.0, wpe_game_feel_time_scale(&feel), EPS);

    /* Fading over the whole second from a standstill: the scale rises linearly, half a second of game time.
     */
    wpe_game_feel_slow_motion(&feel, 0.0f, 1.0f, 1.0f);
    double game = 0.0;
    for (int i = 0; i < 10; ++i)
        game += wpe_game_feel_advance(&feel, 0.1);
    WPE_TEST_ASSERT_NEAR(0.5, game, 1e-4);

    /* A hit-stop inside slow motion freezes, then slow motion carries on (its clock ran meanwhile). */
    wpe_game_feel_slow_motion(&feel, 0.5f, 1.0f, 0.0f);
    wpe_game_feel_hit_stop(&feel, 0.2f);
    WPE_TEST_ASSERT_NEAR(0.1 * 0.5, wpe_game_feel_advance(&feel, 0.3), EPS);
    WPE_TEST_ASSERT_NEAR(0.7, feel.slow_remaining, EPS);
    WPE_TEST_ASSERT_NEAR(0.5, wpe_game_feel_time_scale(&feel), EPS);

    /* Invalid requests change nothing. */
    wpe_game_feel_slow_motion(&feel, -1.0f, 1.0f, 0.0f);
    wpe_game_feel_slow_motion(&feel, 0.5f, 0.0f, 0.0f);
    WPE_TEST_ASSERT_NEAR(0.7, feel.slow_remaining, EPS);
}

/* ---- Shakes ---------------------------------------------------------------------------------------- */

static void test_game_feel_shake_offset_bounds_and_fade(void) {
    const WPE_ShakeParams params = {
        .amplitude = 0.5f, .rotation = 10.0f, .duration = 1.0f, .frequency = 15.0f};
    const float radians = 10.0f * WPE_PI / 180.0f;
    float early = 0.0f, late = 0.0f;
    for (int i = 0; i < 1000; ++i) {
        const float t = (float)i / 1000.0f;
        WPE_Vec3 p, r;
        wpe_shake_offset(&params, 7, t, &p, &r);
        for (int k = 0; k < 3; ++k) {
            WPE_TEST_ASSERT(fabsf(p.v[k]) <= params.amplitude + 1e-6f);
            WPE_TEST_ASSERT(fabsf(r.v[k]) <= radians + 1e-6f);
        }
        const float size = fabsf(p.x) + fabsf(p.y) + fabsf(p.z);
        if (t < 0.2f) early = fmaxf(early, size);
        if (t > 0.8f) late = fmaxf(late, size);
    }
    WPE_TEST_ASSERT(early > 0.2f && late < early * 0.1f); /* Strong at first, faded at the end. */

    WPE_Vec3 a, b, ra, rb;
    wpe_shake_offset(&params, 7, 0.3f, &a, &ra);
    wpe_shake_offset(&params, 7, 0.3f, &b, &rb);
    WPE_TEST_ASSERT(a.x == b.x && a.y == b.y && ra.z == rb.z); /* Deterministic... */
    wpe_shake_offset(&params, 8, 0.3f, &b, &rb);
    WPE_TEST_ASSERT(a.x != b.x || a.y != b.y); /* ...and the seed picks the noise. */
    wpe_shake_offset(&params, 7, 1.0f, &a, &ra);
    WPE_TEST_ASSERT(a.x == 0.0f && a.y == 0.0f && a.z == 0.0f && ra.x == 0.0f); /* Over. */
}

static WPE_Ecs* make_scene(void) {
    WPE_Ecs* ecs = NULL;
    WPE_TEST_ASSERT_OK(wpe_ecs_create(&ecs));
    WPE_TEST_ASSERT_OK(wpe_scene_register_components(ecs));
    return ecs;
}

static WPE_Transform* spawn_at(WPE_Ecs* ecs, WPE_Vec3 position, WPE_Entity* out) {
    *out = wpe_ecs_spawn(ecs);
    WPE_Transform t = wpe_transform_identity();
    t.position = position;
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, *out, WPE_COMP_TRANSFORM, &t, NULL));
    return wpe_ecs_get_component(ecs, *out, WPE_COMP_TRANSFORM);
}

static bool same_pose(const WPE_Transform* t, WPE_Vec3 p, WPE_Quat q) {
    return t->position.x == p.x && t->position.y == p.y && t->position.z == p.z && t->rotation.x == q.x &&
           t->rotation.y == q.y && t->rotation.z == q.z && t->rotation.w == q.w;
}

static void test_game_feel_shakes_restore_the_real_pose(void) {
    WPE_Ecs* ecs = make_scene();
    WPE_GameFeel feel = wpe_game_feel_default();
    WPE_Entity e;
    const WPE_Vec3 home = wpe_vec3(1, 2, 3);
    WPE_Transform* t = spawn_at(ecs, home, &e);
    const WPE_Quat still = t->rotation;
    const WPE_ShakeParams params = {
        .amplitude = 0.5f, .rotation = 10.0f, .duration = 0.5f, .frequency = 20.0f};
    wpe_game_feel_shake(&feel, e, &params);
    WPE_TEST_ASSERT(wpe_game_feel_is_shaking(&feel, e));

    /* On before transforms, off at the start of the next frame: exactly the pose it had. */
    wpe_game_feel_apply_shakes(&feel, ecs, 0.05f);
    WPE_TEST_ASSERT(!same_pose(t, home, still));
    wpe_game_feel_restore_shakes(&feel, ecs);
    WPE_TEST_ASSERT(same_pose(t, home, still));

    /* Game code moves it during the frame: the shake goes on around the new place. */
    t->position = wpe_vec3(5, 5, 5);
    wpe_game_feel_apply_shakes(&feel, ecs, 0.05f);
    WPE_TEST_ASSERT(fabsf(t->position.x - 5.0f) <= 0.5f && !same_pose(t, wpe_vec3(5, 5, 5), still));
    wpe_game_feel_restore_shakes(&feel, ecs);
    WPE_TEST_ASSERT(same_pose(t, wpe_vec3(5, 5, 5), still));

    /* Moved while shaken (a teleport from outside the frame): the new pose stays. */
    wpe_game_feel_apply_shakes(&feel, ecs, 0.05f);
    t->position = wpe_vec3(9, 9, 9);
    wpe_game_feel_restore_shakes(&feel, ecs);
    WPE_TEST_ASSERT(t->position.x == 9.0f && t->position.y == 9.0f);

    /* Finished shakes are dropped with the pose restored. */
    wpe_game_feel_apply_shakes(&feel, ecs, 0.05f);
    wpe_game_feel_apply_shakes(&feel, ecs, 1.0f);
    WPE_TEST_ASSERT_EQ_UINT(0, feel.shake_count);
    WPE_TEST_ASSERT(same_pose(t, wpe_vec3(9, 9, 9), still));
    WPE_TEST_ASSERT(!wpe_game_feel_is_shaking(&feel, e));

    /* Stopped: restored and dropped at the next apply. Reset restores too. */
    wpe_game_feel_shake(&feel, e, &params);
    wpe_game_feel_apply_shakes(&feel, ecs, 0.05f);
    wpe_game_feel_stop_shake(&feel, e);
    WPE_TEST_ASSERT(!wpe_game_feel_is_shaking(&feel, e));
    wpe_game_feel_apply_shakes(&feel, ecs, 0.05f);
    WPE_TEST_ASSERT(feel.shake_count == 0 && same_pose(t, wpe_vec3(9, 9, 9), still));
    wpe_game_feel_shake(&feel, e, &params);
    wpe_game_feel_apply_shakes(&feel, ecs, 0.05f);
    feel.base_scale = 0.5f;
    wpe_game_feel_reset(&feel, ecs);
    WPE_TEST_ASSERT(feel.shake_count == 0 && feel.base_scale == 1.0f &&
                    same_pose(t, wpe_vec3(9, 9, 9), still));

    /* A destroyed entity's shake ends quietly. */
    wpe_game_feel_shake(&feel, e, &params);
    wpe_game_feel_apply_shakes(&feel, ecs, 0.05f);
    WPE_TEST_ASSERT_OK(wpe_ecs_despawn(ecs, e));
    wpe_game_feel_restore_shakes(&feel, ecs);
    wpe_game_feel_apply_shakes(&feel, ecs, 0.05f);
    WPE_TEST_ASSERT_EQ_UINT(0, feel.shake_count);
    wpe_ecs_destroy(ecs);
}

static void test_game_feel_shake_limits(void) {
    WPE_GameFeel feel = wpe_game_feel_default();
    const WPE_ShakeParams params = {.amplitude = 0.1f, .duration = 0.3f, .frequency = 10.0f};
    for (uint32_t i = 0; i < WPE_MAX_SHAKES; ++i)
        wpe_game_feel_shake(&feel, (WPE_Entity)(i + 1), &params);
    /* Shaking an entity again replaces its shake; one more entity than fits is refused. */
    wpe_game_feel_shake(&feel, 1, &params);
    WPE_TEST_ASSERT_EQ_UINT(WPE_MAX_SHAKES, feel.shake_count);
    wpe_log_set_stderr_enabled(false);
    wpe_game_feel_shake(&feel, 1000, &params);
    wpe_log_set_stderr_enabled(true);
    WPE_TEST_ASSERT_EQ_UINT(WPE_MAX_SHAKES, feel.shake_count);
    WPE_TEST_ASSERT(!wpe_game_feel_is_shaking(&feel, 1000));
    const WPE_ShakeParams endless = {.amplitude = 0.1f, .duration = 0.0f};
    wpe_game_feel_shake(&feel, 2000, &endless); /* No duration: ignored. */
    WPE_TEST_ASSERT(!wpe_game_feel_is_shaking(&feel, 2000));
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_game_feel_base_scale_and_hit_stop),
              WPE_TEST_CASE(test_game_feel_slow_motion),
              WPE_TEST_CASE(test_game_feel_shake_offset_bounds_and_fade),
              WPE_TEST_CASE(test_game_feel_shakes_restore_the_real_pose),
              WPE_TEST_CASE(test_game_feel_shake_limits))
