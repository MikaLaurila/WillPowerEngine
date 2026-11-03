/* Replication of networked entities (see replication.h). */
#define WPE_LOG_MODULE "network"
#include "engine/replication.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "core/alloc.h"
#include "core/log.h"
#include "network/bits.h"
#include "scene/scene.h"

#define WPE__MAX_EXTRAPOLATION_MS 250.0
#define WPE__ENTRIES_PER_MESSAGE 40u /* Keeps a state message under a typical packet (about 1200 bytes). */
#define WPE__STATE_BUFFER 1400u
#define WPE__MAX_VELOCITY 256.0f
#define WPE__ROTATION_BITS 15u
#define WPE__VELOCITY_BITS 16u
#define WPE__MAX_NET_IDS 65535u
#define WPE__JUMP_SLACK 0.5 /* Metres a state may be off on top of WPE_NET_JUMP_SPEED (clock jitter). */

struct WPE_Replication {
    WPE_Ecs* ecs;
    WPE_Physics* physics;
    double interval_ms;
    double interpolation_ms;
    double clock_ms; /* Real time since creation. */
    double next_send_ms;
    bool active;       /* Playing the session's scene. */
    uint32_t epoch;    /* Its scene epoch. */
    WPE_Entity* by_id; /* by_id[net_id]; [0] unused. */
    uint32_t id_count; /* Ids 1..id_count. */
    /* Per source player: the sender's clock minus ours (smoothed), and its last time. */
    double offset[WPE_SESSION_MAX_PLAYERS];
    double last_sender[WPE_SESSION_MAX_PLAYERS];
    bool clock_known[WPE_SESSION_MAX_PLAYERS];
    /* A player: an ownership table for a scene not loaded yet. */
    uint32_t pending_epoch;
    uint8_t* pending_owners;
    uint32_t pending_count;
};

/* ---- Component ------------------------------------------------------------------------------- */

static void wpe__identity_on_add(WPE_Ecs* ecs, WPE_Entity entity, void* component, void* user_data) {
    (void)ecs;
    (void)entity;
    (void)user_data;
    WPE_NetIdentity* n = component;
    const uint32_t owner = n->owner < WPE_SESSION_MAX_PLAYERS ? n->owner : 0;
    *n = (WPE_NetIdentity){.owner = owner}; /* Copies share no runtime state. */
}

WPE_Result wpe_replication_register_components(WPE_Ecs* ecs) {
    if (!ecs) return WPE_RESULT_ERROR_NULL_ARG;
    const WPE_ComponentDesc desc = {.name = "NetIdentity",
                                    .size = sizeof(WPE_NetIdentity),
                                    .alignment = alignof(WPE_NetIdentity),
                                    .on_add = wpe__identity_on_add};
    return wpe_ecs_register_component(ecs, WPE_COMP_NET_IDENTITY, &desc);
}

/* ---- Lifetime -------------------------------------------------------------------------------- */

WPE_Result wpe_replication_create(const WPE_ReplicationDesc* desc, WPE_Replication** out_replication) {
    if (!desc || !desc->ecs || !out_replication) return WPE_RESULT_ERROR_NULL_ARG;
    *out_replication = NULL;
    if (!(desc->tick_rate >= 1.0 && desc->tick_rate <= 120.0) ||
        !(desc->interpolation_ms >= 0.0 && desc->interpolation_ms <= 1000.0)) {
        wpe_log_error("replication needs a tick rate of 1..120 and an interpolation delay of 0..1000 ms");
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    WPE_Replication* r = wpe_alloc_zeroed(1, sizeof *r);
    if (!r) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    r->ecs = desc->ecs;
    r->physics = desc->physics;
    r->interval_ms = 1000.0 / desc->tick_rate;
    r->interpolation_ms = desc->interpolation_ms;
    *out_replication = r;
    return WPE_RESULT_OK;
}

void wpe_replication_destroy(WPE_Replication* replication) {
    if (!replication) return;
    wpe_free(replication->by_id);
    wpe_free(replication->pending_owners);
    wpe_free(replication);
}

/* ---- Helpers --------------------------------------------------------------------------------- */

static WPE_NetIdentity* wpe__identity(const WPE_Replication* r, uint32_t net_id, WPE_Entity* out_entity) {
    if (net_id == 0 || net_id > r->id_count) return NULL;
    const WPE_Entity e = r->by_id[net_id];
    WPE_NetIdentity* n = wpe_ecs_get_component(r->ecs, e, WPE_COMP_NET_IDENTITY);
    if (!n || n->net_id != net_id) return NULL; /* Despawned (or the slot was reused). */
    if (out_entity) *out_entity = e;
    return n;
}

WPE_Entity wpe_replication_entity(const WPE_Replication* replication, uint32_t net_id) {
    WPE_Entity e = WPE_ENTITY_NULL;
    return replication && wpe__identity(replication, net_id, &e) ? e : WPE_ENTITY_NULL;
}

static bool wpe__running(const WPE_Replication* r, const WPE_Session* session) {
    return r->active && session && wpe_session_local_player(session) != WPE_SESSION_NO_PLAYER;
}

/* Sends the ownership table: to one player, or to all (player == WPE_SESSION_NO_PLAYER). */
static void wpe__send_owners(WPE_Replication* r, WPE_Session* session, WPE_PlayerId player) {
    if (!wpe__running(r, session) || !wpe_session_is_host(session)) return;
    const size_t capacity = 16 + ((size_t)r->id_count * 3 + 7) / 8;
    uint8_t* message = wpe_alloc(capacity);
    if (!message) return;
    WPE_BitWriter w = wpe_bits_writer(message, capacity);
    wpe_bits_write_varint(&w, r->epoch);
    wpe_bits_write_varint(&w, r->id_count);
    for (uint32_t id = 1; id <= r->id_count; ++id) {
        const WPE_NetIdentity* n = wpe__identity(r, id, NULL);
        wpe_bits_write(&w, n ? n->owner : 0, 3);
    }
    if (!w.error) {
        const size_t size = wpe_bits_writer_size(&w);
        if (player == WPE_SESSION_NO_PLAYER)
            (void)wpe_session_broadcast(session, WPE_NET_CHANNEL_RELIABLE, WPE_REPLICATION_KIND_OWNERS,
                                        message, size);
        else
            (void)wpe_session_send(session, player, WPE_NET_CHANNEL_RELIABLE, WPE_REPLICATION_KIND_OWNERS,
                                   message, size);
    }
    wpe_free(message);
}

typedef struct wpe__Spawned {
    uint64_t serial;
    WPE_Entity entity;
} wpe__Spawned;

static int wpe__by_serial(const void* a, const void* b) {
    const uint64_t x = ((const wpe__Spawned*)a)->serial;
    const uint64_t y = ((const wpe__Spawned*)b)->serial;
    return (x > y) - (x < y);
}

/* Numbers the networked entities in spawn order (the same on every machine for one scene file). */
static void wpe__assign_ids(WPE_Replication* r) {
    r->id_count = 0;
    uint32_t count = 0;
    WPE_EcsIter it = wpe_ecs_query(r->ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_NET_IDENTITY)});
    while (wpe_ecs_iter_next(&it))
        ++count;
    if (count > WPE__MAX_NET_IDS) {
        wpe_log_error("%u networked entities: only the first %u are replicated", count, WPE__MAX_NET_IDS);
        count = WPE__MAX_NET_IDS;
    }
    wpe__Spawned* spawned = count ? wpe_alloc(count * sizeof *spawned) : NULL;
    WPE_Entity* by_id = wpe_realloc(r->by_id, ((size_t)count + 1) * sizeof *by_id);
    if (count && (!spawned || !by_id)) {
        wpe_free(spawned);
        if (by_id) r->by_id = by_id;
        wpe_log_error("out of memory: networked entities are not replicated");
        return;
    }
    if (by_id) r->by_id = by_id;
    uint32_t n = 0;
    it = wpe_ecs_query(r->ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_NET_IDENTITY)});
    while (wpe_ecs_iter_next(&it)) {
        WPE_NetIdentity* identity = wpe_ecs_iter_get(&it, WPE_COMP_NET_IDENTITY);
        identity->net_id = 0;
        if (n < count)
            spawned[n++] =
                (wpe__Spawned){.serial = wpe_ecs_spawn_serial(r->ecs, it.entity), .entity = it.entity};
    }
    if (n > 1) qsort(spawned, n, sizeof *spawned, wpe__by_serial);
    for (uint32_t i = 0; i < n; ++i) {
        WPE_NetIdentity* identity = wpe_ecs_get_component(r->ecs, spawned[i].entity, WPE_COMP_NET_IDENTITY);
        identity->net_id = i + 1;
        identity->wpe__sample_count = 0;
        r->by_id[i + 1] = spawned[i].entity;
    }
    r->id_count = n;
    wpe_free(spawned);
}

static void wpe__apply_owners(WPE_Replication* r, const uint8_t* owners, uint32_t count) {
    for (uint32_t id = 1; id <= count && id <= r->id_count; ++id) {
        WPE_NetIdentity* n = wpe__identity(r, id, NULL);
        if (n) n->owner = owners[id - 1];
    }
}

/* ---- Scenes and players ---------------------------------------------------------------------- */

void wpe_replication_scene_loaded(WPE_Replication* replication, WPE_Session* session, bool shared) {
    if (!replication) return;
    WPE_Replication* r = replication;
    wpe__assign_ids(r);
    r->active = shared && session != NULL;
    r->epoch = session ? wpe_session_scene_epoch(session) : 0;
    memset(r->clock_known, 0, sizeof r->clock_known);
    r->next_send_ms = r->clock_ms;
    if (!r->active) return;
    if (wpe_session_is_host(session)) {
        wpe__send_owners(r, session, WPE_SESSION_NO_PLAYER);
    } else if (r->pending_owners && r->pending_epoch == r->epoch) {
        wpe__apply_owners(r, r->pending_owners, r->pending_count);
    }
}

void wpe_replication_stop(WPE_Replication* replication) {
    if (replication) replication->active = false;
}

void wpe_replication_player_joined(WPE_Replication* replication, WPE_Session* session, WPE_PlayerId player) {
    if (replication) wpe__send_owners(replication, session, player);
}

void wpe_replication_player_left(WPE_Replication* replication, WPE_Session* session, WPE_PlayerId player) {
    if (!replication || !wpe_session_is_host(session)) return;
    bool changed = false;
    for (uint32_t id = 1; id <= replication->id_count; ++id) {
        WPE_NetIdentity* n = wpe__identity(replication, id, NULL);
        if (n && n->owner == player) {
            n->owner = WPE_SESSION_HOST;
            changed = true;
        }
    }
    if (changed) wpe__send_owners(replication, session, WPE_SESSION_NO_PLAYER);
}

WPE_Result wpe_replication_set_owner(WPE_Replication* replication, WPE_Session* session, WPE_Entity entity,
                                     WPE_PlayerId player) {
    if (!replication) return WPE_RESULT_ERROR_NULL_ARG;
    WPE_NetIdentity* n = wpe_ecs_get_component(replication->ecs, entity, WPE_COMP_NET_IDENTITY);
    if (!n || n->net_id == 0) return WPE_RESULT_ERROR_NOT_FOUND;
    if (player >= WPE_SESSION_MAX_PLAYERS) return WPE_RESULT_ERROR_INVALID_ARG;
    if (session && wpe_session_local_player(session) != WPE_SESSION_NO_PLAYER) {
        if (!wpe_session_is_host(session)) return WPE_RESULT_ERROR_INVALID_STATE;
        WPE_SessionPlayer info;
        if (!wpe_session_player(session, player, &info)) return WPE_RESULT_ERROR_INVALID_ARG;
    }
    if (n->owner == player) return WPE_RESULT_OK;
    n->owner = player;
    wpe__send_owners(replication, session, WPE_SESSION_NO_PLAYER);
    return WPE_RESULT_OK;
}

bool wpe_replication_is_local(const WPE_Replication* replication, const WPE_Session* session,
                              WPE_Entity entity) {
    if (!replication || !wpe__running(replication, session)) return true;
    const WPE_NetIdentity* n = wpe_ecs_get_component(replication->ecs, entity, WPE_COMP_NET_IDENTITY);
    return !n || n->net_id == 0 || n->owner == wpe_session_local_player(session);
}

/* ---- Receiving ------------------------------------------------------------------------------- */

/* The sender's clock in milliseconds, unwrapped from its 32 bits against the last one seen. */
static double wpe__sender_time(WPE_Replication* r, WPE_PlayerId source, uint32_t time32) {
    if (!r->clock_known[source]) return (double)time32;
    const uint32_t last32 = (uint32_t)(uint64_t)r->last_sender[source];
    const uint32_t forward = time32 - last32;
    const double delta = forward < 0x80000000u ? (double)forward : (double)forward - 4294967296.0;
    return r->last_sender[source] + delta;
}

/* Estimates the sender's clock against ours: its time minus ours when its message arrived (one-way
 * delay included, which interpolation absorbs), smoothed so jitter does not shake remote entities. */
static void wpe__track_clock(WPE_Replication* r, WPE_PlayerId source, double sender_ms) {
    const double observed = sender_ms - r->clock_ms;
    if (!r->clock_known[source] || fabs(observed - r->offset[source]) > 1000.0) {
        r->offset[source] = observed;
    } else {
        r->offset[source] += (observed - r->offset[source]) * 0.05;
    }
    r->clock_known[source] = true;
    r->last_sender[source] = sender_ms;
}

static void wpe__add_sample(WPE_NetIdentity* n, WPE_NetSample sample) {
    if (n->wpe__sample_count > 0) {
        const WPE_NetSample* last = &n->wpe__samples[n->wpe__sample_count - 1];
        if (sample.time_ms <= last->time_ms) return;
        /* Faster than anything travels: it jumped (a respawn, a teleport). */
        const double reach = (sample.time_ms - last->time_ms) / 1000.0 * WPE_NET_JUMP_SPEED + WPE__JUMP_SLACK;
        sample.jump = wpe_vec3_length(wpe_vec3_sub(sample.position, last->position)) > reach;
    }
    if (n->wpe__sample_count == WPE_NET_SAMPLES) {
        memmove(&n->wpe__samples[0], &n->wpe__samples[1], (WPE_NET_SAMPLES - 1) * sizeof n->wpe__samples[0]);
        --n->wpe__sample_count;
    }
    n->wpe__samples[n->wpe__sample_count++] = sample;
}

static bool wpe__finite(WPE_Vec3 v) {
    return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

static void wpe__receive_state(WPE_Replication* r, WPE_Session* session, const WPE_SessionEvent* e) {
    const bool host = wpe_session_is_host(session);
    const WPE_PlayerId source = e->player;
    if (source >= WPE_SESSION_MAX_PLAYERS || (host ? source == WPE_SESSION_HOST : source != WPE_SESSION_HOST))
        return;
    WPE_BitReader b = wpe_bits_reader(e->data, e->size);
    const uint32_t epoch = wpe_bits_read_varint(&b);
    const uint32_t time32 = wpe_bits_read(&b, 32);
    const uint32_t count = wpe_bits_read_varint(&b);
    if (b.error || epoch != r->epoch || count > WPE__ENTRIES_PER_MESSAGE) return;
    const double sender_ms = wpe__sender_time(r, source, time32);
    wpe__track_clock(r, source, sender_ms);
    const double local_ms = sender_ms - r->offset[source];
    const WPE_PlayerId local = wpe_session_local_player(session);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t id = wpe_bits_read_varint(&b);
        WPE_NetSample s = {.time_ms = local_ms};
        for (int k = 0; k < 3; ++k)
            s.position.v[k] = wpe_bits_read_float(&b);
        s.rotation = wpe_bits_read_quat(&b, WPE__ROTATION_BITS);
        s.velocity = wpe_bits_read_vec3(&b, -WPE__MAX_VELOCITY, WPE__MAX_VELOCITY, WPE__VELOCITY_BITS);
        if (b.error) return;
        WPE_NetIdentity* n = wpe__identity(r, id, NULL);
        if (!n || !wpe__finite(s.position)) continue;
        /* The host takes a player's states only for what that player owns; nobody takes states of
         * their own entities. */
        const bool allowed = host ? n->owner == source : n->owner != local;
        if (allowed) wpe__add_sample(n, s);
    }
}

static void wpe__receive_owners(WPE_Replication* r, WPE_Session* session, const WPE_SessionEvent* e) {
    if (wpe_session_is_host(session) || e->player != WPE_SESSION_HOST) return;
    WPE_BitReader b = wpe_bits_reader(e->data, e->size);
    const uint32_t epoch = wpe_bits_read_varint(&b);
    const uint32_t count = wpe_bits_read_varint(&b);
    if (b.error || count > WPE__MAX_NET_IDS || (uint64_t)count * 3 > wpe_bits_reader_remaining(&b)) return;
    if (r->active && epoch < r->epoch) return; /* About a scene already left. */
    uint8_t* owners = count ? wpe_alloc(count) : NULL;
    if (count && !owners) return;
    const uint32_t max_players = wpe_session_max_players(session);
    for (uint32_t i = 0; i < count; ++i) {
        owners[i] = (uint8_t)wpe_bits_read(&b, 3);
        if (owners[i] >= max_players) {
            wpe_free(owners);
            return;
        }
    }
    if (r->active && epoch == r->epoch) {
        wpe__apply_owners(r, owners, count);
        wpe_free(owners);
        return;
    }
    /* For a scene still to load: kept until it does. */
    wpe_free(r->pending_owners);
    r->pending_owners = owners;
    r->pending_count = count;
    r->pending_epoch = epoch;
}

void wpe_replication_receive(WPE_Replication* replication, WPE_Session* session,
                             const WPE_SessionEvent* event) {
    if (!replication || !session || !event || event->type != WPE_SESSION_EVENT_MESSAGE) return;
    if (event->kind == WPE_REPLICATION_KIND_STATE && wpe__running(replication, session))
        wpe__receive_state(replication, session, event);
    else if (event->kind == WPE_REPLICATION_KIND_OWNERS)
        wpe__receive_owners(replication, session, event);
}

/* ---- Showing remote entities ----------------------------------------------------------------- */

/* The pose at time t from the samples: between the two around it, the newest coasting on its
 * velocity past the end (briefly), or the oldest before the start. */
static void wpe__pose_at(const WPE_NetIdentity* n, double t, WPE_Vec3* out_position, WPE_Quat* out_rotation) {
    const WPE_NetSample* s = n->wpe__samples;
    const uint32_t count = n->wpe__sample_count;
    if (t <= s[0].time_ms) {
        *out_position = s[0].position;
        *out_rotation = s[0].rotation;
        return;
    }
    for (uint32_t i = count; i-- > 0;) {
        if (s[i].time_ms > t) continue;
        if (i == count - 1) {
            double ahead = t - s[i].time_ms;
            if (ahead > WPE__MAX_EXTRAPOLATION_MS) ahead = WPE__MAX_EXTRAPOLATION_MS;
            *out_position =
                wpe_vec3_add(s[i].position, wpe_vec3_scale(s[i].velocity, (float)(ahead / 1000.0)));
            *out_rotation = s[i].rotation;
            return;
        }
        if (s[i + 1].jump) { /* Stays until the jump's time, then is there: never in between. */
            *out_position = s[i].position;
            *out_rotation = s[i].rotation;
            return;
        }
        const float f = (float)((t - s[i].time_ms) / (s[i + 1].time_ms - s[i].time_ms));
        *out_position = wpe_vec3_lerp(s[i].position, s[i + 1].position, f);
        *out_rotation = wpe_quat_slerp(s[i].rotation, s[i + 1].rotation, f);
        return;
    }
}

/* Switches an entity between local and remote: a remote dynamic body is made kinematic. */
static void wpe__set_remote(WPE_Replication* r, WPE_Entity e, WPE_NetIdentity* n, bool remote) {
    n->wpe__remote = remote;
    n->wpe__posed = false;
    WPE_RigidBody* body = wpe_ecs_get_component(r->ecs, e, WPE_COMP_PHYSICS_RIGIDBODY);
    if (remote && body && body->type == WPE_BODY_DYNAMIC) {
        body->type = WPE_BODY_KINEMATIC;
        n->wpe__made_kinematic = true;
        if (r->physics) (void)wpe_physics_refresh(r->physics, e);
    } else if (!remote) {
        const WPE_Vec3 velocity =
            n->wpe__sample_count ? n->wpe__samples[n->wpe__sample_count - 1].velocity : wpe_vec3(0, 0, 0);
        n->wpe__sample_count = 0;
        if (body && n->wpe__made_kinematic) {
            body->type = WPE_BODY_DYNAMIC;
            n->wpe__made_kinematic = false;
            if (r->physics) {
                (void)wpe_physics_refresh(r->physics, e);
                (void)wpe_physics_set_velocity(r->physics, e, velocity); /* Carry on as it moved. */
            }
        }
    }
}

void wpe_replication_apply(WPE_Replication* replication, WPE_Session* session, double real_dt) {
    if (!replication) return;
    WPE_Replication* r = replication;
    if (real_dt > 0.0) r->clock_ms += real_dt * 1000.0;
    const bool running = wpe__running(r, session);
    const WPE_PlayerId local = running ? wpe_session_local_player(session) : WPE_SESSION_NO_PLAYER;
    const double show_at = r->clock_ms - r->interpolation_ms;
    for (uint32_t id = 1; id <= r->id_count; ++id) {
        WPE_Entity e = WPE_ENTITY_NULL;
        WPE_NetIdentity* n = wpe__identity(r, id, &e);
        if (!n) continue;
        const bool remote = running && n->owner != local;
        if (remote != n->wpe__remote) wpe__set_remote(r, e, n, remote);
        if (!remote || n->wpe__sample_count == 0) continue;
        WPE_Transform* t = wpe_ecs_get_component(r->ecs, e, WPE_COMP_TRANSFORM);
        if (!t) continue;
        const WPE_Vec3 before = t->position;
        wpe__pose_at(n, show_at, &t->position, &t->rotation);
        /* The first pose, or a jump in the states: the body jumps too, rather than travelling there with
         * velocity through whatever is in between. */
        const double reach = real_dt * WPE_NET_JUMP_SPEED + WPE__JUMP_SLACK;
        const bool jumped = !n->wpe__posed || wpe_vec3_length(wpe_vec3_sub(t->position, before)) > reach;
        n->wpe__posed = true;
        if (jumped && r->physics) (void)wpe_physics_teleport(r->physics, e);
    }
}

/* ---- Sending --------------------------------------------------------------------------------- */

typedef struct wpe__StateWriter {
    WPE_Replication* r;
    WPE_Session* session;
    WPE_PlayerId to;
    uint8_t body[WPE__STATE_BUFFER];
    uint8_t entries[WPE__STATE_BUFFER];
    WPE_BitWriter w;
    uint32_t count;
} wpe__StateWriter;

static void wpe__flush_state(wpe__StateWriter* s) {
    if (s->count == 0 || s->w.error) {
        s->count = 0;
        s->w = wpe_bits_writer(s->entries, sizeof s->entries);
        return;
    }
    WPE_BitWriter head = wpe_bits_writer(s->body, sizeof s->body);
    wpe_bits_write_varint(&head, s->r->epoch);
    wpe_bits_write(&head, (uint32_t)(uint64_t)s->r->clock_ms, 32);
    wpe_bits_write_varint(&head, s->count);
    /* The entries follow bit for bit (they need not start on a byte). */
    WPE_BitReader entries = wpe_bits_reader(s->entries, wpe_bits_writer_size(&s->w));
    for (size_t left = s->w.bits; left > 0;) {
        const unsigned chunk = left > 32 ? 32 : (unsigned)left;
        wpe_bits_write(&head, wpe_bits_read(&entries, chunk), chunk);
        left -= chunk;
    }
    if (!head.error)
        (void)wpe_session_send(s->session, s->to, WPE_NET_CHANNEL_UNRELIABLE, WPE_REPLICATION_KIND_STATE,
                               s->body, wpe_bits_writer_size(&head));
    s->count = 0;
    s->w = wpe_bits_writer(s->entries, sizeof s->entries);
}

static void wpe__write_state(wpe__StateWriter* s, uint32_t id, WPE_Vec3 position, WPE_Quat rotation,
                             WPE_Vec3 velocity) {
    wpe_bits_write_varint(&s->w, id);
    for (int k = 0; k < 3; ++k)
        wpe_bits_write_float(&s->w, position.v[k]);
    wpe_bits_write_quat(&s->w, rotation, WPE__ROTATION_BITS);
    wpe_bits_write_vec3(&s->w, velocity, -WPE__MAX_VELOCITY, WPE__MAX_VELOCITY, WPE__VELOCITY_BITS);
    if (++s->count == WPE__ENTRIES_PER_MESSAGE) wpe__flush_state(s);
}

/* Sends one recipient the states it should hear: the host sends everything not owned by that
 * player (its own entities as they are, relayed ones as last received); a player sends its own. */
static void wpe__send_to(WPE_Replication* r, WPE_Session* session, WPE_PlayerId to, wpe__StateWriter* s) {
    s->to = to;
    s->count = 0;
    s->w = wpe_bits_writer(s->entries, sizeof s->entries);
    const bool host = wpe_session_is_host(session);
    const WPE_PlayerId local = wpe_session_local_player(session);
    for (uint32_t id = 1; id <= r->id_count; ++id) {
        WPE_Entity e = WPE_ENTITY_NULL;
        const WPE_NetIdentity* n = wpe__identity(r, id, &e);
        if (!n || (host ? n->owner == to : n->owner != local)) continue;
        if (n->wpe__remote) {
            if (n->wpe__sample_count == 0) continue; /* Nothing to relay yet. */
            const WPE_NetSample* last = &n->wpe__samples[n->wpe__sample_count - 1];
            wpe__write_state(s, id, last->position, last->rotation, last->velocity);
            continue;
        }
        const WPE_Transform* t = wpe_ecs_get_component(r->ecs, e, WPE_COMP_TRANSFORM);
        if (!t) continue;
        WPE_Vec3 velocity = wpe_vec3(0, 0, 0);
        if (r->physics && wpe_physics_get_velocity(r->physics, e, &velocity) != WPE_RESULT_OK)
            velocity = wpe_vec3(0, 0, 0);
        wpe__write_state(s, id, t->position, t->rotation, velocity);
    }
    wpe__flush_state(s);
}

void wpe_replication_send(WPE_Replication* replication, WPE_Session* session) {
    if (!replication || !wpe__running(replication, session)) return;
    WPE_Replication* r = replication;
    if (r->clock_ms + 1e-6 < r->next_send_ms) return;
    r->next_send_ms += r->interval_ms;
    if (r->next_send_ms <= r->clock_ms) r->next_send_ms = r->clock_ms + r->interval_ms; /* Fell behind. */
    if (r->id_count == 0) return;
    wpe__StateWriter writer = {.r = r, .session = session}; /* About 2.8 KB. */
    if (!wpe_session_is_host(session)) {
        wpe__send_to(r, session, WPE_SESSION_HOST, &writer);
        return;
    }
    WPE_SessionPlayer info;
    for (WPE_PlayerId p = 1; p < wpe_session_max_players(session); ++p)
        if (wpe_session_player(session, p, &info)) wpe__send_to(r, session, p, &writer);
}
