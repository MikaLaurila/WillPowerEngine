/*
 * Integration: the full scripting API (assets/scripts/api_tour.luau) together with the example
 * scripts, run for 60 simulated seconds through a fixed-timestep game loop, with physics,
 * animation and transform systems in the loop. Must finish without a single script error or leak.
 */
#include "wpe_test.h"

#include <stdio.h>

#include "assets/procedural.h"
#include "graphics/animation.h"
#include "graphics/graphics.h"
#include "scene/scene.h"
#include "script/script.h"

static WPE_Entity spawn_with_script(WPE_Ecs* ecs, WPE_ScriptHost* host, const char* script,
                                    WPE_Vec3 position) {
    const WPE_Entity e = wpe_ecs_spawn(ecs);
    WPE_Transform t = wpe_transform_identity();
    t.position = position;
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, e, WPE_COMP_TRANSFORM, &t, NULL));
    WPE_TEST_ASSERT_OK(wpe_script_attach(host, e, script));
    return e;
}

static void test_integration_scripts_run_sixty_seconds(void) {
    WPE_Ecs* ecs = NULL;
    WPE_TEST_ASSERT_OK(wpe_ecs_create(&ecs));
    WPE_TEST_ASSERT_OK(wpe_scene_register_components(ecs));
    WPE_TEST_ASSERT_OK(wpe_graphics_register_components(ecs));
    WPE_Physics* physics = NULL;
    WPE_TEST_ASSERT_OK(wpe_physics_create(
        &(WPE_PhysicsDesc){.ecs = ecs, .gravity = wpe_vec3(0.0f, -9.81f, 0.0f)}, &physics));
    WPE_ScriptHost* host = NULL;
    WPE_TEST_ASSERT_OK(wpe_script_host_create(
        &(WPE_ScriptHostDesc){.ecs = ecs, .physics = physics, .asset_root = WPE_TEST_ASSETS_DIR}, &host));

    /* Ground: a static 60 x 1 x 60 box with its top face at y = 0. */
    const WPE_Entity ground = wpe_ecs_spawn(ecs);
    WPE_Transform ground_transform = wpe_transform_identity();
    ground_transform.position.y = -0.5f;
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, ground, WPE_COMP_TRANSFORM, &ground_transform, NULL));
    WPE_TEST_ASSERT_OK(wpe_scene_set_name(ecs, ground, "ground"));
    WPE_Collider floor = wpe_collider_default(WPE_COLLIDER_BOX);
    floor.size = wpe_vec3(60.0f, 1.0f, 60.0f);
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, ground, WPE_COMP_PHYSICS_COLLIDER, &floor, NULL));

    WPE_Model *box = NULL, *figure = NULL;
    WPE_TEST_ASSERT_OK(wpe_model_create_box(wpe_vec3(1, 1, 1), NULL, &box));
    WPE_TEST_ASSERT_OK(wpe_model_load(WPE_TEST_ASSETS_DIR "/meshes/RiggedFigure.glb", NULL, &figure));
    WPE_TEST_ASSERT_OK(wpe_script_host_register_prefab(host, "box", box));
    WPE_TEST_ASSERT_OK(wpe_script_host_register_prefab(host, "figure", figure));

    const WPE_Entity tour = spawn_with_script(ecs, host, "scripts/api_tour.luau", wpe_vec3(0, 0, 0));
    spawn_with_script(ecs, host, "scripts/spawner.luau", wpe_vec3(0, 0, 0));
    for (int i = 0; i < 3; ++i)
        spawn_with_script(ecs, host, "scripts/orbit_light.luau", wpe_vec3(0, 1, 0));
    spawn_with_script(ecs, host, "scripts/spinner.luau", wpe_vec3(0, 1, 2));

    /* 60 seconds at 30 frames/s with a 60 Hz fixed step: exactly two fixed steps per frame. */
    WPE_FixedStep clock;
    wpe_fixed_step_init(&clock, 1.0 / 60.0, 8);
    const double frame_dt = 2.0 * clock.step;
    uint32_t max_entities = 0;
    for (int frame = 0; frame < 1800; ++frame) {
        const uint32_t steps = wpe_fixed_step_advance(&clock, frame_dt);
        WPE_TEST_ASSERT_EQ_UINT(2, steps);
        for (uint32_t s = 0; s < steps; ++s) {
            WPE_TEST_ASSERT_OK(wpe_script_host_fixed_update(host, (float)clock.step));
            WPE_TEST_ASSERT_OK(wpe_physics_step(physics, (float)clock.step));
            WPE_TEST_ASSERT_OK(wpe_script_host_dispatch_physics(host));
        }
        WPE_TEST_ASSERT_OK(wpe_script_host_update(host, (float)frame_dt));
        WPE_TEST_ASSERT_OK(wpe_animation_update(ecs, (float)frame_dt));
        WPE_TEST_ASSERT_OK(wpe_scene_update_transforms(ecs));
        const uint32_t count = wpe_ecs_entity_count(ecs);
        max_entities = count > max_entities ? count : max_entities;
    }

    const WPE_ScriptStats stats = wpe_script_host_stats(host);
    WPE_TEST_ASSERT_MSG(stats.errors == 0, "%llu script error(s)", (unsigned long long)stats.errors);
    WPE_TEST_ASSERT_EQ_UINT(0, stats.failed);
    /* Fixed steps of the first frame run before on_start, so they are skipped. */
    WPE_TEST_ASSERT_EQ_STR("tour:1799:3598", wpe_scene_get_name(ecs, tour));
    /* The spawner keeps adding boxes and falling.luau removes them after they land (a collision
     * callback): the population stays bounded. */
    WPE_TEST_ASSERT(max_entities > 40 && wpe_ecs_entity_count(ecs) < max_entities + 1);
    WPE_TEST_ASSERT(max_entities < 80);
    WPE_TEST_ASSERT(wpe_physics_stats(physics).bodies > 5);
    WPE_TEST_ASSERT(stats.scripts >= 5);

    /* on_destroy of the tour cleans up what it created (deferred destroys are flushed by the host). */
    const uint32_t before = wpe_ecs_entity_count(ecs);
    WPE_TEST_ASSERT_OK(wpe_ecs_despawn(ecs, tour));
    WPE_TEST_ASSERT_OK(wpe_script_host_update(host, (float)frame_dt));
    WPE_TEST_ASSERT(wpe_ecs_entity_count(ecs) < before - 1);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_script_host_stats(host).errors);

    wpe_script_host_destroy(host);
    wpe_physics_destroy(physics);
    wpe_ecs_destroy(ecs);
    wpe_model_destroy(figure);
    wpe_model_destroy(box);
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_integration_scripts_run_sixty_seconds))
