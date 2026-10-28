/*
 * Scene playback: the master test scene (assets/scenes/test_full.scene) run by the engine for 60
 * simulated seconds, headless. The scene holds every component type, model instances, an
 * entity without components and extreme transforms; its scripts exercise the whole scripting API
 * (scripts/api_tour.luau), mass spawns, prefab instantiate/destroy cycles, physics, audio and
 * camera switches, with post-processing on. It must finish without a single script error or leak.
 */
#include "wpe_test.h"

#include <stdio.h>

#include "engine/engine.h"
#include "platform/platform.h"
#include "scene/scene.h"

static void test_full_scene_runs_sixty_seconds(void) {
    WPE_Engine* engine = NULL;
    WPE_TEST_ASSERT_OK(wpe_directory_create("saves")); /* wpe.save_data's files, in the build tree. */
    WPE_TEST_ASSERT_OK(wpe_engine_create(&(WPE_EngineDesc){.project_dir = WPE_TEST_ASSETS_DIR,
                                                           .scene = "scenes/test_full.scene",
                                                           .headless = true,
                                                           .workers = 4,
                                                           .save_dir = "saves"},
                                         &engine));
    WPE_Ecs* ecs = wpe_engine_ecs(engine);

    /* At least one entity per component type, an entity with no components, three post effects. */
    const WPE_ComponentType all[] = {WPE_COMP_TRANSFORM,
                                     WPE_COMP_HIERARCHY,
                                     WPE_COMP_NAME,
                                     WPE_COMP_MESH,
                                     WPE_COMP_LIGHT,
                                     WPE_COMP_CAMERA,
                                     WPE_COMP_SCRIPT,
                                     WPE_COMP_PHYSICS_RIGIDBODY,
                                     WPE_COMP_PHYSICS_COLLIDER,
                                     WPE_COMP_AUDIO_SOURCE,
                                     WPE_COMP_ANIMATOR};
    for (size_t i = 0; i < sizeof all / sizeof all[0]; ++i)
        WPE_TEST_ASSERT_MSG(wpe_ecs_component_count(ecs, all[i]) > 0, "no component of type %u", all[i]);
    uint32_t bare = 0;
    WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){0});
    while (wpe_ecs_iter_next(&it))
        bare += wpe_ecs_component_mask(ecs, it.entity) == 0;
    WPE_TEST_ASSERT_EQ_UINT(1, bare);
    const WPE_SceneEnvironment* env = wpe_engine_environment(engine);
    uint32_t effects = 0;
    for (uint32_t i = 0; i < env->postfx_count; ++i)
        effects += env->postfx[i].enabled;
    WPE_TEST_ASSERT(effects >= 3);

    /* 60 seconds at 30 frames per second with the project's 60 Hz fixed step. */
    uint32_t max_entities = 0;
    for (int frame = 0; frame < 1800; ++frame) {
        WPE_TEST_ASSERT_OK(wpe_engine_frame(engine, 1.0 / 30.0));
        WPE_TEST_ASSERT(wpe_engine_running(engine));
        const uint32_t count = wpe_ecs_entity_count(ecs);
        max_entities = count > max_entities ? count : max_entities;
    }
    WPE_TEST_ASSERT_OK(wpe_engine_screenshot(engine, "test_full_scene.png")); /* For inspection. */

    const WPE_ScriptStats stats = wpe_script_host_stats(wpe_engine_scripts(engine));
    WPE_TEST_ASSERT_MSG(stats.errors == 0, "%llu script error(s)", (unsigned long long)stats.errors);
    WPE_TEST_ASSERT_EQ_UINT(0, stats.failed);
    /* Both callbacks of the API tour ran every frame and fixed step (the first frame's fixed steps
     * precede on_start). */
    uint32_t tours = 0, suites = 0, checks = 0, cycles = 0;
    it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_NAME)});
    while (wpe_ecs_iter_next(&it)) {
        const char* name = wpe_scene_get_name(ecs, it.entity);
        tours += strcmp(name, "tour:1799:3598") == 0;
        suites += sscanf(name, "suite:%u:%u", &checks, &cycles) == 2;
    }
    WPE_TEST_ASSERT_EQ_UINT(1, tours);
    WPE_TEST_ASSERT_EQ_UINT(1, suites);
    WPE_TEST_ASSERT_MSG(checks >= 20 && cycles == 60, "suite reported %u checks, %u cycles", checks, cycles);
    /* 2000 entities were spawned and destroyed; the spawner's boxes keep the population bounded. */
    WPE_TEST_ASSERT(max_entities > 2000 && max_entities < 2300);
    WPE_TEST_ASSERT(wpe_ecs_entity_count(ecs) < 300);
    WPE_TEST_ASSERT(wpe_physics_stats(wpe_engine_physics(engine)).bodies > 5);
    WPE_TEST_ASSERT(wpe_audio_music_playing(wpe_engine_audio(engine)));
    WPE_TEST_ASSERT_EQ_UINT(1800, wpe_engine_stats(engine).frames);
    wpe_engine_destroy(engine);
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_full_scene_runs_sixty_seconds))
