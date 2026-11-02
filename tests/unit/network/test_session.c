#include <stdio.h>
#include <time.h>

#include "network/network.h"
#include "wpe_test.h"

#define GAME_ID 0x1234567890ABCDEFull

/* ---- Helpers --------------------------------------------------------------------------------- */

typedef struct Seen {
    uint64_t time;
    int who;
    WPE_SessionEventType type;
    WPE_PlayerId player;
    uint32_t reason;
    uint32_t kind;
    char text[64];
    uint8_t first_byte;
    size_t size;
} Seen;

typedef struct Log {
    Seen events[512];
    size_t count;
} Log;

static void drain(WPE_Session** sessions, int count, uint64_t time, Log* log) {
    for (int who = 0; who < count; ++who) {
        if (!sessions[who]) continue;
        WPE_SessionEvent e;
        while (wpe_session_poll(sessions[who], &e)) {
            WPE_TEST_ASSERT(log->count < sizeof log->events / sizeof log->events[0]);
            Seen* s = &log->events[log->count++];
            *s = (Seen){.time = time,
                        .who = who,
                        .type = e.type,
                        .player = e.player,
                        .reason = e.reason,
                        .kind = e.kind,
                        .first_byte = e.size ? e.data[0] : 0,
                        .size = e.size};
            WPE_TEST_ASSERT(e.text != NULL);
            snprintf(s->text, sizeof s->text, "%s", e.text);
        }
    }
}

static void run(WPE_NetHub* hub, WPE_Session** sessions, int count, uint32_t ms, Log* log) {
    for (uint32_t t = 0; t < ms; t += 10) {
        wpe_network_hub_advance(hub, 10);
        drain(sessions, count, wpe_network_hub_now(hub), log);
    }
}

static const Seen* find(const Log* log, int who, WPE_SessionEventType type, WPE_PlayerId player) {
    for (size_t i = 0; i < log->count; ++i) {
        const Seen* s = &log->events[i];
        if (s->who == who && s->type == type && (player == WPE_SESSION_NO_PLAYER || s->player == player))
            return s;
    }
    return NULL;
}

static const Seen* expect(const Log* log, int who, WPE_SessionEventType type, WPE_PlayerId player) {
    const Seen* s = find(log, who, type, player);
    WPE_TEST_ASSERT_MSG(s != NULL, "session %d: no event %d for player %u", who, (int)type, player);
    return s;
}

/* Descriptions in static storage (each call replaces the last one's). */
static const WPE_SessionDesc* host_desc(WPE_NetHub* hub, uint32_t max_players, const char* name) {
    static WPE_SessionDesc desc;
    desc = (WPE_SessionDesc){.transport = {.hub = hub, .port = 7000, .timeout_ms = 1000},
                             .max_players = max_players,
                             .game_id = GAME_ID,
                             .player_name = name,
                             .game_name = "Test Racer"};
    return &desc;
}

static const WPE_SessionDesc* join_desc(WPE_NetHub* hub, const char* name) {
    static WPE_SessionDesc desc;
    desc = (WPE_SessionDesc){
        .transport = {.hub = hub, .timeout_ms = 1000}, .game_id = GAME_ID, .player_name = name};
    return &desc;
}

static const WPE_NetAddress HOST_ADDRESS = {.ipv4 = 0x7F000001u, .port = 7000};

/* ---- Joining --------------------------------------------------------------------------------- */

static void test_session_join_and_talk(void) {
    static Log log;
    log.count = 0;
    WPE_NetHub* hub = NULL;
    WPE_TEST_ASSERT_OK(wpe_network_hub_create(&hub));
    WPE_Session* s[3] = {0};
    WPE_TEST_ASSERT_OK(wpe_session_host(&(WPE_SessionDesc){.transport = {.hub = hub, .port = 7000},
                                                           .max_players = 3,
                                                           .game_id = GAME_ID,
                                                           .player_name = "Mika"},
                                        &s[0]));
    WPE_TEST_ASSERT(wpe_session_is_host(s[0]));
    WPE_TEST_ASSERT_EQ_UINT(WPE_SESSION_HOST, wpe_session_local_player(s[0]));
    WPE_TEST_ASSERT_OK(wpe_session_join(join_desc(hub, "Alice"), HOST_ADDRESS, &s[1]));
    WPE_TEST_ASSERT_EQ_UINT(WPE_SESSION_NO_PLAYER, wpe_session_local_player(s[1]));
    run(hub, s, 3, 200, &log);
    WPE_TEST_ASSERT_OK(wpe_session_join(join_desc(hub, "Bob"), HOST_ADDRESS, &s[2]));
    run(hub, s, 3, 200, &log);

    /* The host: itself, then each joiner. */
    WPE_TEST_ASSERT_EQ_STR("Mika", expect(&log, 0, WPE_SESSION_EVENT_JOINED, 0)->text);
    WPE_TEST_ASSERT_EQ_STR("Alice", expect(&log, 0, WPE_SESSION_EVENT_PLAYER_JOINED, 1)->text);
    WPE_TEST_ASSERT_EQ_STR("Bob", expect(&log, 0, WPE_SESSION_EVENT_PLAYER_JOINED, 2)->text);
    /* Alice: her id, the host, then Bob joining later. Bob: everyone already there. */
    expect(&log, 1, WPE_SESSION_EVENT_JOINED, 1);
    WPE_TEST_ASSERT_EQ_STR("Mika", expect(&log, 1, WPE_SESSION_EVENT_PLAYER_JOINED, 0)->text);
    WPE_TEST_ASSERT_EQ_STR("Bob", expect(&log, 1, WPE_SESSION_EVENT_PLAYER_JOINED, 2)->text);
    expect(&log, 2, WPE_SESSION_EVENT_JOINED, 2);
    WPE_TEST_ASSERT_EQ_STR("Alice", expect(&log, 2, WPE_SESSION_EVENT_PLAYER_JOINED, 1)->text);
    WPE_TEST_ASSERT(!find(&log, 1, WPE_SESSION_EVENT_SCENE, WPE_SESSION_NO_PLAYER)); /* No scene yet. */
    for (int i = 0; i < 3; ++i)
        WPE_TEST_ASSERT_EQ_UINT(3, wpe_session_player_count(s[i]));
    WPE_SessionPlayer player;
    WPE_TEST_ASSERT(wpe_session_player(s[2], 1, &player));
    WPE_TEST_ASSERT_EQ_STR("Alice", player.name);
    WPE_TEST_ASSERT(!wpe_session_player(s[2], 3, &player));

    /* Messages: a player to the host, the host to one player and to all. */
    log.count = 0;
    const uint8_t hi[] = {42, 1, 2};
    WPE_TEST_ASSERT_OK(wpe_session_send(s[1], WPE_SESSION_HOST, WPE_NET_CHANNEL_RELIABLE, 20, hi, sizeof hi));
    WPE_TEST_ASSERT_OK(wpe_session_send(s[0], 2, WPE_NET_CHANNEL_UNRELIABLE, 21, hi, 1));
    WPE_TEST_ASSERT_OK(wpe_session_broadcast(s[0], WPE_NET_CHANNEL_RELIABLE, 255, NULL, 0));
    run(hub, s, 3, 100, &log);
    const Seen* got = expect(&log, 0, WPE_SESSION_EVENT_MESSAGE, 1);
    WPE_TEST_ASSERT_EQ_UINT(20, got->kind);
    WPE_TEST_ASSERT_EQ_UINT(3, got->size);
    WPE_TEST_ASSERT_EQ_UINT(42, got->first_byte);
    got = expect(&log, 2, WPE_SESSION_EVENT_MESSAGE, WPE_SESSION_HOST);
    WPE_TEST_ASSERT_EQ_UINT(21, got->kind);
    size_t empty_messages = 0;
    for (size_t i = 0; i < log.count; ++i)
        empty_messages += log.events[i].kind == 255 && log.events[i].size == 0;
    WPE_TEST_ASSERT_EQ_UINT(2, empty_messages);

    /* What may not be sent. */
    wpe_log_set_stderr_enabled(false);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG,
                           wpe_session_send(s[1], WPE_SESSION_HOST, WPE_NET_CHANNEL_RELIABLE, 5, hi, 1));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG,
                           wpe_session_send(s[1], WPE_SESSION_HOST, WPE_NET_CHANNEL_RELIABLE, 256, hi, 1));
    wpe_log_set_stderr_enabled(true);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_STATE,
                           wpe_session_send(s[1], 2, WPE_NET_CHANNEL_RELIABLE, 20, hi, 1));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_STATE,
                           wpe_session_send(s[0], 0, WPE_NET_CHANNEL_RELIABLE, 20, hi, 1));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_STATE,
                           wpe_session_broadcast(s[1], WPE_NET_CHANNEL_RELIABLE, 20, hi, 1));

    for (int i = 2; i >= 0; --i)
        wpe_session_destroy(s[i]);
    wpe_network_hub_destroy(hub);
}

static void test_session_refusals_and_names(void) {
    static Log log;
    log.count = 0;
    WPE_NetHub* hub = NULL;
    WPE_TEST_ASSERT_OK(wpe_network_hub_create(&hub));
    WPE_Session* s[5] = {0};
    WPE_TEST_ASSERT_OK(wpe_session_host(host_desc(hub, 2, NULL), &s[0]));
    WPE_TEST_ASSERT_OK(
        wpe_session_join(join_desc(hub, "A\tvery long name that does not fit at all"), HOST_ADDRESS, &s[1]));
    run(hub, s, 2, 200, &log);
    /* Control characters become '?', and long names are cut. */
    WPE_TEST_ASSERT_EQ_STR("A?very long name that does not ",
                           expect(&log, 0, WPE_SESSION_EVENT_PLAYER_JOINED, 1)->text);
    WPE_TEST_ASSERT_EQ_STR("Host", expect(&log, 1, WPE_SESSION_EVENT_PLAYER_JOINED, 0)->text);

    /* Full; another game; not accepting. */
    WPE_SessionDesc other = *join_desc(hub, "Eve");
    WPE_TEST_ASSERT_OK(wpe_session_join(join_desc(hub, "Carl"), HOST_ADDRESS, &s[2]));
    run(hub, s, 4, 300, &log); /* A full host has one spare slot to refuse with: one at a time. */
    other.game_id = GAME_ID + 1;
    WPE_TEST_ASSERT_OK(wpe_session_join(&other, HOST_ADDRESS, &s[3]));
    run(hub, s, 4, 300, &log);
    WPE_TEST_ASSERT_EQ_UINT(WPE_SESSION_REASON_FULL,
                            expect(&log, 2, WPE_SESSION_EVENT_ENDED, WPE_SESSION_NO_PLAYER)->reason);
    WPE_TEST_ASSERT_EQ_UINT(WPE_SESSION_REASON_WRONG_GAME,
                            expect(&log, 3, WPE_SESSION_EVENT_ENDED, WPE_SESSION_NO_PLAYER)->reason);
    WPE_TEST_ASSERT_EQ_STR("game full", wpe_session_reason_name(WPE_SESSION_REASON_FULL));
    WPE_TEST_ASSERT(!find(&log, 2, WPE_SESSION_EVENT_JOINED, WPE_SESSION_NO_PLAYER));
    WPE_TEST_ASSERT(!find(&log, 0, WPE_SESSION_EVENT_PLAYER_JOINED, 2)); /* The host saw no new player. */
    wpe_session_destroy(s[1]);
    s[1] = NULL;
    wpe_session_set_accepting(s[0], false);
    log.count = 0;
    WPE_TEST_ASSERT_OK(wpe_session_join(join_desc(hub, "Dana"), HOST_ADDRESS, &s[4]));
    run(hub, s, 5, 300, &log);
    WPE_TEST_ASSERT_EQ_UINT(WPE_SESSION_REASON_NOT_ACCEPTING,
                            expect(&log, 4, WPE_SESSION_EVENT_ENDED, WPE_SESSION_NO_PLAYER)->reason);
    WPE_TEST_ASSERT_EQ_UINT(WPE_SESSION_NO_PLAYER, wpe_session_local_player(s[4]));

    wpe_log_set_stderr_enabled(false);
    WPE_Session* bad = NULL;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_session_host(host_desc(hub, 9, NULL), &bad));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_session_host(host_desc(hub, 1, NULL), &bad));
    wpe_log_set_stderr_enabled(true);
    for (int i = 4; i >= 0; --i)
        wpe_session_destroy(s[i]);
    wpe_network_hub_destroy(hub);
}

static void test_session_scenes(void) {
    static Log log;
    log.count = 0;
    WPE_NetHub* hub = NULL;
    WPE_TEST_ASSERT_OK(wpe_network_hub_create(&hub));
    WPE_Session* s[3] = {0};
    WPE_TEST_ASSERT_OK(wpe_session_host(host_desc(hub, 4, NULL), &s[0]));
    WPE_TEST_ASSERT_OK(wpe_session_set_scene(s[0], "scenes/lobby.scene"));
    WPE_TEST_ASSERT_OK(wpe_session_join(join_desc(hub, "A"), HOST_ADDRESS, &s[1]));
    run(hub, s, 2, 200, &log);
    /* Joining brings the current scene, after the players. */
    const Seen* scene = expect(&log, 1, WPE_SESSION_EVENT_SCENE, WPE_SESSION_HOST);
    WPE_TEST_ASSERT_EQ_STR("scenes/lobby.scene", scene->text);
    WPE_TEST_ASSERT(scene > expect(&log, 1, WPE_SESSION_EVENT_PLAYER_JOINED, 0));
    WPE_TEST_ASSERT_EQ_STR("scenes/lobby.scene", wpe_session_scene(s[1]));

    log.count = 0;
    WPE_TEST_ASSERT_OK(wpe_session_set_scene(s[0], "scenes/track1.scene"));
    run(hub, s, 2, 100, &log);
    WPE_TEST_ASSERT_EQ_STR("scenes/track1.scene",
                           expect(&log, 1, WPE_SESSION_EVENT_SCENE, WPE_SESSION_HOST)->text);
    WPE_TEST_ASSERT(
        !find(&log, 0, WPE_SESSION_EVENT_SCENE, WPE_SESSION_NO_PLAYER)); /* Not echoed to the host. */

    wpe_log_set_stderr_enabled(false);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_session_set_scene(s[0], "bad\npath"));
    wpe_log_set_stderr_enabled(true);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_STATE, wpe_session_set_scene(s[1], "x.scene"));
    for (int i = 1; i >= 0; --i)
        wpe_session_destroy(s[i]);
    wpe_network_hub_destroy(hub);
}

static void test_session_leaving(void) {
    static Log log;
    log.count = 0;
    WPE_NetHub* hub = NULL;
    WPE_TEST_ASSERT_OK(wpe_network_hub_create(&hub));
    WPE_Session* s[4] = {0};
    WPE_TEST_ASSERT_OK(wpe_session_host(host_desc(hub, 4, NULL), &s[0]));
    for (int i = 1; i <= 3; ++i)
        WPE_TEST_ASSERT_OK(wpe_session_join(join_desc(hub, "P"), HOST_ADDRESS, &s[i]));
    run(hub, s, 4, 300, &log);
    WPE_TEST_ASSERT_EQ_UINT(4, wpe_session_player_count(s[0]));

    /* Leaving: the host and the others see it. */
    log.count = 0;
    const WPE_PlayerId first = wpe_session_local_player(s[1]);
    wpe_session_destroy(s[1]);
    s[1] = NULL;
    run(hub, s, 4, 100, &log);
    WPE_TEST_ASSERT_EQ_UINT(WPE_NET_REASON_CLOSED,
                            expect(&log, 0, WPE_SESSION_EVENT_PLAYER_LEFT, first)->reason);
    WPE_TEST_ASSERT_EQ_UINT(WPE_NET_REASON_CLOSED,
                            expect(&log, 2, WPE_SESSION_EVENT_PLAYER_LEFT, first)->reason);

    /* Its slot is reused by the next player. */
    WPE_TEST_ASSERT_OK(wpe_session_join(join_desc(hub, "Again"), HOST_ADDRESS, &s[1]));
    run(hub, s, 4, 100, &log);
    WPE_TEST_ASSERT_EQ_UINT(first, wpe_session_local_player(s[1]));

    /* A kick: the player hears why; the rest see it leave. */
    log.count = 0;
    const WPE_PlayerId kicked = wpe_session_local_player(s[2]);
    WPE_TEST_ASSERT_OK(wpe_session_kick(s[0], kicked, WPE_SESSION_REASON_GAME + 1));
    run(hub, s, 4, 100, &log);
    WPE_TEST_ASSERT_EQ_UINT(WPE_SESSION_REASON_GAME + 1,
                            expect(&log, 2, WPE_SESSION_EVENT_ENDED, WPE_SESSION_NO_PLAYER)->reason);
    expect(&log, 0, WPE_SESSION_EVENT_PLAYER_LEFT, kicked);
    expect(&log, 3, WPE_SESSION_EVENT_PLAYER_LEFT, kicked);
    WPE_TEST_ASSERT_EQ_UINT(WPE_SESSION_NO_PLAYER, wpe_session_local_player(s[2]));
    wpe_log_set_stderr_enabled(false);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_session_kick(s[0], 1, WPE_SESSION_REASON_FULL));
    wpe_log_set_stderr_enabled(true);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_STATE,
                           wpe_session_kick(s[3], 1, WPE_SESSION_REASON_GAME));

    /* A pulled cable times out; the host leaving ends everyone's session. */
    log.count = 0;
    wpe_network_hub_cut(hub, (WPE_NetTransport*)wpe_session_transport(s[3]));
    const WPE_PlayerId cut = wpe_session_local_player(s[3]);
    run(hub, s, 4, 1200, &log);
    WPE_TEST_ASSERT_EQ_UINT(WPE_NET_REASON_TIMEOUT,
                            expect(&log, 0, WPE_SESSION_EVENT_PLAYER_LEFT, cut)->reason);
    WPE_TEST_ASSERT_EQ_UINT(WPE_NET_REASON_TIMEOUT,
                            expect(&log, 3, WPE_SESSION_EVENT_ENDED, WPE_SESSION_NO_PLAYER)->reason);
    log.count = 0;
    wpe_session_destroy(s[0]);
    s[0] = NULL;
    run(hub, s, 4, 100, &log);
    WPE_TEST_ASSERT_EQ_UINT(WPE_NET_REASON_CLOSED,
                            expect(&log, 1, WPE_SESSION_EVENT_ENDED, WPE_SESSION_NO_PLAYER)->reason);
    for (int i = 0; i < 4; ++i)
        wpe_session_destroy(s[i]);
    wpe_network_hub_destroy(hub);
}

/* ---- Hostile peers --------------------------------------------------------------------------- */

static uint64_t g_rng = 5;
static uint32_t next_u32(void) {
    g_rng = g_rng * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)(g_rng >> 32);
}

/* Polls a raw transport, returning the last disconnect reason seen (0 if none). */
static uint32_t raw_drain(WPE_NetTransport* t, bool* connected) {
    uint32_t reason = 0;
    WPE_NetEvent e;
    while (wpe_network_poll(t, &e)) {
        if (e.type == WPE_NET_EVENT_CONNECTED) *connected = true;
        if (e.type == WPE_NET_EVENT_DISCONNECTED) reason = e.reason;
    }
    return reason;
}

static void test_session_survives_hostile_players(void) {
    static Log log;
    WPE_NetHub* hub = NULL;
    WPE_TEST_ASSERT_OK(wpe_network_hub_create(&hub));
    WPE_Session* host = NULL;
    WPE_TEST_ASSERT_OK(wpe_session_host(host_desc(hub, 4, NULL), &host));
    log.count = 0;
    /* Silent connections are dropped after the timeout; garbage gets a protocol refusal. */
    for (int round = 0; round < 200; ++round) {
        WPE_NetTransport* raw = NULL;
        WPE_TEST_ASSERT_OK(
            wpe_network_connect(&(WPE_NetTransportDesc){.hub = hub, .timeout_ms = 2000}, HOST_ADDRESS, &raw));
        bool connected = false;
        uint32_t reason = 0;
        for (int step = 0; step < 150 && !reason; ++step) {
            wpe_network_hub_advance(hub, 10);
            drain(&host, 1, 0, &log);
            reason = raw_drain(raw, &connected);
            if (connected && step == 5 && round > 0) {
                uint8_t junk[64];
                const size_t size = 1 + next_u32() % sizeof junk;
                for (size_t i = 0; i < size; ++i)
                    junk[i] = (uint8_t)next_u32();
                if (round % 3 == 0) junk[0] = 1; /* A HELLO of random bytes. */
                (void)wpe_network_send(raw, 0, (WPE_NetChannel)(round % 2), junk, size);
            }
        }
        WPE_TEST_ASSERT_MSG(reason == WPE_SESSION_REASON_PROTOCOL, "round %d: reason %u", round, reason);
        wpe_network_destroy(raw);
    }
    WPE_TEST_ASSERT(!find(&log, 0, WPE_SESSION_EVENT_PLAYER_JOINED, WPE_SESSION_NO_PLAYER));
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_session_player_count(host));
    wpe_session_destroy(host);
    wpe_network_hub_destroy(hub);
}

static void test_session_survives_a_hostile_host(void) {
    static Log log;
    WPE_NetHub* hub = NULL;
    WPE_TEST_ASSERT_OK(wpe_network_hub_create(&hub));
    WPE_NetTransport* fake = NULL;
    WPE_TEST_ASSERT_OK(wpe_network_host(&(WPE_NetTransportDesc){.hub = hub, .port = 7000}, &fake));
    for (int round = 0; round < 200; ++round) {
        log.count = 0;
        WPE_Session* player = NULL;
        WPE_TEST_ASSERT_OK(wpe_session_join(join_desc(hub, "Victim"), HOST_ADDRESS, &player));
        bool ended = false;
        for (int step = 0; step < 50 && !ended; ++step) {
            wpe_network_hub_advance(hub, 10);
            WPE_NetEvent e;
            while (wpe_network_poll(fake, &e)) {
                if (e.type != WPE_NET_EVENT_MESSAGE) continue;
                /* Answer the HELLO with random "session" messages (WELCOME, joins, scenes...). */
                for (int m = 0; m < 3; ++m) {
                    uint8_t junk[48];
                    const size_t size = 1 + next_u32() % sizeof junk;
                    for (size_t i = 0; i < size; ++i)
                        junk[i] = (uint8_t)next_u32();
                    junk[0] = (uint8_t)(1 + next_u32() % 5);
                    (void)wpe_network_send(fake, e.peer, WPE_NET_CHANNEL_RELIABLE, junk, size);
                }
            }
            drain(&player, 1, 0, &log);
            ended = find(&log, 0, WPE_SESSION_EVENT_ENDED, WPE_SESSION_NO_PLAYER) != NULL;
        }
        WPE_TEST_ASSERT_MSG(ended, "round %d: garbage was accepted", round);
        WPE_TEST_ASSERT_EQ_UINT(WPE_SESSION_REASON_PROTOCOL,
                                expect(&log, 0, WPE_SESSION_EVENT_ENDED, WPE_SESSION_NO_PLAYER)->reason);
        WPE_TEST_ASSERT_EQ_UINT(WPE_SESSION_NO_PLAYER, wpe_session_local_player(player));
        wpe_session_destroy(player);
        wpe_network_hub_advance(hub, 50);
        WPE_NetEvent e;
        while (wpe_network_poll(fake, &e)) {
        }
    }
    wpe_network_destroy(fake);
    wpe_network_hub_destroy(hub);
}

/* ---- Discovery ------------------------------------------------------------------------------- */

static void test_discovery_on_hub(void) {
    WPE_NetHub* hub = NULL;
    WPE_TEST_ASSERT_OK(wpe_network_hub_create(&hub));
    WPE_Session* s[3] = {0};
    WPE_SessionDesc desc = *host_desc(hub, 4, "Mika");
    desc.discoverable = true;
    WPE_TEST_ASSERT_OK(wpe_session_host(&desc, &s[0]));
    /* Another game on the network: not listed. */
    WPE_SessionDesc other = *host_desc(hub, 4, "Someone");
    other.discoverable = true;
    other.game_id = 99;
    other.transport.port = 7001;
    WPE_TEST_ASSERT_OK(wpe_session_host(&other, &s[2]));

    WPE_DiscoveryBrowser* browser = NULL;
    WPE_TEST_ASSERT_OK(
        wpe_discovery_browser_create(&(WPE_DiscoveryDesc){.hub = hub, .game_id = GAME_ID}, &browser));
    static Log log;
    log.count = 0;
    for (int step = 0; step < 10; ++step) {
        wpe_discovery_browser_update(browser);
        run(hub, s, 3, 10, &log);
    }
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_discovery_browser_count(browser));
    WPE_DiscoveredGame game;
    WPE_TEST_ASSERT(wpe_discovery_browser_game(browser, 0, &game));
    WPE_TEST_ASSERT_EQ_STR("Test Racer", game.info.game_name);
    WPE_TEST_ASSERT_EQ_STR("Mika", game.info.host_name);
    WPE_TEST_ASSERT_EQ_UINT(7000, game.address.port);
    WPE_TEST_ASSERT_EQ_UINT(1, game.info.players);
    WPE_TEST_ASSERT_EQ_UINT(4, game.info.max_players);
    WPE_TEST_ASSERT(game.info.accepting);

    /* Join through the address it found; the next answers count the new player. */
    WPE_TEST_ASSERT_OK(wpe_session_join(join_desc(hub, "Alice"), game.address, &s[1]));
    for (int step = 0; step < 150; ++step) {
        wpe_discovery_browser_update(browser);
        run(hub, s, 3, 10, &log);
    }
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_session_local_player(s[1]));
    WPE_TEST_ASSERT(wpe_discovery_browser_game(browser, 0, &game));
    WPE_TEST_ASSERT_EQ_UINT(2, game.info.players);
    wpe_session_set_accepting(s[0], false);
    wpe_discovery_browser_refresh(browser);
    for (int step = 0; step < 5; ++step) {
        wpe_discovery_browser_update(browser);
        run(hub, s, 3, 10, &log);
    }
    WPE_TEST_ASSERT(wpe_discovery_browser_game(browser, 0, &game));
    WPE_TEST_ASSERT(!game.info.accepting);

    /* A host that is gone drops off the list. */
    wpe_session_destroy(s[0]);
    s[0] = NULL;
    for (int step = 0; step < 400; ++step) {
        wpe_discovery_browser_update(browser);
        run(hub, s, 3, 10, &log);
    }
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_discovery_browser_count(browser));
    wpe_discovery_browser_destroy(browser);
    for (int i = 0; i < 3; ++i)
        wpe_session_destroy(s[i]);
    wpe_network_hub_destroy(hub);
}

static double seconds_now(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void test_discovery_udp_on_this_machine(void) {
    /* A beacon and a browser over real UDP, on 127.0.0.1 only. */
    const WPE_DiscoveryDesc desc = {.port = 47909, .local_only = true, .game_id = GAME_ID};
    const WPE_DiscoveryInfo info = {.game_port = 4321,
                                    .players = 2,
                                    .max_players = 4,
                                    .accepting = true,
                                    .game_name = "UDP Game",
                                    .host_name = "Here"};
    WPE_DiscoveryBeacon* beacon = NULL;
    WPE_TEST_ASSERT_OK(wpe_discovery_beacon_create(&desc, &info, &beacon));
    WPE_DiscoveryBrowser* browser = NULL;
    WPE_TEST_ASSERT_OK(wpe_discovery_browser_create(&desc, &browser));
    const double end = seconds_now() + 10.0;
    while (wpe_discovery_browser_count(browser) == 0 && seconds_now() < end) {
        wpe_discovery_browser_update(browser);
        wpe_discovery_beacon_update(beacon);
    }
    WPE_DiscoveredGame game;
    WPE_TEST_ASSERT_MSG(wpe_discovery_browser_game(browser, 0, &game), "no answer over UDP");
    WPE_TEST_ASSERT_EQ_UINT(0x7F000001u, game.address.ipv4);
    WPE_TEST_ASSERT_EQ_UINT(4321, game.address.port);
    WPE_TEST_ASSERT_EQ_STR("UDP Game", game.info.game_name);
    wpe_discovery_browser_destroy(browser);
    wpe_discovery_beacon_destroy(beacon);
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_session_join_and_talk), WPE_TEST_CASE(test_session_refusals_and_names),
              WPE_TEST_CASE(test_session_scenes), WPE_TEST_CASE(test_session_leaving),
              WPE_TEST_CASE(test_session_survives_hostile_players),
              WPE_TEST_CASE(test_session_survives_a_hostile_host), WPE_TEST_CASE(test_discovery_on_hub),
              WPE_TEST_CASE(test_discovery_udp_on_this_machine))
