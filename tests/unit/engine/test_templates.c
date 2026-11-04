#include "wpe_test.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "engine/engine.h"
#include "scene/scene.h"

/* Projects are created below the test's working directory (the build tree). */
#define ROOT "templates_scratch"
#define MAX_DIRECTORIES 32

/* ---- Helpers --------------------------------------------------------------------------------------- */

typedef struct Removal {
    const char* root;
    char directories[MAX_DIRECTORIES][256];
    uint32_t directory_count;
} Removal;

static bool remove_file(const char* relative_path, void* user_data) {
    Removal* removal = user_data;
    char path[512];
    snprintf(path, sizeof path, "%s/%s", removal->root, relative_path);
    WPE_TEST_ASSERT_OK(wpe_path_remove(path));
    /* Remember every directory above the file, to remove once they are empty. */
    char directory[256];
    snprintf(directory, sizeof directory, "%s", relative_path);
    for (char* slash = strrchr(directory, '/'); slash; slash = strrchr(directory, '/')) {
        *slash = '\0';
        bool known = false;
        for (uint32_t i = 0; i < removal->directory_count && !known; ++i)
            known = strcmp(removal->directories[i], directory) == 0;
        if (!known && removal->directory_count < MAX_DIRECTORIES)
            snprintf(removal->directories[removal->directory_count++], 256, "%s", directory);
    }
    return true;
}

/* Deletes a directory tree made by wpe_project_create, if present: files, then directories deepest first. */
static void remove_project(const char* directory) {
    if (!wpe_directory_exists(directory)) return;
    static Removal removal;
    removal = (Removal){.root = directory};
    WPE_TEST_ASSERT_OK(wpe_directory_walk(directory, remove_file, &removal));
    for (;;) {
        uint32_t deepest = MAX_DIRECTORIES;
        for (uint32_t i = 0; i < removal.directory_count; ++i) {
            if (removal.directories[i][0] &&
                (deepest == MAX_DIRECTORIES ||
                 strlen(removal.directories[i]) > strlen(removal.directories[deepest])))
                deepest = i;
        }
        if (deepest == MAX_DIRECTORIES) break;
        char path[512];
        snprintf(path, sizeof path, "%s/%s", directory, removal.directories[deepest]);
        WPE_TEST_ASSERT_OK(wpe_path_remove(path));
        removal.directories[deepest][0] = '\0';
    }
    WPE_TEST_ASSERT_OK(wpe_path_remove(directory));
}

/* A fresh project from a template, at ROOT/<template>. */
static void create_project(const char* template_name, char* out_dir, size_t capacity) {
    WPE_TEST_ASSERT_OK(wpe_directory_create(ROOT));
    snprintf(out_dir, capacity, ROOT "/%s", template_name);
    remove_project(out_dir); /* Left by an earlier run. */
    WPE_TEST_ASSERT_OK(wpe_project_create(out_dir, "Template Test", template_name));
}

static WPE_Engine* create_engine(const char* project_dir) {
    WPE_Engine* engine = NULL;
    WPE_TEST_ASSERT_OK(wpe_engine_create(&(WPE_EngineDesc){.project_dir = project_dir,
                                                           .embedded = true,
                                                           .no_audio_device = true,
                                                           .save_dir = ROOT "/saves",
                                                           .workers = 2},
                                         &engine));
    return engine;
}

static WPE_Entity find(WPE_Ecs* ecs, const char* name) {
    WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_TRANSFORM)});
    while (wpe_ecs_iter_next(&it)) {
        const char* entity_name = wpe_scene_get_name(ecs, it.entity);
        if (entity_name && strcmp(entity_name, name) == 0) return it.entity;
    }
    return WPE_ENTITY_NULL;
}

static WPE_Vec3 position(WPE_Ecs* ecs, WPE_Entity entity) {
    const WPE_Transform* t = wpe_ecs_get_component(ecs, entity, WPE_COMP_TRANSFORM);
    WPE_TEST_ASSERT(t != NULL);
    return t->position;
}

static void frames(WPE_Engine* engine, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i)
        WPE_TEST_ASSERT_OK(wpe_engine_frame(engine, 1.0 / 30.0));
}

/* A key going down or up (the whole event zeroed: see CLAUDE.md on SDL events in tests). */
static void key(WPE_Engine* engine, SDL_Scancode scancode, bool down) {
    SDL_Event e;
    SDL_zero(e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.scancode = scancode;
    e.key.down = down;
    wpe_engine_handle_event(engine, (const WPE_PlatformEvent*)&e);
}

/* A key tapped: down for one frame, then up. */
static void tap(WPE_Engine* engine, SDL_Scancode scancode) {
    key(engine, scancode, true);
    frames(engine, 1);
    key(engine, scancode, false);
    frames(engine, 1);
}

static uint32_t script_errors(WPE_Engine* engine) {
    return wpe_script_host_stats(wpe_engine_scripts(engine)).errors;
}

/* ---- Every template -------------------------------------------------------------------------------- */

typedef struct Comparison {
    const char* project;
    const char* source;
    uint32_t files;
} Comparison;

/* Each created file (project.json aside, which gets the new name) equals the template's source file. */
static bool compare_file(const char* relative_path, void* user_data) {
    Comparison* c = user_data;
    if (strncmp(relative_path, ".claude/", 8) == 0) return true; /* The skill, not the template's. */
    ++c->files;
    if (strcmp(relative_path, "project.json") == 0) return true;
    char created_path[512];
    char source_path[512];
    snprintf(created_path, sizeof created_path, "%s/%s", c->project, relative_path);
    snprintf(source_path, sizeof source_path, "%s/%s", c->source, relative_path);
    void* created = NULL;
    void* source = NULL;
    size_t created_size = 0;
    size_t source_size = 0;
    WPE_TEST_ASSERT_OK(wpe_file_read_all(created_path, &created, &created_size));
    WPE_TEST_ASSERT_OK(wpe_file_read_all(source_path, &source, &source_size));
    const bool same = created_size == source_size && memcmp(created, source, created_size) == 0;
    wpe_free(created);
    wpe_free(source);
    WPE_TEST_ASSERT_MSG(same, "%s differs from the template's source", relative_path);
    return true;
}

static bool count_file(const char* relative_path, void* user_data) {
    (void)relative_path;
    ++*(uint32_t*)user_data;
    return true;
}

/* Every template creates a project that holds its files, carries the new name, and plays for 20 seconds
 * without a script error, drawing something other than one flat color. */
static void test_templates_create_projects_that_run(void) {
    const uint32_t count = wpe_project_template_count();
    WPE_TEST_ASSERT(count >= 2);
    WPE_TEST_ASSERT(wpe_project_template_name(count) == NULL);
    bool has_default = false;
    for (uint32_t i = 0; i < count; ++i) {
        const char* name = wpe_project_template_name(i);
        has_default = has_default || strcmp(name, WPE_PROJECT_DEFAULT_TEMPLATE) == 0;
        char dir[256];
        create_project(name, dir, sizeof dir);

        char source[512];
        snprintf(source, sizeof source, "%s/%s", WPE_TEST_TEMPLATES_DIR, name);
        Comparison comparison = {.project = dir, .source = source};
        WPE_TEST_ASSERT_OK(wpe_directory_walk(dir, compare_file, &comparison));
        uint32_t source_files = 0;
        WPE_TEST_ASSERT_OK(wpe_directory_walk(source, count_file, &source_files));
        WPE_TEST_ASSERT_EQ_UINT(source_files, comparison.files);
        WPE_TEST_ASSERT_EQ_INT(WPE_SKILL_CURRENT, wpe_project_skill_status(dir)); /* And the Claude skill. */

        WPE_Engine* engine = create_engine(dir);
        WPE_TEST_ASSERT_EQ_STR("Template Test", wpe_engine_project(engine)->name);
        frames(engine, 600);
        WPE_TEST_ASSERT_EQ_UINT(0, script_errors(engine));
        const WPE_Framebuffer* fb = wpe_renderer_framebuffer(wpe_engine_renderer(engine));
        uint32_t differing = 0;
        for (uint32_t p = 0; p < fb->width * fb->height; ++p)
            differing += fb->color[p] != fb->color[0];
        WPE_TEST_ASSERT_MSG(differing > fb->width * fb->height / 4, "template '%s' draws a flat image", name);
        wpe_engine_destroy(engine);
        remove_project(dir);
    }
    WPE_TEST_ASSERT(has_default);
}

static void test_templates_reject_unknown_names(void) {
    wpe_log_set_stderr_enabled(false);
    const WPE_Result result = wpe_project_create(ROOT "/unknown", "X", "no_such_template");
    wpe_log_set_stderr_enabled(true);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND, result);
    WPE_TEST_ASSERT(!wpe_directory_exists(ROOT "/unknown"));
}

/* ---- First person ---------------------------------------------------------------------------------- */

/* Walking forward climbs the stairs onto the platform; Space jumps; E pushes the crate in sight. */
static void test_templates_first_person_plays(void) {
    char dir[256];
    create_project("first_person", dir, sizeof dir);
    WPE_Engine* engine = create_engine(dir);
    WPE_Ecs* ecs = wpe_engine_ecs(engine);
    const WPE_Entity player = find(ecs, "player");
    WPE_TEST_ASSERT(player != WPE_ENTITY_NULL);
    WPE_TEST_ASSERT_EQ_STR("head", wpe_scene_get_name(ecs, wpe_engine_camera(engine)));
    frames(engine, 15); /* Settle onto the ground. */
    WPE_TEST_ASSERT_NEAR(0.0, position(ecs, player).y, 0.05);

    key(engine, SDL_SCANCODE_W, true);
    frames(engine, 90);
    key(engine, SDL_SCANCODE_W, false);
    frames(engine, 15);
    const WPE_Vec3 on_platform = position(ecs, player);
    WPE_TEST_ASSERT_MSG(on_platform.y > 1.55f && on_platform.z < -1.2f, "the player ended at (%g, %g, %g)",
                        (double)on_platform.x, (double)on_platform.y, (double)on_platform.z);

    tap(engine, SDL_SCANCODE_SPACE);
    frames(engine, 4);
    WPE_TEST_ASSERT(position(ecs, player).y > on_platform.y + 0.3f);
    frames(engine, 30);
    WPE_TEST_ASSERT_NEAR(on_platform.y, position(ecs, player).y, 0.05);

    /* Stand on the platform before the lone crate at (2.5, 2.1, -6), looking slightly down at it, and push
     * it. */
    WPE_Entity crate = WPE_ENTITY_NULL;
    WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_PHYSICS_RIGIDBODY)});
    while (wpe_ecs_iter_next(&it)) {
        const WPE_Vec3 p = position(ecs, it.entity);
        if (fabsf(p.x - 2.5f) < 0.2f && fabsf(p.z + 6.0f) < 0.2f) crate = it.entity;
    }
    WPE_TEST_ASSERT(crate != WPE_ENTITY_NULL);
    WPE_TEST_ASSERT_OK(wpe_script_host_run(wpe_engine_scripts(engine), "aim",
                                           "local p = wpe.find('player')\n"
                                           "wpe.set_position(p, vector.create(2.5, 1.6, -3.4))\n"
                                           "local s = wpe.get_script(p)\n"
                                           "s.yaw = 0; s.pitch = -0.4\n"));
    frames(engine, 10);
    const float crate_z = position(ecs, crate).z;
    tap(engine, SDL_SCANCODE_E);
    frames(engine, 20);
    WPE_TEST_ASSERT_MSG(position(ecs, crate).z < crate_z - 0.5f, "the crate moved from z %g to %g",
                        (double)crate_z, (double)position(ecs, crate).z);
    WPE_TEST_ASSERT_EQ_UINT(0, script_errors(engine));
    wpe_engine_destroy(engine);
    remove_project(dir);
}

/* ---- Third person ---------------------------------------------------------------------------------- */

/* Runs Luau with the game's API; its asserts fail the test. */
static void check_lua(WPE_Engine* engine, const char* name, const char* code) {
    WPE_TEST_ASSERT_MSG(wpe_script_host_run(wpe_engine_scripts(engine), name, code) == WPE_RESULT_OK,
                        "the check '%s' failed (see the log)", name);
}

/* The figure walks away from the camera and turns to face its way, animated while moving and standing
 * still otherwise; Space jumps; under the archway's roof the camera moves in rather than through it. */
static void test_templates_third_person_plays(void) {
    char dir[256];
    create_project("third_person", dir, sizeof dir);
    WPE_Engine* engine = create_engine(dir);
    WPE_Ecs* ecs = wpe_engine_ecs(engine);
    const WPE_Entity player = find(ecs, "player");
    const WPE_Entity camera = find(ecs, "camera");
    WPE_TEST_ASSERT(player != WPE_ENTITY_NULL && camera == wpe_engine_camera(engine));
    frames(engine, 15);
    check_lua(engine, "idle at the start",
              "local s = wpe.get_script(wpe.find('player'))\n"
              "assert(s.figure and s.state == 'idle')\n"
              "assert(wpe.animation_speed(s.figure) == 0)\n");
    const WPE_Vec3 start = position(ecs, player);

    key(engine, SDL_SCANCODE_W, true);
    frames(engine, 45);
    const WPE_Vec3 walked = position(ecs, player);
    WPE_TEST_ASSERT_MSG(walked.z < start.z - 3.0f && fabsf(walked.x - start.x) < 0.1f,
                        "the player walked to (%g, %g, %g)", (double)walked.x, (double)walked.y,
                        (double)walked.z);
    WPE_TEST_ASSERT(position(ecs, camera).z > walked.z + 2.0f); /* Still behind. */
    check_lua(engine, "walking",
              "local s = wpe.get_script(wpe.find('player'))\n"
              "assert(s.state == 'move' and wpe.get_animation(s.figure).playing)\n"
              "assert(wpe.animation_speed(s.figure) > 0.5)\n");
    key(engine, SDL_SCANCODE_W, false);
    frames(engine, 20);
    check_lua(engine, "standing again",
              "local s = wpe.get_script(wpe.find('player'))\n"
              "assert(s.state == 'idle' and wpe.animation_speed(s.figure) == 0)\n");

    /* D walks right (+X, with the camera behind), and the figure turns to face that way. */
    key(engine, SDL_SCANCODE_D, true);
    frames(engine, 30);
    key(engine, SDL_SCANCODE_D, false);
    WPE_TEST_ASSERT(position(ecs, player).x > walked.x + 1.5f);
    check_lua(engine, "facing right",
              "assert(math.abs(wpe.get_rotation(wpe.find('player')).y + math.pi / 2) < 0.1)\n");

    frames(engine, 10);
    const float ground = position(ecs, player).y;
    tap(engine, SDL_SCANCODE_SPACE);
    frames(engine, 4);
    WPE_TEST_ASSERT(position(ecs, player).y > ground + 0.3f);
    frames(engine, 30);

    /* Under the archway (roof from y = 3), a camera looking steeply down would be above the roof: it stays
     * under it, close to the player. */
    check_lua(engine, "into the archway",
              "wpe.set_position(wpe.find('player'), vector.create(-10, 0, 6))\n"
              "wpe.get_script(wpe.find('camera')).pitch = -0.8\n");
    frames(engine, 10);
    const WPE_Vec3 eye = position(ecs, camera);
    const WPE_Vec3 focus = wpe_vec3_add(position(ecs, player), wpe_vec3(0.0f, 1.5f, 0.0f));
    WPE_TEST_ASSERT_MSG(eye.y < 3.0f && wpe_vec3_length(wpe_vec3_sub(eye, focus)) < 2.5f,
                        "the camera is at (%g, %g, %g)", (double)eye.x, (double)eye.y, (double)eye.z);
    /* The villager wanders by itself (a nav agent), near its home and on the ground. */
    const WPE_Entity villager = find(ecs, "villager");
    WPE_TEST_ASSERT(villager != WPE_ENTITY_NULL);
    const WPE_Vec3 home = wpe_vec3(12, 0, 4);
    float farthest = 0.0f;
    for (int i = 0; i < 20; ++i) {
        frames(engine, 15);
        const WPE_Vec3 at = position(ecs, villager);
        farthest = fmaxf(farthest, wpe_vec3_length(wpe_vec3_sub(at, home)));
        WPE_TEST_ASSERT_MSG(fabsf(at.y) < 0.1f && wpe_vec3_length(wpe_vec3_sub(at, home)) < 7.5f,
                            "the villager wandered to (%g, %g, %g)", (double)at.x, (double)at.y,
                            (double)at.z);
    }
    WPE_TEST_ASSERT_MSG(farthest > 1.0f, "the villager stayed at home");
    WPE_TEST_ASSERT_EQ_UINT(0, script_errors(engine));
    wpe_engine_destroy(engine);
    remove_project(dir);
}

/* ---- Shooter --------------------------------------------------------------------------------------- */

/* Shots knock a dummy down (it gets up again), use up the magazine and reload it; R reloads; the gun pulls
 * back from a wall in front of it. */
static void test_templates_shooter_plays(void) {
    char dir[256];
    create_project("shooter", dir, sizeof dir);
    WPE_Engine* engine = create_engine(dir);
    frames(engine, 15);
    check_lua(engine, "loaded", "local g = wpe.get_script(wpe.find('gun'))\nassert(g.ammo == g.magazine)\n");
    /* Stand 5 m in front of a dummy (keeping it still), looking at its chest. */
    check_lua(engine, "aim",
              "local d = wpe.find('dummy')\n"
              "local s = wpe.get_script(d)\n"
              "s.resting = 1000\n"
              "wpe.nav_stop(d)\n"
              "local p = wpe.find('player')\n"
              "wpe.set_position(p, wpe.get_position(d) + vector.create(0, 0, 5))\n"
              "local ps = wpe.get_script(p)\n"
              "ps.yaw = 0; ps.pitch = -0.17\n");
    frames(engine, 10);
    for (int shot = 0; shot < 3; ++shot) {
        check_lua(engine, "fire", "assert(wpe.get_script(wpe.find('gun')):fire())\n");
        frames(engine, 6); /* Past the cooldown. */
    }
    check_lua(engine, "knocked down",
              "local g = wpe.get_script(wpe.find('gun'))\n"
              "local d = wpe.get_script(wpe.find('dummy'))\n"
              "assert(d.down > 0, 'the dummy should be down')\n"
              "assert(g.hits == 1 and g.ammo == g.magazine - 3)\n");
    frames(engine, 150); /* down_time (4 s) passes. */
    check_lua(engine, "back up",
              "local d = wpe.get_script(wpe.find('dummy'))\n"
              "assert(d.down <= 0 and d.hp == d.health)\n");

    /* An empty magazine reloads by itself; R reloads a partly used one. */
    check_lua(engine, "empty",
              "local g = wpe.get_script(wpe.find('gun'))\n"
              "g.ammo = 0\n"
              "assert(not g:fire() and g.reloading > 0)\n");
    frames(engine, 40);
    check_lua(engine, "reloaded",
              "local g = wpe.get_script(wpe.find('gun'))\nassert(g.ammo == g.magazine)\n");
    check_lua(engine, "one shot", "assert(wpe.get_script(wpe.find('gun')):fire())\n");
    frames(engine, 6);
    tap(engine, SDL_SCANCODE_R);
    check_lua(engine, "reloading", "assert(wpe.get_script(wpe.find('gun')).reloading > 0)\n");
    frames(engine, 40);

    /* Facing a wall up close, the gun pulls back toward the camera. */
    check_lua(engine, "to the wall",
              "local p = wpe.find('player')\n"
              "wpe.set_position(p, vector.create(0, 0, -19.4))\n"
              "local ps = wpe.get_script(p)\n"
              "ps.yaw = 0; ps.pitch = 0\n");
    frames(engine, 10);
    check_lua(engine, "pulled back",
              "local g = wpe.get_script(wpe.find('gun'))\n"
              "assert(wpe.get_position(wpe.find('gun')).z > g.rest.z + 0.1, 'the gun should pull back')\n");
    WPE_TEST_ASSERT_EQ_UINT(0, script_errors(engine));
    wpe_engine_destroy(engine);
    remove_project(dir);
}

/* ---- Stealth --------------------------------------------------------------------------------------- */

/* A guard's state (guard.luau), checked from Luau. */
static void expect_guard(WPE_Engine* engine, const char* guard, const char* states) {
    char code[512];
    snprintf(code, sizeof code,
             "local g = wpe.get_script(wpe.find('%s'))\n"
             "assert(string.find('%s', g.state, 1, true), '%s is ' .. g.state .. ', not %s')\n",
             guard, states, guard, states);
    check_lua(engine, guard, code);
}

/* Puts the player `distance` m in front of a guard (in plain sight). */
static void show_player_to(WPE_Engine* engine, const char* guard, float distance) {
    char code[256];
    snprintf(code, sizeof code,
             "local g = wpe.find('%s')\n"
             "wpe.set_position(wpe.find('player'), wpe.get_world_position(g) + wpe.forward(g) * %g)\n",
             guard, (double)distance);
    check_lua(engine, "show the player", code);
}

/* Guards patrol, notice the player in their cone, hear noise, chase, lose track and search, go back to
 * their routes, and catch; reaching the exit wins. */
static void test_templates_stealth_plays(void) {
    char dir[256];
    create_project("stealth", dir, sizeof dir);
    WPE_Engine* engine = create_engine(dir);
    WPE_Ecs* ecs = wpe_engine_ecs(engine);
    const WPE_Entity guard = find(ecs, "guard 1");
    WPE_TEST_ASSERT(guard != WPE_ENTITY_NULL);
    const WPE_Vec3 start = position(ecs, guard);
    frames(engine, 150); /* Waits at its first point, then walks the lane. */
    WPE_TEST_ASSERT_MSG(wpe_vec3_length(wpe_vec3_sub(position(ecs, guard), start)) > 1.0f,
                        "guard 1 didn't patrol");
    expect_guard(engine, "guard 1", "patrol");

    /* A noise behind guard 2 (it can't see there): it goes to look, then back to its route. */
    check_lua(engine, "noise behind guard 2",
              "local g = wpe.find('guard 2')\n"
              "local p = wpe.find('player')\n"
              "wpe.set_position(p, wpe.get_world_position(g) - wpe.forward(g) * 3)\n"
              "wpe.get_script(p):make_noise(9)\n");
    frames(engine, 1);
    expect_guard(engine, "guard 2", "investigate look");
    check_lua(engine, "away", "wpe.set_position(wpe.find('player'), vector.create(0, 0, 17))\n");
    frames(engine, 450);
    expect_guard(engine, "guard 2", "return patrol");

    /* In plain sight of guard 3: suspicious, then a chase; out of sight it searches, then gives up. */
    show_player_to(engine, "guard 3", 4.0f);
    frames(engine, 6);
    expect_guard(engine, "guard 3", "suspicious chase");
    frames(engine, 30);
    expect_guard(engine, "guard 3", "chase");
    check_lua(engine, "hide", "wpe.set_position(wpe.find('player'), vector.create(0, 0, 17))\n");
    frames(engine, 120);
    expect_guard(engine, "guard 3", "search");
    frames(engine, 450);
    expect_guard(engine, "guard 3", "return patrol");

    /* Caught by guard 1: the game ends (the player frozen), then the level starts over. */
    show_player_to(engine, "guard 1", 3.0f);
    frames(engine, 90);
    check_lua(engine, "caught",
              "assert(wpe.get_script(wpe.find('game')).result == 'caught')\n"
              "assert(wpe.get_script(wpe.find('player')).frozen)\n"
              "assert(wpe.get_script(wpe.find('guard 1')).done)\n");
    frames(engine, 100);
    check_lua(engine, "started over", "assert(wpe.get_script(wpe.find('game')).result == nil)\n");

    /* Into the exit: escaped. */
    frames(engine, 5);
    check_lua(engine, "to the exit", "wpe.set_position(wpe.find('player'), vector.create(0, 0, -18))\n");
    frames(engine, 10);
    check_lua(engine, "escaped", "assert(wpe.get_script(wpe.find('game')).result == 'escaped')\n");
    WPE_TEST_ASSERT_EQ_UINT(0, script_errors(engine));
    wpe_engine_destroy(engine);
    remove_project(dir);
}

/* ---- Platformer ------------------------------------------------------------------------------------ */

/* The highest the player gets within `count` frames. */
static float highest(WPE_Engine* engine, WPE_Entity player, uint32_t count) {
    float top = -1e9f;
    for (uint32_t i = 0; i < count; ++i) {
        frames(engine, 1);
        top = fmaxf(top, position(wpe_engine_ecs(engine), player).y);
    }
    return top;
}

/* Running right collects coins; a held jump goes higher than a tapped one; spikes and pits send the player
 * back to the last checkpoint; the moving platform carries it; the goal finishes the level and Jump starts
 * it over. */
static void test_templates_platformer_plays(void) {
    char dir[256];
    create_project("platformer", dir, sizeof dir);
    WPE_Engine* engine = create_engine(dir);
    WPE_Ecs* ecs = wpe_engine_ecs(engine);
    WPE_Entity player = find(ecs, "player");
    WPE_TEST_ASSERT(player != WPE_ENTITY_NULL);
    frames(engine, 15);
    check_lua(engine, "fifteen coins", "assert(wpe.get_script(wpe.find('player')).total_coins == 15)\n");

    /* The first three coins hang just above the run. */
    key(engine, SDL_SCANCODE_D, true);
    frames(engine, 40);
    key(engine, SDL_SCANCODE_D, false);
    frames(engine, 15);
    WPE_TEST_ASSERT(position(ecs, player).x > 4.5f && fabsf(position(ecs, player).z) < 1e-3f);
    check_lua(engine, "three coins", "assert(wpe.get_script(wpe.find('player')).coins == 3)\n");

    /* Holding jump goes higher than tapping it. */
    check_lua(engine, "back to the start", "wpe.set_position(wpe.find('player'), vector.create(1, 0, 0))\n");
    frames(engine, 10);
    check_lua(engine, "standing", "assert(wpe.get_script(wpe.find('player')).machine.state == 'idle')\n");
    tap(engine, SDL_SCANCODE_SPACE);
    check_lua(engine, "jumping", "assert(wpe.get_script(wpe.find('player')).machine.state == 'jump')\n");
    const float tapped = highest(engine, player, 30);
    check_lua(engine, "landed",
              "local m = wpe.get_script(wpe.find('player')).machine\n"
              "assert(m.state == 'idle' and m.previous == 'fall', m.state)\n");
    key(engine, SDL_SCANCODE_SPACE, true);
    const float held = highest(engine, player, 30);
    key(engine, SDL_SCANCODE_SPACE, false);
    frames(engine, 10);
    WPE_TEST_ASSERT_MSG(tapped > 0.2f && held > 1.3f && held > tapped + 0.5f,
                        "a tapped jump reached %g, a held one %g", (double)tapped, (double)held);

    /* Spikes: back to the start (the first checkpoint). */
    check_lua(engine, "onto the spikes",
              "wpe.set_position(wpe.find('player'), vector.create(20.7, 1.5, 0))\n");
    for (int i = 0; i < 30; ++i) { /* Watched every frame: it falls onto them, then stands still a moment. */
        frames(engine, 1);
        check_lua(engine, "watch",
                  "local s = wpe.get_script(wpe.find('player'))\n"
                  "s.seen_respawn = s.seen_respawn or s.machine.state == 'respawn'\n");
    }
    check_lua(engine, "respawned",
              "local s = wpe.get_script(wpe.find('player'))\n"
              "assert(s.seen_respawn and s.machine.state ~= 'respawn', s.machine.state)\n");
    WPE_TEST_ASSERT_MSG(position(ecs, player).x < 1.5f, "the spikes left the player at x %g",
                        (double)position(ecs, player).x);

    /* The moving platform carries the player standing on it. */
    check_lua(engine, "onto the moving platform",
              "local p = wpe.get_world_position(wpe.find('moving platform'))\n"
              "wpe.set_position(wpe.find('player'), p + vector.create(0, 0.3, 0))\n");
    frames(engine, 3);
    const float boarded = position(ecs, player).x;
    frames(engine, 20);
    const WPE_Vec3 carried = position(ecs, player);
    WPE_TEST_ASSERT_MSG(fabsf(carried.x - boarded) > 0.5f && carried.y > -0.1f && carried.y < 0.3f,
                        "on the platform the player went from x %g to (%g, %g)", (double)boarded,
                        (double)carried.x, (double)carried.y);

    /* A checkpoint: falling into a pit after it comes back there. */
    check_lua(engine, "through the checkpoint",
              "wpe.set_position(wpe.find('player'), vector.create(35.5, 0.1, 0))\n");
    frames(engine, 5);
    check_lua(engine, "into the pit", "wpe.set_position(wpe.find('player'), vector.create(30, -12, 0))\n");
    frames(engine, 5);
    WPE_TEST_ASSERT_NEAR(35.5, position(ecs, player).x, 0.3);

    /* The goal finishes the level; Jump starts it over with no coins. */
    check_lua(engine, "to the goal", "wpe.set_position(wpe.find('player'), vector.create(56, 2.5, 0))\n");
    frames(engine, 5);
    check_lua(engine, "finished",
              "local s = wpe.get_script(wpe.find('player'))\n"
              "assert(s.finished and s.machine.state == 'finished')\n");
    tap(engine, SDL_SCANCODE_SPACE);
    frames(engine, 5);
    player = find(ecs, "player");
    WPE_TEST_ASSERT(position(ecs, player).x < 1.0f);
    check_lua(engine, "started over",
              "local s = wpe.get_script(wpe.find('player'))\n"
              "assert(not s.finished and s.coins == 0 and s.total_coins == 15)\n");

    /* The menu pauses the game; on its controls screen Jump moves to K (alone: Space, W and Up go), then Move
     * left takes K and Jump gets Move left's A in exchange. The choice is saved for the player. */
    (void)wpe_path_remove(ROOT "/saves/" WPE_BINDINGS_FILE);
    check_lua(engine, "keyboard prompts", "assert(wpe.action_prompt('jump') == 'Space')\n");
    tap(engine, SDL_SCANCODE_ESCAPE);
    check_lua(engine, "paused",
              "assert(wpe.get_time_scale() == 0 and wpe.get_script(wpe.find('menu')).page == 'main')\n");
    const float paused_at = position(ecs, find(ecs, "player")).x;
    tap(engine, SDL_SCANCODE_DOWN);
    tap(engine, SDL_SCANCODE_RETURN);
    check_lua(engine, "controls", "assert(wpe.get_script(wpe.find('menu')).page == 'controls')\n");
    tap(engine, SDL_SCANCODE_DOWN);
    tap(engine, SDL_SCANCODE_DOWN);
    tap(engine, SDL_SCANCODE_RETURN); /* Jump, keyboard: waits for a key. */
    check_lua(engine, "waiting", "assert(wpe.is_listening())\n");
    tap(engine, SDL_SCANCODE_K);
    check_lua(engine, "jump on k",
              "local keys = {}\n"
              "for _, b in wpe.get_bindings('jump') do if b.key then table.insert(keys, b.key) end end\n"
              "assert(#keys == 1 and keys[1] == 'k', table.concat(keys, ','))\n"
              "assert(wpe.get_bindings('jump')[#wpe.get_bindings('jump')].pad_button == 'south')\n");
    tap(engine, SDL_SCANCODE_UP);
    tap(engine, SDL_SCANCODE_UP);
    tap(engine, SDL_SCANCODE_RETURN); /* Move left, keyboard. */
    tap(engine, SDL_SCANCODE_K);
    check_lua(engine, "swapped",
              "local move = wpe.get_bindings('move_x')\n"
              "assert(move[2].key == 'k' and move[2].scale == -1)\n" /* In place of A. */
              "assert(wpe.get_bindings('jump')[1].key == 'a')\n"
              "assert(wpe.action_prompt('jump') == 'A' and wpe.action_prompt('move_x', nil, -1) == 'K')\n");
    /* An Escape while waiting cancels: nothing changes. */
    tap(engine, SDL_SCANCODE_RETURN);
    tap(engine, SDL_SCANCODE_ESCAPE);
    check_lua(engine, "cancelled",
              "assert(not wpe.is_listening() and wpe.get_script(wpe.find('menu')).page == 'controls')\n"
              "assert(wpe.get_bindings('move_x')[2].key == 'k')\n");
    WPE_TEST_ASSERT_NEAR(paused_at, position(ecs, find(ecs, "player")).x, 0.0); /* Nothing moved meanwhile. */
    tap(engine, SDL_SCANCODE_ESCAPE);
    tap(engine, SDL_SCANCODE_ESCAPE);
    check_lua(engine, "resumed", "assert(wpe.get_time_scale() == 1)\n");
    void* saved = NULL;
    size_t saved_size = 0;
    WPE_TEST_ASSERT_OK(wpe_file_read_all(ROOT "/saves/" WPE_BINDINGS_FILE, &saved, &saved_size));
    WPE_TEST_ASSERT(strstr(saved, "\"k\"") && strstr(saved, "\"jump\"") && strstr(saved, "\"move_x\""));
    wpe_free(saved);
    check_lua(engine, "defaults again",
              "wpe.reset_bindings()\nassert(wpe.action_prompt('jump') == 'Space')\n");
    WPE_TEST_ASSERT_EQ_UINT(0, script_errors(engine));
    wpe_engine_destroy(engine);
    remove_project(dir);
}

/* ---- Dialogue -------------------------------------------------------------------------------------- */

/* Walks the player up to a villager (a meter and a half in front of them) and lets them notice. */
static void approach(WPE_Engine* engine, const char* villager, float dx, float dz) {
    char code[256];
    snprintf(code, sizeof code,
             "local p = wpe.get_position(wpe.find('%s'))\n"
             "wpe.set_position(wpe.find('player'), vector.create(p.x + %g, 0, p.z + %g))\n",
             villager, (double)dx, (double)dz);
    check_lua(engine, "approach", code);
    frames(engine, 4);
    snprintf(code, sizeof code, "assert(wpe.get_script(wpe.find('%s')).machine.state == 'noticed')\n",
             villager);
    check_lua(engine, "noticed", code);
}

/* Expects the dialogue box to show `speaker` with text containing `words`. */
static void expect_line(WPE_Engine* engine, const char* speaker, const char* words) {
    char code[512];
    snprintf(code, sizeof code,
             "local talk = wpe.get_script(wpe.find('dialogue box')).talk\n"
             "assert(talk and not talk.done, 'no conversation')\n"
             "assert(talk:speaker() == [[%s]], talk:speaker())\n"
             "assert(string.find(talk:text(), [[%s]], 1, true), talk:text())\n",
             speaker, words);
    check_lua(engine, words, code);
}

static void expect_goal(WPE_Engine* engine, const char* goal) {
    char code[256];
    if (goal)
        snprintf(code, sizeof code, "assert(wpe.get_script(wpe.find('quest')):goal() == [[%s]])\n", goal);
    else
        snprintf(code, sizeof code, "assert(wpe.get_script(wpe.find('quest')):goal() == nil)\n");
    check_lua(engine, "goal", code);
}

/* The whole quest with the keys: Maren asks for her ring, Pip has it, Maren gets it back, and Tobin's
 * lines follow along. Pages type out (Return shows one whole, Return again goes on), choices take
 * Down/Return, the music ducks while someone talks, and the player stands still meanwhile. */
static void test_templates_dialogue_plays(void) {
    char dir[256];
    create_project("dialogue", dir, sizeof dir);
    WPE_Engine* engine = create_engine(dir);
    WPE_Ecs* ecs = wpe_engine_ecs(engine);
    frames(engine, 10);
    expect_goal(engine, "Talk to the villagers");

    /* The state machine module on its own: enter/exit order, events, shared states, push/pop, loops. */
    check_lua(
        engine, "states",
        "local States = require('scripts/lib/states')\n"
        "local log = {}\n"
        "local owner = {}\n"
        "local ground = { jump = function(self) return 'air' end }\n"
        "local states = {\n"
        "  idle = States.extend(ground, { enter = function(self, from, why) table.insert(log, 'enter idle '"
        " .. tostring(why)) end, exit = function(self, to) table.insert(log, 'exit idle to ' .. to) end }),\n"
        "  run = States.extend(ground, { update = function(self, dt) if dt > 1 then return 'idle' end end "
        "}),\n"
        "  air = { enter = function(self) table.insert(log, 'enter air') end, land = function(self) return "
        "'idle' end },\n"
        "  hurt = { enter = function(self) table.insert(log, 'hurt') end },\n"
        "  ping = { enter = function(self) return 'pong' end }, pong = { enter = function(self) return "
        "'ping' end },\n"
        "}\n"
        "local m = States.new(owner, states, 'idle', 'start')\n"
        "assert(log[1] == 'enter idle start' and m.state == 'idle' and m.previous == nil)\n"
        "assert(m:send('jump') and m.state == 'air' and m.previous == 'idle')\n" /* Inherited. */
        "assert(log[2] == 'exit idle to air' and log[3] == 'enter air')\n"
        "assert(not m:send('jump') and m.state == 'air')\n" /* Air has no jump: ignored. */
        "m:send('land'); m:go('run'); m:update(0.5); assert(m:is('run') and m.time == 0.5)\n"
        "m:update(2); assert(m:is('idle'))\n"
        "m:push('hurt'); assert(m:is('hurt')); m:pop(); assert(m:is('idle'))\n"
        "assert(not pcall(m.go, m, 'ping'))\n" /* A loop is stopped. */
        "assert(not pcall(m.go, m, 'nowhere'))\n");

    /* Maren: E starts the talk; it types with voice blips, which duck the music. */
    approach(engine, "Maren", 0.0f, 1.5f);
    tap(engine, SDL_SCANCODE_E);
    frames(engine, 3);
    expect_line(engine, "Maren", "new face in Ashford");
    check_lua(engine, "ducked", "assert(wpe.get_ducking().active, 'the voice should duck the music')\n");
    /* No walking while talking. */
    const WPE_Vec3 before = position(ecs, find(ecs, "player"));
    key(engine, SDL_SCANCODE_W, true);
    frames(engine, 10);
    key(engine, SDL_SCANCODE_W, false);
    WPE_TEST_ASSERT_NEAR(before.z, position(ecs, find(ecs, "player")).z, 1e-3);
    tap(engine, SDL_SCANCODE_RETURN); /* Shows page one whole... */
    tap(engine, SDL_SCANCODE_RETURN); /* ...then page two, */
    tap(engine, SDL_SCANCODE_RETURN); /* shown whole: its choices. */
    expect_line(engine, "Maren", "lost something dear");
    tap(engine, SDL_SCANCODE_RETURN); /* "What did you lose?" */
    expect_line(engine, "Maren", "silver ring");
    tap(engine, SDL_SCANCODE_RETURN);
    tap(engine, SDL_SCANCODE_RETURN);
    tap(engine, SDL_SCANCODE_RETURN);
    tap(engine, SDL_SCANCODE_RETURN); /* "I'll find it." */
    expect_line(engine, "Maren", "children play");
    expect_goal(engine, "Find Maren's silver ring");
    tap(engine, SDL_SCANCODE_RETURN);
    tap(engine, SDL_SCANCODE_RETURN); /* The end. */
    check_lua(engine, "ended", "assert(wpe.get_script(wpe.find('dialogue box')).talk == nil)\n");
    frames(engine, 60);
    check_lua(engine, "music back", "assert(not wpe.get_ducking().active)\n");

    /* Tobin hints at Pip. */
    approach(engine, "Tobin", 0.0f, 2.2f);
    tap(engine, SDL_SCANCODE_E);
    expect_line(engine, "Tobin", "Pip collects shiny things");
    tap(engine, SDL_SCANCODE_RETURN);
    tap(engine, SDL_SCANCODE_RETURN); /* "Thanks, Tobin." */
    frames(engine, 30);

    /* Pip (wandering) stops, and with the quest known gives up the ring. */
    approach(engine, "Pip", 1.2f, 0.0f);
    check_lua(engine, "Pip stopped", "assert(wpe.nav_state(wpe.find('Pip')) ~= 'moving')\n");
    tap(engine, SDL_SCANCODE_E);
    expect_line(engine, "Pip", "shiny rock");
    tap(engine, SDL_SCANCODE_RETURN);
    tap(engine, SDL_SCANCODE_RETURN); /* "That's Maren's ring." */
    expect_line(engine, "Pip", "didn't know it was hers");
    for (int i = 0; i < 4; ++i)
        tap(engine, SDL_SCANCODE_RETURN);
    expect_goal(engine, "Bring the ring back to Maren");
    frames(engine, 30);

    /* Back to Maren: Down to "Not just yet." and Up again to "Here you are.". */
    approach(engine, "Maren", 0.0f, 1.5f);
    tap(engine, SDL_SCANCODE_E);
    expect_line(engine, "Maren", "You found it");
    tap(engine, SDL_SCANCODE_RETURN);
    tap(engine, SDL_SCANCODE_DOWN);
    tap(engine, SDL_SCANCODE_UP);
    tap(engine, SDL_SCANCODE_RETURN);
    expect_line(engine, "Maren", "Pip had it");
    expect_goal(engine, NULL);
    for (int i = 0; i < 4; ++i)
        tap(engine, SDL_SCANCODE_RETURN);
    frames(engine, 30);

    /* Tobin has heard. */
    approach(engine, "Tobin", 0.0f, 2.2f);
    tap(engine, SDL_SCANCODE_E);
    expect_line(engine, "Tobin", "Ashford owes you one");
    WPE_TEST_ASSERT_EQ_UINT(0, script_errors(engine));
    wpe_engine_destroy(engine);
    remove_project(dir);
}

/* ---- Racing ---------------------------------------------------------------------------------------- */

/* Puts the car somewhere (stopped) and lets the physics and triggers see it. */
static void place_car(WPE_Engine* engine, float x, float z) {
    char code[256];
    snprintf(code, sizeof code,
             "local car = wpe.find('car')\n"
             "wpe.set_position(car, vector.create(%g, 0.1, %g))\n"
             "wpe.set_velocity(car, vector.create(0, 0, 0))\n",
             (double)x, (double)z);
    check_lua(engine, "place the car", code);
    frames(engine, 3);
}

/* The countdown holds the car; then it drives, steers and stays between the walls. Laps count only
 * through every checkpoint in order; three laps finish the race and the best lap is saved. */
static void test_templates_racing_plays(void) {
    char dir[256];
    create_project("racing", dir, sizeof dir);
    WPE_TEST_ASSERT_OK(wpe_path_remove(ROOT "/saves/best_lap.json")); /* A best lap from an earlier run. */
    WPE_Engine* engine = create_engine(dir);
    WPE_Ecs* ecs = wpe_engine_ecs(engine);
    const WPE_Entity car = find(ecs, "car");
    WPE_TEST_ASSERT(car != WPE_ENTITY_NULL);
    frames(engine, 2);
    check_lua(engine, "no best lap yet", "assert(wpe.get_script(wpe.find('race')).best == nil)\n");

    /* Throttle during the countdown: held. */
    const WPE_Vec3 start = position(ecs, car);
    key(engine, SDL_SCANCODE_W, true);
    frames(engine, 60);
    WPE_TEST_ASSERT(fabsf(position(ecs, car).x - start.x) < 0.05f);
    check_lua(engine, "counting down", "assert(wpe.get_script(wpe.find('race')).state == 'countdown')\n");

    /* GO (at 3 s): it accelerates along the straight (toward +X). */
    frames(engine, 90);
    check_lua(engine, "racing", "assert(wpe.get_script(wpe.find('race')).state == 'racing')\n");
    WPE_TEST_ASSERT(position(ecs, car).x > start.x + 10.0f);
    check_lua(engine, "fast", "assert(wpe.get_script(wpe.find('car')).speed > 15)\n");

    /* Steering right turns it toward +Z, into the inner wall, which it doesn't pass (its face at z = -29). */
    const float before_turn = position(ecs, car).z;
    key(engine, SDL_SCANCODE_D, true);
    frames(engine, 20);
    WPE_TEST_ASSERT(position(ecs, car).z > before_turn + 0.5f);
    frames(engine, 40);
    key(engine, SDL_SCANCODE_D, false);
    key(engine, SDL_SCANCODE_W, false);
    WPE_TEST_ASSERT_MSG(position(ecs, car).z < -29.0f, "the car went through the wall to z %g",
                        (double)position(ecs, car).z);

    /* Laps: the finish line alone doesn't count; the checkpoints in order then the line do. */
    place_car(engine, 0.0f, -35.0f);
    check_lua(engine, "no lap from the line alone", "assert(wpe.get_script(wpe.find('race')).lap == 1)\n");
    static const float route[][2] = {{60.0f, 0.0f}, {0.0f, 35.0f}, {-60.0f, 0.0f}, {0.0f, -35.0f}};
    for (int lap = 1; lap <= 3; ++lap) {
        for (size_t i = 0; i < sizeof route / sizeof route[0]; ++i) {
            frames(engine, 15); /* Some lap time. */
            place_car(engine, route[i][0], route[i][1] + 3.0f);
            place_car(engine, route[i][0], route[i][1]);
        }
    }
    check_lua(engine, "finished",
              "local race = wpe.get_script(wpe.find('race'))\n"
              "assert(race.state == 'finished' and race.lap == 3)\n"
              "assert(race.best and race.best > 0 and race.best <= race.last_lap)\n"
              "assert(wpe.load_data('best_lap') == race.best)\n"
              "assert(not wpe.get_script(wpe.find('car')).controls)\n");
    WPE_TEST_ASSERT_EQ_UINT(0, script_errors(engine));
    wpe_engine_destroy(engine);
    remove_project(dir);
}

/* ---- Racing online --------------------------------------------------------------------------------- */

/* Frames of every engine, with the hub's clock moving in step. */
static void play(WPE_NetHub* hub, WPE_Engine** engines, int count, uint32_t frame_count) {
    for (uint32_t f = 0; f < frame_count; ++f) {
        wpe_network_hub_advance(hub, 33);
        for (int i = 0; i < count; ++i)
            if (engines[i]) WPE_TEST_ASSERT_OK(wpe_engine_frame(engines[i], 1.0 / 30.0));
    }
}

/* An engine on the project whose games go through `hub`, saving in its own directory (two players on
 * one machine keep their own best laps). */
static WPE_Engine* create_net_engine(const char* project_dir, const char* save_dir, WPE_NetHub* hub) {
    WPE_Engine* engine = NULL;
    WPE_TEST_ASSERT_OK(wpe_engine_create(&(WPE_EngineDesc){.project_dir = project_dir,
                                                           .embedded = true,
                                                           .no_audio_device = true,
                                                           .save_dir = save_dir,
                                                           .workers = 2},
                                         &engine));
    wpe_engine_set_net_defaults(engine, &(WPE_EngineNetDesc){.hub = hub});
    return engine;
}

/* A key going down or up in one engine of a game, or tapped (down for one frame of everyone). */
static void net_tap(WPE_NetHub* hub, WPE_Engine** e, int count, int who, SDL_Scancode scancode) {
    key(e[who], scancode, true);
    play(hub, e, count, 1);
    key(e[who], scancode, false);
    play(hub, e, count, 1);
}

/* Text the keyboard layout typed, for this engine's next frame (then that frame). */
static void type_text(WPE_Engine* engine, const char* text) {
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.text = text;
    wpe_engine_handle_event(engine, (const WPE_PlatformEvent*)&e);
    WPE_TEST_ASSERT_OK(wpe_engine_frame(engine, 1.0 / 30.0));
}

static void expect_phase(WPE_Engine* engine, const char* phase) {
    char code[160];
    snprintf(code, sizeof code,
             "local race = wpe.get_script(wpe.find('race'))\n"
             "if race.phase ~= '%s' then wpe.log('the phase is ' .. race.phase) end\n"
             "assert(race.phase == '%s')\n",
             phase, phase);
    check_lua(engine, phase, code);
}

/* Drives `car` (in the engine that owns it) through the checkpoints for three laps. */
static void drive_laps(WPE_NetHub* hub, WPE_Engine** e, int count, int who, const char* car) {
    static const float route[][2] = {{60.0f, 0.0f}, {0.0f, 35.0f}, {-60.0f, 0.0f}, {0.0f, -35.0f}};
    for (int lap = 1; lap <= 3; ++lap) {
        for (size_t i = 0; i < sizeof route / sizeof route[0]; ++i) {
            play(hub, e, count, 10); /* Some lap time. */
            for (int step = 0; step < 2; ++step) {
                char code[256];
                snprintf(code, sizeof code,
                         "local car = wpe.find('%s')\n"
                         "wpe.set_position(car, vector.create(%g, 0.1, %g))\n"
                         "wpe.set_velocity(car, vector.create(0, 0, 0))\n",
                         car, (double)route[i][0], (double)(route[i][1] + (step == 0 ? 3.0f : 0.0f)));
                check_lua(e[who], "place the car", code);
                play(hub, e, count, 3);
            }
        }
    }
}

/* Practice alone (the free cars parked, laps, Esc to the menu), then a host and a player on the
 * in-memory hub: each drives its own car and sees the other's, the host starts the race, each counts its
 * own laps and the host ranks them; a player who leaves has its car parked, and the host closing the game
 * sends the player back to the menu with a notice. */
static void test_templates_racing_online_plays(void) {
    char dir[256];
    create_project("racing_online", dir, sizeof dir);
    WPE_NetHub* hub = NULL;
    WPE_TEST_ASSERT_OK(wpe_network_hub_create(&hub));
    WPE_Engine* e[2] = {create_net_engine(dir, ROOT "/saves_host", hub),
                        create_net_engine(dir, ROOT "/saves_player", hub)};
    play(hub, e, 2, 2);
    expect_phase(e[0], "menu");

    /* The name is a text field: on its row, Return types; keys type instead of driving actions ("s" is the
     * brake); Return keeps the name (and selects nothing in the menu); Esc puts the old text back. */
    check_lua(e[0], "select the name", "wpe.get_script(wpe.find('menu')).selected = 6\n");
    play(hub, e, 1, 2);
    net_tap(hub, e, 1, 0, SDL_SCANCODE_RETURN);
    check_lua(e[0], "typing", "assert(wpe.ui_editing() == wpe.find('name field'))\n");
    key(e[0], SDL_SCANCODE_LCTRL, true);
    net_tap(hub, e, 1, 0, SDL_SCANCODE_A);
    key(e[0], SDL_SCANCODE_LCTRL, false);
    type_text(e[0], "Ada");
    key(e[0], SDL_SCANCODE_S, true);
    type_text(e[0], "s");
    check_lua(e[0], "keys type",
              "assert(wpe.get_script(wpe.find('menu')).name == 'Adas')\n"
              "assert(wpe.key_down('s') and not wpe.action_down('brake'))\n");
    key(e[0], SDL_SCANCODE_S, false);
    key(e[0], SDL_SCANCODE_RETURN, true);
    play(hub, e, 1, 3); /* Held after typing ends: it selects nothing until let go. */
    key(e[0], SDL_SCANCODE_RETURN, false);
    play(hub, e, 1, 2);
    check_lua(e[0], "the name is kept",
              "local menu = wpe.get_script(wpe.find('menu'))\n"
              "assert(wpe.ui_editing() == nil and menu.name == 'Adas' and menu.selected == 6)\n"
              "assert(wpe.load_data('player').name == 'Adas')\n");
    expect_phase(e[0], "menu");
    net_tap(hub, e, 1, 0, SDL_SCANCODE_RETURN);
    type_text(e[0], "!x"); /* '!' is no name character. */
    check_lua(e[0], "filtered", "assert(wpe.get_script(wpe.find('menu')).name == 'Adasx')\n");
    net_tap(hub, e, 1, 0, SDL_SCANCODE_ESCAPE);
    play(hub, e, 1, 2);
    check_lua(e[0], "Esc restores",
              "assert(wpe.ui_editing() == nil and wpe.get_script(wpe.find('menu')).name == 'Adas')\n");
    expect_phase(e[0], "menu");
    WPE_TEST_ASSERT_EQ_UINT(0, script_errors(e[0]));

    /* Practice: car 1 is yours, the others are parked inside the track. */
    check_lua(e[0], "practice", "wpe.send(wpe.find('menu'), 'practice')\n");
    play(hub, e, 1, 10); /* (Return is taken a moment after a phase begins: the menu's press was Return.) */
    expect_phase(e[0], "lobby");
    WPE_Ecs* host_ecs = wpe_engine_ecs(e[0]);
    WPE_TEST_ASSERT_NEAR(-22.0, position(host_ecs, find(host_ecs, "car 3")).z, 0.2);
    net_tap(hub, e, 1, 0, SDL_SCANCODE_RETURN);
    expect_phase(e[0], "countdown");
    play(hub, e, 1, 95);
    expect_phase(e[0], "racing");
    key(e[0], SDL_SCANCODE_W, true);
    play(hub, e, 1, 30);
    key(e[0], SDL_SCANCODE_W, false);
    WPE_TEST_ASSERT(position(host_ecs, find(host_ecs, "car 1")).x > -4.0f);
    drive_laps(hub, e, 1, 0, "car 1");
    expect_phase(e[0], "finished");
    check_lua(e[0], "a practice result",
              "local race = wpe.get_script(wpe.find('race'))\n"
              "assert(#race.results == 1 and race.results[1].time > 0 and race.best)\n");
    net_tap(hub, e, 1, 0, SDL_SCANCODE_ESCAPE);
    play(hub, e, 1, 2);
    host_ecs = wpe_engine_ecs(e[0]);
    expect_phase(e[0], "menu");

    /* Mika hosts, Alice joins on this computer. */
    check_lua(e[0], "host",
              "local menu = wpe.get_script(wpe.find('menu'))\nmenu.name = 'Mika'\nmenu:host()\n");
    play(hub, e, 2, 2);
    expect_phase(e[0], "lobby");
    check_lua(e[1], "join",
              "local menu = wpe.get_script(wpe.find('menu'))\nmenu.name = 'Alice'\nmenu:join('127.0.0.1')\n");
    play(hub, e, 2, 1);
    expect_phase(e[1], "joining");
    play(hub, e, 2, 20);
    expect_phase(e[1], "lobby");
    WPE_Ecs* player_ecs = wpe_engine_ecs(e[1]);
    check_lua(e[0], "the host gave car 2 away",
              "local car = wpe.find('car 2')\n"
              "assert(wpe.net_owner(car) == 1 and not wpe.net_is_local(car))\n"
              "assert(wpe.net_owner(wpe.get_script(car).body) == 1)\n"
              "assert(wpe.net_is_local(wpe.find('car 3'))) -- parked, the host's\n");
    check_lua(e[1], "Alice drives car 2",
              "assert(wpe.net_player() == 1 and wpe.net_is_local(wpe.find('car 2')))\n"
              "assert(not wpe.net_is_local(wpe.find('car 1')))\n");
    WPE_TEST_ASSERT_NEAR(-8.0, position(player_ecs, find(player_ecs, "car 2")).x, 0.2); /* On the grid. */

    /* Warming up in the lobby: Alice drives, and Mika sees her car go (and its body turn). */
    key(e[1], SDL_SCANCODE_W, true);
    key(e[1], SDL_SCANCODE_D, true);
    play(hub, e, 2, 30);
    key(e[1], SDL_SCANCODE_W, false);
    key(e[1], SDL_SCANCODE_D, false);
    play(hub, e, 2, 10);
    WPE_TEST_ASSERT_MSG(position(host_ecs, find(host_ecs, "car 2")).x > -4.0f, "the host sees car 2 at x %g",
                        (double)position(host_ecs, find(host_ecs, "car 2")).x);
    check_lua(e[0], "car 2 turned",
              "local car = wpe.get_script(wpe.find('car 2'))\n"
              "assert(math.abs(car:body_yaw() + math.pi / 2) > 0.1)\n");

    /* Mika starts the race: both cars go back to the grid for the countdown. */
    net_tap(hub, e, 2, 0, SDL_SCANCODE_RETURN);
    play(hub, e, 2, 5);
    expect_phase(e[0], "countdown");
    expect_phase(e[1], "countdown");
    WPE_TEST_ASSERT_NEAR(-8.0, position(player_ecs, find(player_ecs, "car 2")).x, 0.2);
    play(hub, e, 2, 95);
    expect_phase(e[0], "racing");
    expect_phase(e[1], "racing");

    key(e[0], SDL_SCANCODE_W, true);
    key(e[1], SDL_SCANCODE_W, true);
    play(hub, e, 2, 45);
    key(e[0], SDL_SCANCODE_W, false);
    key(e[1], SDL_SCANCODE_W, false);
    /* Mika laps first, then Alice; each machine counts its own car. */
    drive_laps(hub, e, 2, 0, "car 1");
    check_lua(e[0], "Mika finished",
              "wpe.send(wpe.find('car 1'), 'place', 'home')\n"
              "assert(wpe.get_script(wpe.find('race')).finish_time)\n");
    play(hub, e, 2, 3);
    expect_phase(e[0], "racing");
    check_lua(e[1], "Alice heard",
              "local race = wpe.get_script(wpe.find('race'))\n"
              "assert(race.progress[0] and race.progress[0].time)\n"
              "assert(race.finish_time == nil)\n");
    drive_laps(hub, e, 2, 1, "car 2");
    play(hub, e, 2, 5);
    for (int i = 0; i < 2; ++i) {
        expect_phase(e[i], "finished");
        check_lua(e[i], "results",
                  "local r = wpe.get_script(wpe.find('race')).results\n"
                  "assert(#r == 2 and r[1].name == 'Mika' and r[2].name == 'Alice')\n"
                  "assert(r[1].time and r[2].time and r[1].time < r[2].time)\n");
    }

    /* Back to the lobby; Alice leaves, and her car goes back to Mika, who parks it. */
    play(hub, e, 2, 10);
    net_tap(hub, e, 2, 0, SDL_SCANCODE_RETURN);
    play(hub, e, 2, 3);
    expect_phase(e[1], "lobby");
    net_tap(hub, e, 2, 1, SDL_SCANCODE_ESCAPE);
    play(hub, e, 2, 5);
    expect_phase(e[1], "menu");
    check_lua(e[0], "car 2 is back",
              "assert(wpe.net_is_local(wpe.find('car 2')) and #wpe.net_players() == 1)\n");
    WPE_TEST_ASSERT_NEAR(-22.0, position(host_ecs, find(host_ecs, "car 2")).z, 0.2);

    /* She joins again; then Mika closes the game, and Alice is told in the menu. */
    check_lua(e[1], "join again", "wpe.get_script(wpe.find('menu')):join('127.0.0.1')\n");
    play(hub, e, 2, 20);
    expect_phase(e[1], "lobby");
    net_tap(hub, e, 2, 0, SDL_SCANCODE_ESCAPE);
    play(hub, e, 2, 10);
    expect_phase(e[0], "menu");
    expect_phase(e[1], "menu");
    check_lua(e[1], "told",
              "assert(require('scripts/notice').text == 'The host closed the game')\n"
              "assert(wpe.net_status() == 'ended')\n");

    WPE_TEST_ASSERT_EQ_UINT(0, script_errors(e[0]));
    WPE_TEST_ASSERT_EQ_UINT(0, script_errors(e[1]));
    wpe_engine_destroy(e[1]);
    wpe_engine_destroy(e[0]);
    wpe_network_hub_destroy(hub);
    remove_project(dir);
}

/* wpe_runtime --host and --connect start the game before the scene's scripts have run: the host opens
 * the lobby by itself, and the player waits in "joining" until it is in. */
static void test_templates_racing_online_from_the_command_line(void) {
    char dir[256];
    create_project("racing_online", dir, sizeof dir);
    WPE_NetHub* hub = NULL;
    WPE_TEST_ASSERT_OK(wpe_network_hub_create(&hub));
    WPE_Engine* e[2] = {create_net_engine(dir, ROOT "/saves_host", hub),
                        create_net_engine(dir, ROOT "/saves_player", hub)};
    WPE_TEST_ASSERT_OK(wpe_engine_net_start(e[0], &(WPE_EngineNetDesc){.hub = hub, .player_name = "Server"}));
    WPE_TEST_ASSERT_OK(wpe_engine_net_start(e[1], &(WPE_EngineNetDesc){.hub = hub, .join = "127.0.0.1"}));
    play(hub, e, 2, 1);
    expect_phase(e[0], "lobby");
    expect_phase(e[1], "joining");
    play(hub, e, 2, 20);
    expect_phase(e[1], "lobby");
    check_lua(e[1], "car 2", "assert(wpe.net_is_local(wpe.find('car 2')))\n");
    WPE_TEST_ASSERT_EQ_UINT(0, script_errors(e[0]));
    WPE_TEST_ASSERT_EQ_UINT(0, script_errors(e[1]));
    wpe_engine_destroy(e[1]);
    wpe_engine_destroy(e[0]);
    wpe_network_hub_destroy(hub);
    remove_project(dir);
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_templates_create_projects_that_run),
              WPE_TEST_CASE(test_templates_reject_unknown_names),
              WPE_TEST_CASE(test_templates_first_person_plays),
              WPE_TEST_CASE(test_templates_third_person_plays), WPE_TEST_CASE(test_templates_stealth_plays),
              WPE_TEST_CASE(test_templates_shooter_plays), WPE_TEST_CASE(test_templates_platformer_plays),
              WPE_TEST_CASE(test_templates_dialogue_plays), WPE_TEST_CASE(test_templates_racing_plays),
              WPE_TEST_CASE(test_templates_racing_online_plays),
              WPE_TEST_CASE(test_templates_racing_online_from_the_command_line))
