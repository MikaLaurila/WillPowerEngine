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

#include "core/alloc.h"
#include "core/json.h"
#include "core/log.h"
#include "graphics/animation.h"
#include "graphics/graphics.h"
#include "graphics/scene_render.h"
#include "platform/platform.h"
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

/* Name lookup scans every name: meant for on_start (keep the handle), not for every frame. */
static int wpe__api_find(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    WPE_EcsPoolView view;
    if (wpe_ecs_pool_view(wpe__script_host_from(L)->ecs, WPE_COMP_NAME, &view) == WPE_RESULT_OK) {
        for (uint32_t i = 0; i < view.count; ++i) {
            const WPE_Name* n = (const WPE_Name*)((const unsigned char*)view.data + (size_t)i * view.stride);
            if (strcmp(n->value, name) == 0) {
                lua_pushnumber(L, (double)view.entities[i]);
                return 1;
            }
        }
    }
    lua_pushnil(L);
    return 1;
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
    WPE_Entity root = WPE_ENTITY_NULL;
    if (host->app.instantiate) {
        /* Prefab files belong to the application; anything else is a model. */
        const WPE_Result result = host->app.instantiate(host->app.user_data, name, &root);
        if (result != WPE_RESULT_ERROR_NOT_FOUND) {
            wpe__raise_result(L, "instantiate", result);
            WPE_Transform* t = wpe_ecs_get_component(host->ecs, root, WPE_COMP_TRANSFORM);
            if (has_position && t) t->position = position;
            lua_pushnumber(L, (double)root);
            return 1;
        }
    }
    const WPE_AssetHandle model = wpe__script_find_prefab(host, name);
    if (model == WPE_ASSET_NONE) luaL_errorL(L, "wpe.instantiate: unknown prefab '%s'", name);
    const WPE_InstantiateOptions options = {
        .animation = wpe_asset_model(host->assets, model)->animation_count ? 0 : WPE_MODEL_NONE,
        .play = true};
    wpe__raise_result(L, "instantiate",
                      wpe_model_instantiate(host->ecs, host->assets, model, &options, &root));
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
    const bool loop = lua_isnoneornil(L, 3) ? true : luaL_checkboolean(L, 3);
    const double fade = luaL_optnumber(L, 4, 0.0);
    if (!(fade >= 0.0 && isfinite(fade)))
        luaL_errorL(L, "wpe.play_animation: fade must be 0 or more seconds");
    WPE_Animator* animator = wpe__check_animator(L, entity);
    if (lua_isnoneornil(L, 2)) { /* Resume the current animation. */
        animator->loop = loop;
        animator->playing = true;
        return 0;
    }
    uint32_t index = WPE_MODEL_NONE;
    if (lua_type(L, 2) == LUA_TSTRING) {
        index = wpe_model_find_animation(animator->model, lua_tostring(L, 2));
        if (index == WPE_MODEL_NONE)
            luaL_errorL(L, "wpe.play_animation: the model has no animation '%s'", lua_tostring(L, 2));
    } else {
        const int number = (int)luaL_checkinteger(L, 2); /* 1-based */
        if (number < 1 || (uint32_t)number > animator->model->animation_count) {
            luaL_errorL(L, "animation index %d out of range (model has %u)", number,
                        animator->model->animation_count);
        }
        index = (uint32_t)(number - 1);
    }
    wpe__raise_result(L, "play_animation", wpe_animator_play(animator, index, (float)fade, loop));
    return 0;
}

static int wpe__api_get_animation(lua_State* L) {
    const WPE_Animator* animator = wpe__check_animator(L, wpe__check_entity(L, 1));
    const WPE_Animator copy = *animator;
    const bool valid = copy.animation < copy.model->animation_count;
    lua_createtable(L, 0, 8);
    lua_pushnumber(L, valid ? (double)copy.animation + 1.0 : 0.0);
    lua_setfield(L, -2, "index");
    lua_pushstring(L, valid ? copy.model->animations[copy.animation].name : "");
    lua_setfield(L, -2, "name");
    lua_pushnumber(L, copy.time);
    lua_setfield(L, -2, "time");
    lua_pushnumber(L, valid ? copy.model->animations[copy.animation].duration : 0.0);
    lua_setfield(L, -2, "duration");
    lua_pushnumber(L, copy.speed);
    lua_setfield(L, -2, "speed");
    lua_pushboolean(L, copy.playing);
    lua_setfield(L, -2, "playing");
    lua_pushboolean(L, copy.loop);
    lua_setfield(L, -2, "loop");
    lua_pushboolean(L, copy.fade_duration > 0.0f);
    lua_setfield(L, -2, "fading");
    lua_createtable(L, (int)copy.model->animation_count, 0);
    for (uint32_t i = 0; i < copy.model->animation_count; ++i) {
        lua_pushstring(L, copy.model->animations[i].name);
        lua_rawseti(L, -2, (int)i + 1);
    }
    lua_setfield(L, -2, "names");
    return 1;
}

static int wpe__api_animation_speed(lua_State* L) {
    WPE_Animator* animator = wpe__check_animator(L, wpe__check_entity(L, 1));
    if (!lua_isnoneornil(L, 2)) {
        const double speed = luaL_checknumber(L, 2);
        if (!isfinite(speed)) luaL_errorL(L, "wpe.animation_speed: speed must be finite");
        animator->speed = (float)speed;
    }
    lua_pushnumber(L, animator->speed);
    return 1;
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

/* ---- Physics --------------------------------------------------------------------------------------- */

static WPE_Physics* wpe__physics(lua_State* L) {
    WPE_Physics* physics = wpe__script_host_from(L)->physics;
    if (!physics) luaL_errorL(L, "physics is not enabled for this world");
    return physics;
}

static bool wpe__field_bool(lua_State* L, int table, const char* key, bool fallback) {
    lua_getfield(L, table, key);
    const bool value = lua_isnil(L, -1) ? fallback : luaL_checkboolean(L, -1);
    lua_pop(L, 1);
    return value;
}

/* Raises with a readable message for the body-control error codes. */
static void wpe__raise_body_result(lua_State* L, const char* function, WPE_Entity entity, WPE_Result result) {
    if (result == WPE_RESULT_ERROR_NOT_FOUND)
        luaL_errorL(L, "wpe.%s: entity 0x%08x has no rigid body", function, entity);
    if (result == WPE_RESULT_ERROR_INVALID_STATE) {
        luaL_errorL(L, "wpe.%s: not supported by the body type of entity 0x%08x", function, entity);
    }
    wpe__raise_result(L, function, result);
}

static int wpe__api_add_rigidbody(lua_State* L) {
    WPE_ScriptHost* host = wpe__check_unrestricted(L, "add_rigidbody");
    WPE_Physics* physics = wpe__physics(L);
    const WPE_Entity entity = wpe__check_entity(L, 1);
    const WPE_RigidBody* existing = wpe_ecs_get_component(host->ecs, entity, WPE_COMP_PHYSICS_RIGIDBODY);
    WPE_RigidBody body = existing ? *existing : wpe_rigidbody_default(WPE_BODY_DYNAMIC);
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        const float type = wpe__field_number(L, 2, "type", (float)body.type);
        if (!(type == WPE_BODY_STATIC || type == WPE_BODY_KINEMATIC || type == WPE_BODY_DYNAMIC)) {
            luaL_errorL(L, "rigid body type must be wpe.BODY_STATIC, wpe.BODY_KINEMATIC or wpe.BODY_DYNAMIC");
        }
        body.type = (WPE_BodyType)type;
        body.linear_damping = wpe__field_number(L, 2, "linear_damping", body.linear_damping);
        body.angular_damping = wpe__field_number(L, 2, "angular_damping", body.angular_damping);
        body.gravity_scale = wpe__field_number(L, 2, "gravity_scale", body.gravity_scale);
        body.lock_rotation = wpe__field_bool(L, 2, "lock_rotation", body.lock_rotation);
        body.continuous = wpe__field_bool(L, 2, "continuous", body.continuous);
        lua_getfield(L, 2, "velocity");
        if (!lua_isnil(L, -1)) body.initial_velocity = wpe__check_vec3(L, -1);
        lua_pop(L, 1);
    }
    /* Re-fetch after raising calls; private fields of an existing body are carried over unchanged. */
    WPE_RigidBody* stored = wpe_ecs_get_component(host->ecs, entity, WPE_COMP_PHYSICS_RIGIDBODY);
    if (stored) {
        *stored = body;
    } else {
        wpe__raise_result(L, "add_rigidbody",
                          wpe_ecs_add_component(host->ecs, entity, WPE_COMP_PHYSICS_RIGIDBODY, &body, NULL));
    }
    /* Create the body now so velocities and forces work immediately. */
    wpe__raise_result(L, "add_rigidbody", wpe_physics_refresh(physics, entity));
    return 0;
}

static int wpe__api_get_rigidbody(lua_State* L) {
    wpe__physics(L);
    const WPE_Entity entity = wpe__check_entity(L, 1);
    const WPE_RigidBody* found =
        wpe_ecs_get_component(wpe__script_host_from(L)->ecs, entity, WPE_COMP_PHYSICS_RIGIDBODY);
    if (!found) {
        lua_pushnil(L);
        return 1;
    }
    const WPE_RigidBody body = *found; /* Copy before pushing: pushes may run the GC but not move ECS data. */
    lua_createtable(L, 0, 6);
    lua_pushnumber(L, body.type);
    lua_setfield(L, -2, "type");
    lua_pushnumber(L, body.linear_damping);
    lua_setfield(L, -2, "linear_damping");
    lua_pushnumber(L, body.angular_damping);
    lua_setfield(L, -2, "angular_damping");
    lua_pushnumber(L, body.gravity_scale);
    lua_setfield(L, -2, "gravity_scale");
    lua_pushboolean(L, body.lock_rotation);
    lua_setfield(L, -2, "lock_rotation");
    lua_pushboolean(L, body.continuous);
    lua_setfield(L, -2, "continuous");
    return 1;
}

static int wpe__api_add_collider(lua_State* L) {
    WPE_ScriptHost* host = wpe__check_unrestricted(L, "add_collider");
    WPE_Physics* physics = wpe__physics(L);
    const WPE_Entity entity = wpe__check_entity(L, 1);
    const WPE_Collider* existing = wpe_ecs_get_component(host->ecs, entity, WPE_COMP_PHYSICS_COLLIDER);
    WPE_Collider collider = existing ? *existing : wpe_collider_default(WPE_COLLIDER_BOX);
    char model_name[WPE__SCRIPT_PATH_MAX] = "";
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        const float shape = wpe__field_number(L, 2, "shape", (float)collider.shape);
        if (!(shape >= WPE_COLLIDER_BOX && shape <= WPE_COLLIDER_MESH && shape == floorf(shape))) {
            luaL_errorL(L, "collider shape must be one of the wpe.COLLIDER_* constants");
        }
        collider.shape = (WPE_ColliderShape)shape;
        lua_getfield(L, 2, "size");
        if (!lua_isnil(L, -1)) collider.size = wpe__check_vec3(L, -1);
        lua_pop(L, 1);
        collider.size.x = wpe__field_number(L, 2, "radius", collider.size.x);
        collider.size.y = wpe__field_number(L, 2, "height", collider.size.y);
        lua_getfield(L, 2, "offset");
        if (!lua_isnil(L, -1)) collider.offset = wpe__check_vec3(L, -1);
        lua_pop(L, 1);
        collider.density = wpe__field_number(L, 2, "density", collider.density);
        collider.friction = wpe__field_number(L, 2, "friction", collider.friction);
        collider.restitution = wpe__field_number(L, 2, "restitution", collider.restitution);
        collider.is_trigger = wpe__field_bool(L, 2, "trigger", collider.is_trigger);
        const float mesh = wpe__field_number(L, 2, "mesh", (float)collider.mesh + 1.0f); /* 1-based */
        if (!(mesh >= 1.0f && mesh == floorf(mesh))) luaL_errorL(L, "collider mesh index must be >= 1");
        collider.mesh = (uint32_t)mesh - 1;
        lua_getfield(L, 2, "model");
        if (!lua_isnil(L, -1)) snprintf(model_name, sizeof model_name, "%s", luaL_checkstring(L, -1));
        lua_pop(L, 1);
    }
    if (model_name[0] != '\0') {
        collider.model = wpe__script_find_prefab(host, model_name);
        if (collider.model == WPE_ASSET_NONE)
            luaL_errorL(L, "wpe.add_collider: unknown model '%s'", model_name);
    }
    const bool uses_model = collider.shape == WPE_COLLIDER_CONVEX || collider.shape == WPE_COLLIDER_MESH;
    /* Without a model, the entity's own mesh gives the shape. */
    const WPE_MeshRenderer* renderer = wpe_ecs_get_component(host->ecs, entity, WPE_COMP_MESH);
    if (uses_model && !wpe_asset_model(host->assets, collider.model) && renderer && renderer->model) {
        collider.model = renderer->model_asset;
        collider.mesh = renderer->mesh;
    }
    if (uses_model && !wpe_asset_model(host->assets, collider.model)) {
        luaL_errorL(L, "wpe.add_collider: convex and mesh colliders need a model (or a mesh on the entity)");
    }
    WPE_Collider* stored = wpe_ecs_get_component(host->ecs, entity, WPE_COMP_PHYSICS_COLLIDER);
    if (stored) {
        *stored = collider;
    } else {
        wpe__raise_result(
            L, "add_collider",
            wpe_ecs_add_component(host->ecs, entity, WPE_COMP_PHYSICS_COLLIDER, &collider, NULL));
    }
    wpe__raise_result(L, "add_collider", wpe_physics_refresh(physics, entity));
    return 0;
}

static int wpe__api_remove_physics(lua_State* L, const char* function, WPE_ComponentType type) {
    WPE_ScriptHost* host = wpe__check_unrestricted(L, function);
    wpe__physics(L);
    const WPE_Entity entity = wpe__check_entity(L, 1);
    if (wpe_ecs_has_component(host->ecs, entity, type))
        wpe__raise_result(L, function, wpe_ecs_remove_component(host->ecs, entity, type));
    return 0;
}

static int wpe__api_remove_rigidbody(lua_State* L) {
    return wpe__api_remove_physics(L, "remove_rigidbody", WPE_COMP_PHYSICS_RIGIDBODY);
}

static int wpe__api_remove_collider(lua_State* L) {
    return wpe__api_remove_physics(L, "remove_collider", WPE_COMP_PHYSICS_COLLIDER);
}

static int wpe__api_apply_force(lua_State* L) {
    WPE_Physics* physics = wpe__physics(L);
    const WPE_Entity entity = wpe__check_entity(L, 1);
    wpe__raise_body_result(L, "apply_force", entity,
                           wpe_physics_apply_force(physics, entity, wpe__check_vec3(L, 2)));
    return 0;
}

static int wpe__api_apply_impulse(lua_State* L) {
    WPE_Physics* physics = wpe__physics(L);
    const WPE_Entity entity = wpe__check_entity(L, 1);
    wpe__raise_body_result(L, "apply_impulse", entity,
                           wpe_physics_apply_impulse(physics, entity, wpe__check_vec3(L, 2)));
    return 0;
}

static int wpe__api_set_velocity(lua_State* L) {
    WPE_Physics* physics = wpe__physics(L);
    const WPE_Entity entity = wpe__check_entity(L, 1);
    wpe__raise_body_result(L, "set_velocity", entity,
                           wpe_physics_set_velocity(physics, entity, wpe__check_vec3(L, 2)));
    return 0;
}

static int wpe__api_get_velocity(lua_State* L) {
    WPE_Physics* physics = wpe__physics(L);
    const WPE_Entity entity = wpe__check_entity(L, 1);
    WPE_Vec3 velocity;
    wpe__raise_body_result(L, "get_velocity", entity, wpe_physics_get_velocity(physics, entity, &velocity));
    wpe__push_vec3(L, velocity);
    return 1;
}

/* Optional query options {ignore = entity, triggers = bool} at `index`. */
static WPE_QueryFilter wpe__check_query_filter(lua_State* L, int index) {
    WPE_QueryFilter filter = {.ignore = WPE_ENTITY_NULL};
    if (lua_isnoneornil(L, index)) return filter;
    luaL_checktype(L, index, LUA_TTABLE);
    lua_getfield(L, index, "ignore");
    if (!lua_isnil(L, -1)) filter.ignore = wpe__check_handle(L, -1);
    lua_pop(L, 1);
    filter.include_triggers = wpe__field_bool(L, index, "triggers", false);
    return filter;
}

/* A query shape {shape = wpe.COLLIDER_BOX|SPHERE|CAPSULE, size, radius, height, rotation (Euler)}. */
static WPE_QueryShape wpe__check_query_shape(lua_State* L, int index) {
    luaL_checktype(L, index, LUA_TTABLE);
    WPE_QueryShape shape = {
        .shape = WPE_COLLIDER_SPHERE, .size = wpe_vec3(0.5f, 1.0f, 0.5f), .rotation = wpe_quat_identity()};
    const float type = wpe__field_number(L, index, "shape", (float)shape.shape);
    if (!(type == WPE_COLLIDER_BOX || type == WPE_COLLIDER_SPHERE || type == WPE_COLLIDER_CAPSULE))
        luaL_errorL(L, "query shape must be wpe.COLLIDER_BOX, wpe.COLLIDER_SPHERE or wpe.COLLIDER_CAPSULE");
    shape.shape = (WPE_ColliderShape)type;
    if (shape.shape == WPE_COLLIDER_BOX) shape.size = wpe_vec3(1.0f, 1.0f, 1.0f);
    lua_getfield(L, index, "size");
    if (!lua_isnil(L, -1)) shape.size = wpe__check_vec3(L, -1);
    lua_pop(L, 1);
    shape.size.x = wpe__field_number(L, index, "radius", shape.size.x);
    shape.size.y = wpe__field_number(L, index, "height", shape.size.y);
    lua_getfield(L, index, "rotation");
    if (!lua_isnil(L, -1)) shape.rotation = wpe_quat_from_euler_xyz(wpe__check_vec3(L, -1));
    lua_pop(L, 1);
    const bool positive = shape.shape == WPE_COLLIDER_BOX
                              ? shape.size.x > 0.0f && shape.size.y > 0.0f && shape.size.z > 0.0f
                              : shape.size.x > 0.0f;
    if (!positive || !(isfinite(shape.size.x) && isfinite(shape.size.y) && isfinite(shape.size.z)))
        luaL_errorL(L, "query shape needs a positive size (box) or radius (sphere, capsule)");
    return shape;
}

static void wpe__push_hit(lua_State* L, const WPE_RaycastHit* hit) {
    lua_createtable(L, 0, 4);
    lua_pushnumber(L, (double)hit->entity);
    lua_setfield(L, -2, "entity");
    wpe__push_vec3(L, hit->point);
    lua_setfield(L, -2, "point");
    wpe__push_vec3(L, hit->normal);
    lua_setfield(L, -2, "normal");
    lua_pushnumber(L, hit->distance);
    lua_setfield(L, -2, "distance");
}

static int wpe__api_shape_cast(lua_State* L) {
    WPE_Physics* physics = wpe__physics(L);
    const WPE_QueryShape shape = wpe__check_query_shape(L, 1);
    const WPE_Vec3 origin = wpe__check_vec3(L, 2);
    const WPE_Vec3 direction = wpe__check_vec3(L, 3);
    const float max_distance = (float)luaL_optnumber(L, 4, 1000.0);
    const WPE_QueryFilter filter = wpe__check_query_filter(L, 5);
    WPE_RaycastHit hit;
    if (!wpe_physics_shape_cast(physics, &shape, origin, direction, max_distance, &filter, &hit)) {
        lua_pushnil(L);
        return 1;
    }
    wpe__push_hit(L, &hit);
    return 1;
}

static int wpe__api_overlap(lua_State* L) {
    WPE_Physics* physics = wpe__physics(L);
    const WPE_QueryShape shape = wpe__check_query_shape(L, 1);
    const WPE_Vec3 center = wpe__check_vec3(L, 2);
    const WPE_QueryFilter filter = wpe__check_query_filter(L, 3);
    WPE_Entity entities[64];
    uint32_t count = wpe_physics_overlap(physics, &shape, center, &filter, entities, 64);
    if (count > 64) count = 64; /* NOTE(limitation): at most 64 entities are listed. */
    lua_createtable(L, (int)count, 0);
    for (uint32_t i = 0; i < count; ++i) {
        lua_pushnumber(L, (double)entities[i]);
        lua_rawseti(L, -2, (int)i + 1);
    }
    return 1;
}

static int wpe__api_add_character(lua_State* L) {
    WPE_ScriptHost* host = wpe__check_unrestricted(L, "add_character");
    WPE_Physics* physics = wpe__physics(L);
    const WPE_Entity entity = wpe__check_entity(L, 1);
    const WPE_CharacterController* existing = wpe_ecs_get_component(host->ecs, entity, WPE_COMP_CHARACTER);
    WPE_CharacterController c = existing ? *existing : wpe_character_default();
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        c.radius = wpe__field_number(L, 2, "radius", c.radius);
        c.height = wpe__field_number(L, 2, "height", c.height);
        lua_getfield(L, 2, "offset");
        if (!lua_isnil(L, -1)) c.offset = wpe__check_vec3(L, -1);
        lua_pop(L, 1);
        c.max_slope = wpe__field_number(L, 2, "max_slope", c.max_slope);
        c.step_height = wpe__field_number(L, 2, "step_height", c.step_height);
        c.snap_distance = wpe__field_number(L, 2, "snap_distance", c.snap_distance);
    }
    if (!(c.radius > 0.0f && c.height > 0.0f && c.max_slope >= 0.0f && c.max_slope <= 90.0f &&
          c.step_height >= 0.0f && c.snap_distance >= 0.0f))
        luaL_errorL(L, "wpe.add_character: radius and height must be positive, max_slope 0..90, and "
                       "step_height and snap_distance >= 0");
    if (!wpe_ecs_has_component(host->ecs, entity, WPE_COMP_TRANSFORM))
        luaL_errorL(L, "wpe.add_character: entity 0x%08x has no Transform", entity);
    WPE_CharacterController* stored = wpe_ecs_get_component(host->ecs, entity, WPE_COMP_CHARACTER);
    if (stored) {
        *stored = c;
    } else {
        wpe__raise_result(L, "add_character",
                          wpe_ecs_add_component(host->ecs, entity, WPE_COMP_CHARACTER, &c, NULL));
    }
    wpe__raise_result(L, "add_character", wpe_physics_refresh(physics, entity));
    return 0;
}

static int wpe__api_remove_character(lua_State* L) {
    return wpe__api_remove_physics(L, "remove_character", WPE_COMP_CHARACTER);
}

static const WPE_CharacterController* wpe__check_character(lua_State* L, const char* function,
                                                           WPE_Entity entity) {
    const WPE_CharacterController* c =
        wpe_ecs_get_component(wpe__script_host_from(L)->ecs, entity, WPE_COMP_CHARACTER);
    if (!c) luaL_errorL(L, "wpe.%s: entity 0x%08x has no character controller", function, entity);
    return c;
}

static int wpe__api_move_character(lua_State* L) {
    WPE_Physics* physics = wpe__physics(L);
    const WPE_Entity entity = wpe__check_entity(L, 1);
    const WPE_Vec3 velocity = wpe__check_vec3(L, 2);
    const double dt = luaL_checknumber(L, 3);
    wpe__check_character(L, "move_character", entity);
    if (!(dt > 0.0 && dt <= 1.0) || !(isfinite(velocity.x) && isfinite(velocity.y) && isfinite(velocity.z)))
        luaL_errorL(L, "wpe.move_character: velocity must be finite and dt in (0, 1]");
    WPE_Vec3 clipped;
    wpe__raise_result(L, "move_character",
                      wpe_character_move(physics, entity, velocity, (float)dt, &clipped));
    wpe__push_vec3(L, clipped);
    lua_pushboolean(L, wpe__check_character(L, "move_character", entity)->grounded);
    return 2;
}

static int wpe__api_get_character(lua_State* L) {
    wpe__physics(L);
    const WPE_Entity entity = wpe__check_entity(L, 1);
    const WPE_CharacterController copy = *wpe__check_character(L, "get_character", entity);
    lua_createtable(L, 0, 8);
    lua_pushboolean(L, copy.grounded);
    lua_setfield(L, -2, "grounded");
    wpe__push_vec3(L, copy.ground_normal);
    lua_setfield(L, -2, "ground_normal");
    if (copy.ground != WPE_ENTITY_NULL) {
        lua_pushnumber(L, (double)copy.ground);
        lua_setfield(L, -2, "ground");
    }
    lua_pushnumber(L, copy.radius);
    lua_setfield(L, -2, "radius");
    lua_pushnumber(L, copy.height);
    lua_setfield(L, -2, "height");
    wpe__push_vec3(L, copy.offset);
    lua_setfield(L, -2, "offset");
    lua_pushnumber(L, copy.max_slope);
    lua_setfield(L, -2, "max_slope");
    lua_pushnumber(L, copy.step_height);
    lua_setfield(L, -2, "step_height");
    lua_pushnumber(L, copy.snap_distance);
    lua_setfield(L, -2, "snap_distance");
    return 1;
}

static int wpe__api_raycast(lua_State* L) {
    WPE_Physics* physics = wpe__physics(L);
    const WPE_Vec3 origin = wpe__check_vec3(L, 1);
    const WPE_Vec3 direction = wpe__check_vec3(L, 2);
    const float max_distance = (float)luaL_optnumber(L, 3, 1000.0);
    const WPE_QueryFilter filter = wpe__check_query_filter(L, 4);
    WPE_RaycastHit hit;
    if (!wpe_physics_raycast(physics, origin, direction, max_distance, &filter, &hit)) {
        lua_pushnil(L);
        return 1;
    }
    wpe__push_hit(L, &hit);
    return 1;
}

static int wpe__api_set_gravity(lua_State* L) {
    WPE_Physics* physics = wpe__physics(L);
    const WPE_Vec3 gravity = wpe__check_vec3(L, 1);
    if (!(isfinite(gravity.x) && isfinite(gravity.y) && isfinite(gravity.z)))
        luaL_errorL(L, "gravity must be finite");
    wpe_physics_set_gravity(physics, gravity);
    return 0;
}

static int wpe__api_get_gravity(lua_State* L) {
    wpe__push_vec3(L, wpe_physics_gravity(wpe__physics(L)));
    return 1;
}

/* ---- Timers and messages --------------------------------------------------------------------------- */

static int wpe__add_timer(lua_State* L, const char* function, bool repeat) {
    WPE_ScriptHost* host = wpe__script_host_from(L);
    const double seconds = luaL_checknumber(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (!isfinite(seconds) || seconds < 0.0 || (repeat && !(seconds > 0.0)))
        luaL_errorL(L, "wpe.%s: seconds must be %s", function, repeat ? "positive" : "0 or more");
    lua_pushvalue(L, 2);
    const int fn_ref = lua_ref(L, -1);
    lua_pop(L, 1);
    const uint32_t id = wpe__script_add_timer(host, seconds, repeat ? seconds : 0.0, fn_ref);
    if (id == 0) {
        lua_unref(L, fn_ref);
        luaL_errorL(L, "wpe.%s: out of memory", function);
    }
    lua_pushnumber(L, (double)id);
    return 1;
}

static int wpe__api_after(lua_State* L) {
    return wpe__add_timer(L, "after", false);
}

static int wpe__api_every(lua_State* L) {
    return wpe__add_timer(L, "every", true);
}

static int wpe__api_cancel(lua_State* L) {
    const double id = luaL_checknumber(L, 1);
    const bool valid = id >= 1.0 && id <= (double)UINT32_MAX && id == floor(id);
    lua_pushboolean(L, valid && wpe__script_cancel_timer(wpe__script_host_from(L), (uint32_t)id));
    return 1;
}

/* The live script component of a handle, or NULL (dead entities and missing scripts are not errors:
 * the receiver of a message may be gone). */
static const WPE_Script* wpe__receiver(lua_State* L, int index) {
    const WPE_ScriptHost* host = wpe__script_host_from(L);
    const WPE_Entity entity = wpe__check_handle(L, index);
    if (!wpe_ecs_is_alive(host->ecs, entity)) return NULL;
    const WPE_Script* s = wpe_ecs_get_component(host->ecs, entity, WPE_COMP_SCRIPT);
    return s && !(s->state & WPE__SCRIPT_FAILED) ? s : NULL;
}

static int wpe__api_get_script(lua_State* L) {
    const WPE_Script* s = wpe__receiver(L, 1);
    if (!s) {
        lua_pushnil(L);
        return 1;
    }
    lua_getref(L, s->instance_ref);
    return 1;
}

static int wpe__api_send(lua_State* L) {
    WPE_ScriptHost* host = wpe__script_host_from(L);
    const char* method = luaL_checkstring(L, 2);
    const WPE_Script* s = wpe__receiver(L, 1);
    if (!s) return 0;
    const WPE_Entity entity = (WPE_Entity)lua_tonumber(L, 1);
    const int instance_ref = s->instance_ref;
    const int top = lua_gettop(L); /* Arguments are 3..top. */
    lua_getref(L, instance_ref);
    lua_getfield(L, -1, method);
    if (!lua_isfunction(L, -1)) return 0; /* Receivers that don't handle it ignore it. */
    lua_insert(L, -2);                    /* [fn, instance] */
    for (int a = 3; a <= top; ++a)
        lua_pushvalue(L, a);
    /* Timers the method makes belong to the receiver. An error goes on to the sender (its callback
     * fails), and the callback that called it restores the running instance. */
    const int previous_instance = host->current_instance;
    const WPE_Entity previous_entity = host->current_entity;
    host->current_instance = instance_ref;
    host->current_entity = entity;
    lua_call(L, top - 1, LUA_MULTRET);
    host->current_instance = previous_instance;
    host->current_entity = previous_entity;
    return lua_gettop(L) - top;
}

/* ---- Saved data ------------------------------------------------------------------------------------- */

#define WPE__SAVE_NAME_MAX 64u
#define WPE__SAVE_MAX_DEPTH 32

/* Raises unless `name` is a save name: 1-64 letters, digits, '_' or '-'. */
static void wpe__check_save_name(lua_State* L, const char* function, const char* name) {
    const size_t length = strlen(name);
    bool valid = length > 0 && length <= WPE__SAVE_NAME_MAX;
    for (size_t i = 0; valid && i < length; ++i) {
        const char c = name[i];
        valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                c == '-';
    }
    if (!valid)
        luaL_errorL(L, "wpe.%s: '%s' is not a save name (1-64 letters, digits, '_' and '-')", function, name);
}

/* The file of a save: <save directory>/<name>.json. Raises when the application has nowhere to save. */
static void wpe__save_path(lua_State* L, const char* function, const char* name, char* out, size_t capacity) {
    const WPE_ScriptHost* host = wpe__script_host_from(L);
    const char* directory = host->app.save_directory ? host->app.save_directory(host->app.user_data) : NULL;
    if (!directory || !directory[0]) luaL_errorL(L, "wpe.%s: saving isn't available here", function);
    const size_t length = strlen(directory);
    const bool separated = directory[length - 1] == '/' || directory[length - 1] == '\\';
    const int written = snprintf(out, capacity, "%s%s%s.json", directory, separated ? "" : "/", name);
    if (written < 0 || (size_t)written >= capacity)
        luaL_errorL(L, "wpe.%s: the save path is too long", function);
}

/* A table holding exactly the keys 1..n (n > 0) is saved as a list; any other needs string keys. */
static bool wpe__table_is_list(lua_State* L, int table) {
    const int length = lua_objlen(L, table);
    if (length <= 0) return false;
    int keys = 0;
    lua_pushnil(L);
    while (lua_next(L, table)) {
        lua_pop(L, 1);
        ++keys;
    }
    return keys == length;
}

/* Raises unless the value at `index` (absolute) can be saved. */
static void wpe__check_savable(lua_State* L, int index, int depth) {
    switch (lua_type(L, index)) {
    case LUA_TBOOLEAN:
    case LUA_TVECTOR:
        return;
    case LUA_TNUMBER:
        if (!isfinite(lua_tonumber(L, index))) luaL_errorL(L, "wpe.save_data: numbers must be finite");
        return;
    case LUA_TSTRING: {
        size_t length = 0;
        const char* text = lua_tolstring(L, index, &length);
        if (strlen(text) != length) luaL_errorL(L, "wpe.save_data: strings can't hold zero bytes");
        return;
    }
    case LUA_TTABLE:
        break;
    default:
        luaL_errorL(L,
                    "wpe.save_data: a %s can't be saved (numbers, booleans, strings, vectors and tables can)",
                    luaL_typename(L, index));
    }
    if (depth >= WPE__SAVE_MAX_DEPTH)
        luaL_errorL(L, "wpe.save_data: tables nest deeper than %d (or a table contains itself)",
                    WPE__SAVE_MAX_DEPTH);
    luaL_checkstack(L, 4, "wpe.save_data");
    const bool list = wpe__table_is_list(L, index);
    lua_pushnil(L);
    while (lua_next(L, index)) {
        if (!list && lua_type(L, -2) != LUA_TSTRING)
            luaL_errorL(L, "wpe.save_data: table keys must be strings, or 1..n for a list");
        if (!list) {
            size_t length = 0;
            const char* key = lua_tolstring(L, -2, &length);
            if (strlen(key) != length) luaL_errorL(L, "wpe.save_data: keys can't hold zero bytes");
        }
        wpe__check_savable(L, lua_gettop(L), depth + 1);
        lua_pop(L, 1);
    }
}

/* Writes a checked value (raises nothing). Vectors are {"$vector": [x, y, z]}. */
static void wpe__encode(lua_State* L, int index, WPE_JsonWriter* w) {
    switch (lua_type(L, index)) {
    case LUA_TBOOLEAN:
        wpe_json_write_bool(w, lua_toboolean(L, index) != 0);
        return;
    case LUA_TNUMBER:
        wpe_json_write_double(w, lua_tonumber(L, index));
        return;
    case LUA_TSTRING:
        wpe_json_write_string(w, lua_tostring(L, index));
        return;
    case LUA_TVECTOR: {
        const float* v = lua_tovector(L, index);
        wpe_json_write_object_begin(w);
        wpe_json_write_key(w, "$vector");
        wpe_json_write_array_begin(w, true);
        for (int i = 0; i < 3; ++i)
            wpe_json_write_double(w, v[i]);
        wpe_json_write_array_end(w);
        wpe_json_write_object_end(w);
        return;
    }
    default:
        break;
    }
    if (wpe__table_is_list(L, index)) {
        wpe_json_write_array_begin(w, false);
        const int length = lua_objlen(L, index);
        for (int i = 1; i <= length; ++i) {
            lua_rawgeti(L, index, i);
            wpe__encode(L, lua_gettop(L), w);
            lua_pop(L, 1);
        }
        wpe_json_write_array_end(w);
        return;
    }
    wpe_json_write_object_begin(w);
    lua_pushnil(L);
    while (lua_next(L, index)) {
        wpe_json_write_key(w, lua_tostring(L, -2));
        wpe__encode(L, lua_gettop(L), w);
        lua_pop(L, 1);
    }
    wpe_json_write_object_end(w);
}

static int wpe__api_save_data(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    wpe__check_save_name(L, "save_data", name);
    luaL_checkany(L, 2);
    if (lua_isnil(L, 2)) luaL_errorL(L, "wpe.save_data: nothing to save (wpe.delete_data removes a save)");
    wpe__check_savable(L, 2, 0);
    char path[1024];
    wpe__save_path(L, "save_data", name, path, sizeof path);
    char temporary[sizeof path + 4];
    snprintf(temporary, sizeof temporary, "%s.tmp", path);

    /* Nothing below raises until the writer is gone. Written beside the save, then renamed over it, so
     * a crash mid-write never leaves a half-written save. */
    WPE_JsonWriter* w = NULL;
    WPE_Result result = wpe_json_writer_create(&w);
    if (result == WPE_RESULT_OK) {
        wpe__encode(L, 2, w);
        const char* text = NULL;
        size_t size = 0;
        result = wpe_json_writer_finish(w, &text, &size);
        WPE_File* file = NULL;
        if (result == WPE_RESULT_OK) result = wpe_file_open(temporary, true, &file);
        if (result == WPE_RESULT_OK) {
            result = wpe_file_write(file, text, size);
            const WPE_Result closed = wpe_file_close(file);
            if (result == WPE_RESULT_OK) result = closed;
        }
        if (result == WPE_RESULT_OK) result = wpe_path_rename(temporary, path);
    }
    wpe_json_writer_destroy(w);
    if (result != WPE_RESULT_OK)
        luaL_errorL(L, "wpe.save_data: cannot write '%s': %s", path, wpe_result_to_string(result));
    return 0;
}

typedef struct wpe__DecodeOp {
    const WPE_JsonValue* value;
    int ref; /* out */
} wpe__DecodeOp;

/* Pushes a saved JSON value as Lua (may raise: runs protected). */
static void wpe__decode(lua_State* L, const WPE_JsonValue* v, int depth) {
    if (depth > WPE__SAVE_MAX_DEPTH) luaL_errorL(L, "the saved data nests too deep");
    luaL_checkstack(L, 4, "wpe.load_data");
    switch (wpe_json_type(v)) {
    case WPE_JSON_BOOL:
        lua_pushboolean(L, wpe_json_bool(v, false));
        return;
    case WPE_JSON_NUMBER:
        lua_pushnumber(L, wpe_json_number(v, 0.0));
        return;
    case WPE_JSON_STRING:
        lua_pushstring(L, wpe_json_string(v));
        return;
    case WPE_JSON_ARRAY:
        lua_createtable(L, (int)wpe_json_count(v), 0);
        for (uint32_t i = 0; i < wpe_json_count(v); ++i) {
            wpe__decode(L, wpe_json_at(v, i), depth + 1);
            lua_rawseti(L, -2, (int)i + 1);
        }
        return;
    case WPE_JSON_OBJECT: {
        const WPE_JsonValue* vector = wpe_json_get(v, "$vector");
        if (vector && wpe_json_count(v) == 1 && wpe_json_type(vector) == WPE_JSON_ARRAY &&
            wpe_json_count(vector) == 3) {
            lua_pushvector(L, (float)wpe_json_number(wpe_json_at(vector, 0), 0.0),
                           (float)wpe_json_number(wpe_json_at(vector, 1), 0.0),
                           (float)wpe_json_number(wpe_json_at(vector, 2), 0.0));
            return;
        }
        lua_createtable(L, 0, (int)wpe_json_count(v));
        for (uint32_t i = 0; i < wpe_json_count(v); ++i) {
            const char* key = NULL;
            const WPE_JsonValue* member = wpe_json_member(v, i, &key);
            lua_pushstring(L, key);
            wpe__decode(L, member, depth + 1);
            lua_rawset(L, -3);
        }
        return;
    }
    default:
        lua_pushnil(L);
        return;
    }
}

static int wpe__op_decode(lua_State* L) {
    wpe__DecodeOp* op = lua_touserdata(L, 1);
    wpe__decode(L, op->value, 0);
    op->ref = lua_ref(L, -1);
    return 0;
}

static int wpe__api_load_data(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    wpe__check_save_name(L, "load_data", name);
    char path[1024];
    wpe__save_path(L, "load_data", name, path, sizeof path);
    int64_t modified = 0;
    if (wpe_file_modified_time(path, &modified) != WPE_RESULT_OK) {
        lua_pushnil(L); /* Never saved. */
        return 1;
    }
    /* The text and document are freed before anything raises; Lua values are built protected. */
    void* text = NULL;
    size_t size = 0;
    WPE_JsonDocument* doc = NULL;
    WPE_Result result = wpe_file_read_all(path, &text, &size);
    if (result == WPE_RESULT_OK) result = wpe_json_parse(text, size, &doc, NULL);
    char message[256] = "";
    wpe__DecodeOp op = {.ref = LUA_NOREF};
    if (result == WPE_RESULT_OK) {
        op.value = wpe_json_root(doc);
        if (lua_cpcall(L, wpe__op_decode, &op) != 0) {
            snprintf(message, sizeof message, "%s", lua_tostring(L, -1) ? lua_tostring(L, -1) : "VM error");
            lua_pop(L, 1);
            result = WPE_RESULT_ERROR_OUT_OF_MEMORY;
        }
    }
    wpe_json_free(doc);
    wpe_free(text);
    if (result != WPE_RESULT_OK)
        luaL_errorL(L, "wpe.load_data: save '%s' can't be read (%s)", name,
                    message[0] ? message : wpe_result_to_string(result));
    lua_getref(L, op.ref);
    lua_unref(L, op.ref);
    return 1;
}

static int wpe__api_delete_data(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    wpe__check_save_name(L, "delete_data", name);
    char path[1024];
    wpe__save_path(L, "delete_data", name, path, sizeof path);
    int64_t modified = 0;
    const bool exists = wpe_file_modified_time(path, &modified) == WPE_RESULT_OK;
    if (exists && wpe_path_remove(path) != WPE_RESULT_OK)
        luaL_errorL(L, "wpe.delete_data: cannot remove '%s'", path);
    lua_pushboolean(L, exists);
    return 1;
}

/* ---- Audio ----------------------------------------------------------------------------------------- */

static WPE_Audio* wpe__audio(lua_State* L) {
    WPE_Audio* audio = wpe__script_host_from(L)->audio;
    if (!audio) luaL_errorL(L, "audio is not enabled for this game");
    return audio;
}

static WPE_AudioBus wpe__check_bus(lua_State* L, float value) {
    if (!(value == WPE_AUDIO_BUS_MASTER || value == WPE_AUDIO_BUS_EFFECTS || value == WPE_AUDIO_BUS_MUSIC))
        luaL_errorL(L, "bus must be wpe.BUS_MASTER, wpe.BUS_EFFECTS or wpe.BUS_MUSIC");
    return (WPE_AudioBus)value;
}

/* Loads a sound by path (cached by the host); raises if it cannot be loaded. */
static WPE_AssetHandle wpe__check_sound(lua_State* L, const char* function, const char* path) {
    const WPE_AssetHandle sound = wpe__script_find_sound(wpe__script_host_from(L), path);
    if (sound == WPE_ASSET_NONE) luaL_errorL(L, "wpe.%s: cannot load sound '%s'", function, path);
    return sound;
}

static int wpe__api_play_sound(lua_State* L) {
    WPE_Audio* audio = wpe__audio(L);
    const char* path = luaL_checkstring(L, 1);
    WPE_AudioPlayDesc desc = {.volume = 1.0f, .pitch = 1.0f, .bus = WPE_AUDIO_BUS_EFFECTS};
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        desc.volume = wpe__field_number(L, 2, "volume", desc.volume);
        desc.pitch = wpe__field_number(L, 2, "pitch", desc.pitch);
        desc.min_distance = wpe__field_number(L, 2, "min_distance", 0.0f);
        desc.max_distance = wpe__field_number(L, 2, "max_distance", 0.0f);
        desc.bus = wpe__check_bus(L, wpe__field_number(L, 2, "bus", (float)desc.bus));
        desc.loop = wpe__field_bool(L, 2, "loop", false);
        lua_getfield(L, 2, "position");
        if (!lua_isnil(L, -1)) {
            desc.position = wpe__check_vec3(L, -1);
            desc.spatial = true;
        }
        lua_pop(L, 1);
        if (!(desc.volume >= 0.0f) || !(desc.pitch > 0.0f))
            luaL_errorL(L, "wpe.play_sound: invalid volume or pitch");
    }
    const WPE_AssetHandle sound = wpe__check_sound(L, "play_sound", path);
    lua_pushnumber(L, (double)wpe_audio_play(audio, sound, &desc)); /* 0 when no voice was free. */
    return 1;
}

static int wpe__api_stop_sound(lua_State* L) {
    WPE_Audio* audio = wpe__audio(L);
    const double voice = luaL_checknumber(L, 1);
    if (voice >= 1.0 && voice <= (double)UINT32_MAX && voice == floor(voice))
        wpe_audio_stop(audio, (WPE_Voice)voice);
    return 0;
}

static int wpe__api_add_audio_source(lua_State* L) {
    WPE_ScriptHost* host = wpe__check_unrestricted(L, "add_audio_source");
    wpe__audio(L);
    const WPE_Entity entity = wpe__check_entity(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_getfield(L, 2, "sound");
    const char* path = luaL_checkstring(L, -1); /* Stays valid: the table keeps the string alive. */
    lua_pop(L, 1);
    WPE_AudioSource source = wpe_audio_source_default(WPE_ASSET_NONE);
    source.volume = wpe__field_number(L, 2, "volume", source.volume);
    source.pitch = wpe__field_number(L, 2, "pitch", source.pitch);
    source.min_distance = wpe__field_number(L, 2, "min_distance", source.min_distance);
    source.max_distance = wpe__field_number(L, 2, "max_distance", source.max_distance);
    source.bus = wpe__check_bus(L, wpe__field_number(L, 2, "bus", (float)source.bus));
    source.loop = wpe__field_bool(L, 2, "loop", source.loop);
    source.spatial = wpe__field_bool(L, 2, "spatial", source.spatial);
    source.playing = wpe__field_bool(L, 2, "play", true);
    if (!(source.volume >= 0.0f) || !(source.pitch > 0.0f) || !(source.min_distance > 0.0f) ||
        !(source.max_distance >= source.min_distance))
        luaL_errorL(L, "wpe.add_audio_source: invalid volume, pitch or distances");
    source.sound = wpe__check_sound(L, "add_audio_source", path);
    /* Replaces an existing source (the component owns a reference to its sound). */
    if (wpe_ecs_has_component(host->ecs, entity, WPE_COMP_AUDIO_SOURCE))
        wpe__raise_result(L, "add_audio_source",
                          wpe_ecs_remove_component(host->ecs, entity, WPE_COMP_AUDIO_SOURCE));
    wpe__raise_result(L, "add_audio_source",
                      wpe_ecs_add_component(host->ecs, entity, WPE_COMP_AUDIO_SOURCE, &source, NULL));
    return 0;
}

static int wpe__api_remove_audio_source(lua_State* L) {
    WPE_ScriptHost* host = wpe__check_unrestricted(L, "remove_audio_source");
    wpe__audio(L);
    const WPE_Entity entity = wpe__check_entity(L, 1);
    if (wpe_ecs_has_component(host->ecs, entity, WPE_COMP_AUDIO_SOURCE))
        wpe__raise_result(L, "remove_audio_source",
                          wpe_ecs_remove_component(host->ecs, entity, WPE_COMP_AUDIO_SOURCE));
    return 0;
}

static WPE_AudioSource* wpe__check_audio_source(lua_State* L, WPE_Entity entity) {
    wpe__audio(L);
    WPE_AudioSource* source =
        wpe_ecs_get_component(wpe__script_host_from(L)->ecs, entity, WPE_COMP_AUDIO_SOURCE);
    if (!source) luaL_errorL(L, "entity 0x%08x has no audio source", entity);
    return source;
}

static int wpe__api_set_audio_playing(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    const bool playing = luaL_checkboolean(L, 2);
    wpe__check_audio_source(L, entity)->playing = playing;
    return 0;
}

static int wpe__api_is_audio_playing(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    lua_pushboolean(L, wpe__check_audio_source(L, entity)->playing);
    return 1;
}

static int wpe__api_play_music(lua_State* L) {
    WPE_Audio* audio = wpe__audio(L);
    const char* path = luaL_checkstring(L, 1);
    const float fade = (float)luaL_optnumber(L, 2, 0.0);
    const bool loop = lua_isnoneornil(L, 3) ? true : luaL_checkboolean(L, 3);
    wpe__raise_result(L, "play_music", wpe_audio_play_music(audio, path, fade, loop));
    return 0;
}

static int wpe__api_stop_music(lua_State* L) {
    wpe_audio_stop_music(wpe__audio(L), (float)luaL_optnumber(L, 1, 0.0));
    return 0;
}

static int wpe__api_set_volume(lua_State* L) {
    WPE_Audio* audio = wpe__audio(L);
    const WPE_AudioBus bus = wpe__check_bus(L, (float)luaL_checknumber(L, 1));
    const float volume = (float)luaL_checknumber(L, 2);
    if (!(volume >= 0.0f) || !isfinite(volume)) luaL_errorL(L, "wpe.set_volume: volume must be >= 0");
    wpe_audio_set_volume(audio, bus, volume);
    return 0;
}

static int wpe__api_get_volume(lua_State* L) {
    WPE_Audio* audio = wpe__audio(L);
    lua_pushnumber(L, wpe_audio_volume(audio, wpe__check_bus(L, (float)luaL_checknumber(L, 1))));
    return 1;
}

/* ---- Input ----------------------------------------------------------------------------------------- */

static WPE_Input* wpe__input(lua_State* L) {
    WPE_Input* input = wpe__script_host_from(L)->input;
    if (!input) luaL_errorL(L, "input is not enabled for this game");
    return input;
}

/* Players are numbered 1..WPE_INPUT_MAX_PLAYERS in Luau. */
static uint32_t wpe__check_player(lua_State* L, int index) {
    const double player = luaL_checknumber(L, index);
    if (!(player >= 1.0 && player <= (double)WPE_INPUT_MAX_PLAYERS && player == floor(player)))
        luaL_errorL(L, "argument #%d: player must be a whole number from 1 to %u", index,
                    WPE_INPUT_MAX_PLAYERS);
    return (uint32_t)player - 1u;
}

/* Optional player argument: nil means any player (every device). */
static uint32_t wpe__opt_player(lua_State* L, int index) {
    return lua_isnoneornil(L, index) ? WPE_INPUT_ANY_PLAYER : wpe__check_player(L, index);
}

static WPE_InputAction wpe__check_action(lua_State* L, WPE_Input* input, int index) {
    const char* name = luaL_checkstring(L, index);
    const WPE_InputAction action = wpe_input_find_action(input, name);
    if (action == WPE_INPUT_ACTION_NONE) luaL_errorL(L, "unknown input action '%s'", name);
    return action;
}

static WPE_Key wpe__check_key(lua_State* L, int index) {
    const char* name = luaL_checkstring(L, index);
    WPE_Key key = WPE_KEY_UNKNOWN;
    if (!wpe_input_key_from_name(name, &key)) luaL_errorL(L, "unknown key name '%s'", name);
    return key;
}

static WPE_MouseButton wpe__check_mouse_button(lua_State* L, int index) {
    const char* name = luaL_checkstring(L, index);
    WPE_MouseButton button = WPE_MOUSE_LEFT;
    if (!wpe_input_mouse_button_from_name(name, &button)) luaL_errorL(L, "unknown mouse button '%s'", name);
    return button;
}

static WPE_GamepadButton wpe__check_pad_button(lua_State* L, int index) {
    const char* name = luaL_checkstring(L, index);
    WPE_GamepadButton button = WPE_PAD_SOUTH;
    if (!wpe_input_pad_button_from_name(name, &button)) luaL_errorL(L, "unknown gamepad button '%s'", name);
    return button;
}

static WPE_GamepadAxis wpe__check_pad_axis(lua_State* L, int index) {
    const char* name = luaL_checkstring(L, index);
    WPE_GamepadAxis axis = WPE_PAD_LEFT_X;
    if (!wpe_input_pad_axis_from_name(name, &axis)) luaL_errorL(L, "unknown gamepad axis '%s'", name);
    return axis;
}

static int wpe__api_action_down(lua_State* L) {
    WPE_Input* input = wpe__input(L);
    const WPE_InputAction action = wpe__check_action(L, input, 1);
    lua_pushboolean(L, wpe_input_action_down(input, action, wpe__opt_player(L, 2)));
    return 1;
}

static int wpe__api_action_pressed(lua_State* L) {
    WPE_Input* input = wpe__input(L);
    const WPE_InputAction action = wpe__check_action(L, input, 1);
    lua_pushboolean(L, wpe_input_action_pressed(input, action, wpe__opt_player(L, 2)));
    return 1;
}

static int wpe__api_action_released(lua_State* L) {
    WPE_Input* input = wpe__input(L);
    const WPE_InputAction action = wpe__check_action(L, input, 1);
    lua_pushboolean(L, wpe_input_action_released(input, action, wpe__opt_player(L, 2)));
    return 1;
}

static int wpe__api_action_value(lua_State* L) {
    WPE_Input* input = wpe__input(L);
    const WPE_InputAction action = wpe__check_action(L, input, 1);
    lua_pushnumber(L, wpe_input_action_value(input, action, wpe__opt_player(L, 2)));
    return 1;
}

static int wpe__api_key_down(lua_State* L) {
    WPE_Input* input = wpe__input(L);
    lua_pushboolean(L, wpe_input_key_down(input, wpe__check_key(L, 1)));
    return 1;
}

static int wpe__api_key_pressed(lua_State* L) {
    WPE_Input* input = wpe__input(L);
    lua_pushboolean(L, wpe_input_key_pressed(input, wpe__check_key(L, 1)));
    return 1;
}

static int wpe__api_key_released(lua_State* L) {
    WPE_Input* input = wpe__input(L);
    lua_pushboolean(L, wpe_input_key_released(input, wpe__check_key(L, 1)));
    return 1;
}

static int wpe__api_mouse_down(lua_State* L) {
    WPE_Input* input = wpe__input(L);
    lua_pushboolean(L, wpe_input_mouse_down(input, wpe__check_mouse_button(L, 1)));
    return 1;
}

static int wpe__api_mouse_pressed(lua_State* L) {
    WPE_Input* input = wpe__input(L);
    lua_pushboolean(L, wpe_input_mouse_pressed(input, wpe__check_mouse_button(L, 1)));
    return 1;
}

static int wpe__api_mouse_released(lua_State* L) {
    WPE_Input* input = wpe__input(L);
    lua_pushboolean(L, wpe_input_mouse_released(input, wpe__check_mouse_button(L, 1)));
    return 1;
}

static int wpe__api_mouse_position(lua_State* L) {
    float x = 0.0f, y = 0.0f;
    wpe_input_mouse_position(wpe__input(L), &x, &y);
    lua_pushnumber(L, x);
    lua_pushnumber(L, y);
    return 2;
}

static int wpe__api_mouse_delta(lua_State* L) {
    float x = 0.0f, y = 0.0f;
    wpe_input_mouse_delta(wpe__input(L), &x, &y);
    lua_pushnumber(L, x);
    lua_pushnumber(L, y);
    return 2;
}

static int wpe__api_mouse_wheel(lua_State* L) {
    float x = 0.0f, y = 0.0f;
    wpe_input_mouse_wheel(wpe__input(L), &x, &y);
    lua_pushnumber(L, y);
    return 1;
}

static int wpe__api_set_mouse_captured(lua_State* L) {
    WPE_Input* input = wpe__input(L);
    wpe_input_set_mouse_captured(input, luaL_checkboolean(L, 1));
    return 0;
}

static int wpe__api_is_mouse_captured(lua_State* L) {
    lua_pushboolean(L, wpe_input_mouse_captured(wpe__input(L)));
    return 1;
}

static int wpe__api_gamepad_count(lua_State* L) {
    lua_pushnumber(L, wpe_input_gamepad_count(wpe__input(L)));
    return 1;
}

static int wpe__api_gamepad_connected(lua_State* L) {
    WPE_Input* input = wpe__input(L);
    lua_pushboolean(L, wpe_input_gamepad_connected(input, wpe__check_player(L, 1)));
    return 1;
}

static int wpe__api_gamepad_name(lua_State* L) {
    WPE_Input* input = wpe__input(L);
    const char* name = wpe_input_gamepad_name(input, wpe__check_player(L, 1));
    if (name) {
        lua_pushstring(L, name);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

static int wpe__api_gamepad_down(lua_State* L) {
    WPE_Input* input = wpe__input(L);
    const WPE_GamepadButton button = wpe__check_pad_button(L, 1);
    lua_pushboolean(L, wpe_input_gamepad_down(input, wpe__opt_player(L, 2), button));
    return 1;
}

static int wpe__api_gamepad_pressed(lua_State* L) {
    WPE_Input* input = wpe__input(L);
    const WPE_GamepadButton button = wpe__check_pad_button(L, 1);
    lua_pushboolean(L, wpe_input_gamepad_pressed(input, wpe__opt_player(L, 2), button));
    return 1;
}

static int wpe__api_gamepad_released(lua_State* L) {
    WPE_Input* input = wpe__input(L);
    const WPE_GamepadButton button = wpe__check_pad_button(L, 1);
    lua_pushboolean(L, wpe_input_gamepad_released(input, wpe__opt_player(L, 2), button));
    return 1;
}

static int wpe__api_gamepad_axis(lua_State* L) {
    WPE_Input* input = wpe__input(L);
    const WPE_GamepadAxis axis = wpe__check_pad_axis(L, 1);
    lua_pushnumber(L, wpe_input_gamepad_axis(input, wpe__opt_player(L, 2), axis));
    return 1;
}

static int wpe__api_rumble(lua_State* L) {
    WPE_Input* input = wpe__input(L);
    const float low = (float)luaL_checknumber(L, 1);
    const float high = (float)luaL_checknumber(L, 2);
    const float seconds = (float)luaL_checknumber(L, 3);
    const uint32_t player = wpe__opt_player(L, 4);
    bool any = false;
    for (uint32_t p = 0; p < WPE_INPUT_MAX_PLAYERS; ++p) {
        if (player == WPE_INPUT_ANY_PLAYER || player == p)
            any |= wpe_input_rumble(input, p, low, high, seconds) == WPE_RESULT_OK;
    }
    lua_pushboolean(L, any);
    return 1;
}

/* ---- Application: scenes, camera, exit ------------------------------------------------------------- */

static const WPE_ScriptApp* wpe__app(lua_State* L, bool available, const char* function) {
    if (!available) luaL_errorL(L, "wpe.%s is not available in this application", function);
    return &wpe__script_host_from(L)->app;
}

static int wpe__api_load_scene(lua_State* L) {
    WPE_ScriptHost* host = wpe__script_host_from(L);
    const WPE_ScriptApp* app = wpe__app(L, host->app.load_scene != NULL, "load_scene");
    const char* path = luaL_checkstring(L, 1);
    if (!wpe_vfs_path_is_valid(path) || !wpe_vfs_exists(wpe_asset_manager_vfs(host->assets), path))
        luaL_errorL(L, "wpe.load_scene: scene '%s' not found", path);
    app->load_scene(app->user_data, path);
    return 0;
}

static int wpe__api_quit(lua_State* L) {
    const WPE_ScriptApp* app = wpe__app(L, wpe__script_host_from(L)->app.quit != NULL, "quit");
    app->quit(app->user_data);
    return 0;
}

static int wpe__api_set_camera(lua_State* L) {
    WPE_ScriptHost* host = wpe__script_host_from(L);
    const WPE_ScriptApp* app = wpe__app(L, host->app.set_camera != NULL, "set_camera");
    const WPE_Entity entity = wpe__check_entity(L, 1);
    if (!wpe_ecs_has_component(host->ecs, entity, WPE_COMP_CAMERA))
        luaL_errorL(L, "entity 0x%08x has no camera", entity);
    app->set_camera(app->user_data, entity);
    return 0;
}

static int wpe__api_get_camera(lua_State* L) {
    const WPE_ScriptApp* app = wpe__app(L, wpe__script_host_from(L)->app.get_camera != NULL, "get_camera");
    const WPE_Entity camera = app->get_camera(app->user_data);
    if (camera != WPE_ENTITY_NULL) {
        lua_pushnumber(L, (double)camera);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

static int wpe__api_screen_size(lua_State* L) {
    const WPE_ScriptApp* app = wpe__app(L, wpe__script_host_from(L)->app.screen_size != NULL, "screen_size");
    uint32_t width = 0, height = 0;
    app->screen_size(app->user_data, &width, &height);
    lua_pushnumber(L, width);
    lua_pushnumber(L, height);
    return 2;
}

/* wpe.capture_screen([slot = 1]) -> image path: this frame as the scene renders it (before post effects and
 * 2D), kept in a slot until captured again. */
static int wpe__api_capture_screen(lua_State* L) {
    const WPE_ScriptApp* app =
        wpe__app(L, wpe__script_host_from(L)->app.capture_screen != NULL, "capture_screen");
    const double slot = luaL_optnumber(L, 1, 1.0);
    if (!(slot >= 1.0 && slot <= 8.0 && slot == floor(slot)))
        luaL_errorL(L, "wpe.capture_screen: slots are 1 to 8");
    const char* path = app->capture_screen(app->user_data, (uint32_t)slot);
    if (!path) luaL_errorL(L, "wpe.capture_screen: the frame can't be captured (see the log)");
    lua_pushstring(L, path);
    return 1;
}

/* wpe.world_to_screen(position) -> x, y, in_front: where a world point shows on screen, in pixels (from the
 * top-left), through the current camera. */
static int wpe__api_world_to_screen(lua_State* L) {
    WPE_ScriptHost* host = wpe__script_host_from(L);
    const WPE_ScriptApp* app =
        wpe__app(L, host->app.get_camera != NULL && host->app.screen_size != NULL, "world_to_screen");
    const WPE_Vec3 p = wpe__check_vec3(L, 1);
    uint32_t width = 0, height = 0;
    app->screen_size(app->user_data, &width, &height);
    /* The view from the camera's current placement: its cached world matrix is only refreshed once a frame,
     * after scripts may have moved it. */
    const WPE_Entity camera = app->get_camera(app->user_data);
    WPE_Mat4 view, projection, world, rigid;
    WPE_Vec3 position, scale;
    WPE_Quat rotation;
    if (!wpe_scene_camera_matrices(host->ecs, camera, height ? (float)width / (float)height : 1.0f, &view,
                                   &projection) ||
        !wpe_scene_compute_world_matrix(host->ecs, camera, &world) ||
        !wpe_mat4_decompose(&world, &position, &rotation, &scale))
        luaL_errorL(L, "wpe.world_to_screen: there is no camera");
    rigid = wpe_mat4_from_trs(position, wpe_quat_normalize(rotation), wpe_vec3(1.0f, 1.0f, 1.0f));
    if (!wpe_mat4_inverse(&rigid, &view))
        luaL_errorL(L, "wpe.world_to_screen: the camera's placement is degenerate");
    const WPE_Mat4 view_projection = wpe_mat4_mul(&projection, &view);
    const WPE_Vec4 clip = wpe_mat4_mul_vec4(&view_projection, wpe_vec4(p.x, p.y, p.z, 1.0f));
    const bool in_front = clip.w > 1e-6f;
    const float w = in_front ? clip.w : 1e-6f;
    lua_pushnumber(L, (clip.x / w * 0.5 + 0.5) * width);
    lua_pushnumber(L, (0.5 - clip.y / w * 0.5) * height);
    lua_pushboolean(L, in_front);
    return 3;
}

/* ---- Post-processing ------------------------------------------------------------------------------- */

static WPE_PostFxConfig* wpe__postfx_list(lua_State* L, const char* function, uint32_t** out_count) {
    const WPE_ScriptApp* app = wpe__app(L, wpe__script_host_from(L)->app.postfx != NULL, function);
    return app->postfx(app->user_data, out_count);
}

static void wpe__push_postfx(lua_State* L, const WPE_PostFxConfig* c) {
    uint32_t count = 0;
    const WPE_PostFxParam* params = wpe_postfx_params(c->kind, &count);
    lua_createtable(L, 0, (int)count + 2);
    lua_pushstring(L, wpe_postfx_kind_name(c->kind));
    lua_setfield(L, -2, "effect");
    lua_pushboolean(L, c->enabled);
    lua_setfield(L, -2, "enabled");
    for (uint32_t p = 0; p < count; ++p) {
        lua_pushnumber(L, wpe_postfx_param_get(c, &params[p]));
        lua_setfield(L, -2, params[p].name);
    }
}

/* An effect by 1-based index, or the first of a kind's name; raises when there's none. Returns the 0-based
 * index. */
static uint32_t wpe__check_postfx(lua_State* L, int arg, const WPE_PostFxConfig* list, uint32_t count,
                                  const char* function) {
    if (lua_type(L, arg) == LUA_TSTRING) {
        const char* name = lua_tostring(L, arg);
        for (uint32_t i = 0; i < count; ++i)
            if (strcmp(wpe_postfx_kind_name(list[i].kind), name) == 0) return i;
        luaL_errorL(L, "wpe.%s: the scene has no '%s' effect", function, name);
    }
    const double index = luaL_checknumber(L, arg);
    if (!(index >= 1.0 && index <= (double)count && index == floor(index)))
        luaL_errorL(L, "wpe.%s: no effect %g (the scene has %u)", function, index, count);
    return (uint32_t)index - 1;
}

/* Sets `enabled` and parameters from a table onto *c (a copy: it may raise halfway). "effect" is the
 * caller's. */
static void wpe__read_postfx(lua_State* L, int table, WPE_PostFxConfig* c, const char* function) {
    luaL_checktype(L, table, LUA_TTABLE);
    uint32_t count = 0;
    const WPE_PostFxParam* params = wpe_postfx_params(c->kind, &count);
    lua_pushnil(L);
    while (lua_next(L, table)) {
        if (lua_type(L, -2) != LUA_TSTRING) luaL_errorL(L, "wpe.%s: effect fields are named", function);
        const char* key = lua_tostring(L, -2);
        if (strcmp(key, "effect") == 0) {
            lua_pop(L, 1);
            continue;
        }
        if (strcmp(key, "enabled") == 0) {
            c->enabled = luaL_checkboolean(L, -1);
            lua_pop(L, 1);
            continue;
        }
        const WPE_PostFxParam* param = NULL;
        for (uint32_t p = 0; p < count && !param; ++p)
            if (strcmp(params[p].name, key) == 0) param = &params[p];
        if (!param)
            luaL_errorL(L, "wpe.%s: '%s' has no parameter '%s'", function, wpe_postfx_kind_name(c->kind),
                        key);
        if (lua_type(L, -1) != LUA_TNUMBER) luaL_errorL(L, "wpe.%s: '%s' must be a number", function, key);
        wpe_postfx_param_set(c, param, lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
}

static int wpe__api_get_postfx(lua_State* L) {
    uint32_t* count = NULL;
    const WPE_PostFxConfig* list = wpe__postfx_list(L, "get_postfx", &count);
    lua_createtable(L, (int)*count, 0);
    for (uint32_t i = 0; i < *count; ++i) {
        wpe__push_postfx(L, &list[i]);
        lua_rawseti(L, -2, (int)i + 1);
    }
    return 1;
}

static int wpe__api_set_postfx(lua_State* L) {
    uint32_t* count = NULL;
    WPE_PostFxConfig* list = wpe__postfx_list(L, "set_postfx", &count);
    const uint32_t index = wpe__check_postfx(L, 1, list, *count, "set_postfx");
    WPE_PostFxConfig c = list[index];
    wpe__read_postfx(L, 2, &c, "set_postfx");
    list[index] = c;
    return 0;
}

static int wpe__api_add_postfx(lua_State* L) {
    uint32_t* count = NULL;
    WPE_PostFxConfig* list = wpe__postfx_list(L, "add_postfx", &count);
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "effect");
    WPE_PostFxKind kind;
    if (lua_type(L, -1) != LUA_TSTRING || !wpe_postfx_kind_from_name(lua_tostring(L, -1), &kind))
        luaL_errorL(
            L, "wpe.add_postfx: \"effect\" must name an effect (\"shockwave\", \"depth_of_field\", ...)");
    lua_pop(L, 1);
    if (*count >= WPE_POSTFX_MAX_EFFECTS)
        luaL_errorL(L, "wpe.add_postfx: the scene already has %u effects", WPE_POSTFX_MAX_EFFECTS);
    WPE_PostFxConfig c = wpe_postfx_config_default(kind);
    wpe__read_postfx(L, 1, &c, "add_postfx");
    list[(*count)++] = c;
    lua_pushnumber(L, *count);
    return 1;
}

static int wpe__api_remove_postfx(lua_State* L) {
    uint32_t* count = NULL;
    WPE_PostFxConfig* list = wpe__postfx_list(L, "remove_postfx", &count);
    const uint32_t index = wpe__check_postfx(L, 1, list, *count, "remove_postfx");
    memmove(&list[index], &list[index + 1], (size_t)(*count - index - 1) * sizeof *list);
    --*count;
    return 0;
}

/* ---- Render settings ------------------------------------------------------------------------------- */

static WPE_SceneRenderSettings* wpe__render_settings(lua_State* L, const char* function) {
    const WPE_ScriptApp* app = wpe__app(L, wpe__script_host_from(L)->app.render_settings != NULL, function);
    return app->render_settings(app->user_data);
}

static int wpe__api_set_texture_filter(lua_State* L) {
    WPE_SceneRenderSettings* settings = wpe__render_settings(L, "set_texture_filter");
    WPE_TextureFiltering filtering;
    if (!wpe_texture_filtering_from_name(luaL_checkstring(L, 1), &filtering))
        luaL_errorL(L,
                    "wpe.set_texture_filter: '%s' is not \"material\", \"nearest\", \"bilinear\" or "
                    "\"three_point\"",
                    lua_tostring(L, 1));
    settings->texture_filtering = filtering;
    return 0;
}

static int wpe__api_get_texture_filter(lua_State* L) {
    const WPE_SceneRenderSettings* settings = wpe__render_settings(L, "get_texture_filter");
    lua_pushstring(L, wpe_texture_filtering_name(settings->texture_filtering));
    return 1;
}

/* ---- 2D drawing ------------------------------------------------------------------------------------ */

static WPE_Draw2D* wpe__draw2d(lua_State* L) {
    WPE_Draw2D* draw2d = wpe__script_host_from(L)->draw2d;
    if (!draw2d) luaL_errorL(L, "2D drawing is not enabled for this game");
    return draw2d;
}

static const WPE_BitmapFont* wpe__font(lua_State* L) {
    WPE_ScriptHost* host = wpe__script_host_from(L);
    const WPE_BitmapFont* font = wpe_asset_font(host->assets, host->font);
    if (!font) luaL_errorL(L, "text drawing needs a font (none configured for this game)");
    return font;
}

/* Colors are numbers 0xAARRGGBB (see wpe.color); nil = opaque white. */
static WPE_Color wpe__opt_color(lua_State* L, int index) {
    if (lua_isnoneornil(L, index)) return 0xFFFFFFFFu;
    const double value = luaL_checknumber(L, index);
    if (!(value >= 0.0 && value <= (double)UINT32_MAX && value == floor(value)))
        luaL_errorL(L, "argument #%d is not a color (use wpe.color(r, g, b, a))", index);
    return (WPE_Color)value;
}

static float wpe__opt_scale(lua_State* L, int index) {
    const float scale = (float)luaL_optnumber(L, index, 1.0);
    if (!(scale > 0.0f && scale <= 64.0f)) luaL_errorL(L, "argument #%d: scale must be in (0, 64]", index);
    return scale;
}

static int wpe__api_color(lua_State* L) {
    double channels[4] = {luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3),
                          luaL_optnumber(L, 4, 1.0)};
    uint32_t bytes[4];
    for (int i = 0; i < 4; ++i) {
        const double c = channels[i] < 0.0 ? 0.0 : channels[i] > 1.0 ? 1.0 : channels[i];
        bytes[i] = (uint32_t)(c * 255.0 + 0.5);
    }
    lua_pushnumber(L, (double)((bytes[3] << 24) | (bytes[0] << 16) | (bytes[1] << 8) | bytes[2]));
    return 1;
}

static int wpe__api_draw_text(lua_State* L) {
    WPE_Draw2D* draw2d = wpe__draw2d(L);
    const float x = (float)luaL_checknumber(L, 1);
    const float y = (float)luaL_checknumber(L, 2);
    const char* text = luaL_checkstring(L, 3);
    const WPE_Color color = wpe__opt_color(L, 4);
    const float scale = wpe__opt_scale(L, 5);
    wpe_draw2d_text(draw2d, wpe__font(L), x, y, text, color, scale);
    return 0;
}

static int wpe__api_draw_rect(lua_State* L) {
    WPE_Draw2D* draw2d = wpe__draw2d(L);
    const WPE_Rect rect = {(float)luaL_checknumber(L, 1), (float)luaL_checknumber(L, 2),
                           (float)luaL_checknumber(L, 3), (float)luaL_checknumber(L, 4)};
    wpe_draw2d_rect(draw2d, rect, wpe__opt_color(L, 5));
    return 0;
}

static int wpe__api_text_size(lua_State* L) {
    const char* text = luaL_checkstring(L, 1);
    const float scale = wpe__opt_scale(L, 2);
    uint32_t width = 0, height = 0;
    wpe_font_measure(wpe__font(L), text, &width, &height);
    lua_pushnumber(L, (double)width * scale);
    lua_pushnumber(L, (double)height * scale);
    return 2;
}

/* ---- Billboards ------------------------------------------------------------------------------------ */

static const char* const wpe__billboard_modes[] = {"fixed", "axis_aligned", "axis_aligned_y",
                                                   "screen_aligned"};
static const char* const wpe__billboard_blends[] = {"cutout", "alpha", "additive"};
static_assert(sizeof wpe__billboard_modes / sizeof wpe__billboard_modes[0] ==
                  WPE_BILLBOARD_SCREEN_ALIGNED + 1,
              "billboard mode names");
static_assert(sizeof wpe__billboard_blends / sizeof wpe__billboard_blends[0] == WPE_BILLBOARD_ADDITIVE + 1,
              "billboard blend names");

/* A field naming one of `names` (nil keeps `fallback`); raises on anything else. */
static uint32_t wpe__field_choice(lua_State* L, int table, const char* key, const char* const* names,
                                  uint32_t count, uint32_t fallback) {
    lua_getfield(L, table, key);
    uint32_t value = fallback;
    if (!lua_isnil(L, -1)) {
        const char* name = luaL_checkstring(L, -1);
        value = count;
        for (uint32_t i = 0; i < count; ++i) {
            if (strcmp(name, names[i]) == 0) value = i;
        }
        if (value == count)
            luaL_errorL(L, "%s \"%s\" is not one of the choices (see docs/scripting.md)", key, name);
    }
    lua_pop(L, 1);
    return value;
}

/* A whole-number field in [min, max]. */
static uint32_t wpe__field_count(lua_State* L, int table, const char* key, uint32_t min, uint32_t max,
                                 uint32_t fallback) {
    const float value = wpe__field_number(L, table, key, (float)fallback);
    if (!(value >= (float)min && value <= (float)max) || value != floorf(value))
        luaL_errorL(L, "%s must be a whole number from %u to %u", key, min, max);
    return (uint32_t)value;
}

/* Reads a billboard description table over `base` (missing fields keep their values). An "image" path is
 * returned through *out_image (NULL when the table has none). Raises on bad fields. */
static WPE_Billboard wpe__check_billboard(lua_State* L, int table, WPE_Billboard base,
                                          const char** out_image) {
    luaL_checktype(L, table, LUA_TTABLE);
    lua_getfield(L, table, "image");
    *out_image = lua_isnil(L, -1) ? NULL : luaL_checkstring(L, -1);
    lua_pop(L, 1); /* The string stays valid: the table holds it. */
    lua_getfield(L, table, "size");
    if (!lua_isnil(L, -1)) {
        const WPE_Vec3 size = wpe__check_vec3(L, -1);
        if (!(size.x >= 0.0f && size.y >= 0.0f)) luaL_errorL(L, "size must not be negative");
        base.size = (WPE_Vec2){.x = size.x, .y = size.y};
    }
    lua_pop(L, 1);
    lua_getfield(L, table, "pivot");
    if (!lua_isnil(L, -1)) {
        const WPE_Vec3 pivot = wpe__check_vec3(L, -1);
        base.pivot = (WPE_Vec2){.x = pivot.x, .y = pivot.y};
    }
    lua_pop(L, 1);
    base.rotation = wpe__field_number(L, table, "rotation", base.rotation);
    lua_getfield(L, table, "color");
    if (!lua_isnil(L, -1)) base.color = wpe_color_to_vec4(wpe__opt_color(L, -1));
    lua_pop(L, 1);
    base.columns = wpe__field_count(L, table, "columns", 1, 256, base.columns);
    base.rows = wpe__field_count(L, table, "rows", 1, 256, base.rows);
    base.frame_count =
        wpe__field_count(L, table, "frame_count", 0, base.columns * base.rows, base.frame_count);
    const uint32_t count = base.frame_count ? base.frame_count : base.columns * base.rows;
    base.frame =
        wpe__field_count(L, table, "frame", 1, count, (base.frame < count ? base.frame : count - 1) + 1) - 1;
    base.fps = wpe__field_number(L, table, "fps", base.fps);
    if (!(base.fps >= 0.0f && base.fps <= 1000.0f)) luaL_errorL(L, "fps must be 0..1000");
    base.loop = wpe__field_bool(L, table, "loop", base.loop);
    base.mode = (WPE_BillboardMode)wpe__field_choice(L, table, "mode", wpe__billboard_modes,
                                                     WPE_BILLBOARD_SCREEN_ALIGNED + 1, base.mode);
    base.blend = (WPE_BillboardBlend)wpe__field_choice(L, table, "blend", wpe__billboard_blends,
                                                       WPE_BILLBOARD_ADDITIVE + 1, base.blend);
    base.lit = wpe__field_bool(L, table, "lit", base.lit);
    base.visible = wpe__field_bool(L, table, "visible", base.visible);
    return base;
}

/* Changes (or adds) an entity's billboard; a new image replaces the component, so its hooks take the
 * new reference. */
static int wpe__api_set_billboard(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    WPE_ScriptHost* host = wpe__script_host_from(L);
    const WPE_Billboard* existing = wpe_ecs_get_component(host->ecs, entity, WPE_COMP_BILLBOARD);
    const char* image = NULL;
    WPE_Billboard billboard =
        wpe__check_billboard(L, 2, existing ? *existing : wpe_billboard_default(), &image);
    existing =
        wpe_ecs_get_component(host->ecs, entity, WPE_COMP_BILLBOARD); /* Re-fetch after raising calls. */
    const bool replace = !existing || image;
    if (!replace) {
        *(WPE_Billboard*)existing = billboard;
        return 0;
    }
    wpe__check_unrestricted(L, "set_billboard");
    WPE_AssetHandle loaded = WPE_ASSET_NONE;
    if (image) {
        if (!wpe_vfs_path_is_valid(image))
            luaL_errorL(L, "wpe.set_billboard: '%s' is not an asset path", image);
        wpe__raise_result(L, "set_billboard", wpe_asset_load(host->assets, WPE_ASSET_IMAGE, image, &loaded));
        billboard.image = loaded;
    }
    /* Raising is unsafe while `loaded` is held: collect the result, release, then raise. */
    WPE_Result result = WPE_RESULT_OK;
    if (existing) result = wpe_ecs_remove_component(host->ecs, entity, WPE_COMP_BILLBOARD);
    if (result == WPE_RESULT_OK)
        result = wpe_ecs_add_component(host->ecs, entity, WPE_COMP_BILLBOARD, &billboard, NULL);
    wpe_asset_release(host->assets, loaded);
    wpe__raise_result(L, "set_billboard", result);
    return 0;
}

static int wpe__api_get_billboard(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    WPE_ScriptHost* host = wpe__script_host_from(L);
    const WPE_Billboard* found = wpe_ecs_get_component(host->ecs, entity, WPE_COMP_BILLBOARD);
    if (!found) {
        lua_pushnil(L);
        return 1;
    }
    const WPE_Billboard b = *found; /* Copy before pushing: pushes may run the GC but not move ECS data. */
    const char* image = wpe_asset_path(host->assets, b.image);
    lua_createtable(L, 0, 15);
    if (image) {
        lua_pushstring(L, image);
        lua_setfield(L, -2, "image");
    }
    wpe__push_vec3(L, wpe_vec3(b.size.x, b.size.y, 0));
    lua_setfield(L, -2, "size");
    wpe__push_vec3(L, wpe_vec3(b.pivot.x, b.pivot.y, 0));
    lua_setfield(L, -2, "pivot");
    lua_pushnumber(L, b.rotation);
    lua_setfield(L, -2, "rotation");
    lua_pushnumber(L, (double)wpe_color_from_vec4(b.color));
    lua_setfield(L, -2, "color");
    const struct {
        const char* key;
        double value;
    } numbers[] = {{"columns", b.columns},
                   {"rows", b.rows},
                   {"frame_count", b.frame_count},
                   {"frame", (double)b.frame + 1.0},
                   {"fps", b.fps}};
    for (size_t i = 0; i < sizeof numbers / sizeof numbers[0]; ++i) {
        lua_pushnumber(L, numbers[i].value);
        lua_setfield(L, -2, numbers[i].key);
    }
    lua_pushboolean(L, b.loop);
    lua_setfield(L, -2, "loop");
    lua_pushstring(L, wpe__billboard_modes[b.mode]);
    lua_setfield(L, -2, "mode");
    lua_pushstring(L, wpe__billboard_blends[b.blend]);
    lua_setfield(L, -2, "blend");
    lua_pushboolean(L, b.lit);
    lua_setfield(L, -2, "lit");
    lua_pushboolean(L, b.visible);
    lua_setfield(L, -2, "visible");
    return 1;
}

static int wpe__api_remove_billboard(lua_State* L) {
    WPE_ScriptHost* host = wpe__check_unrestricted(L, "remove_billboard");
    const WPE_Entity entity = wpe__check_entity(L, 1);
    if (wpe_ecs_has_component(host->ecs, entity, WPE_COMP_BILLBOARD))
        wpe__raise_result(L, "remove_billboard",
                          wpe_ecs_remove_component(host->ecs, entity, WPE_COMP_BILLBOARD));
    return 0;
}

/* ---- Particles ------------------------------------------------------------------------------------- */

/* A {low, high} field (nil keeps the values); raises unless both are numbers with low <= high. */
static void wpe__field_range(lua_State* L, int table, const char* key, float* io_low, float* io_high) {
    lua_getfield(L, table, key);
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TTABLE);
        lua_rawgeti(L, -1, 1);
        lua_rawgeti(L, -2, 2);
        if (!lua_isnumber(L, -2) || !lua_isnumber(L, -1)) luaL_errorL(L, "%s must be {low, high}", key);
        const float low = (float)lua_tonumber(L, -2), high = (float)lua_tonumber(L, -1);
        if (!(low <= high)) luaL_errorL(L, "%s must be {low, high} with low <= high", key);
        *io_low = low;
        *io_high = high;
        lua_pop(L, 2);
    }
    lua_pop(L, 1);
}

static WPE_ParticleEmitter wpe__check_particles(lua_State* L, int table, WPE_ParticleEmitter base) {
    luaL_checktype(L, table, LUA_TTABLE);
    base.playing = wpe__field_bool(L, table, "playing", base.playing);
    base.loop = wpe__field_bool(L, table, "loop", base.loop);
    base.world_space = wpe__field_bool(L, table, "world_space", base.world_space);
    base.random_rotation = wpe__field_bool(L, table, "random_rotation", base.random_rotation);
    base.rate = wpe__field_number(L, table, "rate", base.rate);
    base.duration = wpe__field_number(L, table, "duration", base.duration);
    base.drag = wpe__field_number(L, table, "drag", base.drag);
    base.spread = wpe__field_number(L, table, "spread", base.spread);
    if (!(base.rate >= 0.0f && base.duration >= 0.0f && base.drag >= 0.0f))
        luaL_errorL(L, "rate, duration and drag must not be negative");
    if (!(base.spread >= 0.0f && base.spread <= WPE_PI)) luaL_errorL(L, "spread must be 0..pi radians");
    base.burst = wpe__field_count(L, table, "burst", 0, 100000, base.burst);
    base.max_particles = wpe__field_count(L, table, "max_particles", 1, 100000, base.max_particles);
    wpe__field_range(L, table, "lifetime", &base.lifetime_min, &base.lifetime_max);
    if (!(base.lifetime_min > 0.0f)) luaL_errorL(L, "lifetime must be positive");
    wpe__field_range(L, table, "speed", &base.speed_min, &base.speed_max);
    wpe__field_range(L, table, "spin", &base.spin_min, &base.spin_max);
    float size_low = base.size_start, size_high = base.size_end;
    lua_getfield(L, table, "size"); /* {start, end}: a size may shrink, so not a low-high range. */
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TTABLE);
        lua_rawgeti(L, -1, 1);
        lua_rawgeti(L, -2, 2);
        size_low = (float)luaL_checknumber(L, -2);
        size_high = (float)luaL_checknumber(L, -1);
        if (!(size_low >= 0.0f && size_high >= 0.0f)) luaL_errorL(L, "size must not be negative");
        lua_pop(L, 2);
    }
    lua_pop(L, 1);
    base.size_start = size_low;
    base.size_end = size_high;
    const char* vectors[] = {"direction", "gravity"};
    WPE_Vec3* targets[] = {&base.direction, &base.gravity};
    for (int i = 0; i < 2; ++i) {
        lua_getfield(L, table, vectors[i]);
        if (!lua_isnil(L, -1)) *targets[i] = wpe__check_vec3(L, -1);
        lua_pop(L, 1);
    }
    const char* colors[] = {"color_start", "color_end"};
    WPE_Vec4* color_targets[] = {&base.color_start, &base.color_end};
    for (int i = 0; i < 2; ++i) {
        lua_getfield(L, table, colors[i]);
        if (!lua_isnil(L, -1)) *color_targets[i] = wpe_color_to_vec4(wpe__opt_color(L, -1));
        lua_pop(L, 1);
    }
    return base;
}

static WPE_ParticleEmitter* wpe__check_emitter(lua_State* L, WPE_Entity entity) {
    WPE_ParticleEmitter* e = wpe_ecs_get_component(wpe__script_host_from(L)->ecs, entity, WPE_COMP_PARTICLES);
    if (!e) luaL_errorL(L, "entity 0x%08x has no particle emitter", entity);
    return e;
}

/* Changes (or adds) an entity's emitter; live particles stay. */
static int wpe__api_set_particles(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    WPE_ScriptHost* host = wpe__script_host_from(L);
    const WPE_ParticleEmitter* existing = wpe_ecs_get_component(host->ecs, entity, WPE_COMP_PARTICLES);
    const WPE_ParticleEmitter settings =
        wpe__check_particles(L, 2, existing ? *existing : wpe_particle_emitter_default());
    WPE_ParticleEmitter* live = wpe_ecs_get_component(host->ecs, entity, WPE_COMP_PARTICLES); /* Re-fetch. */
    if (live) {
        *live = settings; /* The runtime fields were copied from `live` itself, so they stay valid. */
        return 0;
    }
    wpe__check_unrestricted(L, "set_particles");
    wpe__raise_result(L, "set_particles",
                      wpe_ecs_add_component(host->ecs, entity, WPE_COMP_PARTICLES, &settings, NULL));
    return 0;
}

static int wpe__api_get_particles(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    const WPE_ParticleEmitter* found =
        wpe_ecs_get_component(wpe__script_host_from(L)->ecs, entity, WPE_COMP_PARTICLES);
    if (!found) {
        lua_pushnil(L);
        return 1;
    }
    const WPE_ParticleEmitter e = *found; /* Copy before pushing. */
    lua_createtable(L, 0, 20);
    const struct {
        const char* key;
        bool value;
    } flags[] = {{"playing", e.playing},
                 {"loop", e.loop},
                 {"world_space", e.world_space},
                 {"random_rotation", e.random_rotation}};
    for (size_t i = 0; i < sizeof flags / sizeof flags[0]; ++i) {
        lua_pushboolean(L, flags[i].value);
        lua_setfield(L, -2, flags[i].key);
    }
    const struct {
        const char* key;
        double value;
    } numbers[] = {{"rate", e.rate},     {"duration", e.duration}, {"drag", e.drag},
                   {"spread", e.spread}, {"burst", e.burst},       {"max_particles", e.max_particles},
                   {"count", e.count}};
    for (size_t i = 0; i < sizeof numbers / sizeof numbers[0]; ++i) {
        lua_pushnumber(L, numbers[i].value);
        lua_setfield(L, -2, numbers[i].key);
    }
    const struct {
        const char* key;
        float low, high;
    } ranges[] = {{"lifetime", e.lifetime_min, e.lifetime_max},
                  {"speed", e.speed_min, e.speed_max},
                  {"spin", e.spin_min, e.spin_max},
                  {"size", e.size_start, e.size_end}};
    for (size_t i = 0; i < sizeof ranges / sizeof ranges[0]; ++i) {
        lua_createtable(L, 2, 0);
        lua_pushnumber(L, ranges[i].low);
        lua_rawseti(L, -2, 1);
        lua_pushnumber(L, ranges[i].high);
        lua_rawseti(L, -2, 2);
        lua_setfield(L, -2, ranges[i].key);
    }
    wpe__push_vec3(L, e.direction);
    lua_setfield(L, -2, "direction");
    wpe__push_vec3(L, e.gravity);
    lua_setfield(L, -2, "gravity");
    lua_pushnumber(L, (double)wpe_color_from_vec4(e.color_start));
    lua_setfield(L, -2, "color_start");
    lua_pushnumber(L, (double)wpe_color_from_vec4(e.color_end));
    lua_setfield(L, -2, "color_end");
    return 1;
}

static int wpe__api_emit_particles(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    const double count = luaL_checknumber(L, 2);
    if (!(count >= 0.0 && count <= 100000.0) || count != floor(count))
        luaL_errorL(L, "argument #2: a whole number of particles from 0 to 100000");
    wpe__check_emitter(L, entity);
    lua_pushnumber(L, wpe_particles_emit(wpe__script_host_from(L)->ecs, entity, (uint32_t)count));
    return 1;
}

static int wpe__api_play_particles(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    wpe__check_emitter(L, entity);
    wpe_particles_restart(wpe__script_host_from(L)->ecs, entity);
    return 0;
}

static int wpe__api_stop_particles(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    const bool clear = lua_isnoneornil(L, 2) ? false : luaL_checkboolean(L, 2);
    WPE_ParticleEmitter* e = wpe__check_emitter(L, entity);
    e->playing = false;
    e->started = false;
    if (clear) wpe_particles_clear(wpe__script_host_from(L)->ecs, entity);
    return 0;
}

static int wpe__api_particle_count(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    lua_pushnumber(L, wpe__check_emitter(L, entity)->count);
    return 1;
}

static int wpe__api_remove_particles(lua_State* L) {
    WPE_ScriptHost* host = wpe__check_unrestricted(L, "remove_particles");
    const WPE_Entity entity = wpe__check_entity(L, 1);
    if (wpe_ecs_has_component(host->ecs, entity, WPE_COMP_PARTICLES))
        wpe__raise_result(L, "remove_particles",
                          wpe_ecs_remove_component(host->ecs, entity, WPE_COMP_PARTICLES));
    return 0;
}

/* ---- Trails ---------------------------------------------------------------------------------------- */

static const char* const wpe__trail_blends[] = {"cutout", "alpha", "additive"};

/* The fields of a trail table over `base` (missing ones keep their values); raises on bad values. */
static WPE_Trail wpe__check_trail(lua_State* L, int table, WPE_Trail base) {
    luaL_checktype(L, table, LUA_TTABLE);
    const char* edges[] = {"base", "tip"};
    WPE_Entity* targets[] = {&base.base, &base.tip};
    for (int i = 0; i < 2; ++i) {
        lua_getfield(L, table, edges[i]);
        if (!lua_isnil(L, -1)) *targets[i] = wpe__check_entity(L, lua_gettop(L));
        lua_pop(L, 1);
    }
    base.emitting = wpe__field_bool(L, table, "emitting", base.emitting);
    base.visible = wpe__field_bool(L, table, "visible", base.visible);
    base.lifetime = wpe__field_number(L, table, "lifetime", base.lifetime);
    if (!(base.lifetime > 0.0f && base.lifetime <= 60.0f)) luaL_errorL(L, "lifetime must be 0..60 seconds");
    base.width_end = wpe__field_number(L, table, "width_end", base.width_end);
    if (!(base.width_end >= 0.0f && base.width_end <= 1.0f)) luaL_errorL(L, "width_end must be 0..1");
    base.smoothing = wpe__field_count(L, table, "smoothing", 1, 8, base.smoothing);
    const char* colors[] = {"color_start", "color_end"};
    WPE_Vec4* color_targets[] = {&base.color_start, &base.color_end};
    for (int i = 0; i < 2; ++i) {
        lua_getfield(L, table, colors[i]);
        if (!lua_isnil(L, -1)) *color_targets[i] = wpe_color_to_vec4(wpe__opt_color(L, -1));
        lua_pop(L, 1);
    }
    lua_getfield(L, table, "blend");
    if (!lua_isnil(L, -1)) {
        const char* blend = luaL_checkstring(L, -1);
        uint32_t found = UINT32_MAX;
        for (uint32_t i = 0; i < 3; ++i) {
            if (strcmp(blend, wpe__trail_blends[i]) == 0) found = i;
        }
        if (found == UINT32_MAX) luaL_errorL(L, "blend must be \"additive\", \"alpha\" or \"cutout\"");
        base.blend = (WPE_BillboardBlend)found;
    }
    lua_pop(L, 1);
    return base;
}

/* Changes (or adds) an entity's trail; recorded places stay. */
static int wpe__api_set_trail(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    WPE_ScriptHost* host = wpe__script_host_from(L);
    const WPE_Trail* existing = wpe_ecs_get_component(host->ecs, entity, WPE_COMP_TRAIL);
    const WPE_Trail settings = wpe__check_trail(L, 2, existing ? *existing : wpe_trail_default());
    WPE_Trail* live = wpe_ecs_get_component(host->ecs, entity, WPE_COMP_TRAIL); /* Re-fetch. */
    if (live) {
        *live = settings; /* Its samples were copied from `live` itself. */
        return 0;
    }
    wpe__check_unrestricted(L, "set_trail");
    wpe__raise_result(L, "set_trail",
                      wpe_ecs_add_component(host->ecs, entity, WPE_COMP_TRAIL, &settings, NULL));
    return 0;
}

static int wpe__api_get_trail(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    const WPE_Trail* found = wpe_ecs_get_component(wpe__script_host_from(L)->ecs, entity, WPE_COMP_TRAIL);
    if (!found) {
        lua_pushnil(L);
        return 1;
    }
    const WPE_Trail t = *found; /* Copy before pushing. */
    lua_createtable(L, 0, 11);
    const struct {
        const char* key;
        WPE_Entity value;
    } edges[] = {{"base", t.base}, {"tip", t.tip}};
    for (size_t i = 0; i < 2; ++i) {
        if (edges[i].value == WPE_ENTITY_NULL) {
            lua_pushnil(L);
        } else {
            lua_pushnumber(L, edges[i].value);
        }
        lua_setfield(L, -2, edges[i].key);
    }
    lua_pushboolean(L, t.emitting);
    lua_setfield(L, -2, "emitting");
    lua_pushboolean(L, t.visible);
    lua_setfield(L, -2, "visible");
    lua_pushnumber(L, t.lifetime);
    lua_setfield(L, -2, "lifetime");
    lua_pushnumber(L, t.width_end);
    lua_setfield(L, -2, "width_end");
    lua_pushnumber(L, t.smoothing);
    lua_setfield(L, -2, "smoothing");
    lua_pushnumber(L, (double)wpe_color_from_vec4(t.color_start));
    lua_setfield(L, -2, "color_start");
    lua_pushnumber(L, (double)wpe_color_from_vec4(t.color_end));
    lua_setfield(L, -2, "color_end");
    lua_pushstring(L, wpe__trail_blends[t.blend]);
    lua_setfield(L, -2, "blend");
    lua_pushnumber(L, t.sample_count);
    lua_setfield(L, -2, "samples");
    return 1;
}

static int wpe__api_clear_trail(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    if (!wpe_ecs_has_component(wpe__script_host_from(L)->ecs, entity, WPE_COMP_TRAIL))
        luaL_errorL(L, "entity 0x%08x has no trail", entity);
    wpe_trail_clear(wpe__script_host_from(L)->ecs, entity);
    return 0;
}

static int wpe__api_remove_trail(lua_State* L) {
    WPE_ScriptHost* host = wpe__check_unrestricted(L, "remove_trail");
    const WPE_Entity entity = wpe__check_entity(L, 1);
    if (wpe_ecs_has_component(host->ecs, entity, WPE_COMP_TRAIL))
        wpe__raise_result(L, "remove_trail", wpe_ecs_remove_component(host->ecs, entity, WPE_COMP_TRAIL));
    return 0;
}

/* ---- Slashes --------------------------------------------------------------------------------------- */

static WPE_Slash wpe__check_slash(lua_State* L, int table, WPE_Slash base) {
    luaL_checktype(L, table, LUA_TTABLE);
    lua_getfield(L, table, "points");
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TTABLE);
        if (lua_objlen(L, -1) != 4) luaL_errorL(L, "points must be four vectors");
        for (int i = 0; i < 4; ++i) {
            lua_rawgeti(L, -1, i + 1);
            base.points[i] = wpe__check_vec3(L, lua_gettop(L));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    base.width = wpe__field_number(L, table, "width", base.width);
    base.length = wpe__field_number(L, table, "length", base.length);
    base.duration = wpe__field_number(L, table, "duration", base.duration);
    if (!(base.width >= 0.0f)) luaL_errorL(L, "width must not be negative");
    if (!(base.length > 0.0f && base.length <= 1.0f)) luaL_errorL(L, "length must be 0..1 (above 0)");
    if (!(base.duration > 0.0f && base.duration <= 60.0f)) luaL_errorL(L, "duration must be 0..60 seconds");
    base.segments = wpe__field_count(L, table, "segments", 2, 64, base.segments);
    base.visible = wpe__field_bool(L, table, "visible", base.visible);
    base.even = wpe__field_bool(L, table, "even", base.even);
    base.distortion = wpe__field_number(L, table, "distortion", base.distortion);
    base.distortion_width = wpe__field_number(L, table, "distortion_width", base.distortion_width);
    if (!(fabsf(base.distortion) <= 1e3f)) luaL_errorL(L, "distortion must be -1000..1000 pixels");
    if (!(base.distortion_width >= 0.5f && base.distortion_width <= 1e3f))
        luaL_errorL(L, "distortion_width must be 0.5..1000 pixels");
    const char* colors[] = {"color_head", "color_tail", "distortion_tint"};
    WPE_Vec4* color_targets[] = {&base.color_head, &base.color_tail, &base.distortion_tint};
    for (int i = 0; i < 3; ++i) {
        lua_getfield(L, table, colors[i]);
        if (!lua_isnil(L, -1)) *color_targets[i] = wpe_color_to_vec4(wpe__opt_color(L, -1));
        lua_pop(L, 1);
    }
    lua_getfield(L, table, "head");
    if (!lua_isnil(L, -1)) base.head = wpe__check_entity(L, lua_gettop(L));
    lua_pop(L, 1);
    lua_getfield(L, table, "blend");
    if (!lua_isnil(L, -1)) {
        const char* blend = luaL_checkstring(L, -1);
        uint32_t found = UINT32_MAX;
        for (uint32_t i = 0; i < 3; ++i) {
            if (strcmp(blend, wpe__trail_blends[i]) == 0) found = i;
        }
        if (found == UINT32_MAX) luaL_errorL(L, "blend must be \"additive\", \"alpha\" or \"cutout\"");
        base.blend = (WPE_BillboardBlend)found;
    }
    lua_pop(L, 1);
    return base;
}

/* Changes (or adds) an entity's slash; a sweep in progress goes on. */
static int wpe__api_set_slash(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    WPE_ScriptHost* host = wpe__script_host_from(L);
    const WPE_Slash* existing = wpe_ecs_get_component(host->ecs, entity, WPE_COMP_SLASH);
    const WPE_Slash settings = wpe__check_slash(L, 2, existing ? *existing : wpe_slash_default());
    WPE_Slash* live = wpe_ecs_get_component(host->ecs, entity, WPE_COMP_SLASH); /* Re-fetch. */
    if (live) {
        *live = settings;
        return 0;
    }
    wpe__check_unrestricted(L, "set_slash");
    wpe__raise_result(L, "set_slash",
                      wpe_ecs_add_component(host->ecs, entity, WPE_COMP_SLASH, &settings, NULL));
    return 0;
}

static int wpe__api_get_slash(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    const WPE_Slash* found = wpe_ecs_get_component(wpe__script_host_from(L)->ecs, entity, WPE_COMP_SLASH);
    if (!found) {
        lua_pushnil(L);
        return 1;
    }
    const WPE_Slash s = *found; /* Copy before pushing. */
    lua_createtable(L, 0, 15);
    lua_createtable(L, 4, 0);
    for (int i = 0; i < 4; ++i) {
        wpe__push_vec3(L, s.points[i]);
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "points");
    const struct {
        const char* key;
        double value;
    } numbers[] = {{"width", s.width},
                   {"length", s.length},
                   {"duration", s.duration},
                   {"segments", s.segments},
                   {"time", s.time},
                   {"distortion", s.distortion},
                   {"distortion_width", s.distortion_width}};
    for (size_t i = 0; i < sizeof numbers / sizeof numbers[0]; ++i) {
        lua_pushnumber(L, numbers[i].value);
        lua_setfield(L, -2, numbers[i].key);
    }
    lua_pushboolean(L, s.playing);
    lua_setfield(L, -2, "playing");
    lua_pushboolean(L, s.visible);
    lua_setfield(L, -2, "visible");
    lua_pushboolean(L, s.even);
    lua_setfield(L, -2, "even");
    lua_pushnumber(L, (double)wpe_color_from_vec4(s.color_head));
    lua_setfield(L, -2, "color_head");
    lua_pushnumber(L, (double)wpe_color_from_vec4(s.color_tail));
    lua_setfield(L, -2, "color_tail");
    lua_pushnumber(L, (double)wpe_color_from_vec4(s.distortion_tint));
    lua_setfield(L, -2, "distortion_tint");
    lua_pushstring(L, wpe__trail_blends[s.blend]);
    lua_setfield(L, -2, "blend");
    if (s.head == WPE_ENTITY_NULL) {
        lua_pushnil(L);
    } else {
        lua_pushnumber(L, s.head);
    }
    lua_setfield(L, -2, "head");
    return 1;
}

static int wpe__api_play_slash(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    if (!wpe_ecs_has_component(wpe__script_host_from(L)->ecs, entity, WPE_COMP_SLASH))
        luaL_errorL(L, "entity 0x%08x has no slash", entity);
    wpe_slash_play(wpe__script_host_from(L)->ecs, entity);
    return 0;
}

static int wpe__api_remove_slash(lua_State* L) {
    WPE_ScriptHost* host = wpe__check_unrestricted(L, "remove_slash");
    const WPE_Entity entity = wpe__check_entity(L, 1);
    if (wpe_ecs_has_component(host->ecs, entity, WPE_COMP_SLASH))
        wpe__raise_result(L, "remove_slash", wpe_ecs_remove_component(host->ecs, entity, WPE_COMP_SLASH));
    return 0;
}

/* ---- UI -------------------------------------------------------------------------------------------- */

/* The entity's UI component of `type`; raises when it has none. */
static void* wpe__check_ui_part(lua_State* L, WPE_Entity entity, WPE_ComponentType type, const char* what) {
    void* component = wpe_ecs_get_component(wpe__script_host_from(L)->ecs, entity, type);
    if (!component) luaL_errorL(L, "entity 0x%08x has no %s", entity, what);
    return component;
}

static WPE_Ui* wpe__check_ui(lua_State* L) {
    WPE_Ui* ui = wpe__script_host_from(L)->ui;
    if (!ui) luaL_errorL(L, "the UI is not enabled for this game");
    return ui;
}

static int wpe__api_get_text(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    const WPE_Text* text = wpe__check_ui_part(L, entity, WPE_COMP_TEXT, "Text");
    lua_pushstring(L, text->text ? text->text : "");
    return 1;
}

static int wpe__api_set_text(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    const char* string = luaL_checkstring(L, 2);
    wpe__check_ui_part(L, entity, WPE_COMP_TEXT, "Text");
    wpe__raise_result(L, "set_text", wpe_ui_set_text(wpe__script_host_from(L)->ecs, entity, string));
    return 0;
}

static int wpe__api_set_text_color(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    luaL_checkany(L, 2);
    const WPE_Color color = wpe__opt_color(L, 2);
    WPE_Text* text = wpe__check_ui_part(L, entity, WPE_COMP_TEXT, "Text");
    text->color = wpe_color_to_vec4(color);
    return 0;
}

static int wpe__api_set_sprite_color(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    luaL_checkany(L, 2);
    const WPE_Color color = wpe__opt_color(L, 2);
    WPE_Sprite* sprite = wpe__check_ui_part(L, entity, WPE_COMP_SPRITE, "Sprite");
    sprite->color = wpe_color_to_vec4(color);
    return 0;
}

static int wpe__api_set_ui_visible(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    const bool visible = luaL_checkboolean(L, 2);
    WPE_UiRect* element = wpe__check_ui_part(L, entity, WPE_COMP_UI, "UI rectangle");
    element->visible = visible;
    return 0;
}

static int wpe__api_is_ui_visible(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    const WPE_UiRect* element = wpe__check_ui_part(L, entity, WPE_COMP_UI, "UI rectangle");
    lua_pushboolean(L, element->visible);
    return 1;
}

static int wpe__api_get_ui_offsets(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    const WPE_UiRect element = *(const WPE_UiRect*)wpe__check_ui_part(L, entity, WPE_COMP_UI, "UI rectangle");
    lua_pushnumber(L, element.offset_min.x);
    lua_pushnumber(L, element.offset_min.y);
    lua_pushnumber(L, element.offset_max.x);
    lua_pushnumber(L, element.offset_max.y);
    return 4;
}

static int wpe__api_set_ui_offsets(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    float values[4];
    for (int i = 0; i < 4; ++i) {
        values[i] = (float)luaL_checknumber(L, 2 + i);
        if (!(fabsf(values[i]) <= 1e6f)) luaL_errorL(L, "argument #%d: offsets are -1e6..1e6 pixels", 2 + i);
    }
    WPE_UiRect* element = wpe__check_ui_part(L, entity, WPE_COMP_UI, "UI rectangle");
    element->offset_min = (WPE_Vec2){.x = values[0], .y = values[1]};
    element->offset_max = (WPE_Vec2){.x = values[2], .y = values[3]};
    return 0;
}

/* The rectangle from the last layout (the frame before, during updates). */
static int wpe__api_ui_rect(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    const WPE_Rect r = ((const WPE_UiRect*)wpe__check_ui_part(L, entity, WPE_COMP_UI, "UI rectangle"))->rect;
    lua_pushnumber(L, r.x);
    lua_pushnumber(L, r.y);
    lua_pushnumber(L, r.width);
    lua_pushnumber(L, r.height);
    return 4;
}

static int wpe__api_set_interactable(lua_State* L) {
    const WPE_Entity entity = wpe__check_entity(L, 1);
    const bool interactable = luaL_checkboolean(L, 2);
    WPE_Button* button = wpe__check_ui_part(L, entity, WPE_COMP_BUTTON, "Button");
    button->interactable = interactable;
    return 0;
}

static int wpe__api_ui_focus(lua_State* L) {
    WPE_Ui* ui = wpe__check_ui(L);
    const WPE_Entity entity = lua_isnoneornil(L, 1) ? WPE_ENTITY_NULL : wpe__check_entity(L, 1);
    lua_pushboolean(L, wpe_ui_focus(ui, entity));
    return 1;
}

static int wpe__api_ui_focused(lua_State* L) {
    const WPE_Entity focused = wpe_ui_focused(wpe__check_ui(L));
    if (focused == WPE_ENTITY_NULL) {
        lua_pushnil(L);
    } else {
        lua_pushnumber(L, (double)focused);
    }
    return 1;
}

/* wpe.ui_value(entity, [value]): a toggle's on (boolean) or a slider's value (number); setting it
 * doesn't call on_change, which reports the player's changes. */
static int wpe__api_ui_value(lua_State* L) {
    const WPE_ScriptHost* host = wpe__script_host_from(L);
    const WPE_Entity entity = wpe__check_entity(L, 1);
    WPE_Toggle* toggle = wpe_ecs_get_component(host->ecs, entity, WPE_COMP_TOGGLE);
    WPE_Slider* slider = wpe_ecs_get_component(host->ecs, entity, WPE_COMP_SLIDER);
    if (!toggle && !slider) luaL_errorL(L, "wpe.ui_value: entity 0x%08x has no toggle or slider", entity);
    if (toggle) {
        if (!lua_isnoneornil(L, 2)) toggle->on = luaL_checkboolean(L, 2);
        lua_pushboolean(L, toggle->on);
        return 1;
    }
    if (!lua_isnoneornil(L, 2)) {
        const float value = wpe_slider_constrain(slider, (float)luaL_checknumber(L, 2));
        slider =
            wpe_ecs_get_component(host->ecs, entity, WPE_COMP_SLIDER); /* After the check, which may raise. */
        slider->value = value;
    }
    lua_pushnumber(L, slider->value);
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
    {"find", wpe__api_find},
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
    {"get_animation", wpe__api_get_animation},
    {"animation_speed", wpe__api_animation_speed},
    {"add_rigidbody", wpe__api_add_rigidbody},
    {"add_collider", wpe__api_add_collider},
    {"remove_rigidbody", wpe__api_remove_rigidbody},
    {"remove_collider", wpe__api_remove_collider},
    {"apply_force", wpe__api_apply_force},
    {"apply_impulse", wpe__api_apply_impulse},
    {"get_rigidbody", wpe__api_get_rigidbody},
    {"set_velocity", wpe__api_set_velocity},
    {"get_velocity", wpe__api_get_velocity},
    {"raycast", wpe__api_raycast},
    {"shape_cast", wpe__api_shape_cast},
    {"overlap", wpe__api_overlap},
    {"add_character", wpe__api_add_character},
    {"remove_character", wpe__api_remove_character},
    {"move_character", wpe__api_move_character},
    {"get_character", wpe__api_get_character},
    {"after", wpe__api_after},
    {"every", wpe__api_every},
    {"cancel", wpe__api_cancel},
    {"get_script", wpe__api_get_script},
    {"send", wpe__api_send},
    {"broadcast", wpe__script_broadcast},
    {"save_data", wpe__api_save_data},
    {"load_data", wpe__api_load_data},
    {"delete_data", wpe__api_delete_data},
    {"set_gravity", wpe__api_set_gravity},
    {"get_gravity", wpe__api_get_gravity},
    {"play_sound", wpe__api_play_sound},
    {"stop_sound", wpe__api_stop_sound},
    {"add_audio_source", wpe__api_add_audio_source},
    {"remove_audio_source", wpe__api_remove_audio_source},
    {"set_audio_playing", wpe__api_set_audio_playing},
    {"is_audio_playing", wpe__api_is_audio_playing},
    {"play_music", wpe__api_play_music},
    {"stop_music", wpe__api_stop_music},
    {"set_volume", wpe__api_set_volume},
    {"get_volume", wpe__api_get_volume},
    {"action_down", wpe__api_action_down},
    {"action_pressed", wpe__api_action_pressed},
    {"action_released", wpe__api_action_released},
    {"action_value", wpe__api_action_value},
    {"key_down", wpe__api_key_down},
    {"key_pressed", wpe__api_key_pressed},
    {"key_released", wpe__api_key_released},
    {"mouse_down", wpe__api_mouse_down},
    {"mouse_pressed", wpe__api_mouse_pressed},
    {"mouse_released", wpe__api_mouse_released},
    {"mouse_position", wpe__api_mouse_position},
    {"mouse_delta", wpe__api_mouse_delta},
    {"mouse_wheel", wpe__api_mouse_wheel},
    {"set_mouse_captured", wpe__api_set_mouse_captured},
    {"is_mouse_captured", wpe__api_is_mouse_captured},
    {"gamepad_count", wpe__api_gamepad_count},
    {"gamepad_connected", wpe__api_gamepad_connected},
    {"gamepad_name", wpe__api_gamepad_name},
    {"gamepad_down", wpe__api_gamepad_down},
    {"gamepad_pressed", wpe__api_gamepad_pressed},
    {"gamepad_released", wpe__api_gamepad_released},
    {"gamepad_axis", wpe__api_gamepad_axis},
    {"rumble", wpe__api_rumble},
    {"load_scene", wpe__api_load_scene},
    {"quit", wpe__api_quit},
    {"set_camera", wpe__api_set_camera},
    {"get_camera", wpe__api_get_camera},
    {"screen_size", wpe__api_screen_size},
    {"capture_screen", wpe__api_capture_screen},
    {"world_to_screen", wpe__api_world_to_screen},
    {"get_postfx", wpe__api_get_postfx},
    {"set_postfx", wpe__api_set_postfx},
    {"add_postfx", wpe__api_add_postfx},
    {"remove_postfx", wpe__api_remove_postfx},
    {"set_texture_filter", wpe__api_set_texture_filter},
    {"get_texture_filter", wpe__api_get_texture_filter},
    {"color", wpe__api_color},
    {"draw_text", wpe__api_draw_text},
    {"draw_rect", wpe__api_draw_rect},
    {"text_size", wpe__api_text_size},
    {"set_billboard", wpe__api_set_billboard},
    {"get_billboard", wpe__api_get_billboard},
    {"remove_billboard", wpe__api_remove_billboard},
    {"set_particles", wpe__api_set_particles},
    {"get_particles", wpe__api_get_particles},
    {"emit_particles", wpe__api_emit_particles},
    {"play_particles", wpe__api_play_particles},
    {"stop_particles", wpe__api_stop_particles},
    {"particle_count", wpe__api_particle_count},
    {"remove_particles", wpe__api_remove_particles},
    {"set_trail", wpe__api_set_trail},
    {"get_trail", wpe__api_get_trail},
    {"clear_trail", wpe__api_clear_trail},
    {"remove_trail", wpe__api_remove_trail},
    {"set_slash", wpe__api_set_slash},
    {"get_slash", wpe__api_get_slash},
    {"play_slash", wpe__api_play_slash},
    {"remove_slash", wpe__api_remove_slash},
    {"get_text", wpe__api_get_text},
    {"set_text", wpe__api_set_text},
    {"set_text_color", wpe__api_set_text_color},
    {"set_sprite_color", wpe__api_set_sprite_color},
    {"set_ui_visible", wpe__api_set_ui_visible},
    {"is_ui_visible", wpe__api_is_ui_visible},
    {"get_ui_offsets", wpe__api_get_ui_offsets},
    {"set_ui_offsets", wpe__api_set_ui_offsets},
    {"ui_rect", wpe__api_ui_rect},
    {"set_interactable", wpe__api_set_interactable},
    {"ui_focus", wpe__api_ui_focus},
    {"ui_focused", wpe__api_ui_focused},
    {"ui_value", wpe__api_ui_value},
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
    static const struct {
        const char* name;
        int value;
    } constants[] = {
        {"BODY_STATIC", WPE_BODY_STATIC},         {"BODY_KINEMATIC", WPE_BODY_KINEMATIC},
        {"BODY_DYNAMIC", WPE_BODY_DYNAMIC},       {"COLLIDER_BOX", WPE_COLLIDER_BOX},
        {"COLLIDER_SPHERE", WPE_COLLIDER_SPHERE}, {"COLLIDER_CAPSULE", WPE_COLLIDER_CAPSULE},
        {"COLLIDER_CONVEX", WPE_COLLIDER_CONVEX}, {"COLLIDER_MESH", WPE_COLLIDER_MESH},
        {"BUS_MASTER", WPE_AUDIO_BUS_MASTER},     {"BUS_EFFECTS", WPE_AUDIO_BUS_EFFECTS},
        {"BUS_MUSIC", WPE_AUDIO_BUS_MUSIC},
    };
    for (size_t i = 0; i < sizeof constants / sizeof constants[0]; ++i) {
        lua_pushnumber(L, constants[i].value);
        lua_setfield(L, -2, constants[i].name);
    }
    lua_setglobal(L, "wpe");

    lua_pushcfunction(L, wpe__api_log, "print");
    lua_setglobal(L, "print");
    lua_pushcfunction(L, wpe__script_require, "require");
    lua_setglobal(L, "require");
}
