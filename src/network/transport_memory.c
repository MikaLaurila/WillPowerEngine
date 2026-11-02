/* The in-memory backend: a WPE_NetHub is a pretend network whose clock only moves when asked
 * (wpe_network_hub_advance), so tests of networked games run the same way every time.
 *
 * Each transport is an endpoint with a port and a slot per peer (a link). Packets between links are
 * held by the hub until they are due, then handed to the receiving transport's event queue. The
 * receiver's simulated conditions decide when a packet is due and whether it is lost:
 * - unreliable messages may be lost, and one older than a message already delivered is dropped;
 * - reliable messages and connection packets are never lost, but each loss delays them by a round
 *   trip (a resend), and they never overtake one another on a link.
 * A connection that stops hearing anything (wpe_network_hub_cut) times out after timeout_ms. */
#define WPE_LOG_MODULE "network"
#include <string.h>

#include "core/alloc.h"
#include "core/debug.h"
#include "core/log.h"
#include "network/transport_internal.h"

#define WPE__MEM_HEADER_BYTES 8u
#define WPE__MEM_FIRST_PORT 50000u
#define WPE__MEM_LOOPBACK 0x7F000001u

typedef enum wpe__MemKind {
    WPE__MEM_CONNECT,
    WPE__MEM_ACCEPT,
    WPE__MEM_DATA,
    WPE__MEM_DISCONNECT,
} wpe__MemKind;

typedef struct wpe__MemPacket {
    uint64_t deliver_ms;
    uint64_t order; /* Ties at the same time go in sending order. */
    wpe__MemKind kind;
    uint32_t from_endpoint;
    uint32_t from_slot;
    uint32_t to_endpoint;
    uint32_t to_slot; /* ACCEPT only: the connecting link. */
    WPE_NetChannel channel;
    uint32_t sequence; /* Unreliable DATA. */
    uint32_t reason;   /* DISCONNECT. */
    uint8_t* data;     /* Owned. */
    size_t size;
} wpe__MemPacket;

typedef enum wpe__MemLinkState {
    WPE__LINK_FREE = 0,
    WPE__LINK_CONNECTING,
    WPE__LINK_CONNECTED,
} wpe__MemLinkState;

typedef struct wpe__MemLink {
    uint8_t state;
    uint32_t remote_endpoint;
    uint32_t remote_slot;
    uint64_t started_ms;    /* When connecting began. */
    uint64_t silent_since;  /* 0: hearing the peer; otherwise when it went silent (+1). */
    uint64_t reliable_tail; /* Latest delivery time of a reliable packet sent on this link. */
    uint32_t next_sequence; /* Next unreliable message sent. */
    uint32_t last_sequence; /* Newest unreliable message received... */
    bool received_any;      /* ...if any. */
} wpe__MemLink;

typedef struct wpe__MemEndpoint {
    WPE_NetHub* hub;
    WPE_NetTransport* transport;
    uint32_t id;
    uint16_t port;
    bool cut;
    uint64_t cut_ms;
    wpe__MemLink links[WPE_NET_MAX_PEERS];
} wpe__MemEndpoint;

struct WPE_NetHub {
    uint64_t now;
    uint32_t next_id;
    uint64_t next_order;
    wpe__MemEndpoint** endpoints;
    size_t endpoint_count;
    size_t endpoint_capacity;
    wpe__MemPacket* packets;
    size_t packet_count;
    size_t packet_capacity;
};

/* ---- Helpers --------------------------------------------------------------------------------- */

static wpe__MemEndpoint* wpe__find_endpoint(const WPE_NetHub* hub, uint32_t id) {
    for (size_t i = 0; i < hub->endpoint_count; ++i)
        if (hub->endpoints[i]->id == id) return hub->endpoints[i];
    return NULL;
}

static wpe__MemEndpoint* wpe__find_port(const WPE_NetHub* hub, uint16_t port) {
    for (size_t i = 0; i < hub->endpoint_count; ++i)
        if (hub->endpoints[i]->port == port) return hub->endpoints[i];
    return NULL;
}

/* The link of `e` connected (or connecting) to the remote link (endpoint, slot). */
static wpe__MemLink* wpe__find_link(wpe__MemEndpoint* e, uint32_t endpoint, uint32_t slot) {
    for (uint32_t i = 0; i < e->transport->max_peers; ++i) {
        wpe__MemLink* link = &e->links[i];
        if (link->state != WPE__LINK_FREE && link->remote_endpoint == endpoint && link->remote_slot == slot)
            return link;
    }
    return NULL;
}

static WPE_NetPeerId wpe__slot_of(const wpe__MemEndpoint* e, const wpe__MemLink* link) {
    return (WPE_NetPeerId)(link - e->links);
}

static void wpe__queue(wpe__MemEndpoint* e, WPE_NetEventType type, WPE_NetPeerId peer, WPE_NetChannel channel,
                       uint32_t reason, const void* data, size_t size) {
    if (!wpe__net_queue_event(e->transport, type, peer, channel, reason, data, size, false))
        wpe_log_error("out of memory: a network event was dropped");
}

/* Puts a packet on the wire from link `from_slot` of `from`. `data` is copied. */
static void wpe__post(wpe__MemEndpoint* from, uint32_t from_slot, uint32_t to_endpoint, wpe__MemPacket packet,
                      const void* data) {
    WPE_NetHub* hub = from->hub;
    if (packet.kind == WPE__MEM_DATA)
        from->transport->stats.bytes_sent += packet.size + WPE__MEM_HEADER_BYTES;
    wpe__MemEndpoint* to = wpe__find_endpoint(hub, to_endpoint);
    if (from->cut || !to || to->cut) return; /* Lost on the way. */
    WPE_NetTransport* rt = to->transport;    /* The receiver's conditions apply. */
    const bool reliable = packet.kind != WPE__MEM_DATA || packet.channel == WPE_NET_CHANNEL_RELIABLE;
    uint64_t deliver = hub->now + wpe__net_random_delay(rt);
    if (reliable) {
        /* A lost reliable packet is resent once the sender notices: about a round trip later. */
        const uint64_t resend = rt->conditions.latency_ms * 2u + 10u;
        for (int attempt = 0;
             attempt < 16 && rt->conditions.loss > 0.0f && wpe__net_random(rt) < (double)rt->conditions.loss;
             ++attempt)
            deliver += resend;
        wpe__MemLink* link = &from->links[from_slot];
        if (deliver < link->reliable_tail) deliver = link->reliable_tail;
        link->reliable_tail = deliver;
    } else if (rt->conditions.loss > 0.0f && wpe__net_random(rt) < (double)rt->conditions.loss) {
        return;
    }
    if (hub->packet_count == hub->packet_capacity) {
        const size_t capacity = hub->packet_capacity ? hub->packet_capacity * 2 : 64;
        wpe__MemPacket* grown = wpe_realloc(hub->packets, capacity * sizeof *grown);
        if (!grown) {
            wpe_log_error("out of memory: a network packet was dropped");
            return;
        }
        hub->packets = grown;
        hub->packet_capacity = capacity;
    }
    packet.data = NULL;
    if (packet.size > 0) {
        packet.data = wpe_alloc(packet.size);
        if (!packet.data) return;
        memcpy(packet.data, data, packet.size);
    }
    packet.deliver_ms = deliver;
    packet.order = hub->next_order++;
    packet.from_endpoint = from->id;
    packet.from_slot = from_slot;
    packet.to_endpoint = to_endpoint;
    hub->packets[hub->packet_count++] = packet;
}

static void wpe__post_disconnect(wpe__MemEndpoint* e, uint32_t slot, uint32_t reason) {
    const wpe__MemLink* link = &e->links[slot];
    wpe__post(e, slot, link->remote_endpoint, (wpe__MemPacket){.kind = WPE__MEM_DISCONNECT, .reason = reason},
              NULL);
}

/* ---- Delivery -------------------------------------------------------------------------------- */

static void wpe__deliver(WPE_NetHub* hub, const wpe__MemPacket* p) {
    wpe__MemEndpoint* to = wpe__find_endpoint(hub, p->to_endpoint);
    if (!to || to->cut) return;
    switch (p->kind) {
    case WPE__MEM_CONNECT: {
        if (!to->transport->server || wpe__find_link(to, p->from_endpoint, p->from_slot)) return;
        for (uint32_t i = 0; i < to->transport->max_peers; ++i) {
            wpe__MemLink* link = &to->links[i];
            if (link->state != WPE__LINK_FREE) continue;
            *link = (wpe__MemLink){.state = WPE__LINK_CONNECTED,
                                   .remote_endpoint = p->from_endpoint,
                                   .remote_slot = p->from_slot};
            wpe__queue(to, WPE_NET_EVENT_CONNECTED, i, WPE_NET_CHANNEL_RELIABLE, 0, NULL, 0);
            wpe__post(to, i, p->from_endpoint,
                      (wpe__MemPacket){.kind = WPE__MEM_ACCEPT, .to_slot = p->from_slot}, NULL);
            return;
        }
        return; /* Full: no answer, so the client's attempt fails, as over UDP. */
    }
    case WPE__MEM_ACCEPT: {
        if (p->to_slot >= to->transport->max_peers || !wpe__find_endpoint(hub, p->from_endpoint)) return;
        wpe__MemLink* link = &to->links[p->to_slot];
        if (link->state != WPE__LINK_CONNECTING || link->remote_endpoint != p->from_endpoint) return;
        link->state = WPE__LINK_CONNECTED;
        link->remote_slot = p->from_slot;
        wpe__queue(to, WPE_NET_EVENT_CONNECTED, p->to_slot, WPE_NET_CHANNEL_RELIABLE, 0, NULL, 0);
        return;
    }
    case WPE__MEM_DATA: {
        wpe__MemLink* link = wpe__find_link(to, p->from_endpoint, p->from_slot);
        if (!link || link->state != WPE__LINK_CONNECTED) return;
        if (p->channel == WPE_NET_CHANNEL_UNRELIABLE) {
            if (link->received_any && (int32_t)(p->sequence - link->last_sequence) <= 0) return; /* Stale. */
            link->received_any = true;
            link->last_sequence = p->sequence;
        }
        to->transport->stats.bytes_received += p->size + WPE__MEM_HEADER_BYTES;
        wpe__queue(to, WPE_NET_EVENT_MESSAGE, wpe__slot_of(to, link), p->channel, 0, p->data, p->size);
        return;
    }
    case WPE__MEM_DISCONNECT: {
        wpe__MemLink* link = wpe__find_link(to, p->from_endpoint, p->from_slot);
        if (!link) return;
        const bool was_connected = link->state == WPE__LINK_CONNECTED;
        link->state = WPE__LINK_FREE;
        wpe__queue(to, WPE_NET_EVENT_DISCONNECTED, wpe__slot_of(to, link), WPE_NET_CHANNEL_RELIABLE,
                   was_connected ? p->reason : WPE_NET_REASON_CONNECT_FAILED, NULL, 0);
        return;
    }
    }
}

/* Connections that hear nothing: connecting ones fail, connected ones time out. */
static void wpe__check_timeouts(WPE_NetHub* hub) {
    for (size_t i = 0; i < hub->endpoint_count; ++i) {
        wpe__MemEndpoint* e = hub->endpoints[i];
        const uint64_t timeout = e->transport->timeout_ms;
        for (uint32_t s = 0; s < e->transport->max_peers; ++s) {
            wpe__MemLink* link = &e->links[s];
            if (link->state == WPE__LINK_CONNECTING) {
                if (hub->now - link->started_ms >= timeout) {
                    link->state = WPE__LINK_FREE;
                    wpe__queue(e, WPE_NET_EVENT_DISCONNECTED, s, WPE_NET_CHANNEL_RELIABLE,
                               WPE_NET_REASON_CONNECT_FAILED, NULL, 0);
                }
                continue;
            }
            if (link->state != WPE__LINK_CONNECTED) continue;
            const wpe__MemEndpoint* remote = wpe__find_endpoint(hub, link->remote_endpoint);
            uint64_t silent = 0; /* When this link stopped hearing its peer, +1. */
            if (e->cut)
                silent = e->cut_ms + 1;
            else if (!remote)
                silent = hub->now + 1;
            else if (remote->cut)
                silent = remote->cut_ms + 1;
            if (silent == 0) {
                link->silent_since = 0;
                continue;
            }
            if (link->silent_since == 0) link->silent_since = silent;
            if (hub->now + 1 - link->silent_since >= timeout) {
                link->state = WPE__LINK_FREE;
                wpe__queue(e, WPE_NET_EVENT_DISCONNECTED, s, WPE_NET_CHANNEL_RELIABLE, WPE_NET_REASON_TIMEOUT,
                           NULL, 0);
            }
        }
    }
}

/* ---- Backend --------------------------------------------------------------------------------- */

static uint64_t wpe__mem_now(const WPE_NetTransport* t) {
    const wpe__MemEndpoint* e = t->state;
    return e->hub->now;
}

static WPE_Result wpe__mem_send(WPE_NetTransport* t, WPE_NetPeerId peer, WPE_NetChannel channel,
                                const void* data, size_t size) {
    wpe__MemEndpoint* e = t->state;
    wpe__MemLink* link = &e->links[peer];
    if (link->state != WPE__LINK_CONNECTED) return WPE_RESULT_ERROR_INVALID_STATE;
    const wpe__MemPacket packet = {
        .kind = WPE__MEM_DATA, .channel = channel, .sequence = link->next_sequence, .size = size};
    if (channel == WPE_NET_CHANNEL_UNRELIABLE) ++link->next_sequence;
    wpe__post(e, peer, link->remote_endpoint, packet, data);
    return WPE_RESULT_OK;
}

static void wpe__mem_disconnect(WPE_NetTransport* t, WPE_NetPeerId peer, uint32_t reason) {
    wpe__MemEndpoint* e = t->state;
    wpe__post_disconnect(e, peer, reason);
    e->links[peer].state = WPE__LINK_FREE;
}

static bool wpe__mem_is_linked(const WPE_NetTransport* t, WPE_NetPeerId peer) {
    const wpe__MemEndpoint* e = t->state;
    return e->links[peer].state != WPE__LINK_FREE;
}

static bool wpe__mem_peer_stats(const WPE_NetTransport* t, WPE_NetPeerId peer, WPE_NetPeerStats* out) {
    const wpe__MemEndpoint* e = t->state;
    const wpe__MemEndpoint* remote = wpe__find_endpoint(e->hub, e->links[peer].remote_endpoint);
    const WPE_NetConditions* mine = &t->conditions;
    const WPE_NetConditions* theirs = remote ? &remote->transport->conditions : mine;
    const uint32_t rtt = mine->latency_ms + mine->jitter_ms / 2 + theirs->latency_ms + theirs->jitter_ms / 2;
    *out = (WPE_NetPeerStats){.rtt_ms = rtt, .packet_loss = theirs->loss};
    return true;
}

static bool wpe__mem_peer_address(const WPE_NetTransport* t, WPE_NetPeerId peer, WPE_NetAddress* out) {
    const wpe__MemEndpoint* e = t->state;
    const wpe__MemEndpoint* remote = wpe__find_endpoint(e->hub, e->links[peer].remote_endpoint);
    if (!remote) return false;
    *out = (WPE_NetAddress){.ipv4 = WPE__MEM_LOOPBACK, .port = remote->port};
    return true;
}

static void wpe__mem_destroy(WPE_NetTransport* t) {
    wpe__MemEndpoint* e = t->state;
    if (!e) return;
    WPE_NetHub* hub = e->hub;
    for (uint32_t s = 0; s < t->max_peers; ++s)
        if (e->links[s].state != WPE__LINK_FREE) wpe__post_disconnect(e, s, WPE_NET_REASON_CLOSED);
    for (size_t i = 0; i < hub->endpoint_count; ++i) {
        if (hub->endpoints[i] != e) continue;
        hub->endpoints[i] = hub->endpoints[--hub->endpoint_count];
        break;
    }
    wpe_free(e);
    t->state = NULL;
}

static const wpe__NetBackend wpe__memory_backend = {
    .destroy = wpe__mem_destroy,
    .pump = NULL, /* The hub delivers. */
    .now = wpe__mem_now,
    .send = wpe__mem_send,
    .flush = NULL,
    .disconnect = wpe__mem_disconnect,
    .is_linked = wpe__mem_is_linked,
    .peer_stats = wpe__mem_peer_stats,
    .peer_address = wpe__mem_peer_address,
};

static WPE_Result wpe__mem_attach(WPE_NetTransport* t, WPE_NetHub* hub, uint16_t port) {
    if (port == 0) {
        for (uint32_t p = WPE__MEM_FIRST_PORT; p <= 65535u && port == 0; ++p)
            if (!wpe__find_port(hub, (uint16_t)p)) port = (uint16_t)p;
        if (port == 0) return WPE_RESULT_ERROR_CAPACITY;
    } else if (wpe__find_port(hub, port)) {
        wpe_log_error("port %u is already in use on this network hub", (unsigned)port);
        return WPE_RESULT_ERROR_ALREADY_EXISTS;
    }
    if (hub->endpoint_count == hub->endpoint_capacity) {
        const size_t capacity = hub->endpoint_capacity ? hub->endpoint_capacity * 2 : 8;
        wpe__MemEndpoint** grown = wpe_realloc(hub->endpoints, capacity * sizeof *grown);
        if (!grown) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
        hub->endpoints = grown;
        hub->endpoint_capacity = capacity;
    }
    wpe__MemEndpoint* e = wpe_alloc_zeroed(1, sizeof *e);
    if (!e) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    e->hub = hub;
    e->transport = t;
    e->id = ++hub->next_id;
    e->port = port;
    hub->endpoints[hub->endpoint_count++] = e;
    t->state = e;
    t->backend = &wpe__memory_backend;
    t->local_port = port;
    return WPE_RESULT_OK;
}

WPE_Result wpe__net_memory_host(WPE_NetTransport* t, WPE_NetHub* hub, uint16_t port) {
    return wpe__mem_attach(t, hub, port);
}

WPE_Result wpe__net_memory_connect(WPE_NetTransport* t, WPE_NetHub* hub, WPE_NetAddress server) {
    const WPE_Result result = wpe__mem_attach(t, hub, 0);
    if (result != WPE_RESULT_OK) return result;
    wpe__MemEndpoint* e = t->state;
    const wpe__MemEndpoint* host = wpe__find_port(hub, server.port);
    /* Nobody at that port: the endpoint id 0 never exists, so the attempt times out. */
    const uint32_t to = host ? host->id : 0;
    e->links[0] = (wpe__MemLink){.state = WPE__LINK_CONNECTING,
                                 .remote_endpoint = to,
                                 .remote_slot = UINT32_MAX,
                                 .started_ms = hub->now};
    wpe__post(e, 0, to, (wpe__MemPacket){.kind = WPE__MEM_CONNECT}, NULL);
    return WPE_RESULT_OK;
}

/* ---- Hub ------------------------------------------------------------------------------------- */

WPE_Result wpe_network_hub_create(WPE_NetHub** out_hub) {
    if (!out_hub) return WPE_RESULT_ERROR_NULL_ARG;
    *out_hub = wpe_alloc_zeroed(1, sizeof **out_hub);
    return *out_hub ? WPE_RESULT_OK : WPE_RESULT_ERROR_OUT_OF_MEMORY;
}

void wpe_network_hub_destroy(WPE_NetHub* hub) {
    if (!hub) return;
    wpe_assert(hub->endpoint_count == 0); /* Its transports must be destroyed first. */
    for (size_t i = 0; i < hub->packet_count; ++i)
        wpe_free(hub->packets[i].data);
    wpe_free(hub->packets);
    wpe_free(hub->endpoints);
    wpe_free(hub);
}

void wpe_network_hub_advance(WPE_NetHub* hub, uint32_t milliseconds) {
    if (!hub) return;
    const uint64_t target = hub->now + milliseconds;
    for (;;) {
        /* The earliest due packet (ties in sending order); deliveries can post new ones. */
        size_t best = SIZE_MAX;
        for (size_t i = 0; i < hub->packet_count; ++i) {
            const wpe__MemPacket* p = &hub->packets[i];
            if (p->deliver_ms > target) continue;
            if (best == SIZE_MAX || p->deliver_ms < hub->packets[best].deliver_ms ||
                (p->deliver_ms == hub->packets[best].deliver_ms && p->order < hub->packets[best].order))
                best = i;
        }
        if (best == SIZE_MAX) break;
        const wpe__MemPacket packet = hub->packets[best];
        hub->packets[best] = hub->packets[--hub->packet_count];
        if (packet.deliver_ms > hub->now) hub->now = packet.deliver_ms;
        wpe__check_timeouts(hub);
        wpe__deliver(hub, &packet);
        wpe_free(packet.data);
    }
    hub->now = target;
    wpe__check_timeouts(hub);
}

uint64_t wpe_network_hub_now(const WPE_NetHub* hub) {
    return hub ? hub->now : 0;
}

void wpe_network_hub_cut(WPE_NetHub* hub, WPE_NetTransport* transport) {
    if (!hub || !transport || transport->backend != &wpe__memory_backend) return;
    wpe__MemEndpoint* e = transport->state;
    if (e->hub != hub || e->cut) return;
    e->cut = true;
    e->cut_ms = hub->now;
}
