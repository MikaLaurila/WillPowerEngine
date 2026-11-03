/*
 * Luau bindings for navigation (docs/navigation.md, docs/scripting.md): agents that walk to places or
 * after entities, obstacles that close cells to them, and questions about the nav grid (paths, nearest
 * walkable points, straight walks, random points).
 *
 * Bindings follow the error-safety rule of script_internal.h: every argument is read and checked
 * (which may raise) before the navigation system is touched. Path points belong to the system's query,
 * so nothing is owned across a raise.
 */
#define WPE_LOG_MODULE "script"
#include <math.h>
#include <string.h>

#include "navigation/nav_agents.h"
#include "navigation/nav_path.h"
#include "scene/scene.h"
#include "script/script_internal.h"

#define WPE__NAV_SEARCH 1.0f /* How far (sideways) from the walkable area points may be. */
#define WPE__RANDOM_TRIES 64u

static const char* const wpe__agent_states[] = {"idle", "moving", "arrived", "unreachable"};
static_assert(sizeof wpe__agent_states / sizeof wpe__agent_states[0] == WPE_NAV_AGENT_UNREACHABLE + 1,
              "agent state names out of sync with WPE_NavAgentState");

/* ---- Helpers ------------------------------------------------------------------------------------------ */

static WPE_NavSystem* wpe__nav(lua_State* L, const char* function) {
    WPE_NavSystem* nav = wpe__script_host_from(L)->nav;
    if (!nav) luaL_errorL(L, "wpe.%s: navigation is not enabled for this world", function);
    return nav;
}

static WPE_Entity wpe__nav_entity(lua_State* L, int index) {
    const double value = luaL_checknumber(L, index);
    if (!(value >= 0.0 && value <= (double)UINT32_MAX && value == floor(value)))
        luaL_errorL(L, "argument #%d is not an entity handle", index);
    const WPE_Entity entity = (WPE_Entity)value;
    if (!wpe_ecs_is_alive(wpe__script_host_from(L)->ecs, entity))
        luaL_errorL(L, "argument #%d: entity 0x%08x is not alive", index, entity);
    return entity;
}

static WPE_Vec3 wpe__nav_vector(lua_State* L, int index, const char* function) {
    const float* v = luaL_checkvector(L, index);
    if (!(isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2])))
        luaL_errorL(L, "wpe.%s: argument #%d must be a finite vector", function, index);
    return wpe_vec3(v[0], v[1], v[2]);
}

static void wpe__nav_push_vector(lua_State* L, WPE_Vec3 v) {
    lua_pushvector(L, v.x, v.y, v.z);
}

static float wpe__nav_field(lua_State* L, int table, const char* key, float fallback, const char* function,
                            float min) {
    lua_getfield(L, table, key);
    float value = fallback;
    if (!lua_isnil(L, -1)) {
        value = (float)luaL_checknumber(L, -1);
        if (!isfinite(value) || value < min)
            luaL_errorL(L, "wpe.%s: %s must be a number of at least %g", function, key, (double)min);
    }
    lua_pop(L, 1);
    return value;
}

static bool wpe__nav_flag(lua_State* L, int table, const char* key, bool fallback) {
    lua_getfield(L, table, key);
    const bool value = lua_isnil(L, -1) ? fallback : luaL_checkboolean(L, -1);
    lua_pop(L, 1);
    return value;
}

static WPE_NavAgent* wpe__nav_agent(lua_State* L, const char* function, WPE_Entity entity) {
    WPE_NavAgent* a = wpe_ecs_get_component(wpe__script_host_from(L)->ecs, entity, WPE_COMP_NAV_AGENT);
    if (!a) luaL_errorL(L, "wpe.%s: entity 0x%08x has no nav agent (wpe.add_nav_agent)", function, entity);
    return a;
}

static float wpe__standing(const WPE_NavGrid* grid) {
    return wpe_nav_grid_info(grid).settings.agent_height * 0.5f;
}

/* ---- Agents ---------------------------------------------------------------------------------------------
 */

static int wpe__api_add_nav_agent(lua_State* L) {
    WPE_ScriptHost* host = wpe__script_host_from(L);
    wpe__nav(L, "add_nav_agent");
    const WPE_Entity entity = wpe__nav_entity(L, 1);
    const WPE_NavAgent* existing = wpe_ecs_get_component(host->ecs, entity, WPE_COMP_NAV_AGENT);
    WPE_NavAgent a = existing ? *existing : wpe_nav_agent_default();
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        a.speed = wpe__nav_field(L, 2, "speed", a.speed, "add_nav_agent", 0.0f);
        a.acceleration = wpe__nav_field(L, 2, "acceleration", a.acceleration, "add_nav_agent", 0.0f);
        a.arrive_distance =
            wpe__nav_field(L, 2, "arrive_distance", a.arrive_distance, "add_nav_agent", 0.01f);
        a.slow_distance = wpe__nav_field(L, 2, "slow_distance", a.slow_distance, "add_nav_agent", 0.0f);
        a.turn_speed = wpe__nav_field(L, 2, "turn_speed", a.turn_speed, "add_nav_agent", 0.0f);
        a.repath_interval =
            wpe__nav_field(L, 2, "repath_interval", a.repath_interval, "add_nav_agent", 0.05f);
        a.avoid_others = wpe__nav_flag(L, 2, "avoid_others", a.avoid_others);
    }
    if (!wpe_ecs_has_component(host->ecs, entity, WPE_COMP_TRANSFORM))
        luaL_errorL(L, "wpe.add_nav_agent: entity 0x%08x has no Transform", entity);
    WPE_NavAgent* stored = wpe_ecs_get_component(host->ecs, entity, WPE_COMP_NAV_AGENT);
    if (stored) {
        /* Only the settings change: it keeps going where it was going. */
        stored->speed = a.speed;
        stored->acceleration = a.acceleration;
        stored->arrive_distance = a.arrive_distance;
        stored->slow_distance = a.slow_distance;
        stored->turn_speed = a.turn_speed;
        stored->repath_interval = a.repath_interval;
        stored->avoid_others = a.avoid_others;
        return 0;
    }
    const WPE_Result result = wpe_ecs_add_component(host->ecs, entity, WPE_COMP_NAV_AGENT, &a, NULL);
    if (result != WPE_RESULT_OK) luaL_errorL(L, "wpe.add_nav_agent failed: %s", wpe_result_to_string(result));
    return 0;
}

static int wpe__api_remove_nav_component(lua_State* L, const char* function, WPE_ComponentType type) {
    WPE_ScriptHost* host = wpe__script_host_from(L);
    if (host->restricted_depth > 0 || host->shutting_down)
        luaL_errorL(L, "wpe.%s is not allowed in on_destroy", function);
    wpe__nav(L, function);
    const WPE_Entity entity = wpe__nav_entity(L, 1);
    if (wpe_ecs_has_component(host->ecs, entity, type)) {
        const WPE_Result result = wpe_ecs_remove_component(host->ecs, entity, type);
        if (result != WPE_RESULT_OK)
            luaL_errorL(L, "wpe.%s failed: %s", function, wpe_result_to_string(result));
    }
    return 0;
}

static int wpe__api_remove_nav_agent(lua_State* L) {
    return wpe__api_remove_nav_component(L, "remove_nav_agent", WPE_COMP_NAV_AGENT);
}

static int wpe__api_get_nav_agent(lua_State* L) {
    wpe__nav(L, "get_nav_agent");
    const WPE_NavAgent copy = *wpe__nav_agent(L, "get_nav_agent", wpe__nav_entity(L, 1));
    lua_createtable(L, 0, 12);
    const struct {
        const char* key;
        float value;
    } numbers[] = {{"speed", copy.speed},
                   {"acceleration", copy.acceleration},
                   {"arrive_distance", copy.arrive_distance},
                   {"slow_distance", copy.slow_distance},
                   {"turn_speed", copy.turn_speed},
                   {"repath_interval", copy.repath_interval},
                   {"remaining", copy.remaining}};
    for (size_t i = 0; i < sizeof numbers / sizeof numbers[0]; ++i) {
        lua_pushnumber(L, numbers[i].value);
        lua_setfield(L, -2, numbers[i].key);
    }
    lua_pushboolean(L, copy.avoid_others);
    lua_setfield(L, -2, "avoid_others");
    lua_pushstring(L, wpe__agent_states[copy.state]);
    lua_setfield(L, -2, "state");
    wpe__nav_push_vector(L, copy.velocity);
    lua_setfield(L, -2, "velocity");
    if (copy.state != WPE_NAV_AGENT_IDLE) {
        wpe__nav_push_vector(L, copy.destination);
        lua_setfield(L, -2, "destination");
    }
    if (copy.follow != WPE_ENTITY_NULL) {
        lua_pushnumber(L, (double)copy.follow);
        lua_setfield(L, -2, "following");
    }
    return 1;
}

static int wpe__api_nav_move_to(lua_State* L) {
    WPE_NavSystem* nav = wpe__nav(L, "nav_move_to");
    const WPE_Entity entity = wpe__nav_entity(L, 1);
    const WPE_Vec3 destination = wpe__nav_vector(L, 2, "nav_move_to");
    wpe__nav_agent(L, "nav_move_to", entity);
    (void)wpe_nav_agent_set_destination(nav, entity, destination); /* Checked above: can't fail. */
    return 0;
}

static int wpe__api_nav_follow(lua_State* L) {
    WPE_NavSystem* nav = wpe__nav(L, "nav_follow");
    const WPE_Entity entity = wpe__nav_entity(L, 1);
    const WPE_Entity target = wpe__nav_entity(L, 2);
    wpe__nav_agent(L, "nav_follow", entity);
    if (target == entity) luaL_errorL(L, "wpe.nav_follow: an agent can't follow itself");
    if (!wpe_ecs_has_component(wpe__script_host_from(L)->ecs, target, WPE_COMP_TRANSFORM))
        luaL_errorL(L, "wpe.nav_follow: entity 0x%08x has no Transform", target);
    (void)wpe_nav_agent_follow(nav, entity, target);
    return 0;
}

static int wpe__api_nav_stop(lua_State* L) {
    WPE_NavSystem* nav = wpe__nav(L, "nav_stop");
    const WPE_Entity entity = wpe__nav_entity(L, 1);
    wpe__nav_agent(L, "nav_stop", entity);
    (void)wpe_nav_agent_stop(nav, entity);
    return 0;
}

static int wpe__api_nav_state(lua_State* L) {
    wpe__nav(L, "nav_state");
    const WPE_NavAgent* a = wpe__nav_agent(L, "nav_state", wpe__nav_entity(L, 1));
    const WPE_NavAgentState state = a->state;
    const float remaining = a->remaining;
    lua_pushstring(L, wpe__agent_states[state]);
    lua_pushnumber(L, remaining);
    return 2;
}

static void wpe__push_points(lua_State* L, const WPE_Vec3* points, uint32_t count) {
    lua_createtable(L, (int)count, 0);
    for (uint32_t i = 0; i < count; ++i) {
        wpe__nav_push_vector(L, points[i]);
        lua_rawseti(L, -2, (int)i + 1);
    }
}

static int wpe__api_nav_path(lua_State* L) {
    WPE_NavSystem* nav = wpe__nav(L, "nav_path");
    const WPE_Entity entity = wpe__nav_entity(L, 1);
    wpe__nav_agent(L, "nav_path", entity);
    uint32_t count = 0;
    const WPE_Vec3* points = wpe_nav_agent_path(nav, entity, &count);
    wpe__push_points(L, points, count);
    return 1;
}

/* ---- Obstacles ---------------------------------------------------------------------------------------- */

static int wpe__api_add_nav_obstacle(lua_State* L) {
    WPE_ScriptHost* host = wpe__script_host_from(L);
    wpe__nav(L, "add_nav_obstacle");
    const WPE_Entity entity = wpe__nav_entity(L, 1);
    WPE_NavObstacle o = wpe_nav_obstacle_default();
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        lua_getfield(L, 2, "size");
        if (!lua_isnil(L, -1)) o.size = wpe__nav_vector(L, -1, "add_nav_obstacle");
        lua_pop(L, 1);
        lua_getfield(L, 2, "offset");
        if (!lua_isnil(L, -1)) o.offset = wpe__nav_vector(L, -1, "add_nav_obstacle");
        lua_pop(L, 1);
        o.carve = wpe__nav_flag(L, 2, "carve", o.carve);
    }
    if (!(o.size.x > 0.0f && o.size.y > 0.0f && o.size.z > 0.0f))
        luaL_errorL(L, "wpe.add_nav_obstacle: size must be positive on every axis");
    if (!wpe_ecs_has_component(host->ecs, entity, WPE_COMP_TRANSFORM))
        luaL_errorL(L, "wpe.add_nav_obstacle: entity 0x%08x has no Transform", entity);
    WPE_NavObstacle* stored = wpe_ecs_get_component(host->ecs, entity, WPE_COMP_NAV_OBSTACLE);
    if (stored) {
        /* Re-adding carves it again with the new box (its private pose mark goes). */
        stored->size = o.size;
        stored->offset = o.offset;
        stored->carve = o.carve;
        stored->wpe__placed = false;
        return 0;
    }
    const WPE_Result result = wpe_ecs_add_component(host->ecs, entity, WPE_COMP_NAV_OBSTACLE, &o, NULL);
    if (result != WPE_RESULT_OK)
        luaL_errorL(L, "wpe.add_nav_obstacle failed: %s", wpe_result_to_string(result));
    return 0;
}

static int wpe__api_remove_nav_obstacle(lua_State* L) {
    return wpe__api_remove_nav_component(L, "remove_nav_obstacle", WPE_COMP_NAV_OBSTACLE);
}

/* ---- Questions ---------------------------------------------------------------------------------------- */

static int wpe__api_nav_find_path(lua_State* L) {
    WPE_NavSystem* nav = wpe__nav(L, "nav_find_path");
    const WPE_Vec3 from = wpe__nav_vector(L, 1, "nav_find_path");
    const WPE_Vec3 to = wpe__nav_vector(L, 2, "nav_find_path");
    WPE_NavPathOptions o = wpe_nav_path_options_default();
    bool obstacles = true;
    if (!lua_isnoneornil(L, 3)) {
        luaL_checktype(L, 3, LUA_TTABLE);
        o.allow_partial = wpe__nav_flag(L, 3, "partial", false);
        o.raw = wpe__nav_flag(L, 3, "raw", false);
        o.search_radius = wpe__nav_field(L, 3, "radius", o.search_radius, "nav_find_path", 0.0f);
        obstacles = wpe__nav_flag(L, 3, "obstacles", true);
    }
    WPE_NavQuery* query = wpe_nav_system_query(nav);
    if (!query) {
        lua_pushnil(L);
        return 1;
    }
    o.blockers = obstacles ? wpe_nav_system_blockers(nav) : NULL;
    WPE_NavPath path;
    const WPE_Result result = wpe_nav_find_path(query, from, to, &o, &path);
    if (result == WPE_RESULT_ERROR_OUT_OF_MEMORY) luaL_errorL(L, "wpe.nav_find_path: out of memory");
    if (result != WPE_RESULT_OK) {
        lua_pushnil(L);
        return 1;
    }
    wpe__push_points(L, path.points, path.count);
    lua_pushnumber(L, path.length);
    lua_pushboolean(L, path.partial);
    return 3;
}

static int wpe__api_nav_nearest(lua_State* L) {
    WPE_NavSystem* nav = wpe__nav(L, "nav_nearest");
    const WPE_Vec3 point = wpe__nav_vector(L, 1, "nav_nearest");
    const double radius = luaL_optnumber(L, 2, WPE__NAV_SEARCH);
    if (!(radius >= 0.0 && radius <= 1000.0)) luaL_errorL(L, "wpe.nav_nearest: radius must be 0..1000");
    const WPE_NavGrid* grid = wpe_nav_system_grid(nav);
    WPE_Vec3 nearest;
    if (!grid || wpe_nav_grid_nearest(grid, point, (float)radius, wpe__standing(grid), &nearest) ==
                     WPE_NAV_CELL_NONE) {
        lua_pushnil(L);
        return 1;
    }
    wpe__nav_push_vector(L, nearest);
    return 1;
}

static int wpe__api_nav_can_walk(lua_State* L) {
    WPE_NavSystem* nav = wpe__nav(L, "nav_can_walk");
    const WPE_Vec3 from = wpe__nav_vector(L, 1, "nav_can_walk");
    const WPE_Vec3 to = wpe__nav_vector(L, 2, "nav_can_walk");
    const WPE_NavGrid* grid = wpe_nav_system_grid(nav);
    lua_pushboolean(L, grid && wpe_nav_grid_line_clear(grid, wpe_nav_system_blockers(nav), from, to));
    return 1;
}

static int wpe__api_nav_reachable(lua_State* L) {
    WPE_NavSystem* nav = wpe__nav(L, "nav_reachable");
    const WPE_Vec3 from = wpe__nav_vector(L, 1, "nav_reachable");
    const WPE_Vec3 to = wpe__nav_vector(L, 2, "nav_reachable");
    const WPE_NavGrid* grid = wpe_nav_system_grid(nav);
    bool reachable = false;
    if (grid) {
        const uint32_t a = wpe_nav_grid_nearest(grid, from, WPE__NAV_SEARCH, wpe__standing(grid), NULL);
        const uint32_t b = wpe_nav_grid_nearest(grid, to, WPE__NAV_SEARCH, wpe__standing(grid), NULL);
        reachable = wpe_nav_grid_reachable(grid, a, b);
    }
    lua_pushboolean(L, reachable);
    return 1;
}

static uint32_t wpe__nav_next_random(WPE_ScriptHost* host) {
    uint32_t x = host->nav_random;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return host->nav_random = x;
}

/* A random walkable point: anywhere, or within `radius` of a center (on the center's island when the
 * center stands on the grid). The same sequence every run. */
static int wpe__api_nav_random_point(lua_State* L) {
    WPE_ScriptHost* host = wpe__script_host_from(L);
    WPE_NavSystem* nav = wpe__nav(L, "nav_random_point");
    const bool around = !lua_isnoneornil(L, 1);
    const WPE_Vec3 center = around ? wpe__nav_vector(L, 1, "nav_random_point") : wpe_vec3(0, 0, 0);
    const double radius = luaL_optnumber(L, 2, 10.0);
    if (!(radius > 0.0 && radius <= 10000.0))
        luaL_errorL(L, "wpe.nav_random_point: radius must be in (0, 10000]");
    const WPE_NavGrid* grid = wpe_nav_system_grid(nav);
    const WPE_NavGridInfo info = wpe_nav_grid_info(grid);
    if (!grid || info.cell_count == 0) {
        lua_pushnil(L);
        return 1;
    }
    const uint32_t home = around
                              ? wpe_nav_grid_nearest(grid, center, WPE__NAV_SEARCH, wpe__standing(grid), NULL)
                              : WPE_NAV_CELL_NONE;
    for (uint32_t attempt = 0; attempt < WPE__RANDOM_TRIES; ++attempt) {
        WPE_Vec3 p;
        uint32_t cell;
        if (around) {
            /* A point in the disc around the center, then the walkable point there. */
            const float angle = (float)(wpe__nav_next_random(host) % 100000u) / 100000.0f * 6.2831853f;
            const float r = (float)radius * sqrtf((float)(wpe__nav_next_random(host) % 100000u) / 100000.0f);
            const WPE_Vec3 sample =
                wpe_vec3(center.x + r * cosf(angle), center.y, center.z + r * sinf(angle));
            cell = wpe_nav_grid_nearest(grid, sample, info.settings.cell_size * 2.0f,
                                        info.settings.agent_height * 2.0f, &p);
            if (cell == WPE_NAV_CELL_NONE ||
                (home != WPE_NAV_CELL_NONE && !wpe_nav_grid_reachable(grid, home, cell)))
                continue;
        } else {
            cell = wpe__nav_next_random(host) % info.cell_count;
            WPE_NavCellInfo c;
            if (!wpe_nav_grid_cell(grid, cell, &c)) continue;
            p = c.position;
        }
        if (wpe_nav_blockers_blocked(wpe_nav_system_blockers(nav), cell)) continue;
        wpe__nav_push_vector(L, p);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

static const luaL_Reg wpe__nav_api[] = {
    {"add_nav_agent", wpe__api_add_nav_agent},
    {"remove_nav_agent", wpe__api_remove_nav_agent},
    {"get_nav_agent", wpe__api_get_nav_agent},
    {"nav_move_to", wpe__api_nav_move_to},
    {"nav_follow", wpe__api_nav_follow},
    {"nav_stop", wpe__api_nav_stop},
    {"nav_state", wpe__api_nav_state},
    {"nav_path", wpe__api_nav_path},
    {"add_nav_obstacle", wpe__api_add_nav_obstacle},
    {"remove_nav_obstacle", wpe__api_remove_nav_obstacle},
    {"nav_find_path", wpe__api_nav_find_path},
    {"nav_nearest", wpe__api_nav_nearest},
    {"nav_can_walk", wpe__api_nav_can_walk},
    {"nav_reachable", wpe__api_nav_reachable},
    {"nav_random_point", wpe__api_nav_random_point},
    {NULL, NULL},
};

void wpe__script_nav_register(lua_State* L) {
    luaL_register(L, NULL, wpe__nav_api);
}
