#include <stdio.h>
#include <string.h>

#include "engine/engine.h"
#include "platform/platform.h"
#include "wpe_test.h"

#define ROOT "engine_network_scratch"

static void write_text(const char* path, const char* text) {
    WPE_File* file = NULL;
    WPE_TEST_ASSERT_OK(wpe_file_open(path, true, &file));
    WPE_TEST_ASSERT_OK(wpe_file_write(file, text, strlen(text)));
    WPE_TEST_ASSERT_OK(wpe_file_close(file));
}

/* A game with three scenes told apart by gravity: a (-1), b (-2), menu (-3). `script` goes in
 * scripts/rules.luau (part of the game id). */
static void write_game(const char* dir, const char* script) {
    char path[256];
    snprintf(path, sizeof path, "%s/scenes", dir);
    WPE_TEST_ASSERT_OK(wpe_directory_create(path));
    snprintf(path, sizeof path, "%s/scripts", dir);
    WPE_TEST_ASSERT_OK(wpe_directory_create(path));
    snprintf(path, sizeof path, "%s/project.json", dir);
    write_text(path, "{\"format\": \"wpe.project\", \"version\": 1, \"name\": \"Net Test\",\n"
                     " \"start_scene\": \"scenes/a.scene\", \"display\": {\"width\": 32, \"height\": 24},\n"
                     " \"network\": {\"port\": 7100, \"discovery_port\": 7101, \"max_players\": 3}}\n");
    const char* scenes[3][2] = {{"a", "-1"}, {"b", "-2"}, {"menu", "-3"}};
    for (int i = 0; i < 3; ++i) {
        char text[256];
        snprintf(path, sizeof path, "%s/scenes/%s.scene", dir, scenes[i][0]);
        snprintf(text, sizeof text,
                 "{\"format\": \"wpe.scene\", \"version\": 1, \"environment\": {\"gravity\": [0, %s, 0]}}\n",
                 scenes[i][1]);
        write_text(path, text);
    }
    snprintf(path, sizeof path, "%s/scripts/rules.luau", dir);
    write_text(path, script);
}

static WPE_Engine* start_engine(const char* dir, const char* scene) {
    WPE_Engine* engine = NULL;
    WPE_TEST_ASSERT_OK(wpe_engine_create(
        &(WPE_EngineDesc){.project_dir = dir, .scene = scene, .headless = true, .workers = 1}, &engine));
    return engine;
}

static float gravity_of(const WPE_Engine* engine) {
    return wpe_physics_gravity(wpe_engine_physics(engine)).y;
}

/* Frames of both engines with the hub's clock moving in step. */
static void play(WPE_NetHub* hub, WPE_Engine** engines, int count, int frames) {
    for (int f = 0; f < frames; ++f) {
        wpe_network_hub_advance(hub, 16);
        for (int i = 0; i < count; ++i)
            if (engines[i]) WPE_TEST_ASSERT_OK(wpe_engine_frame(engines[i], 0.016));
    }
}

/* ---- Project settings ---------------------------------------------------------------------- */

static void test_engine_network_project_settings(void) {
    WPE_ProjectSettings s;
    const char* minimal = "{\"format\": \"wpe.project\", \"version\": 1}";
    WPE_TEST_ASSERT_OK(wpe_project_parse(minimal, strlen(minimal), "p", &s));
    WPE_TEST_ASSERT_EQ_UINT(WPE_NET_DEFAULT_PORT, s.network.port);
    WPE_TEST_ASSERT_EQ_UINT(WPE_DISCOVERY_DEFAULT_PORT, s.network.discovery_port);
    WPE_TEST_ASSERT_EQ_UINT(4, s.network.max_players);
    WPE_TEST_ASSERT_EQ_UINT(5000, s.network.timeout_ms);

    const char* custom = "{\"format\": \"wpe.project\", \"version\": 1, \"network\": {\"port\": 9000, "
                         "\"discovery_port\": 9001, \"max_players\": 2, \"timeout_ms\": 2000}}";
    WPE_TEST_ASSERT_OK(wpe_project_parse(custom, strlen(custom), "p", &s));
    WPE_TEST_ASSERT_EQ_UINT(9000, s.network.port);
    WPE_TEST_ASSERT_EQ_UINT(2, s.network.max_players);
    /* Written back, it reads the same. */
    WPE_JsonWriter* writer = NULL;
    WPE_TEST_ASSERT_OK(wpe_json_writer_create(&writer));
    WPE_TEST_ASSERT_OK(wpe_project_write(&s, writer));
    size_t size = 0;
    const char* text = NULL;
    WPE_TEST_ASSERT_OK(wpe_json_writer_finish(writer, &text, &size));
    WPE_ProjectSettings again;
    WPE_TEST_ASSERT_OK(wpe_project_parse(text, size, "written", &again));
    WPE_TEST_ASSERT(memcmp(&s.network, &again.network, sizeof s.network) == 0);
    wpe_json_writer_destroy(writer);

    const char* bad[] = {
        "{\"format\": \"wpe.project\", \"version\": 1, \"network\": {\"max_players\": 9}}",
        "{\"format\": \"wpe.project\", \"version\": 1, \"network\": {\"max_players\": 1}}",
        "{\"format\": \"wpe.project\", \"version\": 1, \"network\": {\"port\": 0}}",
        "{\"format\": \"wpe.project\", \"version\": 1, \"network\": {\"port\": 5, \"discovery_port\": 5}}",
        "{\"format\": \"wpe.project\", \"version\": 1, \"network\": {\"timeout_ms\": 10}}",
        "{\"format\": \"wpe.project\", \"version\": 1, \"network\": {\"tick\": 30}}",
    };
    wpe_log_set_stderr_enabled(false);
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i)
        WPE_TEST_ASSERT_MSG(wpe_project_parse(bad[i], strlen(bad[i]), "bad", &s) == WPE_RESULT_ERROR_PARSE,
                            "%s", bad[i]);
    wpe_log_set_stderr_enabled(true);
}

/* ---- Game id ------------------------------------------------------------------------------- */

static void test_engine_network_game_id(void) {
    write_game(ROOT "/id_a", "return {}\n");
    write_game(ROOT "/id_b", "return {}\r\n"); /* Line endings only: the same game. */
    write_game(ROOT "/id_c", "return {x = 1}\n");
    WPE_Engine* a = start_engine(ROOT "/id_a", NULL);
    WPE_Engine* b = start_engine(ROOT "/id_b", NULL);
    WPE_Engine* c = start_engine(ROOT "/id_c", NULL);
    WPE_TEST_ASSERT(wpe_engine_game_id(a) != 0);
    WPE_TEST_ASSERT_EQ_UINT(wpe_engine_game_id(a), wpe_engine_game_id(b));
    WPE_TEST_ASSERT(wpe_engine_game_id(a) != wpe_engine_game_id(c));
    /* A file that is not game logic does not count. */
    write_text(ROOT "/id_b/notes.txt", "just notes");
    WPE_Engine* b2 = start_engine(ROOT "/id_b", NULL);
    WPE_TEST_ASSERT_EQ_UINT(wpe_engine_game_id(a), wpe_engine_game_id(b2));
    wpe_engine_destroy(b2);
    wpe_engine_destroy(c);
    wpe_engine_destroy(b);
    wpe_engine_destroy(a);
}

/* ---- Playing together ---------------------------------------------------------------------- */

static void test_engine_network_players_follow_the_host_scene(void) {
    write_game(ROOT "/game", "return {}\n");
    WPE_NetHub* hub = NULL;
    WPE_TEST_ASSERT_OK(wpe_network_hub_create(&hub));
    WPE_Engine* e[2] = {start_engine(ROOT "/game", NULL), start_engine(ROOT "/game", "scenes/menu.scene")};
    WPE_TEST_ASSERT_EQ_STR("scenes/a.scene", wpe_engine_scene_path(e[0]));
    WPE_TEST_ASSERT_NEAR(-3.0, gravity_of(e[1]), 1e-6); /* The player waits in its menu. */

    WPE_TEST_ASSERT_OK(wpe_engine_net_start(e[0], &(WPE_EngineNetDesc){.hub = hub, .player_name = "Mika"}));
    WPE_TEST_ASSERT(wpe_session_is_host(wpe_engine_session(e[0])));
    WPE_TEST_ASSERT_EQ_UINT(7100, wpe_network_local_port(wpe_session_transport(wpe_engine_session(e[0]))));
    WPE_TEST_ASSERT_OK(wpe_engine_net_start(
        e[1], &(WPE_EngineNetDesc){.hub = hub, .join = "127.0.0.1", .player_name = "Alice"}));
    play(hub, e, 2, 10);
    /* Joined, and on the host's scene. */
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_session_local_player(wpe_engine_session(e[1])));
    WPE_TEST_ASSERT_EQ_UINT(2, wpe_session_player_count(wpe_engine_session(e[0])));
    WPE_TEST_ASSERT_NEAR(-1.0, gravity_of(e[1]), 1e-6);
    WPE_TEST_ASSERT_EQ_STR("scenes/a.scene", wpe_engine_scene_path(e[1]));

    /* The host changes scene; the player follows. */
    WPE_TEST_ASSERT_OK(wpe_engine_load_scene(e[0], "scenes/b.scene"));
    play(hub, e, 2, 5);
    WPE_TEST_ASSERT_NEAR(-2.0, gravity_of(e[1]), 1e-6);

    /* The host leaves: the player's session ends, and it stays where it is. */
    wpe_engine_net_stop(e[0]);
    WPE_TEST_ASSERT(wpe_engine_session(e[0]) == NULL);
    play(hub, e, 2, 5);
    WPE_TEST_ASSERT_EQ_UINT(WPE_NET_REASON_CLOSED, wpe_session_end_reason(wpe_engine_session(e[1])));
    WPE_TEST_ASSERT_NEAR(-2.0, gravity_of(e[1]), 1e-6);
    wpe_engine_destroy(e[1]); /* Destroying stops the session too. */
    wpe_engine_destroy(e[0]);
    wpe_network_hub_destroy(hub);
}

static void test_engine_network_refuses_another_build(void) {
    write_game(ROOT "/v1", "return {version = 1}\n");
    write_game(ROOT "/v2", "return {version = 2}\n");
    WPE_NetHub* hub = NULL;
    WPE_TEST_ASSERT_OK(wpe_network_hub_create(&hub));
    WPE_Engine* e[2] = {start_engine(ROOT "/v1", NULL), start_engine(ROOT "/v2", "scenes/menu.scene")};
    WPE_TEST_ASSERT_OK(wpe_engine_net_start(e[0], &(WPE_EngineNetDesc){.hub = hub}));
    WPE_TEST_ASSERT_OK(wpe_engine_net_start(e[1], &(WPE_EngineNetDesc){.hub = hub, .join = "localhost"}));
    wpe_log_set_stderr_enabled(false); /* The refusal is logged as a warning. */
    play(hub, e, 2, 10);
    wpe_log_set_stderr_enabled(true);
    WPE_TEST_ASSERT_EQ_UINT(WPE_SESSION_REASON_WRONG_GAME, wpe_session_end_reason(wpe_engine_session(e[1])));
    WPE_TEST_ASSERT_NEAR(-3.0, gravity_of(e[1]), 1e-6); /* Still in its menu. */
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_session_player_count(wpe_engine_session(e[0])));
    wpe_engine_destroy(e[1]);
    wpe_engine_destroy(e[0]);
    wpe_network_hub_destroy(hub);
}

static void test_engine_network_lan_discovery(void) {
    write_game(ROOT "/lan", "return {}\n");
    WPE_NetHub* hub = NULL;
    WPE_TEST_ASSERT_OK(wpe_network_hub_create(&hub));
    WPE_Engine* e[2] = {start_engine(ROOT "/lan", NULL), NULL};
    WPE_TEST_ASSERT_OK(wpe_engine_net_start(e[0], &(WPE_EngineNetDesc){.hub = hub, .player_name = "Mika"}));
    /* A player looks for the game with the project's discovery port and the game's id. */
    WPE_DiscoveryBrowser* browser = NULL;
    WPE_TEST_ASSERT_OK(wpe_discovery_browser_create(
        &(WPE_DiscoveryDesc){.hub = hub, .port = 7101, .game_id = wpe_engine_game_id(e[0])}, &browser));
    for (int f = 0; f < 5; ++f) {
        wpe_discovery_browser_update(browser);
        play(hub, e, 1, 1);
    }
    WPE_DiscoveredGame game;
    WPE_TEST_ASSERT(wpe_discovery_browser_game(browser, 0, &game));
    WPE_TEST_ASSERT_EQ_STR("Net Test", game.info.game_name);
    WPE_TEST_ASSERT_EQ_STR("Mika", game.info.host_name);
    WPE_TEST_ASSERT_EQ_UINT(3, game.info.max_players);
    WPE_TEST_ASSERT_EQ_UINT(7100, game.address.port);
    /* A hidden host does not answer. */
    WPE_TEST_ASSERT_OK(wpe_engine_net_start(e[0], &(WPE_EngineNetDesc){.hub = hub, .hidden = true}));
    for (int f = 0; f < 300; ++f) {
        wpe_discovery_browser_update(browser);
        play(hub, e, 1, 1);
    }
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_discovery_browser_count(browser));
    wpe_discovery_browser_destroy(browser);
    wpe_engine_destroy(e[0]);
    wpe_network_hub_destroy(hub);
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_engine_network_project_settings), WPE_TEST_CASE(test_engine_network_game_id),
              WPE_TEST_CASE(test_engine_network_players_follow_the_host_scene),
              WPE_TEST_CASE(test_engine_network_refuses_another_build),
              WPE_TEST_CASE(test_engine_network_lan_discovery))
