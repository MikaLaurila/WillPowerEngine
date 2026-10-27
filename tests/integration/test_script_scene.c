/*
 * Integration: the full scripting API (assets/scripts/api_tour.luau) together with the example
 * scripts, run for 60 simulated seconds through a fixed-timestep game loop, with physics, audio,
 * animation and transform systems in the loop. Must finish without a single script error or leak.
 */
#include "wpe_test.h"
#include "wpe_test_assets.h"

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
    WPE_TestAssets assets = wpe_test_assets_create(WPE_TEST_ASSETS_DIR, 64);
    WPE_Ecs* ecs = NULL;
    WPE_TEST_ASSERT_OK(wpe_ecs_create(&ecs));
    WPE_TEST_ASSERT_OK(wpe_scene_register_components(ecs));
    WPE_TEST_ASSERT_OK(wpe_graphics_register_components(ecs, assets.assets));
    WPE_Physics* physics = NULL;
    WPE_TEST_ASSERT_OK(wpe_physics_create(
        &(WPE_PhysicsDesc){.ecs = ecs, .assets = assets.assets, .gravity = wpe_vec3(0.0f, -9.81f, 0.0f)},
        &physics));
    /* Audio without a device: the loop mixes each frame's worth of samples itself. */
    WPE_Audio* audio = NULL;
    WPE_TEST_ASSERT_OK(wpe_audio_create(
        &(WPE_AudioDesc){.ecs = ecs, .assets = assets.assets, .no_device = true, .sample_rate = 22050},
        &audio));
    WPE_ScriptHost* host = NULL;
    WPE_TEST_ASSERT_OK(wpe_script_host_create(
        &(WPE_ScriptHostDesc){.ecs = ecs, .assets = assets.assets, .physics = physics, .audio = audio},
        &host));

    /* Ground: a static 60 x 1 x 60 box with its top face at y = 0. */
    const WPE_Entity ground = wpe_ecs_spawn(ecs);
    WPE_Transform ground_transform = wpe_transform_identity();
    ground_transform.position.y = -0.5f;
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, ground, WPE_COMP_TRANSFORM, &ground_transform, NULL));
    WPE_TEST_ASSERT_OK(wpe_scene_set_name(ecs, ground, "ground"));
    WPE_Collider floor = wpe_collider_default(WPE_COLLIDER_BOX);
    floor.size = wpe_vec3(60.0f, 1.0f, 60.0f);
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, ground, WPE_COMP_PHYSICS_COLLIDER, &floor, NULL));

    WPE_Model* box_model = NULL;
    WPE_TEST_ASSERT_OK(wpe_model_create_box(wpe_vec3(1, 1, 1), NULL, &box_model));
    const WPE_AssetHandle box = wpe_test_add_model(assets.assets, "procedural/box", box_model);
    const WPE_AssetHandle figure = wpe_test_load_model(assets.assets, "meshes/RiggedFigure.glb");
    WPE_TEST_ASSERT_OK(wpe_script_host_register_prefab(host, "box", box));
    WPE_TEST_ASSERT_OK(wpe_script_host_register_prefab(host, "figure", figure));
    wpe_asset_release(assets.assets, box); /* The prefab table holds them now. */
    wpe_asset_release(assets.assets, figure);

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
    float samples[735 * 2]; /* One 30 Hz frame of 22.05 kHz stereo. */
    double energy = 0.0;
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
        WPE_TEST_ASSERT_OK(wpe_audio_update(audio));
        WPE_TEST_ASSERT_OK(wpe_audio_render(audio, samples, 735));
        for (int i = 0; i < 735 * 2; ++i)
            energy += (double)samples[i] * samples[i];
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
    /* Music keeps playing and the scene was audible (impact thuds, chime, music). */
    WPE_TEST_ASSERT(wpe_audio_music_playing(audio));
    WPE_TEST_ASSERT(energy > 1.0);
    WPE_TEST_ASSERT(stats.scripts >= 5);

    /* on_destroy of the tour cleans up what it created (deferred destroys are flushed by the host). */
    const uint32_t before = wpe_ecs_entity_count(ecs);
    WPE_TEST_ASSERT_OK(wpe_ecs_despawn(ecs, tour));
    WPE_TEST_ASSERT_OK(wpe_script_host_update(host, (float)frame_dt));
    WPE_TEST_ASSERT(wpe_ecs_entity_count(ecs) < before - 1);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_script_host_stats(host).errors);

    wpe_script_host_destroy(host);
    wpe_audio_destroy(audio);
    wpe_physics_destroy(physics);
    wpe_ecs_destroy(ecs);
    /* Every asset was released by whoever held it. */
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_asset_manager_stats(assets.assets).assets);
    wpe_test_assets_destroy(&assets);
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_integration_scripts_run_sixty_seconds))
