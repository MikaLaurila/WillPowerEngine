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

WPE_TEST_MAIN(WPE_TEST_CASE(test_editor_coalesces_continuous_edits),
              WPE_TEST_CASE(test_editor_opens_project_in_edit_mode),
              WPE_TEST_CASE(test_editor_edits_with_undo_and_redo),
              WPE_TEST_CASE(test_editor_scenes_environment_and_screenshots),
              WPE_TEST_CASE(test_editor_play_mode_restores_the_scene))
