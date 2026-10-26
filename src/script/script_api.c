/*
 * Luau bindings: the read-only global `wpe` library and `print`.
 *
 * Every binding follows the error-safety rule in script_internal.h: validate (may raise) first,
 * then act on the engine, then push results; never hold an owned engine allocation across a call
 * that can raise.
 */
#define WPE_LOG_MODULE "script"
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "core/log.h"
#include "graphics/graphics.h"
#include "scene/scene.h"
#include "script/script_internal.h"

/* ---- Argument helpers ------------------------------------------------------------------------------- */

static WPE_Entity wpe__check_handle(lua_State* L, int index) {
    const double value = luaL_checknumber(L, index);
    if (!(value >= 0.0 && value <= (double)UINT32_MAX && value == floor(value))) {
        luaL_errorL(L, "argument #%d is not an entity handle", index);
    }
    return (WPE_Entity)value;
}

/* Checks that the argument is a live entity; raises otherwise. */
static WPE_Entity wpe__check_entity(lua_State* L, int index) {
    const WPE_Entity entity = wpe__check_handle(L, index);
    if (!wpe_ecs_is_alive(wpe__script_host_from(L)->ecs, entity)) {
        luaL_errorL(L, "argument #%d: entity 0x%08x is not alive", index, entity);
    }
    return entity;
}

static WPE_Vec3 wpe__check_vec3(lua_State* L, int index) {
    const float* v = luaL_checkvector(L, index);
    return wpe_vec3(v[0], v[1], v[2]);
}

static void wpe__push_vec3(lua_State* L, WPE_Vec3 v) {
    lua_pushvector(L, v.x, v.y, v.z);
}

/* Raises if a structural change is attempted from inside on_destroy. */
static WPE_ScriptHost* wpe__check_unrestricted(lua_State* L, const char* function) {
    WPE_ScriptHost* host = wpe__script_host_from(L);
    if (host->restricted_depth > 0 || host->shutting_down)
        luaL_errorL(L, "wpe.%s is not allowed in on_destroy", function);
    return host;
}

/* Transform of an entity, adding an identity transform if missing (structural; checked by caller). */
static WPE_Transform* wpe__transform(lua_State* L, WPE_ScriptHost* host, WPE_Entity entity) {
    WPE_Transform* t = wpe_ecs_get_component(host->ecs, entity, WPE_COMP_TRANSFORM);
    if (!t) luaL_errorL(L, "entity 0x%08x has no Transform", entity);
    return t;
}

static void wpe__raise_result(lua_State* L, const char* function, WPE_Result result) {
    if (result != WPE_RESULT_OK) luaL_errorL(L, "wpe.%s failed: %s", function, wpe_result_to_string(result));
}

/* ---- Logging ---------------------------------------------------------------------------------------- */

static int wpe__api_log(lua_State* L) {
    char message[1024];
    size_t used = 0;
    message[0] = '\0';
    const int count = lua_gettop(L);
    for (int i = 1; i <= count; ++i) {
        size_t length = 0;
        const char* text = luaL_tolstring(L, i, &length); /* pushes */
        const int written =
            snprintf(message + used, sizeof message - used, "%s%.*s", i > 1 ? " " : "", (int)length, text);
        lua_pop(L, 1);
        if (written < 0) break;
        used = (size_t)written >= sizeof message - used ? sizeof message - 1 : used + (size_t)written;
    }
    lua_Debug ar;
    const bool located = lua_getinfo(L, 1, "sl", &ar) != 0;
    wpe_log_write(WPE_LOG_LEVEL_INFO, "script", located ? ar.short_src : "?", located ? ar.currentline : 0,
                  "%s", message);
    return 0;
}

/* ---- Entities and hierarchy ------------------------------------------------------------------------ */

static int wpe__api_spawn(lua_State* L) {
    WPE_ScriptHost* host = wpe__check_unrestricted(L, "spawn");
    const char* name = luaL_optstring(L, 1, NULL);
    const WPE_Entity entity = wpe_ecs_spawn(host->ecs);
    if (entity == WPE_ENTITY_NULL) luaL_errorL(L, "wpe.spawn failed: entity limit reached");
    const WPE_Transform t = wpe_transform_identity();
    WPE_Result result = wpe_ecs_add_component(host->ecs, entity, WPE_COMP_TRANSFORM, &t, NULL);
    if (result == WPE_RESULT_OK && name) result = wpe_scene_set_name(host->ecs, entity, name);
    if (result != WPE_RESULT_OK) {
        wpe_ecs_despawn(host->ecs, entity);
        wpe__raise_result(L, "spawn", result);
    }
    lua_pushnumber(L, (double)entity);
    return 1;
}

static int wpe__api_destroy(lua_State* L) {
    WPE_ScriptHost* host = wpe__script_host_from(L);
    const WPE_Entity entity = wpe__check_handle(L, 1);
    if (wpe_ecs_is_alive(host->ecs, entity))
        wpe__raise_result(L, "destroy", wpe__script_queue_destroy(host, entity));
    return 0;
}

static int wpe__api_is_alive(lua_State* L) {
    const WPE_Entity entity = wpe__check_handle(L, 1);
    lua_pushboolean(L, wpe_ecs_is_alive(wpe__script_host_from(L)->ecs, entity));
    return 1;
}

static int wpe__api_get_name(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    const char* name = wpe_scene_get_name(wpe__script_host_from(L)->ecs, entity);
    if (name) {
        lua_pushstring(L, name);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

static int wpe__api_set_name(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    const char* name = luaL_checkstring(L, 2);
    WPE_ScriptHost* host = wpe__script_host_from(L);
    if (!wpe_ecs_has_component(host->ecs, entity, WPE_COMP_NAME)) wpe__check_unrestricted(L, "set_name");
    wpe__raise_result(L, "set_name", wpe_scene_set_name(host->ecs, entity, name));
    return 0;
}

static int wpe__api_get_parent(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    const WPE_Entity parent = wpe_scene_get_parent(wpe__script_host_from(L)->ecs, entity);
    if (parent == WPE_ENTITY_NULL) {
        lua_pushnil(L);
    } else {
        lua_pushnumber(L, (double)parent);
    }
    return 1;
}

static int wpe__api_set_parent(lua_State* L) {
    WPE_ScriptHost* host = wpe__check_unrestricted(L, "set_parent");
    const WPE_Entity entity = wpe__check_entity(L, 1);
    const WPE_Entity parent = lua_isnoneornil(L, 2) ? WPE_ENTITY_NULL : wpe__check_entity(L, 2);
    const bool keep_world = lua_isnoneornil(L, 3) ? true : luaL_checkboolean(L, 3);
    wpe__raise_result(L, "set_parent", wpe_scene_set_parent(host->ecs, entity, parent, keep_world));
    return 0;
}

static int wpe__api_children(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    const WPE_Ecs* ecs = wpe__script_host_from(L)->ecs;
    lua_newtable(L);
    const WPE_Hierarchy* node = wpe_ecs_get_component(ecs, entity, WPE_COMP_HIERARCHY);
    int index = 1;
    for (WPE_Entity child = node ? node->first_child : WPE_ENTITY_NULL; child != WPE_ENTITY_NULL;) {
        lua_pushnumber(L, (double)child);
        lua_rawseti(L, -2, index++);
        const WPE_Hierarchy* child_node = wpe_ecs_get_component(ecs, child, WPE_COMP_HIERARCHY);
        child = child_node ? child_node->next_sibling : WPE_ENTITY_NULL;
    }
    return 1;
}

static int wpe__api_instantiate(lua_State* L) {
    WPE_ScriptHost* host = wpe__check_unrestricted(L, "instantiate");
    const char* name = luaL_checkstring(L, 1);
    const bool has_position = !lua_isnoneornil(L, 2);
    const WPE_Vec3 position = has_position ? wpe__check_vec3(L, 2) : wpe_vec3(0, 0, 0);
    const WPE_Model* model = wpe__script_find_prefab(host, name);
    if (!model) luaL_errorL(L, "wpe.instantiate: unknown prefab '%s'", name);
    WPE_Entity root = WPE_ENTITY_NULL;
    const WPE_InstantiateOptions options = {.animation = model->animation_count ? 0 : WPE_MODEL_NONE,
                                            .play = true};
    wpe__raise_result(L, "instantiate", wpe_model_instantiate(host->ecs, model, &options, &root));
    WPE_Transform* t = wpe_ecs_get_component(host->ecs, root, WPE_COMP_TRANSFORM);
    if (has_position) t->position = position;
    lua_pushnumber(L, (double)root);
    return 1;
}

static int wpe__api_attach_script(lua_State* L) {
    WPE_ScriptHost* host = wpe__check_unrestricted(L, "attach_script");
    const WPE_Entity entity = wpe__check_entity(L, 1);
    const char* path = luaL_checkstring(L, 2);
    lua_pushboolean(L, wpe_script_attach(host, entity, path) == WPE_RESULT_OK);
    return 1;
}

/* ---- Transform ------------------------------------------------------------------------------------- */

static int wpe__api_get_position(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    wpe__push_vec3(L, wpe__transform(L, wpe__script_host_from(L), entity)->position);
    return 1;
}

static int wpe__api_set_position(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    const WPE_Vec3 position = wpe__check_vec3(L, 2);
    wpe__transform(L, wpe__script_host_from(L), entity)->position = position;
    return 0;
}

static int wpe__api_get_rotation(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    wpe__push_vec3(L, wpe_quat_to_euler_xyz(wpe__transform(L, wpe__script_host_from(L), entity)->rotation));
    return 1;
}

static int wpe__api_set_rotation(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    const WPE_Vec3 euler = wpe__check_vec3(L, 2);
    wpe__transform(L, wpe__script_host_from(L), entity)->rotation = wpe_quat_from_euler_xyz(euler);
    return 0;
}

static int wpe__api_rotate(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    const WPE_Vec3 axis = wpe__check_vec3(L, 2);
    const float angle = (float)luaL_checknumber(L, 3);
    WPE_Transform* t = wpe__transform(L, wpe__script_host_from(L), entity);
    t->rotation = wpe_quat_normalize(wpe_quat_mul(wpe_quat_from_axis_angle(axis, angle), t->rotation));
    return 0;
}

static int wpe__api_get_scale(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    wpe__push_vec3(L, wpe__transform(L, wpe__script_host_from(L), entity)->scale);
    return 1;
}

static int wpe__api_set_scale(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    WPE_Vec3 scale;
    if (lua_isnumber(L, 2)) {
        const float s = (float)lua_tonumber(L, 2);
        scale = wpe_vec3(s, s, s);
    } else {
        scale = wpe__check_vec3(L, 2);
    }
    wpe__transform(L, wpe__script_host_from(L), entity)->scale = scale;
    return 0;
}

static int wpe__api_get_world_position(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    WPE_Mat4 world;
    wpe_scene_compute_world_matrix(wpe__script_host_from(L)->ecs, entity, &world);
    wpe__push_vec3(L, wpe_vec3(world.m[3][0], world.m[3][1], world.m[3][2]));
    return 1;
}

static int wpe__api_forward(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    WPE_Mat4 world;
    wpe_scene_compute_world_matrix(wpe__script_host_from(L)->ecs, entity, &world);
    wpe__push_vec3(L, wpe_vec3_normalize(wpe_mat4_transform_dir(&world, wpe_vec3(0, 0, -1))));
    return 1;
}

static int wpe__api_look_at(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    const WPE_Vec3 target = wpe__check_vec3(L, 2);
    const WPE_Vec3 up = lua_isnoneornil(L, 3) ? wpe_vec3(0, 1, 0) : wpe__check_vec3(L, 3);
    WPE_ScriptHost* host = wpe__script_host_from(L);
    WPE_Transform* t = wpe__transform(L, host, entity);

    WPE_Mat4 world;
    wpe_scene_compute_world_matrix(host->ecs, entity, &world);
    const WPE_Vec3 eye = wpe_vec3(world.m[3][0], world.m[3][1], world.m[3][2]);
    if (wpe_vec3_length_sq(wpe_vec3_sub(target, eye)) < 1e-12f)
        return 0; /* Already there: keep the rotation. */
    const WPE_Mat4 view = wpe_mat4_look_at(eye, target, up);
    WPE_Mat4 desired;
    WPE_Vec3 p, s;
    WPE_Quat world_rotation;
    if (!wpe_mat4_inverse(&view, &desired) || !wpe_mat4_decompose(&desired, &p, &world_rotation, &s))
        return 0;

    /* Express the world rotation relative to the parent's world rotation. */
    WPE_Quat parent_rotation = wpe_quat_identity();
    const WPE_Entity parent = wpe_scene_get_parent(host->ecs, entity);
    WPE_Mat4 parent_world;
    if (parent != WPE_ENTITY_NULL && wpe_scene_compute_world_matrix(host->ecs, parent, &parent_world)) {
        WPE_Vec3 pp, ps;
        if (!wpe_mat4_decompose(&parent_world, &pp, &parent_rotation, &ps))
            parent_rotation = wpe_quat_identity();
    }
    const WPE_Quat inverse_parent = {
        .x = -parent_rotation.x, .y = -parent_rotation.y, .z = -parent_rotation.z, .w = parent_rotation.w};
    t->rotation = wpe_quat_normalize(wpe_quat_mul(inverse_parent, world_rotation));
    return 0;
}

/* ---- Lights ---------------------------------------------------------------------------------------- */

static float wpe__field_number(lua_State* L, int table, const char* key, float fallback) {
    lua_getfield(L, table, key);
    const float value = lua_isnil(L, -1) ? fallback : (float)luaL_checknumber(L, -1);
    lua_pop(L, 1);
    return value;
}

/* Reads a light description table (missing fields keep `base` values). Raises on bad field types. */
static WPE_Light wpe__check_light(lua_State* L, int table, WPE_Light base) {
    luaL_checktype(L, table, LUA_TTABLE);
    const float type = wpe__field_number(L, table, "type", (float)base.type);
    if (!(type == WPE_LIGHT_DIRECTIONAL || type == WPE_LIGHT_POINT || type == WPE_LIGHT_SPOT)) {
        luaL_errorL(L, "light type must be wpe.LIGHT_DIRECTIONAL, wpe.LIGHT_POINT or wpe.LIGHT_SPOT");
    }
    base.type = (WPE_LightType)type;
    lua_getfield(L, table, "color");
    if (!lua_isnil(L, -1)) base.color = wpe__check_vec3(L, -1);
    lua_pop(L, 1);
    base.intensity = wpe__field_number(L, table, "intensity", base.intensity);
    base.range = wpe__field_number(L, table, "range", base.range);
    base.inner_cone = wpe__field_number(L, table, "inner_cone", base.inner_cone);
    base.outer_cone = wpe__field_number(L, table, "outer_cone", base.outer_cone);
    lua_getfield(L, table, "shadows");
    if (!lua_isnil(L, -1)) base.cast_shadows = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return base;
}

static int wpe__api_set_light(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    WPE_ScriptHost* host = wpe__script_host_from(L);
    WPE_Light* existing = wpe_ecs_get_component(host->ecs, entity, WPE_COMP_LIGHT);
    const WPE_Light light = wpe__check_light(L, 2, existing ? *existing : wpe_light_default(WPE_LIGHT_POINT));
    existing = wpe_ecs_get_component(host->ecs, entity, WPE_COMP_LIGHT); /* Re-fetch after raising calls. */
    if (existing) {
        *existing = light;
    } else {
        wpe__check_unrestricted(L, "set_light");
        wpe__raise_result(L, "set_light",
                          wpe_ecs_add_component(host->ecs, entity, WPE_COMP_LIGHT, &light, NULL));
    }
    return 0;
}

static int wpe__api_get_light(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    const WPE_Light* found = wpe_ecs_get_component(wpe__script_host_from(L)->ecs, entity, WPE_COMP_LIGHT);
    if (!found) {
        lua_pushnil(L);
        return 1;
    }
    const WPE_Light light = *found; /* Copy before pushing: pushes may run the GC but not move ECS data. */
    lua_createtable(L, 0, 7);
    lua_pushnumber(L, light.type);
    lua_setfield(L, -2, "type");
    wpe__push_vec3(L, light.color);
    lua_setfield(L, -2, "color");
    lua_pushnumber(L, light.intensity);
    lua_setfield(L, -2, "intensity");
    lua_pushnumber(L, light.range);
    lua_setfield(L, -2, "range");
    lua_pushnumber(L, light.inner_cone);
    lua_setfield(L, -2, "inner_cone");
    lua_pushnumber(L, light.outer_cone);
    lua_setfield(L, -2, "outer_cone");
    lua_pushboolean(L, light.cast_shadows);
    lua_setfield(L, -2, "shadows");
    return 1;
}

static int wpe__api_remove_light(lua_State* L) {
    WPE_ScriptHost* host = wpe__check_unrestricted(L, "remove_light");
    const WPE_Entity entity = wpe__check_entity(L, 1);
    if (wpe_ecs_has_component(host->ecs, entity, WPE_COMP_LIGHT)) {
        wpe__raise_result(L, "remove_light", wpe_ecs_remove_component(host->ecs, entity, WPE_COMP_LIGHT));
    }
    return 0;
}

/* ---- Rendering and animation ----------------------------------------------------------------------- */

static int wpe__api_set_visible(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    const bool visible = luaL_checkboolean(L, 2);
    WPE_ScriptHost* host = wpe__script_host_from(L);
    /* Applies to the entity and all mesh renderers below it (instantiated models are trees). */
    WPE_Entity stack[256];
    uint32_t top = 0;
    stack[top++] = entity;
    while (top > 0) {
        const WPE_Entity e = stack[--top];
        WPE_MeshRenderer* mesh = wpe_ecs_get_component(host->ecs, e, WPE_COMP_MESH);
        if (mesh) mesh->visible = visible;
        const WPE_Hierarchy* node = wpe_ecs_get_component(host->ecs, e, WPE_COMP_HIERARCHY);
        for (WPE_Entity c = node ? node->first_child : WPE_ENTITY_NULL; c != WPE_ENTITY_NULL && top < 256;) {
            stack[top++] = c;
            const WPE_Hierarchy* child = wpe_ecs_get_component(host->ecs, c, WPE_COMP_HIERARCHY);
            c = child ? child->next_sibling : WPE_ENTITY_NULL;
        }
    }
    return 0;
}

static WPE_Animator* wpe__check_animator(lua_State* L, WPE_Entity entity) {
    WPE_Animator* animator = wpe_ecs_get_component(wpe__script_host_from(L)->ecs, entity, WPE_COMP_ANIMATOR);
    if (!animator) luaL_errorL(L, "entity 0x%08x has no Animator (instantiate an animated model)", entity);
    return animator;
}

static int wpe__api_play_animation(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    const int index = (int)luaL_optinteger(L, 2, 0); /* 1-based; 0 / nil keeps the current animation */
    const bool loop = lua_isnoneornil(L, 3) ? true : luaL_checkboolean(L, 3);
    WPE_Animator* animator = wpe__check_animator(L, entity);
    if (index != 0) {
        if (index < 1 || (uint32_t)index > animator->model->animation_count) {
            luaL_errorL(L, "animation index %d out of range (model has %u)", index,
                        animator->model->animation_count);
        }
        animator->animation = (uint32_t)(index - 1);
        animator->time = 0.0f;
    }
    animator->loop = loop;
    animator->playing = true;
    return 0;
}

static int wpe__api_stop_animation(lua_State* L) {
    wpe__check_animator(L, wpe__check_entity(L, 1))->playing = false;
    return 0;
}

static int wpe__api_animation_time(lua_State* L) {
    WPE_Animator* animator = wpe__check_animator(L, wpe__check_entity(L, 1));
    if (!lua_isnoneornil(L, 2)) animator->time = (float)luaL_checknumber(L, 2);
    lua_pushnumber(L, animator->time);
    return 1;
}

/* ---- Time ------------------------------------------------------------------------------------------ */

static int wpe__api_time(lua_State* L) {
    lua_pushnumber(L, wpe__script_host_from(L)->time);
    return 1;
}

static int wpe__api_frame(lua_State* L) {
    lua_pushnumber(L, (double)wpe__script_host_from(L)->frame);
    return 1;
}

/* ---- Registration ---------------------------------------------------------------------------------- */

static const luaL_Reg wpe__api[] = {
    {"log", wpe__api_log},
    {"spawn", wpe__api_spawn},
    {"destroy", wpe__api_destroy},
    {"is_alive", wpe__api_is_alive},
    {"get_name", wpe__api_get_name},
    {"set_name", wpe__api_set_name},
    {"get_parent", wpe__api_get_parent},
    {"set_parent", wpe__api_set_parent},
    {"children", wpe__api_children},
    {"instantiate", wpe__api_instantiate},
    {"attach_script", wpe__api_attach_script},
    {"get_position", wpe__api_get_position},
    {"set_position", wpe__api_set_position},
    {"get_rotation", wpe__api_get_rotation},
    {"set_rotation", wpe__api_set_rotation},
    {"rotate", wpe__api_rotate},
    {"get_scale", wpe__api_get_scale},
    {"set_scale", wpe__api_set_scale},
    {"get_world_position", wpe__api_get_world_position},
    {"forward", wpe__api_forward},
    {"look_at", wpe__api_look_at},
    {"set_light", wpe__api_set_light},
    {"get_light", wpe__api_get_light},
    {"remove_light", wpe__api_remove_light},
    {"set_visible", wpe__api_set_visible},
    {"play_animation", wpe__api_play_animation},
    {"stop_animation", wpe__api_stop_animation},
    {"animation_time", wpe__api_animation_time},
    {"time", wpe__api_time},
    {"frame", wpe__api_frame},
    {NULL, NULL},
};

void wpe__script_api_register(lua_State* L) {
    lua_newtable(L);
    luaL_register(L, NULL, wpe__api);
    lua_pushnumber(L, WPE_LIGHT_DIRECTIONAL);
    lua_setfield(L, -2, "LIGHT_DIRECTIONAL");
    lua_pushnumber(L, WPE_LIGHT_POINT);
    lua_setfield(L, -2, "LIGHT_POINT");
    lua_pushnumber(L, WPE_LIGHT_SPOT);
    lua_setfield(L, -2, "LIGHT_SPOT");
    lua_setglobal(L, "wpe");

    lua_pushcfunction(L, wpe__api_log, "print");
    lua_setglobal(L, "print");
}
