#include "wpe_test.h"
#include "wpe_test_assets.h"

#include <stdio.h>

#include "assets/procedural.h"
#include "graphics/graphics.h"
#include "scene/scene.h"
#include "script/script.h"

/* Scripts are written to this directory (relative to the test's working directory). */
#define ROOT "script_assets"

static void write_script(const char* name, const char* source) {
    char path[256];
    snprintf(path, sizeof path, ROOT "/%s", name);
    FILE* f = fopen(path, "wb");
    WPE_TEST_ASSERT_MSG(f != NULL, "cannot write %s", path);
    fputs(source, f);
    WPE_TEST_ASSERT(fclose(f) == 0);
}

typedef struct Fixture {
    WPE_TestAssets assets; /* VFS mounting ROOT. */
    WPE_Ecs* ecs;
    WPE_ScriptHost* host;
} Fixture;

static Fixture make_fixture(double time_limit_ms, size_t memory_limit) {
    Fixture f = {.assets = wpe_test_assets_create(ROOT, 0)};
    WPE_TEST_ASSERT_OK(wpe_ecs_create(&f.ecs));
    WPE_TEST_ASSERT_OK(wpe_scene_register_components(f.ecs));
    WPE_TEST_ASSERT_OK(wpe_graphics_register_components(f.ecs, f.assets.assets));
    WPE_TEST_ASSERT_OK(wpe_script_host_create(&(WPE_ScriptHostDesc){.ecs = f.ecs,
                                                                    .assets = f.assets.assets,
                                                                    .time_limit_ms = time_limit_ms,
                                                                    .memory_limit = memory_limit},
                                              &f.host));
    return f;
}

static void free_fixture(Fixture* f) {
    wpe_script_host_destroy(f->host);
    wpe_ecs_destroy(f->ecs);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_asset_manager_stats(f->assets.assets).assets); /* Everything released. */
    wpe_test_assets_destroy(&f->assets);
}

static WPE_Entity spawn(Fixture* f, const char* name) {
    const WPE_Entity e = wpe_ecs_spawn(f->ecs);
    const WPE_Transform t = wpe_transform_identity();
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(f->ecs, e, WPE_COMP_TRANSFORM, &t, NULL));
    if (name) WPE_TEST_ASSERT_OK(wpe_scene_set_name(f->ecs, e, name));
    return e;
}

static WPE_Vec3 position(Fixture* f, WPE_Entity e) {
    return ((WPE_Transform*)wpe_ecs_get_component(f->ecs, e, WPE_COMP_TRANSFORM))->position;
}

static void quiet(bool on) {
    wpe_log_set_stderr_enabled(!on);
}

/* ---- Lifecycle ------------------------------------------------------------------------------------- */

static void test_script_lifecycle_order(void) {
    /* x counts on_start (100), on_update (+1 each), y counts fixed updates; on_destroy renames the parent. */
    write_script("lifecycle.luau",
                 "local S = {}\n"
                 "function S:on_start() self.starts = (self.starts or 0) + 1; wpe.set_position(self.entity, "
                 "vector.create(100, 0, 0)) end\n"
                 "function S:on_update(dt) local p = wpe.get_position(self.entity); "
                 "wpe.set_position(self.entity, p + vector.create(1, 0, 0)); assert(self.starts == 1) end\n"
                 "function S:on_fixed_update(dt) local p = wpe.get_position(self.entity); "
                 "wpe.set_position(self.entity, p + vector.create(0, 1, 0)) end\n"
                 "function S:on_destroy() wpe.set_name(wpe.get_parent(self.entity), 'destroyed') end\n"
                 "return S\n");
    Fixture f = make_fixture(0, 0);
    const WPE_Entity parent = spawn(&f, "parent");
    const WPE_Entity e = spawn(&f, NULL);
    WPE_TEST_ASSERT_OK(wpe_scene_set_parent(f.ecs, e, parent, false));
    WPE_TEST_ASSERT_OK(wpe_script_attach(f.host, e, "lifecycle.luau"));

    /* Fixed updates before the first regular update do nothing (on_start has not run). */
    WPE_TEST_ASSERT_OK(wpe_script_host_fixed_update(f.host, 1.0f / 60.0f));
    WPE_TEST_ASSERT_NEAR(0.0, position(&f, e).x, 0);
    WPE_TEST_ASSERT_OK(wpe_script_host_update(f.host, 0.016f));
    WPE_TEST_ASSERT_NEAR(101.0, position(&f, e).x, 0);
    WPE_TEST_ASSERT_OK(wpe_script_host_update(f.host, 0.016f));
    WPE_TEST_ASSERT_OK(wpe_script_host_fixed_update(f.host, 1.0f / 60.0f));
    WPE_TEST_ASSERT_OK(wpe_script_host_fixed_update(f.host, 1.0f / 60.0f));
    WPE_TEST_ASSERT_NEAR(102.0, position(&f, e).x, 0);
    WPE_TEST_ASSERT_NEAR(2.0, position(&f, e).y, 0);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_script_host_stats(f.host).errors);

    /* Despawning the entity runs on_destroy. */
    WPE_TEST_ASSERT_OK(wpe_ecs_despawn(f.ecs, e));
    WPE_TEST_ASSERT_EQ_STR("destroyed", wpe_scene_get_name(f.ecs, parent));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_script_host_stats(f.host).instances);
    free_fixture(&f);
}

static void test_script_spawn_and_self_destroy(void) {
    write_script("spawner.luau",
                 "local S = {}\n"
                 "function S:on_start() self.spawned = {} for i = 1, 5 do local e = wpe.spawn('child' .. i); "
                 "wpe.set_parent(e, self.entity); table.insert(self.spawned, e) end end\n"
                 "function S:on_update(dt) if wpe.frame() == 2 then wpe.destroy(self.entity) end end\n"
                 "return S\n");
    Fixture f = make_fixture(0, 0);
    const WPE_Entity e = spawn(&f, "spawner");
    WPE_TEST_ASSERT_OK(wpe_script_attach(f.host, e, "spawner.luau"));
    WPE_TEST_ASSERT_OK(wpe_script_host_update(f.host, 0.016f));
    WPE_TEST_ASSERT_EQ_UINT(6, wpe_ecs_entity_count(f.ecs));
    WPE_TEST_ASSERT_OK(wpe_script_host_update(f.host, 0.016f));
    WPE_TEST_ASSERT_OK(
        wpe_script_host_update(f.host, 0.016f)); /* frame() == 2 here: destroy is deferred to the end */
    WPE_TEST_ASSERT(!wpe_ecs_is_alive(f.ecs, e));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_ecs_entity_count(f.ecs)); /* children destroyed recursively */
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_script_host_stats(f.host).errors);
    free_fixture(&f);
}

/* ---- Errors, budgets and sandboxing --------------------------------------------------------------- */

static void test_script_runtime_error_is_isolated(void) {
    write_script("broken.luau", "local S = {}\nfunction S:on_update(dt) error('boom') end\nreturn S\n");
    write_script("healthy.luau",
                 "local S = {}\nfunction S:on_update(dt) local p = wpe.get_position(self.entity); "
                 "wpe.set_position(self.entity, p + vector.create(1, 0, 0)) end\nreturn S\n");
    Fixture f = make_fixture(0, 0);
    const WPE_Entity bad = spawn(&f, NULL), good = spawn(&f, NULL);
    WPE_TEST_ASSERT_OK(wpe_script_attach(f.host, bad, "broken.luau"));
    WPE_TEST_ASSERT_OK(wpe_script_attach(f.host, good, "healthy.luau"));
    quiet(true);
    for (int i = 0; i < 3; ++i)
        WPE_TEST_ASSERT_OK(wpe_script_host_update(f.host, 0.016f));
    quiet(false);
    const WPE_ScriptStats stats = wpe_script_host_stats(f.host);
    WPE_TEST_ASSERT_EQ_UINT(1, stats.errors); /* disabled after the first failure */
    WPE_TEST_ASSERT_EQ_UINT(1, stats.failed);
    WPE_TEST_ASSERT_NEAR(3.0, position(&f, good).x, 0);
    free_fixture(&f);
}

static void test_script_time_budget_interrupts_infinite_loop(void) {
    write_script("hang.luau", "local S = {}\nfunction S:on_update(dt) while true do end end\nreturn S\n");
    Fixture f = make_fixture(30.0, 0);
    const WPE_Entity e = spawn(&f, NULL);
    WPE_TEST_ASSERT_OK(wpe_script_attach(f.host, e, "hang.luau"));
    quiet(true);
    WPE_TEST_ASSERT_OK(wpe_script_host_update(f.host, 0.016f));
    quiet(false);
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_script_host_stats(f.host).failed);
    WPE_TEST_ASSERT_OK(wpe_script_host_run(f.host, "after", "local x = 1 + 1")); /* VM still usable */
    free_fixture(&f);
}

static void test_script_memory_limit(void) {
    write_script("hog.luau", "local S = {}\nfunction S:on_update(dt) local t = {} for i = 1, 1e8 do t[i] = "
                             "string.rep('x', 64) .. i end end\nreturn S\n");
    Fixture f = make_fixture(5000.0, (size_t)8 * 1024 * 1024);
    const WPE_Entity e = spawn(&f, NULL);
    WPE_TEST_ASSERT_OK(wpe_script_attach(f.host, e, "hog.luau"));
    quiet(true);
    WPE_TEST_ASSERT_OK(wpe_script_host_update(f.host, 0.016f));
    quiet(false);
    const WPE_ScriptStats stats = wpe_script_host_stats(f.host);
    WPE_TEST_ASSERT_EQ_UINT(1, stats.failed);
    WPE_TEST_ASSERT(stats.memory <= (size_t)8 * 1024 * 1024);
    WPE_TEST_ASSERT_OK(wpe_script_host_run(f.host, "after", "local t = {1, 2, 3}"));
    free_fixture(&f);
}

static void test_script_sandbox(void) {
    Fixture f = make_fixture(0, 0);
    quiet(true);
    /* Dangerous facilities are absent. */
    WPE_TEST_ASSERT_OK(wpe_script_host_run(
        f.host, "probe",
        "assert(io == nil) assert(loadstring == nil) assert(load == nil) assert(dofile == nil)\n"
        "assert(getfenv == nil) assert(setfenv == nil) assert(os.execute == nil) assert(os.remove == nil)\n"
        "assert(require == nil)"));
    /* Libraries and the engine API are read-only. */
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR, wpe_script_host_run(f.host, "patch", "wpe.spawn = nil"));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR, wpe_script_host_run(f.host, "patch", "math.sin = nil"));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR, wpe_script_host_run(f.host, "patch", "string.rep = nil"));
    /* Globals written by one chunk do not leak into another. */
    WPE_TEST_ASSERT_OK(wpe_script_host_run(f.host, "a", "leaked = 42"));
    WPE_TEST_ASSERT_OK(wpe_script_host_run(f.host, "b", "assert(leaked == nil)"));
    /* Compile errors are reported as such. */
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_PARSE, wpe_script_host_run(f.host, "bad", "local = = 1"));
    quiet(false);
    WPE_TEST_ASSERT_EQ_UINT(4, wpe_script_host_stats(f.host).errors);
    free_fixture(&f);
}

static void test_script_attach_validation(void) {
    write_script("not_a_table.luau", "return 42\n");
    write_script("syntax.luau", "local S = {\nreturn S\n");
    write_script("ok.luau", "return {}\n");
    Fixture f = make_fixture(0, 0);
    const WPE_Entity e = spawn(&f, NULL);
    quiet(true);
    const char* unsafe[] = {"../secret.luau", "/abs.luau", "dir\\file.luau", "c:file.luau", "a//b.luau",
                            "./ok.luau",      ""};
    for (size_t i = 0; i < sizeof unsafe / sizeof unsafe[0]; ++i) {
        WPE_TEST_ASSERT_MSG(wpe_script_attach(f.host, e, unsafe[i]) == WPE_RESULT_ERROR_INVALID_ARG,
                            "accepted '%s'", unsafe[i]);
    }
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND, wpe_script_attach(f.host, e, "missing.luau"));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_PARSE, wpe_script_attach(f.host, e, "not_a_table.luau"));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_PARSE, wpe_script_attach(f.host, e, "syntax.luau"));
    quiet(false);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND, wpe_script_attach(f.host, WPE_ENTITY_NULL, "ok.luau"));
    WPE_TEST_ASSERT_OK(wpe_script_attach(f.host, e, "ok.luau"));
    WPE_TEST_ASSERT_OK(wpe_script_attach(f.host, e, "ok.luau")); /* replacing is allowed */
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_script_host_stats(f.host).instances);
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_script_host_stats(f.host).scripts);
    free_fixture(&f);
}

static void test_script_on_destroy_is_restricted(void) {
    write_script(
        "greedy.luau",
        "local S = {}\nfunction S:on_start() end\nfunction S:on_destroy() wpe.spawn('nope') end\nreturn S\n");
    Fixture f = make_fixture(0, 0);
    const WPE_Entity e = spawn(&f, NULL);
    WPE_TEST_ASSERT_OK(wpe_script_attach(f.host, e, "greedy.luau"));
    WPE_TEST_ASSERT_OK(wpe_script_host_update(f.host, 0.016f));
    quiet(true);
    WPE_TEST_ASSERT_OK(wpe_ecs_despawn(f.ecs, e));
    quiet(false);
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_script_host_stats(f.host).errors);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_ecs_entity_count(f.ecs));
    free_fixture(&f);
}

/* ---- Hot reload ------------------------------------------------------------------------------------ */

static void test_script_hot_reload_preserves_state(void) {
    write_script("counter.luau", "local S = {}\nfunction S:on_start() self.count = 0 end\n"
                                 "function S:on_update(dt) self.count += 1; wpe.set_position(self.entity, "
                                 "vector.create(self.count, 0, 0)) end\n"
                                 "return S\n");
    Fixture f = make_fixture(0, 0);
    const WPE_Entity e = spawn(&f, NULL);
    WPE_TEST_ASSERT_OK(wpe_script_attach(f.host, e, "counter.luau"));
    for (int i = 0; i < 3; ++i)
        WPE_TEST_ASSERT_OK(wpe_script_host_update(f.host, 0.016f));
    WPE_TEST_ASSERT_NEAR(3.0, position(&f, e).x, 0);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_script_host_reload_changed(f.host));

    /* New code: counts by 10 and records that on_reload ran; self.count keeps its value. */
    write_script("counter.luau", "local S = {}\nfunction S:on_start() self.count = 0 end\n"
                                 "function S:on_reload() self.reloaded = true end\n"
                                 "function S:on_update(dt) self.count += 10; wpe.set_position(self.entity, "
                                 "vector.create(self.count, self.reloaded and 1 or 0, 0)) end\n"
                                 "return S\n");
    WPE_TEST_ASSERT_OK(wpe_script_host_reload(f.host, "counter.luau"));
    WPE_TEST_ASSERT_OK(wpe_script_host_update(f.host, 0.016f));
    WPE_TEST_ASSERT_NEAR(13.0, position(&f, e).x, 0);
    WPE_TEST_ASSERT_NEAR(1.0, position(&f, e).y, 0);

    /* A broken edit keeps the previous version running. */
    write_script("counter.luau", "local S = {\nreturn S\n");
    quiet(true);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_PARSE, wpe_script_host_reload(f.host, "counter.luau"));
    quiet(false);
    WPE_TEST_ASSERT_OK(wpe_script_host_update(f.host, 0.016f));
    WPE_TEST_ASSERT_NEAR(23.0, position(&f, e).x, 0);

    /* Reloading clears a failed state so fixed code runs again. */
    write_script("counter.luau", "local S = {}\nfunction S:on_update(dt) error('oops') end\nreturn S\n");
    quiet(true);
    WPE_TEST_ASSERT_OK(wpe_script_host_reload(f.host, "counter.luau"));
    WPE_TEST_ASSERT_OK(wpe_script_host_update(f.host, 0.016f));
    quiet(false);
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_script_host_stats(f.host).failed);
    write_script("counter.luau", "local S = {}\nfunction S:on_update(dt) wpe.set_position(self.entity, "
                                 "vector.create(-1, 0, 0)) end\nreturn S\n");
    WPE_TEST_ASSERT_OK(wpe_script_host_reload(f.host, "counter.luau"));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_script_host_stats(f.host).failed);
    WPE_TEST_ASSERT_OK(wpe_script_host_update(f.host, 0.016f));
    WPE_TEST_ASSERT_NEAR(-1.0, position(&f, e).x, 0);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND, wpe_script_host_reload(f.host, "never_loaded.luau"));
    free_fixture(&f);
}

/* ---- API ------------------------------------------------------------------------------------------- */

static void test_script_api_transforms_lights_prefabs(void) {
    write_script(
        "api.luau",
        "local S = {}\n"
        "function S:on_start()\n"
        "  local e = self.entity\n"
        "  wpe.set_rotation(e, vector.create(0.3, -0.4, 0.5))\n"
        "  local r = wpe.get_rotation(e)\n"
        "  assert(math.abs(r.x - 0.3) < 1e-4 and math.abs(r.y + 0.4) < 1e-4 and math.abs(r.z - 0.5) < 1e-4)\n"
        "  wpe.set_scale(e, 2)\n"
        "  assert(wpe.get_scale(e) == vector.create(2, 2, 2))\n"
        "  wpe.set_light(e, {type = wpe.LIGHT_SPOT, color = vector.create(1, 0, 0), intensity = 3, range = "
        "7})\n"
        "  local l = wpe.get_light(e)\n"
        "  assert(l.type == wpe.LIGHT_SPOT and l.intensity == 3 and l.range == 7 and l.color.x == 1)\n"
        "  local box = wpe.instantiate('box', vector.create(4, 5, 6))\n"
        "  assert(wpe.get_position(box) == vector.create(4, 5, 6))\n"
        "  wpe.set_parent(box, e, false)\n"
        "  assert(#wpe.children(e) == 1 and wpe.get_parent(box) == e)\n"
        "  local probe = wpe.spawn('probe')\n"
        "  wpe.set_position(probe, vector.create(0, 0, 0))\n"
        "  wpe.look_at(probe, vector.create(10, 0, 0))\n"
        "  local f = wpe.forward(probe)\n"
        "  assert(math.abs(f.x - 1) < 1e-4 and math.abs(f.y) < 1e-4)\n"
        "  assert(wpe.get_name(probe) == 'probe' and wpe.is_alive(probe))\n"
        "  assert(not pcall(wpe.get_position, 123456789))\n" /* dead/invalid handles raise */
        "  assert(not pcall(wpe.instantiate, '../escape.glb'))\n"
        "  wpe.log('api checks passed', 1, true)\n"
        "  wpe.set_name(e, 'ok')\n"
        "end\n"
        "return S\n");
    Fixture f = make_fixture(0, 0);
    WPE_Model* box_model = NULL;
    WPE_TEST_ASSERT_OK(wpe_model_create_box(wpe_vec3(1, 1, 1), NULL, &box_model));
    const WPE_AssetHandle box = wpe_test_add_model(f.assets.assets, "test/box", box_model);
    WPE_TEST_ASSERT_OK(wpe_script_host_register_prefab(f.host, "box", box));
    wpe_asset_release(f.assets.assets, box); /* The prefab table keeps it. */
    const WPE_Entity e = spawn(&f, NULL);
    WPE_TEST_ASSERT_OK(wpe_script_attach(f.host, e, "api.luau"));
    quiet(true);
    WPE_TEST_ASSERT_OK(wpe_script_host_update(f.host, 0.016f));
    quiet(false);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_script_host_stats(f.host).errors);
    WPE_TEST_ASSERT_EQ_STR("ok", wpe_scene_get_name(f.ecs, e));
    WPE_TEST_ASSERT(wpe_ecs_has_component(f.ecs, e, WPE_COMP_LIGHT));
    free_fixture(&f);
}

static void test_script_host_destroy_and_recreate(void) {
    write_script("bye.luau", "local S = {}\nfunction S:on_start() end\nfunction S:on_destroy() "
                             "wpe.set_name(self.entity, 'bye') end\nreturn S\n");
    WPE_TestAssets assets = wpe_test_assets_create(ROOT, 0);
    WPE_Ecs* ecs = NULL;
    WPE_TEST_ASSERT_OK(wpe_ecs_create(&ecs));
    WPE_TEST_ASSERT_OK(wpe_scene_register_components(ecs));
    WPE_TEST_ASSERT_OK(wpe_graphics_register_components(ecs, assets.assets));
    const WPE_ScriptHostDesc desc = {.ecs = ecs, .assets = assets.assets};
    WPE_ScriptHost* host = NULL;
    WPE_TEST_ASSERT_OK(wpe_script_host_create(&desc, &host));
    WPE_ScriptHost* second = NULL;
    wpe_log_set_stderr_enabled(false);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_ALREADY_EXISTS, wpe_script_host_create(&desc, &second));
    wpe_log_set_stderr_enabled(true);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NULL_ARG,
                           wpe_script_host_create(&(WPE_ScriptHostDesc){.ecs = ecs}, &second));

    const WPE_Entity e = wpe_ecs_spawn(ecs);
    WPE_TEST_ASSERT_OK(wpe_scene_set_name(ecs, e, "hello"));
    WPE_TEST_ASSERT_OK(wpe_script_attach(host, e, "bye.luau"));
    WPE_TEST_ASSERT_OK(wpe_script_host_update(host, 0.016f));
    wpe_script_host_destroy(host);
    WPE_TEST_ASSERT_EQ_STR("bye", wpe_scene_get_name(ecs, e));
    WPE_TEST_ASSERT(!wpe_ecs_is_registered(ecs, WPE_COMP_SCRIPT));

    WPE_TEST_ASSERT_OK(wpe_script_host_create(&desc, &host));
    wpe_script_host_destroy(host);
    wpe_ecs_destroy(ecs);
    wpe_test_assets_destroy(&assets);
}

/* ---- Fixed step ------------------------------------------------------------------------------------ */

static void test_script_fixed_step_clock(void) {
    WPE_FixedStep clock;
    wpe_fixed_step_init(&clock, 0.01, 5);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_fixed_step_advance(&clock, 0.005));
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_fixed_step_advance(&clock, 0.006));
    WPE_TEST_ASSERT_NEAR(0.1, wpe_fixed_step_alpha(&clock), 1e-9);
    WPE_TEST_ASSERT_EQ_UINT(3, wpe_fixed_step_advance(&clock, 0.03));
    /* A one-second hitch is clamped to max_steps and the rest is dropped, not banked. */
    WPE_TEST_ASSERT_EQ_UINT(5, wpe_fixed_step_advance(&clock, 1.0));
    WPE_TEST_ASSERT(clock.dropped >= 90);
    WPE_TEST_ASSERT(clock.accumulator < clock.step);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_fixed_step_advance(&clock, -1.0));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_fixed_step_advance(&clock, NAN));
    wpe_fixed_step_init(&clock, 0.0, 0);
    WPE_TEST_ASSERT_NEAR(1.0 / 60.0, clock.step, 1e-12);
    WPE_TEST_ASSERT_EQ_UINT(1, clock.max_steps);
}

static void test_script_quat_euler_roundtrip(void) {
    const float angles[][3] = {{0.3f, -0.4f, 0.5f}, {1.2f, 0.2f, -2.9f}, {-0.1f, 1.5f, 0.0f}, {0, 0, 0}};
    for (size_t i = 0; i < sizeof angles / sizeof angles[0]; ++i) {
        const WPE_Vec3 a = wpe_vec3(angles[i][0], angles[i][1], angles[i][2]);
        const WPE_Quat q = wpe_quat_from_euler_xyz(a);
        const WPE_Quat back = wpe_quat_from_euler_xyz(wpe_quat_to_euler_xyz(q));
        const float dot = q.x * back.x + q.y * back.y + q.z * back.z + q.w * back.w;
        WPE_TEST_ASSERT_NEAR(1.0, fabsf(dot), 1e-4);
    }
    /* Gimbal lock still reproduces the same rotation. */
    const WPE_Quat locked = wpe_quat_from_euler_xyz(wpe_vec3(0.7f, WPE_PI / 2, 0.2f));
    const WPE_Quat back = wpe_quat_from_euler_xyz(wpe_quat_to_euler_xyz(locked));
    WPE_TEST_ASSERT_NEAR(
        1.0, fabsf(locked.x * back.x + locked.y * back.y + locked.z * back.z + locked.w * back.w), 1e-3);
}

/* ---- Physics --------------------------------------------------------------------------------------- */

typedef struct PhysicsFixture {
    WPE_TestAssets assets;
    WPE_Ecs* ecs;
    WPE_Physics* physics;
    WPE_ScriptHost* host;
} PhysicsFixture;

static PhysicsFixture make_physics_fixture(void) {
    PhysicsFixture f = {.assets = wpe_test_assets_create(ROOT, 0)};
    WPE_TEST_ASSERT_OK(wpe_ecs_create(&f.ecs));
    WPE_TEST_ASSERT_OK(wpe_scene_register_components(f.ecs));
    WPE_TEST_ASSERT_OK(wpe_graphics_register_components(f.ecs, f.assets.assets));
    WPE_TEST_ASSERT_OK(wpe_physics_create(
        &(WPE_PhysicsDesc){.ecs = f.ecs, .assets = f.assets.assets, .gravity = wpe_vec3(0.0f, -9.81f, 0.0f)},
        &f.physics));
    WPE_TEST_ASSERT_OK(wpe_script_host_create(
        &(WPE_ScriptHostDesc){.ecs = f.ecs, .assets = f.assets.assets, .physics = f.physics}, &f.host));
    return f;
}

static void free_physics_fixture(PhysicsFixture* f) {
    wpe_script_host_destroy(f->host);
    wpe_physics_destroy(f->physics);
    wpe_ecs_destroy(f->ecs);
    wpe_test_assets_destroy(&f->assets);
}

/* One frame in the documented order: update, fixed update, physics step, collision callbacks. */
static void physics_frame(PhysicsFixture* f) {
    const float dt = 1.0f / 60.0f;
    WPE_TEST_ASSERT_OK(wpe_script_host_update(f->host, dt));
    WPE_TEST_ASSERT_OK(wpe_script_host_fixed_update(f->host, dt));
    WPE_TEST_ASSERT_OK(wpe_physics_step(f->physics, dt));
    WPE_TEST_ASSERT_OK(wpe_script_host_dispatch_physics(f->host));
}

static void test_script_physics_api_and_callbacks(void) {
    write_script("ball.luau",
                 "local B = {}\n"
                 "function B:on_start()\n"
                 "  wpe.add_rigidbody(self.entity, {type = wpe.BODY_DYNAMIC, linear_damping = 0})\n"
                 "  wpe.add_collider(self.entity, {shape = wpe.COLLIDER_SPHERE, radius = 0.5})\n"
                 "  wpe.apply_impulse(self.entity, vector.create(0, 2, 0))\n"
                 "  local v = wpe.get_velocity(self.entity)\n"
                 "  assert(math.abs(v.y - 2 / (4 / 3 * math.pi * 0.125)) < 1e-3)\n"
                 "  self.hits = 0\n"
                 "end\n"
                 "function B:on_collision_enter(other)\n"
                 "  self.hits += 1\n"
                 "  wpe.set_name(self.entity, 'hit ' .. wpe.get_name(other) .. ' ' .. self.hits)\n"
                 "end\n"
                 "function B:on_trigger_enter(other) wpe.set_scale(self.entity, vector.create(2, 2, 2)) end\n"
                 "return B\n");
    write_script("zone.luau",
                 "local Z = {}\n"
                 "function Z:on_start() wpe.add_collider(self.entity, {shape = wpe.COLLIDER_BOX, size = "
                 "vector.create(4, 1, 4), trigger = true}) end\n"
                 "function Z:on_trigger_enter(other) wpe.set_name(self.entity, 'entered') end\n"
                 "function Z:on_trigger_exit(other) wpe.set_name(self.entity, 'exited') end\n"
                 "return Z\n");
    write_script("probe.luau",
                 "local P = {}\n"
                 "function P:on_update(dt)\n"
                 "  if wpe.frame() == 0 then return end -- the ground is created by the first step\n"
                 "  local hit = wpe.raycast(vector.create(5, 10, 0), vector.create(0, -1, 0))\n"
                 "  assert(hit and math.abs(hit.distance - 10) < 1e-3 and hit.normal.y > 0.99)\n"
                 "  wpe.set_name(self.entity, 'saw ' .. wpe.get_name(hit.entity))\n"
                 "  assert(wpe.raycast(vector.create(5, 10, 0), vector.create(0, 1, 0)) == nil)\n"
                 "  wpe.set_gravity(wpe.get_gravity())\n"
                 "end\n"
                 "return P\n");
    PhysicsFixture f = make_physics_fixture();
    /* Ground set up from C, named for the scripts. */
    const WPE_Entity ground = wpe_ecs_spawn(f.ecs);
    WPE_Transform t = wpe_transform_identity();
    t.position.y = -0.5f;
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(f.ecs, ground, WPE_COMP_TRANSFORM, &t, NULL));
    WPE_TEST_ASSERT_OK(wpe_scene_set_name(f.ecs, ground, "ground"));
    WPE_Collider floor = wpe_collider_default(WPE_COLLIDER_BOX);
    floor.size = wpe_vec3(40.0f, 1.0f, 40.0f);
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(f.ecs, ground, WPE_COMP_PHYSICS_COLLIDER, &floor, NULL));

    const WPE_Entity ball = wpe_ecs_spawn(f.ecs);
    t = wpe_transform_identity();
    t.position.y = 4.0f;
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(f.ecs, ball, WPE_COMP_TRANSFORM, &t, NULL));
    WPE_TEST_ASSERT_OK(wpe_script_attach(f.host, ball, "ball.luau"));
    const WPE_Entity zone = wpe_ecs_spawn(f.ecs);
    t = wpe_transform_identity();
    t.position.y = 2.0f;
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(f.ecs, zone, WPE_COMP_TRANSFORM, &t, NULL));
    WPE_TEST_ASSERT_OK(wpe_script_attach(f.host, zone, "zone.luau"));
    const WPE_Entity probe = wpe_ecs_spawn(f.ecs);
    WPE_TEST_ASSERT_OK(wpe_script_attach(f.host, probe, "probe.luau"));

    for (int i = 0; i < 180; ++i)
        physics_frame(&f);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_script_host_stats(f.host).errors);
    WPE_TEST_ASSERT_EQ_STR("hit ground 1", wpe_scene_get_name(f.ecs, ball));
    WPE_TEST_ASSERT_EQ_STR("exited", wpe_scene_get_name(f.ecs, zone));
    WPE_TEST_ASSERT_EQ_STR("saw ground", wpe_scene_get_name(f.ecs, probe));
    const WPE_Transform* ball_t = wpe_ecs_get_component(f.ecs, ball, WPE_COMP_TRANSFORM);
    WPE_TEST_ASSERT_NEAR(2.0, ball_t->scale.x, 0);       /* The visitor got on_trigger_enter too. */
    WPE_TEST_ASSERT_NEAR(0.5, ball_t->position.y, 0.02); /* Collider built before the scale change. */
    free_physics_fixture(&f);
}

static void test_script_physics_errors(void) {
    write_script("no_physics.luau", "local S = {}\n"
                                    "function S:on_start() wpe.add_rigidbody(self.entity) end\n"
                                    "return S\n");
    write_script("bad_collider.luau", "local S = {}\n"
                                      "function S:on_start() wpe.add_collider(self.entity, {shape = "
                                      "wpe.COLLIDER_MESH}) end\n"
                                      "return S\n");
    write_script("bad_force.luau",
                 "local S = {}\n"
                 "function S:on_start() wpe.apply_force(self.entity, vector.create(1, 0, 0)) end\n"
                 "return S\n");
    write_script("destroy_physics.luau", "local S = {}\n"
                                         "function S:on_destroy() wpe.add_collider(self.entity) end\n"
                                         "return S\n");
    quiet(true);
    Fixture plain = make_fixture(0, 0);
    WPE_TEST_ASSERT_OK(wpe_script_attach(plain.host, spawn(&plain, NULL), "no_physics.luau"));
    WPE_TEST_ASSERT_OK(wpe_script_host_update(plain.host, 0.016f));
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_script_host_stats(plain.host).errors);
    WPE_TEST_ASSERT_OK(wpe_script_host_dispatch_physics(plain.host)); /* No-op without physics. */
    free_fixture(&plain);

    PhysicsFixture f = make_physics_fixture();
    const char* scripts[] = {"bad_collider.luau", "bad_force.luau", "destroy_physics.luau"};
    WPE_Entity entities[3];
    for (int i = 0; i < 3; ++i) {
        entities[i] = wpe_ecs_spawn(f.ecs);
        const WPE_Transform t = wpe_transform_identity();
        WPE_TEST_ASSERT_OK(wpe_ecs_add_component(f.ecs, entities[i], WPE_COMP_TRANSFORM, &t, NULL));
        WPE_TEST_ASSERT_OK(wpe_script_attach(f.host, entities[i], scripts[i]));
    }
    physics_frame(&f);
    WPE_TEST_ASSERT_EQ_UINT(2, wpe_script_host_stats(f.host).errors);
    WPE_TEST_ASSERT(!wpe_ecs_has_component(f.ecs, entities[0], WPE_COMP_PHYSICS_COLLIDER));
    WPE_TEST_ASSERT_OK(wpe_ecs_despawn(f.ecs, entities[2])); /* on_destroy may not add physics. */
    WPE_TEST_ASSERT_EQ_UINT(3, wpe_script_host_stats(f.host).errors);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_physics_stats(f.physics).bodies);
    quiet(false);
    free_physics_fixture(&f);
}

/* ---- Audio ----------------------------------------------------------------------------------------- */

static void test_script_audio_api(void) {
    WPE_TEST_ASSERT_OK(wpe_directory_create(ROOT "/sounds"));
    wpe_test_write_wav(ROOT "/sounds/beep.wav", 1, 22050, 22050 / 4, 440.0f);
    wpe_test_write_wav(ROOT "/sounds/music.wav", 2, 48000, 48000, 220.0f);
    write_script("audio.luau",
                 "local S = {}\n"
                 "function S:on_start()\n"
                 "  local v = wpe.play_sound('sounds/beep.wav', {volume = 0.5, pitch = 1.5, position = "
                 "vector.create(1, 0, 0), bus = wpe.BUS_EFFECTS})\n"
                 "  assert(v > 0)\n"
                 "  wpe.stop_sound(v)\n"
                 "  wpe.stop_sound(v) -- stale ids are ignored\n"
                 "  wpe.add_audio_source(self.entity, {sound = 'sounds/beep.wav', loop = true, volume = 0.8, "
                 "spatial = false})\n"
                 "  assert(wpe.is_audio_playing(self.entity))\n"
                 "  wpe.set_audio_playing(self.entity, false)\n"
                 "  assert(not wpe.is_audio_playing(self.entity))\n"
                 "  wpe.set_audio_playing(self.entity, true)\n"
                 "  wpe.play_music('sounds/music.wav', 0.25, true)\n"
                 "  wpe.set_volume(wpe.BUS_MUSIC, 0.5)\n"
                 "  assert(wpe.get_volume(wpe.BUS_MUSIC) == 0.5 and wpe.get_volume(wpe.BUS_MASTER) == 1)\n"
                 "  assert(not pcall(wpe.set_volume, 7, 1))\n"
                 "  assert(not pcall(wpe.play_sound, 'sounds/missing.wav'))\n"
                 "  assert(not pcall(wpe.play_music, 'sounds/missing.wav'))\n"
                 "  assert(not pcall(wpe.add_audio_source, self.entity, {volume = 1}))\n"
                 "  wpe.set_name(self.entity, 'audio ok')\n"
                 "end\n"
                 "function S:on_update(dt)\n"
                 "  if wpe.frame() == 3 then wpe.stop_music(); wpe.remove_audio_source(self.entity) end\n"
                 "end\n"
                 "return S\n");
    WPE_TestAssets assets = wpe_test_assets_create(ROOT, 0);
    WPE_Ecs* ecs = NULL;
    WPE_TEST_ASSERT_OK(wpe_ecs_create(&ecs));
    WPE_TEST_ASSERT_OK(wpe_scene_register_components(ecs));
    WPE_TEST_ASSERT_OK(wpe_graphics_register_components(ecs, assets.assets));
    WPE_Audio* audio = NULL;
    WPE_TEST_ASSERT_OK(
        wpe_audio_create(&(WPE_AudioDesc){.ecs = ecs, .assets = assets.assets, .no_device = true}, &audio));
    WPE_ScriptHost* host = NULL;
    WPE_TEST_ASSERT_OK(wpe_script_host_create(
        &(WPE_ScriptHostDesc){.ecs = ecs, .assets = assets.assets, .audio = audio}, &host));

    const WPE_Entity e = wpe_ecs_spawn(ecs);
    const WPE_Transform t = wpe_transform_identity();
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, e, WPE_COMP_TRANSFORM, &t, NULL));
    WPE_TEST_ASSERT_OK(wpe_script_attach(host, e, "audio.luau"));
    quiet(true); /* The failing pcall checks log errors. */
    WPE_TEST_ASSERT_OK(wpe_script_host_update(host, 0.016f));
    quiet(false);
    WPE_TEST_ASSERT_OK(wpe_audio_update(audio));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_script_host_stats(host).errors);
    WPE_TEST_ASSERT_EQ_STR("audio ok", wpe_scene_get_name(ecs, e));
    WPE_TEST_ASSERT_EQ_UINT(2, wpe_audio_stats(audio).voices); /* Looping source and music. */
    WPE_TEST_ASSERT_NEAR(0.5, wpe_audio_volume(audio, WPE_AUDIO_BUS_MUSIC), 0);

    for (int i = 0; i < 3; ++i) {
        WPE_TEST_ASSERT_OK(wpe_script_host_update(host, 0.016f));
        WPE_TEST_ASSERT_OK(wpe_audio_update(audio));
    }
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_audio_stats(audio).voices);
    WPE_TEST_ASSERT(!wpe_ecs_has_component(ecs, e, WPE_COMP_AUDIO_SOURCE));

    /* The documented destroy order releases every sound before audio shuts down. */
    wpe_script_host_destroy(host);
    wpe_audio_destroy(audio);
    wpe_ecs_destroy(ecs);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_asset_manager_stats(assets.assets).assets);
    wpe_test_assets_destroy(&assets);

    /* Without audio the API raises. */
    write_script("no_audio.luau",
                 "local S = {}\nfunction S:on_start() wpe.play_sound('sounds/beep.wav') end\nreturn S\n");
    quiet(true);
    Fixture plain = make_fixture(0, 0);
    WPE_TEST_ASSERT_OK(wpe_script_attach(plain.host, spawn(&plain, NULL), "no_audio.luau"));
    WPE_TEST_ASSERT_OK(wpe_script_host_update(plain.host, 0.016f));
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_script_host_stats(plain.host).errors);
    free_fixture(&plain);
    quiet(false);
}

static void test_script_ecs_unregister_component(void) {
    WPE_Ecs* ecs = NULL;
    WPE_TEST_ASSERT_OK(wpe_ecs_create(&ecs));
    WPE_TEST_ASSERT_OK(
        wpe_ecs_register_component(ecs, 40, &(WPE_ComponentDesc){.name = "Temp", .size = 4, .alignment = 4}));
    const WPE_Entity e = wpe_ecs_spawn(ecs);
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, e, 40, NULL, NULL));
    wpe_log_set_stderr_enabled(false);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_STATE, wpe_ecs_unregister_component(ecs, 40));
    wpe_log_set_stderr_enabled(true);
    WPE_TEST_ASSERT_OK(wpe_ecs_remove_component(ecs, e, 40));
    WPE_TEST_ASSERT_OK(wpe_ecs_unregister_component(ecs, 40));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND, wpe_ecs_unregister_component(ecs, 40));
    WPE_TEST_ASSERT_OK(wpe_ecs_register_component(
        ecs, 40, &(WPE_ComponentDesc){.name = "Again", .size = 8, .alignment = 8}));
    wpe_ecs_destroy(ecs);
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_script_lifecycle_order), WPE_TEST_CASE(test_script_spawn_and_self_destroy),
              WPE_TEST_CASE(test_script_runtime_error_is_isolated),
              WPE_TEST_CASE(test_script_time_budget_interrupts_infinite_loop),
              WPE_TEST_CASE(test_script_memory_limit), WPE_TEST_CASE(test_script_sandbox),
              WPE_TEST_CASE(test_script_attach_validation),
              WPE_TEST_CASE(test_script_on_destroy_is_restricted),
              WPE_TEST_CASE(test_script_hot_reload_preserves_state),
              WPE_TEST_CASE(test_script_api_transforms_lights_prefabs),
              WPE_TEST_CASE(test_script_host_destroy_and_recreate),
              WPE_TEST_CASE(test_script_fixed_step_clock), WPE_TEST_CASE(test_script_quat_euler_roundtrip),
              WPE_TEST_CASE(test_script_physics_api_and_callbacks), WPE_TEST_CASE(test_script_physics_errors),
              WPE_TEST_CASE(test_script_audio_api), WPE_TEST_CASE(test_script_ecs_unregister_component))
