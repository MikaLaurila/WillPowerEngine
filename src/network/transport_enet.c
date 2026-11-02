/* The real UDP backend, through ENet: channel 0 sends reliable packets, channel 1 unreliable ones
 * (which ENet sequences: older packets than one already delivered are dropped). Disconnect reasons
 * travel as ENet's disconnect data; 0 there means ENet gave up on the peer (timeout, or no answer
 * to a connection attempt). */
#define WPE_LOG_MODULE "network"
#include <enet/enet.h>
#include <string.h>

#include "core/alloc.h"
#include "core/log.h"
#include "network/transport_internal.h"

typedef enum wpe__EnetPeerState {
    WPE__ENET_FREE = 0,
    WPE__ENET_CONNECTING,
    WPE__ENET_CONNECTED,
    WPE__ENET_CLOSING, /* Disconnected by this side: ENet's final event is not reported. */
} wpe__EnetPeerState;

typedef struct wpe__Enet {
    ENetHost* host;
    uint8_t peer_state[WPE_NET_MAX_PEERS];
    uint32_t last_time; /* enet_time_get() wraps at 2^32 ms; `clock` does not. */
    uint64_t clock;
} wpe__Enet;

/* ---- Global setup ---------------------------------------------------------------------------- */

static int wpe__enet_users = 0;

static void* wpe__enet_malloc(size_t size) {
    return wpe_alloc(size);
}

bool wpe__net_enet_acquire(void) {
    if (wpe__enet_users++ > 0) return true;
    ENetCallbacks callbacks = {.malloc = wpe__enet_malloc, .free = wpe_free, .no_memory = NULL};
    if (enet_initialize_with_callbacks(ENET_VERSION, &callbacks) != 0) {
        wpe__enet_users = 0;
        wpe_log_error("ENet initialization failed");
        return false;
    }
    return true;
}

void wpe__net_enet_release(void) {
    if (wpe__enet_users == 0) return;
    --wpe__enet_users;
    if (wpe__enet_users == 0) enet_deinitialize();
}

bool wpe__net_enet_resolve(const char* name, uint32_t* out_ipv4) {
    if (!wpe__net_enet_acquire()) return false;
    ENetAddress address = {0};
    const bool ok = enet_address_set_host(&address, name) == 0;
    wpe__net_enet_release();
    if (ok) *out_ipv4 = ENET_NET_TO_HOST_32(address.host);
    return ok;
}

/* ---- Datagram sockets (discovery) ------------------------------------------------------------ */

uint64_t wpe__net_enet_clock(void) {
    static uint32_t last = 0;
    static uint64_t clock = 0;
    static bool started = false;
    const uint32_t time = enet_time_get();
    if (started) clock += (uint32_t)(time - last);
    started = true;
    last = time;
    return clock;
}

WPE_Result wpe__net_enet_socket_open(uint16_t port, bool local_only, wpe__NetSocket* out) {
    if (!wpe__net_enet_acquire()) return WPE_RESULT_ERROR_IO;
    ENetSocket* socket = wpe_alloc(sizeof *socket);
    if (!socket) {
        wpe__net_enet_release();
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }
    *socket = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    ENetAddress address = {.host = local_only ? ENET_HOST_TO_NET_32(WPE__NET_LOOPBACK) : ENET_HOST_ANY,
                           .port = port};
    /* Several games on one machine may each listen on the discovery port. */
    const bool ok =
        *socket != ENET_SOCKET_NULL && enet_socket_set_option(*socket, ENET_SOCKOPT_NONBLOCK, 1) == 0 &&
        enet_socket_set_option(*socket, ENET_SOCKOPT_BROADCAST, 1) == 0 &&
        enet_socket_set_option(*socket, ENET_SOCKOPT_REUSEADDR, 1) == 0 &&
        enet_socket_bind(*socket, &address) == 0 && enet_socket_get_address(*socket, &address) == 0;
    if (!ok) {
        if (*socket != ENET_SOCKET_NULL) enet_socket_destroy(*socket);
        wpe_free(socket);
        wpe__net_enet_release();
        wpe_log_error("cannot open UDP port %u for discovery", (unsigned)port);
        return WPE_RESULT_ERROR_IO;
    }
    *out = (wpe__NetSocket){.state = socket, .port = address.port};
    return WPE_RESULT_OK;
}

void wpe__net_enet_socket_close(wpe__NetSocket* s) {
    ENetSocket* socket = s->state;
    enet_socket_destroy(*socket);
    wpe_free(socket);
    s->state = NULL;
    wpe__net_enet_release();
}

bool wpe__net_enet_socket_send(wpe__NetSocket* s, WPE_NetAddress to, const void* data, size_t size) {
    const ENetSocket* socket = s->state;
    const ENetAddress address = {.host = to.ipv4 == WPE__NET_BROADCAST ? ENET_HOST_BROADCAST
                                                                       : ENET_HOST_TO_NET_32(to.ipv4),
                                 .port = to.port};
    ENetBuffer buffer = {.data = (void*)data, .dataLength = size};
    return enet_socket_send(*socket, &address, &buffer, 1) == (int)size;
}

bool wpe__net_enet_socket_receive(wpe__NetSocket* s, void* data, size_t capacity, size_t* out_size,
                                  WPE_NetAddress* out_from) {
    const ENetSocket* socket = s->state;
    ENetAddress from = {0};
    ENetBuffer buffer = {.data = data, .dataLength = capacity};
    /* Errors (such as Windows reporting an unreachable port from an earlier send) just skip. */
    for (int tries = 0; tries < 16; ++tries) {
        const int got = enet_socket_receive(*socket, &from, &buffer, 1);
        if (got == 0) return false;
        if (got < 0) continue;
        *out_size = (size_t)got;
        *out_from = (WPE_NetAddress){.ipv4 = ENET_NET_TO_HOST_32(from.host), .port = from.port};
        return true;
    }
    return false;
}

/* ---- Conditions ------------------------------------------------------------------------------ */

/* The transport being serviced, for the intercept callback (ENet hosts carry no user pointer). */
static thread_local WPE_NetTransport* wpe__receiving = NULL;

/* Sees every received datagram before ENet does: returning 1 drops it, as a lossy link would. */
static int ENET_CALLBACK wpe__intercept(ENetHost* host, ENetEvent* event) {
    (void)host;
    (void)event;
    WPE_NetTransport* t = wpe__receiving;
    return t && t->conditions.loss > 0.0f && wpe__net_random(t) < (double)t->conditions.loss ? 1 : 0;
}

/* ---- Backend --------------------------------------------------------------------------------- */

static uint64_t wpe__enet_now(const WPE_NetTransport* t) {
    wpe__Enet* e = t->state;
    const uint32_t time = enet_time_get();
    e->clock += (uint32_t)(time - e->last_time);
    e->last_time = time;
    return e->clock;
}

static WPE_NetPeerId wpe__peer_id(const wpe__Enet* e, const ENetPeer* peer) {
    return (WPE_NetPeerId)(peer - e->host->peers);
}

static void wpe__enet_count_bytes(WPE_NetTransport* t) {
    wpe__Enet* e = t->state;
    t->stats.bytes_sent += e->host->totalSentData;
    t->stats.bytes_received += e->host->totalReceivedData;
    e->host->totalSentData = 0;
    e->host->totalReceivedData = 0;
}

static void wpe__enet_handle(WPE_NetTransport* t, ENetEvent* event) {
    wpe__Enet* e = t->state;
    const WPE_NetPeerId id = wpe__peer_id(e, event->peer);
    if (id >= t->max_peers) {
        if (event->type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(event->packet);
        return;
    }
    bool queued = true;
    switch (event->type) {
    case ENET_EVENT_TYPE_CONNECT:
        e->peer_state[id] = WPE__ENET_CONNECTED;
        enet_peer_timeout(event->peer, 0, t->timeout_ms, t->timeout_ms);
        queued =
            wpe__net_queue_event(t, WPE_NET_EVENT_CONNECTED, id, WPE_NET_CHANNEL_RELIABLE, 0, NULL, 0, true);
        break;
    case ENET_EVENT_TYPE_DISCONNECT: {
        const uint8_t state = e->peer_state[id];
        e->peer_state[id] = WPE__ENET_FREE;
        if (state == WPE__ENET_CLOSING || state == WPE__ENET_FREE) break;
        uint32_t reason = event->data;
        if (reason == 0)
            reason = state == WPE__ENET_CONNECTED ? WPE_NET_REASON_TIMEOUT : WPE_NET_REASON_CONNECT_FAILED;
        queued = wpe__net_queue_event(t, WPE_NET_EVENT_DISCONNECTED, id, WPE_NET_CHANNEL_RELIABLE, reason,
                                      NULL, 0, true);
        break;
    }
    case ENET_EVENT_TYPE_RECEIVE:
        if (e->peer_state[id] == WPE__ENET_CONNECTED && event->channelID < WPE_NET_CHANNEL_COUNT &&
            event->packet->dataLength > 0 && event->packet->dataLength <= WPE_NET_MAX_MESSAGE_SIZE) {
            queued = wpe__net_queue_event(t, WPE_NET_EVENT_MESSAGE, id, (WPE_NetChannel)event->channelID, 0,
                                          event->packet->data, event->packet->dataLength, true);
        }
        enet_packet_destroy(event->packet);
        break;
    case ENET_EVENT_TYPE_NONE:
        break;
    }
    if (!queued) wpe_log_error("out of memory: a network event was dropped");
}

static void wpe__enet_pump(WPE_NetTransport* t) {
    wpe__Enet* e = t->state;
    wpe__receiving = t;
    ENetEvent event;
    for (int guard = 0; guard < 4096; ++guard) {
        const int got = enet_host_service(e->host, &event, 0);
        if (got < 0) wpe_log_warn("receiving network packets failed");
        if (got <= 0) break;
        wpe__enet_handle(t, &event);
    }
    wpe__receiving = NULL;
    wpe__enet_count_bytes(t);
}

static WPE_Result wpe__enet_send(WPE_NetTransport* t, WPE_NetPeerId peer, WPE_NetChannel channel,
                                 const void* data, size_t size) {
    wpe__Enet* e = t->state;
    if (e->peer_state[peer] != WPE__ENET_CONNECTED) return WPE_RESULT_ERROR_INVALID_STATE;
    const enet_uint32 flags = channel == WPE_NET_CHANNEL_RELIABLE ? ENET_PACKET_FLAG_RELIABLE : 0;
    ENetPacket* packet = enet_packet_create(data, size, flags);
    if (!packet) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    if (enet_peer_send(&e->host->peers[peer], (enet_uint8)channel, packet) != 0) {
        enet_packet_destroy(packet);
        wpe_log_error("cannot queue a %zu-byte network message", size);
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }
    return WPE_RESULT_OK;
}

static void wpe__enet_flush(WPE_NetTransport* t) {
    wpe__Enet* e = t->state;
    enet_host_flush(e->host);
    wpe__enet_count_bytes(t);
}

static void wpe__enet_disconnect(WPE_NetTransport* t, WPE_NetPeerId peer, uint32_t reason) {
    wpe__Enet* e = t->state;
    if (e->peer_state[peer] == WPE__ENET_CONNECTED) {
        enet_peer_disconnect(&e->host->peers[peer], reason); /* After what is already queued. */
        e->peer_state[peer] = WPE__ENET_CLOSING;
    } else {
        enet_peer_reset(&e->host->peers[peer]); /* Still connecting: just give up. */
        e->peer_state[peer] = WPE__ENET_FREE;
    }
}

static bool wpe__enet_is_linked(const WPE_NetTransport* t, WPE_NetPeerId peer) {
    const wpe__Enet* e = t->state;
    return e->peer_state[peer] == WPE__ENET_CONNECTED || e->peer_state[peer] == WPE__ENET_CONNECTING;
}

static bool wpe__enet_peer_stats(const WPE_NetTransport* t, WPE_NetPeerId peer, WPE_NetPeerStats* out) {
    const wpe__Enet* e = t->state;
    const ENetPeer* p = &e->host->peers[peer];
    *out = (WPE_NetPeerStats){.rtt_ms = p->roundTripTime,
                              .packet_loss = (float)p->packetLoss / (float)ENET_PEER_PACKET_LOSS_SCALE};
    return true;
}

static bool wpe__enet_peer_address(const WPE_NetTransport* t, WPE_NetPeerId peer, WPE_NetAddress* out) {
    const wpe__Enet* e = t->state;
    const ENetPeer* p = &e->host->peers[peer];
    *out = (WPE_NetAddress){.ipv4 = ENET_NET_TO_HOST_32(p->address.host), .port = p->address.port};
    return true;
}

static void wpe__enet_destroy(WPE_NetTransport* t) {
    wpe__Enet* e = t->state;
    if (!e) return;
    for (uint32_t i = 0; i < t->max_peers; ++i) {
        if (e->peer_state[i] == WPE__ENET_CONNECTED)
            enet_peer_disconnect_now(&e->host->peers[i], WPE_NET_REASON_CLOSED); /* Sent at once. */
        else if (e->peer_state[i] == WPE__ENET_CONNECTING)
            enet_peer_reset(&e->host->peers[i]);
    }
    enet_host_destroy(e->host);
    wpe_free(e);
    t->state = NULL;
    wpe__net_enet_release();
}

static const wpe__NetBackend wpe__enet_backend = {
    .destroy = wpe__enet_destroy,
    .pump = wpe__enet_pump,
    .now = wpe__enet_now,
    .send = wpe__enet_send,
    .flush = wpe__enet_flush,
    .disconnect = wpe__enet_disconnect,
    .is_linked = wpe__enet_is_linked,
    .peer_stats = wpe__enet_peer_stats,
    .peer_address = wpe__enet_peer_address,
};

/* Makes the ENet host (address NULL: a client). Takes a reference on ENet's global setup. */
static WPE_Result wpe__enet_create(WPE_NetTransport* t, const ENetAddress* address) {
    if (!wpe__net_enet_acquire()) return WPE_RESULT_ERROR_IO;
    wpe__Enet* e = wpe_alloc_zeroed(1, sizeof *e);
    if (!e) {
        wpe__net_enet_release();
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }
    e->host = enet_host_create(address, t->max_peers, WPE_NET_CHANNEL_COUNT, 0, 0);
    if (!e->host) {
        wpe_free(e);
        wpe__net_enet_release();
        if (address)
            wpe_log_error("cannot open UDP port %u (is another game using it?)", (unsigned)address->port);
        else
            wpe_log_error("cannot open a UDP socket");
        return WPE_RESULT_ERROR_IO;
    }
    /* Both sides must compress (the flag is per packet), and every engine transport does. */
    enet_host_compress_with_range_coder(e->host);
    e->host->intercept = wpe__intercept;
    e->last_time = enet_time_get();
    t->state = e;
    t->backend = &wpe__enet_backend;
    t->local_port = e->host->address.port;
    return WPE_RESULT_OK;
}

WPE_Result wpe__net_enet_host(WPE_NetTransport* t, uint16_t port, bool local_only) {
    const ENetAddress address = {.host = local_only ? ENET_HOST_TO_NET_32(0x7F000001u) : ENET_HOST_ANY,
                                 .port = port};
    return wpe__enet_create(t, &address);
}

WPE_Result wpe__net_enet_connect(WPE_NetTransport* t, WPE_NetAddress server) {
    const WPE_Result result = wpe__enet_create(t, NULL);
    if (result != WPE_RESULT_OK) return result;
    wpe__Enet* e = t->state;
    const ENetAddress address = {.host = ENET_HOST_TO_NET_32(server.ipv4), .port = server.port};
    ENetPeer* peer = enet_host_connect(e->host, &address, WPE_NET_CHANNEL_COUNT, 0);
    if (!peer) {
        wpe__enet_destroy(t);
        wpe_log_error("cannot start connecting");
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }
    enet_peer_timeout(peer, 0, t->timeout_ms, t->timeout_ms);
    e->peer_state[wpe__peer_id(e, peer)] = WPE__ENET_CONNECTING;
    enet_host_flush(e->host);
    return WPE_RESULT_OK;
}
