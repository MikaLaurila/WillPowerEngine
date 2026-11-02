# Networking

This guide explains how WillPowerEngine's networking works, from the ground up. It grows with the
networking milestone; sections marked *(coming)* describe parts that are not built yet.

1. [The basics: packets, UDP and why games use it](#the-basics)
2. [Transports: connecting and sending messages](#transports)
3. [Testing without a network: the in-memory hub](#the-in-memory-hub)
4. [Packing messages: bits, quantization and rotations](#packing-messages)
5. [Script values on the wire](#script-values-on-the-wire)
6. [Sessions: players, joining and refusals](#sessions)
7. [LAN discovery: finding games on your network](#lan-discovery)
8. [Playing together: the engine and the project file](#playing-together)
9. [Replication: keeping objects in sync](#replication)
10. Scripting multiplayer games *(coming)*

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

## Sessions

A transport only knows *connections*. A game needs more: who the players are, whether someone may
join, and what everyone is playing. That is the **session** (`network/session.h`), built on a
transport.

**Topology.** WillPowerEngine uses a **star**: one player *hosts*, everyone else connects to the
host, and players talk only to the host. The host sees everything and decides what is true (who
finished first, whose turn it is) and relays what the others need to know. The alternative, every
player connected to every other (peer-to-peer), has no referee and gets complicated quickly, so
most small games use a star with a player hosting (a *listen server*). A host that only serves,
without playing, is a *dedicated server* (the runtime gets options for that later in this milestone).

**Players** are numbered: the host is player 0, and joiners get the lowest free number from 1
(a player who leaves frees the number for the next one). Everyone knows every player's name.

**Joining** is a short conversation, a *handshake*:

```
player                                       host
  |--- connect (transport) ------------------->|
  |--- HELLO: protocol 1, game id, "Alice" --->|   checks the version, the game, room
  |<-- WELCOME: you are 1, players, scene -----|   (or a refusal: disconnect with a reason)
  |                                            |--- PLAYER_JOINED "Alice" ---> the others
```

The **game id** is a 64-bit fingerprint of the game: two copies of the game that differ in a
script or scene would behave differently, and playing them together would go wrong in confusing
ways. So the host refuses players whose id differs (`WPE_SESSION_REASON_WRONG_GAME`), as it does
for another engine protocol (`VERSION`), a full game (`FULL`) or a host not taking players
(`NOT_ACCEPTING`).

The session reports what happens as events:

| Event | Meaning |
|---|---|
| `JOINED` | This side is in the game (the host: right away), as `player` |
| `PLAYER_JOINED` | `player` joined; `text` is the name. A joiner gets one for everyone already there |
| `PLAYER_LEFT` | `player` left; `reason` says why (closed, timed out, kicked...) |
| `SCENE` | The host is playing the scene `text` now |
| `MESSAGE` | A message of `kind` from `player` (layers above the session use kinds 16..255) |
| `ENDED` | This player's game is over: refused, kicked, the host left or went silent |

The host **kicks** with `wpe_session_kick(session, player, reason)`, the reason being the game's
own (from `WPE_SESSION_REASON_GAME`), so the kicked player can say why.

**Distrust, again.** A session also checks every session message. A player sending garbage, or
anything but HELLO first, is refused (`PROTOCOL`), and so is one that connects but never says
HELLO. A player who receives nonsense from a "host" leaves. The tests play both: hostile players
sending random bytes, and a fake host answering with random messages.

## LAN discovery

Typing an IP address is no fun, so on a local network (home Wi-Fi, a LAN party) players can *find*
games. `network/discovery.h` uses UDP **broadcast**: a packet sent to the address
255.255.255.255 reaches every machine on the local network (but no further: routers do not pass
broadcasts on, so this never finds games on the internet).

- A host runs a **beacon** on the *discovery port* (47809 unless the project says otherwise).
- A player looking for games runs a **browser**. Every second it broadcasts a query: "is anyone
  hosting the game with this id?"
- Beacons of the same game answer with their game port, the player count, and the game's and
  host's names. Other games stay silent.
- The browser lists what answered, and drops a game it has not heard from for 3.5 seconds.

```c
WPE_DiscoveryBrowser* browser = NULL;
wpe_discovery_browser_create(&(WPE_DiscoveryDesc){.game_id = wpe_engine_game_id(engine)}, &browser);
/* each frame: */
wpe_discovery_browser_update(browser);
for (uint32_t i = 0; i < wpe_discovery_browser_count(browser); ++i) {
    WPE_DiscoveredGame game;
    wpe_discovery_browser_game(browser, i, &game);
    /* show game.info.game_name, game.info.host_name, game.info.players / game.info.max_players;
       to join, connect to game.address */
}
```

A session hosts with a beacon when `desc.discoverable` is set; the engine does this unless told
`hidden`.

## Playing together

The engine (`engine/engine.h`) puts it together. A game hosts or joins with one call:

```c
/* Host: */
wpe_engine_net_start(engine, &(WPE_EngineNetDesc){.player_name = "Mika"});
/* Join: */
wpe_engine_net_start(engine, &(WPE_EngineNetDesc){.join = "192.168.1.20", .player_name = "Alice"});
```

From then on the engine takes the session's events at the start of every frame and sends what was
queued at the end. **The host's scene is shared**: whenever the host loads a scene file, every
player loads it too, and a player who joins loads the host's current scene. (A player can sit in its
own menu scene until then.) If the host leaves, the players' sessions end and they stay where they
are. `wpe_engine_net_stop` leaves or closes the game.

The **game id** (`wpe_engine_game_id`) is computed from the files that decide what the game does:
`project.json`, scenes, prefabs and scripts. Line endings do not count (a Windows and a Linux
checkout of the same game match), and neither do other files such as textures or notes.

Ports and limits come from the project file:

```json
"network": {"port": 47810, "discovery_port": 47809, "max_players": 4, "timeout_ms": 5000}
```

**Firewalls.** The first time a game hosts, Windows may ask whether to allow it on the network:
allow it on private networks, or nobody can join. Playing over the internet needs the host's router
to forward the game's UDP port to the host (*port forwarding*). Connecting through routers
without that (*NAT traversal*) is not supported yet.

## Replication

Everyone is now in the same scene, but each machine simulates it on its own: my car moves on my
screen only. **Replication** keeps the things that matter in step on every screen.

### Owners

Mark an entity as networked in its scene file and name its **owner**, the player who simulates it:

```json
{"id": 1, "name": "car_red",  "net": {"owner": 0}, "rigidbody": {"type": "dynamic"}, ...},
{"id": 2, "name": "car_blue", "net": {"owner": 1}, "rigidbody": {"type": "dynamic"}, ...}
```

The owner runs the entity as if there were no network: its physics, its scripts, its input. A few
dozen times a second (the project's `tick_rate`, 30 by default) the owner sends the entity's
**state**: its position, rotation and velocity. Everyone else shows the entity as those states say.
Here the host drives the red car, player 1 the blue one, and each sees the other's.

This is **owner authority**: the owner's word is final. It makes your own car respond the instant
you touch the controls, with no round trip to the host, and it is what most casual racing and
action games do. Its weakness is trust: a modified game could claim its car is anywhere. Between
friends that is fine. (The alternative, the host simulating everything and players only sending
their controls, is fair but makes every control respond a round trip late unless the game
*predicts* and *corrects* its own car, which needs a physics engine that can rewind. Box3D cannot,
so the engine does not do that.)

The host still checks what it can: **it only accepts a player's states for entities that player
owns**, so nobody can move someone else's car, and it relays each state to the other players.

The host can hand an entity to another player while playing (`wpe_engine_net_set_owner`), for
example a car to each player who joins. A player who leaves gives their entities back to the host.

### Showing remote entities smoothly

States arrive about 30 times a second, but the game draws 60 frames a second or more; packets also
arrive unevenly (jitter), and some never arrive. Drawing each state the moment it arrives would make
remote cars stutter.

So remote entities are drawn **slightly in the past**: `interpolation_ms` (100 ms by default)
behind. By then the receiver usually holds the states on both sides of that moment, and it draws
the entity **between** them (interpolation):

```
states received:   [t=0.000]------[t=0.033]------[t=0.067]------[t=0.100]
                                       ^
                          drawn: now - 100 ms, 40% of the way from 0.033 to 0.067
```

A lost state just means a longer step between the two around it; the movement stays smooth. If the
newest state is late anyway, the entity coasts on its last velocity for up to a quarter of a second.
The price is that you see other players 100 ms in the past, plus the time their states took to
arrive. In a racing game, a car slightly behind where it really is goes unnoticed; a bigger
`interpolation_ms` survives worse networks, a smaller one shows others more up to date.

To place a state in time, every state carries the sender's clock. The receiver learns how the
sender's clock relates to its own, and smooths that estimate so jitter does not shake the entities.

### Physics

A remote entity with a **dynamic** rigid body becomes **kinematic** on the screens that do not own
it: it no longer falls or bounces by itself, it follows the states it receives. Moving with velocity,
it still pushes dynamic bodies, so cars still bump each other: when the red car hits the blue one on
the blue car's owner's screen, the blue car is pushed by the (kinematic) red one, and the owner
sends where it ended up. When ownership comes back, the body is dynamic again and carries on with
the velocity it was last seen moving at.

### How entities are recognized

Messages name entities by a **network id**. Nobody needs to send those: every machine loads the
same scene file, so the networked entities are numbered in the order the scene creates them, and
the numbers agree everywhere. (The game id check guarantees the files are the same.) Entities a
script creates later are not networked yet.

Every message also carries the **scene epoch**, a count of the host's scene changes, so a state or
an ownership change meant for one scene is never applied to the next one, even when it arrives
just as a player switches scenes.

### Testing it

`tests/unit/engine/test_engine_network.c` plays two and three engines on the in-memory hub and
checks, frame by frame, that remote cars never move backwards or jump, under 60 ms of latency, 20 ms
of jitter and 10% loss; that both screens agree exactly once the cars stop; that ownership moves and
comes back; that a player's forged states for another player's car are ignored; and that random
replication messages harm nothing.
