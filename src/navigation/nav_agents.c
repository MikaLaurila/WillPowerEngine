#define WPE_LOG_MODULE "navigation"
#include "navigation/nav_agents.h"

#include <math.h>
#include <string.h>

#include "core/alloc.h"
#include "core/log.h"
#include "navigation/nav_internal.h"
#include "scene/scene.h"

#define WPE__PI 3.14159265358979f
#define WPE__CORNER_REACHED 0.15f /* A corner counts as passed this near (or within a step). */
#define WPE__AVOID_RANGE 1.0f     /* Agents sidestep others this far beyond touching. */
#define WPE__AVOID_LOOKAHEAD 0.3f /* Seconds of an avoiding move that must stay walkable. */

typedef struct wpe__AgentInfo {
    WPE_Entity entity;
    WPE_Vec3 position;
} wpe__AgentInfo;

struct WPE_NavSystem {
    WPE_Ecs* ecs;
    WPE_Physics* physics;
    WPE_NavGridProvider provider;
    void* provider_user_data;
    bool (*simulates)(WPE_Entity entity, void* user_data);
    void* simulates_user_data;
    const WPE_NavGrid* grid; /* From the provider; NULL until there is one. */
    WPE_NavQuery* query;
    WPE_NavBlockers* blockers;
    bool carve_again; /* Obstacles were added or removed, or the grid is new. */
    WPE_NavEvent* events;
    uint32_t event_count, event_capacity;
    wpe__AgentInfo* infos; /* The agents' positions at the start of the update (for avoidance). */
    uint32_t info_count, info_capacity;
};

/* ---- Components -------------------------------------------------------------------------------------- */

WPE_NavAgent wpe_nav_agent_default(void) {
    return (WPE_NavAgent){.speed = 3.5f,
                          .acceleration = 20.0f,
                          .arrive_distance = 0.2f,
                          .slow_distance = 1.0f,
                          .turn_speed = 720.0f,
                          .repath_interval = 0.5f,
                          .avoid_others = true,
                          .follow = WPE_ENTITY_NULL};
}

WPE_NavObstacle wpe_nav_obstacle_default(void) {
    return (WPE_NavObstacle){.size = wpe_vec3(1.0f, 1.0f, 1.0f), .carve = true};
}

static void wpe__agent_on_add(WPE_Ecs* ecs, WPE_Entity entity, void* component, void* user_data) {
    (void)ecs;
    (void)entity;
    (void)user_data;
    WPE_NavAgent* a = component;
    /* A copy (a prefab, a loaded scene) starts idle with nothing of the original's. */
    a->state = WPE_NAV_AGENT_IDLE;
    a->follow = WPE_ENTITY_NULL;
    a->velocity = wpe_vec3(0, 0, 0);
    a->remaining = 0.0f;
    a->wpe__path = NULL;
    a->wpe__count = a->wpe__capacity = a->wpe__next = 0;
    a->wpe__repath_timer = 0.0f;
    a->wpe__plan = false;
    a->wpe__partial = false;
}

static void wpe__agent_on_remove(WPE_Ecs* ecs, WPE_Entity entity, void* component, void* user_data) {
    (void)ecs;
    (void)entity;
    (void)user_data;
    WPE_NavAgent* a = component;
    wpe_free(a->wpe__path);
    a->wpe__path = NULL;
}

static void wpe__obstacle_on_add(WPE_Ecs* ecs, WPE_Entity entity, void* component, void* user_data) {
    (void)ecs;
    (void)entity;
    ((WPE_NavObstacle*)component)->wpe__placed = false;
    ((WPE_NavSystem*)user_data)->carve_again = true;
}

static void wpe__obstacle_on_remove(WPE_Ecs* ecs, WPE_Entity entity, void* component, void* user_data) {
    (void)ecs;
    (void)entity;
    (void)component;
    ((WPE_NavSystem*)user_data)->carve_again = true;
}

/* ---- System ------------------------------------------------------------------------------------------- */

WPE_Result wpe_nav_system_create(const WPE_NavSystemDesc* desc, WPE_NavSystem** out_system) {
    if (out_system) *out_system = NULL;
    if (!desc || !desc->ecs || !desc->grid || !out_system) return WPE_RESULT_ERROR_NULL_ARG;
    if (!wpe_ecs_is_registered(desc->ecs, WPE_COMP_TRANSFORM)) {
        wpe_log_error("register the scene components before creating navigation");
        return WPE_RESULT_ERROR_INVALID_STATE;
    }
    if (wpe_ecs_is_registered(desc->ecs, WPE_COMP_NAV_AGENT)) {
        wpe_log_error("the world already has a navigation system");
        return WPE_RESULT_ERROR_ALREADY_EXISTS;
    }
    WPE_NavSystem* s = wpe_alloc_zeroed(1, sizeof *s);
    if (!s) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    *s = (WPE_NavSystem){.ecs = desc->ecs,
                         .physics = desc->physics,
                         .provider = desc->grid,
                         .provider_user_data = desc->grid_user_data,
                         .simulates = desc->simulates,
                         .simulates_user_data = desc->simulates_user_data};
    WPE_Result result = wpe_ecs_register_component(s->ecs, WPE_COMP_NAV_AGENT,
                                                   &(WPE_ComponentDesc){.name = "NavAgent",
                                                                        .size = sizeof(WPE_NavAgent),
                                                                        .alignment = alignof(WPE_NavAgent),
                                                                        .on_add = wpe__agent_on_add,
                                                                        .on_remove = wpe__agent_on_remove,
                                                                        .hook_user_data = s});
    if (result == WPE_RESULT_OK) {
        result = wpe_ecs_register_component(s->ecs, WPE_COMP_NAV_OBSTACLE,
                                            &(WPE_ComponentDesc){.name = "NavObstacle",
                                                                 .size = sizeof(WPE_NavObstacle),
                                                                 .alignment = alignof(WPE_NavObstacle),
                                                                 .on_add = wpe__obstacle_on_add,
                                                                 .on_remove = wpe__obstacle_on_remove,
                                                                 .hook_user_data = s});
        if (result != WPE_RESULT_OK) wpe_ecs_unregister_component(s->ecs, WPE_COMP_NAV_AGENT);
    }
    if (result != WPE_RESULT_OK) {
        wpe_free(s);
        return result;
    }
    *out_system = s;
    return WPE_RESULT_OK;
}

static void wpe__remove_all(WPE_Ecs* ecs, WPE_ComponentType type) {
    WPE_EcsPoolView view;
    while (wpe_ecs_pool_view(ecs, type, &view) == WPE_RESULT_OK && view.count > 0)
        wpe_ecs_remove_component(ecs, view.entities[view.count - 1], type);
}

void wpe_nav_system_destroy(WPE_NavSystem* system) {
    if (!system) return;
    wpe__remove_all(system->ecs, WPE_COMP_NAV_OBSTACLE);
    wpe__remove_all(system->ecs, WPE_COMP_NAV_AGENT);
    wpe_ecs_unregister_component(system->ecs, WPE_COMP_NAV_OBSTACLE);
    wpe_ecs_unregister_component(system->ecs, WPE_COMP_NAV_AGENT);
    wpe_nav_query_destroy(system->query);
    wpe_nav_blockers_destroy(system->blockers);
    wpe_free(system->events);
    wpe_free(system->infos);
    wpe_free(system);
}

const WPE_NavGrid* wpe_nav_system_grid(WPE_NavSystem* system) {
    if (!system) return NULL;
    if (system->grid) return system->grid;
    const WPE_NavGrid* grid = system->provider(system->provider_user_data);
    if (!grid) return NULL;
    if (wpe_nav_query_create(grid, &system->query) != WPE_RESULT_OK ||
        wpe_nav_blockers_create(grid, &system->blockers) != WPE_RESULT_OK) {
        wpe_log_error("out of memory setting up navigation on a grid");
        wpe_nav_query_destroy(system->query);
        system->query = NULL;
        return NULL;
    }
    system->grid = grid;
    system->carve_again = true;
    return grid;
}

void wpe_nav_system_grid_changed(WPE_NavSystem* system) {
    if (!system) return;
    wpe_nav_query_destroy(system->query);
    wpe_nav_blockers_destroy(system->blockers);
    system->query = NULL;
    system->blockers = NULL;
    system->grid = NULL;
    WPE_EcsIter it = wpe_ecs_query(system->ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_NAV_AGENT)});
    while (wpe_ecs_iter_next(&it)) {
        WPE_NavAgent* a = wpe_ecs_iter_get(&it, WPE_COMP_NAV_AGENT);
        a->wpe__count = 0;
        if (a->state == WPE_NAV_AGENT_MOVING || a->follow != WPE_ENTITY_NULL) a->wpe__plan = true;
    }
}

WPE_NavQuery* wpe_nav_system_query(WPE_NavSystem* system) {
    return system && wpe_nav_system_grid(system) ? system->query : NULL;
}

const WPE_NavBlockers* wpe_nav_system_blockers(WPE_NavSystem* system) {
    return system && wpe_nav_system_grid(system) ? system->blockers : NULL;
}

const WPE_NavEvent* wpe_nav_system_events(const WPE_NavSystem* system, uint32_t* out_count) {
    if (out_count) *out_count = system ? system->event_count : 0;
    return system ? system->events : NULL;
}

/* ---- Control ------------------------------------------------------------------------------------------ */

static WPE_NavAgent* wpe__agent(WPE_NavSystem* system, WPE_Entity entity) {
    return system ? wpe_ecs_get_component(system->ecs, entity, WPE_COMP_NAV_AGENT) : NULL;
}

static bool wpe__finite3(WPE_Vec3 v) {
    return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

WPE_Result wpe_nav_agent_set_destination(WPE_NavSystem* system, WPE_Entity entity, WPE_Vec3 destination) {
    if (!system) return WPE_RESULT_ERROR_NULL_ARG;
    WPE_NavAgent* a = wpe__agent(system, entity);
    if (!a) return WPE_RESULT_ERROR_NOT_FOUND;
    if (!wpe__finite3(destination)) return WPE_RESULT_ERROR_INVALID_ARG;
    a->destination = destination;
    a->follow = WPE_ENTITY_NULL;
    a->state = WPE_NAV_AGENT_MOVING;
    a->wpe__plan = true;
    return WPE_RESULT_OK;
}

static bool wpe__world_position(const WPE_Ecs* ecs, WPE_Entity entity, WPE_Vec3* out) {
    WPE_Mat4 world;
    if (!wpe_scene_compute_world_matrix(ecs, entity, &world)) return false;
    *out = wpe_vec3(world.m[3][0], world.m[3][1], world.m[3][2]);
    return wpe__finite3(*out);
}

WPE_Result wpe_nav_agent_follow(WPE_NavSystem* system, WPE_Entity entity, WPE_Entity target) {
    if (!system) return WPE_RESULT_ERROR_NULL_ARG;
    WPE_NavAgent* a = wpe__agent(system, entity);
    if (!a) return WPE_RESULT_ERROR_NOT_FOUND;
    WPE_Vec3 where;
    if (target == entity || !wpe_ecs_is_alive(system->ecs, target) ||
        !wpe__world_position(system->ecs, target, &where))
        return WPE_RESULT_ERROR_INVALID_ARG;
    a->follow = target;
    a->destination = where;
    a->state = WPE_NAV_AGENT_MOVING;
    a->wpe__plan = true;
    a->wpe__repath_timer = a->repath_interval;
    return WPE_RESULT_OK;
}

WPE_Result wpe_nav_agent_stop(WPE_NavSystem* system, WPE_Entity entity) {
    if (!system) return WPE_RESULT_ERROR_NULL_ARG;
    WPE_NavAgent* a = wpe__agent(system, entity);
    if (!a) return WPE_RESULT_ERROR_NOT_FOUND;
    a->state = WPE_NAV_AGENT_IDLE;
    a->follow = WPE_ENTITY_NULL;
    a->remaining = 0.0f;
    a->wpe__count = 0;
    a->wpe__plan = false;
    return WPE_RESULT_OK;
}

const WPE_Vec3* wpe_nav_agent_path(const WPE_NavSystem* system, WPE_Entity entity, uint32_t* out_count) {
    if (out_count) *out_count = 0;
    const WPE_NavAgent* a = system ? wpe_ecs_get_component(system->ecs, entity, WPE_COMP_NAV_AGENT) : NULL;
    if (!a || a->wpe__count == 0 || a->wpe__next >= a->wpe__count) return NULL;
    if (out_count) *out_count = a->wpe__count - a->wpe__next;
    return a->wpe__path + a->wpe__next;
}

/* ---- Update -------------------------------------------------------------------------------------------- */

static bool wpe__push_event(WPE_NavSystem* s, WPE_NavEventType type, WPE_Entity entity) {
    if (s->event_count == s->event_capacity) {
        const uint32_t grown = s->event_capacity ? s->event_capacity * 2u : 16u;
        WPE_NavEvent* bigger = wpe_realloc(s->events, (size_t)grown * sizeof *bigger);
        if (!bigger) return false;
        s->events = bigger;
        s->event_capacity = grown;
    }
    s->events[s->event_count++] = (WPE_NavEvent){.type = type, .entity = entity};
    return true;
}

static bool wpe__matrix_moved(const WPE_Mat4* a, const WPE_Mat4* b) {
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            if (fabsf(a->m[c][r] - b->m[c][r]) > 1e-4f) return true;
        }
    }
    return false;
}

/* Carves every obstacle again when any moved (or was added or removed). @return Whether the cells changed. */
static bool wpe__carve(WPE_NavSystem* s) {
    bool changed = s->carve_again;
    WPE_EcsIter it = wpe_ecs_query(s->ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_NAV_OBSTACLE)});
    while (wpe_ecs_iter_next(&it) && !changed) {
        const WPE_NavObstacle* o = wpe_ecs_iter_get(&it, WPE_COMP_NAV_OBSTACLE);
        WPE_Mat4 world;
        if (!wpe_scene_compute_world_matrix(s->ecs, it.entity, &world)) continue;
        changed = !o->wpe__placed || wpe__matrix_moved(&world, &o->wpe__carved);
    }
    if (!changed) return false;
    wpe_nav_blockers_clear(s->blockers);
    it = wpe_ecs_query(s->ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_NAV_OBSTACLE)});
    while (wpe_ecs_iter_next(&it)) {
        WPE_NavObstacle* o = wpe_ecs_iter_get(&it, WPE_COMP_NAV_OBSTACLE);
        WPE_Mat4 world;
        if (!wpe_scene_compute_world_matrix(s->ecs, it.entity, &world)) continue;
        o->wpe__carved = world;
        o->wpe__placed = true;
        if (o->carve)
            (void)wpe_nav_blockers_add_box(s->blockers, &world, o->offset, wpe_vec3_scale(o->size, 0.5f));
    }
    s->carve_again = false;
    return true;
}

static float wpe__distance_xz(WPE_Vec3 a, WPE_Vec3 b) {
    return sqrtf((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z));
}

/* Whether the way from `p` along the rest of the path is still open. */
static bool wpe__path_open(const WPE_NavSystem* s, const WPE_NavAgent* a, WPE_Vec3 p) {
    WPE_Vec3 from = p;
    for (uint32_t i = a->wpe__next; i < a->wpe__count; ++i) {
        if (!wpe_nav_grid_line_clear(s->grid, s->blockers, from, a->wpe__path[i])) return false;
        from = a->wpe__path[i];
    }
    return true;
}

/* Plans the way to the agent's destination. @return false when out of memory. */
static bool wpe__plan(WPE_NavSystem* s, WPE_Entity entity, WPE_NavAgent* a, WPE_Vec3 p) {
    a->wpe__plan = false;
    WPE_NavPathOptions options = wpe_nav_path_options_default();
    options.allow_partial = true;
    options.blockers = s->blockers;
    WPE_NavPath path;
    const WPE_Result result = wpe_nav_find_path(s->query, p, a->destination, &options, &path);
    if (result == WPE_RESULT_ERROR_OUT_OF_MEMORY) return false;
    if (result != WPE_RESULT_OK) {
        a->wpe__count = 0;
        a->remaining = 0.0f;
        const bool news = a->state != WPE_NAV_AGENT_UNREACHABLE;
        a->state = WPE_NAV_AGENT_UNREACHABLE;
        return !news || wpe__push_event(s, WPE_NAV_EVENT_FAILED, entity);
    }
    /* A follower standing by its target stays where it is. */
    if (a->follow != WPE_ENTITY_NULL && a->state == WPE_NAV_AGENT_ARRIVED && !path.partial &&
        path.length <= a->arrive_distance) {
        a->wpe__count = 0;
        return true;
    }
    if (path.count > a->wpe__capacity) {
        WPE_Vec3* bigger = wpe_realloc(a->wpe__path, (size_t)path.count * sizeof *bigger);
        if (!bigger) return false;
        a->wpe__path = bigger;
        a->wpe__capacity = path.count;
    }
    memcpy(a->wpe__path, path.points, (size_t)path.count * sizeof *path.points);
    a->wpe__count = path.count;
    a->wpe__next = path.count > 1 ? 1u : 0u;
    a->wpe__partial = path.partial;
    a->state = WPE_NAV_AGENT_MOVING;
    return true;
}

/* The horizontal velocity the agent wants: along its path, slowing near the end. Arrivals happen here. */
static bool wpe__steer(WPE_NavSystem* s, WPE_Entity entity, WPE_NavAgent* a, WPE_Vec3 p, float dt,
                       WPE_Vec3* out_desired) {
    *out_desired = wpe_vec3(0, 0, 0);
    if (a->state != WPE_NAV_AGENT_MOVING || a->wpe__count == 0) {
        a->remaining = 0.0f;
        return true;
    }
    const float reached = fmaxf(WPE__CORNER_REACHED, a->speed * dt);
    while (a->wpe__next + 1u < a->wpe__count && wpe__distance_xz(p, a->wpe__path[a->wpe__next]) < reached)
        ++a->wpe__next;
    float remaining = wpe__distance_xz(p, a->wpe__path[a->wpe__next]);
    for (uint32_t i = a->wpe__next + 1u; i < a->wpe__count; ++i)
        remaining += wpe__distance_xz(a->wpe__path[i - 1u], a->wpe__path[i]);
    a->remaining = remaining;
    if (remaining <= a->arrive_distance) {
        /* A follower at the end of a path to where its target was: if the target has moved on, after it. */
        WPE_Vec3 target;
        if (a->follow != WPE_ENTITY_NULL && !a->wpe__partial &&
            wpe__world_position(s->ecs, a->follow, &target) &&
            wpe__distance_xz(p, target) > 2.0f * a->arrive_distance) {
            a->destination = target;
            a->wpe__plan = true;
            a->wpe__repath_timer = a->repath_interval;
            return true;
        }
        const bool partial = a->wpe__partial;
        a->state = partial ? WPE_NAV_AGENT_UNREACHABLE : WPE_NAV_AGENT_ARRIVED;
        a->wpe__count = 0;
        a->remaining = 0.0f;
        return wpe__push_event(s, partial ? WPE_NAV_EVENT_FAILED : WPE_NAV_EVENT_ARRIVED, entity);
    }
    const WPE_Vec3 to = a->wpe__path[a->wpe__next];
    const float d = wpe__distance_xz(p, to);
    if (d < 1e-5f) return true;
    const float slow = a->slow_distance > 0.0f ? fminf(1.0f, remaining / a->slow_distance) : 1.0f;
    const float speed = a->speed * fmaxf(slow, 0.1f);
    *out_desired = wpe_vec3((to.x - p.x) / d * speed, 0.0f, (to.z - p.z) / d * speed);
    return true;
}

/* Adds sidesteps around nearby agents (and non-carving obstacles' centers), keeping the move walkable. */
static WPE_Vec3 wpe__avoid(const WPE_NavSystem* s, WPE_Entity entity, const WPE_NavAgent* a, WPE_Vec3 p,
                           WPE_Vec3 desired) {
    const float length = sqrtf(desired.x * desired.x + desired.z * desired.z);
    if (!a->avoid_others || length < 1e-4f) return desired;
    const float touch = 2.0f * s->grid->settings.agent_radius, range = touch + WPE__AVOID_RANGE;
    const WPE_Vec3 heading = wpe_vec3(desired.x / length, 0.0f, desired.z / length);
    WPE_Vec3 steer = desired;
    for (uint32_t i = 0; i < s->info_count; ++i) {
        const wpe__AgentInfo* other = &s->infos[i];
        if (other->entity == entity || fabsf(other->position.y - p.y) > s->grid->settings.agent_height)
            continue;
        const float dx = p.x - other->position.x, dz = p.z - other->position.z;
        const float dist = sqrtf(dx * dx + dz * dz);
        if (dist >= range || dist < 1e-4f) continue;
        const float strength = fminf(1.0f, (range - dist) / (range - touch * 0.5f));
        const float ax = dx / dist, az = dz / dist; /* Away from the other. */
        steer.x += ax * a->speed * strength;
        steer.z += az * a->speed * strength;
        /* Someone ahead: pass on the right (both do, so they don't block each other head on). */
        if (-(ax * heading.x + az * heading.z) > 0.5f) {
            steer.x += -heading.z * a->speed * strength * 0.6f;
            steer.z += heading.x * a->speed * strength * 0.6f;
        }
    }
    const float steer_length = sqrtf(steer.x * steer.x + steer.z * steer.z);
    if (steer_length > a->speed) steer = wpe_vec3_scale(steer, a->speed / steer_length);
    const WPE_Vec3 ahead = wpe_vec3_add(p, wpe_vec3_scale(steer, WPE__AVOID_LOOKAHEAD));
    return wpe_nav_grid_line_clear(s->grid, s->blockers, p, ahead) ? steer : desired;
}

/* Puts an entity at a world position (relative to its parent, as transforms are). */
static void wpe__set_world_position(WPE_Ecs* ecs, WPE_Entity entity, WPE_Vec3 position) {
    const WPE_Entity parent = wpe_scene_get_parent(ecs, entity);
    if (parent != WPE_ENTITY_NULL) {
        WPE_Mat4 parent_world, inverse;
        if (!wpe_scene_compute_world_matrix(ecs, parent, &parent_world) ||
            !wpe_mat4_inverse(&parent_world, &inverse))
            return;
        position = wpe_mat4_transform_point(&inverse, position);
    }
    WPE_Transform* t = wpe_ecs_get_component(ecs, entity, WPE_COMP_TRANSFORM);
    if (t) t->position = position;
}

/* Turns the entity about Y toward the way it walks, at most turn_speed. */
static void wpe__face(WPE_Ecs* ecs, WPE_Entity entity, const WPE_NavAgent* a, WPE_Vec3 velocity, float dt) {
    if (!(a->turn_speed > 0.0f) || velocity.x * velocity.x + velocity.z * velocity.z < 0.01f) return;
    WPE_Mat4 world;
    WPE_Vec3 position, scale;
    WPE_Quat rotation;
    if (!wpe_scene_compute_world_matrix(ecs, entity, &world) ||
        !wpe_mat4_decompose(&world, &position, &rotation, &scale))
        return;
    const WPE_Vec3 forward = wpe_quat_rotate_vec3(rotation, wpe_vec3(0, 0, -1)); /* Entities look down -Z. */
    const float yaw = atan2f(-forward.x, -forward.z), goal = atan2f(-velocity.x, -velocity.z);
    float delta = goal - yaw;
    while (delta > WPE__PI)
        delta -= 2.0f * WPE__PI;
    while (delta < -WPE__PI)
        delta += 2.0f * WPE__PI;
    const float step = a->turn_speed * WPE__PI / 180.0f * dt;
    const float turned = yaw + (fabsf(delta) <= step ? delta : copysignf(step, delta));
    WPE_Quat world_rotation = wpe_quat_from_axis_angle(wpe_vec3(0, 1, 0), turned);
    /* Relative to the parent's rotation. */
    const WPE_Entity parent = wpe_scene_get_parent(ecs, entity);
    WPE_Mat4 parent_world;
    if (parent != WPE_ENTITY_NULL && wpe_scene_compute_world_matrix(ecs, parent, &parent_world)) {
        WPE_Vec3 pp, ps;
        WPE_Quat pr;
        if (!wpe_mat4_decompose(&parent_world, &pp, &pr, &ps)) return;
        const WPE_Quat inverse = {.x = -pr.x, .y = -pr.y, .z = -pr.z, .w = pr.w};
        world_rotation = wpe_quat_mul(inverse, world_rotation);
    }
    WPE_Transform* t = wpe_ecs_get_component(ecs, entity, WPE_COMP_TRANSFORM);
    if (t) t->rotation = wpe_quat_normalize(world_rotation);
}

/* Moves the agent by its velocity: through its character controller, or along the floor. */
static void wpe__move(WPE_NavSystem* s, WPE_Entity entity, WPE_NavAgent* a, WPE_Vec3 p, WPE_Vec3 horizontal,
                      float dt) {
    if (s->physics && wpe_ecs_has_component(s->ecs, entity, WPE_COMP_CHARACTER)) {
        const float fall = a->velocity.y + wpe_physics_gravity(s->physics).y * dt;
        WPE_Vec3 out;
        if (wpe_character_move(s->physics, entity, wpe_vec3(horizontal.x, fall, horizontal.z), dt, &out) ==
            WPE_RESULT_OK)
            a->velocity = out;
        return;
    }
    const WPE_Vec3 wanted = wpe_vec3_add(p, wpe_vec3_scale(horizontal, dt));
    WPE_Vec3 on_floor;
    if (wpe_nav_grid_nearest(s->grid, wanted, s->grid->settings.cell_size,
                             s->grid->settings.agent_height * 0.5f, &on_floor) == WPE_NAV_CELL_NONE) {
        a->velocity = wpe_vec3(0, 0, 0); /* Nothing walkable ahead: stay. */
        return;
    }
    wpe__set_world_position(s->ecs, entity, on_floor);
    a->velocity = wpe_vec3_scale(wpe_vec3_sub(on_floor, p), 1.0f / dt);
}

static bool wpe__snapshot(WPE_NavSystem* s) {
    s->info_count = 0;
    WPE_EcsIter it = wpe_ecs_query(s->ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_NAV_AGENT)});
    while (wpe_ecs_iter_next(&it)) {
        WPE_Vec3 p;
        if (!wpe__world_position(s->ecs, it.entity, &p)) continue;
        if (s->info_count == s->info_capacity) {
            const uint32_t grown = s->info_capacity ? s->info_capacity * 2u : 16u;
            wpe__AgentInfo* bigger = wpe_realloc(s->infos, (size_t)grown * sizeof *bigger);
            if (!bigger) return false;
            s->infos = bigger;
            s->info_capacity = grown;
        }
        s->infos[s->info_count++] = (wpe__AgentInfo){.entity = it.entity, .position = p};
    }
    return true;
}

static uint32_t wpe__count(const WPE_Ecs* ecs, WPE_ComponentType type) {
    WPE_EcsPoolView view;
    return wpe_ecs_pool_view(ecs, type, &view) == WPE_RESULT_OK ? view.count : 0;
}

WPE_Result wpe_nav_system_update(WPE_NavSystem* system, float dt) {
    if (!system) return WPE_RESULT_ERROR_NULL_ARG;
    if (!(dt > 0.0f) || !isfinite(dt)) return WPE_RESULT_ERROR_INVALID_ARG;
    system->event_count = 0;
    if (wpe__count(system->ecs, WPE_COMP_NAV_AGENT) == 0) return WPE_RESULT_OK;
    if (!wpe_nav_system_grid(system)) return WPE_RESULT_OK; /* No grid: agents wait. */
    WPE_NavSystem* s = system;
    const bool carved = wpe__carve(s);
    if (!wpe__snapshot(s)) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    for (uint32_t i = 0; i < s->info_count; ++i) {
        const WPE_Entity entity = s->infos[i].entity;
        WPE_NavAgent* a = wpe_ecs_get_component(s->ecs, entity, WPE_COMP_NAV_AGENT);
        if (!a || (s->simulates && !s->simulates(entity, s->simulates_user_data))) continue;
        const WPE_Vec3 p = s->infos[i].position;
        if (a->follow != WPE_ENTITY_NULL) {
            WPE_Vec3 target;
            if (!wpe_ecs_is_alive(s->ecs, a->follow) || !wpe__world_position(s->ecs, a->follow, &target)) {
                a->follow = WPE_ENTITY_NULL; /* What it followed is gone: stand still. */
                a->state = WPE_NAV_AGENT_IDLE;
                a->wpe__count = 0;
            } else if ((a->wpe__repath_timer -= dt) <= 0.0f) {
                a->wpe__repath_timer = a->repath_interval > 0.0f ? a->repath_interval : dt;
                a->destination = target;
                a->wpe__plan = true;
            }
        }
        if (carved && a->state == WPE_NAV_AGENT_MOVING && a->wpe__count > 0 && !wpe__path_open(s, a, p))
            a->wpe__plan = true;
        if (a->wpe__plan && !wpe__plan(s, entity, a, p)) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
        WPE_Vec3 desired;
        if (!wpe__steer(s, entity, a, p, dt, &desired)) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
        desired = wpe__avoid(s, entity, a, p, desired);
        /* Accelerate the horizontal velocity toward the wanted one. */
        WPE_Vec3 horizontal = wpe_vec3(a->velocity.x, 0.0f, a->velocity.z);
        const WPE_Vec3 change = wpe_vec3_sub(desired, horizontal);
        const float change_length = wpe_vec3_length(change), most = a->acceleration * dt;
        horizontal = change_length <= most || !(a->acceleration > 0.0f)
                         ? desired
                         : wpe_vec3_add(horizontal, wpe_vec3_scale(change, most / change_length));
        wpe__move(s, entity, a, p, horizontal, dt);
        wpe__face(s->ecs, entity, a, horizontal, dt);
    }
    return WPE_RESULT_OK;
}

WPE_Result wpe_nav_system_refresh(WPE_NavSystem* system) {
    if (!system) return WPE_RESULT_ERROR_NULL_ARG;
    if (!wpe_nav_system_grid(system)) return WPE_RESULT_ERROR_NOT_FOUND;
    (void)wpe__carve(system); /* Only when an obstacle moved since. */
    return WPE_RESULT_OK;
}

/* ---- Debug drawing ----------------------------------------------------------------------------------- */

#define WPE__AGENT_COLOR 0x40D0F0u
#define WPE__PATH_COLOR 0xFFD040u
#define WPE__OBSTACLE_COLOR 0xF0A040u
#define WPE__BLOCKED_COLOR 0xF04040u

void wpe_nav_system_debug_draw(WPE_NavSystem* system, WPE_NavSystemLineFn line, void* user_data) {
    if (!system || !line) return;
    const WPE_NavGrid* g = system->grid; /* May be NULL: then only the boxes and footprints. */
    const float radius = g ? g->settings.agent_radius : wpe_nav_settings_default().agent_radius;
    const WPE_Vec3 lift = wpe_vec3(0, 0.1f, 0);
    WPE_EcsIter it = wpe_ecs_query(system->ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_NAV_AGENT)});
    while (wpe_ecs_iter_next(&it)) {
        const WPE_NavAgent* a = wpe_ecs_iter_get(&it, WPE_COMP_NAV_AGENT);
        WPE_Vec3 from;
        if (!wpe__world_position(system->ecs, it.entity, &from)) continue;
        /* Its footprint: the circle its center keeps from walls. */
        enum { SEGMENTS = 16 };
        for (int i = 0; i < SEGMENTS; ++i) {
            const float t0 = 2.0f * WPE__PI * (float)i / SEGMENTS,
                        t1 = 2.0f * WPE__PI * (float)(i + 1) / SEGMENTS;
            line(wpe_vec3(from.x + radius * cosf(t0), from.y + 0.05f, from.z + radius * sinf(t0)),
                 wpe_vec3(from.x + radius * cosf(t1), from.y + 0.05f, from.z + radius * sinf(t1)),
                 WPE__AGENT_COLOR, user_data);
        }
        if (a->state != WPE_NAV_AGENT_MOVING) continue;
        for (uint32_t i = a->wpe__next; i < a->wpe__count; ++i) {
            line(wpe_vec3_add(from, lift), wpe_vec3_add(a->wpe__path[i], lift), WPE__PATH_COLOR, user_data);
            from = a->wpe__path[i];
        }
    }
    it = wpe_ecs_query(system->ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_NAV_OBSTACLE)});
    while (wpe_ecs_iter_next(&it)) {
        const WPE_NavObstacle* o = wpe_ecs_iter_get(&it, WPE_COMP_NAV_OBSTACLE);
        WPE_Mat4 world;
        if (!wpe_scene_compute_world_matrix(system->ecs, it.entity, &world)) continue;
        WPE_Vec3 corners[8];
        for (uint32_t i = 0; i < 8; ++i) {
            const WPE_Vec3 local = wpe_vec3(o->offset.x + ((i & 1u) ? 0.5f : -0.5f) * o->size.x,
                                            o->offset.y + ((i & 2u) ? 0.5f : -0.5f) * o->size.y,
                                            o->offset.z + ((i & 4u) ? 0.5f : -0.5f) * o->size.z);
            corners[i] = wpe_mat4_transform_point(&world, local);
        }
        /* The 12 edges: corner pairs differing in one bit. */
        for (uint32_t i = 0; i < 8; ++i) {
            for (uint32_t bit = 1; bit < 8; bit <<= 1) {
                if (!(i & bit)) line(corners[i], corners[i | bit], WPE__OBSTACLE_COLOR, user_data);
            }
        }
    }
    if (!g || !system->blockers || system->blockers->count == 0) return;
    const float half = g->settings.cell_size * 0.5f;
    for (uint32_t cell = 0; cell < g->cell_count; ++cell) {
        if (!system->blockers->blocked[cell]) continue;
        const WPE_Vec3 c = wpe_vec3_add(wpe__nav_center(g, cell), wpe_vec3(0, 0.06f, 0));
        const WPE_Vec3 k[4] = {wpe_vec3(c.x - half, c.y, c.z - half), wpe_vec3(c.x + half, c.y, c.z - half),
                               wpe_vec3(c.x + half, c.y, c.z + half), wpe_vec3(c.x - half, c.y, c.z + half)};
        for (int e = 0; e < 4; ++e)
            line(k[e], k[(e + 1) % 4], WPE__BLOCKED_COLOR, user_data);
    }
}