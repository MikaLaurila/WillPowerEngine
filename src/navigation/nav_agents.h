/**
 * @file nav_agents.h
 * @brief Navigation agents (entities that walk to a destination along nav grid paths) and moving
 *        obstacles that close cells to them, as ECS components run by a WPE_NavSystem.
 *
 * Components registered by wpe_nav_system_create():
 * - WPE_COMP_NAV_AGENT (WPE_NavAgent): walks the entity to a destination, or after another entity.
 *   With a character controller (physics.h) it moves through wpe_character_move() (gravity
 *   included, so it falls, steps and slides like a player); without one it moves its transform,
 *   held on the grid's floor.
 * - WPE_COMP_NAV_OBSTACLE (WPE_NavObstacle): a box (a door, a cart, a crate pushed around) whose
 *   cells paths avoid (see wpe_nav_blockers_add_box()), refreshed whenever it moves.
 *
 * Each wpe_nav_system_update() (the engine runs it in every fixed step, before the physics step):
 * 1. Obstacles that moved since the last update are carved again; agents whose remaining path now
 *    runs through a closed cell look for a new one.
 * 2. Each agent plans when it was given a destination, or (following an entity) every
 *    repath_interval. A destination it can't reach is walked to as near as possible.
 * 3. It steers along the path: toward the next corner, slowing over the last slow_distance, sidestepping
 *    other agents (when avoid_others), and accelerating toward that velocity.
 * 4. It moves, and turns to face the way it walks.
 * 5. Reaching the end gives an event: ARRIVED, or FAILED when the destination was out of reach.
 *
 * The grid comes from the system's provider on first use (the engine builds the scene's grid lazily);
 * wpe_nav_system_grid_changed() drops everything made for it.
 *
 * NOTE(limitation): avoidance looks at every pair of agents (fine for tens of agents, not hundreds),
 * and an agent pushed off its path by physics walks back to it rather than replanning.
 *
 * - Dependencies: core, math, ecs, scene, physics, navigation.h, nav_path.h
 * - Thread affinity: thread owning the world
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_NAVIGATION_NAV_AGENTS_H
#define WPE_NAVIGATION_NAV_AGENTS_H

#include <stdint.h>

#include "core/result.h"
#include "ecs/ecs.h"
#include "math/wpe_math.h"
#include "navigation/nav_path.h"
#include "navigation/navigation.h"
#include "physics/physics.h"

typedef enum WPE_NavAgentState {
    WPE_NAV_AGENT_IDLE = 0,    /* No destination. */
    WPE_NAV_AGENT_MOVING,      /* On its way. */
    WPE_NAV_AGENT_ARRIVED,     /* Got there (a following agent keeps following). */
    WPE_NAV_AGENT_UNREACHABLE, /* Got as near as it could, or there was no way at all. */
} WPE_NavAgentState;

typedef struct WPE_NavAgent {
    /* Settings (saved in scenes). */
    float speed;           /* Top speed, m/s (default 3.5). */
    float acceleration;    /* How fast it reaches the wanted velocity, m/s² (default 20). */
    float arrive_distance; /* It has arrived this near the destination (default 0.2). */
    float slow_distance;   /* It slows down over the last stretch this long (default 1). */
    float turn_speed; /* It turns to face the way it walks at this many degrees/s (default 720; 0: never). */
    float repath_interval; /* Following an entity: seconds between new paths (default 0.5). */
    bool avoid_others;     /* Sidestep other agents (default true). */

    /* State (not saved; set through the wpe_nav_agent_* functions). */
    WPE_NavAgentState state;
    WPE_Vec3 destination; /* The walkable point it is heading for. */
    WPE_Entity follow;    /* The entity it follows, or WPE_ENTITY_NULL. */
    WPE_Vec3 velocity;    /* Its velocity in the last update (with the fall, for character agents). */
    float remaining;      /* Path distance left (0 when not moving). */

    /* Private (managed by the system; the add hook resets them, so copies share nothing). */
    WPE_Vec3* wpe__path;
    uint32_t wpe__count, wpe__capacity, wpe__next;
    float wpe__repath_timer;
    bool wpe__plan;    /* Plan before moving next update. */
    bool wpe__partial; /* The path ends short of the destination. */
} WPE_NavAgent;

typedef struct WPE_NavObstacle {
    WPE_Vec3 size; /* Full extents of the box in the entity's space (scaled by its world scale; default 1). */
    WPE_Vec3 offset; /* Center of the box in the entity's space. */
    bool carve;      /* Close its cells to paths (default true); otherwise agents only sidestep it. */

    /* Private. */
    WPE_Mat4 wpe__carved; /* World matrix it was carved with. */
    bool wpe__placed;     /* Carved at least once. */
} WPE_NavObstacle;

typedef enum WPE_NavEventType {
    WPE_NAV_EVENT_ARRIVED = 0, /* The agent reached its destination. */
    WPE_NAV_EVENT_FAILED,      /* It got as near as it could (or there was no path at all). */
} WPE_NavEventType;

typedef struct WPE_NavEvent {
    WPE_NavEventType type;
    WPE_Entity entity;
} WPE_NavEvent;

/** Gives the system its grid (the engine's lazily built scene grid). NULL: no grid (nothing moves). */
typedef const WPE_NavGrid* (*WPE_NavGridProvider)(void* user_data);

typedef struct WPE_NavSystemDesc {
    WPE_Ecs* ecs;         /* Borrowed; scene components registered. */
    WPE_Physics* physics; /* Optional, borrowed: character agents (and gravity) need it. */
    WPE_NavGridProvider grid;
    void* grid_user_data;
    /* Optional: whether this side moves an entity (multiplayer: only the owner simulates it). NULL: all. */
    bool (*simulates)(WPE_Entity entity, void* user_data);
    void* simulates_user_data;
} WPE_NavSystemDesc;

typedef struct WPE_NavSystem WPE_NavSystem;

/** Line callback for debug drawing (world-space segment, 0xRRGGBB color). */
typedef void (*WPE_NavSystemLineFn)(WPE_Vec3 a, WPE_Vec3 b, uint32_t color, void* user_data);

/** @return An agent with the defaults listed in WPE_NavAgent, idle. @thread_safety Any thread */
WPE_NavAgent wpe_nav_agent_default(void);

/** @return A 1 x 1 x 1 carving obstacle. @thread_safety Any thread */
WPE_NavObstacle wpe_nav_obstacle_default(void);

/**
 * Creates the system and registers WPE_COMP_NAV_AGENT and WPE_COMP_NAV_OBSTACLE. Destroy it before
 * the world.
 *
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG (no ecs or grid provider); WPE_RESULT_ERROR_INVALID_STATE
 *         (scene components not registered); WPE_RESULT_ERROR_ALREADY_EXISTS; WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Thread owning the world
 */
[[nodiscard]] WPE_Result wpe_nav_system_create(const WPE_NavSystemDesc* desc, WPE_NavSystem** out_system);

/** Removes the components, unregisters them and frees everything; NULL is a no-op. @thread_safety Owning
 * thread */
void wpe_nav_system_destroy(WPE_NavSystem* system);

/**
 * The grid in use, asking the provider when there is none yet (and making the path query and blocked
 * cells for it).
 *
 * @return The grid, or NULL when the provider has none.
 *
 * @thread_safety Owning thread
 */
const WPE_NavGrid* wpe_nav_system_grid(WPE_NavSystem* system);

/** Forgets the grid (call before the provider destroys it): the query and blocked cells go, and every
 * moving agent plans again on the next grid. @thread_safety Owning thread */
void wpe_nav_system_grid_changed(WPE_NavSystem* system);

/** The system's path query and blocked cells for its grid (NULL without one; borrowed until the grid
 * changes), for paths that respect obstacles. @thread_safety Owning thread */
WPE_NavQuery* wpe_nav_system_query(WPE_NavSystem* system);
const WPE_NavBlockers* wpe_nav_system_blockers(WPE_NavSystem* system);

/**
 * Runs one update (see the file comment). Without agents it does nothing, not even asking for a grid.
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_INVALID_ARG (dt <= 0) or
 *         WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Owning thread
 */
WPE_Result wpe_nav_system_update(WPE_NavSystem* system, float dt);

/** @return The events of the last update (borrowed until the next one). @thread_safety Owning thread */
const WPE_NavEvent* wpe_nav_system_events(const WPE_NavSystem* system, uint32_t* out_count);

/**
 * Sends an agent to a point (planned at the next update).
 *
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_NOT_FOUND (no agent component);
 *         WPE_RESULT_ERROR_INVALID_ARG (a non-finite point).
 *
 * @thread_safety Owning thread
 */
WPE_Result wpe_nav_agent_set_destination(WPE_NavSystem* system, WPE_Entity entity, WPE_Vec3 destination);

/** Makes an agent follow another entity (it plans again every repath_interval and keeps following after
 * arriving). @return As wpe_nav_agent_set_destination(); WPE_RESULT_ERROR_INVALID_ARG for a dead target or
 * itself. @thread_safety Owning thread */
WPE_Result wpe_nav_agent_follow(WPE_NavSystem* system, WPE_Entity entity, WPE_Entity target);

/** Stops an agent where it is (IDLE; character agents keep falling). @return WPE_RESULT_OK,
 * WPE_RESULT_ERROR_NULL_ARG or WPE_RESULT_ERROR_NOT_FOUND. @thread_safety Owning thread */
WPE_Result wpe_nav_agent_stop(WPE_NavSystem* system, WPE_Entity entity);

/** @return An agent's remaining path (borrowed until the next update or change), *out_count points from
 * the corner it walks to now; NULL when it has none. @thread_safety Owning thread */
const WPE_Vec3* wpe_nav_agent_path(const WPE_NavSystem* system, WPE_Entity entity, uint32_t* out_count);

/** Emits debug lines: each moving agent's way ahead and the blocked cells' outlines. @thread_safety Owning
 * thread */
void wpe_nav_system_debug_draw(WPE_NavSystem* system, WPE_NavSystemLineFn line, void* user_data);

#endif /* WPE_NAVIGATION_NAV_AGENTS_H */
