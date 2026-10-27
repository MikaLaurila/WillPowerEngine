#include "wpe_test.h"

#include <stdio.h>
#include <string.h>

#include "engine/engine.h"
#include "graphics/graphics.h"
#include "scene/scene.h"

/* Scratch files live below the test's working directory. */
#define ROOT "prefab_scratch"
#define PROJECT ROOT "/project"

static void quiet(bool on) {
    wpe_log_set_stderr_enabled(!on);
}

static void write_text(const char* path, const char* text) {
    WPE_File* file = NULL;
    WPE_TEST_ASSERT_OK(wpe_file_open(path, true, &file));
    WPE_TEST_ASSERT_OK(wpe_file_write(file, text, strlen(text)));
    WPE_TEST_ASSERT_OK(wpe_file_close(file));
}

/* Rewrites a file until its modification time changes (the prefab cache compares them, and file system
 * clocks can be coarse). */
static void rewrite(const char* path, const char* text) {
    int64_t before = 0;
    WPE_TEST_ASSERT_OK(wpe_file_modified_time(path, &before));
    int64_t after = before;
    for (int attempt = 0; attempt < 400 && after == before; ++attempt) {
        const double start = wpe_platform_time_seconds();
        while (attempt > 0 && wpe_platform_time_seconds() - start < 0.01)
            continue; /* Let the file system clock advance before writing again. */
        write_text(path, text);
        WPE_TEST_ASSERT_OK(wpe_file_modified_time(path, &after));
    }
    WPE_TEST_ASSERT(after != before);
}

static char* read_text(const char* path) {
    void* data = NULL;
    size_t size = 0;
    WPE_TEST_ASSERT_OK(wpe_file_read_all(path, &data, &size));
    return data;
}

/* A lamp: a root with a point light, a "bulb" child with a mesh, and a "glow" child with a light. */
static const char* const lamp_prefab =
    "{\"format\": \"wpe.prefab\", \"version\": 1, \"entities\": [\n"
    " {\"id\": 1, \"name\": \"lamp\", \"transform\": {}, \"light\": {\"type\": \"point\", \"intensity\": "
    "2}},\n"
    " {\"id\": 2, \"name\": \"bulb\", \"parent\": 1, \"transform\": {\"position\": [0, 1, 0]},\n"
    "  \"mesh\": {\"model\": \"meshes/box.prim\"}},\n"
    " {\"id\": 3, \"name\": \"glow\", \"parent\": 2, \"light\": {\"type\": \"point\", \"intensity\": 1}}]}\n";

/* Two lamps: "left" as the prefab has it, "right" with overrides and a child added to its bulb. */
static const char* const lamps_scene =
    "{\"format\": \"wpe.scene\", \"version\": 1, \"entities\": [\n"
    " {\"id\": 1, \"name\": \"left\", \"transform\": {\"position\": [-2, 0, 0]},\n"
    "  \"prefab\": {\"path\": \"prefabs/lamp.prefab\"}},\n"
    " {\"id\": 2, \"name\": \"right\", \"transform\": {\"position\": [2, 0, 0]},\n"
    "  \"prefab\": {\"path\": \"prefabs/lamp.prefab\", \"ids\": {\"2\": 7},\n"
    "   \"overrides\": {\"1\": {\"light\": {\"type\": \"spot\"}}, \"2\": {\"mesh\": null}, \"3\": null}}},\n"
    " {\"id\": 3, \"name\": \"moth\", \"parent\": 7}]}\n";

static WPE_Engine* open_project(void) {
    WPE_TEST_ASSERT_OK(wpe_directory_create(PROJECT "/scenes"));
    WPE_TEST_ASSERT_OK(wpe_directory_create(PROJECT "/prefabs"));
    WPE_TEST_ASSERT_OK(wpe_directory_create(PROJECT "/meshes"));
    write_text(
        PROJECT "/project.json",
        "{\"format\": \"wpe.project\", \"version\": 1, \"start_scene\": \"scenes/lamps.scene\", "
        "\"display\": {\"width\": 64, \"height\": 48}, \"prefabs\": {\"lamp\": \"prefabs/lamp.prefab\"}}");
    write_text(PROJECT "/meshes/box.prim", "{\"shape\": \"box\"}");
    write_text(PROJECT "/prefabs/lamp.prefab", lamp_prefab);
    write_text(PROJECT "/scenes/lamps.scene", lamps_scene);
    WPE_Engine* engine = NULL;
    WPE_TEST_ASSERT_OK(
        wpe_engine_create(&(WPE_EngineDesc){.project_dir = PROJECT, .headless = true}, &engine));
    return engine;
}

static WPE_Entity find(WPE_Engine* engine, const char* name) {
    WPE_Ecs* ecs = wpe_engine_ecs(engine);
    WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_NAME)});
    while (wpe_ecs_iter_next(&it)) {
        if (strcmp(wpe_scene_get_name(ecs, it.entity), name) == 0) return it.entity;
    }
    return WPE_ENTITY_NULL;
}

/* The first child named `name` of `parent`. */
static WPE_Entity child(WPE_Engine* engine, WPE_Entity parent, const char* name) {
    WPE_Ecs* ecs = wpe_engine_ecs(engine);
    const WPE_Hierarchy* h = wpe_ecs_get_component(ecs, parent, WPE_COMP_HIERARCHY);
    for (WPE_Entity c = h ? h->first_child : WPE_ENTITY_NULL; c != WPE_ENTITY_NULL;) {
        const char* n = wpe_scene_get_name(ecs, c);
        if (n && strcmp(n, name) == 0) return c;
        c = ((const WPE_Hierarchy*)wpe_ecs_get_component(ecs, c, WPE_COMP_HIERARCHY))->next_sibling;
    }
    return WPE_ENTITY_NULL;
}

static const WPE_Light* light_of(WPE_Engine* engine, WPE_Entity e) {
    return wpe_ecs_get_component(wpe_engine_ecs(engine), e, WPE_COMP_LIGHT);
}

static WPE_SceneContext context_of(WPE_Engine* engine) {
    return (WPE_SceneContext){.ecs = wpe_engine_ecs(engine),
                              .assets = wpe_engine_assets(engine),
                              .scripts = wpe_engine_scripts(engine),
                              .prefabs = wpe_engine_prefabs(engine)};
}

static void test_prefab_instances_load_with_overrides(void) {
    WPE_Engine* engine = open_project();
    WPE_Ecs* ecs = wpe_engine_ecs(engine);
    const WPE_Entity left = find(engine, "left"), right = find(engine, "right");
    WPE_TEST_ASSERT(left && right);
    /* The instance keeps its own name and transform; the prefab gives the rest. */
    WPE_TEST_ASSERT_NEAR(
        -2.0, ((const WPE_Transform*)wpe_ecs_get_component(ecs, left, WPE_COMP_TRANSFORM))->position.x, 0);
    WPE_TEST_ASSERT_EQ_INT(WPE_LIGHT_POINT, light_of(engine, left)->type);
    WPE_TEST_ASSERT_NEAR(2.0, light_of(engine, left)->intensity, 1e-6);
    const WPE_Entity left_bulb = child(engine, left, "bulb");
    WPE_TEST_ASSERT(wpe_ecs_has_component(ecs, left_bulb, WPE_COMP_MESH));
    WPE_TEST_ASSERT(child(engine, left_bulb, "glow") != WPE_ENTITY_NULL);
    WPE_TEST_ASSERT_EQ_UINT(left, wpe_prefab_root(ecs, left_bulb));
    WPE_TEST_ASSERT_EQ_STR("prefabs/lamp.prefab", wpe_prefab_path(ecs, left_bulb));
    /* Overrides: a spot light on the root, no mesh on the bulb, no glow; the moth hangs on the bulb. */
    WPE_TEST_ASSERT_EQ_INT(WPE_LIGHT_SPOT, light_of(engine, right)->type);
    const WPE_Entity right_bulb = child(engine, right, "bulb");
    WPE_TEST_ASSERT(right_bulb && !wpe_ecs_has_component(ecs, right_bulb, WPE_COMP_MESH));
    WPE_TEST_ASSERT_EQ_UINT(WPE_ENTITY_NULL, child(engine, right_bulb, "glow"));
    WPE_TEST_ASSERT_EQ_UINT(right_bulb, wpe_scene_get_parent(ecs, find(engine, "moth")));
    WPE_TEST_ASSERT_EQ_UINT(WPE_ENTITY_NULL, wpe_prefab_root(ecs, find(engine, "moth")));
    WPE_TEST_ASSERT_EQ_UINT(3 + 2 + 1, wpe_ecs_entity_count(ecs));

    const WPE_SceneContext context = context_of(engine);
    WPE_TEST_ASSERT(!wpe_prefab_is_overridden(&context, left, NULL));
    WPE_TEST_ASSERT(wpe_prefab_is_overridden(&context, right, "light"));
    WPE_TEST_ASSERT(!wpe_prefab_is_overridden(&context, right, "transform")); /* The instance's own. */
    WPE_TEST_ASSERT(wpe_prefab_is_overridden(&context, right_bulb, "mesh"));
    WPE_TEST_ASSERT(!wpe_prefab_is_overridden(&context, right_bulb, "transform"));
    wpe_engine_destroy(engine);
}

static void test_prefab_scenes_save_references_and_round_trip(void) {
    WPE_Engine* engine = open_project();
    /* A change made in the world (not through any prefab API) becomes an override when saving. */
    WPE_Light* light = wpe_ecs_get_component(wpe_engine_ecs(engine), find(engine, "glow"), WPE_COMP_LIGHT);
    light->intensity = 3.5f;
    WPE_TEST_ASSERT_OK(wpe_engine_save_scene(engine, ROOT "/saved.scene"));
    char* saved = read_text(ROOT "/saved.scene");
    WPE_TEST_ASSERT(strstr(saved, "\"path\": \"prefabs/lamp.prefab\"") != NULL);
    WPE_TEST_ASSERT(strstr(saved, "\"bulb\"") == NULL); /* Members come from the prefab, not the scene. */
    WPE_TEST_ASSERT(strstr(saved, "\"intensity\": 3.5") != NULL);
    WPE_TEST_ASSERT(strstr(saved, "\"3\": null") != NULL);
    wpe_engine_destroy(engine);

    /* Loading the saved scene and saving it again reproduces it exactly. */
    engine = open_project();
    write_text(PROJECT "/scenes/lamps.scene", saved);
    WPE_TEST_ASSERT_OK(wpe_engine_load_scene(engine, "scenes/lamps.scene"));
    WPE_TEST_ASSERT_NEAR(3.5, light_of(engine, find(engine, "glow"))->intensity, 1e-6);
    WPE_TEST_ASSERT_OK(wpe_engine_save_scene(engine, ROOT "/again.scene"));
    char* again = read_text(ROOT "/again.scene");
    WPE_TEST_ASSERT_EQ_STR(saved, again);
    wpe_free(again);
    wpe_free(saved);
    wpe_engine_destroy(engine);
}

static void test_prefab_edits_reach_instances(void) {
    WPE_Engine* engine = open_project();
    /* The prefab changes on disk: instances pick it up when loaded again, overrides stay. */
    char* text = read_text(PROJECT "/prefabs/lamp.prefab");
    char* at = strstr(text, "\"intensity\": 2");
    WPE_TEST_ASSERT(at != NULL);
    at[13] = '5';
    rewrite(PROJECT "/prefabs/lamp.prefab", text);
    wpe_free(text);
    WPE_TEST_ASSERT_OK(wpe_engine_load_scene(engine, "scenes/lamps.scene"));
    WPE_TEST_ASSERT_NEAR(5.0, light_of(engine, find(engine, "left"))->intensity, 1e-6);
    WPE_TEST_ASSERT_EQ_INT(WPE_LIGHT_SPOT, light_of(engine, find(engine, "right"))->type);
    wpe_engine_destroy(engine);
}

static void test_prefab_save_apply_revert_unpack(void) {
    WPE_Engine* engine = open_project();
    WPE_Ecs* ecs = wpe_engine_ecs(engine);
    const WPE_SceneContext context = context_of(engine);
    WPE_TEST_ASSERT_OK(wpe_scene_clear(ecs));

    /* A plain tree becomes a prefab, and an instance of it in place. */
    WPE_JsonDocument* doc = NULL;
    const char* crate =
        "{\"name\": \"crate\", \"transform\": {\"position\": [4, 1, 0], \"scale\": [2, 2, 2]}, "
        "\"mesh\": {\"model\": \"meshes/box.prim\"}, \"collider\": {\"shape\": \"box\"}}";
    WPE_TEST_ASSERT_OK(wpe_json_parse(crate, strlen(crate), &doc, NULL));
    WPE_Entity original = WPE_ENTITY_NULL;
    WPE_TEST_ASSERT_OK(wpe_scene_create_entity(&context, wpe_json_root(doc), &original));
    wpe_json_free(doc);
    WPE_TEST_ASSERT_OK(
        wpe_prefab_save_file(&context, original, "prefabs/crate.prefab", PROJECT "/prefabs/crate.prefab"));
    WPE_TEST_ASSERT_EQ_UINT(original, wpe_prefab_root(ecs, original));
    WPE_TEST_ASSERT(!wpe_prefab_is_overridden(&context, original, NULL));
    char* file = read_text(PROJECT "/prefabs/crate.prefab");
    WPE_TEST_ASSERT(strstr(file, "\"position\": [0, 0, 0]") != NULL); /* The root sits at the origin... */
    WPE_TEST_ASSERT(strstr(file, "\"scale\": [2, 2, 2]") != NULL);    /* ...keeping its scale. */
    wpe_free(file);

    WPE_Entity copy = WPE_ENTITY_NULL;
    WPE_TEST_ASSERT_OK(wpe_prefab_instantiate(&context, "prefabs/crate.prefab", &copy));
    WPE_TEST_ASSERT(wpe_ecs_has_component(ecs, copy, WPE_COMP_PHYSICS_COLLIDER));

    /* A changed component is an override until reverted. */
    WPE_Collider* collider = wpe_ecs_get_component(ecs, copy, WPE_COMP_PHYSICS_COLLIDER);
    collider->friction = 0.1f;
    WPE_TEST_ASSERT(wpe_prefab_is_overridden(&context, copy, "collider"));
    WPE_TEST_ASSERT(!wpe_prefab_is_overridden(&context, copy, "mesh"));
    WPE_TEST_ASSERT_OK(wpe_prefab_revert(&context, copy, "collider", NULL));
    WPE_TEST_ASSERT(!wpe_prefab_is_overridden(&context, copy, NULL));

    /* Applying writes the instance into the prefab: new instances get the change. */
    collider = wpe_ecs_get_component(ecs, copy, WPE_COMP_PHYSICS_COLLIDER);
    collider->friction = 0.25f;
    WPE_TEST_ASSERT_OK(
        wpe_prefab_save_file(&context, copy, "prefabs/crate.prefab", PROJECT "/prefabs/crate.prefab"));
    WPE_TEST_ASSERT(!wpe_prefab_is_overridden(&context, copy, NULL));
    /* Other instances keep the prefab as they were made from it until they are made again. */
    WPE_TEST_ASSERT_NEAR(
        0.6, ((const WPE_Collider*)wpe_ecs_get_component(ecs, original, WPE_COMP_PHYSICS_COLLIDER))->friction,
        1e-6);
    WPE_TEST_ASSERT(!wpe_prefab_is_overridden(&context, original, NULL));
    WPE_Entity third = WPE_ENTITY_NULL;
    WPE_TEST_ASSERT_OK(wpe_prefab_instantiate(&context, "prefabs/crate.prefab", &third));
    WPE_TEST_ASSERT_NEAR(
        0.25, ((const WPE_Collider*)wpe_ecs_get_component(ecs, third, WPE_COMP_PHYSICS_COLLIDER))->friction,
        1e-6);

    /* Reverting a whole instance rebuilds it in place: same name, parent and transform; added children go. */
    const WPE_Entity extra = wpe_ecs_spawn(ecs);
    WPE_TEST_ASSERT_OK(wpe_scene_set_parent(ecs, extra, original, false));
    WPE_Entity rebuilt = WPE_ENTITY_NULL;
    WPE_TEST_ASSERT_OK(wpe_prefab_revert(&context, original, NULL, &rebuilt));
    WPE_TEST_ASSERT(rebuilt != original && !wpe_ecs_is_alive(ecs, original) && !wpe_ecs_is_alive(ecs, extra));
    WPE_TEST_ASSERT_EQ_STR("crate", wpe_scene_get_name(ecs, rebuilt));
    WPE_TEST_ASSERT_NEAR(
        4.0, ((const WPE_Transform*)wpe_ecs_get_component(ecs, rebuilt, WPE_COMP_TRANSFORM))->position.x, 0);
    WPE_TEST_ASSERT(!wpe_prefab_is_overridden(&context, rebuilt, NULL));

    /* Unpacking leaves plain entities. */
    WPE_TEST_ASSERT_OK(wpe_prefab_unpack(ecs, rebuilt));
    WPE_TEST_ASSERT_EQ_UINT(WPE_ENTITY_NULL, wpe_prefab_root(ecs, rebuilt));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND, wpe_prefab_unpack(ecs, rebuilt));

    /* Scripts instantiate prefabs by path or by the project's name for them. */
    const uint32_t before = wpe_ecs_entity_count(ecs);
    WPE_TEST_ASSERT_OK(wpe_script_host_run(wpe_engine_scripts(engine), "test",
                                           "local a = wpe.instantiate('lamp', vector.create(0, 5, 0))\n"
                                           "assert(wpe.get_position(a).y == 5)\n"
                                           "wpe.instantiate('prefabs/crate.prefab')\n"));
    WPE_TEST_ASSERT_EQ_UINT(before + 3 + 1, wpe_ecs_entity_count(ecs));
    wpe_engine_destroy(engine);
}

static void test_prefab_mistakes_are_reported(void) {
    WPE_Engine* engine = open_project();
    WPE_Ecs* ecs = wpe_engine_ecs(engine);
    const uint32_t count = wpe_ecs_entity_count(ecs);
    static const struct {
        const char* prefab;
        const char* overrides;
    } cases[] = {
        /* Two roots. */
        {"{\"format\": \"wpe.prefab\", \"version\": 1, \"entities\": [{\"id\": 1}, {\"id\": 2}]}", "null"},
        /* A prefab inside a prefab. */
        {"{\"format\": \"wpe.prefab\", \"version\": 1, \"entities\": [{\"id\": 1, \"prefab\": {\"path\": "
         "\"prefabs/lamp.prefab\"}}]}",
         "null"},
        /* Not a prefab. */
        {"{\"format\": \"wpe.scene\", \"version\": 1, \"entities\": [{\"id\": 1}]}", "null"},
        /* Overrides of an entity the prefab doesn't have, of the root's own transform, or removing the root.
         */
        {NULL, "{\"9\": {\"light\": null}}"},
        {NULL, "{\"1\": {\"transform\": {}}}"},
        {NULL, "{\"1\": null}"},
        {NULL, "{\"2\": {\"colour\": 1}}"},
    };
    quiet(true);
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        /* A file per case, so no case sees another's (cached) prefab. */
        char path[64] = "prefabs/lamp.prefab", os_path[128];
        if (cases[i].prefab) {
            snprintf(path, sizeof path, "prefabs/bad%zu.prefab", i);
            snprintf(os_path, sizeof os_path, PROJECT "/%s", path);
            write_text(os_path, cases[i].prefab);
        }
        char scene[512];
        snprintf(
            scene, sizeof scene,
            "{\"format\": \"wpe.scene\", \"version\": 1, \"entities\": [{\"id\": 1, \"name\": \"keep\"}, "
            "{\"id\": 2, \"prefab\": {\"path\": \"%s\", \"overrides\": %s}}]}",
            path, cases[i].overrides);
        const WPE_SceneContext context = context_of(engine);
        WPE_TEST_ASSERT_MSG(wpe_scene_load(&context, scene, strlen(scene), "bad", NULL, NULL) !=
                                WPE_RESULT_OK,
                            "case %zu", i);
        WPE_TEST_ASSERT_EQ_UINT(count, wpe_ecs_entity_count(ecs)); /* Nothing was left behind. */
    }
    /* Instance entries take no components of their own, and missing prefabs are reported. */
    const char* components =
        "{\"format\": \"wpe.scene\", \"version\": 1, \"entities\": [{\"id\": 1, \"light\": {}, "
        "\"prefab\": {\"path\": \"prefabs/lamp.prefab\"}}]}";
    const WPE_SceneContext context = context_of(engine);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_PARSE,
                           wpe_scene_load(&context, components, strlen(components), "c", NULL, NULL));
    WPE_Entity root = WPE_ENTITY_NULL;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND,
                           wpe_prefab_instantiate(&context, "prefabs/none.prefab", &root));
    /* A member that isn't the root can't be saved as a prefab of its own. */
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG,
                           wpe_prefab_save_file(&context, child(engine, find(engine, "left"), "bulb"),
                                                "prefabs/bulb.prefab", PROJECT "/prefabs/bulb.prefab"));
    quiet(false);
    WPE_TEST_ASSERT_EQ_UINT(count, wpe_ecs_entity_count(ecs));
    wpe_engine_destroy(engine);
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_prefab_instances_load_with_overrides),
              WPE_TEST_CASE(test_prefab_scenes_save_references_and_round_trip),
              WPE_TEST_CASE(test_prefab_edits_reach_instances),
              WPE_TEST_CASE(test_prefab_save_apply_revert_unpack),
              WPE_TEST_CASE(test_prefab_mistakes_are_reported))
