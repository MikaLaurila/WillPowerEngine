#include <math.h>
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

/* ---- Replication --------------------------------------------------------------------------- */

/* Two cars without gravity: car1 (the host's) drives along +x at 5 m/s, car2 (player 1's) along -z
 * at 3 m/s, from their scene's initial velocities. */
static void write_race(const char* dir) {
    write_game(dir, "return {}\n");
    char path[256];
    snprintf(path, sizeof path, "%s/scenes/race.scene", dir);
    write_text(
        path,
        "{\"format\": \"wpe.scene\", \"version\": 1, \"environment\": {\"gravity\": [0, 0, 0]},\n"
        " \"entities\": [\n"
        "  {\"id\": 1, \"name\": \"car1\", \"transform\": {\"position\": [0, 0, 0]},\n"
        "   \"rigidbody\": {\"type\": \"dynamic\", \"initial_velocity\": [5, 0, 0]},\n"
        "   \"collider\": {\"shape\": \"sphere\", \"size\": [0.5, 0.5, 0.5]}, \"net\": {}},\n"
        "  {\"id\": 2, \"name\": \"car2\", \"transform\": {\"position\": [20, 0, 0]},\n"
        "   \"rigidbody\": {\"type\": \"dynamic\", \"initial_velocity\": [0, 0, -3]},\n"
        "   \"collider\": {\"shape\": \"sphere\", \"size\": [0.5, 0.5, 0.5]}, \"net\": {\"owner\": 1}},\n"
        "  {\"id\": 3, \"name\": \"scenery\", \"transform\": {\"position\": [0, -5, 0]}}]}\n");
}

static WPE_Entity find_named(const WPE_Engine* engine, const char* name) {
    WPE_EcsIter it =
        wpe_ecs_query(wpe_engine_ecs(engine), (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_NAME)});
    while (wpe_ecs_iter_next(&it)) {
        const char* n = wpe_scene_get_name(wpe_engine_ecs(engine), it.entity);
        if (n && strcmp(n, name) == 0) return it.entity;
    }
    WPE_TEST_ASSERT_MSG(false, "no entity named %s", name);
    return WPE_ENTITY_NULL;
}

static WPE_Vec3 position_of(const WPE_Engine* engine, const char* name) {
    const WPE_Transform* t =
        wpe_ecs_get_component(wpe_engine_ecs(engine), find_named(engine, name), WPE_COMP_TRANSFORM);
    WPE_TEST_ASSERT(t != NULL);
    return t->position;
}

static WPE_BodyType body_of(const WPE_Engine* engine, const char* name) {
    const WPE_RigidBody* b =
        wpe_ecs_get_component(wpe_engine_ecs(engine), find_named(engine, name), WPE_COMP_PHYSICS_RIGIDBODY);
    WPE_TEST_ASSERT(b != NULL);
    return b->type;
}

/* A host on race.scene and a player that joined it. */
static void start_race(WPE_NetHub* hub, WPE_Engine** e, const char* dir, WPE_NetConditions conditions) {
    e[0] = start_engine(dir, "scenes/race.scene");
    e[1] = start_engine(dir, "scenes/menu.scene");
    WPE_TEST_ASSERT_OK(
        wpe_engine_net_start(e[0], &(WPE_EngineNetDesc){.hub = hub, .conditions = conditions}));
    conditions.seed += 1;
    WPE_TEST_ASSERT_OK(wpe_engine_net_start(
        e[1], &(WPE_EngineNetDesc){.hub = hub, .join = "127.0.0.1", .conditions = conditions}));
    for (int f = 0; f < 200 && wpe_session_local_player(wpe_engine_session(e[1])) == WPE_SESSION_NO_PLAYER;
         ++f)
        play(hub, e, 2, 1);
    play(hub, e, 2, 3); /* The scene loads at the end of a frame. */
    WPE_TEST_ASSERT_EQ_STR("scenes/race.scene", wpe_engine_scene_path(e[1]));
}

/* Plays and checks that remote cars move smoothly: never backwards, never in jumps. */
static void play_smoothly(WPE_NetHub* hub, WPE_Engine** e, int frames) {
    float last_x = position_of(e[1], "car1").x;
    float last_z = position_of(e[0], "car2").z;
    for (int f = 0; f < frames; ++f) {
        play(hub, e, 2, 1);
        const float x = position_of(e[1], "car1").x; /* The host's car on the player's screen. */
        const float z = position_of(e[0], "car2").z; /* The player's car on the host's screen. */
        WPE_TEST_ASSERT_MSG(x >= last_x - 1e-4f && x - last_x < 0.3f, "frame %d: car1 x %.4f after %.4f", f,
                            x, last_x);
        WPE_TEST_ASSERT_MSG(z <= last_z + 1e-4f && last_z - z < 0.3f, "frame %d: car2 z %.4f after %.4f", f,
                            z, last_z);
        last_x = x;
        last_z = z;
    }
}

static void stop_cars(WPE_Engine** e) {
    WPE_TEST_ASSERT_OK(
        wpe_physics_set_velocity(wpe_engine_physics(e[0]), find_named(e[0], "car1"), wpe_vec3(0, 0, 0)));
    WPE_TEST_ASSERT_OK(
        wpe_physics_set_velocity(wpe_engine_physics(e[1]), find_named(e[1], "car2"), wpe_vec3(0, 0, 0)));
}

static void expect_same_cars(WPE_Engine** e) {
    for (int car = 1; car <= 2; ++car) {
        const char* name = car == 1 ? "car1" : "car2";
        const WPE_Vec3 a = position_of(e[0], name);
        const WPE_Vec3 b = position_of(e[1], name);
        WPE_TEST_ASSERT_MSG(fabsf(a.x - b.x) < 1e-4f && fabsf(a.y - b.y) < 1e-4f && fabsf(a.z - b.z) < 1e-4f,
                            "%s: host (%.4f %.4f %.4f), player (%.4f %.4f %.4f)", name, a.x, a.y, a.z, b.x,
                            b.y, b.z);
    }
}

static void test_engine_network_replicates_owned_entities(void) {
    write_race(ROOT "/race");
    WPE_NetHub* hub = NULL;
    WPE_TEST_ASSERT_OK(wpe_network_hub_create(&hub));
    WPE_Engine* e[2] = {0};
    start_race(hub, e, ROOT "/race", (WPE_NetConditions){0});
    /* Each side simulates its own car; the other's is kinematic, following what arrives. */
    WPE_TEST_ASSERT(wpe_engine_net_is_local(e[0], find_named(e[0], "car1")));
    WPE_TEST_ASSERT(!wpe_engine_net_is_local(e[0], find_named(e[0], "car2")));
    WPE_TEST_ASSERT(wpe_engine_net_is_local(e[1], find_named(e[1], "car2")));
    WPE_TEST_ASSERT(wpe_engine_net_is_local(e[1], find_named(e[1], "scenery"))); /* Not networked. */
    play_smoothly(hub, e, 60);
    WPE_TEST_ASSERT_EQ_INT(WPE_BODY_DYNAMIC, body_of(e[0], "car1"));
    WPE_TEST_ASSERT_EQ_INT(WPE_BODY_KINEMATIC, body_of(e[0], "car2"));
    WPE_TEST_ASSERT_EQ_INT(WPE_BODY_KINEMATIC, body_of(e[1], "car1"));
    WPE_TEST_ASSERT_EQ_INT(WPE_BODY_DYNAMIC, body_of(e[1], "car2"));
    /* Remote cars run about the interpolation delay (100 ms) behind: 0.5 m for car1, 0.3 m for car2. */
    const float lag1 = position_of(e[0], "car1").x - position_of(e[1], "car1").x;
    const float lag2 = position_of(e[1], "car2").z - position_of(e[0], "car2").z;
    WPE_TEST_ASSERT_MSG(lag1 > 0.3f && lag1 < 1.0f, "car1 lags %.3f m", lag1);
    WPE_TEST_ASSERT_MSG(lag2 < -0.15f && lag2 > -0.6f, "car2 lags %.3f m", lag2);
    /* Stopped, both screens agree exactly (positions are sent as floats). */
    stop_cars(e);
    play(hub, e, 2, 30);
    expect_same_cars(e);
    WPE_TEST_ASSERT(position_of(e[0], "car1").x > 4.0f);

    /* The player may not take the host's car: its states for it are ignored. */
    const WPE_Vec3 before = position_of(e[0], "car1");
    uint8_t forged[64];
    WPE_BitWriter w = wpe_bits_writer(forged, sizeof forged);
    wpe_bits_write_varint(&w, wpe_session_scene_epoch(wpe_engine_session(e[1])));
    wpe_bits_write(&w, 1000000, 32);
    wpe_bits_write_varint(&w, 1);
    wpe_bits_write_varint(&w, 1); /* car1 */
    for (int k = 0; k < 3; ++k)
        wpe_bits_write_float(&w, 999.0f);
    wpe_bits_write_quat(&w, (WPE_Quat){.w = 1}, 15);
    wpe_bits_write_vec3(&w, wpe_vec3(0, 0, 0), -256.0f, 256.0f, 16);
    for (int i = 0; i < 5; ++i)
        WPE_TEST_ASSERT_OK(wpe_session_send(wpe_engine_session(e[1]), WPE_SESSION_HOST,
                                            WPE_NET_CHANNEL_UNRELIABLE, WPE_REPLICATION_KIND_STATE, forged,
                                            wpe_bits_writer_size(&w)));
    play(hub, e, 2, 10);
    WPE_TEST_ASSERT(fabsf(position_of(e[0], "car1").x - before.x) < 1e-4f);
    expect_same_cars(e);
    wpe_engine_destroy(e[1]);
    wpe_engine_destroy(e[0]);
    wpe_network_hub_destroy(hub);
}

static void test_engine_network_replicates_over_a_bad_network(void) {
    write_race(ROOT "/bad");
    WPE_NetHub* hub = NULL;
    WPE_TEST_ASSERT_OK(wpe_network_hub_create(&hub));
    WPE_Engine* e[2] = {0};
    /* 60 ms each way, up to 20 ms of jitter and 10% loss. Interpolation (100 ms) hides it. */
    start_race(hub, e, ROOT "/bad",
               (WPE_NetConditions){.latency_ms = 60, .jitter_ms = 20, .loss = 0.1f, .seed = 3});
    play(hub, e, 2, 20);
    play_smoothly(hub, e, 120);
    stop_cars(e);
    play(hub, e, 2, 60);
    expect_same_cars(e);
    wpe_engine_destroy(e[1]);
    wpe_engine_destroy(e[0]);
    wpe_network_hub_destroy(hub);
}

static void test_engine_network_ownership_moves(void) {
    write_race(ROOT "/owners");
    WPE_NetHub* hub = NULL;
    WPE_TEST_ASSERT_OK(wpe_network_hub_create(&hub));
    WPE_Engine* e[3] = {0};
    start_race(hub, e, ROOT "/owners", (WPE_NetConditions){0});
    stop_cars(e);
    play(hub, e, 2, 20);
    /* Only the host decides. */
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_STATE,
                           wpe_engine_net_set_owner(e[1], find_named(e[1], "car1"), 1));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG,
                           wpe_engine_net_set_owner(e[0], find_named(e[0], "car1"), 2));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND,
                           wpe_engine_net_set_owner(e[0], find_named(e[0], "scenery"), 1));

    /* The host gives car1 to the player, who now drives it; the host's copy follows. */
    WPE_TEST_ASSERT_OK(wpe_engine_net_set_owner(e[0], find_named(e[0], "car1"), 1));
    play(hub, e, 2, 3);
    WPE_TEST_ASSERT(wpe_engine_net_is_local(e[1], find_named(e[1], "car1")));
    WPE_TEST_ASSERT_EQ_INT(WPE_BODY_DYNAMIC, body_of(e[1], "car1"));
    WPE_TEST_ASSERT_EQ_INT(WPE_BODY_KINEMATIC, body_of(e[0], "car1"));
    WPE_TEST_ASSERT_OK(
        wpe_physics_set_velocity(wpe_engine_physics(e[1]), find_named(e[1], "car1"), wpe_vec3(0, 2, 0)));
    play(hub, e, 2, 30);
    WPE_TEST_ASSERT(position_of(e[0], "car1").y > 0.5f);

    /* A second player joins later and learns who owns what. */
    e[2] = start_engine(ROOT "/owners", "scenes/menu.scene");
    WPE_TEST_ASSERT_OK(wpe_engine_net_start(e[2], &(WPE_EngineNetDesc){.hub = hub, .join = "127.0.0.1"}));
    play(hub, e, 3, 20);
    WPE_TEST_ASSERT_EQ_UINT(2, wpe_session_local_player(wpe_engine_session(e[2])));
    WPE_TEST_ASSERT(!wpe_engine_net_is_local(e[2], find_named(e[2], "car1")));
    WPE_TEST_ASSERT_EQ_INT(WPE_BODY_KINEMATIC, body_of(e[2], "car1"));
    WPE_TEST_ASSERT_OK(
        wpe_physics_set_velocity(wpe_engine_physics(e[1]), find_named(e[1], "car1"), wpe_vec3(0, 0, 0)));
    play(hub, e, 3, 30);
    const WPE_Vec3 owner_view = position_of(e[1], "car1");
    const WPE_Vec3 third_view = position_of(e[2], "car1"); /* Relayed through the host. */
    WPE_TEST_ASSERT(fabsf(owner_view.y - third_view.y) < 1e-4f && owner_view.y > 0.5f);

    /* The player leaves: its cars go back to the host, dynamic again. */
    wpe_engine_destroy(e[1]);
    e[1] = NULL;
    play(hub, e, 3, 10);
    WPE_TEST_ASSERT(wpe_engine_net_is_local(e[0], find_named(e[0], "car1")));
    WPE_TEST_ASSERT(wpe_engine_net_is_local(e[0], find_named(e[0], "car2")));
    WPE_TEST_ASSERT_EQ_INT(WPE_BODY_DYNAMIC, body_of(e[0], "car1"));

    /* A new scene: ids start over and owners come from the file again. */
    WPE_TEST_ASSERT_OK(wpe_engine_load_scene(e[0], "scenes/race.scene"));
    play(hub, e, 3, 10);
    WPE_TEST_ASSERT(!wpe_engine_net_is_local(e[2], find_named(e[2], "car1")));
    WPE_TEST_ASSERT(
        !wpe_engine_net_is_local(e[2], find_named(e[2], "car2"))); /* Player 1's: nobody drives it. */
    play(hub, e, 3, 30);
    const WPE_Vec3 host_car1 = position_of(e[0], "car1");
    const WPE_Vec3 seen_car1 = position_of(e[2], "car1");
    WPE_TEST_ASSERT(host_car1.x > 1.0f &&
                    fabsf(host_car1.x - seen_car1.x) < 1.0f); /* Driving again, followed. */
    wpe_engine_destroy(e[2]);
    wpe_engine_destroy(e[0]);
    wpe_network_hub_destroy(hub);
}

/* A state for car2 at x = 999, as player `forger` would send it. */
static size_t forge_car2_state(const WPE_Engine* forger, uint8_t* out, size_t capacity, uint32_t time) {
    WPE_BitWriter w = wpe_bits_writer(out, capacity);
    wpe_bits_write_varint(&w, wpe_session_scene_epoch(wpe_engine_session(forger)));
    wpe_bits_write(&w, time, 32);
    wpe_bits_write_varint(&w, 1);
    wpe_bits_write_varint(&w, 2); /* car2 */
    for (int k = 0; k < 3; ++k)
        wpe_bits_write_float(&w, 999.0f);
    wpe_bits_write_quat(&w, (WPE_Quat){.w = 1}, 15);
    wpe_bits_write_vec3(&w, wpe_vec3(0, 0, 0), -256.0f, 256.0f, 16);
    WPE_TEST_ASSERT(!w.error);
    return wpe_bits_writer_size(&w);
}

static void test_engine_network_ignores_forged_states(void) {
    write_race(ROOT "/forged");
    WPE_NetHub* hub = NULL;
    WPE_TEST_ASSERT_OK(wpe_network_hub_create(&hub));
    WPE_Engine* e[3] = {0};
    start_race(hub, e, ROOT "/forged", (WPE_NetConditions){0});
    e[2] = start_engine(ROOT "/forged", "scenes/menu.scene");
    WPE_TEST_ASSERT_OK(wpe_engine_net_start(e[2], &(WPE_EngineNetDesc){.hub = hub, .join = "127.0.0.1"}));
    play(hub, e, 3, 30);
    WPE_TEST_ASSERT_EQ_UINT(2, wpe_session_local_player(wpe_engine_session(e[2])));
    /* Player 1 (car2's owner) goes quiet for a moment, and player 2 sends car2 states of its own: the
     * newest the host has, so a host that took them would show them (and relay them). */
    WPE_Engine* others[3] = {e[0], NULL, e[2]};
    for (int i = 0; i < 30; ++i) {
        uint8_t forged[64];
        const size_t size = forge_car2_state(e[2], forged, sizeof forged, 1000000u + (uint32_t)i * 16u);
        WPE_TEST_ASSERT_OK(wpe_session_send(wpe_engine_session(e[2]), WPE_SESSION_HOST,
                                            WPE_NET_CHANNEL_UNRELIABLE, WPE_REPLICATION_KIND_STATE, forged,
                                            size));
        play(hub, others, 3, 1);
        WPE_TEST_ASSERT_MSG(position_of(e[0], "car2").x < 100.0f && position_of(e[2], "car2").x < 100.0f,
                            "frame %d: a forged state was shown", i);
    }
    wpe_engine_destroy(e[2]);
    wpe_engine_destroy(e[1]);
    wpe_engine_destroy(e[0]);
    wpe_network_hub_destroy(hub);
}

static void test_engine_network_survives_garbage_replication(void) {
    write_race(ROOT "/garbage");
    WPE_NetHub* hub = NULL;
    WPE_TEST_ASSERT_OK(wpe_network_hub_create(&hub));
    WPE_Engine* e[2] = {0};
    start_race(hub, e, ROOT "/garbage", (WPE_NetConditions){0});
    uint64_t rng = 9;
    for (int round = 0; round < 300; ++round) {
        uint8_t junk[96];
        rng = rng * 6364136223846793005ull + 1442695040888963407ull;
        const size_t size = 1 + (size_t)(rng >> 33) % sizeof junk;
        for (size_t i = 0; i < size; ++i) {
            rng = rng * 6364136223846793005ull + 1442695040888963407ull;
            junk[i] = (uint8_t)(rng >> 40);
        }
        if (round % 4 == 0)
            junk[0] = (uint8_t)wpe_session_scene_epoch(wpe_engine_session(e[0])); /* Right epoch. */
        const uint32_t kind = round % 2 ? WPE_REPLICATION_KIND_STATE : WPE_REPLICATION_KIND_OWNERS;
        WPE_TEST_ASSERT_OK(
            wpe_session_send(wpe_engine_session(e[0]), 1, WPE_NET_CHANNEL_RELIABLE, kind, junk, size));
        WPE_TEST_ASSERT_OK(wpe_session_send(wpe_engine_session(e[1]), WPE_SESSION_HOST,
                                            WPE_NET_CHANNEL_RELIABLE, kind, junk, size));
        play(hub, e, 2, 1);
    }
    for (int i = 0; i < 2; ++i) {
        for (int car = 1; car <= 2; ++car) {
            const WPE_Vec3 p = position_of(e[i], car == 1 ? "car1" : "car2");
            WPE_TEST_ASSERT(isfinite(p.x) && isfinite(p.y) && isfinite(p.z));
        }
    }
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_session_local_player(wpe_engine_session(e[1]))); /* Still playing. */
    wpe_engine_destroy(e[1]);
    wpe_engine_destroy(e[0]);
    wpe_network_hub_destroy(hub);
}

/* ---- Scripts ------------------------------------------------------------------------------- */

/* A lobby whose script logs every network callback into a shared module (scripts/netlog.luau), and
 * answers: the host welcomes joiners with a table, joiners thank everyone. */
static void write_script_game(const char* dir) {
    write_game(dir, "return {}\n");
    char path[256];
    snprintf(path, sizeof path, "%s/scenes/lobby.scene", dir);
    write_text(path,
               "{\"format\": \"wpe.scene\", \"version\": 1, \"entities\": [\n"
               "  {\"id\": 1, \"name\": \"game\", \"script\": {\"path\": \"scripts/netgame.luau\"}}]}\n");
    snprintf(path, sizeof path, "%s/scripts/netlog.luau", dir);
    write_text(path, "return {}\n");
    snprintf(path, sizeof path, "%s/scripts/netgame.luau", dir);
    write_text(
        path,
        "local Log = require(\"scripts/netlog\")\n"
        "local G = {}\n"
        "local function note(text) table.insert(Log, text) end\n"
        "function G:on_net_joined(id) note(\"joined \" .. id .. \" \" .. wpe.net_status()) end\n"
        "function G:on_player_joined(id, name)\n"
        "  note(\"player \" .. id .. \" \" .. name)\n"
        "  if wpe.net_is_host() then\n"
        "    assert(wpe.net_send(id, \"welcome\", {text = \"hi \" .. name, list = {1, 2, 3},\n"
        "      at = vector.create(1, 2, 3), deep = {a = {b = true}}, [7] = -0.5}))\n"
        "  end\n"
        "end\n"
        "function G:on_player_left(id, name, reason) note(\"left \" .. id .. \" \" .. name .. \" \" .. "
        "reason) end\n"
        "function G:on_net_ended(reason) note(\"ended \" .. reason) end\n"
        "function G:on_net_message(from, name, value)\n"
        "  if name == \"welcome\" then\n"
        "    assert(value.text:sub(1, 3) == \"hi \" and #value.list == 3 and value.list[3] == 3)\n"
        "    assert(value.at == vector.create(1, 2, 3) and value.deep.a.b == true and value[7] == -0.5)\n"
        "    note(\"welcome from \" .. from)\n"
        "    assert(wpe.net_send(\"all\", \"thanks\", 42))\n"
        "  else\n"
        "    note(name .. \" \" .. tostring(value) .. \" from \" .. from)\n"
        "  end\n"
        "end\n"
        "return G\n");
}

/* Runs Luau in an engine's VM (where `require` reaches the log module). */
static void run_luau(WPE_Engine* engine, const char* source) {
    WPE_TEST_ASSERT_MSG(wpe_script_host_run(wpe_engine_scripts(engine), "test", source) == WPE_RESULT_OK,
                        "Luau failed: %s", source);
}

/* Asserts an engine's log is exactly `expected` (entries joined with "; "), printing it otherwise. */
static void expect_log(WPE_Engine* engine, const char* expected) {
    char source[1024];
    snprintf(source, sizeof source,
             "local got = table.concat(require(\"scripts/netlog\"), \"; \")\n"
             "if got ~= \"%s\" then wpe.log(\"log: \" .. got) end\n"
             "assert(got == \"%s\")\n",
             expected, expected);
    run_luau(engine, source);
}

static void test_engine_network_scripts_play_together(void) {
    write_script_game(ROOT "/scripted");
    WPE_NetHub* hub = NULL;
    WPE_TEST_ASSERT_OK(wpe_network_hub_create(&hub));
    WPE_Engine* e[3] = {0};
    for (int i = 0; i < 3; ++i) {
        e[i] = start_engine(ROOT "/scripted", "scenes/lobby.scene");
        wpe_engine_set_net_defaults(e[i], &(WPE_EngineNetDesc){.hub = hub});
    }
    play(hub, e, 3, 1); /* Scripts start. */
    run_luau(
        e[0],
        "assert(wpe.net_host({name = 'Mika'}))\nassert(wpe.net_status() == 'hosting' and wpe.net_is_host())");
    run_luau(e[1],
             "assert(wpe.net_join('127.0.0.1', {name = 'Alice'}))\nassert(wpe.net_status() == 'connecting')");
    play(hub, e, 3, 10);
    expect_log(e[0], "joined 0 hosting; player 1 Alice; thanks 42 from 1");
    /* Alice followed the host into the lobby again (a new script instance), and heard all of it. */
    expect_log(e[1], "joined 1 joined; player 0 Mika; welcome from 0");
    run_luau(e[1], "assert(wpe.net_player() == 1 and not wpe.net_is_host() and #wpe.net_players() == 2)\n"
                   "local p = wpe.net_players()\n"
                   "assert(p[1].id == 0 and p[1].name == 'Mika' and p[2].name == 'Alice')");

    /* Bob finds the game on the LAN and joins it from the list. */
    for (int f = 0; f < 10; ++f) {
        run_luau(
            e[2],
            "local games = wpe.net_find_games()\n"
            "if #games > 0 and wpe.net_status() == 'offline' then\n"
            "  assert(games[1].name == 'Net Test' and games[1].host == 'Mika' and games[1].players == 2)\n"
            "  assert(wpe.net_join(games[1].address, {name = 'Bob'}))\n"
            "end");
        play(hub, e, 3, 1);
    }
    run_luau(e[2], "wpe.net_stop_finding()");
    play(hub, e, 3, 10);
    /* Bob's thanks went to everyone, through the host, signed with his id. */
    expect_log(e[0], "joined 0 hosting; player 1 Alice; thanks 42 from 1; player 2 Bob; thanks 42 from 2");
    expect_log(e[1], "joined 1 joined; player 0 Mika; welcome from 0; player 2 Bob; thanks 42 from 2");
    expect_log(e[2], "joined 2 joined; player 0 Mika; player 1 Alice; welcome from 0");

    /* Unreliable messages and messages between two players (through the host). */
    run_luau(e[1],
             "assert(wpe.net_send(2, 'psst', 'secret'))\nassert(wpe.net_send('host', 'pos', 5, false))\n"
             "assert(not wpe.net_send(1, 'self', 0)) -- not to yourself");
    play(hub, e, 3, 5);
    expect_log(
        e[0],
        "joined 0 hosting; player 1 Alice; thanks 42 from 1; player 2 Bob; thanks 42 from 2; pos 5 from 1");
    expect_log(e[2], "joined 2 joined; player 0 Mika; player 1 Alice; welcome from 0; psst secret from 1");

    /* The host kicks Bob, then leaves. */
    run_luau(e[0], "assert(wpe.net_kick(2))");
    play(hub, e, 3, 5);
    expect_log(
        e[2],
        "joined 2 joined; player 0 Mika; player 1 Alice; welcome from 0; psst secret from 1; ended kicked");
    run_luau(e[2],
             "local status, reason = wpe.net_status()\nassert(status == 'ended' and reason == 'kicked')");
    expect_log(e[1], "joined 1 joined; player 0 Mika; welcome from 0; player 2 Bob; thanks 42 from 2; "
                     "left 2 Bob kicked");
    run_luau(e[0], "wpe.net_leave()\nassert(wpe.net_status() == 'offline')");
    play(hub, e, 3, 5);
    expect_log(e[1], "joined 1 joined; player 0 Mika; welcome from 0; player 2 Bob; thanks 42 from 2; "
                     "left 2 Bob kicked; ended closed");
    for (int i = 2; i >= 0; --i)
        wpe_engine_destroy(e[i]);
    wpe_network_hub_destroy(hub);
}

static void test_engine_network_scene_files_keep_owners(void) {
    write_race(ROOT "/files");
    WPE_Engine* engine = start_engine(ROOT "/files", "scenes/race.scene");
    const WPE_NetIdentity* net =
        wpe_ecs_get_component(wpe_engine_ecs(engine), find_named(engine, "car2"), WPE_COMP_NET_IDENTITY);
    WPE_TEST_ASSERT(net != NULL);
    WPE_TEST_ASSERT_EQ_UINT(1, net->owner);
    WPE_TEST_ASSERT_EQ_UINT(2, net->net_id); /* Spawn order. */
    WPE_TEST_ASSERT_OK(wpe_engine_save_scene(engine, ROOT "/files/scenes/saved.scene"));
    WPE_TEST_ASSERT_OK(wpe_engine_load_scene(engine, "scenes/saved.scene"));
    net = wpe_ecs_get_component(wpe_engine_ecs(engine), find_named(engine, "car2"), WPE_COMP_NET_IDENTITY);
    WPE_TEST_ASSERT(net && net->owner == 1);
    WPE_TEST_ASSERT(
        wpe_ecs_has_component(wpe_engine_ecs(engine), find_named(engine, "car1"), WPE_COMP_NET_IDENTITY));
    wpe_log_set_stderr_enabled(false);
    const char* bad =
        "{\"format\": \"wpe.scene\", \"version\": 1, \"entities\": [{\"id\": 1, \"net\": {\"owner\": 8}}]}";
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_PARSE,
                           wpe_engine_load_scene_text(engine, bad, strlen(bad), "bad"));
    wpe_log_set_stderr_enabled(true);
    wpe_engine_destroy(engine);
}

/* ---- Tools: the editor's way of hosting, and the overlay ------------------------------------- */

/* A copy of the framebuffer's colors. */
static uint32_t* grab(const WPE_Engine* engine, size_t* out_count) {
    const WPE_Framebuffer* fb = wpe_renderer_framebuffer(wpe_engine_renderer(engine));
    *out_count = (size_t)fb->width * fb->height;
    uint32_t* copy = wpe_alloc(*out_count * sizeof *copy);
    WPE_TEST_ASSERT(copy != NULL);
    memcpy(copy, fb->color, *out_count * sizeof *copy);
    return copy;
}

static void test_engine_network_hosts_like_the_editor(void) {
    write_game(ROOT "/tools", "return {}\n");
    WPE_NetHub* hub = NULL;
    WPE_TEST_ASSERT_OK(wpe_network_hub_create(&hub));
    WPE_Engine* e[2] = {start_engine(ROOT "/tools", NULL), start_engine(ROOT "/tools", NULL)};
    /* The editor's world comes from text (its snapshot of the saved file), so it names the file. */
    const char* b =
        "{\"format\": \"wpe.scene\", \"version\": 1, \"environment\": {\"gravity\": [0, -2, 0]}}\n";
    WPE_TEST_ASSERT_OK(wpe_engine_load_scene_text(e[0], b, strlen(b), "snapshot"));
    WPE_TEST_ASSERT_EQ_STR("", wpe_engine_scene_path(e[0]));
    WPE_TEST_ASSERT_OK(wpe_engine_net_start(
        e[0], &(WPE_EngineNetDesc){.hub = hub, .any_port = true, .scene = "scenes/b.scene"}));
    const uint16_t port = wpe_network_local_port(wpe_session_transport(wpe_engine_session(e[0])));
    WPE_TEST_ASSERT(port != 7100); /* Any free port, not the project's. */
    char address[32];
    snprintf(address, sizeof address, "127.0.0.1:%u", (unsigned)port);
    WPE_TEST_ASSERT_OK(wpe_engine_net_start(e[1], &(WPE_EngineNetDesc){.hub = hub, .join = address}));
    play(hub, e, 2, 10);
    WPE_TEST_ASSERT_EQ_STR("scenes/b.scene", wpe_engine_scene_path(e[1]));
    WPE_TEST_ASSERT_NEAR(-2.0, gravity_of(e[1]), 1e-6);

    /* The overlay draws only while asked to and a game runs. */
    size_t count = 0;
    uint32_t* plain = grab(e[1], &count);
    WPE_EngineDebugDraw debug = wpe_engine_debug_draw(e[1]);
    debug.network = true;
    wpe_engine_set_debug_draw(e[1], &debug);
    play(hub, e, 2, 1);
    uint32_t* shown = grab(e[1], &count);
    WPE_TEST_ASSERT(memcmp(plain, shown, count * sizeof *plain) != 0);
    wpe_engine_net_stop(e[1]);
    play(hub, e, 2, 1);
    uint32_t* alone = grab(e[1], &count);
    WPE_TEST_ASSERT(memcmp(plain, alone, count * sizeof *plain) == 0);
    wpe_free(alone);
    wpe_free(shown);
    wpe_free(plain);

    /* A game file edited between games (in the editor) changes the id the next game uses. */
    const uint64_t before = wpe_engine_game_id(e[0]);
    write_text(ROOT "/tools/scripts/rules.luau", "return {edited = true}\n");
    WPE_TEST_ASSERT_EQ_UINT(before, wpe_engine_game_id(e[0])); /* Cached while the game runs. */
    WPE_TEST_ASSERT_OK(wpe_engine_net_start(e[0], &(WPE_EngineNetDesc){.hub = hub}));
    WPE_TEST_ASSERT(wpe_engine_game_id(e[0]) != before);
    wpe_engine_destroy(e[1]);
    wpe_engine_destroy(e[0]);
    wpe_network_hub_destroy(hub);
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_engine_network_project_settings), WPE_TEST_CASE(test_engine_network_game_id),
              WPE_TEST_CASE(test_engine_network_hosts_like_the_editor),
              WPE_TEST_CASE(test_engine_network_players_follow_the_host_scene),
              WPE_TEST_CASE(test_engine_network_refuses_another_build),
              WPE_TEST_CASE(test_engine_network_lan_discovery),
              WPE_TEST_CASE(test_engine_network_replicates_owned_entities),
              WPE_TEST_CASE(test_engine_network_replicates_over_a_bad_network),
              WPE_TEST_CASE(test_engine_network_ownership_moves),
              WPE_TEST_CASE(test_engine_network_ignores_forged_states),
              WPE_TEST_CASE(test_engine_network_scripts_play_together),
              WPE_TEST_CASE(test_engine_network_survives_garbage_replication),
              WPE_TEST_CASE(test_engine_network_scene_files_keep_owners))
