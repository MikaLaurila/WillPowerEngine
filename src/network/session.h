/**
 * @file session.h
 * @brief A game session: one host and up to WPE_SESSION_MAX_PLAYERS - 1 joined players, on top of
 *        a transport. The session handles joining (with version and game checks), player ids and
 *        names, refusals ("full", "wrong game"), kicks, and the scene the host is playing.
 *
 * Players are numbered: the host is player 0 (WPE_SESSION_HOST), joined players get the lowest free
 * id from 1 up, kept until they leave. Every session knows every player's name.
 *
 * Joining, step by step:
 *  1. The player's transport connects to the host.
 *  2. The player sends HELLO: the protocol version, the game id and its name.
 *  3. The host checks them. If something is wrong it disconnects the player with a reason
 *     (WPE_SESSION_REASON_*), which the player gets in an ENDED event. Otherwise it picks an id and
 *     answers WELCOME: the id, the other players and the current scene. Everyone else hears
 *     PLAYER_JOINED.
 *
 * A full host keeps one spare connection to refuse players with; when several try to join a full
 * game at the same moment, the ones beyond that get no answer (ENDED with CONNECT_FAILED) instead.
 *
 * The network is a star: players talk to the host only (the host relays what others must see).
 * Messages carry a `kind`: the session's own use 0..15, layers above (replication, script
 * messages) choose theirs from WPE_SESSION_KIND_FIRST up. Messages from a player before it has
 * joined are dropped.
 *
 * Events (wpe_session_poll):
 * - JOINED: this side is in the game as `player` (on the host, right away).
 * - PLAYER_JOINED / PLAYER_LEFT: someone else came or went (`text` is the name; `reason` why).
 *   A joining player gets PLAYER_JOINED for everyone already there, host included.
 * - SCENE: the host plays the scene `text` now (also sent to every joining player).
 * - MESSAGE: `kind`, `data` and `size` from `player`, on `channel`.
 * - ENDED: this player's session is over (`reason`: refused, kicked, host gone, timed out...).
 *
 * - Dependencies: core, network/transport, network/discovery, network/bits
 * - Thread affinity: a session belongs to one thread (its hub's, if any)
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_NETWORK_SESSION_H
#define WPE_NETWORK_SESSION_H

#include <stddef.h>
#include <stdint.h>

#include "core/result.h"
#include "network/discovery.h"
#include "network/transport.h"

#define WPE_SESSION_MAX_PLAYERS 8u
#define WPE_SESSION_NAME_MAX 32u   /* Player names, including the NUL. */
#define WPE_SESSION_SCENE_MAX 256u /* Scene paths, including the NUL. */
#define WPE_SESSION_PROTOCOL_VERSION 1u
/** The first message kind for layers above the session (up to 255). */
#define WPE_SESSION_KIND_FIRST 16u
#define WPE_SESSION_HOST 0u
#define WPE_SESSION_NO_PLAYER UINT32_MAX

typedef uint32_t WPE_PlayerId;
typedef struct WPE_Session WPE_Session;

/** Why a session refuses, removes or loses a player (besides WPE_NetReason's). */
typedef enum WPE_SessionReason {
    WPE_SESSION_REASON_VERSION = WPE_NET_REASON_USER,   /* Another engine protocol version. */
    WPE_SESSION_REASON_WRONG_GAME,                      /* Another game, or another build of it. */
    WPE_SESSION_REASON_FULL,                            /* No player slot left. */
    WPE_SESSION_REASON_NOT_ACCEPTING,                   /* The host is not taking players now. */
    WPE_SESSION_REASON_PROTOCOL,                        /* A malformed or unexpected message. */
    WPE_SESSION_REASON_GAME = WPE_NET_REASON_USER + 16, /* First reason a game can pass to kick. */
} WPE_SessionReason;

typedef enum WPE_SessionEventType {
    WPE_SESSION_EVENT_NONE = 0,
    WPE_SESSION_EVENT_JOINED,
    WPE_SESSION_EVENT_PLAYER_JOINED,
    WPE_SESSION_EVENT_PLAYER_LEFT,
    WPE_SESSION_EVENT_SCENE,
    WPE_SESSION_EVENT_MESSAGE,
    WPE_SESSION_EVENT_ENDED,
} WPE_SessionEventType;

typedef struct WPE_SessionEvent {
    WPE_SessionEventType type;
    WPE_PlayerId player;
    uint32_t reason;        /* PLAYER_LEFT, ENDED; SCENE: the scene epoch (wpe_session_scene_epoch) */
    uint32_t kind;          /* MESSAGE */
    WPE_NetChannel channel; /* MESSAGE */
    const uint8_t* data;    /* MESSAGE; valid until the next poll */
    size_t size;
    const char* text; /* PLAYER_JOINED: the name; SCENE: the path; valid until the next poll; else "" */
} WPE_SessionEvent;

typedef struct WPE_SessionDesc {
    WPE_NetTransportDesc
        transport;           /* hub, port (host), timeout, conditions, local_only; max_peers is set here. */
    uint32_t max_players;    /* Host: 2..WPE_SESSION_MAX_PLAYERS, the host included; 0 means 4. */
    uint64_t game_id;        /* Only the same id may join: the same game and build. */
    const char* player_name; /* NULL: "Host" or "Player". Truncated to fit; control characters become '?'. */
    const char* game_name;   /* Host: shown by LAN discovery (NULL: "Game"). */
    bool discoverable;       /* Host: answer LAN discovery queries. */
    uint16_t discovery_port; /* 0: WPE_DISCOVERY_DEFAULT_PORT. */
} WPE_SessionDesc;

typedef struct WPE_SessionPlayer {
    char name[WPE_SESSION_NAME_MAX];
    uint32_t rtt_ms; /* Round trip to this player (the host's view; 0 for yourself). */
} WPE_SessionPlayer;

/**
 * Hosts a session; this side is player 0 and gets a JOINED event at the first poll.
 *
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_INVALID_ARG (max_players;
 *         logged); transport and discovery errors (logged).
 *
 * @thread_safety Owning thread
 */
[[nodiscard]] WPE_Result wpe_session_host(const WPE_SessionDesc* desc, WPE_Session** out_session);

/**
 * Joins the session hosted at `address`; JOINED or ENDED follows.
 *
 * @return As wpe_session_host().
 *
 * @thread_safety Owning thread
 */
[[nodiscard]] WPE_Result wpe_session_join(const WPE_SessionDesc* desc, WPE_NetAddress address,
                                          WPE_Session** out_session);

/** Leaves (the others see this player leave, reason CLOSED) and frees the session. NULL is a no-op.
 * @thread_safety Owning thread */
void wpe_session_destroy(WPE_Session* session);

/**
 * Takes the next event (also answers discovery queries on a host).
 *
 * @return false when there is none.
 *
 * @thread_safety Owning thread
 */
bool wpe_session_poll(WPE_Session* session, WPE_SessionEvent* out_event);

/**
 * Sends a message of `kind` (WPE_SESSION_KIND_FIRST..255) to one player. A joined player can only
 * send to the host.
 *
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_INVALID_ARG (kind, or a size
 *         over WPE_NET_MAX_MESSAGE_SIZE - 1; logged); WPE_RESULT_ERROR_INVALID_STATE (not joined,
 *         or no such player); WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Owning thread
 */
WPE_Result wpe_session_send(WPE_Session* session, WPE_PlayerId player, WPE_NetChannel channel, uint32_t kind,
                            const void* data, size_t size);

/** Host: sends to every joined player. @return As wpe_session_send() (INVALID_STATE on a player).
 * @thread_safety Owning thread */
WPE_Result wpe_session_broadcast(WPE_Session* session, WPE_NetChannel channel, uint32_t kind,
                                 const void* data, size_t size);

/**
 * Host: removes a player, who gets ENDED with `reason` (>= WPE_SESSION_REASON_GAME); this side gets
 * PLAYER_LEFT at the next poll.
 *
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_INVALID_ARG (reason);
 *         WPE_RESULT_ERROR_INVALID_STATE (not the host, or no such player).
 *
 * @thread_safety Owning thread
 */
WPE_Result wpe_session_kick(WPE_Session* session, WPE_PlayerId player, uint32_t reason);

/**
 * Host: the scene now played; joined players get a SCENE event, and so does everyone who joins later.
 *
 * @param path A scene path (shorter than WPE_SESSION_SCENE_MAX), or "" for none.
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_INVALID_ARG (too long, or
 *         control characters); WPE_RESULT_ERROR_INVALID_STATE (not the host).
 *
 * @thread_safety Owning thread
 */
WPE_Result wpe_session_set_scene(WPE_Session* session, const char* path);

/** Host: whether new players may join (default true); refusals say NOT_ACCEPTING. @thread_safety Owning
 * thread */
void wpe_session_set_accepting(WPE_Session* session, bool accepting);

/** Sends queued messages now. @thread_safety Owning thread */
void wpe_session_flush(WPE_Session* session);

/** Whether this side hosts. @thread_safety Owning thread */
bool wpe_session_is_host(const WPE_Session* session);

/** This side's player id, or WPE_SESSION_NO_PLAYER until joined (and after ENDED). @thread_safety Owning
 * thread */
WPE_PlayerId wpe_session_local_player(const WPE_Session* session);

/** The player limit, the host included. @thread_safety Owning thread */
uint32_t wpe_session_max_players(const WPE_Session* session);

/** How many players are in the game, this side included (0 before joining). @thread_safety Owning thread */
uint32_t wpe_session_player_count(const WPE_Session* session);

/** A present player's details. @return false if `player` is not in the game. @thread_safety Owning thread */
bool wpe_session_player(const WPE_Session* session, WPE_PlayerId player, WPE_SessionPlayer* out_player);

/** Why a player's session ENDED, or 0 while it runs (always 0 on a host). @thread_safety Owning thread */
uint32_t wpe_session_end_reason(const WPE_Session* session);

/**
 * Counts the host's scene changes: 1 for the first shared scene, then up by one with each
 * wpe_session_set_scene(); 0 before any. Players learn it with each SCENE event (also in the event's
 * `reason` field). Layers above tag their messages with it, so a message about one scene is never
 * applied to the next.
 *
 * @thread_safety Owning thread
 */
uint32_t wpe_session_scene_epoch(const WPE_Session* session);

/** The current scene ("" for none). @thread_safety Owning thread */
const char* wpe_session_scene(const WPE_Session* session);

/** The transport underneath, for statistics. @thread_safety Owning thread */
const WPE_NetTransport* wpe_session_transport(const WPE_Session* session);

/** A readable name for a WPE_NetReason or WPE_SessionReason ("kicked" for WPE_SESSION_REASON_GAME, "game
 * reason" for the game's others).
 * @thread_safety Any thread */
const char* wpe_session_reason_name(uint32_t reason);

#endif /* WPE_NETWORK_SESSION_H */
