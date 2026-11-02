/**
 * @file transport.h
 * @brief Message transport between game instances: connections, a reliable ordered channel and an
 *        unreliable sequenced channel, over real UDP (ENet) or an in-memory network for tests.
 *
 * A transport is one endpoint. wpe_network_host() listens for up to `max_peers` connections;
 * wpe_network_connect() makes a transport with one peer, the server (peer 0). Everything that
 * happens arrives as events from wpe_network_poll(): a peer connected, a peer disconnected (with a
 * reason), or a message arrived.
 *
 * Channels:
 * - WPE_NET_CHANNEL_RELIABLE: every message arrives, once, in the order sent (events, RPCs).
 * - WPE_NET_CHANNEL_UNRELIABLE: messages may be lost, and a message older than one already
 *   delivered is dropped, so the newest state wins (snapshots).
 *
 * Backends:
 * - Real UDP (desc.hub == NULL), through ENet. Messages are sent by wpe_network_poll() and
 *   wpe_network_flush().
 * - The in-memory network (desc.hub set): every transport on one WPE_NetHub can reach the others
 *   by port, and time only moves with wpe_network_hub_advance(). With the same seeds and the same
 *   calls, every run delivers the same events at the same times, so tests of networked games are
 *   deterministic.
 *
 * Simulated bad networks (WPE_NetConditions) apply to what a transport receives: latency, jitter
 * and packet loss. On real UDP, loss drops incoming datagrams below ENet (so reliable messages are
 * resent, as on a real lossy link) and latency/jitter delay the delivery of events. In memory, lost
 * reliable messages arrive later, as a resend would.
 *
 * - Dependencies: core; ENet (private)
 * - Thread affinity: a transport (and a hub with its transports) belongs to one thread
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_NETWORK_TRANSPORT_H
#define WPE_NETWORK_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#include "core/result.h"

/** The most peers a host accepts. */
#define WPE_NET_MAX_PEERS 32u
/** The largest message, in bytes. */
#define WPE_NET_MAX_MESSAGE_SIZE 65536u
/** The port games listen on unless they choose another. */
#define WPE_NET_DEFAULT_PORT 47810u

typedef struct WPE_NetTransport WPE_NetTransport;
typedef struct WPE_NetHub WPE_NetHub;

/** A peer of a transport: 0..max_peers-1 on a host; the server is 0 on a connected client. */
typedef uint32_t WPE_NetPeerId;

typedef enum WPE_NetChannel {
    WPE_NET_CHANNEL_RELIABLE = 0,
    WPE_NET_CHANNEL_UNRELIABLE = 1,
} WPE_NetChannel;
#define WPE_NET_CHANNEL_COUNT 2u

/** Why a peer disconnected. Values from WPE_NET_REASON_USER up are the game's own. */
typedef enum WPE_NetReason {
    WPE_NET_REASON_NONE = 0,
    WPE_NET_REASON_CLOSED = 1,         /* The other side disconnected or shut down. */
    WPE_NET_REASON_TIMEOUT = 2,        /* Nothing was heard from the peer for timeout_ms. */
    WPE_NET_REASON_CONNECT_FAILED = 3, /* The connection was never made (no answer). */
    WPE_NET_REASON_USER = 16,          /* First reason a game can pass to wpe_network_disconnect(). */
} WPE_NetReason;

typedef enum WPE_NetEventType {
    WPE_NET_EVENT_NONE = 0,
    WPE_NET_EVENT_CONNECTED,
    WPE_NET_EVENT_DISCONNECTED,
    WPE_NET_EVENT_MESSAGE,
} WPE_NetEventType;

typedef struct WPE_NetEvent {
    WPE_NetEventType type;
    WPE_NetPeerId peer;
    WPE_NetChannel channel; /* MESSAGE */
    uint32_t reason;        /* DISCONNECTED: a WPE_NetReason, or the game's reason (>= USER) */
    const uint8_t* data;    /* MESSAGE: valid until the next poll or the transport's destruction */
    size_t size;
} WPE_NetEvent;

/** An IPv4 address and port, both in host byte order. */
typedef struct WPE_NetAddress {
    uint32_t ipv4; /* 127.0.0.1 is 0x7F000001 */
    uint16_t port;
} WPE_NetAddress;

/** A simulated network, applied to what a transport receives. All zero: a perfect network. */
typedef struct WPE_NetConditions {
    uint32_t latency_ms; /* Added to every delivery, 0..5000. */
    uint32_t jitter_ms;  /* A further random 0..jitter_ms, 0..5000; deliveries never reorder. */
    float loss;          /* Chance that a packet is lost, 0..0.9. */
    uint64_t seed;       /* Seeds the random numbers (0 is a valid seed). */
} WPE_NetConditions;

typedef struct WPE_NetTransportDesc {
    WPE_NetHub* hub;     /* NULL: real UDP (ENet); otherwise the in-memory network. Borrowed. */
    uint16_t port;       /* Host: the port to listen on; 0 picks a free one (wpe_network_local_port). */
    uint32_t max_peers;  /* Host: 1..WPE_NET_MAX_PEERS; 0 means 4. A client has one peer. */
    uint32_t timeout_ms; /* A peer unheard for this long times out; 0 means 5000. */
    bool local_only;     /* UDP host: listen on 127.0.0.1 only (tests: no firewall prompt). */
    WPE_NetConditions conditions;
} WPE_NetTransportDesc;

/** Round-trip time and loss as measured for one peer. */
typedef struct WPE_NetPeerStats {
    uint32_t rtt_ms;   /* Mean round-trip time. */
    float packet_loss; /* Share of reliable packets that needed a resend, 0..1. */
} WPE_NetPeerStats;

/** Totals since the transport was made. */
typedef struct WPE_NetTransportStats {
    uint64_t bytes_sent; /* Wire bytes (in memory: payloads plus an 8-byte header each). */
    uint64_t bytes_received;
    uint64_t messages_sent;
    uint64_t messages_received;
} WPE_NetTransportStats;

/**
 * Listens for connections.
 *
 * @param desc Settings; the hub (if any) must outlive the transport.
 * @param out_transport Receives the owned transport; free it with wpe_network_destroy().
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_INVALID_ARG (a setting out of
 *         range; logged); WPE_RESULT_ERROR_ALREADY_EXISTS (in memory: the port is taken; logged);
 *         WPE_RESULT_ERROR_IO (UDP: the port cannot be opened, usually because it is in use;
 *         logged); WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety The hub's thread (any thread for UDP)
 */
[[nodiscard]] WPE_Result wpe_network_host(const WPE_NetTransportDesc* desc, WPE_NetTransport** out_transport);

/**
 * Starts connecting to a host; a CONNECTED event (peer 0) follows on success, or a DISCONNECTED
 * event with WPE_NET_REASON_CONNECT_FAILED when the host does not answer within timeout_ms (a host
 * that is full does not answer either). In memory, the address's ipv4 is ignored: the port picks
 * the host.
 *
 * @param desc Settings (port and max_peers are ignored).
 * @return As wpe_network_host(), without ALREADY_EXISTS.
 *
 * @thread_safety The hub's thread (any thread for UDP)
 */
[[nodiscard]] WPE_Result wpe_network_connect(const WPE_NetTransportDesc* desc, WPE_NetAddress server,
                                             WPE_NetTransport** out_transport);

/**
 * Disconnects every peer (they see WPE_NET_REASON_CLOSED, unless the notice is lost: then they
 * time out) and frees the transport. NULL is a no-op.
 *
 * @thread_safety Owning thread
 */
void wpe_network_destroy(WPE_NetTransport* transport);

/**
 * Takes the next event. Over UDP this also sends queued messages and receives new ones.
 *
 * @param out_event Receives the event; a message's data stays valid until the next call.
 * @return true with an event, false when there is none (out_event->type is then NONE).
 *
 * @thread_safety Owning thread
 */
bool wpe_network_poll(WPE_NetTransport* transport, WPE_NetEvent* out_event);

/**
 * Sends a message (copied). Over UDP it leaves with the next poll or flush.
 *
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_INVALID_ARG (bad channel, or
 *         size 0 or over WPE_NET_MAX_MESSAGE_SIZE; logged); WPE_RESULT_ERROR_INVALID_STATE (the
 *         peer is not connected); WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Owning thread
 */
WPE_Result wpe_network_send(WPE_NetTransport* transport, WPE_NetPeerId peer, WPE_NetChannel channel,
                            const void* data, size_t size);

/** Sends queued messages now (UDP; in memory a no-op). @thread_safety Owning thread */
void wpe_network_flush(WPE_NetTransport* transport);

/**
 * Disconnects one peer. The peer gets a DISCONNECTED event with `reason` once the messages already
 * sent to it are delivered; this side gets no event: the peer is gone at once, and events of it not
 * yet taken are dropped.
 *
 * @param reason WPE_NET_REASON_CLOSED or a game reason (>= WPE_NET_REASON_USER).
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_INVALID_ARG (reason);
 *         WPE_RESULT_ERROR_INVALID_STATE (not connected or connecting).
 *
 * @thread_safety Owning thread
 */
WPE_Result wpe_network_disconnect(WPE_NetTransport* transport, WPE_NetPeerId peer, uint32_t reason);

/** Whether the peer is connected (as of the events taken so far). @thread_safety Owning thread */
bool wpe_network_peer_connected(const WPE_NetTransport* transport, WPE_NetPeerId peer);

/**
 * Measured round trip and loss of a connected peer.
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or WPE_RESULT_ERROR_INVALID_STATE (not connected).
 *
 * @thread_safety Owning thread
 */
WPE_Result wpe_network_peer_stats(const WPE_NetTransport* transport, WPE_NetPeerId peer,
                                  WPE_NetPeerStats* out_stats);

/** The remote address of a connected peer. @return As wpe_network_peer_stats(). @thread_safety Owning thread
 */
WPE_Result wpe_network_peer_address(const WPE_NetTransport* transport, WPE_NetPeerId peer,
                                    WPE_NetAddress* out_address);

/** Byte and message totals. @thread_safety Owning thread */
WPE_NetTransportStats wpe_network_stats(const WPE_NetTransport* transport);

/** The port this transport listens or sends on (the chosen one when desc.port was 0). @thread_safety Owning
 * thread */
uint16_t wpe_network_local_port(const WPE_NetTransport* transport);

/** How many peers a host has room for (1 on a client). @thread_safety Owning thread */
uint32_t wpe_network_max_peers(const WPE_NetTransport* transport);

/**
 * Parses "a.b.c.d", "localhost" or a host name (resolved with DNS, which may block), each with an
 * optional ":port".
 *
 * @param default_port Used when the text has no port.
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_PARSE (malformed; logged);
 *         WPE_RESULT_ERROR_NOT_FOUND (the name does not resolve; logged).
 *
 * @thread_safety Any thread
 */
[[nodiscard]] WPE_Result wpe_network_address_parse(const char* text, uint16_t default_port,
                                                   WPE_NetAddress* out_address);

/**
 * Writes "a.b.c.d:port" into `buffer` (at least 22 bytes).
 *
 * @thread_safety Any thread
 */
void wpe_network_address_format(WPE_NetAddress address, char* buffer, size_t capacity);

/**
 * Creates an in-memory network. Its clock starts at 0 ms.
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Any thread; the hub then belongs to that thread
 */
[[nodiscard]] WPE_Result wpe_network_hub_create(WPE_NetHub** out_hub);

/** Frees a hub; its transports must be destroyed first. NULL is a no-op. @thread_safety Owning thread */
void wpe_network_hub_destroy(WPE_NetHub* hub);

/**
 * Moves the hub's clock forward, delivering everything due on the way (in time order) and timing
 * out connections that went silent.
 *
 * @thread_safety Owning thread
 */
void wpe_network_hub_advance(WPE_NetHub* hub, uint32_t milliseconds);

/** The hub's clock in milliseconds. @thread_safety Owning thread */
uint64_t wpe_network_hub_now(const WPE_NetHub* hub);

/**
 * Pulls a transport's cable: from now on nothing reaches it or leaves it, so its connections time
 * out on both sides. Its own sends still succeed (and vanish).
 *
 * @thread_safety Owning thread
 */
void wpe_network_hub_cut(WPE_NetHub* hub, WPE_NetTransport* transport);

#endif /* WPE_NETWORK_TRANSPORT_H */
