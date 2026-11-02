#define WPE_LOG_MODULE "network"
#include "network/transport.h"

#include <stdio.h>
#include <string.h>

#include "core/alloc.h"
#include "core/log.h"
#include "network/transport_internal.h"

/* ---- Shared transport state ------------------------------------------------------------------ */

static bool wpe__conditions_valid(const WPE_NetConditions* c) {
    return c->latency_ms <= 5000 && c->jitter_ms <= 5000 && c->loss >= 0.0f && c->loss <= 0.9f;
}

WPE_Result wpe__net_transport_new(const WPE_NetTransportDesc* desc, bool server, uint32_t max_peers,
                                  WPE_NetTransport** out) {
    if (max_peers == 0 || max_peers > WPE_NET_MAX_PEERS) {
        wpe_log_error("max_peers must be 1..%u (got %u)", WPE_NET_MAX_PEERS, max_peers);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    if (!wpe__conditions_valid(&desc->conditions)) {
        wpe_log_error("network conditions out of range (latency and jitter 0..5000 ms, loss 0..0.9)");
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    if (desc->timeout_ms > 600000) {
        wpe_log_error("timeout_ms must be at most 600000 (got %u)", desc->timeout_ms);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    WPE_NetTransport* t = wpe_alloc_zeroed(1, sizeof *t);
    if (!t) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    t->server = server;
    t->max_peers = max_peers;
    t->timeout_ms = desc->timeout_ms ? desc->timeout_ms : 5000;
    t->conditions = desc->conditions;
    /* splitmix64 state; any seed (0 included) gives a good sequence. */
    t->rng = desc->conditions.seed ^ 0x9E3779B97F4A7C15ull;
    t->app_connected = wpe_alloc_zeroed(max_peers, sizeof *t->app_connected);
    t->last_release = wpe_alloc_zeroed(max_peers, sizeof *t->last_release);
    if (!t->app_connected || !t->last_release) {
        wpe__net_transport_free(t);
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }
    *out = t;
    return WPE_RESULT_OK;
}

void wpe__net_transport_free(WPE_NetTransport* t) {
    for (size_t i = 0; i < t->queue_count; ++i)
        wpe_free(t->queue[i].data);
    wpe_free(t->queue);
    wpe_free(t->delivered);
    wpe_free(t->app_connected);
    wpe_free(t->last_release);
    wpe_free(t);
}

double wpe__net_random(WPE_NetTransport* t) {
    uint64_t z = (t->rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return (double)(z >> 11) * 0x1.0p-53;
}

uint64_t wpe__net_random_delay(WPE_NetTransport* t) {
    uint64_t delay = t->conditions.latency_ms;
    if (t->conditions.jitter_ms > 0)
        delay += (uint64_t)(wpe__net_random(t) * (t->conditions.jitter_ms + 1.0));
    return delay;
}

bool wpe__net_queue_event(WPE_NetTransport* t, WPE_NetEventType type, WPE_NetPeerId peer,
                          WPE_NetChannel channel, uint32_t reason, const void* data, size_t size,
                          bool delayed) {
    if (t->queue_count == t->queue_capacity) {
        const size_t capacity = t->queue_capacity ? t->queue_capacity * 2 : 32;
        wpe__NetQueued* grown = wpe_realloc(t->queue, capacity * sizeof *grown);
        if (!grown) return false;
        t->queue = grown;
        t->queue_capacity = capacity;
    }
    uint8_t* copy = NULL;
    if (size > 0) {
        copy = wpe_alloc(size);
        if (!copy) return false;
        memcpy(copy, data, size);
    }
    uint64_t release = t->backend->now(t);
    if (delayed) release += wpe__net_random_delay(t);
    if (release < t->last_release[peer]) release = t->last_release[peer];
    t->last_release[peer] = release;
    t->queue[t->queue_count++] = (wpe__NetQueued){.type = type,
                                                  .peer = peer,
                                                  .channel = channel,
                                                  .reason = reason,
                                                  .data = copy,
                                                  .size = size,
                                                  .release_ms = release};
    return true;
}

void wpe__net_drop_events(WPE_NetTransport* t, WPE_NetPeerId peer) {
    size_t kept = 0;
    for (size_t i = 0; i < t->queue_count; ++i) {
        if (t->queue[i].peer == peer) {
            wpe_free(t->queue[i].data);
        } else {
            t->queue[kept++] = t->queue[i];
        }
    }
    t->queue_count = kept;
    t->last_release[peer] = 0;
}

/* The first queued event that is due. Per peer, release times never decrease, so taking the first
 * due one in queue order keeps every peer's events in order. */
static bool wpe__take_due(WPE_NetTransport* t, WPE_NetEvent* out) {
    if (t->queue_count == 0) return false;
    const uint64_t now = t->backend->now(t);
    for (size_t i = 0; i < t->queue_count; ++i) {
        if (t->queue[i].release_ms > now) continue;
        const wpe__NetQueued q = t->queue[i];
        memmove(&t->queue[i], &t->queue[i + 1], (t->queue_count - i - 1) * sizeof *t->queue);
        --t->queue_count;
        *out = (WPE_NetEvent){.type = q.type,
                              .peer = q.peer,
                              .channel = q.channel,
                              .reason = q.reason,
                              .data = q.data,
                              .size = q.size};
        t->delivered = q.data;
        if (q.type == WPE_NET_EVENT_CONNECTED) t->app_connected[q.peer] = 1;
        if (q.type == WPE_NET_EVENT_DISCONNECTED) {
            t->app_connected[q.peer] = 0;
            t->last_release[q.peer] = 0;
        }
        if (q.type == WPE_NET_EVENT_MESSAGE) ++t->stats.messages_received;
        return true;
    }
    return false;
}

/* ---- Public API -------------------------------------------------------------------------------- */

WPE_Result wpe_network_host(const WPE_NetTransportDesc* desc, WPE_NetTransport** out_transport) {
    if (!desc || !out_transport) return WPE_RESULT_ERROR_NULL_ARG;
    *out_transport = NULL;
    WPE_NetTransport* t = NULL;
    const WPE_Result made = wpe__net_transport_new(desc, true, desc->max_peers ? desc->max_peers : 4, &t);
    if (made != WPE_RESULT_OK) return made;
    const WPE_Result result = desc->hub ? wpe__net_memory_host(t, desc->hub, desc->port)
                                        : wpe__net_enet_host(t, desc->port, desc->local_only);
    if (result != WPE_RESULT_OK) {
        wpe__net_transport_free(t);
        return result;
    }
    *out_transport = t;
    return WPE_RESULT_OK;
}

WPE_Result wpe_network_connect(const WPE_NetTransportDesc* desc, WPE_NetAddress server,
                               WPE_NetTransport** out_transport) {
    if (!desc || !out_transport) return WPE_RESULT_ERROR_NULL_ARG;
    *out_transport = NULL;
    if (server.port == 0) {
        wpe_log_error("cannot connect to port 0");
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    WPE_NetTransport* t = NULL;
    const WPE_Result made = wpe__net_transport_new(desc, false, 1, &t);
    if (made != WPE_RESULT_OK) return made;
    const WPE_Result result =
        desc->hub ? wpe__net_memory_connect(t, desc->hub, server) : wpe__net_enet_connect(t, server);
    if (result != WPE_RESULT_OK) {
        wpe__net_transport_free(t);
        return result;
    }
    *out_transport = t;
    return WPE_RESULT_OK;
}

void wpe_network_destroy(WPE_NetTransport* transport) {
    if (!transport) return;
    transport->backend->destroy(transport);
    wpe__net_transport_free(transport);
}

bool wpe_network_poll(WPE_NetTransport* transport, WPE_NetEvent* out_event) {
    if (out_event) *out_event = (WPE_NetEvent){0};
    if (!transport || !out_event) return false;
    wpe_free(transport->delivered);
    transport->delivered = NULL;
    if (wpe__take_due(transport, out_event)) return true;
    if (transport->backend->pump) transport->backend->pump(transport);
    return wpe__take_due(transport, out_event);
}

WPE_Result wpe_network_send(WPE_NetTransport* transport, WPE_NetPeerId peer, WPE_NetChannel channel,
                            const void* data, size_t size) {
    if (!transport || !data) return WPE_RESULT_ERROR_NULL_ARG;
    if ((unsigned)channel >= WPE_NET_CHANNEL_COUNT || size == 0 || size > WPE_NET_MAX_MESSAGE_SIZE) {
        wpe_log_error("cannot send %zu bytes on channel %u (1..%u bytes, channels 0..%u)", size,
                      (unsigned)channel, WPE_NET_MAX_MESSAGE_SIZE, WPE_NET_CHANNEL_COUNT - 1);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    if (peer >= transport->max_peers || !transport->app_connected[peer] ||
        !transport->backend->is_linked(transport, peer))
        return WPE_RESULT_ERROR_INVALID_STATE;
    const WPE_Result result = transport->backend->send(transport, peer, channel, data, size);
    if (result == WPE_RESULT_OK) ++transport->stats.messages_sent;
    return result;
}

void wpe_network_flush(WPE_NetTransport* transport) {
    if (transport && transport->backend->flush) transport->backend->flush(transport);
}

WPE_Result wpe_network_disconnect(WPE_NetTransport* transport, WPE_NetPeerId peer, uint32_t reason) {
    if (!transport) return WPE_RESULT_ERROR_NULL_ARG;
    if (reason != WPE_NET_REASON_CLOSED && reason < WPE_NET_REASON_USER) {
        wpe_log_error(
            "disconnect reason %u is reserved (use WPE_NET_REASON_CLOSED or >= WPE_NET_REASON_USER)", reason);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    if (peer >= transport->max_peers || !transport->backend->is_linked(transport, peer))
        return WPE_RESULT_ERROR_INVALID_STATE;
    transport->backend->disconnect(transport, peer, reason);
    wpe__net_drop_events(transport, peer);
    transport->app_connected[peer] = 0;
    return WPE_RESULT_OK;
}

bool wpe_network_peer_connected(const WPE_NetTransport* transport, WPE_NetPeerId peer) {
    return transport && peer < transport->max_peers && transport->app_connected[peer] &&
           transport->backend->is_linked(transport, peer);
}

WPE_Result wpe_network_peer_stats(const WPE_NetTransport* transport, WPE_NetPeerId peer,
                                  WPE_NetPeerStats* out_stats) {
    if (!transport || !out_stats) return WPE_RESULT_ERROR_NULL_ARG;
    *out_stats = (WPE_NetPeerStats){0};
    if (!wpe_network_peer_connected(transport, peer) ||
        !transport->backend->peer_stats(transport, peer, out_stats))
        return WPE_RESULT_ERROR_INVALID_STATE;
    return WPE_RESULT_OK;
}

WPE_Result wpe_network_peer_address(const WPE_NetTransport* transport, WPE_NetPeerId peer,
                                    WPE_NetAddress* out_address) {
    if (!transport || !out_address) return WPE_RESULT_ERROR_NULL_ARG;
    *out_address = (WPE_NetAddress){0};
    if (!wpe_network_peer_connected(transport, peer) ||
        !transport->backend->peer_address(transport, peer, out_address))
        return WPE_RESULT_ERROR_INVALID_STATE;
    return WPE_RESULT_OK;
}

WPE_NetTransportStats wpe_network_stats(const WPE_NetTransport* transport) {
    return transport ? transport->stats : (WPE_NetTransportStats){0};
}

uint64_t wpe__net_transport_now(const WPE_NetTransport* t) {
    return t->backend->now(t);
}

uint16_t wpe_network_local_port(const WPE_NetTransport* transport) {
    return transport ? transport->local_port : 0;
}

uint32_t wpe_network_max_peers(const WPE_NetTransport* transport) {
    return transport ? transport->max_peers : 0;
}

/* ---- Addresses ------------------------------------------------------------------------------- */

/* Parses a decimal number of at most `max` from text[0..length). */
static bool wpe__parse_decimal(const char* text, size_t length, uint32_t max, uint32_t* out) {
    if (length == 0 || length > 5) return false;
    uint32_t value = 0;
    for (size_t i = 0; i < length; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        value = value * 10 + (uint32_t)(text[i] - '0');
    }
    if (value > max) return false;
    *out = value;
    return true;
}

/* "a.b.c.d" exactly. */
static bool wpe__parse_dotted(const char* text, size_t length, uint32_t* out) {
    uint32_t address = 0;
    size_t start = 0;
    for (int part = 0; part < 4; ++part) {
        size_t end = start;
        while (end < length && text[end] != '.')
            ++end;
        uint32_t byte = 0;
        if (!wpe__parse_decimal(text + start, end - start, 255, &byte)) return false;
        address = address << 8 | byte;
        if (part < 3 && end >= length) return false;
        start = end + 1;
    }
    if (start <= length) return false; /* Text after the fourth number. */
    *out = address;
    return true;
}

WPE_Result wpe_network_address_parse(const char* text, uint16_t default_port, WPE_NetAddress* out_address) {
    if (!text || !out_address) return WPE_RESULT_ERROR_NULL_ARG;
    *out_address = (WPE_NetAddress){0};
    size_t host_length = strlen(text);
    uint32_t port = default_port;
    const char* colon = strrchr(text, ':');
    if (colon) {
        host_length = (size_t)(colon - text);
        if (!wpe__parse_decimal(colon + 1, strlen(colon + 1), 65535, &port) || port == 0) {
            wpe_log_error("bad port in address \"%s\"", text);
            return WPE_RESULT_ERROR_PARSE;
        }
    }
    if (host_length == 0 || host_length > 253) {
        wpe_log_error("bad address \"%s\"", text);
        return WPE_RESULT_ERROR_PARSE;
    }
    char host[256];
    memcpy(host, text, host_length);
    host[host_length] = '\0';
    uint32_t ipv4 = 0;
    if (strcmp(host, "localhost") == 0) {
        ipv4 = 0x7F000001u;
    } else if (!wpe__parse_dotted(host, host_length, &ipv4)) {
        if (strspn(host, "0123456789.") == host_length) { /* Numbers, but not an address. */
            wpe_log_error("bad address \"%s\"", text);
            return WPE_RESULT_ERROR_PARSE;
        }
        for (size_t i = 0; i < host_length; ++i) {
            const char c = host[i];
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                            c == '-' || c == '.';
            if (!ok) {
                wpe_log_error("bad address \"%s\"", text);
                return WPE_RESULT_ERROR_PARSE;
            }
        }
        if (!wpe__net_enet_resolve(host, &ipv4)) {
            wpe_log_error("cannot resolve \"%s\"", host);
            return WPE_RESULT_ERROR_NOT_FOUND;
        }
    }
    *out_address = (WPE_NetAddress){.ipv4 = ipv4, .port = (uint16_t)port};
    return WPE_RESULT_OK;
}

void wpe_network_address_format(WPE_NetAddress address, char* buffer, size_t capacity) {
    if (!buffer || capacity == 0) return;
    snprintf(buffer, capacity, "%u.%u.%u.%u:%u", (unsigned)(address.ipv4 >> 24),
             (unsigned)(address.ipv4 >> 16 & 255u), (unsigned)(address.ipv4 >> 8 & 255u),
             (unsigned)(address.ipv4 & 255u), (unsigned)address.port);
}
