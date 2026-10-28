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

WPE_TEST_MAIN(WPE_TEST_CASE(test_templates_create_projects_that_run),
              WPE_TEST_CASE(test_templates_reject_unknown_names),
              WPE_TEST_CASE(test_templates_first_person_plays))
