#include "wpe_test.h"
#include "wpe_test_assets.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#include "engine/engine.h"
#include "graphics/graphics.h"
#include "scene/scene.h"

/* Scratch files live below the test's working directory. */
#define ROOT "engine_scratch"

static void quiet(bool on) {
    wpe_log_set_stderr_enabled(!on);
}

static void write_text(const char* path, const char* text) {
    WPE_File* file = NULL;
    WPE_TEST_ASSERT_OK(wpe_file_open(path, true, &file));
    WPE_TEST_ASSERT_OK(wpe_file_write(file, text, strlen(text)));
    WPE_TEST_ASSERT_OK(wpe_file_close(file));
}

/* ---- Project files --------------------------------------------------------------------------------- */

static void test_engine_project_from_assets(void) {
    WPE_TestAssets t = wpe_test_assets_create(WPE_TEST_ASSETS_DIR, 0);
    static WPE_ProjectSettings p;
    WPE_TEST_ASSERT_OK(wpe_project_load(t.vfs, &p));
    WPE_TEST_ASSERT_EQ_STR("WillPowerEngine Demo", p.name);
    WPE_TEST_ASSERT_EQ_STR("scenes/title.scene", p.start_scene);
    WPE_TEST_ASSERT_EQ_STR("fonts/small.png", p.font);
    WPE_TEST_ASSERT(p.width == 320 && p.height == 240 && p.scale == 3 && p.vsync && !p.fullscreen);
    WPE_TEST_ASSERT_NEAR(60.0, p.fixed_rate, 0.0);
    WPE_TEST_ASSERT_NEAR(0.7, p.volumes[WPE_AUDIO_BUS_MUSIC], 1e-6);
    WPE_TEST_ASSERT_EQ_UINT(0, p.input.keyboard_player); /* "1" in the file: players are 1-based there. */
    WPE_TEST_ASSERT_EQ_UINT(4, p.prefab_count);
    WPE_TEST_ASSERT_EQ_STR("crate", p.prefabs[1].name);
    WPE_TEST_ASSERT_EQ_STR("meshes/crate.prim", p.prefabs[1].path);

    const WPE_InputActionDesc* jump = NULL;
    const WPE_InputActionDesc* move_y = NULL;
    for (uint32_t i = 0; i < p.action_count; ++i) {
        if (strcmp(p.actions[i].name, "jump") == 0) jump = &p.actions[i];
        if (strcmp(p.actions[i].name, "move_y") == 0) move_y = &p.actions[i];
    }
    WPE_TEST_ASSERT(jump && jump->binding_count == 2);
    WPE_TEST_ASSERT(jump->bindings[0].source == WPE_INPUT_SOURCE_KEY &&
                    jump->bindings[0].code == WPE_KEY_SPACE);
    WPE_TEST_ASSERT(jump->bindings[1].source == WPE_INPUT_SOURCE_PAD_BUTTON &&
                    jump->bindings[1].code == WPE_PAD_SOUTH);
    WPE_TEST_ASSERT(move_y && move_y->bindings[4].source == WPE_INPUT_SOURCE_PAD_AXIS);
    WPE_TEST_ASSERT(move_y->bindings[4].code == WPE_PAD_LEFT_Y && move_y->bindings[4].scale == -1.0f);
    wpe_test_assets_destroy(&t);
}

static void test_engine_project_rejects_mistakes(void) {
    static const struct {
        const char* text;
        WPE_Result expected;
    } cases[] = {
        {"{}", WPE_RESULT_ERROR_PARSE},
        {"{\"format\": \"wpe.scene\", \"version\": 1}", WPE_RESULT_ERROR_PARSE},
        {"{\"format\": \"wpe.project\"}", WPE_RESULT_ERROR_PARSE},
        {"{\"format\": \"wpe.project\", \"version\": 2}", WPE_RESULT_ERROR_UNSUPPORTED},
        {"{\"format\": \"wpe.project\", \"version\": 1, \"colour\": 1}", WPE_RESULT_ERROR_PARSE},
        {"{\"format\": \"wpe.project\", \"version\": 1, \"display\": {\"width\": 5}}",
         WPE_RESULT_ERROR_PARSE},
        {"{\"format\": \"wpe.project\", \"version\": 1, \"display\": {\"width\": 320.5}}",
         WPE_RESULT_ERROR_PARSE},
        {"{\"format\": \"wpe.project\", \"version\": 1, \"input\": {\"actions\": {\"a\": [{\"key\": "
         "\"space\", "
         "\"pad_button\": \"a\"}]}}}",
         WPE_RESULT_ERROR_PARSE},
        {"{\"format\": \"wpe.project\", \"version\": 1, \"input\": {\"actions\": {\"a\": [{\"key\": "
         "\"spacebar\"}]}}}",
         WPE_RESULT_ERROR_PARSE},
        {"{\"format\": \"wpe.project\", \"version\": 1, \"input\": {\"actions\": {\"a\": [{\"key\": \"x\", "
         "\"range\": "
         "\"positive\"}]}}}",
         WPE_RESULT_ERROR_PARSE},
        {"{\"format\": \"wpe.project\", \"version\": 1, \"input\": {\"keyboard_player\": 0}}",
         WPE_RESULT_ERROR_PARSE},
        {"{\"format\": \"wpe.project\", \"version\": 1, \"input\": {\"stick_deadzone\": 0.5, "
         "\"stick_outer\": 0.4}}",
         WPE_RESULT_ERROR_PARSE},
        {"{\"format\": \"wpe.project\", \"version\": 1, \"prefabs\": {\"x\": \"../escape.glb\"}}",
         WPE_RESULT_ERROR_PARSE},
        {"{\"format\": \"wpe.project\", \"version\": 1, \"start_scene\": \"/abs.scene\"}",
         WPE_RESULT_ERROR_PARSE},
        {"{\"format\": \"wpe.project\", \"version\": 1,}", WPE_RESULT_ERROR_PARSE},
    };
    static WPE_ProjectSettings p;
    quiet(true);
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        const WPE_Result result = wpe_project_parse(cases[i].text, strlen(cases[i].text), "case", &p);
        WPE_TEST_ASSERT_MSG(result == cases[i].expected, "case %zu: got %s", i, wpe_result_to_string(result));
        WPE_TEST_ASSERT_EQ_STR("WillPowerEngine Game", p.name); /* Defaults after a failure. */
        WPE_TEST_ASSERT_EQ_UINT(0, p.action_count);
    }
    quiet(false);
    /* The minimal valid project. */
    const char* minimal = "{\"format\": \"wpe.project\", \"version\": 1}";
    WPE_TEST_ASSERT_OK(wpe_project_parse(minimal, strlen(minimal), NULL, &p));
    WPE_TEST_ASSERT_EQ_UINT(320, p.width);
}

static char* write_project_text(const WPE_ProjectSettings* p, WPE_JsonWriter** out_writer) {
    WPE_TEST_ASSERT_OK(wpe_json_writer_create(out_writer));
    WPE_TEST_ASSERT_OK(wpe_project_write(p, *out_writer));
    const char* text = NULL;
    WPE_TEST_ASSERT_OK(wpe_json_writer_finish(*out_writer, &text, NULL));
    return (char*)text;
}

static void test_engine_project_write_round_trip(void) {
    WPE_TestAssets t = wpe_test_assets_create(WPE_TEST_ASSETS_DIR, 0);
    static WPE_ProjectSettings a, b;
    WPE_TEST_ASSERT_OK(wpe_project_load(t.vfs, &a));
    WPE_JsonWriter* w1 = NULL;
    WPE_JsonWriter* w2 = NULL;
    const char* first = write_project_text(&a, &w1);
    WPE_TEST_ASSERT_OK(wpe_project_parse(first, strlen(first), "written", &b));
    const char* second = write_project_text(&b, &w2);
    WPE_TEST_ASSERT_EQ_STR(first, second);
    WPE_TEST_ASSERT_EQ_UINT(a.action_count, b.action_count);
    WPE_TEST_ASSERT_EQ_UINT(a.prefab_count, b.prefab_count);
    wpe_json_writer_destroy(w1);
    wpe_json_writer_destroy(w2);
    wpe_test_assets_destroy(&t);
}

/* ---- Scene files ------------------------------------------------------------------------------------ */

typedef struct World {
    WPE_TestAssets assets;
    WPE_Ecs* ecs;
    WPE_Physics* physics;
    WPE_Audio* audio;
    WPE_Ui* ui;
    WPE_ScriptHost* scripts;
} World;

static World make_world(void) {
    World w = {.assets = wpe_test_assets_create(WPE_TEST_ASSETS_DIR, 16)};
    WPE_TEST_ASSERT_OK(wpe_ecs_create(&w.ecs));
    WPE_TEST_ASSERT_OK(wpe_scene_register_components(w.ecs));
    WPE_TEST_ASSERT_OK(wpe_graphics_register_components(w.ecs, w.assets.assets));
    WPE_TEST_ASSERT_OK(wpe_prefab_register(w.ecs)); /* The scene holds a prefab instance. */
    WPE_TEST_ASSERT_OK(wpe_physics_create(
        &(WPE_PhysicsDesc){.ecs = w.ecs, .assets = w.assets.assets, .gravity = {{0, -9.81f, 0}}},
        &w.physics));
    WPE_TEST_ASSERT_OK(wpe_audio_create(
        &(WPE_AudioDesc){.ecs = w.ecs, .assets = w.assets.assets, .no_device = true}, &w.audio));
    WPE_TEST_ASSERT_OK(wpe_ui_create(&(WPE_UiDesc){.ecs = w.ecs, .assets = w.assets.assets}, &w.ui));
    WPE_TEST_ASSERT_OK(wpe_script_host_create(
        &(WPE_ScriptHostDesc){
            .ecs = w.ecs, .assets = w.assets.assets, .physics = w.physics, .audio = w.audio, .ui = w.ui},
        &w.scripts));
    return w;
}

static void free_world(World* w) {
    wpe_script_host_destroy(w->scripts);
    wpe_audio_destroy(w->audio);
    wpe_physics_destroy(w->physics);
    wpe_ecs_destroy(w->ecs);
    wpe_ui_destroy(w->ui);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_asset_manager_stats(w->assets.assets).assets); /* Nothing leaked. */
    wpe_test_assets_destroy(&w->assets);
}

static WPE_SceneContext context_of(World* w) {
    return (WPE_SceneContext){.ecs = w->ecs, .assets = w->assets.assets, .scripts = w->scripts};
}

/* Saves a world; returns the text (owned by *out_writer). */
static const char* save_text(const WPE_SceneContext* ctx, const WPE_SceneEnvironment* env, WPE_Entity camera,
                             WPE_JsonWriter** out_writer) {
    WPE_TEST_ASSERT_OK(wpe_json_writer_create(out_writer));
    WPE_TEST_ASSERT_OK(wpe_scene_save(ctx, env, camera, *out_writer));
    const char* text = NULL;
    WPE_TEST_ASSERT_OK(wpe_json_writer_finish(*out_writer, &text, NULL));
    return text;
}

/* Compares two saved scenes; on a difference both are written next to the test for diffing. */
static void expect_same_text(const char* expected, const char* actual, const char* label) {
    if (strcmp(expected, actual) == 0) return;
    char path[128];
    snprintf(path, sizeof path, ROOT "/%s_expected.scene", label);
    write_text(path, expected);
    snprintf(path, sizeof path, ROOT "/%s_actual.scene", label);
    write_text(path, actual);
    WPE_TEST_ASSERT_MSG(false, "saved scenes differ (%s): see " ROOT "/%s_*.scene", label, label);
}

static uint32_t count_of(const World* w, WPE_ComponentType type) {
    return wpe_ecs_component_count(w->ecs, type);
}

static void test_engine_scene_round_trip(void) {
    /* The master test scene uses every component type, model instances, script properties and the
     * whole environment. Saved, reloaded into a fresh world and saved again, the text is identical. */
    World a = make_world();
    const WPE_SceneContext ctx_a = context_of(&a);
    WPE_SceneEnvironment env;
    WPE_Entity camera = WPE_ENTITY_NULL;
    WPE_TEST_ASSERT_OK(wpe_scene_load_asset(&ctx_a, "scenes/test_full.scene", &env, &camera));
    WPE_TEST_ASSERT(camera != WPE_ENTITY_NULL);
    WPE_TEST_ASSERT_EQ_STR("main camera", wpe_scene_get_name(a.ecs, camera));
    WPE_TEST_ASSERT_EQ_UINT(6, env.postfx_count);
    WPE_TEST_ASSERT(env.postfx[5].kind == WPE_POSTFX_DEPTH_OF_FIELD &&
                    env.postfx[5].depth_of_field.falloff == 8.0f);
    WPE_TEST_ASSERT(!env.postfx[4].enabled && env.postfx[2].kind == WPE_POSTFX_PS1 &&
                    env.postfx[2].ps1.levels == 32);
    WPE_TEST_ASSERT_EQ_STR("music/ambient.wav", env.music);
    WPE_TEST_ASSERT_EQ_INT(WPE_FOG_EXPONENTIAL, env.render.fog.mode);
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
                                     WPE_COMP_ANIMATOR,
                                     WPE_COMP_UI,
                                     WPE_COMP_SPRITE,
                                     WPE_COMP_TEXT,
                                     WPE_COMP_BUTTON,
                                     WPE_COMP_BILLBOARD,
                                     WPE_COMP_PARTICLES,
                                     WPE_COMP_CHARACTER,
                                     WPE_COMP_TOGGLE,
                                     WPE_COMP_SLIDER,
                                     WPE_COMP_TRAIL,
                                     WPE_COMP_SLASH,
                                     WPE_COMP_WATER};
    for (size_t i = 0; i < sizeof all / sizeof all[0]; ++i)
        WPE_TEST_ASSERT_MSG(count_of(&a, all[i]) > 0, "no component of type %u", all[i]);
    WPE_TEST_ASSERT_EQ_UINT(2, count_of(&a, WPE_COMP_CAMERA));

    WPE_JsonWriter* w1 = NULL;
    const char* first = save_text(&ctx_a, &env, camera, &w1);
    const uint32_t entities = wpe_ecs_entity_count(a.ecs);

    World b = make_world();
    const WPE_SceneContext ctx_b = context_of(&b);
    WPE_SceneEnvironment env_b;
    WPE_Entity camera_b = WPE_ENTITY_NULL;
    WPE_TEST_ASSERT_OK(wpe_scene_load(&ctx_b, first, strlen(first), "saved", &env_b, &camera_b));
    WPE_TEST_ASSERT_EQ_UINT(entities, wpe_ecs_entity_count(b.ecs));
    for (size_t i = 0; i < sizeof all / sizeof all[0]; ++i)
        WPE_TEST_ASSERT_EQ_UINT(count_of(&a, all[i]), count_of(&b, all[i]));
    WPE_JsonWriter* w2 = NULL;
    const char* second = save_text(&ctx_b, &env_b, camera_b, &w2);
    expect_same_text(first, second, "reloaded");

    /* Scenes switch by clearing; a load-save cycle in a reused world keeps the file stable too. */
    WPE_TEST_ASSERT_OK(wpe_scene_clear(b.ecs));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_ecs_entity_count(b.ecs));
    WPE_TEST_ASSERT_OK(wpe_scene_load(&ctx_b, first, strlen(first), "saved", &env_b, &camera_b));
    WPE_JsonWriter* w3 = NULL;
    const char* third = save_text(&ctx_b, &env_b, camera_b, &w3);
    expect_same_text(first, third, "reused");

    /* Spot checks of reloaded data. */
    WPE_EcsIter it = wpe_ecs_query(b.ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_PHYSICS_COLLIDER)});
    uint32_t mesh_colliders = 0, triggers = 0;
    while (wpe_ecs_iter_next(&it)) {
        const WPE_Collider* c = wpe_ecs_iter_get(&it, WPE_COMP_PHYSICS_COLLIDER);
        mesh_colliders += c->shape == WPE_COLLIDER_MESH;
        triggers += c->is_trigger;
    }
    WPE_TEST_ASSERT(mesh_colliders == 1 && triggers == 1);
    it = wpe_ecs_query(b.ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_TEXT)});
    uint32_t wrapped = 0;
    while (wpe_ecs_iter_next(&it)) {
        const WPE_Text* text = wpe_ecs_iter_get(&it, WPE_COMP_TEXT);
        if (strcmp(text->text, "UI over 3D") != 0) continue;
        wrapped += text->wrap && text->align_x == WPE_UI_ALIGN_CENTER && text->glyphs != NULL;
    }
    WPE_TEST_ASSERT_EQ_UINT(1, wrapped);
    it = wpe_ecs_query(b.ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_SPRITE)});
    while (wpe_ecs_iter_next(&it)) {
        const WPE_Sprite* sprite = wpe_ecs_iter_get(&it, WPE_COMP_SPRITE);
        WPE_TEST_ASSERT(sprite->texture != NULL && sprite->slice.x == 4.0f);
    }

    wpe_json_writer_destroy(w1);
    wpe_json_writer_destroy(w2);
    wpe_json_writer_destroy(w3);
    free_world(&b);
    free_world(&a);
}

static void test_engine_scene_rejects_mistakes_and_rolls_back(void) {
    /* A valid entity is followed by a broken one: nothing may remain from a failed load. */
    static const char* const bad[] = {
        "{\"id\": 2, \"colour\": 1}",
        "{\"id\": 1}",                 /* Duplicate id. */
        "{\"id\": 2, \"parent\": 99}", /* Dangling reference. */
        "{\"id\": 2, \"parent\": 2}",  /* Cycle. */
        "{\"id\": 2.5}",
        "{\"name\": \"no id\"}",
        "{\"id\": 2, \"mesh\": {\"model\": \"meshes/box.prim\", \"mesh\": 5}}", /* Mesh index out of range. */
        "{\"id\": 2, \"mesh\": {\"model\": \"meshes/missing.glb\"}}",
        "{\"id\": 2, \"mesh\": {\"model\": \"meshes/box.prim\", \"skin\": 0, \"joints\": []}}",
        "{\"id\": 2, \"collider\": {\"shape\": \"convex\"}}",
        "{\"id\": 2, \"collider\": {\"shape\": \"capsule\", \"size\": [1, 1, 0]}}",
        "{\"id\": 2, \"collider\": {\"shape\": \"box\", \"model\": \"meshes/box.prim\"}}",
        "{\"id\": 2, \"light\": {\"type\": \"laser\"}}",
        "{\"id\": 2, \"light\": {\"type\": \"spot\", \"inner_cone\": 40, \"outer_cone\": 20}}",
        "{\"id\": 2, \"camera\": {\"near\": 10, \"far\": 1}}",
        "{\"id\": 2, \"camera\": {\"fov\": 180}}",
        "{\"id\": 2, \"script\": {\"path\": \"scripts/missing.luau\"}}",
        "{\"id\": 2, \"script\": {\"path\": \"scripts/spinner.luau\", \"properties\": {\"speed\": {}}}}",
        "{\"id\": 2, \"script\": {\"path\": \"scripts/spinner.luau\", \"properties\": {\"entity\": 1}}}",
        "{\"id\": 2, \"audio_source\": {\"sound\": \"sounds/chime.wav\", \"bus\": \"master\"}}",
        "{\"id\": 2, \"audio_source\": {\"volume\": 1}}",
        "{\"id\": 2, \"transform\": {\"rotation\": [0, 0, 0, 0]}}",
        "{\"id\": 2, \"transform\": {\"rotation\": [0, 0, 0, 1], \"euler\": [0, 0, 0]}}",
        "{\"id\": 2, \"transform\": {\"position\": [1, 2]}}",
        "{\"id\": 2, \"rigidbody\": {\"type\": \"floating\"}}",
        "{\"id\": 2, \"instance\": {\"model\": \"meshes/box.prim\", \"animation\": 0}}",
        "{\"id\": 2, \"components\": {\"Unknown\": {}}}",
        "{\"id\": 2, \"ui\": {\"anchor_min\": [0]}}",
        "{\"id\": 2, \"ui\": {\"offset_max\": [1e9, 0]}}",
        "{\"id\": 2, \"sprite\": {\"color\": [1, 1, 1, 2]}}",
        "{\"id\": 2, \"sprite\": {\"image\": \"ui/missing.png\"}}",
        "{\"id\": 2, \"sprite\": {\"slice\": [-1, 0, 0, 0]}}",
        "{\"id\": 2, \"text\": {\"text\": 5}}",
        "{\"id\": 2, \"text\": {\"align\": \"middle\"}}",
        "{\"id\": 2, \"text\": {\"scale\": 0}}",
        "{\"id\": 2, \"text\": {\"font\": \"meshes/box.prim\"}}",
        "{\"id\": 2, \"button\": {\"pressed\": [3, 1, 1, 1]}}",
        "{\"id\": 2, \"button\": {\"clickable\": true}}",
        "{\"id\": 2, \"billboard\": {\"mode\": \"sideways\"}}",
        "{\"id\": 2, \"billboard\": {\"blend\": \"multiply\"}}",
        "{\"id\": 2, \"billboard\": {\"columns\": 2, \"rows\": 2, \"frame_count\": 5}}",
        "{\"id\": 2, \"billboard\": {\"columns\": 4, \"frame\": 4}}",
        "{\"id\": 2, \"billboard\": {\"size\": [-1, 1]}}",
        "{\"id\": 2, \"billboard\": {\"image\": \"sprites/missing.png\"}}",
        "{\"id\": 2, \"billboard\": {\"fps\": -1}}",
        "{\"id\": 2, \"particles\": {\"lifetime\": [2, 1]}}",
        "{\"id\": 2, \"particles\": {\"shape\": \"cone\"}}",
        "{\"id\": 2, \"particles\": {\"max_particles\": 0}}",
        "{\"id\": 2, \"particles\": {\"spread\": 200}}",
        "{\"id\": 2, \"particles\": {\"columns\": 2, \"frame_count\": 3}}",
        "{\"id\": 2, \"particles\": {\"rate\": -1}}",
        "{\"id\": 2, \"particles\": {\"size\": [1]}}",
    };
    World w = make_world();
    const WPE_SceneContext ctx = context_of(&w);
    quiet(true);
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        char text[1024];
        snprintf(text, sizeof text,
                 "{\"format\": \"wpe.scene\", \"version\": 1, \"entities\": [{\"id\": 1, \"name\": \"ok\", "
                 "\"instance\": {\"model\": \"meshes/RiggedFigure.glb\"}, \"mesh\": {\"model\": "
                 "\"meshes/box.prim\"}}, %s]}",
                 bad[i]);
        const WPE_Result result = wpe_scene_load(&ctx, text, strlen(text), "bad", NULL, NULL);
        WPE_TEST_ASSERT_MSG(result != WPE_RESULT_OK, "case %zu was accepted", i);
        WPE_TEST_ASSERT_MSG(wpe_ecs_entity_count(w.ecs) == 0, "case %zu left %u entities", i,
                            wpe_ecs_entity_count(w.ecs));
        WPE_TEST_ASSERT_MSG(wpe_asset_manager_stats(w.assets.assets).references == 0,
                            "case %zu left asset references", i);
    }
    /* Document-level mistakes. */
    static const struct {
        const char* text;
        WPE_Result expected;
    } docs[] = {
        {"{\"format\": \"wpe.scene\", \"version\": 2}", WPE_RESULT_ERROR_UNSUPPORTED},
        {"{\"format\": \"wpe.project\", \"version\": 1}", WPE_RESULT_ERROR_PARSE},
        {"{\"format\": \"wpe.scene\", \"version\": 1, \"environment\": {\"fog\": {\"mode\": \"thick\"}}}",
         WPE_RESULT_ERROR_PARSE},
        {"{\"format\": \"wpe.scene\", \"version\": 1, \"environment\": {\"postfx\": [{\"effect\": "
         "\"bloom\"}]}}",
         WPE_RESULT_ERROR_PARSE},
        {"{\"format\": \"wpe.scene\", \"version\": 1, \"environment\": {\"postfx\": [{\"effect\": \"ps1\", "
         "\"radius\": 1}]}}",
         WPE_RESULT_ERROR_PARSE},
        {"{\"format\": \"wpe.scene\", \"version\": 1, \"environment\": {\"default_sun\": {\"direction\": [0, "
         "0, 0]}}}",
         WPE_RESULT_ERROR_PARSE},
        {"{\"format\": \"wpe.scene\", \"version\": 1, \"camera\": 1, \"entities\": [{\"id\": 1}]}",
         WPE_RESULT_ERROR_PARSE},
        {"{\"format\": \"wpe.scene\", \"version\": 1, \"entities\": {}}", WPE_RESULT_ERROR_PARSE},
        {"[1, 2]", WPE_RESULT_ERROR_PARSE},
    };
    for (size_t i = 0; i < sizeof docs / sizeof docs[0]; ++i) {
        const WPE_Result result = wpe_scene_load(&ctx, docs[i].text, strlen(docs[i].text), "doc", NULL, NULL);
        WPE_TEST_ASSERT_MSG(result == docs[i].expected, "document %zu: got %s", i,
                            wpe_result_to_string(result));
        WPE_TEST_ASSERT_EQ_UINT(0, wpe_ecs_entity_count(w.ecs));
    }
    quiet(false);
    /* The empty scene is fine and yields the default environment. */
    const char* empty = "{\"format\": \"wpe.scene\", \"version\": 1}";
    WPE_SceneEnvironment env;
    WPE_TEST_ASSERT_OK(wpe_scene_load(&ctx, empty, strlen(empty), "empty", &env, NULL));
    WPE_TEST_ASSERT_NEAR(-9.81, env.gravity.y, 1e-6);
    free_world(&w);
}

/* A game-defined component with an entity reference, saved through a serializer. */
#define COMP_FOLLOWER 40u
typedef struct Follower {
    float speed;
    WPE_Entity target;
} Follower;

static WPE_Result save_follower(const WPE_SceneContext* context, const WPE_SceneIds* ids, WPE_Entity entity,
                                const void* component, WPE_JsonWriter* writer, void* user_data) {
    (void)context;
    (void)entity;
    (void)user_data;
    const Follower* f = component;
    wpe_json_write_object_begin(writer);
    wpe_json_write_key(writer, "speed");
    wpe_json_write_number(writer, f->speed);
    wpe_json_write_key(writer, "target");
    wpe_json_write_integer(writer, wpe_scene_ids_to_file(ids, f->target));
    wpe_json_write_object_end(writer);
    return WPE_RESULT_OK;
}

static WPE_Result load_follower(const WPE_SceneContext* context, const WPE_SceneIds* ids, WPE_Entity entity,
                                const WPE_JsonValue* value, void* out_component, void* user_data) {
    (void)context;
    (void)entity;
    (void)user_data;
    Follower* f = out_component;
    const WPE_JsonValue* speed = wpe_json_get(value, "speed");
    if (wpe_json_type(speed) != WPE_JSON_NUMBER) return WPE_RESULT_ERROR_PARSE;
    f->speed = (float)wpe_json_number(speed, 0);
    f->target = wpe_scene_ids_to_entity(ids, (uint32_t)wpe_json_number(wpe_json_get(value, "target"), 0));
    return WPE_RESULT_OK;
}

static void test_engine_scene_game_components(void) {
    World w = make_world();
    WPE_TEST_ASSERT_OK(wpe_ecs_register_component(
        w.ecs, COMP_FOLLOWER,
        &(WPE_ComponentDesc){.name = "Follower", .size = sizeof(Follower), .alignment = alignof(Follower)}));
    const WPE_ComponentSerializer serializer = {
        .type = COMP_FOLLOWER, .name = "follower", .save = save_follower, .load = load_follower};
    WPE_SceneContext ctx = context_of(&w);
    ctx.serializers = &serializer;
    ctx.serializer_count = 1;

    const WPE_Entity leader = wpe_ecs_spawn(w.ecs);
    WPE_TEST_ASSERT_OK(wpe_scene_set_name(w.ecs, leader, "leader"));
    const WPE_Entity follower = wpe_ecs_spawn(w.ecs);
    const Follower data = {.speed = 2.5f, .target = leader};
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(w.ecs, follower, COMP_FOLLOWER, &data, NULL));
    const WPE_SceneEnvironment env = wpe_scene_environment_default();
    WPE_JsonWriter* writer = NULL;
    const char* text = save_text(&ctx, &env, WPE_ENTITY_NULL, &writer);
    WPE_TEST_ASSERT(strstr(text, "\"follower\": {") != NULL);

    WPE_TEST_ASSERT_OK(wpe_scene_clear(w.ecs));
    WPE_TEST_ASSERT_OK(wpe_scene_load(&ctx, text, strlen(text), "followers", NULL, NULL));
    WPE_EcsIter it = wpe_ecs_query(w.ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(COMP_FOLLOWER)});
    WPE_TEST_ASSERT(wpe_ecs_iter_next(&it));
    const Follower* loaded = wpe_ecs_iter_get(&it, COMP_FOLLOWER);
    WPE_TEST_ASSERT_NEAR(2.5, loaded->speed, 0);
    WPE_TEST_ASSERT_EQ_STR("leader", wpe_scene_get_name(w.ecs, loaded->target)); /* Remapped reference. */

    /* Without the serializer the component name is unknown. */
    quiet(true);
    const WPE_SceneContext plain = context_of(&w);
    WPE_TEST_ASSERT_OK(wpe_scene_clear(w.ecs));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_PARSE,
                           wpe_scene_load(&plain, text, strlen(text), "followers", NULL, NULL));
    quiet(false);
    wpe_json_writer_destroy(writer);
    WPE_TEST_ASSERT_OK(wpe_scene_clear(w.ecs));
    WPE_TEST_ASSERT_OK(wpe_ecs_unregister_component(w.ecs, COMP_FOLLOWER));
    free_world(&w);
}

static WPE_JsonDocument* parse_json(const char* text) {
    WPE_JsonDocument* doc = NULL;
    WPE_TEST_ASSERT_OK(wpe_json_parse(text, strlen(text), &doc, NULL));
    return doc;
}

static void test_engine_scene_editing_functions(void) {
    World w = make_world();
    const WPE_SceneContext ctx = context_of(&w);
    /* Create with live-handle references. */
    WPE_JsonDocument* parent_doc =
        parse_json("{\"name\": \"parent\", \"transform\": {\"position\": [1, 2, 3]}}");
    WPE_Entity parent = WPE_ENTITY_NULL;
    WPE_TEST_ASSERT_OK(wpe_scene_create_entity(&ctx, wpe_json_root(parent_doc), &parent));
    char text[512];
    snprintf(text, sizeof text,
             "{\"name\": \"lamp\", \"parent\": %u, \"light\": {\"type\": \"point\", \"intensity\": 3},"
             " \"script\": {\"path\": \"scripts/spinner.luau\", \"properties\": {\"speed\": 2}}}",
             parent);
    WPE_JsonDocument* lamp_doc = parse_json(text);
    WPE_Entity lamp = WPE_ENTITY_NULL;
    WPE_TEST_ASSERT_OK(wpe_scene_create_entity(&ctx, wpe_json_root(lamp_doc), &lamp));
    WPE_TEST_ASSERT_EQ_UINT(parent, wpe_scene_get_parent(w.ecs, lamp));
    WPE_TEST_ASSERT_NEAR(3.0, ((WPE_Light*)wpe_ecs_get_component(w.ecs, lamp, WPE_COMP_LIGHT))->intensity, 0);

    /* Save one entity: ids are live handles. */
    WPE_JsonWriter* writer = NULL;
    WPE_TEST_ASSERT_OK(wpe_json_writer_create(&writer));
    WPE_TEST_ASSERT_OK(wpe_scene_save_entity(&ctx, lamp, writer));
    const char* saved = NULL;
    WPE_TEST_ASSERT_OK(wpe_json_writer_finish(writer, &saved, NULL));
    char expect[64];
    snprintf(expect, sizeof expect, "\"parent\": %u", parent);
    WPE_TEST_ASSERT(strstr(saved, expect) && strstr(saved, "\"speed\": 2"));
    wpe_json_writer_destroy(writer);

    /* Replace a component; a bad description keeps the previous one. */
    WPE_JsonDocument* spot = parse_json("{\"type\": \"spot\", \"range\": 4}");
    WPE_TEST_ASSERT_OK(wpe_scene_set_component(&ctx, lamp, "light", wpe_json_root(spot)));
    const WPE_Light* light = wpe_ecs_get_component(w.ecs, lamp, WPE_COMP_LIGHT);
    WPE_TEST_ASSERT(light->type == WPE_LIGHT_SPOT && light->range == 4.0f && light->intensity == 1.0f);
    WPE_JsonDocument* bad = parse_json("{\"type\": \"laser\"}");
    quiet(true);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_PARSE,
                           wpe_scene_set_component(&ctx, lamp, "light", wpe_json_root(bad)));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_PARSE,
                           wpe_scene_set_component(&ctx, lamp, "lamp", wpe_json_root(spot)));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_scene_remove_component(&ctx, lamp, "lamp"));
    quiet(false);
    light = wpe_ecs_get_component(w.ecs, lamp, WPE_COMP_LIGHT);
    WPE_TEST_ASSERT(light && light->type == WPE_LIGHT_SPOT);
    /* Mesh with a model reference, name and parent changes, removal. */
    WPE_JsonDocument* mesh = parse_json("{\"model\": \"meshes/box.prim\"}");
    WPE_TEST_ASSERT_OK(wpe_scene_set_component(&ctx, lamp, "mesh", wpe_json_root(mesh)));
    WPE_TEST_ASSERT_OK(wpe_scene_set_component(&ctx, lamp, "mesh", wpe_json_root(mesh))); /* Replace. */
    /* A mesh or convex collider without a model takes the entity's mesh (and names it when saved). */
    WPE_JsonDocument* mesh_collider = parse_json("{\"shape\": \"mesh\", \"model\": null}");
    WPE_TEST_ASSERT_OK(wpe_scene_set_component(&ctx, lamp, "collider", wpe_json_root(mesh_collider)));
    const WPE_Collider* collider = wpe_ecs_get_component(w.ecs, lamp, WPE_COMP_PHYSICS_COLLIDER);
    const WPE_MeshRenderer* renderer = wpe_ecs_get_component(w.ecs, lamp, WPE_COMP_MESH);
    WPE_TEST_ASSERT(collider && collider->shape == WPE_COLLIDER_MESH);
    WPE_TEST_ASSERT(collider->model == renderer->model_asset && collider->mesh == renderer->mesh);
    WPE_TEST_ASSERT_OK(wpe_json_writer_create(&writer));
    WPE_TEST_ASSERT_OK(wpe_scene_save_entity(&ctx, lamp, writer));
    WPE_TEST_ASSERT_OK(wpe_json_writer_finish(writer, &saved, NULL));
    const char* saved_collider = strstr(saved, "\"collider\"");
    WPE_TEST_ASSERT(saved_collider && strstr(saved_collider, "\"model\": \"meshes/box.prim\"") != NULL);
    wpe_json_writer_destroy(writer);
    WPE_TEST_ASSERT_OK(wpe_scene_remove_component(&ctx, lamp, "collider"));
    WPE_JsonDocument* name = parse_json("\"renamed\"");
    WPE_TEST_ASSERT_OK(wpe_scene_set_component(&ctx, lamp, "name", wpe_json_root(name)));
    WPE_TEST_ASSERT_EQ_STR("renamed", wpe_scene_get_name(w.ecs, lamp));
    WPE_JsonDocument* detach = parse_json("null");
    WPE_TEST_ASSERT_OK(wpe_scene_set_component(&ctx, lamp, "parent", wpe_json_root(detach)));
    WPE_TEST_ASSERT_EQ_UINT(WPE_ENTITY_NULL, wpe_scene_get_parent(w.ecs, lamp));
    WPE_TEST_ASSERT_OK(wpe_scene_remove_component(&ctx, lamp, "mesh"));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND, wpe_scene_remove_component(&ctx, lamp, "mesh"));

    /* Environment: partial updates. */
    WPE_SceneEnvironment env = wpe_scene_environment_default();
    WPE_JsonDocument* fog = parse_json("{\"fog\": {\"mode\": \"linear\", \"start\": 1, \"end\": 9}}");
    WPE_TEST_ASSERT_OK(wpe_scene_read_environment(wpe_json_root(fog), "test", &env));
    WPE_TEST_ASSERT(env.render.fog.mode == WPE_FOG_LINEAR && env.render.fog.end == 9.0f &&
                    env.gravity.y < -9.0f);
    WPE_TEST_ASSERT_OK(wpe_json_writer_create(&writer));
    WPE_TEST_ASSERT_OK(wpe_scene_write_environment(&env, writer));
    WPE_TEST_ASSERT_OK(wpe_json_writer_finish(writer, &saved, NULL));
    WPE_TEST_ASSERT(strstr(saved, "\"mode\": \"linear\"") != NULL);
    wpe_json_writer_destroy(writer);

    /* The include filter keeps an entity (and its children) out of saved scenes. */
    WPE_SceneContext filtered = ctx;
    filtered.include = NULL;
    WPE_JsonWriter* all = NULL;
    const char* all_text = save_text(&filtered, &env, WPE_ENTITY_NULL, &all);
    WPE_TEST_ASSERT(strstr(all_text, "\"parent\"") != NULL);
    wpe_json_writer_destroy(all);

    WPE_JsonDocument* docs[] = {parent_doc, lamp_doc, spot, bad, mesh, mesh_collider, name, detach, fog};
    for (size_t i = 0; i < sizeof docs / sizeof docs[0]; ++i)
        wpe_json_free(docs[i]);
    free_world(&w);
}

static bool exclude_named_parent(WPE_Entity entity, void* user_data) {
    const char* name = wpe_scene_get_name(user_data, entity);
    return !name || strcmp(name, "parent") != 0;
}

static void test_engine_scene_save_filter(void) {
    World w = make_world();
    WPE_SceneContext ctx = context_of(&w);
    const WPE_Entity parent = wpe_ecs_spawn(w.ecs);
    WPE_TEST_ASSERT_OK(wpe_scene_set_name(w.ecs, parent, "parent"));
    const WPE_Entity child = wpe_ecs_spawn(w.ecs);
    WPE_TEST_ASSERT_OK(wpe_scene_set_name(w.ecs, child, "child"));
    WPE_TEST_ASSERT_OK(wpe_scene_set_parent(w.ecs, child, parent, false));
    const WPE_Entity other = wpe_ecs_spawn(w.ecs);
    WPE_TEST_ASSERT_OK(wpe_scene_set_name(w.ecs, other, "other"));
    ctx.include = exclude_named_parent;
    ctx.include_user_data = w.ecs;
    const WPE_SceneEnvironment env = wpe_scene_environment_default();
    WPE_JsonWriter* writer = NULL;
    const char* text = save_text(&ctx, &env, WPE_ENTITY_NULL, &writer);
    WPE_TEST_ASSERT(strstr(text, "\"other\"") && !strstr(text, "\"parent\"") && !strstr(text, "\"child\""));
    wpe_json_writer_destroy(writer);
    free_world(&w);
}

/* ---- The engine ------------------------------------------------------------------------------------ */

static void write_tiny_game(void);

static void test_engine_embedded_edit_mode(void) {
    write_tiny_game();
    WPE_Engine* engine = NULL;
    WPE_TEST_ASSERT_OK(wpe_engine_create(
        &(WPE_EngineDesc){
            .project_dir = ROOT "/game", .embedded = true, .no_audio_device = true, .workers = 2},
        &engine));
    /* Edit mode: frames render but scripts do not run (the switch script would load scene b). */
    wpe_engine_set_simulating(engine, false);
    WPE_TEST_ASSERT(!wpe_engine_simulating(engine));
    for (int i = 0; i < 5; ++i)
        WPE_TEST_ASSERT_OK(wpe_engine_frame(engine, 1.0 / 30.0));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_script_host_stats(wpe_engine_scripts(engine)).errors);
    WPE_TEST_ASSERT_NEAR(-1.0, wpe_physics_gravity(wpe_engine_physics(engine)).y, 1e-6); /* Still scene a. */
    /* Play: the scripts start and switch scenes after three updates. */
    wpe_engine_set_simulating(engine, true);
    for (int i = 0; i < 4; ++i)
        WPE_TEST_ASSERT_OK(wpe_engine_frame(engine, 1.0 / 30.0));
    WPE_TEST_ASSERT_NEAR(-2.0, wpe_physics_gravity(wpe_engine_physics(engine)).y, 1e-6);
    wpe_engine_destroy(engine);
}

/* A tiny game: scene a switches to scene b after three frames; b quits after two more. */
static void write_tiny_game(void) {
    WPE_TEST_ASSERT_OK(wpe_directory_create(ROOT "/game/scenes"));
    WPE_TEST_ASSERT_OK(wpe_directory_create(ROOT "/game/scripts"));
    write_text(ROOT "/game/project.json",
               "{\"format\": \"wpe.project\", \"version\": 1, \"name\": \"Tiny\", \"start_scene\": "
               "\"scenes/a.scene\",\n"
               " \"display\": {\"width\": 64, \"height\": 48},\n"
               " \"input\": {\"actions\": {\"go\": [{\"key\": \"g\"}, {\"pad_button\": \"start\"}]}}}\n");
    write_text(ROOT "/game/scenes/a.scene",
               "{\"format\": \"wpe.scene\", \"version\": 1,\n"
               " \"environment\": {\"clear_color\": [1, 0, 0], \"gravity\": [0, -1, 0]},\n"
               " \"entities\": [{\"id\": 1, \"name\": \"switcher\", \"script\": {\"path\": "
               "\"scripts/switch.luau\"}}]}\n");
    write_text(
        ROOT "/game/scenes/b.scene",
        "{\"format\": \"wpe.scene\", \"version\": 1,\n"
        " \"environment\": {\"clear_color\": [0, 0, 1], \"gravity\": [0, -2, 0]},\n"
        " \"entities\": [{\"id\": 1, \"name\": \"eye\", \"transform\": {}, \"camera\": {}},\n"
        "              {\"id\": 2, \"name\": \"quitter\", \"script\": {\"path\": \"scripts/quit.luau\",\n"
        "               \"properties\": {\"frames\": 2}}}]}\n");
    write_text(ROOT "/game/scripts/switch.luau",
               "local S = {}\n"
               "function S:on_start() self.n = 0; assert(wpe.get_camera() == nil)\n"
               "  assert(wpe.text_size('x') > 0) end -- the built-in font: the project names none\n"
               "function S:on_update(dt)\n"
               "  self.n += 1\n"
               "  assert(not wpe.action_down('go'))\n"
               "  if self.n == 3 then wpe.load_scene('scenes/b.scene') end\n"
               "end\n"
               "return S\n");
    write_text(ROOT "/game/scripts/quit.luau",
               "local S = {}\n"
               "function S:on_start() self.n = 0; assert(wpe.get_name(wpe.get_camera()) == 'eye') end\n"
               "function S:on_update(dt)\n"
               "  self.n += 1\n"
               "  wpe.draw_rect(0, 0, 8, 8, wpe.color(0, 1, 0))\n"
               "  if self.n == self.frames then wpe.quit() end\n"
               "end\n"
               "return S\n");
}

/* Hit-stop and shake through the engine: game time stops and resumes; scripts never see a shaken pose. */
static void test_engine_hit_stop_and_shake(void) {
    WPE_TEST_ASSERT_OK(wpe_directory_create(ROOT "/feel/scenes"));
    WPE_TEST_ASSERT_OK(wpe_directory_create(ROOT "/feel/scripts"));
    write_text(ROOT "/feel/project.json",
               "{\"format\": \"wpe.project\", \"version\": 1, \"name\": \"Feel\", \"start_scene\": "
               "\"scenes/main.scene\", \"display\": {\"width\": 64, \"height\": 48}}\n");
    write_text(ROOT "/feel/scenes/main.scene",
               "{\"format\": \"wpe.scene\", \"version\": 1, \"entities\": [\n"
               " {\"id\": 1, \"name\": \"clock\", \"script\": {\"path\": \"scripts/feel.luau\"}},\n"
               " {\"id\": 2, \"name\": \"box\", \"transform\": {\"position\": [1, 2, 3]}}]}\n");
    /* Each update's game time goes into the clock's name. The first starts a 0.1 s hit-stop and a shake; the
     * fourth half-speed slow motion for 0.1 s. */
    write_text(
        ROOT "/feel/scripts/feel.luau",
        "local S = {}\n"
        "function S:on_start() self.times = {}; self.box = wpe.find('box') end\n"
        "function S:on_update(dt)\n"
        "  local p = wpe.get_position(self.box)\n"
        "  assert(p.x == 1 and p.y == 2 and p.z == 3)\n"
        "  if #self.times == 0 then wpe.hit_stop(0.1); wpe.shake(self.box, {amplitude = 0.5, duration = 1}) "
        "end\n"
        "  if #self.times == 3 then wpe.slow_motion(0.5, 0.1); assert(wpe.get_time_scale() == 0.5) end\n"
        "  if #self.times < 7 then table.insert(self.times, string.format('%.3f', dt)) end\n"
        "  assert(math.abs(wpe.real_delta() - 0.05) < 1e-6)\n"
        "  wpe.set_name(self.entity, table.concat(self.times, ','))\n"
        "end\n"
        "return S\n");
    WPE_Engine* engine = NULL;
    WPE_TEST_ASSERT_OK(wpe_engine_create(
        &(WPE_EngineDesc){
            .project_dir = ROOT "/feel", .embedded = true, .no_audio_device = true, .workers = 2},
        &engine));
    for (int i = 0; i < 9; ++i)
        WPE_TEST_ASSERT_OK(wpe_engine_frame(engine, 0.05));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_script_host_stats(wpe_engine_scripts(engine)).errors);
    WPE_Ecs* ecs = wpe_engine_ecs(engine);
    WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_SCRIPT)});
    WPE_TEST_ASSERT(wpe_ecs_iter_next(&it));
    /* 0.05 s, then the hit-stop freezes two frames (0.1 s of real time), then time runs again; slow motion
     * halves two frames (0.1 s of real time) and lets go. */
    WPE_TEST_ASSERT_EQ_STR("0.050,0.000,0.000,0.050,0.025,0.025,0.050", wpe_scene_get_name(ecs, it.entity));
    /* Between frames the box shows its shaken pose (what was rendered). */
    WPE_EcsIter boxes = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_TRANSFORM)});
    WPE_TEST_ASSERT(wpe_ecs_iter_next(&boxes));
    const WPE_Transform* box = wpe_ecs_iter_get(&boxes, WPE_COMP_TRANSFORM);
    WPE_TEST_ASSERT(box->position.x != 1.0f || box->position.y != 2.0f || box->position.z != 3.0f);
    WPE_TEST_ASSERT(wpe_game_feel_is_shaking(wpe_engine_game_feel(engine), boxes.entity));
    /* A scene load ends it all. */
    WPE_TEST_ASSERT_OK(wpe_engine_load_scene(engine, "scenes/main.scene"));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_engine_game_feel(engine)->shake_count);
    WPE_TEST_ASSERT_NEAR(1.0, wpe_game_feel_time_scale(wpe_engine_game_feel(engine)), 1e-6);
    wpe_engine_destroy(engine);
}

/* A fade between two scenes: the screen closes over 0.2 s, the next scene loads covered, then it opens. */
static void test_engine_scene_transition(void) {
    WPE_TEST_ASSERT_OK(wpe_directory_create(ROOT "/fade/scenes"));
    WPE_TEST_ASSERT_OK(wpe_directory_create(ROOT "/fade/scripts"));
    write_text(ROOT "/fade/project.json",
               "{\"format\": \"wpe.project\", \"version\": 1, \"name\": \"Fade\", \"start_scene\": "
               "\"scenes/a.scene\", \"display\": {\"width\": 16, \"height\": 16}}\n");
    write_text(ROOT "/fade/scenes/a.scene",
               "{\"format\": \"wpe.scene\", \"version\": 1, \"environment\": {\"clear_color\": [1, 1, 1],\n"
               " \"gravity\": [0, -1, 0]},\n"
               " \"entities\": [{\"id\": 1, \"script\": {\"path\": \"scripts/go.luau\"}}]}\n");
    write_text(ROOT "/fade/scenes/b.scene",
               "{\"format\": \"wpe.scene\", \"version\": 1, \"environment\": {\"clear_color\": [1, 1, 1],\n"
               " \"gravity\": [0, -2, 0]}}\n");
    write_text(ROOT "/fade/scripts/go.luau",
               "local S = {}\n"
               "function S:on_update(dt)\n"
               "  wpe.load_scene('scenes/b.scene', {transition = 'fade', duration = 0.2})\n"
               "end\n"
               "return S\n");
    WPE_Engine* engine = NULL;
    WPE_TEST_ASSERT_OK(wpe_engine_create(
        &(WPE_EngineDesc){
            .project_dir = ROOT "/fade", .embedded = true, .no_audio_device = true, .workers = 2},
        &engine));
    const WPE_Framebuffer* fb = wpe_renderer_framebuffer(wpe_engine_renderer(engine));
    uint32_t shade[9];
    float gravity[9];
    for (int i = 0; i < 9; ++i) {
        WPE_TEST_ASSERT_OK(wpe_engine_frame(engine, 0.05));
        shade[i] = fb->color[0] & 0xFFu; /* White fading to black and back. */
        gravity[i] = wpe_physics_gravity(wpe_engine_physics(engine)).y;
    }
    /* Closing (the script asks again every frame: it only retargets), black on the fourth frame, when b loads
     * after it; then opening, white again by the eighth. */
    WPE_TEST_ASSERT(shade[0] < 255 && shade[1] < shade[0] && shade[2] < shade[1]);
    WPE_TEST_ASSERT_EQ_UINT(0, shade[3]);
    WPE_TEST_ASSERT_NEAR(-1.0, gravity[2], 1e-6);
    WPE_TEST_ASSERT_NEAR(-2.0, gravity[3], 1e-6);
    WPE_TEST_ASSERT(shade[4] > 0 && shade[5] > shade[4] && shade[6] > shade[5]);
    WPE_TEST_ASSERT_EQ_UINT(255, shade[7]);
    WPE_TEST_ASSERT_EQ_UINT(255, shade[8]);
    /* A load from outside (the editor stopping play) cuts: no transition carries over. */
    WPE_TEST_ASSERT_OK(wpe_engine_load_scene(engine, "scenes/a.scene"));
    WPE_TEST_ASSERT_OK(wpe_engine_frame(engine, 0.05)); /* The script starts a fade again... */
    WPE_TEST_ASSERT_OK(wpe_engine_load_scene(engine, "scenes/b.scene"));
    WPE_TEST_ASSERT_OK(wpe_engine_frame(engine, 0.05)); /* ...which the load ended. */
    WPE_TEST_ASSERT_EQ_UINT(255, fb->color[0] & 0xFFu);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_script_host_stats(wpe_engine_scripts(engine)).errors);
    wpe_engine_destroy(engine);
}

/* A 2D menu: no camera, a button whose script writes its click count into a label. */
static void write_menu_game(void) {
    WPE_TEST_ASSERT_OK(wpe_directory_create(ROOT "/menu/scenes"));
    WPE_TEST_ASSERT_OK(wpe_directory_create(ROOT "/menu/scripts"));
    WPE_TEST_ASSERT_OK(wpe_directory_create(ROOT "/menu/fonts"));
    void* font = NULL;
    size_t size = 0;
    WPE_TEST_ASSERT_OK(wpe_file_read_all(WPE_TEST_ASSETS_DIR "/fonts/small.png", &font, &size));
    WPE_File* file = NULL;
    WPE_TEST_ASSERT_OK(wpe_file_open(ROOT "/menu/fonts/small.png", true, &file));
    WPE_TEST_ASSERT_OK(wpe_file_write(file, font, size));
    WPE_TEST_ASSERT_OK(wpe_file_close(file));
    wpe_free(font);
    write_text(ROOT "/menu/project.json",
               "{\"format\": \"wpe.project\", \"version\": 1, \"name\": \"Menu\", \"start_scene\": "
               "\"scenes/menu.scene\", \"font\": \"fonts/small.png\",\n"
               " \"display\": {\"width\": 64, \"height\": 48}}\n");
    write_text(ROOT "/menu/scenes/menu.scene",
               "{\"format\": \"wpe.scene\", \"version\": 1,\n"
               " \"environment\": {\"clear_color\": [0, 0, 1]},\n"
               " \"entities\": [\n"
               "  {\"id\": 1, \"name\": \"start\", \"ui\": {\"anchor_max\": [0, 0], \"offset_min\": [8, "
               "8],\n   \"offset_max\": [56, 24]},\n"
               "   \"sprite\": {\"color\": [1, 0, 0, 1]}, \"button\": {\"focused\": [1, 1, 1, 1]},\n"
               "   \"toggle\": {\"mark_color\": [1, 1, 1, 0]},\n"
               "   \"script\": {\"path\": \"scripts/start.luau\"}},\n"
               "  {\"id\": 2, \"name\": \"label\", \"ui\": {\"anchor_min\": [0, 1], \"offset_min\": [0, "
               "-18], \"offset_max\": [0, -8]},\n"
               "   \"text\": {\"text\": \"none\"}}]}\n");
    write_text(ROOT "/menu/scripts/start.luau",
               "local S = {}\n"
               "function S:on_start() self.clicks = 0; self.label = wpe.find('label') end\n"
               "-- It's a toggle too: on_change comes first, with the new value.\n"
               "function S:on_change(on) self.on = on; assert(wpe.ui_value(self.entity) == on) end\n"
               "function S:on_click()\n"
               "  self.clicks += 1\n"
               "  assert(wpe.ui_focused() == self.entity)\n"
               "  wpe.set_text(self.label, 'clicks ' .. self.clicks .. ' ' .. tostring(self.on))\n"
               "end\n"
               "return S\n");
}

static void send_event(WPE_Engine* engine, SDL_Event e) {
    wpe_engine_handle_event(engine, (const WPE_PlatformEvent*)&e);
}

/* A zeroed event: an initializer list would only set the union's first member. */
static SDL_Event make_event(SDL_EventType type) {
    SDL_Event e;
    SDL_zero(e);
    e.type = type;
    return e;
}

static void test_engine_ui_clicks_reach_scripts(void) {
    write_menu_game();
    WPE_Engine* engine = NULL;
    WPE_TEST_ASSERT_OK(wpe_engine_create(
        &(WPE_EngineDesc){
            .project_dir = ROOT "/menu", .embedded = true, .no_audio_device = true, .workers = 2},
        &engine));
    WPE_Ecs* ecs = wpe_engine_ecs(engine);
    WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_TEXT)});
    WPE_TEST_ASSERT(wpe_ecs_iter_next(&it));
    const WPE_Entity label = it.entity;
    WPE_TEST_ASSERT_OK(wpe_engine_frame(engine, 1.0 / 30.0)); /* on_start runs. */
    /* A 2D scene: the clear color, the UI on top. */
    const WPE_Framebuffer* fb = wpe_renderer_framebuffer(wpe_engine_renderer(engine));
    WPE_TEST_ASSERT_EQ_UINT(0xFF0000FFu, fb->color[2 * fb->width + 2]);
    WPE_TEST_ASSERT_EQ_UINT(0xFFFF0000u, fb->color[10 * fb->width + 10]);

    /* A mouse click on the button. */
    SDL_Event motion = make_event(SDL_EVENT_MOUSE_MOTION);
    motion.motion.x = 20.0f;
    motion.motion.y = 12.0f;
    motion.motion.xrel = 1.0f;
    send_event(engine, motion);
    SDL_Event button = make_event(SDL_EVENT_MOUSE_BUTTON_DOWN);
    button.button.button = SDL_BUTTON_LEFT;
    button.button.down = true;
    button.button.x = 20.0f;
    button.button.y = 12.0f;
    send_event(engine, button);
    WPE_TEST_ASSERT_OK(wpe_engine_frame(engine, 1.0 / 30.0));
    button.type = SDL_EVENT_MOUSE_BUTTON_UP;
    button.button.down = false;
    send_event(engine, button);
    WPE_TEST_ASSERT_OK(wpe_engine_frame(engine, 1.0 / 30.0));
    WPE_TEST_ASSERT_EQ_STR("clicks 1 true",
                           ((const WPE_Text*)wpe_ecs_get_component(ecs, label, WPE_COMP_TEXT))->text);

    /* Return clicks the focused button. */
    SDL_Event key = make_event(SDL_EVENT_KEY_DOWN);
    key.key.scancode = SDL_SCANCODE_RETURN;
    key.key.down = true;
    send_event(engine, key);
    key.type = SDL_EVENT_KEY_UP;
    key.key.down = false;
    send_event(engine, key);
    WPE_TEST_ASSERT_OK(wpe_engine_frame(engine, 1.0 / 30.0));
    WPE_TEST_ASSERT_EQ_STR("clicks 2 false",
                           ((const WPE_Text*)wpe_ecs_get_component(ecs, label, WPE_COMP_TEXT))->text);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_script_host_stats(wpe_engine_scripts(engine)).errors);

    /* In edit mode the UI is drawn but does not react; the editor's 3D view hides it. */
    wpe_engine_set_simulating(engine, false);
    send_event(engine, key);
    WPE_TEST_ASSERT_OK(wpe_engine_frame(engine, 1.0 / 30.0));
    WPE_TEST_ASSERT_EQ_STR("clicks 2 false",
                           ((const WPE_Text*)wpe_ecs_get_component(ecs, label, WPE_COMP_TEXT))->text);
    WPE_TEST_ASSERT(wpe_engine_draws_ui(engine));
    wpe_engine_set_draw_ui(engine, false);
    WPE_TEST_ASSERT_OK(wpe_engine_frame(engine, 1.0 / 30.0));
    WPE_TEST_ASSERT_EQ_UINT(0xFF0000FFu, fb->color[10 * fb->width + 10]);
    wpe_engine_destroy(engine);
}

static bool remove_file(const char* relative_path, void* user_data) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", (const char*)user_data, relative_path);
    WPE_TEST_ASSERT_OK(wpe_path_remove(path));
    return true;
}

/* Deletes a project made by wpe_project_create (its files, then its directories), if present. */
static void remove_new_project(const char* directory) {
    if (!wpe_directory_exists(directory)) return;
    WPE_TEST_ASSERT_OK(wpe_directory_walk(directory, remove_file, (void*)directory));
    static const char* const subdirectories[] = {"scenes", "scripts", "meshes"};
    for (size_t i = 0; i < sizeof subdirectories / sizeof subdirectories[0]; ++i) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s", directory, subdirectories[i]);
        if (wpe_directory_exists(path)) WPE_TEST_ASSERT_OK(wpe_path_remove(path));
    }
    WPE_TEST_ASSERT_OK(wpe_path_remove(directory));
}

/* A new project runs as it is: its scene loads, the cube's script spins it, and nothing is overwritten. */
static void test_engine_creates_new_projects(void) {
    remove_new_project(ROOT "/new_game"); /* Left by an earlier run. */
    WPE_TEST_ASSERT_OK(wpe_project_create(ROOT "/new_game", "New Game", NULL));
    quiet(true);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_ALREADY_EXISTS,
                           wpe_project_create(ROOT "/new_game", "Again", NULL));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_project_create(ROOT "/unnamed", "", NULL));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND,
                           wpe_project_create(ROOT "/unnamed", "X", "no_such_template"));
    quiet(false);
    WPE_TEST_ASSERT(!wpe_directory_exists(ROOT "/unnamed"));

    WPE_Engine* engine = NULL;
    WPE_TEST_ASSERT_OK(wpe_engine_create(
        &(WPE_EngineDesc){.project_dir = ROOT "/new_game", .headless = true, .workers = 2}, &engine));
    WPE_TEST_ASSERT_EQ_STR("New Game", wpe_engine_project(engine)->name);
    WPE_TEST_ASSERT_EQ_UINT(3, wpe_input_action_count(wpe_engine_input(engine)));
    WPE_Ecs* ecs = wpe_engine_ecs(engine);
    WPE_TEST_ASSERT_EQ_STR("camera", wpe_scene_get_name(ecs, wpe_engine_camera(engine)));
    WPE_Entity cube = WPE_ENTITY_NULL;
    WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_SCRIPT)});
    if (wpe_ecs_iter_next(&it)) cube = it.entity;
    WPE_TEST_ASSERT_EQ_STR("cube", wpe_scene_get_name(ecs, cube));
    WPE_TEST_ASSERT_OK(wpe_engine_run(engine, &(WPE_RunOptions){.frames = 10, .fixed_dt = 1.0 / 30.0}));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_script_host_stats(wpe_engine_scripts(engine)).errors);
    const WPE_Transform* t = wpe_ecs_get_component(ecs, cube, WPE_COMP_TRANSFORM);
    WPE_TEST_ASSERT(fabsf(t->rotation.y) > 0.1f); /* Turned about Y. */
    /* The ground fills the bottom of the view, below the sky's clear color. */
    const WPE_Framebuffer* fb = wpe_renderer_framebuffer(wpe_engine_renderer(engine));
    WPE_TEST_ASSERT(fb->color[(fb->height - 4) * fb->width + 4] != fb->color[4 * fb->width + 4]);
    wpe_engine_destroy(engine);
}

/* A project whose font file is missing starts with the built-in font, and its texts are drawn. */
static void test_engine_falls_back_to_the_builtin_font(void) {
    WPE_TEST_ASSERT_OK(wpe_directory_create(ROOT "/nofont/scenes"));
    write_text(ROOT "/nofont/project.json",
               "{\"format\": \"wpe.project\", \"version\": 1, \"name\": \"No font\", \"start_scene\": "
               "\"scenes/s.scene\", \"font\": \"fonts/missing.png\", \"display\": {\"width\": 64, "
               "\"height\": 48}}\n");
    write_text(ROOT "/nofont/scenes/s.scene",
               "{\"format\": \"wpe.scene\", \"version\": 1, \"environment\": {\"clear_color\": [0, 0, 0]},\n"
               " \"entities\": [{\"id\": 1, \"ui\": {}, \"text\": {\"text\": \"HELLO\"}}]}\n");
    quiet(true);
    WPE_Engine* engine = NULL;
    WPE_TEST_ASSERT_OK(wpe_engine_create(
        &(WPE_EngineDesc){.project_dir = ROOT "/nofont", .headless = true, .workers = 2}, &engine));
    quiet(false);
    WPE_TEST_ASSERT_OK(wpe_engine_frame(engine, 1.0 / 30.0));
    const WPE_Framebuffer* fb = wpe_renderer_framebuffer(wpe_engine_renderer(engine));
    uint32_t ink = 0;
    for (uint32_t i = 0; i < fb->width * fb->height; ++i)
        ink += fb->color[i] == 0xFFFFFFFFu;
    WPE_TEST_ASSERT(ink > 10);
    wpe_engine_destroy(engine);
}

/* wpe.capture_screen keeps the frame as the scene rendered it: here red, while post-processing turns the
 * screen gray. The capture is an image asset by its path. */
static void test_engine_captures_the_frame_before_post_effects(void) {
    WPE_TEST_ASSERT_OK(wpe_directory_create(ROOT "/capture/scenes"));
    WPE_TEST_ASSERT_OK(wpe_directory_create(ROOT "/capture/scripts"));
    write_text(ROOT "/capture/project.json",
               "{\"format\": \"wpe.project\", \"version\": 1, \"name\": \"Capture\", \"start_scene\": "
               "\"scenes/main.scene\", \"display\": {\"width\": 32, \"height\": 24}}\n");
    write_text(ROOT "/capture/scenes/main.scene",
               "{\"format\": \"wpe.scene\", \"version\": 1,\n"
               " \"environment\": {\"clear_color\": [1, 0, 0], \"postfx\": [{\"effect\": \"grayscale\"}]},\n"
               " \"entities\": [{\"id\": 1, \"name\": \"eye\", \"transform\": {}, \"camera\": {}},\n"
               "              {\"id\": 2, \"name\": \"grabber\", \"script\": {\"path\": "
               "\"scripts/grab.luau\"}}]}\n");
    write_text(ROOT "/capture/scripts/grab.luau",
               "local S = {}\n"
               "function S:on_update(dt)\n"
               "  if self.done then return end\n"
               "  self.done = true\n"
               "  assert(wpe.capture_screen() == 'capture/1' and wpe.capture_screen(3) == 'capture/3')\n"
               "  assert(not pcall(wpe.capture_screen, 0) and not pcall(wpe.capture_screen, 9))\n"
               "end\n"
               "return S\n");
    WPE_Engine* engine = NULL;
    WPE_TEST_ASSERT_OK(wpe_engine_create(
        &(WPE_EngineDesc){.project_dir = ROOT "/capture", .headless = true, .workers = 2}, &engine));
    WPE_TEST_ASSERT_OK(wpe_engine_frame(engine, 1.0 / 30.0));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_script_host_stats(wpe_engine_scripts(engine)).errors);
    const WPE_Color shown = wpe_renderer_framebuffer(wpe_engine_renderer(engine))->color[0];
    WPE_TEST_ASSERT(((shown >> 16) & 0xFFu) == (shown & 0xFFu)); /* Gray on screen... */
    WPE_AssetHandle capture = WPE_ASSET_NONE;
    WPE_TEST_ASSERT_OK(wpe_asset_load(wpe_engine_assets(engine), WPE_ASSET_IMAGE, "capture/1", &capture));
    const WPE_Texture* texture = wpe_asset_image(wpe_engine_assets(engine), capture);
    WPE_TEST_ASSERT(texture && texture->levels[0].width == 32 && texture->levels[0].height == 24);
    WPE_TEST_ASSERT_EQ_UINT(0xFFFF0000u, texture->levels[0].texels[0]); /* ...red in the capture. */
    wpe_asset_release(wpe_engine_assets(engine), capture);
    wpe_engine_destroy(engine);
}

static void test_engine_runs_scenes_headless(void) {
    write_tiny_game();
    WPE_Engine* engine = NULL;
    WPE_TEST_ASSERT_OK(wpe_engine_create(
        &(WPE_EngineDesc){.project_dir = ROOT "/game", .headless = true, .workers = 2}, &engine));
    WPE_TEST_ASSERT_EQ_STR("Tiny", wpe_engine_project(engine)->name);
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_input_action_count(wpe_engine_input(engine)));
    WPE_TEST_ASSERT_NEAR(-1.0, wpe_physics_gravity(wpe_engine_physics(engine)).y, 1e-6);
    WPE_TEST_ASSERT(wpe_engine_camera(engine) == WPE_ENTITY_NULL);

    /* Scene a renders the clear color only (no camera), then switches to b, which quits. */
    WPE_TEST_ASSERT_OK(wpe_engine_frame(engine, 1.0 / 30.0));
    WPE_TEST_ASSERT_EQ_UINT(0xFFFF0000u, wpe_renderer_framebuffer(wpe_engine_renderer(engine))->color[0]);
    WPE_TEST_ASSERT_OK(wpe_engine_run(engine, &(WPE_RunOptions){.frames = 100, .fixed_dt = 1.0 / 30.0}));
    WPE_TEST_ASSERT(!wpe_engine_running(engine));
    WPE_TEST_ASSERT_EQ_UINT(
        5, wpe_engine_stats(engine).frames); /* 3 in a (switching after the third), 2 in b. */
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_script_host_stats(wpe_engine_scripts(engine)).errors);
    WPE_TEST_ASSERT_NEAR(-2.0, wpe_physics_gravity(wpe_engine_physics(engine)).y, 1e-6);
    WPE_TEST_ASSERT_EQ_STR("eye", wpe_scene_get_name(wpe_engine_ecs(engine), wpe_engine_camera(engine)));
    /* Script 2D drawing lands on top of the blue background. */
    const WPE_Framebuffer* fb = wpe_renderer_framebuffer(wpe_engine_renderer(engine));
    WPE_TEST_ASSERT_EQ_UINT(0xFF00FF00u, fb->color[2 * fb->width + 2]);
    WPE_TEST_ASSERT_EQ_UINT(0xFF0000FFu, fb->color[20 * fb->width + 30]);

    /* The live world saves as a scene file that loads again. */
    WPE_TEST_ASSERT_OK(wpe_engine_save_scene(engine, ROOT "/game/scenes/saved.scene"));
    WPE_TEST_ASSERT_OK(wpe_engine_load_scene(engine, "scenes/saved.scene"));
    WPE_TEST_ASSERT_EQ_STR("eye", wpe_scene_get_name(wpe_engine_ecs(engine), wpe_engine_camera(engine)));
    WPE_TEST_ASSERT_OK(wpe_engine_screenshot(engine, ROOT "/shot.png"));
    wpe_engine_destroy(engine);

    /* Startup errors. */
    quiet(true);
    WPE_TEST_ASSERT_EQ_INT(
        WPE_RESULT_ERROR_INVALID_ARG,
        wpe_engine_create(&(WPE_EngineDesc){.project_dir = ROOT, .pak_path = "x.pak"}, &engine));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG,
                           wpe_engine_create(&(WPE_EngineDesc){.headless = true}, &engine));
    WPE_TEST_ASSERT_EQ_INT(
        WPE_RESULT_ERROR_NOT_FOUND,
        wpe_engine_create(&(WPE_EngineDesc){.project_dir = ROOT "/game/scenes", .headless = true}, &engine));
    WPE_TEST_ASSERT_EQ_INT(
        WPE_RESULT_ERROR_NOT_FOUND,
        wpe_engine_create(
            &(WPE_EngineDesc){.project_dir = ROOT "/game", .scene = "scenes/none.scene", .headless = true},
            &engine));
    quiet(false);
    WPE_TEST_ASSERT(engine == NULL);
}

static void test_engine_export_embeds_the_project(void) {
    write_tiny_game();
    WPE_TEST_ASSERT_OK(wpe_directory_create(ROOT "/out"));
    write_text(ROOT "/out/runtime.bin", "stand-in for the runtime executable");
    WPE_ExportStats stats;
    WPE_TEST_ASSERT_OK(wpe_export_game(&(WPE_ExportDesc){.project_dir = ROOT "/game",
                                                         .runtime_path = ROOT "/out/runtime.bin",
                                                         .output_path = ROOT "/out/tiny.bin"},
                                       &stats));
    WPE_TEST_ASSERT(stats.files >= 5 && stats.output_bytes > stats.asset_bytes / 4);
    /* The game's files are inside the output, which still starts with the runtime. */
    WPE_Vfs* vfs = NULL;
    WPE_TEST_ASSERT_OK(wpe_vfs_create(&vfs));
    WPE_TEST_ASSERT_OK(wpe_vfs_mount_pak(vfs, ROOT "/out/tiny.bin"));
    WPE_TEST_ASSERT(wpe_vfs_exists(vfs, "project.json") && wpe_vfs_exists(vfs, "scenes/b.scene"));
    wpe_vfs_destroy(vfs);
    WPE_TEST_ASSERT(!wpe_pak_probe(ROOT "/out/tiny.bin.pak.tmp")); /* Temporary archive removed. */

    quiet(true);
    /* Not inside the project, not a broken project, not an already exported game as the runtime. */
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG,
                           wpe_export_game(&(WPE_ExportDesc){.project_dir = ROOT "/game",
                                                             .runtime_path = ROOT "/out/runtime.bin",
                                                             .output_path = ROOT "/game/inside.bin"},
                                           NULL));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG,
                           wpe_export_game(&(WPE_ExportDesc){.project_dir = ROOT "/game",
                                                             .runtime_path = ROOT "/out/tiny.bin",
                                                             .output_path = ROOT "/out/twice.bin"},
                                           NULL));
    write_text(ROOT "/game/project.json",
               "{\"format\": \"wpe.project\", \"version\": 1, \"start_scene\": \"scenes/gone.scene\"}");
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND,
                           wpe_export_game(&(WPE_ExportDesc){.project_dir = ROOT "/game",
                                                             .runtime_path = ROOT "/out/runtime.bin",
                                                             .output_path = ROOT "/out/broken.bin"},
                                           NULL));
    quiet(false);
    WPE_TEST_ASSERT(!wpe_pak_probe(ROOT "/out/broken.bin"));
    write_tiny_game(); /* Restore for other cases. */
}

WPE_TEST_MAIN(
    WPE_TEST_CASE(test_engine_project_from_assets), WPE_TEST_CASE(test_engine_project_rejects_mistakes),
    WPE_TEST_CASE(test_engine_project_write_round_trip), WPE_TEST_CASE(test_engine_scene_round_trip),
    WPE_TEST_CASE(test_engine_scene_rejects_mistakes_and_rolls_back),
    WPE_TEST_CASE(test_engine_scene_game_components), WPE_TEST_CASE(test_engine_scene_editing_functions),
    WPE_TEST_CASE(test_engine_scene_save_filter), WPE_TEST_CASE(test_engine_embedded_edit_mode),
    WPE_TEST_CASE(test_engine_captures_the_frame_before_post_effects),
    WPE_TEST_CASE(test_engine_runs_scenes_headless), WPE_TEST_CASE(test_engine_ui_clicks_reach_scripts),
    WPE_TEST_CASE(test_engine_hit_stop_and_shake), WPE_TEST_CASE(test_engine_scene_transition),
    WPE_TEST_CASE(test_engine_falls_back_to_the_builtin_font),
    WPE_TEST_CASE(test_engine_creates_new_projects), WPE_TEST_CASE(test_engine_export_embeds_the_project))