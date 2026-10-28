/*
 * Scene files: saving the world to JSON and loading it back (format in docs/scenes.md).
 *
 * Loading validates everything before it reaches a component (indices against the referenced
 * model, enum names, ranges, entity references), because renderers and simulations trust their
 * components. Any failure rolls the whole load back.
 */
#define WPE_LOG_MODULE "scene"
#include "engine/scene_file.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio.h"
#include "core/alloc.h"
#include "core/debug.h"
#include "core/log.h"
#include "engine/json_read.h"
#include "engine/prefab.h"
#include "engine/scene_file_internal.h"
#include "graphics/graphics.h"
#include "physics/physics.h"
#include "platform/platform.h"
#include "scene/scene.h"

#define WPE__DEG_PER_RAD (180.0 / 3.14159265358979323846)
#define WPE__ANGLE_DIGITS 6 /* Degrees are written rounded: "60" rather than "59.9999986". */

/* ---- Names of enumerations ---------------------------------------------------------------------------- */

static const char* const wpe__light_types[] = {"directional", "point", "spot"};
static const char* const wpe__body_types[] = {"static", "kinematic", "dynamic"};
static const char* const wpe__collider_shapes[] = {"box", "sphere", "capsule", "convex", "mesh"};
static const char* const wpe__buses[] = {"master", "effects", "music"};
static const char* const wpe__fog_modes[] = {"none", "linear", "exponential"};
static const char* const wpe__sky_modes[] = {"color", "gradient", "panorama"};

#define WPE__COUNT(array) ((uint32_t)(sizeof(array) / sizeof((array)[0])))

static_assert(WPE__COUNT(wpe__light_types) == WPE_LIGHT_SPOT + 1, "light type names");
static_assert(WPE__COUNT(wpe__body_types) == WPE_BODY_DYNAMIC + 1, "body type names");
static_assert(WPE__COUNT(wpe__collider_shapes) == WPE_COLLIDER_MESH + 1, "collider shape names");
static_assert(WPE__COUNT(wpe__buses) == WPE_AUDIO_BUS_COUNT, "bus names");
static_assert(WPE__COUNT(wpe__fog_modes) == WPE_FOG_EXPONENTIAL + 1, "fog mode names");

/* ---- Environment ---------------------------------------------------------------------------------- */

WPE_SceneEnvironment wpe_scene_environment_default(void) {
    return (WPE_SceneEnvironment){.clear_color = wpe_vec3(0.11f, 0.125f, 0.17f),
                                  .render = wpe_scene_render_settings_default(),
                                  .gravity = wpe_vec3(0.0f, -9.81f, 0.0f),
                                  .music_fade = 1.0f};
}

uint32_t wpe_scene_environment_effects(const WPE_SceneEnvironment* environment, WPE_PostEffect* out_effects) {
    if (!environment || !out_effects) return 0;
    const uint32_t count =
        environment->postfx_count < WPE_SCENE_MAX_POSTFX ? environment->postfx_count : WPE_SCENE_MAX_POSTFX;
    for (uint32_t i = 0; i < count; ++i)
        out_effects[i] = wpe_postfx_from_config(&environment->postfx[i]);
    return count;
}

/* ---- Entity ids ------------------------------------------------------------------------------------- */

int wpe__compare_by_entity(const void* a, const void* b) {
    const WPE_Entity x = ((const wpe__IdPair*)a)->entity, y = ((const wpe__IdPair*)b)->entity;
    return (x > y) - (x < y);
}

int wpe__compare_by_id(const void* a, const void* b) {
    const uint32_t x = ((const wpe__IdPair*)a)->id, y = ((const wpe__IdPair*)b)->id;
    return (x > y) - (x < y);
}

uint32_t wpe_scene_ids_to_file(const WPE_SceneIds* ids, WPE_Entity entity) {
    if (ids && ids->identity) return wpe_ecs_is_alive(ids->identity, entity) ? entity : 0;
    if (!ids || !ids->by_entity || entity == WPE_ENTITY_NULL) return 0;
    const wpe__IdPair key = {.entity = entity};
    const wpe__IdPair* pair = bsearch(&key, ids->by_entity, ids->count, sizeof key, wpe__compare_by_entity);
    return pair ? pair->id : 0;
}

WPE_Entity wpe_scene_ids_to_entity(const WPE_SceneIds* ids, uint32_t id) {
    if (ids && ids->identity) return wpe_ecs_is_alive(ids->identity, id) ? id : WPE_ENTITY_NULL;
    if (!ids || !ids->by_id || id == 0) return WPE_ENTITY_NULL;
    const wpe__IdPair key = {.id = id};
    const wpe__IdPair* pair = bsearch(&key, ids->by_id, ids->count, sizeof key, wpe__compare_by_id);
    return pair ? pair->entity : WPE_ENTITY_NULL;
}

/* ---- Saving --------------------------------------------------------------------------------------- */

static void wpe__write_vec3(WPE_JsonWriter* w, const char* key, WPE_Vec3 v) {
    wpe_json_write_key(w, key);
    wpe_json_write_array_begin(w, true);
    for (int i = 0; i < 3; ++i)
        wpe_json_write_number(w, v.v[i]);
    wpe_json_write_array_end(w);
}

static void wpe__write_number(WPE_JsonWriter* w, const char* key, double value) {
    wpe_json_write_key(w, key);
    wpe_json_write_number(w, value);
}

static void wpe__write_degrees(WPE_JsonWriter* w, const char* key, float radians) {
    wpe_json_write_key(w, key);
    wpe_json_write_rounded(w, (double)radians * WPE__DEG_PER_RAD, WPE__ANGLE_DIGITS);
}

static void wpe__write_bool(WPE_JsonWriter* w, const char* key, bool value) {
    wpe_json_write_key(w, key);
    wpe_json_write_bool(w, value);
}

static void wpe__write_string(WPE_JsonWriter* w, const char* key, const char* value) {
    wpe_json_write_key(w, key);
    wpe_json_write_string(w, value);
}

static void wpe__write_integer(WPE_JsonWriter* w, const char* key, int64_t value) {
    wpe_json_write_key(w, key);
    wpe_json_write_integer(w, value);
}

static void wpe__write_ref(wpe__Saver* s, const char* key, WPE_Entity entity) {
    wpe_json_write_key(s->w, key);
    const uint32_t id = wpe_scene_ids_to_file(&s->ids, entity);
    if (id) {
        wpe_json_write_integer(s->w, id);
    } else {
        wpe_json_write_null(s->w);
    }
}

static void wpe__write_refs(wpe__Saver* s, const char* key, const WPE_Entity* entities, uint32_t count) {
    wpe_json_write_key(s->w, key);
    wpe_json_write_array_begin(s->w, true);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t id = wpe_scene_ids_to_file(&s->ids, entities[i]);
        if (id) {
            wpe_json_write_integer(s->w, id);
        } else {
            wpe_json_write_null(s->w);
        }
    }
    wpe_json_write_array_end(s->w);
}

void wpe__write_asset(wpe__Saver* s, const char* key, WPE_AssetHandle handle) {
    wpe_json_write_key(s->w, key);
    const char* path = wpe_asset_path(s->context->assets, handle);
    if (path) {
        wpe_json_write_string(s->w, path);
    } else {
        wpe_json_write_null(s->w);
    }
}

static void wpe__save_environment(WPE_JsonWriter* w, const WPE_SceneEnvironment* env) {
    const WPE_SceneRenderSettings* r = &env->render;
    wpe_json_write_object_begin(w);
    wpe__write_vec3(w, "clear_color", env->clear_color);
    wpe__write_vec3(w, "ambient", r->ambient);
    wpe_json_write_key(w, "fog");
    wpe_json_write_object_begin(w);
    wpe__write_string(w, "mode", wpe__fog_modes[r->fog.mode]);
    wpe__write_vec3(w, "color", r->fog.color);
    wpe__write_number(w, "start", r->fog.start);
    wpe__write_number(w, "end", r->fog.end);
    wpe__write_number(w, "density", r->fog.density);
    wpe_json_write_object_end(w);
    wpe_json_write_key(w, "sky");
    wpe_json_write_object_begin(w);
    wpe__write_string(w, "mode", wpe__sky_modes[r->sky.mode]);
    wpe__write_vec3(w, "zenith", r->sky.zenith);
    wpe__write_vec3(w, "horizon", r->sky.horizon);
    wpe__write_vec3(w, "ground", r->sky.ground);
    wpe__write_number(w, "curve", r->sky.curve);
    wpe_json_write_key(w, "image");
    if (env->sky_image[0]) {
        wpe_json_write_string(w, env->sky_image);
    } else {
        wpe_json_write_null(w);
    }
    wpe__write_number(w, "rotation", r->sky.rotation);
    wpe__write_number(w, "sun_size", r->sky.sun_size);
    wpe__write_vec3(w, "sun_color", r->sky.sun_color);
    wpe__write_number(w, "sun_glow", r->sky.sun_glow);
    wpe_json_write_object_end(w);
    wpe_json_write_key(w, "shadows");
    wpe_json_write_object_begin(w);
    wpe__write_bool(w, "enabled", r->shadows.enabled);
    wpe__write_integer(w, "map_size", r->shadows.map_size);
    wpe__write_number(w, "distance", r->shadows.distance);
    wpe__write_number(w, "caster_extension", r->shadows.caster_extension);
    wpe__write_number(w, "bias", r->shadows.bias);
    wpe_json_write_object_end(w);
    wpe__write_bool(w, "dither_transparency", r->dither_transparency);
    wpe__write_bool(w, "frustum_culling", r->frustum_culling);
    wpe_json_write_key(w, "subdivision");
    wpe_json_write_object_begin(w);
    wpe__write_integer(w, "levels", r->subdivision_levels);
    wpe__write_number(w, "edge_pixels", r->subdivision_edge_pixels);
    wpe_json_write_object_end(w);
    wpe_json_write_key(w, "default_sun");
    wpe_json_write_object_begin(w);
    wpe__write_bool(w, "enabled", r->default_sun);
    wpe__write_vec3(w, "direction", r->default_sun_direction);
    wpe__write_vec3(w, "color", r->default_sun_color);
    wpe_json_write_object_end(w);
    wpe__write_vec3(w, "gravity", env->gravity);
    if (env->music[0]) {
        wpe_json_write_key(w, "music");
        wpe_json_write_object_begin(w);
        wpe__write_string(w, "path", env->music);
        wpe__write_number(w, "fade", env->music_fade);
        wpe_json_write_object_end(w);
    }
    wpe_json_write_key(w, "postfx");
    wpe_json_write_array_begin(w, false);
    for (uint32_t i = 0; i < env->postfx_count && i < WPE_SCENE_MAX_POSTFX; ++i) {
        const WPE_PostFxConfig* c = &env->postfx[i];
        wpe_json_write_object_begin(w);
        wpe__write_string(w, "effect", wpe_postfx_kind_name(c->kind));
        wpe__write_bool(w, "enabled", c->enabled);
        uint32_t count = 0;
        const WPE_PostFxParam* params = wpe_postfx_params(c->kind, &count);
        for (uint32_t p = 0; p < count; ++p) {
            const double value = wpe_postfx_param_get(c, &params[p]);
            if (params[p].integer) {
                wpe__write_integer(w, params[p].name, (uint32_t)value);
            } else {
                wpe__write_number(w, params[p].name, (float)value);
            }
        }
        wpe_json_write_object_end(w);
    }
    wpe_json_write_array_end(w);
    wpe_json_write_object_end(w);
}

static void wpe__save_script_property(const char* name, const WPE_ScriptValue* value, void* user_data) {
    WPE_JsonWriter* w = user_data;
    wpe_json_write_key(w, name);
    switch (value->type) {
    case WPE_SCRIPT_VALUE_NUMBER:
        wpe_json_write_double(w, value->number); /* Luau numbers are doubles. */
        break;
    case WPE_SCRIPT_VALUE_BOOL:
        wpe_json_write_bool(w, value->boolean);
        break;
    case WPE_SCRIPT_VALUE_STRING:
        wpe_json_write_string(w, value->string);
        break;
    case WPE_SCRIPT_VALUE_VECTOR:
        wpe_json_write_array_begin(w, true);
        for (int i = 0; i < 3; ++i)
            wpe_json_write_number(w, value->vector.v[i]);
        wpe_json_write_array_end(w);
        break;
    case WPE_SCRIPT_VALUE_NIL:
        wpe_json_write_null(w); /* Not reported by the visitor. */
        break;
    }
}

/* "transform" (nothing when `t` is NULL). The rotation is written as stored, so load-save keeps it. */
static void wpe__save_transform(WPE_JsonWriter* w, const WPE_Transform* t) {
    if (!t) return;
    wpe_json_write_key(w, "transform");
    wpe_json_write_object_begin(w);
    wpe__write_vec3(w, "position", t->position);
    wpe_json_write_key(w, "rotation");
    wpe_json_write_array_begin(w, true);
    for (int i = 0; i < 4; ++i)
        wpe_json_write_number(w, t->rotation.v[i]);
    wpe_json_write_array_end(w);
    wpe__write_vec3(w, "scale", t->scale);
    wpe_json_write_object_end(w);
}

void wpe__save_entity(wpe__Saver* s, WPE_Entity e) {
    WPE_Ecs* ecs = s->context->ecs;
    WPE_JsonWriter* w = s->w;
    wpe_json_write_object_begin(w);
    wpe__write_integer(w, "id", wpe_scene_ids_to_file(&s->ids, e));
    const char* name = wpe_scene_get_name(ecs, e);
    if (name) wpe__write_string(w, "name", name);
    const WPE_Entity parent = wpe_scene_get_parent(ecs, e);
    if (parent != WPE_ENTITY_NULL) wpe__write_ref(s, "parent", parent);

    wpe__save_transform(w, wpe_ecs_get_component(ecs, e, WPE_COMP_TRANSFORM));
    const WPE_MeshRenderer* mesh = wpe_ecs_get_component(ecs, e, WPE_COMP_MESH);
    if (mesh) {
        wpe_json_write_key(w, "mesh");
        wpe_json_write_object_begin(w);
        wpe__write_asset(s, "model", mesh->model_asset);
        wpe__write_integer(w, "mesh", mesh->mesh);
        if (mesh->skin != WPE_MODEL_NONE) {
            wpe__write_integer(w, "skin", mesh->skin);
            wpe__write_refs(s, "joints", mesh->joints, mesh->joint_count);
        }
        wpe__write_bool(w, "visible", mesh->visible);
        wpe__write_bool(w, "unlit", mesh->unlit);
        wpe__write_bool(w, "cast_shadows", mesh->cast_shadows);
        wpe__write_bool(w, "receive_shadows", mesh->receive_shadows);
        wpe__write_bool(w, "double_sided", mesh->double_sided);
        wpe_json_write_object_end(w);
    }
    const WPE_Camera* camera = wpe_ecs_get_component(ecs, e, WPE_COMP_CAMERA);
    if (camera) {
        wpe_json_write_key(w, "camera");
        wpe_json_write_object_begin(w);
        wpe__write_degrees(w, "fov", camera->fov_y);
        wpe__write_number(w, "near", camera->near_z);
        wpe__write_number(w, "far", camera->far_z);
        wpe__write_bool(w, "orthographic", camera->orthographic);
        wpe__write_number(w, "ortho_height", camera->ortho_height);
        wpe_json_write_object_end(w);
    }
    const WPE_Light* light = wpe_ecs_get_component(ecs, e, WPE_COMP_LIGHT);
    if (light) {
        wpe_json_write_key(w, "light");
        wpe_json_write_object_begin(w);
        wpe__write_string(w, "type", wpe__light_types[light->type]);
        wpe__write_vec3(w, "color", light->color);
        wpe__write_number(w, "intensity", light->intensity);
        wpe__write_number(w, "range", light->range);
        wpe__write_degrees(w, "inner_cone", light->inner_cone);
        wpe__write_degrees(w, "outer_cone", light->outer_cone);
        wpe__write_bool(w, "cast_shadows", light->cast_shadows);
        wpe_json_write_object_end(w);
    }
    wpe__save_billboard(s, e);
    wpe__save_particles(s, e);
    const WPE_Animator* animator = wpe_ecs_get_component(ecs, e, WPE_COMP_ANIMATOR);
    if (animator) {
        wpe_json_write_key(w, "animator");
        wpe_json_write_object_begin(w);
        wpe__write_asset(s, "model", animator->model_asset);
        wpe__write_integer(w, "animation", animator->animation);
        wpe__write_number(w, "time", animator->time);
        wpe__write_number(w, "speed", animator->speed);
        wpe__write_bool(w, "playing", animator->playing);
        wpe__write_bool(w, "loop", animator->loop);
        wpe__write_refs(s, "nodes", animator->node_entities, animator->node_count);
        wpe_json_write_object_end(w);
    }
    const WPE_RigidBody* body = wpe_ecs_get_component(ecs, e, WPE_COMP_PHYSICS_RIGIDBODY);
    if (body) {
        wpe_json_write_key(w, "rigidbody");
        wpe_json_write_object_begin(w);
        wpe__write_string(w, "type", wpe__body_types[body->type]);
        wpe__write_number(w, "linear_damping", body->linear_damping);
        wpe__write_number(w, "angular_damping", body->angular_damping);
        wpe__write_number(w, "gravity_scale", body->gravity_scale);
        wpe__write_bool(w, "lock_rotation", body->lock_rotation);
        wpe__write_bool(w, "continuous", body->continuous);
        wpe__write_vec3(w, "initial_velocity", body->initial_velocity);
        wpe_json_write_object_end(w);
    }
    const WPE_Collider* collider = wpe_ecs_get_component(ecs, e, WPE_COMP_PHYSICS_COLLIDER);
    if (collider) {
        wpe_json_write_key(w, "collider");
        wpe_json_write_object_begin(w);
        wpe__write_string(w, "shape", wpe__collider_shapes[collider->shape]);
        wpe__write_vec3(w, "size", collider->size);
        wpe__write_vec3(w, "offset", collider->offset);
        if (collider->shape == WPE_COLLIDER_CONVEX || collider->shape == WPE_COLLIDER_MESH) {
            wpe__write_asset(s, "model", collider->model);
            wpe__write_integer(w, "mesh", collider->mesh);
        }
        wpe__write_number(w, "density", collider->density);
        wpe__write_number(w, "friction", collider->friction);
        wpe__write_number(w, "restitution", collider->restitution);
        wpe__write_bool(w, "trigger", collider->is_trigger);
        wpe_json_write_object_end(w);
    }
    const WPE_CharacterController* character = wpe_ecs_get_component(ecs, e, WPE_COMP_CHARACTER);
    if (character) {
        wpe_json_write_key(w, "character");
        wpe_json_write_object_begin(w);
        wpe__write_number(w, "radius", character->radius);
        wpe__write_number(w, "height", character->height);
        wpe__write_vec3(w, "offset", character->offset);
        wpe__write_number(w, "max_slope", character->max_slope);
        wpe__write_number(w, "step_height", character->step_height);
        wpe__write_number(w, "snap_distance", character->snap_distance);
        wpe_json_write_object_end(w);
    }
    const WPE_AudioSource* source = wpe_ecs_get_component(ecs, e, WPE_COMP_AUDIO_SOURCE);
    if (source) {
        wpe_json_write_key(w, "audio_source");
        wpe_json_write_object_begin(w);
        wpe__write_asset(s, "sound", source->sound);
        wpe__write_number(w, "volume", source->volume);
        wpe__write_number(w, "pitch", source->pitch);
        wpe__write_number(w, "min_distance", source->min_distance);
        wpe__write_number(w, "max_distance", source->max_distance);
        wpe__write_string(w, "bus", wpe__buses[source->bus]);
        wpe__write_bool(w, "loop", source->loop);
        wpe__write_bool(w, "spatial", source->spatial);
        wpe__write_bool(w, "playing", source->playing);
        wpe_json_write_object_end(w);
    }
    wpe__save_ui_components(s, e);
    if (wpe_ecs_has_component(ecs, e, WPE_COMP_SCRIPT)) {
        const char* path = wpe_script_path(s->context->scripts, e);
        if (!path) {
            wpe_log_error("entity 0x%08x has a script, but no script host was given to save it", e);
            s->result = WPE_RESULT_ERROR_INVALID_STATE;
        } else {
            wpe_json_write_key(w, "script");
            wpe_json_write_object_begin(w);
            wpe__write_string(w, "path", path);
            wpe_json_write_key(w, "properties");
            wpe_json_write_object_begin(w);
            wpe_script_visit_properties(s->context->scripts, e, wpe__save_script_property, w);
            wpe_json_write_object_end(w);
            wpe_json_write_object_end(w);
        }
    }
    bool components_open = false;
    for (uint32_t i = 0; i < s->context->serializer_count && s->result == WPE_RESULT_OK; ++i) {
        const WPE_ComponentSerializer* serializer = &s->context->serializers[i];
        const void* component = wpe_ecs_get_component(ecs, e, serializer->type);
        if (!component) continue;
        if (!components_open) {
            wpe_json_write_key(w, "components");
            wpe_json_write_object_begin(w);
            components_open = true;
        }
        wpe_json_write_key(w, serializer->name);
        const WPE_Result result =
            serializer->save(s->context, &s->ids, e, component, w, serializer->user_data);
        if (result != WPE_RESULT_OK) {
            wpe_log_error("saving component '%s' of entity 0x%08x failed: %s", serializer->name, e,
                          wpe_result_to_string(result));
            s->result = result;
        }
    }
    if (components_open) wpe_json_write_object_end(w);
    if (s->describe_prefabs && wpe__prefab_root(ecs, e) == e) {
        wpe_json_write_key(w, "prefab");
        wpe__prefab_save_reference(s, e, false);
    }
    wpe_json_write_object_end(w);
}

/* A prefab instance in a scene: its own name, parent and transform, and the prefab reference (members
 * are not written; they come from the prefab). */
static void wpe__save_instance(wpe__Saver* s, WPE_Entity root) {
    WPE_Ecs* ecs = s->context->ecs;
    WPE_JsonWriter* w = s->w;
    wpe_json_write_object_begin(w);
    wpe__write_integer(w, "id", wpe_scene_ids_to_file(&s->ids, root));
    const char* name = wpe_scene_get_name(ecs, root);
    if (name) wpe__write_string(w, "name", name);
    const WPE_Entity parent = wpe_scene_get_parent(ecs, root);
    if (parent != WPE_ENTITY_NULL) wpe__write_ref(s, "parent", parent);
    wpe__save_transform(w, wpe_ecs_get_component(ecs, root, WPE_COMP_TRANSFORM));
    wpe_json_write_key(w, "prefab");
    wpe__prefab_save_reference(s, root, true);
    wpe_json_write_object_end(w);
}

typedef struct wpe__Spawned {
    uint64_t serial;
    WPE_Entity entity;
} wpe__Spawned;

static int wpe__compare_spawn_order(const void* a, const void* b) {
    const uint64_t x = ((const wpe__Spawned*)a)->serial, y = ((const wpe__Spawned*)b)->serial;
    return (x > y) - (x < y);
}

static bool wpe__included(const WPE_SceneContext* context, WPE_Entity entity) {
    return !context->include || context->include(entity, context->include_user_data);
}

/* Appends `root` and its included descendants (depth first, child order) to `order`. */
static uint32_t wpe__append_subtree(const WPE_SceneContext* context, WPE_Entity root, WPE_Entity* order,
                                    uint32_t count, WPE_Entity* stack) {
    WPE_Ecs* ecs = context->ecs;
    uint32_t top = 0;
    stack[top++] = root;
    while (top > 0) {
        const WPE_Entity e = stack[--top];
        order[count++] = e;
        const WPE_Hierarchy* h = wpe_ecs_get_component(ecs, e, WPE_COMP_HIERARCHY);
        if (!h || h->child_count == 0) continue;
        /* Push children in reverse so the first child is visited first. */
        const uint32_t first = top;
        for (WPE_Entity c = h->first_child; c != WPE_ENTITY_NULL;) {
            if (wpe__included(context, c)) stack[top++] = c;
            const WPE_Hierarchy* ch = wpe_ecs_get_component(ecs, c, WPE_COMP_HIERARCHY);
            c = ch ? ch->next_sibling : WPE_ENTITY_NULL;
        }
        for (uint32_t i = first, j = top - 1; i < j; ++i, --j) {
            const WPE_Entity swap = stack[i];
            stack[i] = stack[j];
            stack[j] = swap;
        }
    }
    return count;
}

WPE_Result wpe_scene_save(const WPE_SceneContext* context, const WPE_SceneEnvironment* environment,
                          WPE_Entity camera, WPE_JsonWriter* writer) {
    if (!context || !context->ecs || !context->assets || !environment || !writer)
        return WPE_RESULT_ERROR_NULL_ARG;
    WPE_Ecs* ecs = context->ecs;
    const uint32_t count = wpe_ecs_entity_count(ecs);
    wpe__Spawned* alive = wpe_alloc_zeroed(count ? count : 1, sizeof *alive);
    WPE_Entity* order = wpe_alloc_zeroed(count ? count : 1, sizeof *order);
    WPE_Entity* stack = wpe_alloc_zeroed(count ? count : 1, sizeof *stack);
    wpe__Saver s = {.context = context, .w = writer};
    s.ids.by_entity = wpe_alloc_zeroed(count ? count : 1, sizeof *s.ids.by_entity);
    if (!alive || !order || !stack || !s.ids.by_entity) {
        s.result = WPE_RESULT_ERROR_OUT_OF_MEMORY;
        goto done;
    }

    /* Roots are written in creation order, which loading reproduces (it creates entities in file
     * order), so load-save cycles keep files stable. */
    uint32_t n = 0;
    WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){0});
    while (wpe_ecs_iter_next(&it) && n < count) {
        alive[n] = (wpe__Spawned){.serial = wpe_ecs_spawn_serial(ecs, it.entity), .entity = it.entity};
        ++n;
    }
    qsort(alive, n, sizeof *alive, wpe__compare_spawn_order);
    uint32_t ordered = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (wpe_scene_get_parent(ecs, alive[i].entity) == WPE_ENTITY_NULL &&
            wpe__included(context, alive[i].entity))
            ordered = wpe__append_subtree(context, alive[i].entity, order, ordered, stack);
    }
    wpe_assert(ordered <= n);
    for (uint32_t i = 0; i < ordered; ++i)
        s.ids.by_entity[i] = (wpe__IdPair){.id = i + 1, .entity = order[i]};
    s.ids.count = ordered;
    qsort(s.ids.by_entity, ordered, sizeof *s.ids.by_entity, wpe__compare_by_entity);

    wpe_json_write_object_begin(writer);
    wpe__write_string(writer, "format", "wpe.scene");
    wpe__write_integer(writer, "version", WPE_SCENE_FORMAT_VERSION);
    wpe_json_write_key(writer, "environment");
    wpe__save_environment(writer, environment);
    wpe__write_ref(&s, "camera", camera);
    wpe_json_write_key(writer, "entities");
    wpe_json_write_array_begin(writer, false);
    for (uint32_t i = 0; i < ordered && s.result == WPE_RESULT_OK; ++i) {
        /* Prefab instances are written as references; their members come from the prefab. */
        const WPE_Entity root = wpe__prefab_root(ecs, order[i]);
        if (root == order[i]) {
            wpe__save_instance(&s, order[i]);
        } else if (root == WPE_ENTITY_NULL) {
            wpe__save_entity(&s, order[i]);
        }
    }
    wpe_json_write_array_end(writer);
    wpe_json_write_object_end(writer);

done:
    wpe_free(alive);
    wpe_free(order);
    wpe_free(stack);
    wpe_free(s.ids.by_entity);
    return s.result;
}

WPE_Result wpe_scene_save_file(const WPE_SceneContext* context, const WPE_SceneEnvironment* environment,
                               WPE_Entity camera, const char* os_path) {
    if (!os_path) return WPE_RESULT_ERROR_NULL_ARG;
    WPE_JsonWriter* writer = NULL;
    WPE_Result result = wpe_json_writer_create(&writer);
    if (result == WPE_RESULT_OK) result = wpe_scene_save(context, environment, camera, writer);
    const char* text = NULL;
    size_t size = 0;
    if (result == WPE_RESULT_OK) result = wpe_json_writer_finish(writer, &text, &size);
    if (result == WPE_RESULT_OK) result = wpe__write_file_atomic(os_path, text, size);
    wpe_json_writer_destroy(writer);
    return result;
}

WPE_Result wpe__write_file_atomic(const char* os_path, const char* text, size_t size) {
    /* Write next to the target, then replace it, so a failed save never leaves a truncated file. */
    char temp[1024];
    if (snprintf(temp, sizeof temp, "%s.tmp", os_path) >= (int)sizeof temp)
        return WPE_RESULT_ERROR_INVALID_ARG;
    WPE_File* file = NULL;
    WPE_Result result = wpe_file_open(temp, true, &file);
    if (result != WPE_RESULT_OK) return result;
    result = wpe_file_write(file, text, size);
    const WPE_Result closed = wpe_file_close(file);
    if (result == WPE_RESULT_OK) result = closed;
    if (result == WPE_RESULT_OK) result = wpe_path_rename(temp, os_path);
    if (result != WPE_RESULT_OK) wpe_path_remove(temp);
    return result;
}

/* ---- Loading -------------------------------------------------------------------------------------- */

bool wpe__read_asset(wpe__Loader* l, const WPE_JsonValue* object, const char* key, WPE_AssetType type,
                     WPE_AssetHandle* out) {
    *out = WPE_ASSET_NONE;
    const WPE_JsonValue* v = wpe_json_get(object, key);
    if (!v || wpe_json_type(v) == WPE_JSON_NULL) return true;
    const char* path = wpe_json_string(v);
    if (!path || !wpe_vfs_path_is_valid(path))
        return wpe__json_fail(&l->r, v, WPE_RESULT_ERROR_PARSE, "\"%s\" must be an asset path", key);
    const WPE_Result result = wpe_asset_load(l->context->assets, type, path, out);
    if (result != WPE_RESULT_OK)
        return wpe__json_fail(&l->r, v, result, "cannot load \"%s\" '%s': %s", key, path,
                              wpe_result_to_string(result));
    return true;
}

/* Entity reference: a file id or null. */
static bool wpe__read_ref_value(wpe__Loader* l, const WPE_JsonValue* v, const char* key, WPE_Entity* out) {
    *out = WPE_ENTITY_NULL;
    if (!v || wpe_json_type(v) == WPE_JSON_NULL) return true;
    const double id = wpe_json_number(v, 0.0);
    const double max_id = l->ids.identity ? (double)UINT32_MAX : (double)WPE__MAX_FILE_ID;
    if (wpe_json_type(v) != WPE_JSON_NUMBER || id != floor(id) || id < 1.0 || id > max_id)
        return wpe__json_fail(&l->r, v, WPE_RESULT_ERROR_PARSE, "\"%s\" must be an entity id or null", key);
    *out = wpe_scene_ids_to_entity(&l->ids, (uint32_t)id);
    if (*out == WPE_ENTITY_NULL)
        return wpe__json_fail(&l->r, v, WPE_RESULT_ERROR_PARSE,
                              "\"%s\" references entity %u, which is not in the scene", key, (uint32_t)id);
    return true;
}

/* Array of entity references of exactly `count` entries; *out is a wpe_alloc() array owned by the caller. */
static bool wpe__read_refs(wpe__Loader* l, const WPE_JsonValue* object, const char* key, uint32_t count,
                           WPE_Entity** out) {
    *out = NULL;
    const WPE_JsonValue* v = wpe_json_get(object, key);
    if (wpe_json_type(v) != WPE_JSON_ARRAY || wpe_json_count(v) != count)
        return wpe__json_fail(&l->r, v ? v : object, WPE_RESULT_ERROR_PARSE,
                              "\"%s\" must list %u entity ids (or nulls)", key, count);
    WPE_Entity* entities = wpe_alloc_zeroed(count ? count : 1, sizeof *entities);
    if (!entities) return wpe__json_fail(&l->r, v, WPE_RESULT_ERROR_OUT_OF_MEMORY, "out of memory");
    for (uint32_t i = 0; i < count; ++i) {
        if (!wpe__read_ref_value(l, wpe_json_at(v, i), key, &entities[i])) {
            wpe_free(entities);
            return false;
        }
    }
    *out = entities;
    return true;
}

bool wpe__add(wpe__Loader* l, const WPE_JsonValue* at, WPE_Entity e, WPE_ComponentType type, const void* data,
              const char* what) {
    if (!wpe_ecs_is_registered(l->context->ecs, type))
        return wpe__json_fail(&l->r, at, WPE_RESULT_ERROR_UNSUPPORTED,
                              "%s components are not available (module not created)", what);
    const WPE_Result result = wpe_ecs_add_component(l->context->ecs, e, type, data, NULL);
    if (result != WPE_RESULT_OK)
        return wpe__json_fail(&l->r, at, result, "cannot add %s: %s", what, wpe_result_to_string(result));
    return true;
}

/* "sky" (absent: unchanged). The panorama image is kept as a path; the engine loads it. */
static bool wpe__load_sky(wpe__Loader* l, const WPE_JsonValue* v, WPE_SceneEnvironment* env) {
    if (!v) return true;
    WPE_SkySettings* sky = &env->render.sky;
    uint32_t mode = sky->mode;
    if (!WPE__JSON_CHECK_OBJECT(&l->r, v, "sky", "mode", "zenith", "horizon", "ground", "curve", "image",
                                "rotation", "sun_size", "sun_color", "sun_glow") ||
        !wpe__json_read_enum(&l->r, v, "mode", wpe__sky_modes, WPE__COUNT(wpe__sky_modes), &mode) ||
        !wpe__json_read_vec3(&l->r, v, "zenith", &sky->zenith) ||
        !wpe__json_read_vec3(&l->r, v, "horizon", &sky->horizon) ||
        !wpe__json_read_vec3(&l->r, v, "ground", &sky->ground) ||
        !wpe__json_read_float(&l->r, v, "curve", 0.01f, 100.0f, &sky->curve) ||
        !wpe__json_read_float(&l->r, v, "rotation", -360.0f, 360.0f, &sky->rotation) ||
        !wpe__json_read_float(&l->r, v, "sun_size", 0.0f, 45.0f, &sky->sun_size) ||
        !wpe__json_read_vec3(&l->r, v, "sun_color", &sky->sun_color) ||
        !wpe__json_read_float(&l->r, v, "sun_glow", 0.0f, 1.0f, &sky->sun_glow))
        return false;
    sky->mode = (WPE_SkyMode)mode;
    const WPE_JsonValue* image = wpe_json_get(v, "image");
    if (image && wpe_json_type(image) != WPE_JSON_NULL) {
        const char* path = wpe_json_string(image);
        if (!path || !wpe_vfs_path_is_valid(path))
            return wpe__json_fail(&l->r, image, WPE_RESULT_ERROR_PARSE,
                                  "the sky's \"image\" must be an asset path or null");
        snprintf(env->sky_image, sizeof env->sky_image, "%s", path);
    } else if (image) {
        env->sky_image[0] = '\0';
    }
    if (sky->mode == WPE_SKY_PANORAMA && !env->sky_image[0])
        return wpe__json_fail(&l->r, v, WPE_RESULT_ERROR_PARSE, "a panorama sky needs an \"image\"");
    return true;
}

static bool wpe__load_environment(wpe__Loader* l, const WPE_JsonValue* v, WPE_SceneEnvironment* env) {
    if (!WPE__JSON_CHECK_OBJECT(&l->r, v, "environment", "clear_color", "ambient", "fog", "sky", "shadows",
                                "dither_transparency", "frustum_culling", "subdivision", "default_sun",
                                "gravity", "music", "postfx"))
        return false;
    if (!wpe__load_sky(l, wpe_json_get(v, "sky"), env)) return false;
    WPE_SceneRenderSettings* r = &env->render;
    if (!wpe__json_read_vec3(&l->r, v, "clear_color", &env->clear_color) ||
        !wpe__json_read_vec3(&l->r, v, "ambient", &r->ambient) ||
        !wpe__json_read_vec3(&l->r, v, "gravity", &env->gravity) ||
        !wpe__json_read_bool(&l->r, v, "dither_transparency", &r->dither_transparency) ||
        !wpe__json_read_bool(&l->r, v, "frustum_culling", &r->frustum_culling))
        return false;
    const WPE_JsonValue* fog = wpe_json_get(v, "fog");
    if (fog) {
        uint32_t mode = r->fog.mode;
        if (!WPE__JSON_CHECK_OBJECT(&l->r, fog, "fog", "mode", "color", "start", "end", "density") ||
            !wpe__json_read_enum(&l->r, fog, "mode", wpe__fog_modes, WPE__COUNT(wpe__fog_modes), &mode) ||
            !wpe__json_read_vec3(&l->r, fog, "color", &r->fog.color) ||
            !wpe__json_read_float(&l->r, fog, "start", 0, 1e9, &r->fog.start) ||
            !wpe__json_read_float(&l->r, fog, "end", 0, 1e9, &r->fog.end) ||
            !wpe__json_read_float(&l->r, fog, "density", 0, 1e3, &r->fog.density))
            return false;
        r->fog.mode = (WPE_FogMode)mode;
        if (r->fog.mode == WPE_FOG_LINEAR && !(r->fog.end > r->fog.start))
            return wpe__json_fail(&l->r, fog, WPE_RESULT_ERROR_PARSE,
                                  "linear fog needs \"end\" greater than \"start\"");
    }
    const WPE_JsonValue* shadows = wpe_json_get(v, "shadows");
    if (shadows &&
        (!WPE__JSON_CHECK_OBJECT(&l->r, shadows, "shadows", "enabled", "map_size", "distance",
                                 "caster_extension", "bias") ||
         !wpe__json_read_bool(&l->r, shadows, "enabled", &r->shadows.enabled) ||
         !wpe__json_read_uint(&l->r, shadows, "map_size", 16, 4096, &r->shadows.map_size) ||
         !wpe__json_read_float(&l->r, shadows, "distance", 0.01, 1e6, &r->shadows.distance) ||
         !wpe__json_read_float(&l->r, shadows, "caster_extension", 0, 1e6, &r->shadows.caster_extension) ||
         !wpe__json_read_float(&l->r, shadows, "bias", 0, 1, &r->shadows.bias)))
        return false;
    const WPE_JsonValue* subdivision = wpe_json_get(v, "subdivision");
    if (subdivision &&
        (!WPE__JSON_CHECK_OBJECT(&l->r, subdivision, "subdivision", "levels", "edge_pixels") ||
         !wpe__json_read_uint(&l->r, subdivision, "levels", 0, WPE_MAX_SUBDIVISION_LEVELS,
                              &r->subdivision_levels) ||
         !wpe__json_read_float(&l->r, subdivision, "edge_pixels", 1, 4096, &r->subdivision_edge_pixels)))
        return false;
    const WPE_JsonValue* sun = wpe_json_get(v, "default_sun");
    if (sun && (!WPE__JSON_CHECK_OBJECT(&l->r, sun, "default_sun", "enabled", "direction", "color") ||
                !wpe__json_read_bool(&l->r, sun, "enabled", &r->default_sun) ||
                !wpe__json_read_vec3(&l->r, sun, "direction", &r->default_sun_direction) ||
                !wpe__json_read_vec3(&l->r, sun, "color", &r->default_sun_color)))
        return false;
    if (wpe_vec3_length(r->default_sun_direction) < 1e-6f)
        return wpe__json_fail(&l->r, sun ? sun : v, WPE_RESULT_ERROR_PARSE,
                              "the default sun direction must not be zero");
    const WPE_JsonValue* music = wpe_json_get(v, "music");
    if (music) {
        if (!WPE__JSON_CHECK_OBJECT(&l->r, music, "music", "path", "fade") ||
            !wpe__json_read_float(&l->r, music, "fade", 0, 60, &env->music_fade))
            return false;
        const char* path = wpe_json_string(wpe_json_get(music, "path"));
        if (!path || !wpe_vfs_path_is_valid(path))
            return wpe__json_fail(&l->r, music, WPE_RESULT_ERROR_PARSE,
                                  "music needs \"path\", an asset path");
        snprintf(env->music, sizeof env->music, "%s", path);
    }
    const WPE_JsonValue* postfx = wpe_json_get(v, "postfx");
    if (postfx) {
        if (wpe_json_type(postfx) != WPE_JSON_ARRAY || wpe_json_count(postfx) > WPE_SCENE_MAX_POSTFX)
            return wpe__json_fail(&l->r, postfx, WPE_RESULT_ERROR_PARSE,
                                  "\"postfx\" must be an array of at most %u effects", WPE_SCENE_MAX_POSTFX);
        env->postfx_count = 0;
        for (uint32_t i = 0; i < wpe_json_count(postfx); ++i) {
            const WPE_JsonValue* e = wpe_json_at(postfx, i);
            uint32_t kind = UINT32_MAX;
            if (wpe_json_type(e) != WPE_JSON_OBJECT || !wpe_json_get(e, "effect"))
                return wpe__json_fail(&l->r, e, WPE_RESULT_ERROR_PARSE,
                                      "each post effect needs an \"effect\" name");
            const char* kinds[WPE_POSTFX_KIND_COUNT];
            for (uint32_t k = 0; k < WPE_POSTFX_KIND_COUNT; ++k)
                kinds[k] = wpe_postfx_kind_name((WPE_PostFxKind)k);
            if (!wpe__json_read_enum(&l->r, e, "effect", kinds, WPE_POSTFX_KIND_COUNT, &kind)) return false;
            WPE_PostFxConfig c = wpe_postfx_config_default((WPE_PostFxKind)kind);
            /* The kind's parameters, described by graphics/postfx.h: names, ranges, whole numbers. */
            uint32_t count = 0;
            const WPE_PostFxParam* params = wpe_postfx_params(c.kind, &count);
            const char* allowed[2 + 16] = {"effect", "enabled"};
            for (uint32_t p = 0; p < count && p < 16; ++p)
                allowed[2 + p] = params[p].name;
            if (!wpe__json_check_object(&l->r, e, kinds[kind], allowed, 2 + count)) return false;
            for (uint32_t p = 0; p < count; ++p) {
                const WPE_PostFxParam* param = &params[p];
                bool ok = false;
                if (param->integer) {
                    uint32_t value = (uint32_t)wpe_postfx_param_get(&c, param);
                    ok = wpe__json_read_uint(&l->r, e, param->name, (uint32_t)param->min,
                                             (uint32_t)param->max, &value);
                    wpe_postfx_param_set(&c, param, value);
                } else {
                    float value = (float)wpe_postfx_param_get(&c, param);
                    ok = wpe__json_read_float(&l->r, e, param->name, param->min, param->max, &value);
                    wpe_postfx_param_set(&c, param, value);
                }
                if (!ok) return false;
            }
            if (!wpe__json_read_bool(&l->r, e, "enabled", &c.enabled)) return false;
            env->postfx[env->postfx_count++] = c;
        }
    }
    return true;
}

/* Every vertex weight of a skinned mesh must reference a joint the skin provides. */
static bool wpe__skin_covers_mesh(const WPE_Model* model, uint32_t mesh_index, uint32_t joint_count) {
    const WPE_Mesh* mesh = &model->meshes[mesh_index];
    for (uint32_t p = 0; p < mesh->primitive_count; ++p) {
        const WPE_MeshPrimitive* prim = &model->primitives[mesh->first_primitive + p];
        for (uint32_t v = 0; v < prim->vertex_count; ++v) {
            const WPE_MeshVertex* vertex = &model->vertices[prim->vertex_offset + v];
            for (int j = 0; j < 4; ++j) {
                if (vertex->weights[j] > 0.0f && vertex->joints[j] >= joint_count) return false;
            }
        }
    }
    return true;
}

static bool wpe__load_mesh(wpe__Loader* l, WPE_Entity e, const WPE_JsonValue* v) {
    if (!WPE__JSON_CHECK_OBJECT(&l->r, v, "mesh", "model", "mesh", "skin", "joints", "visible", "unlit",
                                "cast_shadows", "receive_shadows", "double_sided"))
        return false;
    WPE_MeshRenderer mesh = wpe_mesh_renderer_default(WPE_ASSET_NONE, 0);
    if (!wpe__json_read_bool(&l->r, v, "visible", &mesh.visible) ||
        !wpe__json_read_bool(&l->r, v, "unlit", &mesh.unlit) ||
        !wpe__json_read_bool(&l->r, v, "cast_shadows", &mesh.cast_shadows) ||
        !wpe__json_read_bool(&l->r, v, "receive_shadows", &mesh.receive_shadows) ||
        !wpe__json_read_bool(&l->r, v, "double_sided", &mesh.double_sided) ||
        !wpe__read_asset(l, v, "model", WPE_ASSET_MODEL, &mesh.model_asset))
        return false;
    const WPE_Model* model = wpe_asset_model(l->context->assets, mesh.model_asset);
    bool ok = true;
    if (model) {
        ok = wpe__json_read_uint(&l->r, v, "mesh", 0, model->mesh_count ? model->mesh_count - 1 : 0,
                                 &mesh.mesh);
        if (ok && model->mesh_count == 0)
            ok = wpe__json_fail(&l->r, v, WPE_RESULT_ERROR_PARSE, "the model has no meshes");
        const WPE_JsonValue* skin = wpe_json_get(v, "skin");
        if (ok && skin && wpe_json_type(skin) != WPE_JSON_NULL) {
            ok = model->skin_count > 0
                     ? wpe__json_read_uint(&l->r, v, "skin", 0, model->skin_count - 1, &mesh.skin)
                     : wpe__json_fail(&l->r, skin, WPE_RESULT_ERROR_PARSE, "the model has no skins");
            if (ok) {
                mesh.joint_count = model->skins[mesh.skin].joint_count;
                ok = wpe__read_refs(l, v, "joints", mesh.joint_count, &mesh.joints);
            }
            if (ok && !wpe__skin_covers_mesh(model, mesh.mesh, mesh.joint_count))
                ok = wpe__json_fail(&l->r, skin, WPE_RESULT_ERROR_PARSE, "skin %u does not cover mesh %u",
                                    mesh.skin, mesh.mesh);
        } else if (ok && wpe_json_get(v, "joints")) {
            ok = wpe__json_fail(&l->r, v, WPE_RESULT_ERROR_PARSE, "\"joints\" requires \"skin\"");
        }
    } else if (wpe_json_get(v, "skin") || wpe_json_get(v, "joints")) {
        ok = wpe__json_fail(&l->r, v, WPE_RESULT_ERROR_PARSE, "a skin requires a model");
    }
    if (ok) ok = wpe__add(l, v, e, WPE_COMP_MESH, &mesh, "mesh renderer");
    if (!ok) wpe_free(mesh.joints);                          /* Owned by the component only once added. */
    wpe_asset_release(l->context->assets, mesh.model_asset); /* The component holds its own reference. */
    return ok;
}

static bool wpe__load_camera(wpe__Loader* l, WPE_Entity e, const WPE_JsonValue* v) {
    WPE_Camera camera = wpe_camera_default();
    if (!WPE__JSON_CHECK_OBJECT(&l->r, v, "camera", "fov", "near", "far", "orthographic", "ortho_height") ||
        !wpe__json_read_degrees(&l->r, v, "fov", 0.1, 179, &camera.fov_y) ||
        !wpe__json_read_float(&l->r, v, "near", 1e-6, 1e9, &camera.near_z) ||
        !wpe__json_read_float(&l->r, v, "far", 1e-6, 1e9, &camera.far_z) ||
        !wpe__json_read_bool(&l->r, v, "orthographic", &camera.orthographic) ||
        !wpe__json_read_float(&l->r, v, "ortho_height", 1e-6, 1e9, &camera.ortho_height))
        return false;
    if (!(camera.far_z > camera.near_z))
        return wpe__json_fail(&l->r, v, WPE_RESULT_ERROR_PARSE,
                              "the camera's \"far\" must be greater than \"near\"");
    return wpe__add(l, v, e, WPE_COMP_CAMERA, &camera, "camera");
}

static bool wpe__load_light(wpe__Loader* l, WPE_Entity e, const WPE_JsonValue* v) {
    uint32_t type = WPE_LIGHT_POINT;
    if (!WPE__JSON_CHECK_OBJECT(&l->r, v, "light", "type", "color", "intensity", "range", "inner_cone",
                                "outer_cone", "cast_shadows") ||
        !wpe__json_read_enum(&l->r, v, "type", wpe__light_types, WPE__COUNT(wpe__light_types), &type))
        return false;
    WPE_Light light = wpe_light_default((WPE_LightType)type);
    if (!wpe__json_read_vec3(&l->r, v, "color", &light.color) ||
        !wpe__json_read_float(&l->r, v, "intensity", 0, 1e6, &light.intensity) ||
        !wpe__json_read_float(&l->r, v, "range", 1e-3, 1e9, &light.range) ||
        !wpe__json_read_degrees(&l->r, v, "inner_cone", 0, 89.9, &light.inner_cone) ||
        !wpe__json_read_degrees(&l->r, v, "outer_cone", 0, 89.9, &light.outer_cone) ||
        !wpe__json_read_bool(&l->r, v, "cast_shadows", &light.cast_shadows))
        return false;
    if (light.inner_cone > light.outer_cone)
        return wpe__json_fail(&l->r, v, WPE_RESULT_ERROR_PARSE,
                              "\"inner_cone\" must not exceed \"outer_cone\"");
    return wpe__add(l, v, e, WPE_COMP_LIGHT, &light, "light");
}

static bool wpe__load_animator(wpe__Loader* l, WPE_Entity e, const WPE_JsonValue* v) {
    if (!WPE__JSON_CHECK_OBJECT(&l->r, v, "animator", "model", "animation", "time", "speed", "playing",
                                "loop", "nodes"))
        return false;
    WPE_Animator animator = {.speed = 1.0f, .loop = true};
    if (!wpe__read_asset(l, v, "model", WPE_ASSET_MODEL, &animator.model_asset)) return false;
    const WPE_Model* model = wpe_asset_model(l->context->assets, animator.model_asset);
    bool ok = false;
    if (!model) {
        wpe__json_fail(&l->r, v, WPE_RESULT_ERROR_PARSE, "an animator needs a \"model\"");
    } else if (model->animation_count == 0) {
        wpe__json_fail(&l->r, v, WPE_RESULT_ERROR_PARSE, "the model has no animations");
    } else {
        animator.node_count = model->node_count;
        ok = wpe__json_read_uint(&l->r, v, "animation", 0, model->animation_count - 1, &animator.animation) &&
             wpe__json_read_float(&l->r, v, "time", -1e9, 1e9, &animator.time) &&
             wpe__json_read_float(&l->r, v, "speed", -1e3, 1e3, &animator.speed) &&
             wpe__json_read_bool(&l->r, v, "playing", &animator.playing) &&
             wpe__json_read_bool(&l->r, v, "loop", &animator.loop) &&
             wpe__read_refs(l, v, "nodes", animator.node_count, &animator.node_entities) &&
             wpe__add(l, v, e, WPE_COMP_ANIMATOR, &animator, "animator");
    }
    if (!ok) wpe_free(animator.node_entities);
    wpe_asset_release(l->context->assets, animator.model_asset);
    return ok;
}

static bool wpe__load_rigidbody(wpe__Loader* l, WPE_Entity e, const WPE_JsonValue* v) {
    uint32_t type = WPE_BODY_DYNAMIC;
    if (!WPE__JSON_CHECK_OBJECT(&l->r, v, "rigidbody", "type", "linear_damping", "angular_damping",
                                "gravity_scale", "lock_rotation", "continuous", "initial_velocity") ||
        !wpe__json_read_enum(&l->r, v, "type", wpe__body_types, WPE__COUNT(wpe__body_types), &type))
        return false;
    WPE_RigidBody body = wpe_rigidbody_default((WPE_BodyType)type);
    if (!wpe__json_read_float(&l->r, v, "linear_damping", 0, 1e6, &body.linear_damping) ||
        !wpe__json_read_float(&l->r, v, "angular_damping", 0, 1e6, &body.angular_damping) ||
        !wpe__json_read_float(&l->r, v, "gravity_scale", -1e6, 1e6, &body.gravity_scale) ||
        !wpe__json_read_bool(&l->r, v, "lock_rotation", &body.lock_rotation) ||
        !wpe__json_read_bool(&l->r, v, "continuous", &body.continuous) ||
        !wpe__json_read_vec3(&l->r, v, "initial_velocity", &body.initial_velocity))
        return false;
    return wpe__add(l, v, e, WPE_COMP_PHYSICS_RIGIDBODY, &body, "rigid body");
}

static bool wpe__load_collider(wpe__Loader* l, WPE_Entity e, const WPE_JsonValue* v) {
    uint32_t shape = WPE_COLLIDER_BOX;
    if (!WPE__JSON_CHECK_OBJECT(&l->r, v, "collider", "shape", "size", "offset", "model", "mesh", "density",
                                "friction", "restitution", "trigger") ||
        !wpe__json_read_enum(&l->r, v, "shape", wpe__collider_shapes, WPE__COUNT(wpe__collider_shapes),
                             &shape))
        return false;
    WPE_Collider collider = wpe_collider_default((WPE_ColliderShape)shape);
    if (!wpe__json_read_vec3(&l->r, v, "size", &collider.size) ||
        !wpe__json_read_vec3(&l->r, v, "offset", &collider.offset) ||
        !wpe__json_read_float(&l->r, v, "density", 0, 1e9, &collider.density) ||
        !wpe__json_read_float(&l->r, v, "friction", 0, 1e3, &collider.friction) ||
        !wpe__json_read_float(&l->r, v, "restitution", 0, 1e3, &collider.restitution) ||
        !wpe__json_read_bool(&l->r, v, "trigger", &collider.is_trigger))
        return false;
    const bool needs_model = shape == WPE_COLLIDER_CONVEX || shape == WPE_COLLIDER_MESH;
    if (!needs_model && (wpe_json_get(v, "model") || wpe_json_get(v, "mesh")))
        return wpe__json_fail(&l->r, v, WPE_RESULT_ERROR_PARSE,
                              "only convex and mesh colliders take a \"model\"");
    if (!needs_model) {
        const float minimum =
            fminf(collider.size.x,
                  shape == WPE_COLLIDER_BOX ? fminf(collider.size.y, collider.size.z) : collider.size.x);
        if (!(minimum > 0.0f))
            return wpe__json_fail(&l->r, v, WPE_RESULT_ERROR_PARSE, "collider sizes must be positive");
        if (shape == WPE_COLLIDER_CAPSULE && !(collider.size.y >= 2.0f * collider.size.x))
            return wpe__json_fail(&l->r, v, WPE_RESULT_ERROR_PARSE,
                                  "a capsule's height (size.y) must be at least twice its radius");
        return wpe__add(l, v, e, WPE_COMP_PHYSICS_COLLIDER, &collider, "collider");
    }
    if (!wpe__read_asset(l, v, "model", WPE_ASSET_MODEL, &collider.model)) return false;
    const WPE_Model* model = wpe_asset_model(l->context->assets, collider.model);
    bool ok = false;
    if (!model || model->mesh_count == 0) {
        wpe__json_fail(&l->r, v, WPE_RESULT_ERROR_PARSE,
                       "convex and mesh colliders need a \"model\" with meshes");
    } else {
        ok = wpe__json_read_uint(&l->r, v, "mesh", 0, model->mesh_count - 1, &collider.mesh) &&
             wpe__add(l, v, e, WPE_COMP_PHYSICS_COLLIDER, &collider, "collider");
    }
    wpe_asset_release(l->context->assets, collider.model);
    return ok;
}

static bool wpe__load_character(wpe__Loader* l, WPE_Entity e, const WPE_JsonValue* v) {
    if (!WPE__JSON_CHECK_OBJECT(&l->r, v, "character", "radius", "height", "offset", "max_slope",
                                "step_height", "snap_distance"))
        return false;
    WPE_CharacterController c = wpe_character_default();
    if (!wpe__json_read_float(&l->r, v, "radius", 0.001f, 1e4f, &c.radius) ||
        !wpe__json_read_float(&l->r, v, "height", 0.001f, 1e4f, &c.height) ||
        !wpe__json_read_vec3(&l->r, v, "offset", &c.offset) ||
        !wpe__json_read_float(&l->r, v, "max_slope", 0.0f, 90.0f, &c.max_slope) ||
        !wpe__json_read_float(&l->r, v, "step_height", 0.0f, 1e4f, &c.step_height) ||
        !wpe__json_read_float(&l->r, v, "snap_distance", 0.0f, 1e4f, &c.snap_distance))
        return false;
    return wpe__add(l, v, e, WPE_COMP_CHARACTER, &c, "character controller");
}

static bool wpe__load_audio_source(wpe__Loader* l, WPE_Entity e, const WPE_JsonValue* v) {
    if (!WPE__JSON_CHECK_OBJECT(&l->r, v, "audio_source", "sound", "volume", "pitch", "min_distance",
                                "max_distance", "bus", "loop", "spatial", "playing"))
        return false;
    if (!wpe_ecs_is_registered(l->context->ecs, WPE_COMP_AUDIO_SOURCE))
        return wpe__json_fail(&l->r, v, WPE_RESULT_ERROR_UNSUPPORTED,
                              "audio source components are not available (no audio)");
    WPE_AudioSource source = wpe_audio_source_default(WPE_ASSET_NONE);
    uint32_t bus = source.bus;
    if (!wpe__json_read_float(&l->r, v, "volume", 0, 1e3, &source.volume) ||
        !wpe__json_read_float(&l->r, v, "pitch", 1e-3, 1e3, &source.pitch) ||
        !wpe__json_read_float(&l->r, v, "min_distance", 1e-3, 1e9, &source.min_distance) ||
        !wpe__json_read_float(&l->r, v, "max_distance", 1e-3, 1e9, &source.max_distance) ||
        !wpe__json_read_enum(&l->r, v, "bus", wpe__buses, WPE__COUNT(wpe__buses), &bus) ||
        !wpe__json_read_bool(&l->r, v, "loop", &source.loop) ||
        !wpe__json_read_bool(&l->r, v, "spatial", &source.spatial) ||
        !wpe__json_read_bool(&l->r, v, "playing", &source.playing))
        return false;
    if (bus == WPE_AUDIO_BUS_MASTER)
        return wpe__json_fail(&l->r, v, WPE_RESULT_ERROR_PARSE,
                              "audio sources play on the \"effects\" or \"music\" bus");
    if (source.max_distance < source.min_distance)
        return wpe__json_fail(&l->r, v, WPE_RESULT_ERROR_PARSE,
                              "\"max_distance\" must not be below \"min_distance\"");
    source.bus = (WPE_AudioBus)bus;
    if (!wpe_json_get(v, "sound"))
        return wpe__json_fail(&l->r, v, WPE_RESULT_ERROR_PARSE, "an audio source needs a \"sound\"");
    if (!wpe__read_asset(l, v, "sound", WPE_ASSET_SOUND, &source.sound)) return false;
    const bool ok = wpe__add(l, v, e, WPE_COMP_AUDIO_SOURCE, &source, "audio source");
    wpe_asset_release(l->context->assets, source.sound);
    return ok;
}

static bool wpe__load_script(wpe__Loader* l, WPE_Entity e, const WPE_JsonValue* v) {
    if (!WPE__JSON_CHECK_OBJECT(&l->r, v, "script", "path", "properties")) return false;
    WPE_ScriptHost* host = l->context->scripts;
    if (!host)
        return wpe__json_fail(&l->r, v, WPE_RESULT_ERROR_UNSUPPORTED,
                              "scripts are not available (no script host)");
    const char* path = wpe_json_string(wpe_json_get(v, "path"));
    if (!path) return wpe__json_fail(&l->r, v, WPE_RESULT_ERROR_PARSE, "a script needs a \"path\"");
    WPE_Result result = wpe_script_attach(host, e, path);
    if (result != WPE_RESULT_OK)
        return wpe__json_fail(&l->r, v, result, "cannot attach script '%s': %s", path,
                              wpe_result_to_string(result));
    const WPE_JsonValue* properties = wpe_json_get(v, "properties");
    if (properties && wpe_json_type(properties) != WPE_JSON_OBJECT)
        return wpe__json_fail(&l->r, properties, WPE_RESULT_ERROR_PARSE, "\"properties\" must be an object");
    for (uint32_t i = 0; i < wpe_json_count(properties); ++i) {
        const char* key = NULL;
        const WPE_JsonValue* p = wpe_json_member(properties, i, &key);
        WPE_ScriptValue value = {0};
        switch (wpe_json_type(p)) {
        case WPE_JSON_NUMBER:
            value = (WPE_ScriptValue){.type = WPE_SCRIPT_VALUE_NUMBER, .number = wpe_json_number(p, 0.0)};
            break;
        case WPE_JSON_BOOL:
            value = (WPE_ScriptValue){.type = WPE_SCRIPT_VALUE_BOOL, .boolean = wpe_json_bool(p, false)};
            break;
        case WPE_JSON_STRING:
            value = (WPE_ScriptValue){.type = WPE_SCRIPT_VALUE_STRING, .string = wpe_json_string(p)};
            break;
        case WPE_JSON_ARRAY:
            value.type = WPE_SCRIPT_VALUE_VECTOR;
            if (!wpe__json_read_floats(&l->r, p, key, 3, value.vector.v)) return false;
            break;
        default:
            return wpe__json_fail(&l->r, p, WPE_RESULT_ERROR_PARSE,
                                  "script property \"%s\" must be a number, boolean, string or [x, y, z]",
                                  key);
        }
        result = wpe_script_set_property(host, e, key, &value);
        if (result != WPE_RESULT_OK)
            return wpe__json_fail(&l->r, p, result, "cannot set script property \"%s\": %s", key,
                                  wpe_result_to_string(result));
    }
    return true;
}

static bool wpe__load_components(wpe__Loader* l, WPE_Entity e, const WPE_JsonValue* v) {
    if (wpe_json_type(v) != WPE_JSON_OBJECT)
        return wpe__json_fail(&l->r, v, WPE_RESULT_ERROR_PARSE, "\"components\" must be an object");
    for (uint32_t i = 0; i < wpe_json_count(v); ++i) {
        const char* key = NULL;
        const WPE_JsonValue* value = wpe_json_member(v, i, &key);
        const WPE_ComponentSerializer* serializer = NULL;
        for (uint32_t s = 0; s < l->context->serializer_count && !serializer; ++s) {
            if (strcmp(l->context->serializers[s].name, key) == 0) serializer = &l->context->serializers[s];
        }
        if (!serializer)
            return wpe__json_fail(&l->r, value, WPE_RESULT_ERROR_PARSE, "unknown component \"%s\"", key);
        const uint32_t size = wpe_ecs_component_size(l->context->ecs, serializer->type);
        if (size == 0)
            return wpe__json_fail(&l->r, value, WPE_RESULT_ERROR_UNSUPPORTED,
                                  "component \"%s\" is not registered", key);
        void* data = wpe_alloc_zeroed(1, size);
        if (!data) return wpe__json_fail(&l->r, value, WPE_RESULT_ERROR_OUT_OF_MEMORY, "out of memory");
        const WPE_Result result =
            serializer->load(l->context, &l->ids, e, value, data, serializer->user_data);
        const bool ok = result == WPE_RESULT_OK
                            ? wpe__add(l, value, e, serializer->type, data, key)
                            : wpe__json_fail(&l->r, value, result, "loading component \"%s\" failed: %s", key,
                                             wpe_result_to_string(result));
        wpe_free(data);
        if (!ok) return false;
    }
    return true;
}

/* Authoring shorthand: instantiates a whole model (its node hierarchy, meshes, skins and an optional
 * animator) beneath the entity. Saving writes the created entities out explicitly. */
static bool wpe__load_instance(wpe__Loader* l, WPE_Entity e, const WPE_JsonValue* v) {
    if (!WPE__JSON_CHECK_OBJECT(&l->r, v, "instance", "model", "animation", "play")) return false;
    if (!wpe_json_get(v, "model"))
        return wpe__json_fail(&l->r, v, WPE_RESULT_ERROR_PARSE, "an instance needs a \"model\"");
    WPE_AssetHandle model = WPE_ASSET_NONE;
    if (!wpe__read_asset(l, v, "model", WPE_ASSET_MODEL, &model)) return false;
    const WPE_Model* data = wpe_asset_model(l->context->assets, model);
    WPE_InstantiateOptions options = {.animation = WPE_MODEL_NONE};
    const WPE_JsonValue* animation = wpe_json_get(v, "animation");
    bool ok = true;
    if (animation && wpe_json_type(animation) != WPE_JSON_NULL) {
        if (!data || data->animation_count == 0) {
            ok = wpe__json_fail(&l->r, animation, WPE_RESULT_ERROR_PARSE, "the model has no animations");
        } else {
            ok = wpe__json_read_uint(&l->r, v, "animation", 0, data->animation_count - 1, &options.animation);
        }
    }
    ok = ok && wpe__json_read_bool(&l->r, v, "play", &options.play);
    WPE_Entity root = WPE_ENTITY_NULL;
    if (ok) {
        const WPE_Result result =
            wpe_model_instantiate(l->context->ecs, l->context->assets, model, &options, &root);
        if (result != WPE_RESULT_OK)
            ok = wpe__json_fail(&l->r, v, result, "cannot instantiate the model: %s",
                                wpe_result_to_string(result));
    }
    if (ok) {
        const WPE_Result result = wpe_scene_set_parent(l->context->ecs, root, e, false);
        if (result != WPE_RESULT_OK) {
            wpe_scene_destroy_recursive(l->context->ecs, root);
            ok = wpe__json_fail(&l->r, v, result, "cannot attach the instance: %s",
                                wpe_result_to_string(result));
        }
    }
    wpe_asset_release(l->context->assets, model); /* The created components hold their own references. */
    return ok;
}

static bool wpe__load_transform(wpe__Loader* l, WPE_Entity e, const WPE_JsonValue* v) {
    if (!WPE__JSON_CHECK_OBJECT(&l->r, v, "transform", "position", "rotation", "euler", "scale"))
        return false;
    WPE_Transform t = wpe_transform_identity();
    if (!wpe__json_read_vec3(&l->r, v, "position", &t.position) ||
        !wpe__json_read_vec3(&l->r, v, "scale", &t.scale))
        return false;
    const WPE_JsonValue* rotation = wpe_json_get(v, "rotation");
    const WPE_JsonValue* euler = wpe_json_get(v, "euler");
    if (rotation && euler)
        return wpe__json_fail(&l->r, v, WPE_RESULT_ERROR_PARSE, "give \"rotation\" or \"euler\", not both");
    if (rotation) {
        if (!wpe__json_read_floats(&l->r, rotation, "rotation", 4, t.rotation.v)) return false;
        const WPE_Quat q = t.rotation;
        const float length = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        if (!(length > 1e-6f))
            return wpe__json_fail(&l->r, rotation, WPE_RESULT_ERROR_PARSE, "\"rotation\" must not be zero");
        /* Kept exactly as written (transforms normalize when building matrices), so saving reproduces it. */
    } else if (euler) {
        WPE_Vec3 degrees;
        if (!wpe__json_read_floats(&l->r, euler, "euler", 3, degrees.v)) return false;
        t.rotation = wpe_quat_from_euler_xyz(wpe_vec3_scale(degrees, (float)(1.0 / WPE__DEG_PER_RAD)));
    }
    return wpe__add(l, v, e, WPE_COMP_TRANSFORM, &t, "transform");
}

uint32_t wpe__entity_id_capacity(const WPE_JsonValue* entities) {
    uint32_t capacity = wpe_json_count(entities);
    for (uint32_t i = 0; i < wpe_json_count(entities); ++i)
        capacity += wpe_json_count(wpe_json_get(wpe_json_get(wpe_json_at(entities, i), "prefab"), "ids"));
    return capacity;
}

/* A file id: a whole number from 1 to WPE__MAX_FILE_ID. */
static bool wpe__read_file_id(wpe__Loader* l, const WPE_JsonValue* v, const WPE_JsonValue* at,
                              uint32_t* out) {
    const double id = wpe_json_number(v, 0.0);
    if (wpe_json_type(v) != WPE_JSON_NUMBER || id != floor(id) || id < 1.0 || id > WPE__MAX_FILE_ID)
        return wpe__json_fail(&l->r, v ? v : at, WPE_RESULT_ERROR_PARSE,
                              "entity ids are whole numbers from 1 to %u", WPE__MAX_FILE_ID);
    *out = (uint32_t)id;
    return true;
}

/* A prefab instance entry: instantiates the prefab (with its overrides) and registers the scene ids of
 * its members ("ids": prefab-local id -> scene id). */
static bool wpe__load_instance_entry(wpe__Loader* l, const WPE_JsonValue* v, const WPE_JsonValue* prefab,
                                     WPE_Entity* out_root, uint32_t* io_id_count) {
    if (l->in_prefab)
        return wpe__json_fail(&l->r, prefab, WPE_RESULT_ERROR_UNSUPPORTED,
                              "prefab files cannot contain prefab instances");
    static const char* const own[] = {"id", "name", "parent", "transform", "prefab"};
    for (uint32_t i = 0; !l->ids.identity && i < wpe_json_count(v); ++i) {
        const char* key = NULL;
        const WPE_JsonValue* value = wpe_json_member(v, i, &key);
        bool allowed = false;
        for (size_t k = 0; k < sizeof own / sizeof own[0]; ++k)
            allowed = allowed || strcmp(key, own[k]) == 0;
        if (!allowed)
            return wpe__json_fail(&l->r, value, WPE_RESULT_ERROR_PARSE,
                                  "\"%s\": an instance's components come from its prefab; change them under "
                                  "\"prefab\": {\"overrides\": {\"<prefab entity id>\": {...}}}",
                                  key);
    }
    if (!WPE__JSON_CHECK_OBJECT(&l->r, prefab, "prefab", "path", "ids", "overrides")) return false;
    const char* path = wpe_json_string(wpe_json_get(prefab, "path"));
    if (!path || !wpe_vfs_path_is_valid(path))
        return wpe__json_fail(&l->r, prefab, WPE_RESULT_ERROR_PARSE,
                              "\"path\" must be the prefab's asset path");
    wpe__IdPair* members = NULL;
    uint32_t member_count = 0;
    if (!wpe__prefab_load(l, prefab, path, wpe_json_get(prefab, "overrides"), out_root, &members,
                          &member_count))
        return false;
    const WPE_JsonValue* ids = wpe_json_get(prefab, "ids");
    bool ok = !ids || wpe_json_type(ids) == WPE_JSON_OBJECT ||
              wpe__json_fail(&l->r, ids, WPE_RESULT_ERROR_PARSE, "\"ids\" must map prefab ids to scene ids");
    for (uint32_t i = 0; ok && !l->ids.identity && i < wpe_json_count(ids); ++i) {
        const char* key = NULL;
        const WPE_JsonValue* scene_id = wpe_json_member(ids, i, &key);
        char* end = NULL;
        const unsigned long local = strtoul(key, &end, 10);
        WPE_Entity member = WPE_ENTITY_NULL;
        for (uint32_t m = 0; m < member_count && *end == '\0'; ++m) {
            if (members[m].id == local) member = members[m].entity;
        }
        uint32_t id = 0;
        ok = wpe__read_file_id(l, scene_id, ids, &id);
        /* Members removed by an override have no entity: their ids are not registered. */
        if (ok && *end != '\0')
            ok = wpe__json_fail(&l->r, scene_id, WPE_RESULT_ERROR_PARSE, "\"%s\" is not a prefab entity id",
                                key);
        if (ok && member != WPE_ENTITY_NULL)
            l->ids.by_id[(*io_id_count)++] = (wpe__IdPair){.id = id, .entity = member};
    }
    wpe_free(members);
    if (!ok) wpe_scene_destroy_recursive(l->context->ecs, *out_root);
    return ok;
}

/* Every load stage; returns false on the first error (recorded in l->r.result). */
bool wpe__load_entities(wpe__Loader* l, const WPE_JsonValue* entities, WPE_Entity* created,
                        uint32_t* io_count) {
    WPE_Ecs* ecs = l->context->ecs;
    const uint32_t count = wpe_json_count(entities);
    /* 1. Create every entity first so references can resolve in any order. Prefab instances are
     * created whole, so their members' scene ids resolve too. */
    uint32_t id_count = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const WPE_JsonValue* v = wpe_json_at(entities, i);
        if (!WPE__JSON_CHECK_OBJECT(&l->r, v, "entity", "id", "name", "parent", "transform", "instance",
                                    "mesh", "camera", "light", "billboard", "particles", "animator",
                                    "rigidbody", "collider", "character", "audio_source", "ui", "sprite",
                                    "text", "button", "toggle", "slider", "script", "components", "prefab"))
            return false;
        uint32_t id = 0;
        if (!wpe__read_file_id(l, wpe_json_get(v, "id"), v, &id))
            return wpe__json_fail(&l->r, v, WPE_RESULT_ERROR_PARSE,
                                  "every entity needs an \"id\" (a whole number from 1 to %u)",
                                  WPE__MAX_FILE_ID);
        const WPE_JsonValue* prefab = wpe_json_get(v, "prefab");
        WPE_Entity e = WPE_ENTITY_NULL;
        if (prefab) {
            if (!wpe__load_instance_entry(l, v, prefab, &e, &id_count)) return false;
        } else {
            e = wpe_ecs_spawn(ecs);
            if (e == WPE_ENTITY_NULL)
                return wpe__json_fail(&l->r, v, WPE_RESULT_ERROR_CAPACITY, "entity limit reached");
        }
        created[(*io_count)++] = e;
        l->ids.by_id[id_count++] = (wpe__IdPair){.id = id, .entity = e};
    }
    l->ids.count = id_count;
    qsort(l->ids.by_id, id_count, sizeof *l->ids.by_id, wpe__compare_by_id);
    for (uint32_t i = 1; i < id_count; ++i) {
        if (l->ids.by_id[i].id == l->ids.by_id[i - 1].id)
            return wpe__json_fail(&l->r, entities, WPE_RESULT_ERROR_PARSE, "entity id %u is used twice",
                                  l->ids.by_id[i].id);
    }
    /* 2. Names and transforms; 3. parents in file order (so children keep their order). */
    for (uint32_t i = 0; i < count; ++i) {
        const WPE_JsonValue* v = wpe_json_at(entities, i);
        const WPE_Entity e = created[i];
        const WPE_JsonValue* name = wpe_json_get(v, "name");
        if (name && !wpe_json_string(name))
            return wpe__json_fail(&l->r, name, WPE_RESULT_ERROR_PARSE, "\"name\" must be a string");
        if (name && wpe_scene_set_name(ecs, e, wpe_json_string(name)) != WPE_RESULT_OK)
            return wpe__json_fail(&l->r, name, WPE_RESULT_ERROR_OUT_OF_MEMORY, "cannot set the name");
        const WPE_JsonValue* transform = wpe_json_get(v, "transform");
        /* An instance's own transform replaces its prefab root's. */
        if (transform && wpe_ecs_has_component(ecs, e, WPE_COMP_TRANSFORM))
            wpe_ecs_remove_component(ecs, e, WPE_COMP_TRANSFORM);
        if (transform && !wpe__load_transform(l, e, transform)) return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const WPE_JsonValue* parent_value = wpe_json_get(wpe_json_at(entities, i), "parent");
        WPE_Entity parent = WPE_ENTITY_NULL;
        if (!wpe__read_ref_value(l, parent_value, "parent", &parent)) return false;
        if (parent == WPE_ENTITY_NULL) continue;
        const WPE_Result result = wpe_scene_set_parent(ecs, created[i], parent, false);
        if (result != WPE_RESULT_OK)
            return wpe__json_fail(&l->r, parent_value, WPE_RESULT_ERROR_PARSE, "invalid parent (%s)",
                                  result == WPE_RESULT_ERROR_INVALID_ARG
                                      ? "the hierarchy would contain a cycle"
                                      : wpe_result_to_string(result));
    }
    /* 4. Components, in this order. Scripts come last, after everything they might look at. */
    static const struct {
        const char* key;
        bool (*load)(wpe__Loader* l, WPE_Entity e, const WPE_JsonValue* v);
    } loaders[] = {
        {"instance", wpe__load_instance},
        {"mesh", wpe__load_mesh},
        {"camera", wpe__load_camera},
        {"light", wpe__load_light},
        {"billboard", wpe__load_billboard},
        {"particles", wpe__load_particles},
        {"animator", wpe__load_animator},
        {"rigidbody", wpe__load_rigidbody},
        {"collider", wpe__load_collider},
        {"character", wpe__load_character},
        {"audio_source", wpe__load_audio_source},
        {"ui", wpe__load_ui},
        {"sprite", wpe__load_sprite},
        {"text", wpe__load_text},
        {"button", wpe__load_button},
        {"toggle", wpe__load_toggle},
        {"slider", wpe__load_slider},
        {"components", wpe__load_components},
    };
    for (uint32_t i = 0; i < count; ++i) {
        const WPE_JsonValue* v = wpe_json_at(entities, i);
        for (size_t k = 0; k < sizeof loaders / sizeof loaders[0]; ++k) {
            const WPE_JsonValue* component = wpe_json_get(v, loaders[k].key);
            if (component && !loaders[k].load(l, created[i], component)) return false;
        }
    }
    for (uint32_t i = 0; i < count; ++i) {
        const WPE_JsonValue* c = wpe_json_get(wpe_json_at(entities, i), "script");
        if (c && !wpe__load_script(l, created[i], c)) return false;
    }
    return true;
}

WPE_Result wpe_scene_load(const WPE_SceneContext* context, const char* text, size_t size, const char* name,
                          WPE_SceneEnvironment* out_environment, WPE_Entity* out_camera) {
    if (!context || !context->ecs || !context->assets || !text) return WPE_RESULT_ERROR_NULL_ARG;
    if (out_camera) *out_camera = WPE_ENTITY_NULL;
    wpe__Loader l = {.r = {.name = name ? name : "scene", .kind = "scene"}, .context = context};
    WPE_JsonDocument* doc = NULL;
    WPE_JsonError error;
    WPE_Result result = wpe_json_parse(text, size, &doc, &error);
    if (result == WPE_RESULT_ERROR_PARSE)
        wpe_log_error("scene '%s', line %u, column %u: %s", l.r.name, error.line, error.column,
                      error.message);
    if (result != WPE_RESULT_OK) return result;

    const WPE_JsonValue* root = wpe_json_root(doc);
    WPE_SceneEnvironment environment = wpe_scene_environment_default();
    const WPE_JsonValue* entities = wpe_json_get(root, "entities");
    const uint32_t count = wpe_json_count(entities);
    WPE_Entity* created = wpe_alloc_zeroed(count ? count : 1, sizeof *created);
    const uint32_t id_capacity = wpe__entity_id_capacity(entities);
    l.ids.by_id = wpe_alloc_zeroed(id_capacity ? id_capacity : 1, sizeof *l.ids.by_id);
    uint32_t created_count = 0;
    WPE_Entity camera = WPE_ENTITY_NULL;
    if (!created || !l.ids.by_id) {
        wpe_free(created);
        wpe_free(l.ids.by_id);
        wpe_json_free(doc);
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }
    bool ok =
        WPE__JSON_CHECK_OBJECT(&l.r, root, "scene", "format", "version", "environment", "camera", "entities");
    if (ok && (!wpe_json_string(wpe_json_get(root, "format")) ||
               strcmp(wpe_json_string(wpe_json_get(root, "format")), "wpe.scene") != 0))
        ok = wpe__json_fail(&l.r, root, WPE_RESULT_ERROR_PARSE,
                            "not a scene file (\"format\" must be \"wpe.scene\")");
    uint32_t version = 0;
    ok = ok && wpe__json_read_uint(&l.r, root, "version", 1, UINT32_MAX, &version);
    if (ok && version > WPE_SCENE_FORMAT_VERSION)
        ok = wpe__json_fail(&l.r, root, WPE_RESULT_ERROR_UNSUPPORTED,
                            "format version %u is newer than this engine's (%u)", version,
                            WPE_SCENE_FORMAT_VERSION);
    if (ok && version == 0)
        ok = wpe__json_fail(&l.r, root, WPE_RESULT_ERROR_PARSE, "\"version\" is required");
    if (ok && wpe_json_get(root, "environment"))
        ok = wpe__load_environment(&l, wpe_json_get(root, "environment"), &environment);
    if (ok && entities && wpe_json_type(entities) != WPE_JSON_ARRAY)
        ok = wpe__json_fail(&l.r, entities, WPE_RESULT_ERROR_PARSE, "\"entities\" must be an array");
    ok = ok && wpe__load_entities(&l, entities, created, &created_count);
    ok = ok && wpe__read_ref_value(&l, wpe_json_get(root, "camera"), "camera", &camera);
    if (ok && camera != WPE_ENTITY_NULL && !wpe_ecs_has_component(context->ecs, camera, WPE_COMP_CAMERA))
        ok = wpe__json_fail(&l.r, wpe_json_get(root, "camera"), WPE_RESULT_ERROR_PARSE,
                            "\"camera\" has no camera component");

    if (!ok) {
        /* Roll back, including entities created beneath file entities (model instances). */
        for (uint32_t i = created_count; i-- > 0;) {
            if (wpe_ecs_is_alive(context->ecs, created[i]))
                wpe_scene_destroy_recursive(context->ecs, created[i]);
        }
    } else {
        if (out_environment) *out_environment = environment;
        if (out_camera) *out_camera = camera;
    }
    wpe_free(created);
    wpe_free(l.ids.by_id);
    wpe_json_free(doc);
    return ok ? WPE_RESULT_OK : l.r.result;
}

WPE_Result wpe_scene_load_asset(const WPE_SceneContext* context, const char* path,
                                WPE_SceneEnvironment* out_environment, WPE_Entity* out_camera) {
    if (!context || !context->assets || !path) return WPE_RESULT_ERROR_NULL_ARG;
    void* data = NULL;
    size_t size = 0;
    WPE_Result result = wpe_vfs_read_all(wpe_asset_manager_vfs(context->assets), path, &data, &size);
    if (result == WPE_RESULT_ERROR_NOT_FOUND) wpe_log_error("scene '%s' not found", path);
    if (result == WPE_RESULT_OK)
        result = wpe_scene_load(context, data, size, path, out_environment, out_camera);
    wpe_free(data);
    return result;
}

WPE_Result wpe_scene_clear(WPE_Ecs* ecs) {
    if (!ecs) return WPE_RESULT_ERROR_NULL_ARG;
    const uint32_t count = wpe_ecs_entity_count(ecs);
    if (count == 0) return WPE_RESULT_OK;
    WPE_Entity* entities = wpe_alloc(count * sizeof *entities);
    if (!entities) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    uint32_t n = 0;
    WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){0});
    while (wpe_ecs_iter_next(&it) && n < count)
        entities[n++] = it.entity;
    /* on_destroy handlers may despawn others first (deferred destroys run later and skip dead ones). */
    for (uint32_t i = 0; i < n; ++i) {
        if (wpe_ecs_is_alive(ecs, entities[i])) wpe_ecs_despawn(ecs, entities[i]);
    }
    wpe_free(entities);
    return WPE_RESULT_OK;
}

/* ---- Editing: single entities and components ------------------------------------------------------- */

/* Loader whose ids are the world's live entity handles. */
static wpe__Loader wpe__edit_loader(const WPE_SceneContext* context, const char* what) {
    return (wpe__Loader){
        .r = {.name = what, .kind = "edit"}, .context = context, .ids = {.identity = context->ecs}};
}

WPE_Result wpe_scene_save_entity(const WPE_SceneContext* context, WPE_Entity entity, WPE_JsonWriter* writer) {
    if (!context || !context->ecs || !context->assets || !writer) return WPE_RESULT_ERROR_NULL_ARG;
    if (!wpe_ecs_is_alive(context->ecs, entity)) return WPE_RESULT_ERROR_NOT_FOUND;
    wpe__Saver s = {
        .context = context, .w = writer, .ids = {.identity = context->ecs}, .describe_prefabs = true};
    wpe__save_entity(&s, entity);
    return s.result;
}

typedef bool (*wpe__ComponentLoadFn)(wpe__Loader* l, WPE_Entity e, const WPE_JsonValue* v);

static const struct {
    const char* key;
    WPE_ComponentType type;
    wpe__ComponentLoadFn load;
} wpe__edit_components[] = {
    {"transform", WPE_COMP_TRANSFORM, wpe__load_transform},
    {"mesh", WPE_COMP_MESH, wpe__load_mesh},
    {"camera", WPE_COMP_CAMERA, wpe__load_camera},
    {"light", WPE_COMP_LIGHT, wpe__load_light},
    {"billboard", WPE_COMP_BILLBOARD, wpe__load_billboard},
    {"particles", WPE_COMP_PARTICLES, wpe__load_particles},
    {"animator", WPE_COMP_ANIMATOR, wpe__load_animator},
    {"rigidbody", WPE_COMP_PHYSICS_RIGIDBODY, wpe__load_rigidbody},
    {"collider", WPE_COMP_PHYSICS_COLLIDER, wpe__load_collider},
    {"character", WPE_COMP_CHARACTER, wpe__load_character},
    {"audio_source", WPE_COMP_AUDIO_SOURCE, wpe__load_audio_source},
    {"ui", WPE_COMP_UI, wpe__load_ui},
    {"sprite", WPE_COMP_SPRITE, wpe__load_sprite},
    {"text", WPE_COMP_TEXT, wpe__load_text},
    {"button", WPE_COMP_BUTTON, wpe__load_button},
    {"toggle", WPE_COMP_TOGGLE, wpe__load_toggle},
    {"slider", WPE_COMP_SLIDER, wpe__load_slider},
    {"script", WPE_COMP_SCRIPT, wpe__load_script},
};

WPE_ComponentType wpe__edit_type(const WPE_SceneContext* context, const char* key) {
    for (size_t i = 0; i < sizeof wpe__edit_components / sizeof wpe__edit_components[0]; ++i) {
        if (strcmp(wpe__edit_components[i].key, key) == 0) return wpe__edit_components[i].type;
    }
    for (uint32_t i = 0; i < context->serializer_count; ++i) {
        if (strcmp(context->serializers[i].name, key) == 0) return context->serializers[i].type;
    }
    return WPE_COMP_NONE;
}

/* Loads one component description (no removal of an existing one). */
bool wpe__apply(wpe__Loader* l, WPE_Entity e, const char* key, const WPE_JsonValue* value) {
    if (strcmp(key, "name") == 0) {
        const char* name = wpe_json_string(value);
        if (!name) return wpe__json_fail(&l->r, value, WPE_RESULT_ERROR_PARSE, "\"name\" must be a string");
        const WPE_Result result = wpe_scene_set_name(l->context->ecs, e, name);
        return result == WPE_RESULT_OK || wpe__json_fail(&l->r, value, result, "cannot set the name");
    }
    if (strcmp(key, "parent") == 0) {
        WPE_Entity parent = WPE_ENTITY_NULL;
        if (!wpe__read_ref_value(l, value, "parent", &parent)) return false;
        const WPE_Result result = wpe_scene_set_parent(l->context->ecs, e, parent, false);
        return result == WPE_RESULT_OK || wpe__json_fail(&l->r, value, WPE_RESULT_ERROR_PARSE,
                                                         "invalid parent (%s)", wpe_result_to_string(result));
    }
    if (strcmp(key, "instance") == 0) return wpe__load_instance(l, e, value);
    for (size_t i = 0; i < sizeof wpe__edit_components / sizeof wpe__edit_components[0]; ++i) {
        if (strcmp(wpe__edit_components[i].key, key) == 0) return wpe__edit_components[i].load(l, e, value);
    }
    /* Game components by serializer name, through the same path as a scene's "components" object. */
    for (uint32_t i = 0; i < l->context->serializer_count; ++i) {
        if (strcmp(l->context->serializers[i].name, key) != 0) continue;
        const WPE_ComponentSerializer* serializer = &l->context->serializers[i];
        const uint32_t size = wpe_ecs_component_size(l->context->ecs, serializer->type);
        if (size == 0)
            return wpe__json_fail(&l->r, value, WPE_RESULT_ERROR_UNSUPPORTED, "\"%s\" is not registered",
                                  key);
        void* data = wpe_alloc_zeroed(1, size);
        if (!data) return wpe__json_fail(&l->r, value, WPE_RESULT_ERROR_OUT_OF_MEMORY, "out of memory");
        const WPE_Result result =
            serializer->load(l->context, &l->ids, e, value, data, serializer->user_data);
        const bool ok = result == WPE_RESULT_OK
                            ? wpe__add(l, value, e, serializer->type, data, key)
                            : wpe__json_fail(&l->r, value, result, "loading \"%s\" failed", key);
        wpe_free(data);
        return ok;
    }
    return wpe__json_fail(&l->r, value, WPE_RESULT_ERROR_PARSE, "unknown component \"%s\"", key);
}

WPE_Result wpe_scene_set_component(const WPE_SceneContext* context, WPE_Entity entity, const char* key,
                                   const WPE_JsonValue* value) {
    if (!context || !context->ecs || !context->assets || !key || !value) return WPE_RESULT_ERROR_NULL_ARG;
    if (!wpe_ecs_is_alive(context->ecs, entity)) return WPE_RESULT_ERROR_NOT_FOUND;
    wpe__Loader l = wpe__edit_loader(context, key);
    const WPE_ComponentType type = wpe__edit_type(context, key);
    if (type == WPE_COMP_NONE || type == WPE_COMP_SCRIPT ||
        !wpe_ecs_has_component(context->ecs, entity, type)) {
        /* Nothing to replace (scripts replace themselves when attached). */
        wpe__apply(&l, entity, key, value);
        return l.r.result;
    }
    /* Replace atomically: remember the current component, remove it, load the new one, and restore
     * the old one if the new description is rejected. */
    WPE_JsonWriter* writer = NULL;
    WPE_Result result = wpe_json_writer_create(&writer);
    if (result != WPE_RESULT_OK) return result;
    result = wpe_scene_save_entity(context, entity, writer);
    const char* text = NULL;
    size_t size = 0;
    if (result == WPE_RESULT_OK) result = wpe_json_writer_finish(writer, &text, &size);
    WPE_JsonDocument* previous = NULL;
    if (result == WPE_RESULT_OK) result = wpe_json_parse(text, size, &previous, NULL);
    if (result == WPE_RESULT_OK) result = wpe_ecs_remove_component(context->ecs, entity, type);
    if (result == WPE_RESULT_OK && !wpe__apply(&l, entity, key, value)) {
        result = l.r.result;
        wpe__Loader restore = wpe__edit_loader(context, key);
        const WPE_JsonValue* old = wpe_json_get(wpe_json_root(previous), key);
        if (wpe_ecs_has_component(context->ecs, entity, type))
            wpe_ecs_remove_component(context->ecs, entity, type);
        if (old) wpe__apply(&restore, entity, key, old);
    }
    wpe_json_free(previous);
    wpe_json_writer_destroy(writer);
    return result;
}

WPE_Result wpe_scene_remove_component(const WPE_SceneContext* context, WPE_Entity entity, const char* key) {
    if (!context || !context->ecs || !key) return WPE_RESULT_ERROR_NULL_ARG;
    if (!wpe_ecs_is_alive(context->ecs, entity)) return WPE_RESULT_ERROR_NOT_FOUND;
    const WPE_ComponentType type = wpe__edit_type(context, key);
    if (type == WPE_COMP_NONE) {
        wpe_log_error("unknown component \"%s\"", key);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    if (!wpe_ecs_has_component(context->ecs, entity, type)) return WPE_RESULT_ERROR_NOT_FOUND;
    return wpe_ecs_remove_component(context->ecs, entity, type);
}

WPE_Result wpe_scene_create_entity(const WPE_SceneContext* context, const WPE_JsonValue* description,
                                   WPE_Entity* out_entity) {
    if (!context || !context->ecs || !context->assets || !description || !out_entity)
        return WPE_RESULT_ERROR_NULL_ARG;
    *out_entity = WPE_ENTITY_NULL;
    wpe__Loader l = wpe__edit_loader(context, "create_entity");
    /* "id" is accepted and ignored, so a saved entity (wpe_scene_save_entity) can be fed back as a copy. */
    if (!WPE__JSON_CHECK_OBJECT(&l.r, description, "entity", "id", "name", "parent", "transform", "instance",
                                "mesh", "camera", "light", "billboard", "particles", "animator", "rigidbody",
                                "collider", "character", "audio_source", "ui", "sprite", "text", "button",
                                "toggle", "slider", "script", "components", "prefab"))
        return l.r.result;
    /* A prefab instance starts from its prefab; the description's parts then replace the root's. */
    const WPE_JsonValue* prefab = wpe_json_get(description, "prefab");
    WPE_Entity e = WPE_ENTITY_NULL;
    if (prefab) {
        uint32_t no_ids = 0;
        if (!wpe__load_instance_entry(&l, description, prefab, &e, &no_ids)) return l.r.result;
    } else {
        e = wpe_ecs_spawn(context->ecs);
        if (e == WPE_ENTITY_NULL) return WPE_RESULT_ERROR_CAPACITY;
    }
    /* The scene-file order: identity first, then components, scripts last. */
    static const char* const order[] = {"name",      "transform", "parent",    "instance",     "mesh",
                                        "camera",    "light",     "billboard", "particles",    "animator",
                                        "rigidbody", "collider",  "character", "audio_source", "ui",
                                        "sprite",    "text",      "button",    "toggle",       "slider"};
    bool ok = true;
    for (size_t i = 0; ok && i < sizeof order / sizeof order[0]; ++i) {
        const WPE_JsonValue* value = wpe_json_get(description, order[i]);
        if (!value) continue;
        const WPE_ComponentType type = wpe__edit_type(context, order[i]);
        if (type != WPE_COMP_NONE && wpe_ecs_has_component(context->ecs, e, type))
            wpe_ecs_remove_component(context->ecs, e, type);
        ok = wpe__apply(&l, e, order[i], value);
    }
    const WPE_JsonValue* components = wpe_json_get(description, "components");
    for (uint32_t i = 0; ok && i < wpe_json_count(components); ++i) {
        const char* key = NULL;
        wpe_json_member(components, i, &key);
        const WPE_ComponentType type = wpe__edit_type(context, key);
        if (type != WPE_COMP_NONE && wpe_ecs_has_component(context->ecs, e, type))
            wpe_ecs_remove_component(context->ecs, e, type);
    }
    if (ok && components) ok = wpe__load_components(&l, e, components);
    const WPE_JsonValue* script = wpe_json_get(description, "script");
    if (ok && script && wpe_ecs_has_component(context->ecs, e, WPE_COMP_SCRIPT))
        wpe_ecs_remove_component(context->ecs, e, WPE_COMP_SCRIPT);
    if (ok && script) ok = wpe__load_script(&l, e, script);
    if (!ok) {
        wpe_scene_destroy_recursive(context->ecs, e);
        return l.r.result;
    }
    *out_entity = e;
    return WPE_RESULT_OK;
}

WPE_Result wpe_scene_write_environment(const WPE_SceneEnvironment* environment, WPE_JsonWriter* writer) {
    if (!environment || !writer) return WPE_RESULT_ERROR_NULL_ARG;
    wpe__save_environment(writer, environment);
    return WPE_RESULT_OK;
}

WPE_Result wpe_scene_read_environment(const WPE_JsonValue* value, const char* name,
                                      WPE_SceneEnvironment* io_environment) {
    if (!value || !io_environment) return WPE_RESULT_ERROR_NULL_ARG;
    wpe__Loader l = {.r = {.name = name ? name : "environment", .kind = "edit"}};
    WPE_SceneEnvironment environment = *io_environment;
    if (!wpe__load_environment(&l, value, &environment)) return l.r.result;
    *io_environment = environment;
    return WPE_RESULT_OK;
}