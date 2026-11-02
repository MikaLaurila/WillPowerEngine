#include <stdio.h>
#include <time.h>

#include "network/network.h"
#include "wpe_test.h"

/* ---- Helpers --------------------------------------------------------------------------------- */

typedef struct Recorded {
    uint64_t time;
    int who;
    WPE_NetEventType type;
    WPE_NetPeerId peer;
    WPE_NetChannel channel;
    uint32_t reason;
    uint32_t value; /* First 4 bytes of a message. */
    size_t size;
} Recorded;

typedef struct Trace {
    Recorded events[4096];
    size_t count;
} Trace;

static uint32_t read_u32(const uint8_t* data) {
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

static void write_u32(uint8_t* data, uint32_t value) {
    for (int i = 0; i < 4; ++i)
        data[i] = (uint8_t)(value >> (8 * i));
}

/* Takes every pending event of the transports; `time` is stamped on each. */
static void drain(WPE_NetTransport** transports, int count, uint64_t time, Trace* trace) {
    for (int who = 0; who < count; ++who) {
        if (!transports[who]) continue;
        WPE_NetEvent event;
        while (wpe_network_poll(transports[who], &event)) {
            WPE_TEST_ASSERT(trace->count < sizeof trace->events / sizeof trace->events[0]);
            trace->events[trace->count++] = (Recorded){.time = time,
                                                       .who = who,
                                                       .type = event.type,
                                                       .peer = event.peer,
                                                       .channel = event.channel,
                                                       .reason = event.reason,
                                                       .value = event.size >= 4 ? read_u32(event.data) : 0,
                                                       .size = event.size};
        }
    }
}

/* Advances the hub in 5 ms steps for `ms`, draining after each. */
static void run(WPE_NetHub* hub, WPE_NetTransport** transports, int count, uint32_t ms, Trace* trace) {
    for (uint32_t t = 0; t < ms; t += 5) {
        wpe_network_hub_advance(hub, 5);
        drain(transports, count, wpe_network_hub_now(hub), trace);
    }
}

static const Recorded* find(const Trace* trace, int who, WPE_NetEventType type) {
    for (size_t i = 0; i < trace->count; ++i)
        if (trace->events[i].who == who && trace->events[i].type == type) return &trace->events[i];
    return NULL;
}

static size_t count_of(const Trace* trace, int who, WPE_NetEventType type) {
    size_t n = 0;
    for (size_t i = 0; i < trace->count; ++i)
        n += trace->events[i].who == who && trace->events[i].type == type;
    return n;
}

static WPE_NetAddress at_port(uint16_t port) {
    return (WPE_NetAddress){.ipv4 = 0x7F000001u, .port = port};
}

static void send_number(WPE_NetTransport* t, WPE_NetPeerId peer, WPE_NetChannel channel, uint32_t value) {
    uint8_t data[4];
    write_u32(data, value);
    WPE_TEST_ASSERT_OK(wpe_network_send(t, peer, channel, data, sizeof data));
}

/* A host and one connected client on a hub. */
typedef struct Pair {
    WPE_NetHub* hub;
    WPE_NetTransport* t[2]; /* [0] host, [1] client */
} Pair;

static Pair make_pair(WPE_NetConditions host_conditions, WPE_NetConditions client_conditions, Trace* trace) {
    Pair p = {0};
    WPE_TEST_ASSERT_OK(wpe_network_hub_create(&p.hub));
    WPE_TEST_ASSERT_OK(wpe_network_host(
        &(WPE_NetTransportDesc){.hub = p.hub, .port = 7000, .conditions = host_conditions}, &p.t[0]));
    WPE_TEST_ASSERT_OK(wpe_network_connect(
        &(WPE_NetTransportDesc){.hub = p.hub, .conditions = client_conditions}, at_port(7000), &p.t[1]));
    run(p.hub, p.t, 2, 2000, trace);
    WPE_TEST_ASSERT(wpe_network_peer_connected(p.t[0], 0));
    WPE_TEST_ASSERT(wpe_network_peer_connected(p.t[1], 0));
    return p;
}

static void free_pair(Pair* p) {
    wpe_network_destroy(p->t[0]);
    wpe_network_destroy(p->t[1]);
    wpe_network_hub_destroy(p->hub);
}

/* ---- In memory --------------------------------------------------------------------------------- */

static void test_network_memory_connect_and_exchange(void) {
    static Trace trace;
    trace.count = 0;
    Pair p = make_pair((WPE_NetConditions){.latency_ms = 20}, (WPE_NetConditions){.latency_ms = 30}, &trace);
    /* Connection events: the host hears the client after the client's packet reaches it (20 ms),
     * the client after the answer comes back (+30 ms). */
    const Recorded* host_connected = find(&trace, 0, WPE_NET_EVENT_CONNECTED);
    const Recorded* client_connected = find(&trace, 1, WPE_NET_EVENT_CONNECTED);
    WPE_TEST_ASSERT(host_connected && client_connected);
    WPE_TEST_ASSERT_EQ_UINT(20, host_connected->time);
    WPE_TEST_ASSERT_EQ_UINT(50, client_connected->time);
    WPE_TEST_ASSERT_EQ_UINT(0, client_connected->peer);
    WPE_TEST_ASSERT_EQ_UINT(4, wpe_network_max_peers(p.t[0]));
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_network_max_peers(p.t[1]));
    WPE_TEST_ASSERT_EQ_UINT(7000, wpe_network_local_port(p.t[0]));

    WPE_NetAddress address = {0};
    WPE_TEST_ASSERT_OK(wpe_network_peer_address(p.t[1], 0, &address));
    WPE_TEST_ASSERT_EQ_UINT(7000, address.port);
    WPE_NetPeerStats stats = {0};
    WPE_TEST_ASSERT_OK(wpe_network_peer_stats(p.t[1], 0, &stats));
    WPE_TEST_ASSERT_EQ_UINT(50, stats.rtt_ms);

    /* Both channels, both ways; latency is the receiver's. */
    trace.count = 0;
    const uint64_t sent_at = wpe_network_hub_now(p.hub);
    send_number(p.t[1], 0, WPE_NET_CHANNEL_RELIABLE, 111);
    send_number(p.t[1], 0, WPE_NET_CHANNEL_UNRELIABLE, 222);
    send_number(p.t[0], 0, WPE_NET_CHANNEL_RELIABLE, 333);
    run(p.hub, p.t, 2, 200, &trace);
    WPE_TEST_ASSERT_EQ_UINT(3, trace.count);
    WPE_TEST_ASSERT_EQ_INT(0, trace.events[0].who);
    WPE_TEST_ASSERT_EQ_UINT(111, trace.events[0].value);
    WPE_TEST_ASSERT_EQ_INT(WPE_NET_CHANNEL_RELIABLE, trace.events[0].channel);
    WPE_TEST_ASSERT_EQ_UINT(sent_at + 20, trace.events[0].time);
    WPE_TEST_ASSERT_EQ_UINT(222, trace.events[1].value);
    WPE_TEST_ASSERT_EQ_INT(WPE_NET_CHANNEL_UNRELIABLE, trace.events[1].channel);
    WPE_TEST_ASSERT_EQ_INT(1, trace.events[2].who);
    WPE_TEST_ASSERT_EQ_UINT(333, trace.events[2].value);
    WPE_TEST_ASSERT_EQ_UINT(sent_at + 30, trace.events[2].time);

    const WPE_NetTransportStats totals = wpe_network_stats(p.t[1]);
    WPE_TEST_ASSERT_EQ_UINT(2, totals.messages_sent);
    WPE_TEST_ASSERT_EQ_UINT(1, totals.messages_received);
    WPE_TEST_ASSERT_EQ_UINT(2 * (4 + 8), totals.bytes_sent);
    WPE_TEST_ASSERT_EQ_UINT(4 + 8, totals.bytes_received);
    free_pair(&p);
}

static void test_network_memory_reliable_survives_loss(void) {
    static Trace trace;
    trace.count = 0;
    const WPE_NetConditions bad = {.latency_ms = 40, .jitter_ms = 30, .loss = 0.3f, .seed = 7};
    Pair p = make_pair(bad, bad, &trace);
    trace.count = 0;
    for (uint32_t i = 0; i < 300; ++i) {
        send_number(p.t[1], 0, WPE_NET_CHANNEL_RELIABLE, i);
        send_number(p.t[1], 0, WPE_NET_CHANNEL_UNRELIABLE, 1000 + i);
        run(p.hub, p.t, 2, 10, &trace);
    }
    run(p.hub, p.t, 2, 3000, &trace);
    /* Every reliable message, once and in order; unreliable ones lose some, and never go back. */
    uint32_t next = 0;
    size_t unreliable = 0;
    uint32_t last_unreliable = 0;
    for (size_t i = 0; i < trace.count; ++i) {
        const Recorded* r = &trace.events[i];
        WPE_TEST_ASSERT_EQ_INT(0, r->who);
        WPE_TEST_ASSERT_EQ_INT(WPE_NET_EVENT_MESSAGE, r->type);
        if (r->channel == WPE_NET_CHANNEL_RELIABLE) {
            WPE_TEST_ASSERT_EQ_UINT(next, r->value);
            ++next;
        } else {
            WPE_TEST_ASSERT(r->value > last_unreliable);
            last_unreliable = r->value;
            ++unreliable;
        }
    }
    WPE_TEST_ASSERT_EQ_UINT(300, next);
    WPE_TEST_ASSERT_MSG(unreliable > 150 && unreliable < 260, "%zu of 300 unreliable messages arrived",
                        unreliable);
    free_pair(&p);
}

/* Records a scenario under jitter and loss; the same seeds must give the same trace. */
static void record_scenario(Trace* trace) {
    trace->count = 0;
    const WPE_NetConditions bad = {.latency_ms = 25, .jitter_ms = 40, .loss = 0.25f, .seed = 99};
    Pair p = make_pair(bad, (WPE_NetConditions){.latency_ms = 10, .jitter_ms = 15, .loss = 0.1f, .seed = 5},
                       trace);
    for (uint32_t i = 0; i < 100; ++i) {
        send_number(p.t[0], 0, i % 2 ? WPE_NET_CHANNEL_RELIABLE : WPE_NET_CHANNEL_UNRELIABLE, i);
        send_number(p.t[1], 0, WPE_NET_CHANNEL_UNRELIABLE, 500 + i);
        run(p.hub, p.t, 2, 15, trace);
    }
    run(p.hub, p.t, 2, 2000, trace);
    free_pair(&p);
}

static void test_network_memory_is_deterministic(void) {
    static Trace first;
    static Trace second;
    record_scenario(&first);
    record_scenario(&second);
    WPE_TEST_ASSERT(first.count > 100);
    WPE_TEST_ASSERT_EQ_UINT(first.count, second.count);
    for (size_t i = 0; i < first.count; ++i) {
        WPE_TEST_ASSERT_EQ_UINT(first.events[i].time, second.events[i].time);
        WPE_TEST_ASSERT_EQ_INT(first.events[i].who, second.events[i].who);
        WPE_TEST_ASSERT_EQ_UINT(first.events[i].value, second.events[i].value);
    }
}

static void test_network_memory_disconnects(void) {
    static Trace trace;
    trace.count = 0;
    Pair p = make_pair((WPE_NetConditions){.latency_ms = 10}, (WPE_NetConditions){.latency_ms = 10}, &trace);
    trace.count = 0;

    /* A game reason travels after the messages already sent; this side forgets the peer at once. */
    send_number(p.t[0], 0, WPE_NET_CHANNEL_RELIABLE, 42);
    WPE_TEST_ASSERT_OK(wpe_network_disconnect(p.t[0], 0, WPE_NET_REASON_USER + 3));
    WPE_TEST_ASSERT(!wpe_network_peer_connected(p.t[0], 0));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_STATE,
                           wpe_network_send(p.t[0], 0, WPE_NET_CHANNEL_RELIABLE, "x", 1));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_STATE,
                           wpe_network_disconnect(p.t[0], 0, WPE_NET_REASON_CLOSED));
    run(p.hub, p.t, 2, 100, &trace);
    WPE_TEST_ASSERT_EQ_UINT(2, trace.count);
    WPE_TEST_ASSERT_EQ_INT(WPE_NET_EVENT_MESSAGE, trace.events[0].type);
    WPE_TEST_ASSERT_EQ_INT(WPE_NET_EVENT_DISCONNECTED, trace.events[1].type);
    WPE_TEST_ASSERT_EQ_INT(1, trace.events[1].who);
    WPE_TEST_ASSERT_EQ_UINT(WPE_NET_REASON_USER + 3, trace.events[1].reason);
    WPE_TEST_ASSERT(!wpe_network_peer_connected(p.t[1], 0));

    /* Reconnecting reuses the slot; destroying a side tells the other one. */
    wpe_network_destroy(p.t[1]);
    WPE_TEST_ASSERT_OK(wpe_network_connect(&(WPE_NetTransportDesc){.hub = p.hub}, at_port(7000), &p.t[1]));
    trace.count = 0;
    run(p.hub, p.t, 2, 100, &trace);
    WPE_TEST_ASSERT(find(&trace, 0, WPE_NET_EVENT_CONNECTED) && find(&trace, 1, WPE_NET_EVENT_CONNECTED));
    wpe_network_destroy(p.t[1]);
    p.t[1] = NULL;
    trace.count = 0;
    run(p.hub, p.t, 2, 100, &trace);
    const Recorded* closed = find(&trace, 0, WPE_NET_EVENT_DISCONNECTED);
    WPE_TEST_ASSERT(closed);
    WPE_TEST_ASSERT_EQ_UINT(WPE_NET_REASON_CLOSED, closed->reason);
    free_pair(&p);
}

static void test_network_memory_timeouts_and_failures(void) {
    static Trace trace;
    trace.count = 0;
    WPE_NetHub* hub = NULL;
    WPE_TEST_ASSERT_OK(wpe_network_hub_create(&hub));
    WPE_NetTransport* t[4] = {0};
    WPE_TEST_ASSERT_OK(wpe_network_host(
        &(WPE_NetTransportDesc){.hub = hub, .port = 9000, .max_peers = 2, .timeout_ms = 1000}, &t[0]));
    for (int i = 1; i <= 3; ++i)
        WPE_TEST_ASSERT_OK(wpe_network_connect(&(WPE_NetTransportDesc){.hub = hub, .timeout_ms = 1000},
                                               at_port(9000), &t[i]));
    run(hub, t, 4, 1500, &trace);
    /* Two fit; the third gets no answer and fails after its timeout. */
    WPE_TEST_ASSERT_EQ_UINT(2, count_of(&trace, 0, WPE_NET_EVENT_CONNECTED));
    WPE_TEST_ASSERT(find(&trace, 1, WPE_NET_EVENT_CONNECTED) && find(&trace, 2, WPE_NET_EVENT_CONNECTED));
    const Recorded* failed = find(&trace, 3, WPE_NET_EVENT_DISCONNECTED);
    WPE_TEST_ASSERT(failed && !find(&trace, 3, WPE_NET_EVENT_CONNECTED));
    WPE_TEST_ASSERT_EQ_UINT(WPE_NET_REASON_CONNECT_FAILED, failed->reason);
    WPE_TEST_ASSERT_EQ_UINT(1000, failed->time);
    wpe_network_destroy(t[3]);
    t[3] = NULL;

    /* A pulled cable: both ends time out. */
    trace.count = 0;
    wpe_network_hub_cut(hub, t[1]);
    const uint64_t cut_at = wpe_network_hub_now(hub);
    run(hub, t, 4, 1500, &trace);
    const Recorded* host_side = find(&trace, 0, WPE_NET_EVENT_DISCONNECTED);
    const Recorded* client_side = find(&trace, 1, WPE_NET_EVENT_DISCONNECTED);
    WPE_TEST_ASSERT(host_side && client_side);
    WPE_TEST_ASSERT_EQ_UINT(WPE_NET_REASON_TIMEOUT, host_side->reason);
    WPE_TEST_ASSERT_EQ_UINT(WPE_NET_REASON_TIMEOUT, client_side->reason);
    WPE_TEST_ASSERT_EQ_UINT(cut_at + 1000, host_side->time);
    WPE_TEST_ASSERT(!find(&trace, 2, WPE_NET_EVENT_DISCONNECTED)); /* The other client is fine. */
    WPE_TEST_ASSERT(wpe_network_peer_connected(t[2], 0));

    /* Nobody at the port. */
    WPE_NetTransport* lonely = NULL;
    WPE_TEST_ASSERT_OK(
        wpe_network_connect(&(WPE_NetTransportDesc){.hub = hub, .timeout_ms = 300}, at_port(1234), &lonely));
    trace.count = 0;
    run(hub, &lonely, 1, 400, &trace);
    WPE_TEST_ASSERT_EQ_UINT(1, trace.count);
    WPE_TEST_ASSERT_EQ_UINT(WPE_NET_REASON_CONNECT_FAILED, trace.events[0].reason);
    wpe_network_destroy(lonely);
    for (int i = 0; i < 4; ++i)
        wpe_network_destroy(t[i]);
    wpe_network_hub_destroy(hub);
}

static void test_network_rejects_bad_arguments(void) {
    WPE_NetHub* hub = NULL;
    WPE_TEST_ASSERT_OK(wpe_network_hub_create(&hub));
    WPE_NetTransport* t = NULL;
    wpe_log_set_stderr_enabled(false);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG,
                           wpe_network_host(&(WPE_NetTransportDesc){.hub = hub, .max_peers = 33}, &t));
    WPE_TEST_ASSERT_EQ_INT(
        WPE_RESULT_ERROR_INVALID_ARG,
        wpe_network_host(&(WPE_NetTransportDesc){.hub = hub, .conditions = {.loss = 0.95f}}, &t));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG,
                           wpe_network_connect(&(WPE_NetTransportDesc){.hub = hub}, at_port(0), &t));
    WPE_TEST_ASSERT(t == NULL);
    WPE_NetTransport* host = NULL;
    WPE_TEST_ASSERT_OK(wpe_network_host(&(WPE_NetTransportDesc){.hub = hub, .port = 5}, &host));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_ALREADY_EXISTS,
                           wpe_network_host(&(WPE_NetTransportDesc){.hub = hub, .port = 5}, &t));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_STATE,
                           wpe_network_send(host, 0, WPE_NET_CHANNEL_RELIABLE, "x", 1)); /* Nobody there. */
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG,
                           wpe_network_send(host, 0, WPE_NET_CHANNEL_RELIABLE, "x", 0));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG,
                           wpe_network_send(host, 0, (WPE_NetChannel)2, "x", 1));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_network_send(host, 0, WPE_NET_CHANNEL_RELIABLE,
                                                                          "x", WPE_NET_MAX_MESSAGE_SIZE + 1));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG,
                           wpe_network_disconnect(host, 0, WPE_NET_REASON_TIMEOUT));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_STATE,
                           wpe_network_disconnect(host, 99, WPE_NET_REASON_CLOSED));
    wpe_log_set_stderr_enabled(true);
    WPE_NetEvent event;
    WPE_TEST_ASSERT(!wpe_network_poll(host, &event));
    WPE_TEST_ASSERT_EQ_INT(WPE_NET_EVENT_NONE, event.type);
    WPE_TEST_ASSERT(!wpe_network_poll(NULL, &event));
    wpe_network_flush(host);
    wpe_network_destroy(host);
    wpe_network_destroy(NULL);
    wpe_network_hub_destroy(hub);
}

static void test_network_address_parse_and_format(void) {
    WPE_NetAddress a = {0};
    WPE_TEST_ASSERT_OK(wpe_network_address_parse("192.168.1.20:5000", 47810, &a));
    WPE_TEST_ASSERT_EQ_UINT(0xC0A80114u, a.ipv4);
    WPE_TEST_ASSERT_EQ_UINT(5000, a.port);
    WPE_TEST_ASSERT_OK(wpe_network_address_parse("10.0.0.1", 47810, &a));
    WPE_TEST_ASSERT_EQ_UINT(0x0A000001u, a.ipv4);
    WPE_TEST_ASSERT_EQ_UINT(47810, a.port);
    WPE_TEST_ASSERT_OK(wpe_network_address_parse("localhost:1", 47810, &a));
    WPE_TEST_ASSERT_EQ_UINT(0x7F000001u, a.ipv4);
    char text[32];
    wpe_network_address_format((WPE_NetAddress){.ipv4 = 0xC0A80114u, .port = 65535}, text, sizeof text);
    WPE_TEST_ASSERT_EQ_STR("192.168.1.20:65535", text);
    wpe_log_set_stderr_enabled(false);
    const char* bad[] = {"",          ":80", "1.2.3.4:", "1.2.3.4:0", "1.2.3.4:70000",
                         "1.2.3.400", "a b", "1.2.3.4:x"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i)
        WPE_TEST_ASSERT_MSG(wpe_network_address_parse(bad[i], 1, &a) == WPE_RESULT_ERROR_PARSE, "\"%s\"",
                            bad[i]);
    wpe_log_set_stderr_enabled(true);
}

/* ---- Real UDP on this machine ---------------------------------------------------------------- */

static double seconds_now(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Polls both transports until `done` holds or `seconds` pass. */
typedef bool (*Condition)(const Trace* trace);

static bool poll_until(WPE_NetTransport** t, int count, Trace* trace, Condition done, double seconds) {
    const double end = seconds_now() + seconds;
    while (seconds_now() < end) {
        drain(t, count, 0, trace);
        if (done(trace)) return true;
    }
    return false;
}

static bool both_connected(const Trace* trace) {
    return find(trace, 0, WPE_NET_EVENT_CONNECTED) && find(trace, 1, WPE_NET_EVENT_CONNECTED);
}

static size_t g_expected_messages;
static bool host_has_messages(const Trace* trace) {
    return count_of(trace, 0, WPE_NET_EVENT_MESSAGE) >= g_expected_messages;
}
static bool host_saw_disconnect(const Trace* trace) {
    return find(trace, 0, WPE_NET_EVENT_DISCONNECTED) != NULL;
}
static bool client_saw_disconnect(const Trace* trace) {
    return find(trace, 1, WPE_NET_EVENT_DISCONNECTED) != NULL;
}

static void connect_udp(WPE_NetTransport** t, WPE_NetConditions conditions, Trace* trace) {
    WPE_TEST_ASSERT_OK(
        wpe_network_host(&(WPE_NetTransportDesc){.local_only = true, .conditions = conditions}, &t[0]));
    const uint16_t port = wpe_network_local_port(t[0]);
    WPE_TEST_ASSERT(port != 0);
    conditions.seed += 1;
    WPE_TEST_ASSERT_OK(
        wpe_network_connect(&(WPE_NetTransportDesc){.conditions = conditions}, at_port(port), &t[1]));
    WPE_TEST_ASSERT_MSG(poll_until(t, 2, trace, both_connected, 10.0), "no UDP connection on 127.0.0.1:%u",
                        port);
}

static void test_network_udp_loopback(void) {
    static Trace trace;
    trace.count = 0;
    WPE_NetTransport* t[2] = {0};
    connect_udp(t, (WPE_NetConditions){0}, &trace);
    WPE_NetAddress address = {0};
    WPE_TEST_ASSERT_OK(wpe_network_peer_address(t[0], 0, &address));
    WPE_TEST_ASSERT_EQ_UINT(0x7F000001u, address.ipv4);

    trace.count = 0;
    send_number(t[1], 0, WPE_NET_CHANNEL_RELIABLE, 7);
    send_number(t[1], 0, WPE_NET_CHANNEL_UNRELIABLE, 8);
    /* A large reliable message is split into fragments and put back together. */
    static uint8_t big[WPE_NET_MAX_MESSAGE_SIZE];
    for (size_t i = 0; i < sizeof big; ++i)
        big[i] = (uint8_t)(i * 7u);
    WPE_TEST_ASSERT_OK(wpe_network_send(t[1], 0, WPE_NET_CHANNEL_RELIABLE, big, sizeof big));
    wpe_network_flush(t[1]);
    g_expected_messages = 3;
    WPE_TEST_ASSERT(poll_until(t, 2, &trace, host_has_messages, 10.0));
    WPE_TEST_ASSERT_EQ_UINT(7, trace.events[0].value);
    size_t big_seen = 0;
    for (size_t i = 0; i < trace.count; ++i)
        big_seen += trace.events[i].size == sizeof big;
    WPE_TEST_ASSERT_EQ_UINT(1, big_seen);
    WPE_TEST_ASSERT(wpe_network_stats(t[1]).bytes_sent > 0);
    WPE_TEST_ASSERT(wpe_network_stats(t[0]).bytes_received > 0);

    /* The host closes the client with a game reason. */
    trace.count = 0;
    WPE_TEST_ASSERT_OK(wpe_network_disconnect(t[0], 0, WPE_NET_REASON_USER));
    WPE_TEST_ASSERT(poll_until(t, 2, &trace, client_saw_disconnect, 10.0));
    WPE_TEST_ASSERT_EQ_UINT(WPE_NET_REASON_USER, find(&trace, 1, WPE_NET_EVENT_DISCONNECTED)->reason);
    WPE_TEST_ASSERT(!find(&trace, 0, WPE_NET_EVENT_DISCONNECTED)); /* No event on the side that closed. */
    wpe_network_destroy(t[1]);

    /* A new client; destroying it is seen as CLOSED. */
    trace.count = 0;
    WPE_TEST_ASSERT_OK(
        wpe_network_connect(&(WPE_NetTransportDesc){0}, at_port(wpe_network_local_port(t[0])), &t[1]));
    WPE_TEST_ASSERT(poll_until(t, 2, &trace, both_connected, 10.0));
    wpe_network_destroy(t[1]);
    t[1] = NULL;
    trace.count = 0;
    WPE_TEST_ASSERT(poll_until(t, 1, &trace, host_saw_disconnect, 10.0));
    WPE_TEST_ASSERT_EQ_UINT(WPE_NET_REASON_CLOSED, trace.events[0].reason);
    wpe_network_destroy(t[0]);
}

static void test_network_udp_reliable_survives_loss(void) {
    static Trace trace;
    trace.count = 0;
    WPE_NetTransport* t[2] = {0};
    connect_udp(t, (WPE_NetConditions){.latency_ms = 5, .jitter_ms = 5, .loss = 0.2f, .seed = 3}, &trace);
    trace.count = 0;
    for (uint32_t i = 0; i < 100; ++i)
        send_number(t[1], 0, WPE_NET_CHANNEL_RELIABLE, i);
    wpe_network_flush(t[1]);
    g_expected_messages = 100;
    WPE_TEST_ASSERT_MSG(poll_until(t, 2, &trace, host_has_messages, 20.0), "%zu of 100 arrived",
                        count_of(&trace, 0, WPE_NET_EVENT_MESSAGE));
    uint32_t next = 0;
    for (size_t i = 0; i < trace.count; ++i)
        if (trace.events[i].who == 0 && trace.events[i].type == WPE_NET_EVENT_MESSAGE)
            WPE_TEST_ASSERT_EQ_UINT(next++, trace.events[i].value);
    wpe_network_destroy(t[1]);
    wpe_network_destroy(t[0]);
}

static void test_network_udp_connect_fails_without_host(void) {
    static Trace trace;
    trace.count = 0;
    /* A port that was free a moment ago. */
    WPE_NetTransport* probe = NULL;
    WPE_TEST_ASSERT_OK(wpe_network_host(&(WPE_NetTransportDesc){.local_only = true}, &probe));
    const uint16_t port = wpe_network_local_port(probe);
    wpe_network_destroy(probe);
    WPE_NetTransport* t[2] = {0};
    WPE_TEST_ASSERT_OK(wpe_network_connect(&(WPE_NetTransportDesc){.timeout_ms = 500}, at_port(port), &t[1]));
    WPE_TEST_ASSERT(poll_until(t, 2, &trace, client_saw_disconnect, 10.0));
    WPE_TEST_ASSERT_EQ_UINT(WPE_NET_REASON_CONNECT_FAILED, trace.events[0].reason);
    wpe_network_destroy(t[1]);
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_network_memory_connect_and_exchange),
              WPE_TEST_CASE(test_network_memory_reliable_survives_loss),
              WPE_TEST_CASE(test_network_memory_is_deterministic),
              WPE_TEST_CASE(test_network_memory_disconnects),
              WPE_TEST_CASE(test_network_memory_timeouts_and_failures),
              WPE_TEST_CASE(test_network_rejects_bad_arguments),
              WPE_TEST_CASE(test_network_address_parse_and_format), WPE_TEST_CASE(test_network_udp_loopback),
              WPE_TEST_CASE(test_network_udp_reliable_survives_loss),
              WPE_TEST_CASE(test_network_udp_connect_fails_without_host))
