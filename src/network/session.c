/* Sessions over a transport (see session.h).
 *
 * Every transport message starts with a kind byte. The session's own kinds:
 *   HELLO          player -> host   magic, protocol version, game id (64), name
 *   WELCOME        host -> player   your id, max players, scene epoch, scene, then each present player
 *                                   (id, name)
 *   PLAYER_JOINED  host -> players  id, name
 *   PLAYER_LEFT    host -> players  id, reason
 *   SCENE          host -> players  epoch, path
 * Kinds from WPE_SESSION_KIND_FIRST up belong to the layers above and are passed through.
 * Strings are a varint length and bytes (bits.h). */
#define WPE_LOG_MODULE "network"
#include "network/session.h"

#include <stdio.h>
#include <string.h>

#include "core/alloc.h"
#include "core/log.h"
#include "network/bits.h"
#include "network/transport_internal.h"

#define WPE__HELLO_MAGIC 0x31455057u /* "WPE1" read little-endian */
#define WPE__QUEUE_MAX 16u
#define WPE__CONTROL_MAX 1024u /* WELCOME with 8 names and a scene path fits easily. */

typedef enum wpe__Kind {
    WPE__HELLO = 1,
    WPE__WELCOME = 2,
    WPE__PLAYER_JOINED = 3,
    WPE__PLAYER_LEFT = 4,
    WPE__SCENE = 5,
} wpe__Kind;

typedef enum wpe__ClientState {
    WPE__CLIENT_CONNECTING = 0, /* Until WELCOME. */
    WPE__CLIENT_JOINED,
    WPE__CLIENT_ENDED,
} wpe__ClientState;

typedef struct wpe__Player {
    bool present;
    char name[WPE_SESSION_NAME_MAX];
    WPE_NetPeerId peer; /* Host: its transport peer. */
} wpe__Player;

typedef struct wpe__Queued {
    WPE_SessionEventType type;
    WPE_PlayerId player;
    uint32_t reason;
    char text[WPE_SESSION_SCENE_MAX];
} wpe__Queued;

struct WPE_Session {
    WPE_NetTransport* transport;
    WPE_DiscoveryBeacon* beacon; /* Host, when discoverable. */
    bool host;
    bool accepting;
    uint32_t max_players;
    uint64_t game_id;
    char game_name[WPE_DISCOVERY_GAME_NAME_MAX];
    WPE_PlayerId local;
    wpe__ClientState state; /* Player sessions. */
    uint32_t end_reason;    /* Why it ENDED. */
    wpe__Player players[WPE_SESSION_MAX_PLAYERS];
    /* Host: what each transport peer is. */
    WPE_PlayerId peer_player[WPE_SESSION_MAX_PLAYERS];
    bool peer_pending[WPE_SESSION_MAX_PLAYERS]; /* Connected, no HELLO yet. */
    uint64_t peer_since[WPE_SESSION_MAX_PLAYERS];
    char scene[WPE_SESSION_SCENE_MAX];
    uint32_t scene_epoch; /* Counts the host's scene changes (0: none yet). */
    wpe__Queued queue[WPE__QUEUE_MAX];
    uint32_t queue_head;
    uint32_t queue_count;
    char event_text[WPE_SESSION_SCENE_MAX]; /* The text of the event last handed out. */
};

/* ---- Small helpers --------------------------------------------------------------------------- */

static void wpe__clean_name(char* out, const char* text, const char* fallback) {
    if (!text || !text[0]) text = fallback;
    size_t length = 0;
    while (length < WPE_SESSION_NAME_MAX - 1 && text[length]) {
        const unsigned char c = (unsigned char)text[length];
        out[length] = c < 0x20u || c == 0x7Fu ? '?' : (char)c;
        ++length;
    }
    out[length] = '\0';
}

static bool wpe__has_control(const char* text) {
    for (; *text; ++text)
        if ((unsigned char)*text < 0x20u || *text == 0x7F) return true;
    return false;
}

static void wpe__write_text(WPE_BitWriter* w, const char* text) {
    const size_t length = strlen(text);
    wpe_bits_write_varint(w, (uint32_t)length);
    wpe_bits_write_bytes(w, text, length);
}

/* Reads a string shorter than `max` into `out`; names get control characters replaced, other
 * strings with them fail. */
static bool wpe__read_text(WPE_BitReader* r, char* out, size_t max, bool is_name) {
    const uint32_t length = wpe_bits_read_varint(r);
    if (r->error || length >= max) return false;
    const uint8_t* bytes = wpe_bits_read_bytes(r, length);
    if (!bytes) return false;
    for (uint32_t i = 0; i < length; ++i) {
        const bool control = bytes[i] < 0x20u || bytes[i] == 0x7Fu;
        if (control && !is_name) return false;
        out[i] = control ? '?' : (char)bytes[i];
    }
    out[length] = '\0';
    return true;
}

static void wpe__enqueue(WPE_Session* s, WPE_SessionEventType type, WPE_PlayerId player, uint32_t reason,
                         const char* text) {
    if (s->queue_count == WPE__QUEUE_MAX) {
        wpe_log_error("session event queue full: an event was dropped");
        return;
    }
    wpe__Queued* q = &s->queue[(s->queue_head + s->queue_count++) % WPE__QUEUE_MAX];
    *q = (wpe__Queued){.type = type, .player = player, .reason = reason};
    /* A plain bounded copy: `text` is often another field of the session (a name, the scene). */
    for (size_t i = 0; text && i + 1 < sizeof q->text && text[i]; ++i)
        q->text[i] = text[i];
}

static bool wpe__dequeue(WPE_Session* s, WPE_SessionEvent* out) {
    if (s->queue_count == 0) return false;
    const wpe__Queued* q = &s->queue[s->queue_head];
    s->queue_head = (s->queue_head + 1) % WPE__QUEUE_MAX;
    --s->queue_count;
    memcpy(s->event_text, q->text, sizeof s->event_text);
    *out =
        (WPE_SessionEvent){.type = q->type, .player = q->player, .reason = q->reason, .text = s->event_text};
    return true;
}

/* Sends a session message: the kind byte, then `body` (may be empty). */
static WPE_Result wpe__send_raw(WPE_Session* s, WPE_NetPeerId peer, WPE_NetChannel channel, uint32_t kind,
                                const void* body, size_t size) {
    if (size + 1 > WPE_NET_MAX_MESSAGE_SIZE) {
        wpe_log_error("a %zu-byte session message is too large (at most %u)", size,
                      WPE_NET_MAX_MESSAGE_SIZE - 1);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    uint8_t small[256];
    uint8_t* message = size + 1 <= sizeof small ? small : wpe_alloc(size + 1);
    if (!message) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    message[0] = (uint8_t)kind;
    if (size) memcpy(message + 1, body, size);
    const WPE_Result result = wpe_network_send(s->transport, peer, channel, message, size + 1);
    if (message != small) wpe_free(message);
    return result;
}

static uint32_t wpe__count_players(const WPE_Session* s) {
    uint32_t count = 0;
    for (uint32_t p = 0; p < s->max_players; ++p)
        count += s->players[p].present;
    return count;
}

static void wpe__update_beacon(WPE_Session* s) {
    if (!s->beacon) return;
    WPE_DiscoveryInfo info = {
        .game_port = wpe_network_local_port(s->transport),
        .players = (uint8_t)wpe__count_players(s),
        .max_players = (uint8_t)s->max_players,
        .accepting = s->accepting && wpe__count_players(s) < s->max_players,
    };
    snprintf(info.game_name, sizeof info.game_name, "%s", s->game_name);
    snprintf(info.host_name, sizeof info.host_name, "%s", s->players[WPE_SESSION_HOST].name);
    wpe_discovery_beacon_set_info(s->beacon, &info);
}

/* ---- Host ------------------------------------------------------------------------------------ */

/* Tells every joined player except `except` (a message of the session's own, reliable). */
static void wpe__host_tell_all(WPE_Session* s, WPE_PlayerId except, const uint8_t* message, size_t size) {
    for (WPE_PlayerId p = 1; p < s->max_players; ++p)
        if (s->players[p].present && p != except)
            (void)wpe_network_send(s->transport, s->players[p].peer, WPE_NET_CHANNEL_RELIABLE, message, size);
}

/* A player is gone: local event, the others told, the slot freed. */
static void wpe__host_remove(WPE_Session* s, WPE_PlayerId p, uint32_t reason) {
    wpe__Player* player = &s->players[p];
    s->peer_player[player->peer] = WPE_SESSION_NO_PLAYER;
    player->present = false;
    uint8_t message[16];
    WPE_BitWriter w = wpe_bits_writer(message, sizeof message);
    wpe_bits_write(&w, WPE__PLAYER_LEFT, 8);
    wpe_bits_write_varint(&w, p);
    wpe_bits_write_varint(&w, reason);
    wpe__host_tell_all(s, WPE_SESSION_NO_PLAYER, message, wpe_bits_writer_size(&w));
    wpe__enqueue(s, WPE_SESSION_EVENT_PLAYER_LEFT, p, reason, player->name);
    wpe__update_beacon(s);
}

static void wpe__host_refuse(WPE_Session* s, WPE_NetPeerId peer, uint32_t reason) {
    s->peer_pending[peer] = false;
    (void)wpe_network_disconnect(s->transport, peer, reason);
}

static void wpe__host_hello(WPE_Session* s, WPE_NetPeerId peer, WPE_BitReader* r) {
    const uint32_t magic = wpe_bits_read(r, 32);
    const uint32_t version = wpe_bits_read_varint(r);
    const uint64_t id_low = wpe_bits_read(r, 32);
    const uint64_t id_high = wpe_bits_read(r, 32);
    char name[WPE_SESSION_NAME_MAX];
    if (!wpe__read_text(r, name, sizeof name, true) || r->error || magic != WPE__HELLO_MAGIC) {
        wpe__host_refuse(s, peer, WPE_SESSION_REASON_PROTOCOL);
        return;
    }
    if (version != WPE_SESSION_PROTOCOL_VERSION) {
        wpe__host_refuse(s, peer, WPE_SESSION_REASON_VERSION);
        return;
    }
    if ((id_low | id_high << 32) != s->game_id) {
        wpe__host_refuse(s, peer, WPE_SESSION_REASON_WRONG_GAME);
        return;
    }
    if (!s->accepting) {
        wpe__host_refuse(s, peer, WPE_SESSION_REASON_NOT_ACCEPTING);
        return;
    }
    WPE_PlayerId p = 1;
    while (p < s->max_players && s->players[p].present)
        ++p;
    if (p == s->max_players) {
        wpe__host_refuse(s, peer, WPE_SESSION_REASON_FULL);
        return;
    }
    s->peer_pending[peer] = false;
    s->peer_player[peer] = p;
    s->players[p] = (wpe__Player){.present = true, .peer = peer};
    wpe__clean_name(s->players[p].name, name, "Player");

    uint8_t message[WPE__CONTROL_MAX];
    WPE_BitWriter w = wpe_bits_writer(message, sizeof message);
    wpe_bits_write(&w, WPE__WELCOME, 8);
    wpe_bits_write_varint(&w, p);
    wpe_bits_write_varint(&w, s->max_players);
    wpe_bits_write_varint(&w, s->scene_epoch);
    wpe__write_text(&w, s->scene);
    wpe_bits_write_varint(&w, wpe__count_players(s));
    for (WPE_PlayerId q = 0; q < s->max_players; ++q) {
        if (!s->players[q].present) continue;
        wpe_bits_write_varint(&w, q);
        wpe__write_text(&w, s->players[q].name);
    }
    (void)wpe_network_send(s->transport, peer, WPE_NET_CHANNEL_RELIABLE, message, wpe_bits_writer_size(&w));

    w = wpe_bits_writer(message, sizeof message);
    wpe_bits_write(&w, WPE__PLAYER_JOINED, 8);
    wpe_bits_write_varint(&w, p);
    wpe__write_text(&w, s->players[p].name);
    wpe__host_tell_all(s, p, message, wpe_bits_writer_size(&w));
    wpe__enqueue(s, WPE_SESSION_EVENT_PLAYER_JOINED, p, 0, s->players[p].name);
    wpe__update_beacon(s);
}

/* Handles one transport event; true with *out set when it is a game message for the caller. */
static bool wpe__host_handle(WPE_Session* s, const WPE_NetEvent* e, WPE_SessionEvent* out) {
    const WPE_NetPeerId peer = e->peer;
    switch (e->type) {
    case WPE_NET_EVENT_CONNECTED:
        s->peer_pending[peer] = true;
        s->peer_since[peer] = wpe__net_transport_now(s->transport);
        return false;
    case WPE_NET_EVENT_DISCONNECTED:
        s->peer_pending[peer] = false;
        if (s->peer_player[peer] != WPE_SESSION_NO_PLAYER)
            wpe__host_remove(s, s->peer_player[peer], e->reason);
        return false;
    case WPE_NET_EVENT_MESSAGE: {
        const uint32_t kind = e->data[0];
        if (s->peer_pending[peer]) {
            if (kind != WPE__HELLO) {
                wpe__host_refuse(s, peer, WPE_SESSION_REASON_PROTOCOL);
                return false;
            }
            WPE_BitReader r = wpe_bits_reader(e->data + 1, e->size - 1);
            wpe__host_hello(s, peer, &r);
            return false;
        }
        const WPE_PlayerId p = s->peer_player[peer];
        if (p == WPE_SESSION_NO_PLAYER) return false;
        if (kind < WPE_SESSION_KIND_FIRST) {
            /* Players send nothing of the session's own after HELLO. */
            (void)wpe_network_disconnect(s->transport, peer, WPE_SESSION_REASON_PROTOCOL);
            wpe__host_remove(s, p, WPE_SESSION_REASON_PROTOCOL);
            return false;
        }
        *out = (WPE_SessionEvent){.type = WPE_SESSION_EVENT_MESSAGE,
                                  .player = p,
                                  .kind = kind,
                                  .channel = e->channel,
                                  .data = e->data + 1,
                                  .size = e->size - 1,
                                  .text = ""};
        return true;
    }
    case WPE_NET_EVENT_NONE:
        break;
    }
    return false;
}

/* Players connected but silent for the timeout are dropped. */
static void wpe__host_check_pending(WPE_Session* s) {
    const uint64_t now = wpe__net_transport_now(s->transport);
    for (WPE_NetPeerId peer = 0; peer < wpe_network_max_peers(s->transport); ++peer)
        if (s->peer_pending[peer] && now - s->peer_since[peer] >= s->transport->timeout_ms)
            wpe__host_refuse(s, peer, WPE_SESSION_REASON_PROTOCOL);
}

/* ---- Player ---------------------------------------------------------------------------------- */

static void wpe__client_end(WPE_Session* s, uint32_t reason) {
    if (s->state == WPE__CLIENT_ENDED) return;
    s->state = WPE__CLIENT_ENDED;
    s->end_reason = reason ? reason : WPE_NET_REASON_CLOSED;
    s->local = WPE_SESSION_NO_PLAYER;
    memset(s->players, 0, sizeof s->players);
    wpe__enqueue(s, WPE_SESSION_EVENT_ENDED, WPE_SESSION_NO_PLAYER, reason, NULL);
}

static void wpe__client_violation(WPE_Session* s) {
    (void)wpe_network_disconnect(s->transport, 0, WPE_SESSION_REASON_PROTOCOL);
    wpe__client_end(s, WPE_SESSION_REASON_PROTOCOL);
}

static void wpe__client_welcome(WPE_Session* s, WPE_BitReader* r) {
    const uint32_t id = wpe_bits_read_varint(r);
    const uint32_t max_players = wpe_bits_read_varint(r);
    const uint32_t epoch = wpe_bits_read_varint(r);
    char scene[WPE_SESSION_SCENE_MAX] = {0}; /* Zeroed: copied whole below. */
    const bool scene_ok = wpe__read_text(r, scene, sizeof scene, false);
    const uint32_t count = wpe_bits_read_varint(r);
    if (r->error || !scene_ok || max_players < 2 || max_players > WPE_SESSION_MAX_PLAYERS || id == 0 ||
        id >= max_players || count > max_players) {
        wpe__client_violation(s);
        return;
    }
    wpe__Player players[WPE_SESSION_MAX_PLAYERS] = {0};
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t p = wpe_bits_read_varint(r);
        if (r->error || p >= max_players || players[p].present) {
            wpe__client_violation(s);
            return;
        }
        players[p].present = true;
        if (!wpe__read_text(r, players[p].name, sizeof players[p].name, true)) {
            wpe__client_violation(s);
            return;
        }
    }
    if (!players[id].present || !players[WPE_SESSION_HOST].present) {
        wpe__client_violation(s);
        return;
    }
    memcpy(s->players, players, sizeof players);
    s->max_players = max_players;
    s->local = id;
    s->state = WPE__CLIENT_JOINED;
    s->scene_epoch = epoch;
    memcpy(s->scene, scene, sizeof scene);
    wpe__enqueue(s, WPE_SESSION_EVENT_JOINED, id, 0, s->players[id].name);
    for (WPE_PlayerId p = 0; p < max_players; ++p)
        if (p != id && s->players[p].present)
            wpe__enqueue(s, WPE_SESSION_EVENT_PLAYER_JOINED, p, 0, s->players[p].name);
    if (s->scene[0]) wpe__enqueue(s, WPE_SESSION_EVENT_SCENE, WPE_SESSION_HOST, epoch, s->scene);
}

static bool wpe__client_handle(WPE_Session* s, const WPE_NetEvent* e, WPE_SessionEvent* out) {
    switch (e->type) {
    case WPE_NET_EVENT_CONNECTED: {
        uint8_t message[64];
        WPE_BitWriter w = wpe_bits_writer(message, sizeof message);
        wpe_bits_write(&w, WPE__HELLO, 8);
        wpe_bits_write(&w, WPE__HELLO_MAGIC, 32);
        wpe_bits_write_varint(&w, WPE_SESSION_PROTOCOL_VERSION);
        wpe_bits_write(&w, (uint32_t)s->game_id, 32);
        wpe_bits_write(&w, (uint32_t)(s->game_id >> 32), 32);
        wpe__write_text(&w, s->players[0].name); /* Our own name, kept in slot 0 until WELCOME. */
        (void)wpe_network_send(s->transport, 0, WPE_NET_CHANNEL_RELIABLE, message, wpe_bits_writer_size(&w));
        return false;
    }
    case WPE_NET_EVENT_DISCONNECTED:
        wpe__client_end(s, e->reason);
        return false;
    case WPE_NET_EVENT_MESSAGE: {
        const uint32_t kind = e->data[0];
        WPE_BitReader r = wpe_bits_reader(e->data + 1, e->size - 1);
        if (s->state == WPE__CLIENT_CONNECTING) {
            if (kind == WPE__WELCOME)
                wpe__client_welcome(s, &r);
            else if (kind < WPE_SESSION_KIND_FIRST)
                wpe__client_violation(s);
            return false; /* Game messages before WELCOME cannot be for us yet. */
        }
        if (s->state != WPE__CLIENT_JOINED) return false;
        if (kind >= WPE_SESSION_KIND_FIRST) {
            *out = (WPE_SessionEvent){.type = WPE_SESSION_EVENT_MESSAGE,
                                      .player = WPE_SESSION_HOST,
                                      .kind = kind,
                                      .channel = e->channel,
                                      .data = e->data + 1,
                                      .size = e->size - 1,
                                      .text = ""};
            return true;
        }
        if (kind == WPE__PLAYER_JOINED) {
            const uint32_t p = wpe_bits_read_varint(&r);
            char name[WPE_SESSION_NAME_MAX] = {0}; /* Zeroed: copied whole below. */
            if (!wpe__read_text(&r, name, sizeof name, true) || r.error || p == 0 || p >= s->max_players ||
                p == s->local) {
                wpe__client_violation(s);
                return false;
            }
            s->players[p].present = true;
            memcpy(s->players[p].name, name, sizeof name);
            wpe__enqueue(s, WPE_SESSION_EVENT_PLAYER_JOINED, p, 0, name);
        } else if (kind == WPE__PLAYER_LEFT) {
            const uint32_t p = wpe_bits_read_varint(&r);
            const uint32_t reason = wpe_bits_read_varint(&r);
            if (r.error || p == 0 || p >= s->max_players || p == s->local) {
                wpe__client_violation(s);
                return false;
            }
            if (!s->players[p].present) return false;
            s->players[p].present = false;
            wpe__enqueue(s, WPE_SESSION_EVENT_PLAYER_LEFT, p, reason, s->players[p].name);
        } else if (kind == WPE__SCENE) {
            const uint32_t epoch = wpe_bits_read_varint(&r);
            char scene[WPE_SESSION_SCENE_MAX] = {0}; /* Zeroed: copied whole below. */
            if (!wpe__read_text(&r, scene, sizeof scene, false) || r.error || epoch == 0) {
                wpe__client_violation(s);
                return false;
            }
            memcpy(s->scene, scene, sizeof scene);
            s->scene_epoch = epoch;
            wpe__enqueue(s, WPE_SESSION_EVENT_SCENE, WPE_SESSION_HOST, epoch, scene);
        } else {
            wpe__client_violation(s);
        }
        return false;
    }
    case WPE_NET_EVENT_NONE:
        break;
    }
    return false;
}

/* ---- Public API ------------------------------------------------------------------------------ */

static WPE_Result wpe__session_new(const WPE_SessionDesc* desc, bool host, WPE_Session** out) {
    const uint32_t max_players = desc->max_players ? desc->max_players : 4;
    if (host && (max_players < 2 || max_players > WPE_SESSION_MAX_PLAYERS)) {
        wpe_log_error("max_players must be 2..%u (got %u)", WPE_SESSION_MAX_PLAYERS, max_players);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    WPE_Session* s = wpe_alloc_zeroed(1, sizeof *s);
    if (!s) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    s->host = host;
    s->accepting = true;
    s->max_players = max_players;
    s->game_id = desc->game_id;
    s->local = WPE_SESSION_NO_PLAYER;
    snprintf(s->game_name, sizeof s->game_name, "%s", desc->game_name ? desc->game_name : "Game");
    for (uint32_t i = 0; i < WPE_SESSION_MAX_PLAYERS; ++i)
        s->peer_player[i] = WPE_SESSION_NO_PLAYER;
    wpe__clean_name(s->players[0].name, desc->player_name, host ? "Host" : "Player");
    *out = s;
    return WPE_RESULT_OK;
}

WPE_Result wpe_session_host(const WPE_SessionDesc* desc, WPE_Session** out_session) {
    if (!desc || !out_session) return WPE_RESULT_ERROR_NULL_ARG;
    *out_session = NULL;
    WPE_Session* s = NULL;
    WPE_Result result = wpe__session_new(desc, true, &s);
    if (result != WPE_RESULT_OK) return result;
    /* One transport slot per other player, and a spare so a full host can still say "full". */
    WPE_NetTransportDesc transport = desc->transport;
    transport.max_peers = s->max_players;
    result = wpe_network_host(&transport, &s->transport);
    if (result == WPE_RESULT_OK && desc->discoverable) {
        const WPE_DiscoveryDesc discovery = {.hub = transport.hub,
                                             .port = desc->discovery_port,
                                             .local_only = transport.local_only,
                                             .game_id = desc->game_id};
        const WPE_DiscoveryInfo info = {0};
        result = wpe_discovery_beacon_create(&discovery, &info, &s->beacon);
    }
    if (result != WPE_RESULT_OK) {
        wpe_session_destroy(s);
        return result;
    }
    s->local = WPE_SESSION_HOST;
    s->players[WPE_SESSION_HOST].present = true;
    wpe__update_beacon(s);
    wpe__enqueue(s, WPE_SESSION_EVENT_JOINED, WPE_SESSION_HOST, 0, s->players[WPE_SESSION_HOST].name);
    *out_session = s;
    return WPE_RESULT_OK;
}

WPE_Result wpe_session_join(const WPE_SessionDesc* desc, WPE_NetAddress address, WPE_Session** out_session) {
    if (!desc || !out_session) return WPE_RESULT_ERROR_NULL_ARG;
    *out_session = NULL;
    WPE_Session* s = NULL;
    WPE_Result result = wpe__session_new(desc, false, &s);
    if (result != WPE_RESULT_OK) return result;
    result = wpe_network_connect(&desc->transport, address, &s->transport);
    if (result != WPE_RESULT_OK) {
        wpe_session_destroy(s);
        return result;
    }
    *out_session = s;
    return WPE_RESULT_OK;
}

void wpe_session_destroy(WPE_Session* session) {
    if (!session) return;
    wpe_discovery_beacon_destroy(session->beacon);
    wpe_network_destroy(session->transport);
    wpe_free(session);
}

bool wpe_session_poll(WPE_Session* session, WPE_SessionEvent* out_event) {
    if (out_event) *out_event = (WPE_SessionEvent){.text = ""};
    if (!session || !out_event) return false;
    if (wpe__dequeue(session, out_event)) return true;
    if (session->host) {
        wpe_discovery_beacon_update(session->beacon);
        wpe__host_check_pending(session);
    }
    WPE_NetEvent e;
    while (wpe_network_poll(session->transport, &e)) {
        const bool message = session->host ? wpe__host_handle(session, &e, out_event)
                                           : wpe__client_handle(session, &e, out_event);
        /* A game message is handed out at once: its data lives only until the next transport poll.
         * Order is kept: the queue is always empty here (anything queued by an earlier event was
         * returned before polling on), and the path that returns a message queues nothing. */
        if (message) return true;
        if (wpe__dequeue(session, out_event)) return true;
    }
    return false;
}

static WPE_Result wpe__check_kind(uint32_t kind) {
    if (kind >= WPE_SESSION_KIND_FIRST && kind <= 255) return WPE_RESULT_OK;
    wpe_log_error("message kind %u is outside %u..255", kind, WPE_SESSION_KIND_FIRST);
    return WPE_RESULT_ERROR_INVALID_ARG;
}

WPE_Result wpe_session_send(WPE_Session* session, WPE_PlayerId player, WPE_NetChannel channel, uint32_t kind,
                            const void* data, size_t size) {
    if (!session || (!data && size > 0)) return WPE_RESULT_ERROR_NULL_ARG;
    const WPE_Result valid = wpe__check_kind(kind);
    if (valid != WPE_RESULT_OK) return valid;
    if (session->host) {
        if (player == WPE_SESSION_HOST || player >= session->max_players || !session->players[player].present)
            return WPE_RESULT_ERROR_INVALID_STATE;
        return wpe__send_raw(session, session->players[player].peer, channel, kind, data, size);
    }
    if (session->state != WPE__CLIENT_JOINED || player != WPE_SESSION_HOST)
        return WPE_RESULT_ERROR_INVALID_STATE;
    return wpe__send_raw(session, 0, channel, kind, data, size);
}

WPE_Result wpe_session_broadcast(WPE_Session* session, WPE_NetChannel channel, uint32_t kind,
                                 const void* data, size_t size) {
    if (!session || (!data && size > 0)) return WPE_RESULT_ERROR_NULL_ARG;
    const WPE_Result valid = wpe__check_kind(kind);
    if (valid != WPE_RESULT_OK) return valid;
    if (!session->host) return WPE_RESULT_ERROR_INVALID_STATE;
    WPE_Result result = WPE_RESULT_OK;
    for (WPE_PlayerId p = 1; p < session->max_players; ++p) {
        if (!session->players[p].present) continue;
        const WPE_Result sent = wpe__send_raw(session, session->players[p].peer, channel, kind, data, size);
        if (sent != WPE_RESULT_OK) result = sent;
    }
    return result;
}

WPE_Result wpe_session_kick(WPE_Session* session, WPE_PlayerId player, uint32_t reason) {
    if (!session) return WPE_RESULT_ERROR_NULL_ARG;
    if (reason < WPE_SESSION_REASON_GAME) {
        wpe_log_error("kick reason %u is reserved (game reasons start at %u)", reason,
                      WPE_SESSION_REASON_GAME);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    if (!session->host || player == WPE_SESSION_HOST || player >= session->max_players ||
        !session->players[player].present)
        return WPE_RESULT_ERROR_INVALID_STATE;
    (void)wpe_network_disconnect(session->transport, session->players[player].peer, reason);
    wpe__host_remove(session, player, reason);
    return WPE_RESULT_OK;
}

WPE_Result wpe_session_set_scene(WPE_Session* session, const char* path) {
    if (!session || !path) return WPE_RESULT_ERROR_NULL_ARG;
    if (!session->host) return WPE_RESULT_ERROR_INVALID_STATE;
    if (strlen(path) >= WPE_SESSION_SCENE_MAX || wpe__has_control(path)) {
        wpe_log_error("cannot share the scene path \"%s\" (too long, or control characters)", path);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    snprintf(session->scene, sizeof session->scene, "%s", path);
    ++session->scene_epoch;
    uint8_t message[WPE_SESSION_SCENE_MAX + 16];
    WPE_BitWriter w = wpe_bits_writer(message, sizeof message);
    wpe_bits_write(&w, WPE__SCENE, 8);
    wpe_bits_write_varint(&w, session->scene_epoch);
    wpe__write_text(&w, session->scene);
    wpe__host_tell_all(session, WPE_SESSION_NO_PLAYER, message, wpe_bits_writer_size(&w));
    return WPE_RESULT_OK;
}

void wpe_session_set_accepting(WPE_Session* session, bool accepting) {
    if (!session || !session->host) return;
    session->accepting = accepting;
    wpe__update_beacon(session);
}

void wpe_session_flush(WPE_Session* session) {
    if (session) wpe_network_flush(session->transport);
}

bool wpe_session_is_host(const WPE_Session* session) {
    return session && session->host;
}

WPE_PlayerId wpe_session_local_player(const WPE_Session* session) {
    return session ? session->local : WPE_SESSION_NO_PLAYER;
}

uint32_t wpe_session_max_players(const WPE_Session* session) {
    return session ? session->max_players : 0;
}

uint32_t wpe_session_player_count(const WPE_Session* session) {
    if (!session || session->local == WPE_SESSION_NO_PLAYER) return 0;
    return wpe__count_players(session);
}

bool wpe_session_player(const WPE_Session* session, WPE_PlayerId player, WPE_SessionPlayer* out_player) {
    if (!session || !out_player || session->local == WPE_SESSION_NO_PLAYER ||
        player >= session->max_players || !session->players[player].present)
        return false;
    *out_player = (WPE_SessionPlayer){0};
    memcpy(out_player->name, session->players[player].name, sizeof out_player->name);
    /* The host measures each player; a player measures the host. */
    const WPE_NetPeerId peer = session->host ? session->players[player].peer : 0;
    WPE_NetPeerStats stats = {0};
    const bool remote = player != session->local && (session->host || player == WPE_SESSION_HOST);
    if (remote && wpe_network_peer_stats(session->transport, peer, &stats) == WPE_RESULT_OK)
        out_player->rtt_ms = stats.rtt_ms;
    return true;
}

uint32_t wpe_session_end_reason(const WPE_Session* session) {
    return session ? session->end_reason : 0;
}

uint32_t wpe_session_scene_epoch(const WPE_Session* session) {
    return session ? session->scene_epoch : 0;
}

const char* wpe_session_scene(const WPE_Session* session) {
    return session ? session->scene : "";
}

const WPE_NetTransport* wpe_session_transport(const WPE_Session* session) {
    return session ? session->transport : NULL;
}

const char* wpe_session_reason_name(uint32_t reason) {
    switch (reason) {
    case WPE_NET_REASON_NONE:
        return "none";
    case WPE_NET_REASON_CLOSED:
        return "closed";
    case WPE_NET_REASON_TIMEOUT:
        return "timed out";
    case WPE_NET_REASON_CONNECT_FAILED:
        return "no answer";
    case WPE_SESSION_REASON_VERSION:
        return "different engine version";
    case WPE_SESSION_REASON_WRONG_GAME:
        return "different game or build";
    case WPE_SESSION_REASON_FULL:
        return "game full";
    case WPE_SESSION_REASON_NOT_ACCEPTING:
        return "not accepting players";
    case WPE_SESSION_REASON_PROTOCOL:
        return "protocol error";
    default:
        return reason >= WPE_SESSION_REASON_GAME ? "game reason" : "unknown";
    }
}
