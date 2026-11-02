/* LAN discovery: a query/answer protocol over datagram sockets (see discovery.h).
 *
 * Datagram layout (bits.h):
 *   magic "WPED" (32 bits), version (8), kind (8: 1 query, 2 answer), game id (64)
 *   answer only: beacon nonce (32), game port (16), players (8), max players (8), accepting (1),
 *                game name and host name (varint length + bytes each)
 * The nonce tells answers of one beacon apart from another's: a host on this machine answers both
 * the broadcast and the 127.0.0.1 query, from two addresses, and is still listed once. */
#define WPE_LOG_MODULE "network"
#include "network/discovery.h"

#include <string.h>

#include "core/alloc.h"
#include "core/log.h"
#include "network/bits.h"
#include "network/transport_internal.h"

#define WPE__DISCOVERY_MAGIC 0x44455057u /* "WPED" read little-endian */
#define WPE__DISCOVERY_VERSION 1u
#define WPE__KIND_QUERY 1u
#define WPE__KIND_ANSWER 2u
#define WPE__DATAGRAM_MAX 256u
#define WPE__RECEIVES_PER_UPDATE 64

/* ---- Datagram sockets ------------------------------------------------------------------------ */

WPE_Result wpe__net_socket_open(WPE_NetHub* hub, uint16_t port, bool local_only, wpe__NetSocket* out) {
    *out = (wpe__NetSocket){0};
    return hub ? wpe__net_memory_socket_open(hub, port, out)
               : wpe__net_enet_socket_open(port, local_only, out);
}

void wpe__net_socket_close(wpe__NetSocket* s) {
    if (!s->state) return;
    if (s->hub)
        wpe__net_memory_socket_close(s);
    else
        wpe__net_enet_socket_close(s);
}

bool wpe__net_socket_send(wpe__NetSocket* s, WPE_NetAddress to, const void* data, size_t size) {
    return s->hub ? wpe__net_memory_socket_send(s, data, size, to.port)
                  : wpe__net_enet_socket_send(s, to, data, size);
}

bool wpe__net_socket_receive(wpe__NetSocket* s, void* buffer, size_t capacity, size_t* out_size,
                             WPE_NetAddress* out_from) {
    return s->hub ? wpe__net_memory_socket_receive(s, buffer, capacity, out_size, out_from)
                  : wpe__net_enet_socket_receive(s, buffer, capacity, out_size, out_from);
}

uint64_t wpe__net_socket_now(const wpe__NetSocket* s) {
    return s->hub ? wpe_network_hub_now(s->hub) : wpe__net_enet_clock();
}

/* ---- Wire format ----------------------------------------------------------------------------- */

static void wpe__write_header(WPE_BitWriter* w, uint32_t kind, uint64_t game_id) {
    wpe_bits_write(w, WPE__DISCOVERY_MAGIC, 32);
    wpe_bits_write(w, WPE__DISCOVERY_VERSION, 8);
    wpe_bits_write(w, kind, 8);
    wpe_bits_write(w, (uint32_t)game_id, 32);
    wpe_bits_write(w, (uint32_t)(game_id >> 32), 32);
}

/* Reads a header; false unless it is ours, of `kind`, for `game_id`. */
static bool wpe__read_header(WPE_BitReader* r, uint32_t kind, uint64_t game_id) {
    if (wpe_bits_read(r, 32) != WPE__DISCOVERY_MAGIC || wpe_bits_read(r, 8) != WPE__DISCOVERY_VERSION ||
        wpe_bits_read(r, 8) != kind)
        return false;
    const uint64_t low = wpe_bits_read(r, 32);
    const uint64_t high = wpe_bits_read(r, 32);
    return !r->error && (low | high << 32) == game_id;
}

/* strlen, at most `max` (strnlen is POSIX, not ISO C). */
static size_t wpe__text_length(const char* text, size_t max) {
    size_t length = 0;
    while (length < max && text[length])
        ++length;
    return length;
}

static void wpe__write_text(WPE_BitWriter* w, const char* text, size_t max) {
    const size_t length = wpe__text_length(text, max - 1);
    wpe_bits_write_varint(w, (uint32_t)length);
    wpe_bits_write_bytes(w, text, length);
}

/* Reads a name into `out` (max bytes, NUL-terminated); control characters become '?'. */
static bool wpe__read_text(WPE_BitReader* r, char* out, size_t max) {
    const uint32_t length = wpe_bits_read_varint(r);
    if (r->error || length >= max) return false;
    const uint8_t* bytes = wpe_bits_read_bytes(r, length);
    if (!bytes) return false;
    for (uint32_t i = 0; i < length; ++i)
        out[i] = bytes[i] < 0x20u || bytes[i] == 0x7Fu ? '?' : (char)bytes[i];
    out[length] = '\0';
    return true;
}

static void wpe__copy_text(char* out, const char* text, size_t max) {
    const size_t length = text ? wpe__text_length(text, max - 1) : 0;
    if (length) memcpy(out, text, length);
    out[length] = '\0';
}

/* ---- Beacon ---------------------------------------------------------------------------------- */

struct WPE_DiscoveryBeacon {
    wpe__NetSocket socket;
    uint64_t game_id;
    uint32_t nonce;
    WPE_DiscoveryInfo info;
};

static uint32_t wpe__new_nonce(uint64_t now, uint16_t port) {
    static uint32_t counter = 0;
    uint64_t z = now ^ ((uint64_t)port << 32) ^ ((uint64_t)++counter * 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return (uint32_t)(z ^ (z >> 31));
}

static uint16_t wpe__discovery_port(const WPE_DiscoveryDesc* desc) {
    return desc->port ? desc->port : (uint16_t)WPE_DISCOVERY_DEFAULT_PORT;
}

WPE_Result wpe_discovery_beacon_create(const WPE_DiscoveryDesc* desc, const WPE_DiscoveryInfo* info,
                                       WPE_DiscoveryBeacon** out_beacon) {
    if (!desc || !info || !out_beacon) return WPE_RESULT_ERROR_NULL_ARG;
    *out_beacon = NULL;
    WPE_DiscoveryBeacon* b = wpe_alloc_zeroed(1, sizeof *b);
    if (!b) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    const WPE_Result result =
        wpe__net_socket_open(desc->hub, wpe__discovery_port(desc), desc->local_only, &b->socket);
    if (result != WPE_RESULT_OK) {
        wpe_free(b);
        return result;
    }
    b->game_id = desc->game_id;
    b->nonce = wpe__new_nonce(wpe__net_socket_now(&b->socket), b->socket.port);
    wpe_discovery_beacon_set_info(b, info);
    *out_beacon = b;
    return WPE_RESULT_OK;
}

void wpe_discovery_beacon_destroy(WPE_DiscoveryBeacon* beacon) {
    if (!beacon) return;
    wpe__net_socket_close(&beacon->socket);
    wpe_free(beacon);
}

void wpe_discovery_beacon_set_info(WPE_DiscoveryBeacon* beacon, const WPE_DiscoveryInfo* info) {
    if (!beacon || !info) return;
    beacon->info = *info;
    wpe__copy_text(beacon->info.game_name, info->game_name, sizeof beacon->info.game_name);
    wpe__copy_text(beacon->info.host_name, info->host_name, sizeof beacon->info.host_name);
}

void wpe_discovery_beacon_update(WPE_DiscoveryBeacon* beacon) {
    if (!beacon) return;
    uint8_t datagram[WPE__DATAGRAM_MAX];
    for (int i = 0; i < WPE__RECEIVES_PER_UPDATE; ++i) {
        size_t size = 0;
        WPE_NetAddress from = {0};
        if (!wpe__net_socket_receive(&beacon->socket, datagram, sizeof datagram, &size, &from)) break;
        WPE_BitReader r = wpe_bits_reader(datagram, size);
        if (!wpe__read_header(&r, WPE__KIND_QUERY, beacon->game_id)) continue;
        uint8_t answer[WPE__DATAGRAM_MAX];
        WPE_BitWriter w = wpe_bits_writer(answer, sizeof answer);
        const WPE_DiscoveryInfo* info = &beacon->info;
        wpe__write_header(&w, WPE__KIND_ANSWER, beacon->game_id);
        wpe_bits_write(&w, beacon->nonce, 32);
        wpe_bits_write(&w, info->game_port, 16);
        wpe_bits_write(&w, info->players, 8);
        wpe_bits_write(&w, info->max_players, 8);
        wpe_bits_write_bool(&w, info->accepting);
        wpe__write_text(&w, info->game_name, sizeof info->game_name);
        wpe__write_text(&w, info->host_name, sizeof info->host_name);
        if (!w.error) (void)wpe__net_socket_send(&beacon->socket, from, answer, wpe_bits_writer_size(&w));
    }
}

/* ---- Browser --------------------------------------------------------------------------------- */

typedef struct wpe__Found {
    WPE_DiscoveredGame game;
    uint32_t nonce;
} wpe__Found;

struct WPE_DiscoveryBrowser {
    wpe__NetSocket socket;
    uint64_t game_id;
    uint16_t port; /* The discovery port asked. */
    bool local_only;
    bool asked;
    uint64_t next_query_ms;
    wpe__Found found[WPE_DISCOVERY_MAX_GAMES];
    uint32_t count;
};

WPE_Result wpe_discovery_browser_create(const WPE_DiscoveryDesc* desc, WPE_DiscoveryBrowser** out_browser) {
    if (!desc || !out_browser) return WPE_RESULT_ERROR_NULL_ARG;
    *out_browser = NULL;
    WPE_DiscoveryBrowser* b = wpe_alloc_zeroed(1, sizeof *b);
    if (!b) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    const WPE_Result result = wpe__net_socket_open(desc->hub, 0, desc->local_only, &b->socket);
    if (result != WPE_RESULT_OK) {
        wpe_free(b);
        return result;
    }
    b->game_id = desc->game_id;
    b->port = wpe__discovery_port(desc);
    b->local_only = desc->local_only;
    *out_browser = b;
    return WPE_RESULT_OK;
}

void wpe_discovery_browser_destroy(WPE_DiscoveryBrowser* browser) {
    if (!browser) return;
    wpe__net_socket_close(&browser->socket);
    wpe_free(browser);
}

void wpe_discovery_browser_refresh(WPE_DiscoveryBrowser* browser) {
    if (browser) browser->asked = false;
}

static void wpe__send_queries(WPE_DiscoveryBrowser* b) {
    uint8_t query[32];
    WPE_BitWriter w = wpe_bits_writer(query, sizeof query);
    wpe__write_header(&w, WPE__KIND_QUERY, b->game_id);
    const size_t size = wpe_bits_writer_size(&w);
    if (!b->local_only)
        (void)wpe__net_socket_send(&b->socket, (WPE_NetAddress){.ipv4 = WPE__NET_BROADCAST, .port = b->port},
                                   query, size);
    /* Broadcasts do not always come back to the sending machine; ask it directly too. On a hub one
     * send reaches every beacon already. */
    if (!b->socket.hub || b->local_only)
        (void)wpe__net_socket_send(&b->socket, (WPE_NetAddress){.ipv4 = WPE__NET_LOOPBACK, .port = b->port},
                                   query, size);
}

static void wpe__take_answer(WPE_DiscoveryBrowser* b, const uint8_t* data, size_t size, WPE_NetAddress from,
                             uint64_t now) {
    WPE_BitReader r = wpe_bits_reader(data, size);
    if (!wpe__read_header(&r, WPE__KIND_ANSWER, b->game_id)) return;
    const uint32_t nonce = wpe_bits_read(&r, 32);
    WPE_DiscoveryInfo info = {0};
    info.game_port = (uint16_t)wpe_bits_read(&r, 16);
    info.players = (uint8_t)wpe_bits_read(&r, 8);
    info.max_players = (uint8_t)wpe_bits_read(&r, 8);
    info.accepting = wpe_bits_read_bool(&r);
    if (!wpe__read_text(&r, info.game_name, sizeof info.game_name) ||
        !wpe__read_text(&r, info.host_name, sizeof info.host_name) || r.error || info.game_port == 0)
        return;
    const WPE_NetAddress address = {.ipv4 = from.ipv4, .port = info.game_port};
    for (uint32_t i = 0; i < b->count; ++i) {
        wpe__Found* f = &b->found[i];
        if (f->nonce != nonce) continue;
        /* The same host seen again (maybe from its other address): a LAN address beats 127.0.0.1,
         * which only works on this machine. */
        if (f->game.address.ipv4 == WPE__NET_LOOPBACK || address.ipv4 != WPE__NET_LOOPBACK)
            f->game.address = address;
        f->game.info = info;
        f->game.last_seen_ms = now;
        return;
    }
    if (b->count == WPE_DISCOVERY_MAX_GAMES) return;
    b->found[b->count++] =
        (wpe__Found){.game = {.address = address, .info = info, .last_seen_ms = now}, .nonce = nonce};
}

void wpe_discovery_browser_update(WPE_DiscoveryBrowser* browser) {
    if (!browser) return;
    const uint64_t now = wpe__net_socket_now(&browser->socket);
    if (!browser->asked || now >= browser->next_query_ms) {
        wpe__send_queries(browser);
        browser->asked = true;
        browser->next_query_ms = now + WPE_DISCOVERY_QUERY_INTERVAL_MS;
    }
    uint8_t datagram[WPE__DATAGRAM_MAX];
    for (int i = 0; i < WPE__RECEIVES_PER_UPDATE; ++i) {
        size_t size = 0;
        WPE_NetAddress from = {0};
        if (!wpe__net_socket_receive(&browser->socket, datagram, sizeof datagram, &size, &from)) break;
        wpe__take_answer(browser, datagram, size, from, now);
    }
    uint32_t kept = 0;
    for (uint32_t i = 0; i < browser->count; ++i)
        if (now - browser->found[i].game.last_seen_ms < WPE_DISCOVERY_EXPIRY_MS)
            browser->found[kept++] = browser->found[i];
    browser->count = kept;
}

uint32_t wpe_discovery_browser_count(const WPE_DiscoveryBrowser* browser) {
    return browser ? browser->count : 0;
}

bool wpe_discovery_browser_game(const WPE_DiscoveryBrowser* browser, uint32_t index,
                                WPE_DiscoveredGame* out_game) {
    if (!browser || !out_game || index >= browser->count) return false;
    *out_game = browser->found[index].game;
    return true;
}
