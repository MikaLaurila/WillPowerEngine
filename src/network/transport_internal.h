/**
 * @file transport_internal.h
 * @brief Private: the transport object shared by the UDP (ENet) and in-memory backends.
 *
 * Backends turn what they receive into queued events (wpe__net_queue_event). The common code in
 * transport.c hands them out in order once their release time has come: the UDP backend delays
 * events by the simulated latency, the in-memory hub delivers them when they are due and queues
 * them for release at once.
 */
#ifndef WPE_NETWORK_TRANSPORT_INTERNAL_H
#define WPE_NETWORK_TRANSPORT_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "network/transport.h"

typedef struct wpe__NetQueued {
    WPE_NetEventType type;
    WPE_NetPeerId peer;
    WPE_NetChannel channel;
    uint32_t reason;
    uint8_t* data; /* Owned. */
    size_t size;
    uint64_t release_ms;
} wpe__NetQueued;

typedef struct wpe__NetBackend {
    void (*destroy)(WPE_NetTransport* t);
    void (*pump)(WPE_NetTransport* t); /* Receives what has arrived (may be NULL). */
    uint64_t (*now)(const WPE_NetTransport* t);
    WPE_Result (*send)(WPE_NetTransport* t, WPE_NetPeerId peer, WPE_NetChannel channel, const void* data,
                       size_t size);
    void (*flush)(WPE_NetTransport* t);
    void (*disconnect)(WPE_NetTransport* t, WPE_NetPeerId peer, uint32_t reason);
    bool (*is_linked)(const WPE_NetTransport* t, WPE_NetPeerId peer); /* Connected or connecting. */
    bool (*peer_stats)(const WPE_NetTransport* t, WPE_NetPeerId peer, WPE_NetPeerStats* out);
    bool (*peer_address)(const WPE_NetTransport* t, WPE_NetPeerId peer, WPE_NetAddress* out);
} wpe__NetBackend;

struct WPE_NetTransport {
    const wpe__NetBackend* backend;
    void* state; /* The backend's. */
    bool server;
    uint32_t max_peers;
    uint32_t timeout_ms;
    uint16_t local_port;
    WPE_NetConditions conditions;
    uint64_t rng;
    uint8_t* app_connected; /* Per peer: CONNECTED handed out and no DISCONNECTED since. */
    uint64_t* last_release; /* Per peer: release time of its last queued event (keeps order). */
    wpe__NetQueued* queue;
    size_t queue_count;
    size_t queue_capacity;
    uint8_t* delivered; /* Data of the last message handed out; freed on the next poll. */
    WPE_NetTransportStats stats;
};

/** Validates `desc` and allocates a transport with `max_peers` peer slots (logged on failure). */
WPE_Result wpe__net_transport_new(const WPE_NetTransportDesc* desc, bool server, uint32_t max_peers,
                                  WPE_NetTransport** out);

/** Frees what wpe__net_transport_new() made, after the backend has let go. */
void wpe__net_transport_free(WPE_NetTransport* t);

/**
 * Queues an event (copying `data`). With `delayed`, it is released after the simulated latency and
 * jitter; never before an earlier event of the same peer. Returns false when out of memory.
 */
bool wpe__net_queue_event(WPE_NetTransport* t, WPE_NetEventType type, WPE_NetPeerId peer,
                          WPE_NetChannel channel, uint32_t reason, const void* data, size_t size,
                          bool delayed);

/** Drops the queued events of one peer. */
void wpe__net_drop_events(WPE_NetTransport* t, WPE_NetPeerId peer);

/** A random number in [0, 1) from the transport's generator. */
double wpe__net_random(WPE_NetTransport* t);

/** latency + a random share of jitter, in milliseconds. */
uint64_t wpe__net_random_delay(WPE_NetTransport* t);

/** ENet's global setup, reference counted (main-thread use). Returns false on failure (logged). */
bool wpe__net_enet_acquire(void);
void wpe__net_enet_release(void);

/** Resolves a host name to an IPv4 address (host byte order) with DNS. */
bool wpe__net_enet_resolve(const char* name, uint32_t* out_ipv4);

/* ---- Datagram sockets (LAN discovery) -------------------------------------------------------
 * Connectionless packets, possibly broadcast (ipv4 0xFFFFFFFF). On a hub, a datagram reaches every
 * other socket bound to the port it is sent to (the address is ignored) 1 ms later. */
#define WPE__NET_BROADCAST 0xFFFFFFFFu
#define WPE__NET_LOOPBACK 0x7F000001u

typedef struct wpe__NetSocket {
    WPE_NetHub* hub;
    void* state; /* The backend's. */
    uint16_t port;
} wpe__NetSocket;

/** Opens a socket on `port` (0: any free one); several may share a port. Logged on failure. */
WPE_Result wpe__net_socket_open(WPE_NetHub* hub, uint16_t port, bool local_only, wpe__NetSocket* out);
void wpe__net_socket_close(wpe__NetSocket* s);
/** Sends one datagram; false if it could not be sent (not logged: discovery is best effort). */
bool wpe__net_socket_send(wpe__NetSocket* s, WPE_NetAddress to, const void* data, size_t size);
/** Takes one received datagram (truncated to `capacity`); false when none is waiting. */
bool wpe__net_socket_receive(wpe__NetSocket* s, void* buffer, size_t capacity, size_t* out_size,
                             WPE_NetAddress* out_from);
/** Milliseconds on the socket's clock (the hub's, or real time). */
uint64_t wpe__net_socket_now(const wpe__NetSocket* s);

WPE_Result wpe__net_enet_socket_open(uint16_t port, bool local_only, wpe__NetSocket* out);
void wpe__net_enet_socket_close(wpe__NetSocket* s);
bool wpe__net_enet_socket_send(wpe__NetSocket* s, WPE_NetAddress to, const void* data, size_t size);
bool wpe__net_enet_socket_receive(wpe__NetSocket* s, void* buffer, size_t capacity, size_t* out_size,
                                  WPE_NetAddress* out_from);
uint64_t wpe__net_enet_clock(void);
WPE_Result wpe__net_memory_socket_open(WPE_NetHub* hub, uint16_t port, wpe__NetSocket* out);
void wpe__net_memory_socket_close(wpe__NetSocket* s);
bool wpe__net_memory_socket_send(wpe__NetSocket* s, const void* data, size_t size, uint16_t to_port);
bool wpe__net_memory_socket_receive(wpe__NetSocket* s, void* buffer, size_t capacity, size_t* out_size,
                                    WPE_NetAddress* out_from);

/** A transport's clock in milliseconds (for layers above it). */
uint64_t wpe__net_transport_now(const WPE_NetTransport* t);

/* Backend constructors: fill t->backend/t->state (and t->local_port). */
WPE_Result wpe__net_enet_host(WPE_NetTransport* t, uint16_t port, bool local_only);
WPE_Result wpe__net_enet_connect(WPE_NetTransport* t, WPE_NetAddress server);
WPE_Result wpe__net_memory_host(WPE_NetTransport* t, WPE_NetHub* hub, uint16_t port);
WPE_Result wpe__net_memory_connect(WPE_NetTransport* t, WPE_NetHub* hub, WPE_NetAddress server);

#endif /* WPE_NETWORK_TRANSPORT_INTERNAL_H */
