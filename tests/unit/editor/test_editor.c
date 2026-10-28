#include "wpe_test.h"

#include <stdio.h>
#include <string.h>

#include "editor/editor.h"
#include "platform/platform.h"
#include "scene/scene.h"

#define ROOT "editor_scratch"

static void write_text(const char* path, const char* text) {
    WPE_File* file = NULL;
    WPE_TEST_ASSERT_OK(wpe_file_open(path, true, &file));
    WPE_TEST_ASSERT_OK(wpe_file_write(file, text, strlen(text)));
    WPE_TEST_ASSERT_OK(wpe_file_close(file));
}

static void write_project(void) {
    WPE_TEST_ASSERT_OK(wpe_directory_create(ROOT "/project/scenes"));
    WPE_TEST_ASSERT_OK(wpe_directory_create(ROOT "/project/meshes"));
    WPE_TEST_ASSERT_OK(wpe_directory_create(ROOT "/project/scripts"));
    write_text(ROOT "/project/project.json", "{\"format\": \"wpe.project\", \"version\": 1, \"name\": \"Edit "
                                             "me\", \"start_scene\": \"scenes/start.scene\","
                                             " \"display\": {\"width\": 64, \"height\": 48}}");
    write_text(ROOT "/project/meshes/box.prim", "{\"shape\": \"box\", \"color\": [1, 0, 0]}");
    write_text(ROOT "/project/scripts/mover.luau",
               "local M = {}\nfunction M:on_update(dt) wpe.set_position(self.entity, "
               "wpe.get_position(self.entity) + "
               "vector.create(0, 1, 0)) end\nreturn M\n");
    write_text(
        ROOT "/project/scenes/start.scene",
        "{\"format\": \"wpe.scene\", \"version\": 1, \"camera\": 1, \"entities\": [\n"
        " {\"id\": 1, \"name\": \"camera\", \"transform\": {\"position\": [0, 2, 8]}, \"camera\": {}},\n"
        " {\"id\": 2, \"name\": \"box\", \"mesh\": {\"model\": \"meshes/box.prim\"}, \"transform\": {},\n"
        "  \"script\": {\"path\": \"scripts/mover.luau\"}}]}\n");
}

/* Runs a command; returns the parsed reply (free with wpe_json_free) and asserts "ok" == expect_ok. */
static WPE_JsonDocument* run(WPE_Editor* editor, const char* command, bool expect_ok) {
    WPE_JsonWriter* writer = NULL;
    WPE_TEST_ASSERT_OK(wpe_json_writer_create(&writer));
    const WPE_Result result = wpe_editor_execute(editor, command, strlen(command), writer);
    const char* text = NULL;
    size_t size = 0;
    WPE_TEST_ASSERT_OK(wpe_json_writer_finish(writer, &text, &size));
    WPE_JsonDocument* reply = NULL;
    WPE_TEST_ASSERT_OK(wpe_json_parse(text, size, &reply, NULL));
    const bool ok = wpe_json_bool(wpe_json_get(wpe_json_root(reply), "ok"), false);
    WPE_TEST_ASSERT_MSG(ok == expect_ok && (result == WPE_RESULT_OK) == ok, "%s -> %s", command, text);
    wpe_json_writer_destroy(writer);
    return reply;
}

static const WPE_JsonValue* result_of(const WPE_JsonDocument* reply) {
    return wpe_json_get(wpe_json_root(reply), "result");
}

static void run_ok(WPE_Editor* editor, const char* command) {
    wpe_json_free(run(editor, command, true));
}

static uint32_t entity_count(WPE_Editor* editor) {
    WPE_JsonDocument* reply = run(editor, "{\"cmd\": \"list_entities\"}", true);
    const uint32_t count = wpe_json_count(result_of(reply));
    wpe_json_free(reply);
    return count;
}

static WPE_Editor* open_editor(void) {
    write_project();
    WPE_Editor* editor = NULL;
    WPE_TEST_ASSERT_OK(wpe_editor_create(
        &(WPE_EditorDesc){.project_dir = ROOT "/project", .no_audio_device = true, .workers = 2}, &editor));
    return editor;
}

static void test_editor_opens_project_in_edit_mode(void) {
    WPE_Editor* editor = open_editor();
    WPE_TEST_ASSERT_EQ_INT(WPE_EDITOR_EDIT, wpe_editor_mode(editor));
    WPE_TEST_ASSERT_EQ_STR("scenes/start.scene", wpe_editor_scene_path(editor));
    WPE_TEST_ASSERT_EQ_UINT(2, entity_count(editor)); /* The editor camera is not listed. */
    WPE_JsonDocument* status = run(editor, "{\"cmd\": \"status\"}", true);
    WPE_TEST_ASSERT_EQ_STR("edit", wpe_json_string(wpe_json_get(result_of(status), "mode")));
    wpe_json_free(status);
    WPE_JsonDocument* help = run(editor, "{\"cmd\": \"help\"}", true);
    WPE_TEST_ASSERT(wpe_json_get(result_of(help), "set_component") != NULL);
    wpe_json_free(help);
    /* Edit mode does not run scripts: the box stays put. */
    for (int i = 0; i < 3; ++i)
        WPE_TEST_ASSERT_OK(wpe_editor_frame(editor, 1.0 / 60.0));
    WPE_JsonDocument* list = run(editor, "{\"cmd\": \"list_entities\"}", true);
    const WPE_JsonValue* box = NULL;
    for (uint32_t i = 0; i < wpe_json_count(result_of(list)); ++i) {
        const WPE_JsonValue* e = wpe_json_at(result_of(list), i);
        if (strcmp(wpe_json_string(wpe_json_get(e, "name")), "box") == 0) box = e;
    }
    WPE_TEST_ASSERT(box != NULL);
    const WPE_Entity id = (WPE_Entity)wpe_json_number(wpe_json_get(box, "id"), 0);
    const WPE_Transform* t =
        wpe_ecs_get_component(wpe_engine_ecs(wpe_editor_engine(editor)), id, WPE_COMP_TRANSFORM);
    WPE_TEST_ASSERT_NEAR(0.0, t->position.y, 0.0);
    wpe_json_free(list);
    wpe_editor_destroy(editor);
}

static void test_editor_edits_with_undo_and_redo(void) {
    WPE_Editor* editor = open_editor();
    WPE_JsonDocument* created =
        run(editor,
            "{\"cmd\": \"create_entity\", \"entity\": {\"name\": \"lamp\", \"transform\": "
            "{\"position\": [1, 2, 3]}, \"light\": {\"type\": \"point\"}}}",
            true);
    const WPE_Entity lamp = (WPE_Entity)wpe_json_number(result_of(created), 0);
    wpe_json_free(created);
    WPE_TEST_ASSERT_EQ_UINT(lamp, wpe_editor_selection(editor));
    WPE_TEST_ASSERT_EQ_UINT(3, entity_count(editor));
    WPE_TEST_ASSERT(wpe_editor_dirty(editor));

    char command[512];
    snprintf(
        command, sizeof command,
        "{\"cmd\": \"set_component\", \"id\": %u, \"component\": \"light\", \"value\": {\"type\": \"spot\", "
        "\"intensity\": 4}}",
        lamp);
    WPE_JsonDocument* updated = run(editor, command, true);
    WPE_TEST_ASSERT_EQ_STR("spot",
                           wpe_json_string(wpe_json_get(wpe_json_get(result_of(updated), "light"), "type")));
    wpe_json_free(updated);

    /* A bad value fails with the loader's message in the reply's log, and changes nothing. */
    wpe_log_set_stderr_enabled(false);
    snprintf(command, sizeof command,
             "{\"cmd\": \"set_component\", \"id\": %u, \"component\": \"light\", \"value\": {\"type\": "
             "\"laser\"}}",
             lamp);
    WPE_JsonDocument* failed = run(editor, command, false);
    const WPE_JsonValue* log = wpe_json_get(wpe_json_root(failed), "log");
    WPE_TEST_ASSERT(wpe_json_count(log) >= 1 &&
                    strstr(wpe_json_string(wpe_json_at(log, 0)), "laser") != NULL);
    wpe_json_free(failed);
    wpe_json_free(run(editor, "{\"cmd\": \"get_entity\", \"id\": 12345}", false));
    wpe_json_free(run(editor, "{\"cmd\": \"no_such_command\"}", false));
    wpe_json_free(run(editor, "{\"cmd\": ", false));
    wpe_json_free(run(editor, "[1]", false));
    wpe_log_set_stderr_enabled(true);

    snprintf(command, sizeof command, "{\"cmd\": \"duplicate_entity\", \"id\": %u}", lamp);
    run_ok(editor, command);
    WPE_TEST_ASSERT_EQ_UINT(4, entity_count(editor));
    snprintf(command, sizeof command, "{\"cmd\": \"delete_entity\", \"id\": %u}", lamp);
    run_ok(editor, command);
    WPE_TEST_ASSERT_EQ_UINT(3, entity_count(editor));
    run_ok(editor, "{\"cmd\": \"run_lua\", \"code\": \"wpe.spawn('from lua')\"}");
    WPE_TEST_ASSERT_EQ_UINT(4, entity_count(editor));

    /* Undo walks back through every change; redo forward again. */
    WPE_TEST_ASSERT_EQ_UINT(5, wpe_editor_undo_count(editor));
    run_ok(editor, "{\"cmd\": \"undo\"}");
    WPE_TEST_ASSERT_EQ_UINT(3, entity_count(editor)); /* Lua's entity is gone. */
    run_ok(editor, "{\"cmd\": \"undo\"}");
    WPE_TEST_ASSERT_EQ_UINT(4, entity_count(editor)); /* The lamp is back. */
    run_ok(editor, "{\"cmd\": \"undo\"}");
    run_ok(editor, "{\"cmd\": \"undo\"}");
    run_ok(editor, "{\"cmd\": \"undo\"}");
    WPE_TEST_ASSERT_EQ_UINT(2, entity_count(editor));
    wpe_json_free(run(editor, "{\"cmd\": \"undo\"}", false)); /* Nothing left. */
    WPE_TEST_ASSERT_EQ_UINT(5, wpe_editor_redo_count(editor));
    run_ok(editor, "{\"cmd\": \"redo\"}");
    WPE_TEST_ASSERT_EQ_UINT(3, entity_count(editor));
    wpe_editor_destroy(editor);
}

static void test_editor_scenes_environment_and_screenshots(void) {
    WPE_Editor* editor = open_editor();
    run_ok(editor, "{\"cmd\": \"set_environment\", \"value\": {\"fog\": {\"mode\": \"linear\", \"start\": 2, "
                   "\"end\": 20}}}");
    WPE_JsonDocument* env = run(editor, "{\"cmd\": \"get_environment\"}", true);
    WPE_TEST_ASSERT_EQ_STR("linear",
                           wpe_json_string(wpe_json_get(wpe_json_get(result_of(env), "fog"), "mode")));
    wpe_json_free(env);
    run_ok(editor, "{\"cmd\": \"create_entity\", \"entity\": {\"name\": \"extra\"}}");
    run_ok(editor, "{\"cmd\": \"save_scene\", \"path\": \"scenes/saved.scene\"}");
    WPE_TEST_ASSERT(!wpe_editor_dirty(editor));
    WPE_TEST_ASSERT_EQ_STR("scenes/saved.scene", wpe_editor_scene_path(editor));
    /* The editor camera is not saved. */
    void* data = NULL;
    size_t size = 0;
    WPE_TEST_ASSERT_OK(wpe_file_read_all(ROOT "/project/scenes/saved.scene", &data, &size));
    WPE_TEST_ASSERT(strstr(data, "editor camera") == NULL && strstr(data, "\"extra\"") != NULL);
    wpe_free(data);
    run_ok(editor, "{\"cmd\": \"new_scene\"}");
    WPE_TEST_ASSERT_EQ_UINT(0, entity_count(editor));
    wpe_log_set_stderr_enabled(false);
    wpe_json_free(run(editor, "{\"cmd\": \"save_scene\"}", false)); /* A new scene needs a path. */
    wpe_json_free(run(editor, "{\"cmd\": \"load_scene\", \"path\": \"scenes/missing.scene\"}", false));
    wpe_log_set_stderr_enabled(true);
    run_ok(editor, "{\"cmd\": \"load_scene\", \"path\": \"scenes/saved.scene\"}");
    WPE_TEST_ASSERT_EQ_UINT(3, entity_count(editor));
    run_ok(editor, "{\"cmd\": \"camera\", \"position\": [0, 5, 10], \"target\": [0, 0, 0]}");
    run_ok(editor, "{\"cmd\": \"screenshot\", \"path\": \"" ROOT "/game.png\"}");
    WPE_Image image = {0};
    WPE_TEST_ASSERT_OK(wpe_image_load_png(ROOT "/game.png", &image));
    WPE_TEST_ASSERT(image.width == 64 && image.height == 48);
    /* The red box is in view. */
    uint32_t red = 0;
    for (uint32_t i = 0; i < image.width * image.height; ++i)
        red += ((image.pixels[i] >> 16) & 0xFF) > 60 &&
               ((image.pixels[i] >> 16) & 0xFF) > 2 * ((image.pixels[i] >> 8) & 0xFF);
    WPE_TEST_ASSERT(red > 6);
    wpe_image_free(&image);
    wpe_log_set_stderr_enabled(false);
    wpe_json_free(
        run(editor, "{\"cmd\": \"screenshot\", \"path\": \"x.png\", \"target\": \"editor\"}", false));
    wpe_log_set_stderr_enabled(true);
    WPE_JsonDocument* assets = run(editor, "{\"cmd\": \"list_assets\", \"prefix\": \"meshes/\"}", true);
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_json_count(result_of(assets)));
    wpe_json_free(assets);
    wpe_editor_destroy(editor);
}

static void test_editor_play_mode_restores_the_scene(void) {
    WPE_Editor* editor = open_editor();
    WPE_Ecs* ecs = wpe_engine_ecs(wpe_editor_engine(editor));
    run_ok(editor, "{\"cmd\": \"play\"}");
    WPE_TEST_ASSERT_EQ_INT(WPE_EDITOR_PLAY, wpe_editor_mode(editor));
    run_ok(editor, "{\"cmd\": \"frames\", \"count\": 5}");
    /* The script moved the box while playing. */
    float moved = 0.0f;
    WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_SCRIPT)});
    while (wpe_ecs_iter_next(&it))
        moved = ((WPE_Transform*)wpe_ecs_get_component(ecs, it.entity, WPE_COMP_TRANSFORM))->position.y;
    WPE_TEST_ASSERT(moved >= 4.0f);
    /* No editing while playing. */
    wpe_log_set_stderr_enabled(false);
    wpe_json_free(run(editor, "{\"cmd\": \"create_entity\", \"entity\": {}}", false));
    wpe_log_set_stderr_enabled(true);
    run_ok(editor, "{\"cmd\": \"pause\"}");
    WPE_TEST_ASSERT_EQ_INT(WPE_EDITOR_PAUSED, wpe_editor_mode(editor));
    run_ok(editor, "{\"cmd\": \"play\"}");
    run_ok(editor, "{\"cmd\": \"stop\"}");
    WPE_TEST_ASSERT_EQ_INT(WPE_EDITOR_EDIT, wpe_editor_mode(editor));
    it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_SCRIPT)});
    while (wpe_ecs_iter_next(&it))
        moved = ((WPE_Transform*)wpe_ecs_get_component(ecs, it.entity, WPE_COMP_TRANSFORM))->position.y;
    WPE_TEST_ASSERT_NEAR(0.0, moved, 0.0); /* Back where it was. */
    WPE_TEST_ASSERT(wpe_editor_camera(editor) != WPE_ENTITY_NULL);
    WPE_TEST_ASSERT(!wpe_editor_dirty(editor));
    wpe_editor_destroy(editor);
}

static void test_editor_coalesces_continuous_edits(void) {
    WPE_Editor* editor = open_editor();
    WPE_JsonDocument* created =
        run(editor, "{\"cmd\": \"create_entity\", \"entity\": {\"name\": \"slider\"}}", true);
    const WPE_Entity e = (WPE_Entity)wpe_json_number(result_of(created), 0);
    wpe_json_free(created);
    char command[256];
    /* A drag: many edits with one key make one undo step... */
    for (int i = 1; i <= 5; ++i) {
        snprintf(command, sizeof command,
                 "{\"cmd\": \"set_component\", \"id\": %u, \"component\": \"transform\", \"value\": "
                 "{\"position\": "
                 "[%d, 0, 0]}, \"coalesce\": \"drag-1\"}",
                 e, i);
        run_ok(editor, command);
    }
    WPE_TEST_ASSERT_EQ_UINT(2, wpe_editor_undo_count(editor));
    /* ...a new drag (another key) starts another step, and so does any uncoalesced edit. */
    snprintf(
        command, sizeof command,
        "{\"cmd\": \"set_component\", \"id\": %u, \"component\": \"transform\", \"value\": {}, \"coalesce\": "
        "\"drag-2\"}",
        e);
    run_ok(editor, command);
    run_ok(editor, "{\"cmd\": \"create_entity\", \"entity\": {}}");
    WPE_TEST_ASSERT_EQ_UINT(4, wpe_editor_undo_count(editor));
    /* Undoing the first drag returns to before it started. */
    run_ok(editor, "{\"cmd\": \"undo\"}");
    run_ok(editor, "{\"cmd\": \"undo\"}");
    run_ok(editor, "{\"cmd\": \"undo\"}");
    WPE_JsonDocument* list = run(editor, "{\"cmd\": \"list_entities\"}", true);
    WPE_TEST_ASSERT_EQ_UINT(3, wpe_json_count(result_of(list))); /* camera, box, slider */
    wpe_json_free(list);
    wpe_editor_destroy(editor);
}

/* Renders a frame and returns how many pixels differ from `previous` (then holds the new frame). */
static uint32_t frame_changes(WPE_Editor* editor, WPE_Color* previous) {
    WPE_TEST_ASSERT_OK(wpe_editor_frame(editor, 1.0 / 60.0));
    const WPE_Framebuffer* fb = wpe_renderer_framebuffer(wpe_engine_renderer(wpe_editor_engine(editor)));
    uint32_t changed = 0;
    for (uint32_t i = 0; i < fb->width * fb->height; ++i) {
        changed += fb->color[i] != previous[i];
        previous[i] = fb->color[i];
    }
    return changed;
}

static void test_editor_view_settings_and_overlays(void) {
    WPE_Editor* editor = open_editor();
    const WPE_EditorView defaults = wpe_editor_view(editor);
    WPE_TEST_ASSERT(defaults.grid && !defaults.physics && defaults.lights && defaults.hot_reload &&
                    !defaults.gizmo_local);
    WPE_TEST_ASSERT_EQ_INT(WPE_EDITOR_GIZMO_MOVE, defaults.gizmo);
    /* Merged: keys left out keep their values. */
    WPE_JsonDocument* view =
        run(editor,
            "{\"cmd\": \"set_view\", \"value\": {\"gizmo\": \"rotate\", \"gizmo_space\": "
            "\"local\", \"lights\": false}}",
            true);
    WPE_TEST_ASSERT_EQ_STR("local", wpe_json_string(wpe_json_get(result_of(view), "gizmo_space")));
    WPE_TEST_ASSERT(!wpe_json_bool(wpe_json_get(result_of(view), "lights"), true));
    wpe_json_free(view);
    WPE_TEST_ASSERT(wpe_editor_view(editor).grid && wpe_editor_view(editor).gizmo_local &&
                    !wpe_editor_view(editor).lights);
    WPE_TEST_ASSERT_EQ_INT(WPE_EDITOR_GIZMO_ROTATE, wpe_editor_view(editor).gizmo);
    /* All or nothing, and never an undo step or unsaved change. */
    wpe_json_free(
        run(editor, "{\"cmd\": \"set_view\", \"value\": {\"physics\": true, \"grid_spacing\": 0}}", false));
    wpe_json_free(run(editor, "{\"cmd\": \"set_view\", \"value\": {\"gizmo\": \"spin\"}}", false));
    wpe_json_free(run(editor, "{\"cmd\": \"set_view\", \"value\": {\"grid\": 1}}", false));
    wpe_json_free(run(editor, "{\"cmd\": \"set_view\", \"value\": {\"colour\": true}}", false));
    WPE_TEST_ASSERT(!wpe_editor_view(editor).physics);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_editor_undo_count(editor));
    WPE_TEST_ASSERT(!wpe_editor_dirty(editor));

    /* The grid and physics shapes draw over the view; switching them off removes them. */
    run_ok(editor, "{\"cmd\": \"camera\", \"position\": [3, 6, 9], \"target\": [0, 0, 0]}");
    run_ok(editor,
           "{\"cmd\": \"create_entity\", \"entity\": {\"name\": \"wall\", \"transform\": {\"position\": "
           "[2, 1, 0]}, \"collider\": {\"shape\": \"box\"}}}");
    static WPE_Color frame[64 * 48];
    frame_changes(editor, frame);
    WPE_TEST_ASSERT_EQ_UINT(0, frame_changes(editor, frame)); /* A still view renders the same. */
    run_ok(editor, "{\"cmd\": \"set_view\", \"value\": {\"grid\": false}}");
    WPE_TEST_ASSERT(frame_changes(editor, frame) > 20);
    run_ok(editor, "{\"cmd\": \"set_view\", \"value\": {\"physics\": true}}");
    WPE_TEST_ASSERT(frame_changes(editor, frame) > 10);
    wpe_editor_destroy(editor);
}

static bool raycast_down(WPE_Editor* editor, float x, WPE_RaycastHit* hit) {
    return wpe_physics_raycast(wpe_engine_physics(wpe_editor_engine(editor)), wpe_vec3(x, 10, 0),
                               wpe_vec3(0, -1, 0), 20.0f, hit);
}

/* Edit mode doesn't simulate, but physics follows edits: moves, rescales and new colliders. */
static void test_editor_physics_follows_edits(void) {
    WPE_Editor* editor = open_editor();
    WPE_JsonDocument* created =
        run(editor,
            "{\"cmd\": \"create_entity\", \"entity\": {\"name\": \"wall\", \"transform\": "
            "{\"position\": [5, 0, 0]}, \"collider\": {\"shape\": \"box\"}}}",
            true);
    const WPE_Entity wall = (WPE_Entity)wpe_json_number(result_of(created), 0);
    wpe_json_free(created);
    WPE_TEST_ASSERT_OK(wpe_editor_frame(editor, 1.0 / 60.0));
    WPE_RaycastHit hit;
    WPE_TEST_ASSERT(raycast_down(editor, 5.0f, &hit));
    WPE_TEST_ASSERT_NEAR(0.5, hit.point.y, 1e-3);

    char command[256];
    snprintf(
        command, sizeof command,
        "{\"cmd\": \"set_component\", \"id\": %u, \"component\": \"transform\", \"value\": {\"position\": "
        "[-5, 0, 0], \"scale\": [1, 4, 1]}}",
        wall);
    run_ok(editor, command);
    WPE_TEST_ASSERT_OK(wpe_editor_frame(editor, 1.0 / 60.0));
    WPE_TEST_ASSERT(!raycast_down(editor, 5.0f, &hit));
    WPE_TEST_ASSERT(raycast_down(editor, -5.0f, &hit));
    WPE_TEST_ASSERT_NEAR(2.0, hit.point.y, 1e-3); /* The shape was rebuilt for the new scale. */
    wpe_editor_destroy(editor);
}

static float box_height(WPE_Editor* editor) {
    WPE_Ecs* ecs = wpe_engine_ecs(wpe_editor_engine(editor));
    WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_SCRIPT)});
    WPE_TEST_ASSERT(wpe_ecs_iter_next(&it));
    return ((const WPE_Transform*)wpe_ecs_get_component(ecs, it.entity, WPE_COMP_TRANSFORM))->position.y;
}

/* Rewrites a file until its modification time changes (coarse file system clocks). */
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

static void test_editor_hot_reloads_scripts(void) {
    WPE_Editor* editor = open_editor();
    run_ok(editor, "{\"cmd\": \"play\"}");
    run_ok(editor, "{\"cmd\": \"frames\", \"count\": 2}");
    const float start = box_height(editor);
    run_ok(editor, "{\"cmd\": \"frames\"}");
    WPE_TEST_ASSERT_NEAR(1.0, box_height(editor) - start, 1e-4); /* mover.luau: +1 per frame. */

    /* Edited while playing: picked up within half a second, and the game keeps running. */
    rewrite(ROOT "/project/scripts/mover.luau",
            "local M = {}\nfunction M:on_update(dt) wpe.set_position(self.entity, "
            "wpe.get_position(self.entity) + vector.create(0, 100, 0)) end\n"
            "return M\n");
    run_ok(editor, "{\"cmd\": \"frames\", \"count\": 31}");
    const float before = box_height(editor);
    run_ok(editor, "{\"cmd\": \"frames\"}");
    WPE_TEST_ASSERT_NEAR(100.0, box_height(editor) - before, 1e-3);

    /* Switched off, edits wait. */
    run_ok(editor, "{\"cmd\": \"set_view\", \"value\": {\"hot_reload\": false}}");
    rewrite(ROOT "/project/scripts/mover.luau", "local M = {}\nfunction M:on_update(dt) end\nreturn M\n");
    run_ok(editor, "{\"cmd\": \"frames\", \"count\": 40}");
    const float still_moving = box_height(editor);
    run_ok(editor, "{\"cmd\": \"frames\"}");
    WPE_TEST_ASSERT_NEAR(100.0, box_height(editor) - still_moving, 1e-3);
    run_ok(editor, "{\"cmd\": \"stop\"}");
    wpe_editor_destroy(editor);
}

/* Commands that replace the scene never drop unsaved changes by accident. */
static void test_editor_guards_unsaved_changes(void) {
    WPE_Editor* editor = open_editor();
    run_ok(editor, "{\"cmd\": \"new_scene\"}"); /* Nothing unsaved yet. */
    run_ok(editor, "{\"cmd\": \"create_entity\", \"entity\": {\"name\": \"precious\"}}");
    WPE_TEST_ASSERT(wpe_editor_dirty(editor));
    WPE_JsonDocument* refused =
        run(editor, "{\"cmd\": \"load_scene\", \"path\": \"scenes/start.scene\"}", false);
    WPE_TEST_ASSERT(strstr(wpe_json_string(wpe_json_get(wpe_json_root(refused), "error")), "discard") !=
                    NULL);
    wpe_json_free(refused);
    wpe_json_free(run(editor, "{\"cmd\": \"new_scene\"}", false));
    wpe_json_free(run(editor, "{\"cmd\": \"quit\"}", false));
    wpe_json_free(run(editor, "{\"cmd\": \"quit\", \"discard\": false}", false));
    WPE_TEST_ASSERT(!wpe_editor_quit_requested(editor));
    WPE_TEST_ASSERT_EQ_UINT(1, entity_count(editor)); /* Still there. */

    /* Saving makes them safe to leave; "discard" drops them on purpose. */
    run_ok(editor, "{\"cmd\": \"save_scene\", \"path\": \"scenes/precious.scene\"}");
    run_ok(editor, "{\"cmd\": \"load_scene\", \"path\": \"scenes/start.scene\"}");
    run_ok(editor, "{\"cmd\": \"create_entity\", \"entity\": {}}");
    run_ok(editor, "{\"cmd\": \"load_scene\", \"path\": \"scenes/precious.scene\", \"discard\": true}");
    WPE_TEST_ASSERT_EQ_UINT(1, entity_count(editor));
    run_ok(editor, "{\"cmd\": \"create_entity\", \"entity\": {}}");
    run_ok(editor, "{\"cmd\": \"quit\", \"discard\": true}");
    WPE_TEST_ASSERT(wpe_editor_quit_requested(editor));
    wpe_editor_destroy(editor);
}

static void test_editor_focus_frames_the_selection(void) {
    WPE_Editor* editor = open_editor();
    wpe_log_set_stderr_enabled(false);
    wpe_json_free(run(editor, "{\"cmd\": \"focus\"}", false)); /* Nothing selected. */
    wpe_log_set_stderr_enabled(true);
    run_ok(editor, "{\"cmd\": \"camera\", \"position\": [0, 4, 0], \"target\": [-6, 0, -8]}");
    /* A 1x1x1 box scaled to 4 wide, at x = 20: framed whole, from the direction the camera faces. */
    run_ok(editor, "{\"cmd\": \"create_entity\", \"entity\": {\"name\": \"far\", \"mesh\": {\"model\": "
                   "\"meshes/box.prim\"}, \"transform\": {\"position\": [20, 1, 5], \"scale\": [4, 1, 1]}}}");
    WPE_JsonDocument* reply = run(editor, "{\"cmd\": \"focus\"}", true);
    const WPE_JsonValue* position = wpe_json_get(result_of(reply), "position");
    const WPE_JsonValue* target = wpe_json_get(result_of(reply), "target");
    WPE_TEST_ASSERT_NEAR(20.0, wpe_json_number(wpe_json_at(target, 0), 0), 1e-4);
    WPE_TEST_ASSERT_NEAR(1.0, wpe_json_number(wpe_json_at(target, 1), 0), 1e-4);
    WPE_Ecs* ecs = wpe_engine_ecs(wpe_editor_engine(editor));
    const WPE_Transform* t = wpe_ecs_get_component(ecs, wpe_editor_camera(editor), WPE_COMP_TRANSFORM);
    WPE_TEST_ASSERT_NEAR(wpe_json_number(wpe_json_at(position, 0), 0), t->position.x, 1e-4);
    /* The target is straight ahead, far enough that the bounding sphere (radius ~2.1) fits. */
    const WPE_Vec3 forward = wpe_quat_rotate_vec3(t->rotation, wpe_vec3(0, 0, -1));
    const WPE_Vec3 to_target = wpe_vec3_sub(wpe_vec3(20, 1, 5), t->position);
    const float distance = wpe_vec3_length(to_target);
    WPE_TEST_ASSERT_NEAR(1.0, wpe_vec3_dot(forward, to_target) / distance, 1e-4);
    const WPE_Camera* camera = wpe_ecs_get_component(ecs, wpe_editor_camera(editor), WPE_COMP_CAMERA);
    WPE_TEST_ASSERT(distance * sinf(camera->fov_y * 0.5f) >= 2.1f);
    WPE_TEST_ASSERT(distance < 12.0f);
    wpe_json_free(reply);
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_editor_undo_count(editor)); /* Only the creation; focusing is no edit. */
    wpe_editor_destroy(editor);
}

static void test_editor_snap_settings(void) {
    WPE_Editor* editor = open_editor();
    const WPE_EditorView defaults = wpe_editor_view(editor);
    WPE_TEST_ASSERT(!defaults.snap);
    WPE_TEST_ASSERT_NEAR(0.5, defaults.snap_move, 0);
    WPE_TEST_ASSERT_NEAR(15.0, defaults.snap_rotate, 0);
    WPE_TEST_ASSERT_NEAR(0.1, defaults.snap_scale, 1e-7);
    run_ok(editor,
           "{\"cmd\": \"set_view\", \"value\": {\"snap\": true, \"snap_move\": 0.25, \"snap_rotate\": 45, "
           "\"snap_scale\": 0.5}}");
    const WPE_EditorView view = wpe_editor_view(editor);
    WPE_TEST_ASSERT(view.snap);
    WPE_TEST_ASSERT_NEAR(0.25, view.snap_move, 0);
    WPE_TEST_ASSERT_NEAR(45.0, view.snap_rotate, 0);
    WPE_TEST_ASSERT_NEAR(0.5, view.snap_scale, 0);
    wpe_json_free(run(editor, "{\"cmd\": \"set_view\", \"value\": {\"snap_rotate\": 0}}", false));
    wpe_json_free(run(editor, "{\"cmd\": \"set_view\", \"value\": {\"snap_move\": -1}}", false));
    wpe_json_free(run(editor, "{\"cmd\": \"set_view\", \"value\": {\"snap\": \"yes\"}}", false));
    WPE_TEST_ASSERT_NEAR(45.0, wpe_editor_view(editor).snap_rotate, 0);
    wpe_editor_destroy(editor);
}

static WPE_Entity id_of(WPE_Editor* editor, const char* name);

/* An entity's collider, read back through get_entity: size, offset and shape; returns its density. */
static double collider_of(WPE_Editor* editor, WPE_Entity e, float size[3], float offset[3],
                          const char** shape) {
    char command[64];
    snprintf(command, sizeof command, "{\"cmd\": \"get_entity\", \"id\": %u}", e);
    WPE_JsonDocument* reply = run(editor, command, true);
    const WPE_JsonValue* collider = wpe_json_get(result_of(reply), "collider");
    WPE_TEST_ASSERT(collider != NULL);
    for (uint32_t i = 0; i < 3; ++i) {
        size[i] = (float)wpe_json_number(wpe_json_at(wpe_json_get(collider, "size"), i), -1);
        offset[i] = (float)wpe_json_number(wpe_json_at(wpe_json_get(collider, "offset"), i), -1);
    }
    static char shape_name[16];
    snprintf(shape_name, sizeof shape_name, "%s", wpe_json_string(wpe_json_get(collider, "shape")));
    *shape = shape_name;
    const double density = wpe_json_number(wpe_json_get(collider, "density"), -1);
    wpe_json_free(reply);
    return density;
}

static void test_editor_fits_colliders_to_meshes(void) {
    WPE_Editor* editor = open_editor();
    float size[3], offset[3];
    const char* shape = NULL;
    /* Its own mesh (a unit box), measured in its own space: the entity's scale applies on top. */
    run_ok(editor,
           "{\"cmd\": \"create_entity\", \"entity\": {\"name\": \"long\", \"transform\": {\"scale\": [2, 1, "
           "1], \"position\": [5, 0, 0]}, \"mesh\": {\"model\": \"meshes/box.prim\"}}}");
    const WPE_Entity box = id_of(editor, "long");
    const uint32_t steps = wpe_editor_undo_count(editor);
    run_ok(editor, "{\"cmd\": \"fit_collider\"}"); /* The selection: the entity just created. */
    WPE_TEST_ASSERT_EQ_UINT(steps + 1, wpe_editor_undo_count(editor));
    collider_of(editor, box, size, offset, &shape);
    WPE_TEST_ASSERT_EQ_STR("box", shape);
    for (int i = 0; i < 3; ++i) {
        WPE_TEST_ASSERT_NEAR(1.0, size[i], 1e-5);
        WPE_TEST_ASSERT_NEAR(0.0, offset[i], 1e-5);
    }
    /* An empty parent around a child moved up and stretched. */
    run_ok(editor, "{\"cmd\": \"create_entity\", \"entity\": {\"name\": \"holder\", \"transform\": {}}}");
    const WPE_Entity holder = id_of(editor, "holder");
    char command[256];
    snprintf(command, sizeof command,
             "{\"cmd\": \"create_entity\", \"entity\": {\"name\": \"part\", \"parent\": %u, \"transform\": "
             "{\"position\": [0, 1, 0], \"scale\": [1, 2, 1]}, \"mesh\": {\"model\": \"meshes/box.prim\"}}}",
             holder);
    run_ok(editor, command);
    /* A sphere keeps its shape and its other settings. */
    snprintf(command, sizeof command,
             "{\"cmd\": \"set_component\", \"id\": %u, \"component\": \"collider\", \"value\": {\"shape\": "
             "\"sphere\", \"density\": 3}}",
             holder);
    run_ok(editor, command);
    snprintf(command, sizeof command, "{\"cmd\": \"fit_collider\", \"id\": %u}", holder);
    run_ok(editor, command);
    WPE_TEST_ASSERT_NEAR(3.0, collider_of(editor, holder, size, offset, &shape), 0);
    WPE_TEST_ASSERT_EQ_STR("sphere", shape);
    WPE_TEST_ASSERT_NEAR(1.0, size[0], 1e-5); /* Half the tallest extent (2). */
    WPE_TEST_ASSERT_NEAR(1.0, offset[1], 1e-5);
    WPE_TEST_ASSERT_NEAR(0.0, offset[0], 1e-5);
    /* Nothing to fit to: refused, and no undo step is left. */
    run_ok(editor, "{\"cmd\": \"create_entity\", \"entity\": {\"name\": \"empty\", \"transform\": {}}}");
    const uint32_t before = wpe_editor_undo_count(editor);
    wpe_json_free(run(editor, "{\"cmd\": \"fit_collider\"}", false));
    WPE_TEST_ASSERT_EQ_UINT(before, wpe_editor_undo_count(editor));
    wpe_editor_destroy(editor);
}

static WPE_Color pixel(WPE_Editor* editor, uint32_t x, uint32_t y) {
    const WPE_Framebuffer* fb = wpe_renderer_framebuffer(wpe_engine_renderer(wpe_editor_engine(editor)));
    return fb->color[y * fb->width + x];
}

static void test_editor_2d_view_shows_the_game_and_its_ui(void) {
    WPE_Editor* editor = open_editor();
    WPE_Engine* engine = wpe_editor_engine(editor);
    WPE_TEST_ASSERT(!wpe_editor_view(editor).view_2d);
    WPE_TEST_ASSERT_NEAR(8.0, wpe_editor_view(editor).snap_ui, 0);
    /* An 8x8 green square in the top-left corner. */
    run_ok(editor,
           "{\"cmd\": \"create_entity\", \"entity\": {\"name\": \"hud\", \"ui\": {\"anchor_max\": [0, 0], "
           "\"offset_max\": [8, 8]}, \"sprite\": {\"color\": [0, 1, 0, 1]}}}");
    WPE_JsonDocument* list = run(editor, "{\"cmd\": \"list_entities\"}", true);
    bool listed = false;
    for (uint32_t i = 0; i < wpe_json_count(result_of(list)); ++i) {
        const WPE_JsonValue* components = wpe_json_get(wpe_json_at(result_of(list), i), "components");
        for (uint32_t c = 0; c < wpe_json_count(components); ++c)
            listed = listed || strcmp(wpe_json_string(wpe_json_at(components, c)), "sprite") == 0;
    }
    wpe_json_free(list);
    WPE_TEST_ASSERT(listed);

    /* The 3D view: the editor camera, without the UI. */
    WPE_TEST_ASSERT_OK(wpe_editor_frame(editor, 1.0 / 60.0));
    WPE_TEST_ASSERT(!wpe_engine_draws_ui(engine));
    WPE_TEST_ASSERT_EQ_UINT(wpe_editor_camera(editor), wpe_engine_camera(engine));
    WPE_TEST_ASSERT(pixel(editor, 2, 2) != 0xFF00FF00u);

    /* The 2D view: the scene's camera and the UI over it; never saved or undone. */
    WPE_JsonDocument* view =
        run(editor, "{\"cmd\": \"set_view\", \"value\": {\"mode\": \"2d\", \"snap_ui\": 4}}", true);
    WPE_TEST_ASSERT_EQ_STR("2d", wpe_json_string(wpe_json_get(result_of(view), "mode")));
    wpe_json_free(view);
    WPE_TEST_ASSERT(wpe_editor_view(editor).view_2d);
    WPE_TEST_ASSERT_NEAR(4.0, wpe_editor_view(editor).snap_ui, 0);
    WPE_TEST_ASSERT_OK(wpe_editor_frame(editor, 1.0 / 60.0));
    WPE_TEST_ASSERT(wpe_engine_draws_ui(engine) && wpe_engine_draws_scene(engine));
    WPE_TEST_ASSERT_EQ_UINT(id_of(editor, "camera"), wpe_engine_camera(engine));
    WPE_TEST_ASSERT_EQ_UINT(0xFF00FF00u, pixel(editor, 2, 2));
    wpe_json_free(run(editor, "{\"cmd\": \"set_view\", \"value\": {\"mode\": \"4d\"}}", false));
    wpe_json_free(run(editor, "{\"cmd\": \"set_view\", \"value\": {\"snap_ui\": 0}}", false));

    /* A scene without a camera is a 2D scene: the clear color under the UI. */
    char command[96];
    snprintf(command, sizeof command, "{\"cmd\": \"delete_entity\", \"id\": %u}", id_of(editor, "camera"));
    run_ok(editor, command);
    WPE_TEST_ASSERT_OK(wpe_editor_frame(editor, 1.0 / 60.0));
    WPE_TEST_ASSERT(!wpe_engine_draws_scene(engine));
    WPE_TEST_ASSERT_EQ_UINT(0xFF00FF00u, pixel(editor, 2, 2));
    WPE_TEST_ASSERT_EQ_UINT(pixel(editor, 32, 24), pixel(editor, 60, 44)); /* The box isn't drawn. */

    /* Play mode shows the game, UI included, whichever view is chosen. */
    run_ok(editor, "{\"cmd\": \"set_view\", \"value\": {\"mode\": \"3d\"}}");
    run_ok(editor, "{\"cmd\": \"play\"}");
    WPE_TEST_ASSERT_OK(wpe_editor_frame(editor, 1.0 / 60.0));
    WPE_TEST_ASSERT(wpe_engine_draws_ui(engine) && wpe_engine_draws_scene(engine));
    WPE_TEST_ASSERT_EQ_UINT(0xFF00FF00u, pixel(editor, 2, 2));
    run_ok(editor, "{\"cmd\": \"stop\"}");
    WPE_TEST_ASSERT_OK(wpe_editor_frame(editor, 1.0 / 60.0));
    WPE_TEST_ASSERT(!wpe_engine_draws_ui(engine));
    wpe_editor_destroy(editor);
}

/* The id of the first listed entity named `name`. */
static WPE_Entity id_of(WPE_Editor* editor, const char* name) {
    WPE_JsonDocument* list = run(editor, "{\"cmd\": \"list_entities\"}", true);
    WPE_Entity id = WPE_ENTITY_NULL;
    for (uint32_t i = 0; i < wpe_json_count(result_of(list)) && !id; ++i) {
        const WPE_JsonValue* e = wpe_json_at(result_of(list), i);
        const char* n = wpe_json_string(wpe_json_get(e, "name"));
        if (n && strcmp(n, name) == 0) id = (WPE_Entity)wpe_json_number(wpe_json_get(e, "id"), 0);
    }
    wpe_json_free(list);
    return id;
}

static float light_intensity(WPE_Editor* editor, WPE_Entity e) {
    const WPE_Light* light =
        wpe_ecs_get_component(wpe_engine_ecs(wpe_editor_engine(editor)), e, WPE_COMP_LIGHT);
    WPE_TEST_ASSERT(light != NULL);
    return light->intensity;
}

static void test_editor_prefab_commands(void) {
    WPE_Editor* editor = open_editor();
    char command[512];
    /* A lamp with a bulb child, saved as a prefab: it becomes an instance in place. */
    run_ok(editor,
           "{\"cmd\": \"create_entity\", \"entity\": {\"name\": \"lamp\", \"light\": {\"type\": \"point\", "
           "\"intensity\": 2}}}");
    const WPE_Entity lamp = id_of(editor, "lamp");
    snprintf(
        command, sizeof command,
        "{\"cmd\": \"create_entity\", \"entity\": {\"name\": \"bulb\", \"parent\": %u, \"mesh\": {\"model\": "
        "\"meshes/box.prim\"}}}",
        lamp);
    run_ok(editor, command);
    snprintf(command, sizeof command,
             "{\"cmd\": \"save_prefab\", \"id\": %u, \"path\": \"prefabs/lamp.prefab\"}", lamp);
    WPE_JsonDocument* saved = run(editor, command, true);
    WPE_TEST_ASSERT(!wpe_json_bool(wpe_json_get(result_of(saved), "reloaded"), true));
    wpe_json_free(saved);
    int64_t written = 0;
    WPE_TEST_ASSERT_OK(wpe_file_modified_time(ROOT "/project/prefabs/lamp.prefab", &written));
    WPE_JsonDocument* list = run(editor, "{\"cmd\": \"list_entities\"}", true);
    uint32_t members = 0;
    for (uint32_t i = 0; i < wpe_json_count(result_of(list)); ++i)
        members += wpe_json_get(wpe_json_at(result_of(list), i), "prefab") != NULL;
    WPE_TEST_ASSERT_EQ_UINT(2, members);
    wpe_json_free(list);

    /* A second instance, as an agent places one; members can't be moved out of an instance. */
    run_ok(editor,
           "{\"cmd\": \"create_entity\", \"entity\": {\"name\": \"lamp 2\", \"transform\": {\"position\": "
           "[5, 0, 0]}, \"prefab\": {\"path\": \"prefabs/lamp.prefab\"}}}");
    WPE_TEST_ASSERT(id_of(editor, "lamp 2") != WPE_ENTITY_NULL);
    wpe_log_set_stderr_enabled(false);
    snprintf(command, sizeof command, "{\"cmd\": \"set_parent\", \"id\": %u, \"parent\": null}",
             id_of(editor, "bulb"));
    wpe_json_free(run(editor, command, false));
    wpe_log_set_stderr_enabled(true);

    /* Applying a change to one instance reloads the scene so the other one gets it too. */
    snprintf(
        command, sizeof command,
        "{\"cmd\": \"set_component\", \"id\": %u, \"component\": \"light\", \"value\": {\"type\": \"point\", "
        "\"intensity\": 7}}",
        lamp);
    run_ok(editor, command);
    snprintf(command, sizeof command, "{\"cmd\": \"apply_prefab\", \"id\": %u}", lamp);
    WPE_JsonDocument* applied = run(editor, command, true);
    WPE_TEST_ASSERT(wpe_json_bool(wpe_json_get(result_of(applied), "reloaded"), false));
    wpe_json_free(applied);
    WPE_TEST_ASSERT_NEAR(7.0, light_intensity(editor, id_of(editor, "lamp 2")), 1e-6);

    /* An override can be reverted alone, or the whole instance rebuilt. */
    const WPE_Entity second = id_of(editor, "lamp 2");
    snprintf(
        command, sizeof command,
        "{\"cmd\": \"set_component\", \"id\": %u, \"component\": \"light\", \"value\": {\"type\": \"spot\"}}",
        second);
    run_ok(editor, command);
    snprintf(command, sizeof command, "{\"cmd\": \"revert_prefab\", \"id\": %u, \"component\": \"light\"}",
             second);
    run_ok(editor, command);
    WPE_TEST_ASSERT_NEAR(7.0, light_intensity(editor, second), 1e-6);
    snprintf(command, sizeof command, "{\"cmd\": \"revert_prefab\", \"id\": %u}", second);
    WPE_JsonDocument* rebuilt = run(editor, command, true);
    const WPE_Entity fresh = (WPE_Entity)wpe_json_number(result_of(rebuilt), 0);
    wpe_json_free(rebuilt);
    WPE_TEST_ASSERT(fresh != second && fresh == id_of(editor, "lamp 2"));

    /* Saved scenes refer to the prefab; unpacking makes plain entities; undo brings the link back. */
    run_ok(editor, "{\"cmd\": \"save_scene\", \"path\": \"scenes/lamps.scene\"}");
    char* text = NULL;
    size_t size = 0;
    WPE_TEST_ASSERT_OK(wpe_file_read_all(ROOT "/project/scenes/lamps.scene", (void**)&text, &size));
    WPE_TEST_ASSERT(strstr(text, "\"path\": \"prefabs/lamp.prefab\"") != NULL &&
                    strstr(text, "\"bulb\"") == NULL);
    wpe_free(text);
    snprintf(command, sizeof command, "{\"cmd\": \"unpack_prefab\", \"id\": %u}", fresh);
    run_ok(editor, command);
    WPE_TEST_ASSERT_EQ_UINT(
        WPE_ENTITY_NULL, wpe_prefab_root(wpe_engine_ecs(wpe_editor_engine(editor)), id_of(editor, "lamp 2")));
    run_ok(editor, "{\"cmd\": \"undo\"}");
    WPE_TEST_ASSERT(wpe_prefab_root(wpe_engine_ecs(wpe_editor_engine(editor)), id_of(editor, "lamp 2")) !=
                    WPE_ENTITY_NULL);
    wpe_log_set_stderr_enabled(false);
    snprintf(command, sizeof command, "{\"cmd\": \"apply_prefab\", \"id\": %u}", id_of(editor, "box"));
    wpe_json_free(run(editor, command, false)); /* Not an instance. */
    snprintf(command, sizeof command, "{\"cmd\": \"save_prefab\", \"id\": %u, \"path\": \"prefabs/x.scene\"}",
             id_of(editor, "box"));
    wpe_json_free(run(editor, command, false)); /* Not a prefab path. */
    wpe_log_set_stderr_enabled(true);
    wpe_editor_destroy(editor);
}

WPE_TEST_MAIN(
    WPE_TEST_CASE(test_editor_coalesces_continuous_edits), WPE_TEST_CASE(test_editor_prefab_commands),
    WPE_TEST_CASE(test_editor_2d_view_shows_the_game_and_its_ui),
    WPE_TEST_CASE(test_editor_fits_colliders_to_meshes), WPE_TEST_CASE(test_editor_guards_unsaved_changes),
    WPE_TEST_CASE(test_editor_focus_frames_the_selection), WPE_TEST_CASE(test_editor_snap_settings),
    WPE_TEST_CASE(test_editor_view_settings_and_overlays), WPE_TEST_CASE(test_editor_physics_follows_edits),
    WPE_TEST_CASE(test_editor_hot_reloads_scripts), WPE_TEST_CASE(test_editor_opens_project_in_edit_mode),
    WPE_TEST_CASE(test_editor_edits_with_undo_and_redo),
    WPE_TEST_CASE(test_editor_scenes_environment_and_screenshots),
    WPE_TEST_CASE(test_editor_play_mode_restores_the_scene))
