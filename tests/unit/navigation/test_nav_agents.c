#include "wpe_test.h"

#include <math.h>
#include <string.h>

#include "navigation/nav_agents.h"
#include "navigation/nav_path.h"
#include "navigation/navigation.h"
#include "physics/physics.h"
#include "scene/scene.h"

#define DT (1.0f / 60.0f)

/* ---- A world with colliders, a grid built from them and a navigation system ------------------------- */

typedef struct World {
    WPE_Ecs* ecs;
    WPE_Physics* physics;
    WPE_NavSystem* nav;
    WPE_NavGrid* grid;
    bool no_grid; /* The provider has none to give. */
    uint32_t provided;
} World;

static const WPE_NavGrid* provide(void* user_data) {
    World* w = user_data;
    if (w->no_grid) return NULL;
    if (!w->grid) WPE_TEST_ASSERT_OK(wpe_nav_grid_build_from_world(w->ecs, NULL, NULL, &w->grid));
    ++w->provided;
    return w->grid;
}

static WPE_Entity spawn_at(World* w, WPE_Vec3 position) {
    const WPE_Entity e = wpe_ecs_spawn(w->ecs);
    WPE_Transform t = wpe_transform_identity();
    t.position = position;
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(w->ecs, e, WPE_COMP_TRANSFORM, &t, NULL));
    return e;
}

static void add_wall(World* w, WPE_Vec3 min, WPE_Vec3 max) {
    const WPE_Entity e = spawn_at(w, wpe_vec3_scale(wpe_vec3_add(min, max), 0.5f));
    WPE_Collider c = wpe_collider_default(WPE_COLLIDER_BOX);
    c.size = wpe_vec3_sub(max, min);
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(w->ecs, e, WPE_COMP_PHYSICS_COLLIDER, &c, NULL));
}

/* A 10 x 10 floor (top at y = 0), optionally split at x = 0 by a wall with a 2 m doorway at z = 3..5. */
static World make_world(bool wall) {
    World w = {0};
    WPE_TEST_ASSERT_OK(wpe_ecs_create(&w.ecs));
    WPE_TEST_ASSERT_OK(wpe_scene_register_components(w.ecs));
    WPE_TEST_ASSERT_OK(
        wpe_physics_create(&(WPE_PhysicsDesc){.ecs = w.ecs, .gravity = wpe_vec3(0, -9.81f, 0)}, &w.physics));
    add_wall(&w, wpe_vec3(-5, -1, -5), wpe_vec3(5, 0, 5));
    if (wall) add_wall(&w, wpe_vec3(-0.25f, 0, -5), wpe_vec3(0.25f, 2, 3));
    return w;
}

static void start_nav(World* w) {
    WPE_TEST_ASSERT_OK(wpe_nav_system_create(
        &(WPE_NavSystemDesc){.ecs = w->ecs, .physics = w->physics, .grid = provide, .grid_user_data = w},
        &w->nav));
}

static void destroy_world(World* w) {
    wpe_nav_system_destroy(w->nav);
    wpe_nav_grid_destroy(w->grid);
    wpe_physics_destroy(w->physics);
    wpe_ecs_destroy(w->ecs);
}

static WPE_Entity spawn_agent(World* w, WPE_Vec3 at, bool character) {
    const WPE_Entity e = spawn_at(w, at);
    const WPE_NavAgent agent = wpe_nav_agent_default();
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(w->ecs, e, WPE_COMP_NAV_AGENT, &agent, NULL));
    if (character) {
        const WPE_CharacterController c = wpe_character_default();
        WPE_TEST_ASSERT_OK(wpe_ecs_add_component(w->ecs, e, WPE_COMP_CHARACTER, &c, NULL));
    }
    return e;
}

static WPE_Vec3 position_of(World* w, WPE_Entity e) {
    WPE_Mat4 m;
    WPE_TEST_ASSERT(wpe_scene_compute_world_matrix(w->ecs, e, &m));
    return wpe_vec3(m.m[3][0], m.m[3][1], m.m[3][2]);
}

static const WPE_NavAgent* agent_of(World* w, WPE_Entity e) {
    const WPE_NavAgent* a = wpe_ecs_get_component(w->ecs, e, WPE_COMP_NAV_AGENT);
    WPE_TEST_ASSERT(a != NULL);
    return a;
}

static float distance_xz(WPE_Vec3 a, WPE_Vec3 b) {
    return sqrtf((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z));
}

typedef struct Events {
    uint32_t arrived, failed;
} Events;

/* One fixed step: navigation, then physics (as the engine runs them); counts the events for `e`. */
static void step(World* w, WPE_Entity e, Events* events) {
    WPE_TEST_ASSERT_OK(wpe_nav_system_update(w->nav, DT));
    uint32_t count = 0;
    const WPE_NavEvent* list = wpe_nav_system_events(w->nav, &count);
    for (uint32_t i = 0; i < count && events; ++i) {
        if (list[i].entity != e) continue;
        events->arrived += list[i].type == WPE_NAV_EVENT_ARRIVED;
        events->failed += list[i].type == WPE_NAV_EVENT_FAILED;
    }
    WPE_TEST_ASSERT_OK(wpe_physics_step(w->physics, DT));
}

/* ---- Blocked cells ---------------------------------------------------------------------------------- */

static void test_nav_blockers_close_cells_to_paths(void) {
    World w = make_world(false);
    const WPE_NavGrid* grid = provide(&w);
    WPE_NavBlockers* b = NULL;
    WPE_TEST_ASSERT_OK(wpe_nav_blockers_create(grid, &b));
    /* A 2 x 2 box in the middle, turned 45 degrees: its cells and an agent radius around them close. */
    const WPE_Mat4 turned = wpe_mat4_from_trs(
        wpe_vec3(0, 1, 0), wpe_quat_from_axis_angle(wpe_vec3(0, 1, 0), 0.785398f), wpe_vec3(1, 1, 1));
    const uint32_t added = wpe_nav_blockers_add_box(b, &turned, wpe_vec3(0, 0, 0), wpe_vec3(1, 1, 1));
    WPE_TEST_ASSERT(added > 0 && wpe_nav_blockers_count(b) == added);
    WPE_TEST_ASSERT(wpe_nav_blockers_blocked(b, wpe_nav_grid_find_cell(grid, wpe_vec3(0, 0, 0), 0.1f)));
    WPE_TEST_ASSERT(
        wpe_nav_blockers_blocked(b, wpe_nav_grid_find_cell(grid, wpe_vec3(1.6f, 0, 0), 0.1f))); /* Tip. */
    WPE_TEST_ASSERT(
        !wpe_nav_blockers_blocked(b, wpe_nav_grid_find_cell(grid, wpe_vec3(1.2f, 0, 1.2f), 0.1f)));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_nav_blockers_add_box(b, &turned, wpe_vec3(0, 0, 0), wpe_vec3(1, 1, 1)));

    /* Paths and lines go around; the cell a path starts in may be closed. */
    WPE_NavQuery* q = NULL;
    WPE_TEST_ASSERT_OK(wpe_nav_query_create(grid, &q));
    WPE_NavPathOptions o = wpe_nav_path_options_default();
    WPE_NavPath path;
    WPE_TEST_ASSERT_OK(wpe_nav_find_path(q, wpe_vec3(-3, 0, 0), wpe_vec3(3, 0, 0), &o, &path));
    WPE_TEST_ASSERT_EQ_UINT(2, path.count);
    o.blockers = b;
    WPE_TEST_ASSERT_OK(wpe_nav_find_path(q, wpe_vec3(-3, 0, 0), wpe_vec3(3, 0, 0), &o, &path));
    WPE_TEST_ASSERT(path.count >= 3 && path.length > 6.2f);
    for (uint32_t i = 1; i < path.count; ++i)
        WPE_TEST_ASSERT_MSG(wpe_nav_grid_line_clear(grid, b, path.points[i - 1], path.points[i]),
                            "segment %u: %.3f %.3f -> %.3f %.3f", i, (double)path.points[i - 1].x,
                            (double)path.points[i - 1].z, (double)path.points[i].x, (double)path.points[i].z);
    WPE_TEST_ASSERT(!wpe_nav_grid_line_clear(grid, b, wpe_vec3(-3, 0, 0), wpe_vec3(3, 0, 0)));
    WPE_TEST_ASSERT(wpe_nav_grid_line_walkable(grid, wpe_vec3(-3, 0, 0), wpe_vec3(3, 0, 0)));
    WPE_TEST_ASSERT(
        wpe_nav_grid_line_clear(grid, b, wpe_vec3(0.3f, 0, 0.2f), wpe_vec3(3, 0, 3))); /* Out of it. */
    WPE_TEST_ASSERT_OK(wpe_nav_find_path(q, wpe_vec3(0, 0, 0), wpe_vec3(3, 0, 3), &o, &path));
    /* A goal inside: as near as possible. */
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND,
                           wpe_nav_find_path(q, wpe_vec3(-3, 0, 0), wpe_vec3(0, 0, 0), &o, &path));
    o.allow_partial = true;
    WPE_TEST_ASSERT_OK(wpe_nav_find_path(q, wpe_vec3(-3, 0, 0), wpe_vec3(0, 0, 0), &o, &path));
    WPE_TEST_ASSERT(path.partial);

    /* Flow fields go around too. */
    WPE_NavFlowField* field = NULL;
    WPE_TEST_ASSERT_OK(wpe_nav_flow_create(grid, &field));
    const WPE_Vec3 goal = wpe_vec3(3, 0, 0);
    WPE_TEST_ASSERT_OK(wpe_nav_flow_build(field, &goal, 1, 0.0f, b));
    const uint32_t start = wpe_nav_grid_find_cell(grid, wpe_vec3(-3, 0, 0), 0.1f);
    WPE_TEST_ASSERT(wpe_nav_flow_distance(field, start) > 6.2f);
    /* From inside the box the flow leads out (an agent caught there isn't stuck). */
    const uint32_t inside = wpe_nav_grid_find_cell(grid, wpe_vec3(0, 0, 0), 0.1f);
    WPE_TEST_ASSERT(isfinite(wpe_nav_flow_distance(field, inside)));
    uint32_t hops = 0;
    for (uint32_t c = inside; wpe_nav_flow_next(field, c) != c && hops < 1000; ++hops) {
        const uint32_t next = wpe_nav_flow_next(field, c);
        WPE_TEST_ASSERT(wpe_nav_blockers_blocked(b, c) ||
                        !wpe_nav_blockers_blocked(b, next)); /* Never back in. */
        c = next;
    }
    wpe_nav_flow_destroy(field);

    /* A box well above the agents' heads closes nothing; clearing opens everything. */
    wpe_nav_blockers_clear(b);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_nav_blockers_count(b));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_nav_blockers_add_box(b, NULL, wpe_vec3(0, 3.5f, 0), wpe_vec3(1, 0.5f, 1)));
    WPE_TEST_ASSERT(wpe_nav_blockers_add_box(b, NULL, wpe_vec3(0, 1.5f, 0), wpe_vec3(1, 0.5f, 1)) > 0);
    /* Another grid's blockers are refused. */
    WPE_NavGeometry* other_geometry = NULL;
    WPE_TEST_ASSERT_OK(wpe_nav_geometry_create(&other_geometry));
    WPE_TEST_ASSERT_OK(wpe_nav_geometry_add_box(other_geometry, wpe_vec3(0, -1, 0), wpe_vec3(2, 1, 2), NULL));
    WPE_NavGrid* other = NULL;
    WPE_TEST_ASSERT_OK(wpe_nav_grid_build(other_geometry, NULL, &other));
    WPE_NavQuery* other_query = NULL;
    WPE_TEST_ASSERT_OK(wpe_nav_query_create(other, &other_query));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG,
                           wpe_nav_find_path(other_query, wpe_vec3(0, 0, 0), wpe_vec3(1, 0, 1), &o, &path));
    wpe_nav_query_destroy(other_query);
    wpe_nav_grid_destroy(other);
    wpe_nav_geometry_destroy(other_geometry);
    WPE_TEST_ASSERT(!wpe_nav_blockers_blocked(NULL, 0));
    WPE_TEST_ASSERT(!wpe_nav_blockers_blocked(b, 1u << 30));
    wpe_nav_blockers_destroy(NULL);
    wpe_nav_blockers_destroy(b);
    wpe_nav_query_destroy(q);
    destroy_world(&w);
}

/* ---- Agents -------------------------------------------------------------------------------------------- */

static void test_nav_agent_walks_through_the_doorway(void) {
    World w = make_world(true);
    start_nav(&w);
    const WPE_Entity agent = spawn_agent(&w, wpe_vec3(-3, 0, -3), false);
    WPE_TEST_ASSERT_EQ_INT(WPE_NAV_AGENT_IDLE, agent_of(&w, agent)->state);
    const WPE_Vec3 goal = wpe_vec3(3, 0, -3);
    WPE_TEST_ASSERT_OK(wpe_nav_agent_set_destination(w.nav, agent, goal));
    Events events = {0};
    bool through_door = false;
    int steps = 0;
    for (; steps < 600 && agent_of(&w, agent)->state == WPE_NAV_AGENT_MOVING; ++steps) {
        step(&w, agent, &events);
        const WPE_Vec3 p = position_of(&w, agent);
        WPE_TEST_ASSERT(wpe_nav_grid_find_cell(w.grid, p, 0.2f) !=
                        WPE_NAV_CELL_NONE); /* Always on the grid. */
        through_door |= fabsf(p.x) < 0.3f && p.z > 3.0f;
        if (steps == 30) {
            uint32_t count = 0;
            WPE_TEST_ASSERT(wpe_nav_agent_path(w.nav, agent, &count) != NULL && count >= 2);
            WPE_TEST_ASSERT(agent_of(&w, agent)->remaining > 10.0f);
            WPE_TEST_ASSERT_NEAR(3.5f, wpe_vec3_length(agent_of(&w, agent)->velocity),
                                 0.05); /* Full speed. */
        }
    }
    WPE_TEST_ASSERT(through_door);
    WPE_TEST_ASSERT_EQ_INT(WPE_NAV_AGENT_ARRIVED, agent_of(&w, agent)->state);
    WPE_TEST_ASSERT_EQ_UINT(1, events.arrived);
    WPE_TEST_ASSERT_EQ_UINT(0, events.failed);
    WPE_TEST_ASSERT(distance_xz(position_of(&w, agent), goal) <= 0.21f);
    WPE_TEST_ASSERT(steps > 60 * 4 && steps < 60 * 7); /* ~16 m at 3.5 m/s, plus the slow end. */
    /* It turned to face the way it walked (forward is -Z): it came toward -Z at the end. */
    WPE_Transform* t = wpe_ecs_get_component(w.ecs, agent, WPE_COMP_TRANSFORM);
    const WPE_Vec3 forward = wpe_quat_rotate_vec3(t->rotation, wpe_vec3(0, 0, -1));
    WPE_TEST_ASSERT(forward.z < -0.5f);
    /* It stays arrived and stops. */
    for (int i = 0; i < 30; ++i)
        step(&w, agent, &events);
    WPE_TEST_ASSERT_EQ_UINT(1, events.arrived);
    WPE_TEST_ASSERT(wpe_vec3_length(agent_of(&w, agent)->velocity) < 0.01f);
    destroy_world(&w);
}

static void test_nav_character_agent_walks_with_physics(void) {
    World w = make_world(true);
    start_nav(&w);
    const WPE_Entity agent = spawn_agent(&w, wpe_vec3(-3, 0.2f, -3), true);
    WPE_TEST_ASSERT_OK(wpe_physics_create_bodies(w.physics));
    const WPE_Vec3 goal = wpe_vec3(3, 0, -2);
    WPE_TEST_ASSERT_OK(wpe_nav_agent_set_destination(w.nav, agent, goal));
    Events events = {0};
    for (int i = 0; i < 600 && agent_of(&w, agent)->state == WPE_NAV_AGENT_MOVING; ++i)
        step(&w, agent, &events);
    WPE_TEST_ASSERT_EQ_INT(WPE_NAV_AGENT_ARRIVED, agent_of(&w, agent)->state);
    WPE_TEST_ASSERT_EQ_UINT(1, events.arrived);
    const WPE_Vec3 p = position_of(&w, agent);
    WPE_TEST_ASSERT(distance_xz(p, goal) <= 0.25f);
    WPE_TEST_ASSERT_NEAR(0.0f, p.y, 0.05); /* Fell onto the floor and walked on it. */
    const WPE_CharacterController* c = wpe_ecs_get_component(w.ecs, agent, WPE_COMP_CHARACTER);
    WPE_TEST_ASSERT(c->grounded);
    destroy_world(&w);
}

static void test_nav_agent_unreachable_destinations(void) {
    World w = make_world(false);
    add_wall(&w, wpe_vec3(2, 0, -1), wpe_vec3(4, 1.5f, 1)); /* A block too high to step onto. */
    start_nav(&w);
    const WPE_Entity agent = spawn_agent(&w, wpe_vec3(-3, 0, 0), false);
    /* On top of the block: walks as near as it can, then fails. */
    WPE_TEST_ASSERT_OK(wpe_nav_agent_set_destination(w.nav, agent, wpe_vec3(3, 1.5f, 0)));
    Events events = {0};
    for (int i = 0; i < 600 && agent_of(&w, agent)->state == WPE_NAV_AGENT_MOVING; ++i)
        step(&w, agent, &events);
    WPE_TEST_ASSERT_EQ_INT(WPE_NAV_AGENT_UNREACHABLE, agent_of(&w, agent)->state);
    WPE_TEST_ASSERT_EQ_UINT(1, events.failed);
    WPE_TEST_ASSERT_EQ_UINT(0, events.arrived);
    WPE_TEST_ASSERT(position_of(&w, agent).x > 1.0f); /* Next to the block. */
    /* Far off the grid: fails at once and doesn't move. */
    WPE_TEST_ASSERT_OK(wpe_nav_agent_set_destination(w.nav, agent, wpe_vec3(50, 0, 0)));
    const WPE_Vec3 before = position_of(&w, agent);
    step(&w, agent, &events);
    WPE_TEST_ASSERT_EQ_INT(WPE_NAV_AGENT_UNREACHABLE, agent_of(&w, agent)->state);
    WPE_TEST_ASSERT_EQ_UINT(2, events.failed);
    for (int i = 0; i < 30; ++i)
        step(&w, agent, &events);
    WPE_TEST_ASSERT(distance_xz(before, position_of(&w, agent)) < 0.1f);
    destroy_world(&w);
}

static void test_nav_agent_follows_an_entity(void) {
    World w = make_world(false);
    start_nav(&w);
    const WPE_Entity agent = spawn_agent(&w, wpe_vec3(-4, 0, -4), false);
    const WPE_Entity target = spawn_at(&w, wpe_vec3(3, 0, 3));
    WPE_TEST_ASSERT_OK(wpe_nav_agent_follow(w.nav, agent, target));
    WPE_TEST_ASSERT_EQ_UINT(target, agent_of(&w, agent)->follow);
    Events events = {0};
    for (int i = 0; i < 240; ++i)
        step(&w, agent, &events);
    WPE_TEST_ASSERT(distance_xz(position_of(&w, agent), wpe_vec3(3, 0, 3)) < 0.3f);
    WPE_TEST_ASSERT_EQ_UINT(1, events.arrived);
    /* The target walks away; the agent comes after it, arriving once more when it catches up. */
    WPE_Transform* t = wpe_ecs_get_component(w.ecs, target, WPE_COMP_TRANSFORM);
    for (int i = 0; i < 360; ++i) {
        if (i < 120) t->position.x -= 2.0f * DT;
        step(&w, agent, &events);
        t = wpe_ecs_get_component(w.ecs, target, WPE_COMP_TRANSFORM);
    }
    WPE_TEST_ASSERT(distance_xz(position_of(&w, agent), t->position) < 0.3f);
    WPE_TEST_ASSERT_EQ_UINT(2, events.arrived);
    /* The target goes: the agent stops. */
    wpe_ecs_despawn(w.ecs, target);
    step(&w, agent, &events);
    WPE_TEST_ASSERT_EQ_INT(WPE_NAV_AGENT_IDLE, agent_of(&w, agent)->state);
    WPE_TEST_ASSERT_EQ_UINT(WPE_ENTITY_NULL, agent_of(&w, agent)->follow);
    destroy_world(&w);
}

static void test_nav_agent_replans_around_new_obstacles(void) {
    World w = make_world(false);
    start_nav(&w);
    const WPE_Entity agent = spawn_agent(&w, wpe_vec3(-4, 0, 0), false);
    const WPE_Vec3 goal = wpe_vec3(4, 0, 0);
    WPE_TEST_ASSERT_OK(wpe_nav_agent_set_destination(w.nav, agent, goal));
    Events events = {0};
    for (int i = 0; i < 20; ++i)
        step(&w, agent, &events);
    uint32_t count = 0;
    WPE_TEST_ASSERT(wpe_nav_agent_path(w.nav, agent, &count) && count == 1); /* Straight across. */
    /* A crate drops into its way. */
    const WPE_Entity crate = spawn_at(&w, wpe_vec3(1, 0.75f, 0));
    WPE_NavObstacle o = wpe_nav_obstacle_default();
    o.size = wpe_vec3(1.5f, 1.5f, 3.0f);
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(w.ecs, crate, WPE_COMP_NAV_OBSTACLE, &o, NULL));
    const WPE_NavBlockers* blockers = wpe_nav_system_blockers(w.nav);
    for (int i = 0; i < 600 && agent_of(&w, agent)->state == WPE_NAV_AGENT_MOVING; ++i) {
        step(&w, agent, &events);
        const uint32_t cell = wpe_nav_grid_find_cell(w.grid, position_of(&w, agent), 0.2f);
        WPE_TEST_ASSERT_MSG(!wpe_nav_blockers_blocked(blockers, cell), "walked into the crate at step %d", i);
    }
    WPE_TEST_ASSERT(wpe_nav_blockers_count(blockers) > 0);
    WPE_TEST_ASSERT_EQ_UINT(1, events.arrived);
    WPE_TEST_ASSERT(distance_xz(position_of(&w, agent), goal) <= 0.21f);
    /* Moving the crate carves it again where it is now; removing it opens everything. */
    WPE_Transform* t = wpe_ecs_get_component(w.ecs, crate, WPE_COMP_TRANSFORM);
    t->position.z = 3.0f;
    step(&w, agent, NULL);
    WPE_TEST_ASSERT(
        wpe_nav_blockers_blocked(blockers, wpe_nav_grid_find_cell(w.grid, wpe_vec3(1, 0, 3), 0.2f)));
    WPE_TEST_ASSERT(
        !wpe_nav_blockers_blocked(blockers, wpe_nav_grid_find_cell(w.grid, wpe_vec3(1, 0, 0), 0.2f)));
    WPE_TEST_ASSERT_OK(wpe_ecs_remove_component(w.ecs, crate, WPE_COMP_NAV_OBSTACLE));
    step(&w, agent, NULL);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_nav_blockers_count(blockers));
    destroy_world(&w);
}

static void test_nav_agents_pass_each_other(void) {
    World w = make_world(false);
    start_nav(&w);
    const WPE_Entity a = spawn_agent(&w, wpe_vec3(-4, 0, 0), false);
    const WPE_Entity b = spawn_agent(&w, wpe_vec3(4, 0, 0), false);
    WPE_TEST_ASSERT_OK(wpe_nav_agent_set_destination(w.nav, a, wpe_vec3(4, 0, 0.0f)));
    WPE_TEST_ASSERT_OK(wpe_nav_agent_set_destination(w.nav, b, wpe_vec3(-4, 0, 0.0f)));
    float closest = INFINITY;
    for (int i = 0; i < 600; ++i) {
        step(&w, a, NULL);
        closest = fminf(closest, distance_xz(position_of(&w, a), position_of(&w, b)));
        if (agent_of(&w, a)->state != WPE_NAV_AGENT_MOVING && agent_of(&w, b)->state != WPE_NAV_AGENT_MOVING)
            break;
    }
    WPE_TEST_ASSERT_EQ_INT(WPE_NAV_AGENT_ARRIVED, agent_of(&w, a)->state);
    WPE_TEST_ASSERT_EQ_INT(WPE_NAV_AGENT_ARRIVED, agent_of(&w, b)->state);
    WPE_TEST_ASSERT_MSG(closest > 0.5f, "they came %.2f m close (each is 0.4 m around)", (double)closest);
    destroy_world(&w);
}

static void test_nav_system_validation_and_grid_changes(void) {
    World w = make_world(false);
    WPE_NavSystem* nav = NULL;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NULL_ARG, wpe_nav_system_create(NULL, &nav));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NULL_ARG,
                           wpe_nav_system_create(&(WPE_NavSystemDesc){.ecs = w.ecs}, &nav));
    start_nav(&w);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_ALREADY_EXISTS,
                           wpe_nav_system_create(&(WPE_NavSystemDesc){.ecs = w.ecs, .grid = provide}, &nav));
    /* Without agents nothing asks for a grid. */
    WPE_TEST_ASSERT_OK(wpe_nav_system_update(w.nav, DT));
    WPE_TEST_ASSERT_EQ_UINT(0, w.provided);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_nav_system_update(w.nav, 0.0f));

    const WPE_Entity plain = spawn_at(&w, wpe_vec3(0, 0, 0));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND,
                           wpe_nav_agent_set_destination(w.nav, plain, wpe_vec3(1, 0, 1)));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND, wpe_nav_agent_stop(w.nav, plain));
    /* A copied agent (garbage in its private fields) starts clean. */
    WPE_NavAgent copy = wpe_nav_agent_default();
    copy.state = WPE_NAV_AGENT_MOVING;
    copy.wpe__count = 99;
    copy.wpe__path = (WPE_Vec3*)&copy; /* Never freed: the add hook drops it. */
    const WPE_Entity agent = spawn_at(&w, wpe_vec3(-3, 0, 0));
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(w.ecs, agent, WPE_COMP_NAV_AGENT, &copy, NULL));
    WPE_TEST_ASSERT_EQ_INT(WPE_NAV_AGENT_IDLE, agent_of(&w, agent)->state);
    WPE_TEST_ASSERT(agent_of(&w, agent)->wpe__path == NULL);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG,
                           wpe_nav_agent_set_destination(w.nav, agent, wpe_vec3(NAN, 0, 0)));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_nav_agent_follow(w.nav, agent, agent));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_nav_agent_follow(w.nav, agent, 0x7FFFFu));

    /* No grid yet: the agent waits. */
    w.no_grid = true;
    WPE_TEST_ASSERT_OK(wpe_nav_agent_set_destination(w.nav, agent, wpe_vec3(3, 0, 0)));
    for (int i = 0; i < 10; ++i)
        step(&w, agent, NULL);
    WPE_TEST_ASSERT_NEAR(-3.0f, position_of(&w, agent).x, 1e-5);
    WPE_TEST_ASSERT(wpe_nav_system_query(w.nav) == NULL);
    w.no_grid = false;
    for (int i = 0; i < 30; ++i)
        step(&w, agent, NULL);
    WPE_TEST_ASSERT(position_of(&w, agent).x > -2.8f);
    WPE_TEST_ASSERT(wpe_nav_system_grid(w.nav) == w.grid && wpe_nav_system_query(w.nav) != NULL);
    /* The grid is rebuilt (a level change): the agent plans on the new one and goes on. */
    wpe_nav_system_grid_changed(w.nav);
    wpe_nav_grid_destroy(w.grid);
    w.grid = NULL;
    Events events = {0};
    for (int i = 0; i < 300; ++i)
        step(&w, agent, &events);
    WPE_TEST_ASSERT_EQ_UINT(1, events.arrived);
    /* Stop: idle where it is. */
    WPE_TEST_ASSERT_OK(wpe_nav_agent_set_destination(w.nav, agent, wpe_vec3(-3, 0, 0)));
    step(&w, agent, NULL);
    WPE_TEST_ASSERT_OK(wpe_nav_agent_stop(w.nav, agent));
    WPE_TEST_ASSERT_EQ_INT(WPE_NAV_AGENT_IDLE, agent_of(&w, agent)->state);
    uint32_t count = 1;
    WPE_TEST_ASSERT(wpe_nav_agent_path(w.nav, agent, &count) == NULL && count == 0);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NULL_ARG, wpe_nav_system_update(NULL, DT));
    wpe_nav_system_destroy(NULL);
    destroy_world(&w);
}

typedef struct Lines {
    uint32_t path, blocked;
} Lines;

static void count_line(WPE_Vec3 a, WPE_Vec3 b, uint32_t color, void* user_data) {
    (void)a;
    (void)b;
    Lines* lines = user_data;
    lines->path += color == 0xFFD040u;
    lines->blocked += color == 0xF04040u;
}

static void test_nav_system_debug_draw(void) {
    World w = make_world(true);
    start_nav(&w);
    const WPE_Entity agent = spawn_agent(&w, wpe_vec3(-3, 0, -3), false);
    const WPE_Entity crate = spawn_at(&w, wpe_vec3(-3, 0.5f, 3));
    const WPE_NavObstacle o = wpe_nav_obstacle_default();
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(w.ecs, crate, WPE_COMP_NAV_OBSTACLE, &o, NULL));
    WPE_TEST_ASSERT_OK(wpe_nav_agent_set_destination(w.nav, agent, wpe_vec3(3, 0, -3)));
    step(&w, agent, NULL);
    Lines lines = {0};
    wpe_nav_system_debug_draw(w.nav, count_line, &lines);
    WPE_TEST_ASSERT(lines.path >= 2);
    WPE_TEST_ASSERT_EQ_UINT(4u * wpe_nav_blockers_count(wpe_nav_system_blockers(w.nav)), lines.blocked);
    destroy_world(&w);
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_nav_blockers_close_cells_to_paths),
              WPE_TEST_CASE(test_nav_agent_walks_through_the_doorway),
              WPE_TEST_CASE(test_nav_character_agent_walks_with_physics),
              WPE_TEST_CASE(test_nav_agent_unreachable_destinations),
              WPE_TEST_CASE(test_nav_agent_follows_an_entity),
              WPE_TEST_CASE(test_nav_agent_replans_around_new_obstacles),
              WPE_TEST_CASE(test_nav_agents_pass_each_other),
              WPE_TEST_CASE(test_nav_system_validation_and_grid_changes),
              WPE_TEST_CASE(test_nav_system_debug_draw))
