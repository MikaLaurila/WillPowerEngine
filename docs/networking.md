# Networking

This guide explains how WillPowerEngine's networking works, from the ground up. It grows with the
networking milestone; sections marked *(coming)* describe parts that are not built yet.

1. [The basics: packets, UDP and why games use it](#the-basics)
2. [Transports: connecting and sending messages](#transports)
3. [Testing without a network: the in-memory hub](#the-in-memory-hub)
4. [Packing messages: bits, quantization and rotations](#packing-messages)
5. [Script values on the wire](#script-values-on-the-wire)
6. Sessions, LAN discovery and replication *(coming)*

## The basics

Computers on a network exchange **packets**: small chunks of bytes (usually under about 1400
bytes) sent to an address and port. There are two common ways to send them:

- **TCP** gives you a reliable, ordered stream of bytes. If a packet is lost, TCP resends it, and
  everything sent after it waits until it arrives. That wait (*head-of-line blocking*) is bad for
  games: one lost packet can freeze a car's position for a fraction of a second, even though a
  newer position is already on its way.
- **UDP** sends each packet on its own. It may be lost, duplicated or arrive out of order, and
  nobody tells you. That sounds worse, but it lets a game decide what matters: a car's position from
  100 ms ago is worthless once a newer one has arrived, so there is no point waiting for it.

Games usually want both kinds of delivery: **reliable** for things that must happen exactly once
("player 2 finished lap 3", "a chat message"), and **unreliable** for state that is replaced many
times a second (positions, rotations). The engine uses **ENet**, a small C library that builds both
on top of UDP: it numbers packets, acknowledges them, resends the reliable ones that went missing,
drops stale unreliable ones, and splits big messages into packet-sized fragments.

Three numbers describe how good a connection is:

- **Latency** (ping): how long a packet takes to arrive. Round-trip time (RTT) is there and back.
  On a LAN it is about 1 ms; across a country 20–60 ms; across the world 150 ms or more.
- **Jitter**: how much the latency varies from packet to packet.
- **Packet loss**: the share of packets that never arrive. Wi-Fi and mobile networks lose a few
  percent at times.

A networked game has to look smooth despite all three. Later sections (replication) show how: by
sending state regularly, and drawing remote objects slightly in the past, between two known states.

## Transports

`network/transport.h` is the lowest layer. A **transport** is one end of the conversation.

```c
#include "network/network.h"

/* The host listens on a port... */
WPE_NetTransport* host = NULL;
wpe_network_host(&(WPE_NetTransportDesc){.port = WPE_NET_DEFAULT_PORT, .max_peers = 4}, &host);

/* ...and each player connects to it. */
WPE_NetAddress address;
wpe_network_address_parse("192.168.1.20", WPE_NET_DEFAULT_PORT, &address);
WPE_NetTransport* client = NULL;
wpe_network_connect(&(WPE_NetTransportDesc){0}, address, &client);
```

Everything that happens arrives as an **event**, which you take with `wpe_network_poll` once per
frame until it returns false:

```c
WPE_NetEvent event;
while (wpe_network_poll(host, &event)) {
    switch (event.type) {
    case WPE_NET_EVENT_CONNECTED:    /* event.peer joined */ break;
    case WPE_NET_EVENT_DISCONNECTED: /* event.peer left; event.reason says why */ break;
    case WPE_NET_EVENT_MESSAGE:      /* event.data / event.size, valid until the next poll */ break;
    default: break;
    }
}
```

Each connection is a **peer**, numbered from 0. On a host the peers are the players; on a client
there is one peer, the host (peer 0). Messages go to a peer on one of two **channels**:

| Channel | Delivery | Use it for |
|---|---|---|
| `WPE_NET_CHANNEL_RELIABLE` | Every message, exactly once, in order | Events, chat, "start the race" |
| `WPE_NET_CHANNEL_UNRELIABLE` | May be lost; an older one than already delivered is dropped | Positions and other state sent often |

A peer **disconnects** for a reason: the other side closed (`WPE_NET_REASON_CLOSED`), nothing was
heard from it for `timeout_ms` (`TIMEOUT`, e.g. a pulled cable), the connection was never made
(`CONNECT_FAILED`, e.g. a wrong address or a full host), or a reason of the game's own, from
`WPE_NET_REASON_USER` up ("kicked", "wrong version"...). `wpe_network_disconnect` sends that reason
after the messages already sent to the peer, so a final "you were kicked because..." message still
arrives first.

### Simulating a bad network

Every transport can pretend its connection is worse than it is, which is how you find out early
what lag does to a game. `WPE_NetConditions` applies to what that transport *receives*:

```c
WPE_NetTransportDesc desc = {
    .conditions = {.latency_ms = 100, .jitter_ms = 30, .loss = 0.05f, .seed = 1},
};
```

Over real UDP, lost packets are thrown away before ENet sees them (so ENet really resends the
reliable ones, as on a bad Wi-Fi link), and arriving events are held back by the latency. Events of
one peer are never reordered by the simulation.

## The in-memory hub

Real networks are not repeatable: timing differs on every run, so a test that passes once may fail
the next time. For tests, a transport can live on a **hub** (`WPE_NetHub`) instead: a pretend
network inside the process.

```c
WPE_NetHub* hub = NULL;
wpe_network_hub_create(&hub);
WPE_NetTransport* host = NULL;
wpe_network_host(&(WPE_NetTransportDesc){.hub = hub, .port = 7000}, &host);
WPE_NetTransport* client = NULL;
wpe_network_connect(&(WPE_NetTransportDesc){.hub = hub}, (WPE_NetAddress){.port = 7000}, &client);

for (int frame = 0; frame < 60; ++frame) {
    wpe_network_hub_advance(hub, 16); /* 16 ms pass; due packets are delivered */
    /* ...poll both transports, run the game... */
}
```

The hub's clock moves **only** when you call `wpe_network_hub_advance`. Every packet waits in the
hub until its arrival time (set by the receiver's simulated conditions), so with the same seeds the
same packets are lost and arrive at the same times on every run and every machine. A minute of play
with 150 ms of lag and 10% loss runs in a fraction of a second, the same way each time.
`wpe_network_hub_cut(hub, transport)` pulls a transport's cable, to test timeouts.

The game code does not change between the hub and real UDP: only `desc.hub` differs.

## Packing messages

Games send state many times a second to every player, so message size matters. `network/bits.h`
writes values in exactly as many **bits** as they need:

```c
uint8_t buffer[256];
WPE_BitWriter w = wpe_bits_writer(buffer, sizeof buffer);
wpe_bits_write_bool(&w, braking);           /* 1 bit */
wpe_bits_write(&w, lap, 4);                 /* 4 bits: laps 0..15 */
wpe_bits_write_vec3(&w, position, -512.0f, 512.0f, 18);   /* 3 x 18 bits */
wpe_bits_write_quat(&w, rotation, 15);      /* 2 + 3 x 15 bits */
if (w.error) { /* the buffer was too small */ }
wpe_network_send(t, peer, WPE_NET_CHANNEL_UNRELIABLE, buffer, wpe_bits_writer_size(&w));
```

The reader on the other side reads the same things in the same order:

```c
WPE_BitReader r = wpe_bits_reader(event.data, event.size);
bool braking = wpe_bits_read_bool(&r);
uint32_t lap = wpe_bits_read(&r, 4);
WPE_Vec3 position = wpe_bits_read_vec3(&r, -512.0f, 512.0f, 18);
WPE_Quat rotation = wpe_bits_read_quat(&r, 15);
if (r.error) { /* malformed or truncated: ignore the message */ }
```

Two tricks do most of the saving:

- **Quantization.** A float takes 32 bits, but a position on a track 1 km wide does not need that
  precision. `wpe_bits_write_quantized(w, value, min, max, bits)` stores the value as one of 2^bits
  evenly spaced steps between min and max. With 18 bits over 1024 m, a step is 1024 / 262143 ≈ 4 mm
  and the error at most half of that: invisible, at 56% of the size.
- **"Smallest three" rotations.** A rotation quaternion has four components, but its length is
  always 1, so the largest one can be worked out from the other three. Those three are never
  bigger than 1/√2 ≈ 0.707, so they quantize well. The writer sends which component is the largest
  (2 bits) and the other three (15 bits each, an error under 0.01°): 47 bits instead of 128.

**Errors are sticky.** A writer that runs out of room, or a reader that runs past the end, sets its
`error` flag; later calls do nothing (reads return zeros). So you check once at the end instead of
after every call, and a malformed message can never read outside its buffer.

**Never trust data from the network.** Anyone can send any bytes to your port, and a bug in another
version of the game can too. Everything that reads messages assumes they may be broken or hostile;
the tests throw random and corrupted data at the readers (*fuzzing*) to check that they reject it
safely.

## Script values on the wire

Scripts will send each other values (`wpe.net_send` *(coming)*): numbers, strings, booleans,
vectors and tables of them. `network/value.h` is their wire format:

| Value | Encoding |
|---|---|
| `nil`, `false`, `true` | a 3-bit tag |
| an integer that fits in 32 bits | tag + a variable-length integer (0..63 and -64..-1 take 1 byte) |
| any other number | tag + 64 bits (exact) |
| string | tag + length + bytes |
| vector | tag + three 32-bit floats |
| table | tag + entry count, then each key and value |

For example, `{lap = 3, best = 61.25}` takes 22 bytes. The reader checks everything before a script
sees it: tables nest at most 16 deep, a message holds at most 65536 values, keys must be strings,
booleans or numbers (not NaN), and a table cannot claim more entries than its bytes could hold, so
a forged "4 billion entries" header cannot make the game allocate gigabytes.
