/**
 * @file replication.h
 * @brief Replication: keeping networked entities in step on every player's machine.
 *
 * An entity with a WPE_NetIdentity (scene files: "net": {"owner": 1}) is networked. Its **owner**
 * (a player; 0 is the host) simulates it as usual and sends its state (local position, rotation and
 * velocity) at the project's tick rate. Everyone else shows it **interpolated**: they keep the last
 * few states they received and draw the entity `interpolation_ms` in the past, between two known
 * states, so it moves smoothly even when packets arrive unevenly or go missing. When the newest
 * state is late, the entity coasts on its last velocity for up to 250 ms.
 *
 * The host relays: a player's entity reaches the other players through the host. The host only
 * accepts a player's states for entities that player owns.
 *
 * Remote entities with a dynamic rigid body become kinematic while someone else owns them: they
 * follow the received states with velocity, so they still push the owner's local bodies (cars still
 * bump). They become dynamic again when ownership comes back.
 *
 * Network ids: every machine loads the same scene file, so the entities with a NetIdentity are
 * numbered in spawn order when a scene loads (1, 2, 3...) and agree everywhere without sending
 * ids. Entities spawned later are not networked (yet).
 *
 * Ownership is the host's to change (wpe_replication_set_owner()): it sends the whole table,
 * reliably, whenever it changes, and to every player who joins. A player who leaves gives their
 * entities back to the host. Messages carry the session's scene epoch, so nothing about one scene is
 * applied to the next.
 *
 * Messages (session kinds):
 *   STATE (16, unreliable): epoch, sender clock (ms), count, then per entity:
 *                           net id, position (3 floats), rotation (smallest three, 15 bits),
 *                           velocity (quantized to +-256 m/s, 16 bits per axis)
 *   OWNERS (17, reliable):  epoch, count, then each id's owner (3 bits)
 *
 * - Dependencies: ecs, scene, physics, network
 * - Thread affinity: main thread
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_ENGINE_REPLICATION_H
#define WPE_ENGINE_REPLICATION_H

#include <stdint.h>

#include "core/result.h"
#include "ecs/ecs.h"
#include "math/wpe_math.h"
#include "network/session.h"
#include "physics/physics.h"

#define WPE_NET_SAMPLES 8u
#define WPE_REPLICATION_KIND_STATE 16u
#define WPE_REPLICATION_KIND_OWNERS 17u

/** A received state (private to replication). */
typedef struct WPE_NetSample {
    double time_ms; /* On the receiver's clock. */
    WPE_Vec3 position;
    WPE_Quat rotation;
    WPE_Vec3 velocity;
} WPE_NetSample;

/** A networked entity. Only `owner` is saved; the rest is managed at runtime. */
typedef struct WPE_NetIdentity {
    uint32_t owner; /* The player that simulates it and sends its state (0: the host). */

    /* Runtime (reset when the component is added). */
    uint32_t net_id; /* 1.. for scene entities; 0: not networked. */
    WPE_NetSample wpe__samples[WPE_NET_SAMPLES];
    uint32_t wpe__sample_count;
    bool wpe__remote;         /* Shown from received states. */
    bool wpe__made_kinematic; /* Its dynamic body was made kinematic while remote. */
} WPE_NetIdentity;

typedef struct WPE_Replication WPE_Replication;

typedef struct WPE_ReplicationDesc {
    WPE_Ecs* ecs;            /* Borrowed; WPE_COMP_NET_IDENTITY registered. */
    WPE_Physics* physics;    /* Borrowed; may be NULL. */
    double tick_rate;        /* States sent per second (1..120). */
    double interpolation_ms; /* How far in the past remote entities are shown (0..1000). */
} WPE_ReplicationDesc;

/** Registers WPE_COMP_NET_IDENTITY ("NetIdentity"). @thread_safety Owning thread of the world */
WPE_Result wpe_replication_register_components(WPE_Ecs* ecs);

/** @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, _INVALID_ARG or _OUT_OF_MEMORY. @thread_safety Main
 * thread */
[[nodiscard]] WPE_Result wpe_replication_create(const WPE_ReplicationDesc* desc,
                                                WPE_Replication** out_replication);

/** Frees it (NULL is a no-op); destroy it before the world. @thread_safety Main thread */
void wpe_replication_destroy(WPE_Replication* replication);

/**
 * A scene has loaded: numbers its networked entities. `shared` tells whether it is the session's
 * scene (the host's, or a player's load of it); only then does replication run. A host sends the
 * ownership table.
 *
 * @thread_safety Main thread
 */
void wpe_replication_scene_loaded(WPE_Replication* replication, WPE_Session* session, bool shared);

/** The session ended or stopped: every entity is local again from the next update. @thread_safety Main thread
 */
void wpe_replication_stop(WPE_Replication* replication);

/** Takes a session MESSAGE of a replication kind (others are ignored). @thread_safety Main thread */
void wpe_replication_receive(WPE_Replication* replication, WPE_Session* session,
                             const WPE_SessionEvent* event);

/** Host: a player joined (gets the ownership table). @thread_safety Main thread */
void wpe_replication_player_joined(WPE_Replication* replication, WPE_Session* session, WPE_PlayerId player);

/** Host: a player left (its entities go back to the host). @thread_safety Main thread */
void wpe_replication_player_left(WPE_Replication* replication, WPE_Session* session, WPE_PlayerId player);

/**
 * Advances the replication clock and poses remote entities (call before the simulation steps, with
 * real time: hit-stops don't stop the network).
 *
 * @thread_safety Main thread
 */
void wpe_replication_apply(WPE_Replication* replication, WPE_Session* session, double real_dt);

/** Sends the states of this side's entities when a tick is due (after the simulation). @thread_safety Main
 * thread */
void wpe_replication_send(WPE_Replication* replication, WPE_Session* session);

/**
 * Host: gives a networked entity to a player present in the game (or 0, the host).
 *
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_NOT_FOUND (not a networked
 *         entity); WPE_RESULT_ERROR_INVALID_ARG (no such player); WPE_RESULT_ERROR_INVALID_STATE (a
 *         player's session: only the host decides). Without a session the owner is just recorded.
 *
 * @thread_safety Main thread
 */
WPE_Result wpe_replication_set_owner(WPE_Replication* replication, WPE_Session* session, WPE_Entity entity,
                                     WPE_PlayerId player);

/** Whether this side simulates the entity (true for anything not networked, or without a running session).
 * @thread_safety Main thread */
bool wpe_replication_is_local(const WPE_Replication* replication, const WPE_Session* session,
                              WPE_Entity entity);

/** The entity with a network id, or WPE_ENTITY_NULL. @thread_safety Main thread */
WPE_Entity wpe_replication_entity(const WPE_Replication* replication, uint32_t net_id);

#endif /* WPE_ENGINE_REPLICATION_H */
