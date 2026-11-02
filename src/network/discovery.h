/**
 * @file discovery.h
 * @brief LAN discovery: hosts answer "who is hosting this game?" questions on a well-known UDP
 *        port, so players on the same network find games without typing addresses.
 *
 * A host runs a **beacon** on the discovery port. A player runs a **browser**, which every second
 * broadcasts a query carrying the game's id to that port (and asks 127.0.0.1, for games hosted on
 * the same machine). Beacons of the same game answer with their game port, player counts and names;
 * other games stay silent. The browser lists what answered and forgets a game it has not heard from
 * for WPE_DISCOVERY_EXPIRY_MS.
 *
 * Broadcasts only reach the local network (they do not cross routers), so this finds LAN games
 * only; internet games still need an address. Discovery is best effort: datagrams may be lost, and
 * the next query asks again.
 *
 * On a WPE_NetHub (desc.hub), discovery runs on the in-memory network: a query reaches every
 * beacon on the hub's discovery port, deterministically.
 *
 * - Dependencies: core, network/transport; ENet (private)
 * - Thread affinity: a beacon or browser belongs to one thread (its hub's, if any)
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_NETWORK_DISCOVERY_H
#define WPE_NETWORK_DISCOVERY_H

#include <stddef.h>
#include <stdint.h>

#include "core/result.h"
#include "network/transport.h"

/** The port beacons listen on unless a game chooses another. */
#define WPE_DISCOVERY_DEFAULT_PORT 47809u
#define WPE_DISCOVERY_GAME_NAME_MAX 64u
#define WPE_DISCOVERY_HOST_NAME_MAX 32u
#define WPE_DISCOVERY_MAX_GAMES 32u
/** A browser asks again this often... */
#define WPE_DISCOVERY_QUERY_INTERVAL_MS 1000u
/** ...and drops a game unheard for this long. */
#define WPE_DISCOVERY_EXPIRY_MS 3500u

typedef struct WPE_DiscoveryBeacon WPE_DiscoveryBeacon;
typedef struct WPE_DiscoveryBrowser WPE_DiscoveryBrowser;

typedef struct WPE_DiscoveryDesc {
    WPE_NetHub* hub;  /* NULL: real UDP; otherwise the in-memory network. Borrowed. */
    uint16_t port;    /* The discovery port; 0 means WPE_DISCOVERY_DEFAULT_PORT. */
    bool local_only;  /* Only this machine (127.0.0.1): tests, and no firewall prompt. */
    uint64_t game_id; /* Only beacons and browsers of the same game talk. */
} WPE_DiscoveryDesc;

/** What a beacon tells browsers about its game. */
typedef struct WPE_DiscoveryInfo {
    uint16_t game_port; /* Where to connect. */
    uint8_t players;
    uint8_t max_players;
    bool accepting; /* Whether a join would be accepted now. */
    char game_name[WPE_DISCOVERY_GAME_NAME_MAX];
    char host_name[WPE_DISCOVERY_HOST_NAME_MAX];
} WPE_DiscoveryInfo;

/** A game a browser has found. */
typedef struct WPE_DiscoveredGame {
    WPE_NetAddress address; /* The host's address and game port: pass it to connect. */
    WPE_DiscoveryInfo info;
    uint64_t last_seen_ms; /* On the browser's clock. */
} WPE_DiscoveredGame;

/**
 * Starts answering queries.
 *
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_IO (the port cannot be opened;
 *         logged); WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Owning thread
 */
[[nodiscard]] WPE_Result wpe_discovery_beacon_create(const WPE_DiscoveryDesc* desc,
                                                     const WPE_DiscoveryInfo* info,
                                                     WPE_DiscoveryBeacon** out_beacon);

/** Stops answering and frees the beacon; NULL is a no-op. @thread_safety Owning thread */
void wpe_discovery_beacon_destroy(WPE_DiscoveryBeacon* beacon);

/** Changes what later answers say (names are truncated to fit). @thread_safety Owning thread */
void wpe_discovery_beacon_set_info(WPE_DiscoveryBeacon* beacon, const WPE_DiscoveryInfo* info);

/** Answers the queries that have arrived; call once per frame. @thread_safety Owning thread */
void wpe_discovery_beacon_update(WPE_DiscoveryBeacon* beacon);

/**
 * Starts looking for games (the first query goes out at the first update).
 *
 * @return As wpe_discovery_beacon_create().
 *
 * @thread_safety Owning thread
 */
[[nodiscard]] WPE_Result wpe_discovery_browser_create(const WPE_DiscoveryDesc* desc,
                                                      WPE_DiscoveryBrowser** out_browser);

/** Stops looking and frees the browser; NULL is a no-op. @thread_safety Owning thread */
void wpe_discovery_browser_destroy(WPE_DiscoveryBrowser* browser);

/** Sends queries when due, takes answers and drops games gone quiet; call once per frame. @thread_safety
 * Owning thread */
void wpe_discovery_browser_update(WPE_DiscoveryBrowser* browser);

/** Asks again at the next update instead of waiting for the interval. @thread_safety Owning thread */
void wpe_discovery_browser_refresh(WPE_DiscoveryBrowser* browser);

/** How many games are listed. @thread_safety Owning thread */
uint32_t wpe_discovery_browser_count(const WPE_DiscoveryBrowser* browser);

/**
 * A listed game, in the order they were found.
 *
 * @return false past the end.
 *
 * @thread_safety Owning thread
 */
bool wpe_discovery_browser_game(const WPE_DiscoveryBrowser* browser, uint32_t index,
                                WPE_DiscoveredGame* out_game);

#endif /* WPE_NETWORK_DISCOVERY_H */
