#define WPE_LOG_MODULE "physics"
#include "physics/physics.h"

#include <box3d/box3d.h>
#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "core/alloc.h"
#include "core/debug.h"
#include "core/log.h"
#include "scene/scene.h"

#define WPE__DEFAULT_SUBSTEPS 4u
#define WPE__MAX_HULL_VERTICES 64
#define WPE__MIN_EXTENT 0.001f /* Smallest half extent / radius handed to Box3D. */
#define WPE__CIRCLE_SEGMENTS 16

enum {
    WPE__STATE_PENDING = 0,
    WPE__STATE_CREATED,
    WPE__STATE_FAILED,
};

struct WPE_Physics {
    WPE_Ecs* ecs;
    WPE_AssetManager* assets; /* Optional: models of convex/mesh colliders. */
    b3WorldId world;
    int substeps;
    uint32_t body_count;       /* Box3D bodies owned by this world (explicit and implicit static). */
    WPE_Entity* body_entities; /* Entity of each body, indexed by b3BodyId.index1 (dense in Box3D). */
    uint32_t body_entity_capacity;
    WPE_PhysicsEvent* events;
    uint32_t event_count;
    uint32_t event_capacity;
    bool shutting_down; /* The Box3D world is gone; hooks only release engine-side data. */
};

typedef struct wpe__Pose {
    WPE_Vec3 position;
    WPE_Quat rotation;
    WPE_Vec3 scale;
} wpe__Pose;

/* ---- Box3D global hooks --------------------------------------------------------------------------- */

/* Box3D requests aligned blocks; the original pointer is stored just before the aligned address. */
static void* wpe__b3_alloc(int32_t size, int32_t alignment) {
    wpe_assert(alignment > 0 && (alignment & (alignment - 1)) == 0);
    const size_t align = (size_t)alignment > sizeof(void*) ? (size_t)alignment : sizeof(void*);
    const size_t bytes = size > 0 ? (size_t)size : 1;
    uint8_t* raw = wpe_alloc(bytes + align + sizeof(void*));
    if (!raw) return NULL;
    const size_t misalignment = ((uintptr_t)raw + sizeof(void*)) & (align - 1);
    uint8_t* aligned = raw + sizeof(void*) + (misalignment ? align - misalignment : 0);
    memcpy(aligned - sizeof(void*), &raw, sizeof raw);
    return aligned;
}

static void wpe__b3_free(void* memory) {
    if (!memory) return;
    void* raw = NULL;
    memcpy(&raw, (uint8_t*)memory - sizeof(void*), sizeof raw);
    wpe_free(raw);
}

static int wpe__b3_assert(const char* condition, const char* file, int line) {
    wpe_assert_fail(condition, file, line);
}

/* Box3D's hooks are process-global. Installed before the first Box3D call, never changed afterwards
 * (memory allocated through them must be freed through them). Main thread only, like create. */
static void wpe__install_box3d_hooks(void) {
    static bool installed = false;
    if (installed) return;
    b3SetAllocator(wpe__b3_alloc, wpe__b3_free);
    b3SetAssertFcn(wpe__b3_assert);
    installed = true;
}

/* ---- Conversions ---------------------------------------------------------------------------------- */

static b3Vec3 wpe__b3_vec(WPE_Vec3 v) {
    return (b3Vec3){v.x, v.y, v.z};
}

static WPE_Vec3 wpe__vec(b3Vec3 v) {
    return wpe_vec3(v.x, v.y, v.z);
}

static b3Quat wpe__b3_quat(WPE_Quat q) {
    return (b3Quat){.v = {q.x, q.y, q.z}, .s = q.w};
}

static WPE_Quat wpe__quat(b3Quat q) {
    return (WPE_Quat){.x = q.v.x, .y = q.v.y, .z = q.v.z, .w = q.s};
}

/* ---- Body to entity mapping ----------------------------------------------------------------------- */

static bool wpe__map_body(WPE_Physics* physics, b3BodyId body, WPE_Entity entity) {
    const uint32_t index = (uint32_t)body.index1;
    if (index >= physics->body_entity_capacity) {
        uint32_t capacity = physics->body_entity_capacity ? physics->body_entity_capacity : 64;
        while (capacity <= index)
            capacity *= 2;
        WPE_Entity* entities = wpe_realloc(physics->body_entities, (size_t)capacity * sizeof *entities);
        if (!entities) return false;
        memset(entities + physics->body_entity_capacity, 0,
               (size_t)(capacity - physics->body_entity_capacity) * sizeof *entities);
        physics->body_entities = entities;
        physics->body_entity_capacity = capacity;
    }
    physics->body_entities[index] = entity;
    return true;
}

static WPE_Entity wpe__body_entity(const WPE_Physics* physics, b3BodyId body) {
    const uint32_t index = (uint32_t)body.index1;
    return index < physics->body_entity_capacity ? physics->body_entities[index] : WPE_ENTITY_NULL;
}

static WPE_Entity wpe__shape_entity(const WPE_Physics* physics, b3ShapeId shape) {
    return wpe__body_entity(physics, b3Shape_GetBody(shape));
}

static b3BodyType wpe__b3_body_type(WPE_BodyType type) {
    switch (type) {
    case WPE_BODY_KINEMATIC:
        return b3_kinematicBody;
    case WPE_BODY_DYNAMIC:
        return b3_dynamicBody;
    case WPE_BODY_STATIC:
        break;
    }
    return b3_staticBody;
}

static bool wpe__finite3(WPE_Vec3 v) {
    return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

/* World pose of an entity (identity for entities without transformed ancestry). */
static bool wpe__world_pose(const WPE_Ecs* ecs, WPE_Entity entity, wpe__Pose* out) {
    WPE_Mat4 world;
    if (!wpe_scene_compute_world_matrix(ecs, entity, &world)) return false;
    if (!wpe_mat4_decompose(&world, &out->position, &out->rotation, &out->scale)) return false;
    out->rotation = wpe_quat_normalize(out->rotation);
    return wpe__finite3(out->position) && wpe__finite3(out->scale);
}

/*
 * Whether game code moved an entity away from its body. Write-back goes through the parent's
 * inverse matrix, so the recomputed world pose carries float error that grows with distance from
 * the origin; the tolerance follows it so a body is never teleported onto its own position.
 */
static bool wpe__pose_differs(const wpe__Pose* pose, b3WorldTransform body) {
    const WPE_Vec3 delta = wpe_vec3_sub(pose->position, wpe__vec(body.p));
    const float tolerance = 1e-4f + 1e-5f * wpe_vec3_length(pose->position);
    if (wpe_vec3_length_sq(delta) > tolerance * tolerance) return true;
    const WPE_Quat q = wpe__quat(body.q);
    const float dot = fabsf(pose->rotation.x * q.x + pose->rotation.y * q.y + pose->rotation.z * q.z +
                            pose->rotation.w * q.w);
    return dot < 1.0f - 1e-6f;
}

/* ---- Shapes and bodies ---------------------------------------------------------------------------- */

static float wpe__max_abs(float a, float b) {
    return fabsf(a) > fabsf(b) ? fabsf(a) : fabsf(b);
}

static float wpe__at_least(float value, float minimum) {
    return value > minimum ? value : minimum;
}

/* Collects the vertex positions of a model mesh, scaled and offset into body space. */
static b3Vec3* wpe__mesh_points(const WPE_Model* model, const WPE_Collider* collider, WPE_Vec3 scale,
                                uint32_t* out_count) {
    const WPE_Mesh* mesh = &model->meshes[collider->mesh];
    uint32_t count = 0;
    for (uint32_t p = 0; p < mesh->primitive_count; ++p)
        count += model->primitives[mesh->first_primitive + p].vertex_count;
    b3Vec3* points = count ? wpe_alloc((size_t)count * sizeof *points) : NULL;
    if (!points) return NULL;
    uint32_t n = 0;
    for (uint32_t p = 0; p < mesh->primitive_count; ++p) {
        const WPE_MeshPrimitive* primitive = &model->primitives[mesh->first_primitive + p];
        for (uint32_t v = 0; v < primitive->vertex_count; ++v) {
            const WPE_Vec3 position = model->vertices[primitive->vertex_offset + v].position;
            points[n++] = wpe__b3_vec(wpe_vec3_mul(wpe_vec3_add(position, collider->offset), scale));
        }
    }
    *out_count = count;
    return points;
}

/* Builds an owned collision mesh (unscaled; the shape applies the entity scale). */
static b3MeshData* wpe__build_mesh(const WPE_Model* model, const WPE_Collider* collider) {
    const WPE_Mesh* mesh = &model->meshes[collider->mesh];
    uint32_t point_count = 0;
    b3Vec3* points = wpe__mesh_points(model, collider, wpe_vec3(1.0f, 1.0f, 1.0f), &point_count);
    uint32_t index_count = 0;
    for (uint32_t p = 0; p < mesh->primitive_count; ++p)
        index_count += model->primitives[mesh->first_primitive + p].index_count / 3 * 3;
    int32_t* indices = index_count ? wpe_alloc((size_t)index_count * sizeof *indices) : NULL;
    if (!points || !indices || point_count > INT32_MAX || index_count > INT32_MAX) {
        wpe_free(points);
        wpe_free(indices);
        return NULL;
    }

    /* Primitive indices are relative to their own vertex range; points are concatenated in order. */
    uint32_t n = 0;
    uint32_t base = 0;
    for (uint32_t p = 0; p < mesh->primitive_count; ++p) {
        const WPE_MeshPrimitive* primitive = &model->primitives[mesh->first_primitive + p];
        const uint32_t usable = primitive->index_count / 3 * 3;
        for (uint32_t i = 0; i < usable; ++i)
            indices[n++] = (int32_t)(base + model->indices[primitive->index_offset + i]);
        base += primitive->vertex_count;
    }

    b3MeshDef def = {
        .vertices = points,
        .indices = indices,
        .vertexCount = (int)point_count,
        .triangleCount = (int)(index_count / 3),
        .weldVertices = true,
        .weldTolerance = 1e-4f,
        .identifyEdges = true,
    };
    b3MeshData* data = b3CreateMesh(&def, NULL, 0);
    wpe_free(points);
    wpe_free(indices);
    return data;
}

static WPE_Result wpe__validate_collider(const WPE_Collider* collider, const WPE_Model* model,
                                         b3BodyType body_type) {
    if (!wpe__finite3(collider->size) || !wpe__finite3(collider->offset) || !isfinite(collider->density) ||
        collider->density < 0.0f || !isfinite(collider->friction) || collider->friction < 0.0f ||
        !isfinite(collider->restitution) || collider->restitution < 0.0f) {
        wpe_log_error("collider has a non-finite or negative size, density, friction or restitution");
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    switch (collider->shape) {
    case WPE_COLLIDER_BOX:
    case WPE_COLLIDER_SPHERE:
    case WPE_COLLIDER_CAPSULE:
        return WPE_RESULT_OK;
    case WPE_COLLIDER_CONVEX:
    case WPE_COLLIDER_MESH:
        if (!model || collider->mesh >= model->mesh_count) {
            wpe_log_error("convex/mesh collider needs a loaded model asset and a valid mesh index");
            return WPE_RESULT_ERROR_INVALID_ARG;
        }
        if (collider->shape == WPE_COLLIDER_MESH && body_type == b3_dynamicBody) {
            wpe_log_error("mesh colliders cannot be dynamic; use a convex collider instead");
            return WPE_RESULT_ERROR_UNSUPPORTED;
        }
        return WPE_RESULT_OK;
    }
    wpe_log_error("unknown collider shape %d", (int)collider->shape);
    return WPE_RESULT_ERROR_INVALID_ARG;
}

/* Creates the collider's shape on `body`, using the entity's world scale. */
static WPE_Result wpe__create_shape(WPE_Physics* physics, WPE_Entity entity, WPE_Collider* collider,
                                    b3BodyId body, WPE_Vec3 scale) {
    const bool uses_model = collider->shape == WPE_COLLIDER_CONVEX || collider->shape == WPE_COLLIDER_MESH;
    const WPE_Model* model = uses_model ? wpe_asset_model(physics->assets, collider->model) : NULL;
    const WPE_Result valid = wpe__validate_collider(collider, model, b3Body_GetType(body));
    if (valid != WPE_RESULT_OK) return valid;

    b3ShapeDef def = b3DefaultShapeDef();
    def.density = collider->density;
    def.baseMaterial.friction = collider->friction;
    def.baseMaterial.restitution = collider->restitution;
    def.isSensor = collider->is_trigger;
    def.enableSensorEvents = true; /* Triggers report, other shapes are reported. */
    def.enableContactEvents = !collider->is_trigger;

    const WPE_Vec3 offset = wpe_vec3_mul(collider->offset, scale);
    b3ShapeId shape = b3_nullShapeId;
    switch (collider->shape) {
    case WPE_COLLIDER_BOX: {
        const b3BoxHull box = b3MakeOffsetBoxHull(
            wpe__at_least(fabsf(collider->size.x * scale.x) * 0.5f, WPE__MIN_EXTENT),
            wpe__at_least(fabsf(collider->size.y * scale.y) * 0.5f, WPE__MIN_EXTENT),
            wpe__at_least(fabsf(collider->size.z * scale.z) * 0.5f, WPE__MIN_EXTENT), wpe__b3_vec(offset));
        shape = b3CreateHullShape(body, &def, &box.base);
        break;
    }
    case WPE_COLLIDER_SPHERE: {
        const float largest = wpe__max_abs(wpe__max_abs(scale.x, scale.y), scale.z);
        const b3Sphere sphere = {.center = wpe__b3_vec(offset),
                                 .radius = wpe__at_least(fabsf(collider->size.x) * largest, WPE__MIN_EXTENT)};
        shape = b3CreateSphereShape(body, &def, &sphere);
        break;
    }
    case WPE_COLLIDER_CAPSULE: {
        const float radius =
            wpe__at_least(fabsf(collider->size.x) * wpe__max_abs(scale.x, scale.z), WPE__MIN_EXTENT);
        const float half_segment = fabsf(collider->size.y * scale.y) * 0.5f - radius;
        if (half_segment <= WPE__MIN_EXTENT) {
            /* Height <= 2 * radius degenerates to a sphere. */
            const b3Sphere sphere = {.center = wpe__b3_vec(offset), .radius = radius};
            shape = b3CreateSphereShape(body, &def, &sphere);
        } else {
            const b3Capsule capsule = {
                .center1 = wpe__b3_vec(wpe_vec3_add(offset, wpe_vec3(0.0f, -half_segment, 0.0f))),
                .center2 = wpe__b3_vec(wpe_vec3_add(offset, wpe_vec3(0.0f, half_segment, 0.0f))),
                .radius = radius,
            };
            shape = b3CreateCapsuleShape(body, &def, &capsule);
        }
        break;
    }
    case WPE_COLLIDER_CONVEX: {
        uint32_t count = 0;
        b3Vec3* points = wpe__mesh_points(model, collider, scale, &count);
        if (!points) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
        b3HullData* hull =
            count <= INT32_MAX ? b3CreateHull(points, (int)count, WPE__MAX_HULL_VERTICES) : NULL;
        wpe_free(points);
        if (!hull) {
            wpe_log_error("could not build a convex hull from mesh %u (flat or fewer than 4 points?)",
                          collider->mesh);
            return WPE_RESULT_ERROR_INVALID_ARG;
        }
        shape = b3CreateHullShape(body, &def, hull); /* Copies the hull. */
        b3DestroyHull(hull);
        break;
    }
    case WPE_COLLIDER_MESH: {
        b3MeshData* mesh = wpe__build_mesh(model, collider);
        if (!mesh) {
            wpe_log_error("could not build a collision mesh from mesh %u", collider->mesh);
            return WPE_RESULT_ERROR_OUT_OF_MEMORY;
        }
        const b3Vec3 mesh_scale = {
            wpe__at_least(fabsf(scale.x), WPE__MIN_EXTENT) * (scale.x < 0.0f ? -1.0f : 1.0f),
            wpe__at_least(fabsf(scale.y), WPE__MIN_EXTENT) * (scale.y < 0.0f ? -1.0f : 1.0f),
            wpe__at_least(fabsf(scale.z), WPE__MIN_EXTENT) * (scale.z < 0.0f ? -1.0f : 1.0f)};
        shape = b3CreateMeshShape(body, &def, mesh, mesh_scale); /* References the mesh. */
        if (B3_IS_NULL(shape)) {
            b3DestroyMesh(mesh);
            break;
        }
        collider->wpe__mesh = mesh;
        break;
    }
    }
    if (B3_IS_NULL(shape)) {
        wpe_log_error("Box3D rejected the collider shape of entity 0x%08x", entity);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    collider->wpe__shape = b3StoreShapeId(shape);
    collider->wpe__scale = scale;
    return WPE_RESULT_OK;
}

/* Makes the component's held model reference match its `model` field (game code may have changed
 * it before a refresh). The reference lives as long as the component, so rebuilds can always reach
 * the model. */
static void wpe__hold_model(WPE_Physics* physics, WPE_Collider* collider) {
    if (collider->wpe__model_ref == collider->model) return;
    if (wpe_asset_model(physics->assets, collider->model)) wpe_asset_retain(physics->assets, collider->model);
    wpe_asset_release(physics->assets, collider->wpe__model_ref);
    collider->wpe__model_ref =
        wpe_asset_model(physics->assets, collider->model) ? collider->model : WPE_ASSET_NONE;
}

static b3BodyId wpe__create_b3_body(WPE_Physics* physics, WPE_Entity entity, const WPE_RigidBody* rb,
                                    const wpe__Pose* pose) {
    b3BodyDef def = b3DefaultBodyDef();
    def.type = rb ? wpe__b3_body_type(rb->type) : b3_staticBody;
    def.position = wpe__b3_vec(pose->position);
    def.rotation = wpe__b3_quat(pose->rotation);
    if (rb) {
        def.linearDamping = rb->linear_damping;
        def.angularDamping = rb->angular_damping;
        def.gravityScale = rb->gravity_scale;
        def.isBullet = rb->continuous && rb->type == WPE_BODY_DYNAMIC;
        def.motionLocks.angularX = def.motionLocks.angularY = def.motionLocks.angularZ = rb->lock_rotation;
        if (rb->type != WPE_BODY_STATIC) def.linearVelocity = wpe__b3_vec(rb->initial_velocity);
    }
    const b3BodyId body = b3CreateBody(physics->world, &def);
    if (B3_IS_NULL(body)) return body;
    if (!wpe__map_body(physics, body, entity)) {
        b3DestroyBody(body);
        return b3_nullBodyId;
    }
    ++physics->body_count;
    return body;
}

static void wpe__destroy_b3_body(WPE_Physics* physics, uint64_t stored) {
    const b3BodyId body = b3LoadBodyId(stored);
    physics->body_entities[(uint32_t)body.index1] = WPE_ENTITY_NULL;
    if (!physics->shutting_down) b3DestroyBody(body);
    wpe_assert(physics->body_count > 0);
    --physics->body_count;
}

/* Releases a collider's simulation objects. `body_destroyed`: its rigid body was destroyed already
 * (taking the shape with it). */
static void wpe__release_collider(WPE_Physics* physics, WPE_Collider* collider, bool body_destroyed) {
    if (collider->wpe__state == WPE__STATE_CREATED) {
        if (collider->wpe__static_body) {
            wpe__destroy_b3_body(physics, collider->wpe__static_body);
        } else if (!body_destroyed && !physics->shutting_down) {
            b3DestroyShape(b3LoadShapeId(collider->wpe__shape), true);
        }
    }
    /* After the shape: Box3D references the mesh until then. */
    if (collider->wpe__mesh) b3DestroyMesh(collider->wpe__mesh);
    collider->wpe__mesh = NULL;
    collider->wpe__shape = 0;
    collider->wpe__static_body = 0;
    collider->wpe__state = WPE__STATE_PENDING;
}

static void wpe__release_rigidbody(WPE_Physics* physics, WPE_Entity entity, WPE_RigidBody* rb) {
    if (rb->wpe__state == WPE__STATE_CREATED) {
        wpe__destroy_b3_body(physics, rb->wpe__body);
        /* The body took its shape along; the collider falls back to an implicit static body. */
        WPE_Collider* collider = wpe_ecs_get_component(physics->ecs, entity, WPE_COMP_PHYSICS_COLLIDER);
        if (collider && collider->wpe__state == WPE__STATE_CREATED && !collider->wpe__static_body)
            wpe__release_collider(physics, collider, true);
    }
    rb->wpe__body = 0;
    rb->wpe__target_move = false;
    rb->wpe__state = WPE__STATE_PENDING;
}

/* Creates whatever is still missing for an entity. No ECS structural changes (safe while iterating). */
static void wpe__ensure_created(WPE_Physics* physics, WPE_Entity entity) {
    WPE_RigidBody* rb = wpe_ecs_get_component(physics->ecs, entity, WPE_COMP_PHYSICS_RIGIDBODY);
    WPE_Collider* collider = wpe_ecs_get_component(physics->ecs, entity, WPE_COMP_PHYSICS_COLLIDER);
    if (collider) wpe__hold_model(physics, collider);
    const bool needs_body = rb ? rb->wpe__state == WPE__STATE_PENDING : false;
    const bool needs_shape =
        collider && collider->wpe__state != WPE__STATE_FAILED &&
        (collider->wpe__state == WPE__STATE_PENDING || (rb && collider->wpe__static_body));
    if (!needs_body && !needs_shape) return;

    wpe__Pose pose;
    if (!wpe__world_pose(physics->ecs, entity, &pose)) {
        wpe_log_error("entity 0x%08x has a degenerate world transform (zero scale?); physics disabled for it "
                      "until wpe_physics_refresh()",
                      entity);
        if (needs_body) rb->wpe__state = WPE__STATE_FAILED;
        if (collider && collider->wpe__state == WPE__STATE_PENDING) collider->wpe__state = WPE__STATE_FAILED;
        return;
    }

    if (rb) {
        if (needs_body) {
            if (!isfinite(rb->linear_damping) || rb->linear_damping < 0.0f ||
                !isfinite(rb->angular_damping) || rb->angular_damping < 0.0f ||
                !isfinite(rb->gravity_scale) || !wpe__finite3(rb->initial_velocity)) {
                wpe_log_error(
                    "rigid body of entity 0x%08x has invalid damping, gravity scale or initial velocity",
                    entity);
                rb->wpe__state = WPE__STATE_FAILED;
                return;
            }
            const b3BodyId body = wpe__create_b3_body(physics, entity, rb, &pose);
            if (B3_IS_NULL(body)) {
                rb->wpe__state = WPE__STATE_FAILED;
                return;
            }
            rb->wpe__body = b3StoreBodyId(body);
            rb->wpe__state = WPE__STATE_CREATED;
        }
        if (rb->wpe__state != WPE__STATE_CREATED || !collider || collider->wpe__state == WPE__STATE_FAILED)
            return;
        /* A collider that was level geometry moves onto the new rigid body. */
        if (collider->wpe__state == WPE__STATE_CREATED && collider->wpe__static_body)
            wpe__release_collider(physics, collider, false);
        if (collider->wpe__state != WPE__STATE_PENDING) return;
        collider->wpe__state = wpe__create_shape(physics, entity, collider, b3LoadBodyId(rb->wpe__body),
                                                 pose.scale) == WPE_RESULT_OK
                                   ? WPE__STATE_CREATED
                                   : WPE__STATE_FAILED;
        return;
    }

    /* Collider without a rigid body: level geometry on an implicit static body. */
    const b3BodyId body = wpe__create_b3_body(physics, entity, NULL, &pose);
    if (B3_IS_NULL(body)) {
        collider->wpe__state = WPE__STATE_FAILED;
        return;
    }
    if (wpe__create_shape(physics, entity, collider, body, pose.scale) != WPE_RESULT_OK) {
        wpe__destroy_b3_body(physics, b3StoreBodyId(body));
        collider->wpe__state = WPE__STATE_FAILED;
        return;
    }
    collider->wpe__static_body = b3StoreBodyId(body);
    collider->wpe__state = WPE__STATE_CREATED;
}

/* Moves a kinematic body toward `pose` over the step, with velocity, so it pushes dynamic bodies.
 * `target_move` remembers a move in progress, so the body stops when it arrives. */
static void wpe__follow_kinematic(b3BodyId body, const wpe__Pose* pose, float dt, bool* target_move) {
    if (wpe__pose_differs(pose, b3Body_GetTransform(body))) {
        const b3WorldTransform target = {.p = wpe__b3_vec(pose->position), .q = wpe__b3_quat(pose->rotation)};
        b3Body_SetTargetTransform(body, target, dt, true);
        *target_move = true;
    } else if (*target_move) {
        /* Arrived: stop instead of coasting with the velocity of the last move. */
        b3Body_SetLinearVelocity(body, (b3Vec3){0.0f, 0.0f, 0.0f});
        b3Body_SetAngularVelocity(body, (b3Vec3){0.0f, 0.0f, 0.0f});
        *target_move = false;
    }
}

/* Character controllers (defined with the character code below). */
static void wpe__ensure_character(WPE_Physics* physics, WPE_Entity entity, WPE_CharacterController* c);
static void wpe__release_character(WPE_Physics* physics, WPE_CharacterController* c);
static bool wpe__character_pose(const WPE_Physics* physics, WPE_Entity entity, wpe__Pose* out);

/* Follows game-code moves of an entity (see file comment of physics.h, step 2). */
static void wpe__sync_body(WPE_Physics* physics, WPE_Entity entity, uint64_t stored, WPE_RigidBody* rb,
                           float dt) {
    if (!wpe_ecs_has_component(physics->ecs, entity, WPE_COMP_TRANSFORM)) return;
    wpe__Pose pose;
    if (!wpe__world_pose(physics->ecs, entity, &pose)) return;
    const b3BodyId body = b3LoadBodyId(stored);
    if (rb && rb->type == WPE_BODY_KINEMATIC) {
        wpe__follow_kinematic(body, &pose, dt, &rb->wpe__target_move);
        return;
    }
    const bool moved = wpe__pose_differs(&pose, b3Body_GetTransform(body));
    if (moved) {
        b3Body_SetTransform(body, wpe__b3_vec(pose.position), wpe__b3_quat(pose.rotation));
        if (rb && rb->type == WPE_BODY_DYNAMIC) b3Body_SetAwake(body, true);
    }
}

/* Writes a simulated world pose into the entity's local transform. */
static void wpe__write_back(WPE_Physics* physics, WPE_Entity entity, b3WorldTransform xf) {
    WPE_Vec3 position = wpe__vec(xf.p);
    WPE_Quat rotation = wpe__quat(xf.q);
    const WPE_Entity parent = wpe_scene_get_parent(physics->ecs, entity);
    if (parent != WPE_ENTITY_NULL) {
        WPE_Mat4 parent_world;
        WPE_Mat4 inverse;
        if (!wpe_scene_compute_world_matrix(physics->ecs, parent, &parent_world) ||
            !wpe_mat4_inverse(&parent_world, &inverse))
            return;
        const WPE_Mat4 world = wpe_mat4_from_trs(position, rotation, wpe_vec3(1.0f, 1.0f, 1.0f));
        const WPE_Mat4 local = wpe_mat4_mul(&inverse, &world);
        WPE_Vec3 local_scale;
        if (!wpe_mat4_decompose(&local, &position, &rotation, &local_scale)) return;
    }
    WPE_Transform* transform = wpe_ecs_get_component(physics->ecs, entity, WPE_COMP_TRANSFORM);
    if (!transform) return;
    transform->position = position;
    transform->rotation = rotation;
}

static bool wpe__push_event(WPE_Physics* physics, WPE_PhysicsEventType type, b3ShapeId a, b3ShapeId b) {
    /* End events can refer to shapes destroyed since the previous step; skip those. */
    if (!b3Shape_IsValid(a) || !b3Shape_IsValid(b)) return true;
    if (physics->event_count == physics->event_capacity) {
        const uint32_t capacity = physics->event_capacity ? physics->event_capacity * 2 : 64;
        WPE_PhysicsEvent* events = wpe_realloc(physics->events, (size_t)capacity * sizeof *events);
        if (!events) return false;
        physics->events = events;
        physics->event_capacity = capacity;
    }
    physics->events[physics->event_count++] = (WPE_PhysicsEvent){
        .type = type, .entity_a = wpe__shape_entity(physics, a), .entity_b = wpe__shape_entity(physics, b)};
    return true;
}

static bool wpe__collect_events(WPE_Physics* physics) {
    physics->event_count = 0;
    bool ok = true;
    const b3ContactEvents contacts = b3World_GetContactEvents(physics->world);
    for (int i = 0; i < contacts.beginCount; ++i)
        ok &= wpe__push_event(physics, WPE_PHYSICS_CONTACT_BEGIN, contacts.beginEvents[i].shapeIdA,
                              contacts.beginEvents[i].shapeIdB);
    for (int i = 0; i < contacts.endCount; ++i)
        ok &= wpe__push_event(physics, WPE_PHYSICS_CONTACT_END, contacts.endEvents[i].shapeIdA,
                              contacts.endEvents[i].shapeIdB);
    const b3SensorEvents sensors = b3World_GetSensorEvents(physics->world);
    for (int i = 0; i < sensors.beginCount; ++i)
        ok &= wpe__push_event(physics, WPE_PHYSICS_TRIGGER_ENTER, sensors.beginEvents[i].sensorShapeId,
                              sensors.beginEvents[i].visitorShapeId);
    for (int i = 0; i < sensors.endCount; ++i)
        ok &= wpe__push_event(physics, WPE_PHYSICS_TRIGGER_EXIT, sensors.endEvents[i].sensorShapeId,
                              sensors.endEvents[i].visitorShapeId);
    return ok;
}

/* ---- ECS hooks ------------------------------------------------------------------------------------ */

static void wpe__rigidbody_on_remove(WPE_Ecs* ecs, WPE_Entity entity, void* component, void* user_data) {
    (void)ecs;
    wpe__release_rigidbody(user_data, entity, component);
}

static void wpe__collider_on_add(WPE_Ecs* ecs, WPE_Entity entity, void* component, void* user_data) {
    (void)ecs;
    (void)entity;
    WPE_Collider* collider = component;
    collider->wpe__model_ref = WPE_ASSET_NONE; /* Private fields of the initial data are not trusted. */
    wpe__hold_model(user_data, collider);
}

static void wpe__collider_on_remove(WPE_Ecs* ecs, WPE_Entity entity, void* component, void* user_data) {
    (void)ecs;
    (void)entity;
    WPE_Collider* collider = component;
    wpe__release_collider(user_data, collider, false);
    WPE_Physics* physics = user_data;
    wpe_asset_release(physics->assets, collider->wpe__model_ref);
    collider->wpe__model_ref = WPE_ASSET_NONE;
}

static void wpe__character_on_add(WPE_Ecs* ecs, WPE_Entity entity, void* component, void* user_data) {
    (void)ecs;
    (void)entity;
    (void)user_data;
    WPE_CharacterController* c = component;
    /* Private fields of the initial data (a copy of another character) are not trusted. */
    c->wpe__body = 0;
    c->wpe__state = WPE__STATE_PENDING;
    c->wpe__target_move = false;
}

static void wpe__character_on_remove(WPE_Ecs* ecs, WPE_Entity entity, void* component, void* user_data) {
    (void)ecs;
    (void)entity;
    wpe__release_character(user_data, component);
}

/* ---- Public API ----------------------------------------------------------------------------------- */

WPE_RigidBody wpe_rigidbody_default(WPE_BodyType type) {
    return (WPE_RigidBody){.type = type, .gravity_scale = 1.0f};
}

WPE_Collider wpe_collider_default(WPE_ColliderShape shape) {
    return (WPE_Collider){
        .shape = shape,
        .size = wpe_vec3(1.0f, 1.0f, 1.0f),
        .density = 1.0f,
        .friction = 0.6f,
    };
}

WPE_Result wpe_physics_create(const WPE_PhysicsDesc* desc, WPE_Physics** out_physics) {
    if (!desc || !desc->ecs || !out_physics) return WPE_RESULT_ERROR_NULL_ARG;
    *out_physics = NULL;
    if (!wpe__finite3(desc->gravity) || desc->substeps > 64) return WPE_RESULT_ERROR_INVALID_ARG;
    if (!wpe_ecs_is_registered(desc->ecs, WPE_COMP_TRANSFORM)) {
        wpe_log_error("register the scene components before creating physics");
        return WPE_RESULT_ERROR_INVALID_STATE;
    }
    if (wpe_ecs_is_registered(desc->ecs, WPE_COMP_PHYSICS_RIGIDBODY) ||
        wpe_ecs_is_registered(desc->ecs, WPE_COMP_PHYSICS_COLLIDER)) {
        wpe_log_error("the world already has physics");
        return WPE_RESULT_ERROR_ALREADY_EXISTS;
    }

    WPE_Physics* physics = wpe_alloc_zeroed(1, sizeof *physics);
    if (!physics) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    physics->ecs = desc->ecs;
    physics->assets = desc->assets;
    physics->substeps = (int)(desc->substeps ? desc->substeps : WPE__DEFAULT_SUBSTEPS);

    wpe__install_box3d_hooks();
    b3WorldDef def = b3DefaultWorldDef();
    def.gravity = wpe__b3_vec(desc->gravity);
    physics->world = b3CreateWorld(&def);
    if (B3_IS_NULL(physics->world)) {
        wpe_free(physics);
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }

    WPE_Result result = wpe_ecs_register_component(physics->ecs, WPE_COMP_PHYSICS_RIGIDBODY,
                                                   &(WPE_ComponentDesc){.name = "RigidBody",
                                                                        .size = sizeof(WPE_RigidBody),
                                                                        .alignment = alignof(WPE_RigidBody),
                                                                        .on_remove = wpe__rigidbody_on_remove,
                                                                        .hook_user_data = physics});
    if (result == WPE_RESULT_OK) {
        result = wpe_ecs_register_component(physics->ecs, WPE_COMP_PHYSICS_COLLIDER,
                                            &(WPE_ComponentDesc){.name = "Collider",
                                                                 .size = sizeof(WPE_Collider),
                                                                 .alignment = alignof(WPE_Collider),
                                                                 .on_add = wpe__collider_on_add,
                                                                 .on_remove = wpe__collider_on_remove,
                                                                 .hook_user_data = physics});
        if (result != WPE_RESULT_OK) wpe_ecs_unregister_component(physics->ecs, WPE_COMP_PHYSICS_RIGIDBODY);
    }
    if (result == WPE_RESULT_OK) {
        result =
            wpe_ecs_register_component(physics->ecs, WPE_COMP_CHARACTER,
                                       &(WPE_ComponentDesc){.name = "CharacterController",
                                                            .size = sizeof(WPE_CharacterController),
                                                            .alignment = alignof(WPE_CharacterController),
                                                            .on_add = wpe__character_on_add,
                                                            .on_remove = wpe__character_on_remove,
                                                            .hook_user_data = physics});
        if (result != WPE_RESULT_OK) {
            wpe_ecs_unregister_component(physics->ecs, WPE_COMP_PHYSICS_COLLIDER);
            wpe_ecs_unregister_component(physics->ecs, WPE_COMP_PHYSICS_RIGIDBODY);
        }
    }
    if (result != WPE_RESULT_OK) {
        b3DestroyWorld(physics->world);
        wpe_free(physics);
        return result;
    }
    *out_physics = physics;
    return WPE_RESULT_OK;
}

static void wpe__remove_all(WPE_Ecs* ecs, WPE_ComponentType type) {
    WPE_EcsPoolView view;
    while (wpe_ecs_pool_view(ecs, type, &view) == WPE_RESULT_OK && view.count > 0)
        wpe_ecs_remove_component(ecs, view.entities[view.count - 1], type);
}

void wpe_physics_destroy(WPE_Physics* physics) {
    if (!physics) return;
    /* Destroying the Box3D world releases every body and shape at once; the hooks then only free
     * engine-side data. */
    b3DestroyWorld(physics->world);
    physics->shutting_down = true;
    wpe__remove_all(physics->ecs, WPE_COMP_CHARACTER);
    wpe__remove_all(physics->ecs, WPE_COMP_PHYSICS_COLLIDER);
    wpe__remove_all(physics->ecs, WPE_COMP_PHYSICS_RIGIDBODY);
    wpe_ecs_unregister_component(physics->ecs, WPE_COMP_CHARACTER);
    wpe_ecs_unregister_component(physics->ecs, WPE_COMP_PHYSICS_COLLIDER);
    wpe_ecs_unregister_component(physics->ecs, WPE_COMP_PHYSICS_RIGIDBODY);
    wpe_assert(physics->body_count == 0);
    wpe_free(physics->body_entities);
    wpe_free(physics->events);
    wpe_free(physics);
}

WPE_Result wpe_physics_step(WPE_Physics* physics, float dt) {
    if (!physics) return WPE_RESULT_ERROR_NULL_ARG;
    if (!(dt > 0.0f) || !isfinite(dt)) return WPE_RESULT_ERROR_INVALID_ARG;
    WPE_Ecs* ecs = physics->ecs;

    WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_PHYSICS_RIGIDBODY)});
    while (wpe_ecs_iter_next(&it)) {
        wpe__ensure_created(physics, it.entity);
        WPE_RigidBody* rb = wpe_ecs_iter_get(&it, WPE_COMP_PHYSICS_RIGIDBODY);
        if (rb->wpe__state == WPE__STATE_CREATED) wpe__sync_body(physics, it.entity, rb->wpe__body, rb, dt);
    }
    it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_PHYSICS_COLLIDER),
                                           .none = WPE_ECS_MASK(WPE_COMP_PHYSICS_RIGIDBODY)});
    while (wpe_ecs_iter_next(&it)) {
        wpe__ensure_created(physics, it.entity);
        WPE_Collider* collider = wpe_ecs_iter_get(&it, WPE_COMP_PHYSICS_COLLIDER);
        if (collider->wpe__state == WPE__STATE_CREATED)
            wpe__sync_body(physics, it.entity, collider->wpe__static_body, NULL, dt);
    }
    it = wpe_ecs_query(
        ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_CHARACTER) | WPE_ECS_MASK(WPE_COMP_TRANSFORM)});
    while (wpe_ecs_iter_next(&it)) {
        WPE_CharacterController* c = wpe_ecs_iter_get(&it, WPE_COMP_CHARACTER);
        wpe__ensure_character(physics, it.entity, c);
        wpe__Pose pose;
        if (c->wpe__state == WPE__STATE_CREATED && wpe__character_pose(physics, it.entity, &pose))
            wpe__follow_kinematic(b3LoadBodyId(c->wpe__body), &pose, dt, &c->wpe__target_move);
    }

    b3World_Step(physics->world, dt, physics->substeps);

    const b3BodyEvents moves = b3World_GetBodyEvents(physics->world);
    for (int i = 0; i < moves.moveCount; ++i) {
        const b3BodyMoveEvent* move = &moves.moveEvents[i];
        const WPE_Entity entity = wpe__body_entity(physics, move->bodyId);
        const WPE_RigidBody* rb = wpe_ecs_get_component(ecs, entity, WPE_COMP_PHYSICS_RIGIDBODY);
        if (rb && rb->wpe__state == WPE__STATE_CREATED && rb->wpe__body == b3StoreBodyId(move->bodyId))
            wpe__write_back(physics, entity, move->transform);
    }
    return wpe__collect_events(physics) ? WPE_RESULT_OK : WPE_RESULT_ERROR_OUT_OF_MEMORY;
}

WPE_Result wpe_physics_create_bodies(WPE_Physics* physics) {
    if (!physics) return WPE_RESULT_ERROR_NULL_ARG;
    WPE_EcsIter it =
        wpe_ecs_query(physics->ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_PHYSICS_RIGIDBODY)});
    while (wpe_ecs_iter_next(&it))
        wpe__ensure_created(physics, it.entity);
    it = wpe_ecs_query(physics->ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_PHYSICS_COLLIDER),
                                                    .none = WPE_ECS_MASK(WPE_COMP_PHYSICS_RIGIDBODY)});
    while (wpe_ecs_iter_next(&it))
        wpe__ensure_created(physics, it.entity);
    it = wpe_ecs_query(physics->ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_CHARACTER)});
    while (wpe_ecs_iter_next(&it))
        wpe__ensure_character(physics, it.entity, wpe_ecs_iter_get(&it, WPE_COMP_CHARACTER));
    return WPE_RESULT_OK;
}

WPE_Result wpe_physics_refresh(WPE_Physics* physics, WPE_Entity entity) {
    if (!physics) return WPE_RESULT_ERROR_NULL_ARG;
    if (!wpe_ecs_is_alive(physics->ecs, entity)) return WPE_RESULT_ERROR_NOT_FOUND;
    WPE_Collider* collider = wpe_ecs_get_component(physics->ecs, entity, WPE_COMP_PHYSICS_COLLIDER);
    WPE_RigidBody* rb = wpe_ecs_get_component(physics->ecs, entity, WPE_COMP_PHYSICS_RIGIDBODY);
    WPE_CharacterController* character = wpe_ecs_get_component(physics->ecs, entity, WPE_COMP_CHARACTER);
    if (!collider && !rb && !character) return WPE_RESULT_ERROR_NOT_FOUND;
    if (collider) wpe__release_collider(physics, collider, false);
    if (rb) wpe__release_rigidbody(physics, entity, rb);
    if (character) wpe__release_character(physics, character);
    wpe__ensure_created(physics, entity);
    if (character) wpe__ensure_character(physics, entity, character);
    if ((rb && rb->wpe__state != WPE__STATE_CREATED) ||
        (collider && collider->wpe__state != WPE__STATE_CREATED) ||
        (character && character->wpe__state != WPE__STATE_CREATED))
        return WPE_RESULT_ERROR_INVALID_ARG;
    return WPE_RESULT_OK;
}

static bool wpe__scale_differs(WPE_Vec3 a, WPE_Vec3 b) {
    for (int i = 0; i < 3; ++i) {
        const float magnitude = fabsf(a.v[i]) > 1.0f ? fabsf(a.v[i]) : 1.0f;
        if (fabsf(a.v[i] - b.v[i]) > 1e-5f * magnitude) return true;
    }
    return false;
}

/* Moves a body to its entity's world pose, as an edit (no velocity, no kinematic move). */
static void wpe__place_body(WPE_Physics* physics, WPE_Entity entity, uint64_t stored) {
    wpe__Pose pose;
    if (!wpe__world_pose(physics->ecs, entity, &pose)) return;
    const b3BodyId body = b3LoadBodyId(stored);
    if (wpe__pose_differs(&pose, b3Body_GetTransform(body)))
        b3Body_SetTransform(body, wpe__b3_vec(pose.position), wpe__b3_quat(pose.rotation));
}

WPE_Result wpe_physics_sync(WPE_Physics* physics) {
    if (!physics) return WPE_RESULT_ERROR_NULL_ARG;
    WPE_Ecs* ecs = physics->ecs;
    wpe_physics_create_bodies(physics);
    /* Shapes are built for one world scale: rebuild those whose entity was rescaled. Rebuilding
     * changes no ECS structure, so iterating the pool stays valid. */
    WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_PHYSICS_COLLIDER)});
    while (wpe_ecs_iter_next(&it)) {
        const WPE_Collider* collider = wpe_ecs_iter_get(&it, WPE_COMP_PHYSICS_COLLIDER);
        wpe__Pose pose;
        if (collider->wpe__state == WPE__STATE_CREATED && wpe__world_pose(ecs, it.entity, &pose) &&
            wpe__scale_differs(pose.scale, collider->wpe__scale))
            wpe_physics_refresh(physics, it.entity);
    }
    it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_PHYSICS_RIGIDBODY)});
    while (wpe_ecs_iter_next(&it)) {
        const WPE_RigidBody* rb = wpe_ecs_iter_get(&it, WPE_COMP_PHYSICS_RIGIDBODY);
        if (rb->wpe__state == WPE__STATE_CREATED) wpe__place_body(physics, it.entity, rb->wpe__body);
    }
    it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_PHYSICS_COLLIDER),
                                           .none = WPE_ECS_MASK(WPE_COMP_PHYSICS_RIGIDBODY)});
    while (wpe_ecs_iter_next(&it)) {
        const WPE_Collider* collider = wpe_ecs_iter_get(&it, WPE_COMP_PHYSICS_COLLIDER);
        if (collider->wpe__state == WPE__STATE_CREATED)
            wpe__place_body(physics, it.entity, collider->wpe__static_body);
    }
    it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_CHARACTER)});
    while (wpe_ecs_iter_next(&it)) {
        const WPE_CharacterController* c = wpe_ecs_iter_get(&it, WPE_COMP_CHARACTER);
        wpe__Pose pose;
        if (c->wpe__state != WPE__STATE_CREATED || !wpe__character_pose(physics, it.entity, &pose)) continue;
        const b3BodyId body = b3LoadBodyId(c->wpe__body);
        if (wpe__pose_differs(&pose, b3Body_GetTransform(body)))
            b3Body_SetTransform(body, wpe__b3_vec(pose.position), wpe__b3_quat(pose.rotation));
    }
    return WPE_RESULT_OK;
}

const WPE_PhysicsEvent* wpe_physics_events(const WPE_Physics* physics, uint32_t* out_count) {
    if (out_count) *out_count = physics ? physics->event_count : 0;
    return physics ? physics->events : NULL;
}

void wpe_physics_set_gravity(WPE_Physics* physics, WPE_Vec3 gravity) {
    if (!wpe_check(physics != NULL && wpe__finite3(gravity))) return;
    b3World_SetGravity(physics->world, wpe__b3_vec(gravity));
}

WPE_Vec3 wpe_physics_gravity(const WPE_Physics* physics) {
    return physics ? wpe__vec(b3World_GetGravity(physics->world)) : wpe_vec3(0.0f, 0.0f, 0.0f);
}

/* ---- Queries -------------------------------------------------------------------------------------- */

/* Whether a query with `filter` (NULL: solid colliders) sees `shape`. Box3D's queries report triggers
 * too, so every query callback asks this. */
static bool wpe__query_accepts(const WPE_Physics* physics, const WPE_QueryFilter* filter, b3ShapeId shape) {
    if (b3Shape_IsSensor(shape) && !(filter && filter->include_triggers)) return false;
    return !filter || filter->ignore == WPE_ENTITY_NULL ||
           wpe__shape_entity(physics, shape) != filter->ignore;
}

typedef struct wpe__RayContext {
    const WPE_Physics* physics;
    const WPE_QueryFilter* filter;
    b3ShapeId shape;
    b3Vec3 point;
    b3Vec3 normal;
    float fraction;
} wpe__RayContext;

/* Keeps the closest accepted hit (returning the fraction clips the cast; -1 ignores the shape). Ray
 * and shape casts share it. */
static float wpe__ray_callback(b3ShapeId shape, b3Pos point, b3Vec3 normal, float fraction, uint64_t material,
                               int triangle, int child, void* context) {
    (void)material;
    (void)triangle;
    (void)child;
    wpe__RayContext* ray = context;
    if (!wpe__query_accepts(ray->physics, ray->filter, shape)) return -1.0f;
    ray->shape = shape;
    ray->point = point;
    ray->normal = normal;
    ray->fraction = fraction;
    return fraction;
}

/* The cast's translation, or false for unusable arguments. */
static bool wpe__cast_translation(WPE_Vec3 origin, WPE_Vec3 direction, float max_distance, b3Vec3* out) {
    if (!wpe__finite3(origin) || !wpe__finite3(direction) || !(max_distance > 0.0f) ||
        !isfinite(max_distance))
        return false;
    const float length = wpe_vec3_length(direction);
    if (!(length > 0.0f)) return false;
    *out = wpe__b3_vec(wpe_vec3_scale(direction, max_distance / length));
    return true;
}

static void wpe__write_hit(const WPE_Physics* physics, const wpe__RayContext* ray, float max_distance,
                           WPE_RaycastHit* out_hit) {
    if (!out_hit) return;
    *out_hit = (WPE_RaycastHit){
        .entity = wpe__shape_entity(physics, ray->shape),
        .point = wpe__vec(ray->point),
        .normal = wpe__vec(ray->normal),
        .distance = ray->fraction * max_distance,
    };
}

bool wpe_physics_raycast(const WPE_Physics* physics, WPE_Vec3 origin, WPE_Vec3 direction, float max_distance,
                         const WPE_QueryFilter* filter, WPE_RaycastHit* out_hit) {
    b3Vec3 translation;
    if (!physics || !wpe__cast_translation(origin, direction, max_distance, &translation)) return false;
    wpe__RayContext ray = {.physics = physics, .filter = filter, .shape = b3_nullShapeId, .fraction = 1.0f};
    b3World_CastRay(physics->world, wpe__b3_vec(origin), translation, b3DefaultQueryFilter(),
                    wpe__ray_callback, &ray);
    if (B3_IS_NULL(ray.shape)) return false;
    wpe__write_hit(physics, &ray, max_distance, out_hit);
    return true;
}

/* A query shape as Box3D's point cloud with a radius, relative to its center. False for shapes that
 * can't be queried. */
static bool wpe__query_proxy(const WPE_QueryShape* shape, b3Vec3 points[8], b3ShapeProxy* out) {
    if (!shape || !wpe__finite3(shape->size)) return false;
    const WPE_Quat q = wpe_quat_normalize(shape->rotation);
    if (!(isfinite(q.x) && isfinite(q.y) && isfinite(q.z) && isfinite(q.w))) return false;
    switch (shape->shape) {
    case WPE_COLLIDER_SPHERE:
        if (!(shape->size.x > 0.0f)) return false;
        points[0] = (b3Vec3){0.0f, 0.0f, 0.0f};
        *out = (b3ShapeProxy){.points = points, .count = 1, .radius = shape->size.x};
        return true;
    case WPE_COLLIDER_CAPSULE: {
        if (!(shape->size.x > 0.0f)) return false;
        const float half = shape->size.y * 0.5f - shape->size.x;
        const WPE_Vec3 axis = wpe_quat_rotate_vec3(q, wpe_vec3(0.0f, half > 0.0f ? half : 0.0f, 0.0f));
        points[0] = wpe__b3_vec(wpe_vec3_scale(axis, -1.0f));
        points[1] = wpe__b3_vec(axis);
        *out = (b3ShapeProxy){.points = points, .count = half > 0.0f ? 2 : 1, .radius = shape->size.x};
        return true;
    }
    case WPE_COLLIDER_BOX: {
        const WPE_Vec3 half = wpe_vec3_scale(shape->size, 0.5f);
        if (!(half.x > 0.0f && half.y > 0.0f && half.z > 0.0f)) return false;
        for (int i = 0; i < 8; ++i) {
            const WPE_Vec3 corner =
                wpe_vec3((i & 1) ? half.x : -half.x, (i & 2) ? half.y : -half.y, (i & 4) ? half.z : -half.z);
            points[i] = wpe__b3_vec(wpe_quat_rotate_vec3(q, corner));
        }
        *out = (b3ShapeProxy){.points = points, .count = 8, .radius = 0.0f};
        return true;
    }
    case WPE_COLLIDER_CONVEX:
    case WPE_COLLIDER_MESH:
        break;
    }
    return false;
}

bool wpe_physics_shape_cast(const WPE_Physics* physics, const WPE_QueryShape* shape, WPE_Vec3 origin,
                            WPE_Vec3 direction, float max_distance, const WPE_QueryFilter* filter,
                            WPE_RaycastHit* out_hit) {
    b3Vec3 points[8];
    b3ShapeProxy proxy;
    b3Vec3 translation;
    if (!physics || !wpe__query_proxy(shape, points, &proxy) ||
        !wpe__cast_translation(origin, direction, max_distance, &translation))
        return false;
    wpe__RayContext ray = {.physics = physics, .filter = filter, .shape = b3_nullShapeId, .fraction = 1.0f};
    b3World_CastShape(physics->world, wpe__b3_vec(origin), &proxy, translation, b3DefaultQueryFilter(),
                      wpe__ray_callback, &ray);
    if (B3_IS_NULL(ray.shape)) return false;
    wpe__write_hit(physics, &ray, max_distance, out_hit);
    return true;
}

typedef struct wpe__OverlapContext {
    const WPE_Physics* physics;
    const WPE_QueryFilter* filter;
    WPE_Entity* entities;
    uint32_t capacity;
    uint32_t count;
    /* Entities seen, to report each once even when it has several shapes (a character and a
     * collider); those past `capacity` are only counted. */
    WPE_Entity seen[64];
} wpe__OverlapContext;

static bool wpe__overlap_callback(b3ShapeId shape, void* context) {
    wpe__OverlapContext* overlap = context;
    if (!wpe__query_accepts(overlap->physics, overlap->filter, shape)) return true;
    const WPE_Entity entity = wpe__shape_entity(overlap->physics, shape);
    const uint32_t listed = overlap->count < overlap->capacity ? overlap->count : overlap->capacity;
    for (uint32_t i = 0; i < listed; ++i)
        if (overlap->entities[i] == entity) return true;
    const uint32_t seen = overlap->count < 64 ? overlap->count : 64;
    for (uint32_t i = 0; i < seen; ++i)
        if (overlap->seen[i] == entity) return true;
    if (overlap->count < 64) overlap->seen[overlap->count] = entity;
    if (overlap->count < overlap->capacity) overlap->entities[overlap->count] = entity;
    ++overlap->count;
    return true;
}

uint32_t wpe_physics_overlap(const WPE_Physics* physics, const WPE_QueryShape* shape, WPE_Vec3 center,
                             const WPE_QueryFilter* filter, WPE_Entity* out_entities, uint32_t capacity) {
    b3Vec3 points[8];
    b3ShapeProxy proxy;
    if (!physics || !wpe__finite3(center) || !wpe__query_proxy(shape, points, &proxy)) return 0;
    wpe__OverlapContext overlap = {.physics = physics,
                                   .filter = filter,
                                   .entities = out_entities,
                                   .capacity = out_entities ? capacity : 0};
    b3World_OverlapShape(physics->world, wpe__b3_vec(center), &proxy, b3DefaultQueryFilter(),
                         wpe__overlap_callback, &overlap);
    return overlap.count;
}

/* ---- Character controllers ------------------------------------------------------------------------ */

#define WPE__MAX_PLANES 64
#define WPE__MOVE_ITERATIONS 5
#define WPE__PUSH_DEPTH 0.05f /* How far a character may sink into a dynamic body it pushes. */

WPE_CharacterController wpe_character_default(void) {
    return (WPE_CharacterController){
        .radius = 0.4f,
        .height = 1.8f,
        .offset = wpe_vec3(0.0f, 0.9f, 0.0f),
        .max_slope = 45.0f,
        .step_height = 0.3f,
        .snap_distance = 0.3f,
        .ground_normal = wpe_vec3(0.0f, 1.0f, 0.0f),
    };
}

static bool wpe__character_valid(const WPE_CharacterController* c) {
    return isfinite(c->radius) && c->radius > 0.0f && isfinite(c->height) && c->height > 0.0f &&
           wpe__finite3(c->offset) && isfinite(c->max_slope) && c->max_slope >= 0.0f &&
           c->max_slope <= 90.0f && isfinite(c->step_height) && c->step_height >= 0.0f &&
           isfinite(c->snap_distance) && c->snap_distance >= 0.0f;
}

/* Half the length of the capsule's core segment (0: a sphere). */
static float wpe__character_half_segment(const WPE_CharacterController* c) {
    const float half = c->height * 0.5f - c->radius;
    return half > WPE__MIN_EXTENT ? half : 0.0f;
}

/* The capsule around `center`, relative to it (movers are relative to their origin). */
static b3Capsule wpe__character_mover(const WPE_CharacterController* c) {
    const float half = wpe__character_half_segment(c);
    return (b3Capsule){.center1 = {0.0f, -half, 0.0f}, .center2 = {0.0f, half, 0.0f}, .radius = c->radius};
}

static void wpe__ensure_character(WPE_Physics* physics, WPE_Entity entity, WPE_CharacterController* c) {
    if (c->wpe__state != WPE__STATE_PENDING) return;
    wpe__Pose pose;
    if (!wpe__character_valid(c) || !wpe__world_pose(physics->ecs, entity, &pose)) {
        wpe_log_error(
            "character controller of entity 0x%08x has an invalid size, slope or snap distance, or a "
            "degenerate transform; disabled until wpe_physics_refresh()",
            entity);
        c->wpe__state = WPE__STATE_FAILED;
        return;
    }
    const WPE_RigidBody kinematic = wpe_rigidbody_default(WPE_BODY_KINEMATIC);
    pose.rotation = wpe_quat_identity(); /* Always upright. */
    const b3BodyId body = wpe__create_b3_body(physics, entity, &kinematic, &pose);
    if (B3_IS_NULL(body)) {
        c->wpe__state = WPE__STATE_FAILED;
        return;
    }
    b3ShapeDef def = b3DefaultShapeDef();
    def.baseMaterial.friction = 0.0f;
    def.enableSensorEvents = true;
    def.enableContactEvents = true;
    const float half = wpe__character_half_segment(c);
    const b3Vec3 center = wpe__b3_vec(c->offset);
    b3ShapeId shape;
    if (half > 0.0f) {
        const b3Capsule capsule = {.center1 = {center.x, center.y - half, center.z},
                                   .center2 = {center.x, center.y + half, center.z},
                                   .radius = c->radius};
        shape = b3CreateCapsuleShape(body, &def, &capsule);
    } else {
        const b3Sphere sphere = {.center = center, .radius = c->radius};
        shape = b3CreateSphereShape(body, &def, &sphere);
    }
    if (B3_IS_NULL(shape)) {
        wpe_log_error("Box3D rejected the character capsule of entity 0x%08x", entity);
        wpe__destroy_b3_body(physics, b3StoreBodyId(body));
        c->wpe__state = WPE__STATE_FAILED;
        return;
    }
    c->wpe__body = b3StoreBodyId(body);
    c->wpe__state = WPE__STATE_CREATED;
}

static void wpe__release_character(WPE_Physics* physics, WPE_CharacterController* c) {
    if (c->wpe__state == WPE__STATE_CREATED) wpe__destroy_b3_body(physics, c->wpe__body);
    c->wpe__body = 0;
    c->wpe__target_move = false;
    c->wpe__state = WPE__STATE_PENDING;
}

/* Where the character's body should be: the entity's world position, upright. */
static bool wpe__character_pose(const WPE_Physics* physics, WPE_Entity entity, wpe__Pose* out) {
    if (!wpe__world_pose(physics->ecs, entity, out)) return false;
    out->rotation = wpe_quat_identity();
    return true;
}

typedef struct wpe__Planes {
    const WPE_Physics* physics;
    WPE_Entity self;
    float min_up; /* Normals with at least this much +Y are walkable ground. */
    b3CollisionPlane planes[WPE__MAX_PLANES];
    WPE_Entity entities[WPE__MAX_PLANES];
    bool wall[WPE__MAX_PLANES];   /* A vertical companion of a steep plane. */
    bool pushed[WPE__MAX_PLANES]; /* Against a dynamic body being pushed: doesn't stop the velocity. */
    bool dynamic[WPE__MAX_PLANES];
    WPE_Vec3 points[WPE__MAX_PLANES]; /* World-space contact points. */
    int count;
    WPE_Vec3 origin; /* The capsule center the planes were collected at. */
    /* Ground found by wpe__find_ground(). */
    WPE_Vec3 ground_normal;
    WPE_Vec3 ground_point;
    WPE_Entity ground_entity;
} wpe__Planes;

static void wpe__add_plane(wpe__Planes* collected, b3Plane plane, b3Vec3 point, WPE_Entity entity, bool wall,
                           bool pushed, bool dynamic) {
    if (collected->count == WPE__MAX_PLANES) return;
    collected->planes[collected->count] =
        (b3CollisionPlane){.plane = plane, .pushLimit = FLT_MAX, .push = 0.0f, .clipVelocity = true};
    collected->points[collected->count] = wpe_vec3_add(collected->origin, wpe__vec(point));
    collected->entities[collected->count] = entity;
    collected->pushed[collected->count] = pushed;
    collected->dynamic[collected->count] = dynamic;
    collected->wall[collected->count++] = wall;
}

/* Collects the planes a mover touches.
 * - Sliding along a slope too steep to walk would carry a character up it, so each steep plane gets
 *   a vertical wall beside it: the character can't move further into the slope sideways, while the
 *   slope itself still holds it up (and lets it slide down). The wall's offset is the solver's slop,
 *   so it holds still rather than pushing away.
 * - A dynamic body the character walks into lets it sink in up to WPE__PUSH_DEPTH, so the character's
 *   body overlaps it and the next step pushes it away; one that can't move still stops it there.
 *   Walkable dynamic surfaces (standing on a crate) are solid ground. */
static bool wpe__collect_plane(b3ShapeId shape, const b3PlaneResult* planes, int count, void* context) {
    wpe__Planes* collected = context;
    const WPE_QueryFilter filter = {.ignore = collected->self};
    if (!wpe__query_accepts(collected->physics, &filter, shape)) return true;
    const WPE_Entity entity = wpe__shape_entity(collected->physics, shape);
    const bool dynamic = b3Body_GetType(b3Shape_GetBody(shape)) == b3_dynamicBody;
    for (int i = 0; i < count; ++i) {
        b3Plane plane = planes[i].plane;
        const bool pushed = dynamic && plane.normal.y < collected->min_up;
        if (pushed) plane.offset -= WPE__PUSH_DEPTH;
        wpe__add_plane(collected, plane, planes[i].point, entity, false, pushed, dynamic);
        const float horizontal = sqrtf(plane.normal.x * plane.normal.x + plane.normal.z * plane.normal.z);
        if (!pushed && plane.normal.y > 1e-3f && plane.normal.y < collected->min_up && horizontal > 1e-3f) {
            const b3Plane wall = {.normal = {plane.normal.x / horizontal, 0.0f, plane.normal.z / horizontal},
                                  .offset = B3_LINEAR_SLOP};
            wpe__add_plane(collected, wall, planes[i].point, entity, true, false, dynamic);
        }
    }
    return collected->count < WPE__MAX_PLANES;
}

/* The sweep ignores dynamic bodies: the planes handle them, letting the character press into them. */
static bool wpe__mover_accepts(b3ShapeId shape, void* context) {
    const wpe__Planes* collected = context;
    const WPE_QueryFilter filter = {.ignore = collected->self};
    return wpe__query_accepts(collected->physics, &filter, shape) &&
           b3Body_GetType(b3Shape_GetBody(shape)) != b3_dynamicBody;
}

/* The planes the capsule centered at `center` touches. */
static void wpe__collide_character(wpe__Planes* planes, const b3Capsule* mover, WPE_Vec3 center) {
    planes->count = 0;
    planes->origin = center;
    b3World_CollideMover(planes->physics->world, wpe__b3_vec(center), mover, b3DefaultQueryFilter(),
                         wpe__collect_plane, planes);
}

/*
 * The ground under the capsule: the most upward-facing contact, when it's walkable (its normal within
 * max_slope of +Y). A contact that isn't but still holds the capsule up, like the edge of a step the
 * rounded bottom rests on, counts when the surface just inside it is walkable (a short ray finds it).
 * Returns that plane's index, filling ground_normal and ground_entity, or -1.
 */
static int wpe__find_ground(wpe__Planes* planes) {
    int best = -1;
    for (int i = 0; i < planes->count; ++i)
        if (best < 0 || planes->planes[i].plane.normal.y > planes->planes[best].plane.normal.y) best = i;
    if (best < 0) return -1;
    const WPE_Vec3 normal = wpe__vec(planes->planes[best].plane.normal);
    planes->ground_entity = planes->entities[best];
    planes->ground_normal = normal;
    planes->ground_point = planes->points[best];
    if (normal.y >= planes->min_up) return best;
    const WPE_Vec3 horizontal = wpe_vec3(normal.x, 0.0f, normal.z);
    if (normal.y < 0.05f || wpe_vec3_length(horizontal) < 1e-3f) return -1;
    /* From just above the contact point, a little into the shape, straight down. */
    const WPE_Vec3 origin = wpe_vec3_add(
        wpe_vec3_sub(planes->points[best], wpe_vec3_scale(wpe_vec3_normalize(horizontal), 0.02f)),
        wpe_vec3(0.0f, 0.05f, 0.0f));
    WPE_RaycastHit hit;
    if (!wpe_physics_raycast(planes->physics, origin, wpe_vec3(0.0f, -1.0f, 0.0f), 0.1f,
                             &(WPE_QueryFilter){.ignore = planes->self}, &hit) ||
        hit.normal.y < planes->min_up)
        return -1;
    planes->ground_normal = hit.normal;
    planes->ground_point = hit.point;
    planes->ground_entity = hit.entity;
    return best;
}

/* Sets the entity's world position, keeping its rotation and scale (through the parent's inverse). */
static void wpe__set_world_position(WPE_Physics* physics, WPE_Entity entity, WPE_Vec3 position) {
    const WPE_Entity parent = wpe_scene_get_parent(physics->ecs, entity);
    if (parent != WPE_ENTITY_NULL) {
        WPE_Mat4 parent_world;
        WPE_Mat4 inverse;
        if (!wpe_scene_compute_world_matrix(physics->ecs, parent, &parent_world) ||
            !wpe_mat4_inverse(&parent_world, &inverse))
            return;
        position = wpe_mat4_transform_point(&inverse, position);
    }
    WPE_Transform* transform = wpe_ecs_get_component(physics->ecs, entity, WPE_COMP_TRANSFORM);
    if (transform) transform->position = position;
}

/* Moves the capsule from `position` toward `target`: collide, solve the planes for the rest of the
 * way, then sweep there (stopping at anything the planes missed), a few times over. */
static WPE_Vec3 wpe__slide(wpe__Planes* planes, const b3Capsule* mover, WPE_Vec3 position, WPE_Vec3 target) {
    for (int iteration = 0; iteration < WPE__MOVE_ITERATIONS; ++iteration) {
        wpe__collide_character(planes, mover, position);
        const b3PlaneSolverResult solved =
            b3SolvePlanes(wpe__b3_vec(wpe_vec3_sub(target, position)), planes->planes, planes->count);
        const float fraction =
            b3World_CastMover(planes->physics->world, wpe__b3_vec(position), mover, solved.delta,
                              b3DefaultQueryFilter(), wpe__mover_accepts, planes);
        const WPE_Vec3 step = wpe_vec3_scale(wpe__vec(solved.delta), fraction);
        position = wpe_vec3_add(position, step);
        if (wpe_vec3_length_sq(step) < 1e-8f) break;
    }
    return position;
}

/* Horizontal distance covered along the horizontal direction of `delta`. */
static float wpe__progress(WPE_Vec3 from, WPE_Vec3 to, WPE_Vec3 delta) {
    const WPE_Vec3 direction = wpe_vec3_normalize(wpe_vec3(delta.x, 0.0f, delta.z));
    return wpe_vec3_dot(wpe_vec3_sub(to, from), direction);
}

/* Steps: when a grounded character is held back, tries the move again lifted by up to `height`, then
 * sets it down. The step is taken when it gets further and lands on walkable, static or kinematic
 * ground (a crate is pushed, not climbed) no higher than `height` above its feet. A step the capsule
 * only reaches the edge of in one frame is climbed over a few: it rests on the edge in between
 * (wpe__find_ground counts that as ground). Returns the position to keep. */
static WPE_Vec3 wpe__try_step(wpe__Planes* planes, const b3Capsule* mover, float height, WPE_Vec3 start,
                              WPE_Vec3 delta, WPE_Vec3 slid) {
    const float wanted = sqrtf(delta.x * delta.x + delta.z * delta.z);
    if (wanted < 1e-5f) return slid;
    const float achieved = wpe__progress(start, slid, delta);
    if (achieved > wanted * 0.99f) return slid; /* Not held back. */
    const b3WorldId world = planes->physics->world;
    const float up =
        height * b3World_CastMover(world, wpe__b3_vec(start), mover, (b3Vec3){0.0f, height, 0.0f},
                                   b3DefaultQueryFilter(), wpe__mover_accepts, planes);
    const WPE_Vec3 raised = wpe_vec3_add(start, wpe_vec3(0.0f, up, 0.0f));
    const WPE_Vec3 moved =
        wpe__slide(planes, mover, raised, wpe_vec3_add(raised, wpe_vec3(delta.x, 0.0f, delta.z)));
    if (wpe__progress(start, moved, delta) <= achieved + 1e-3f) return slid;
    const float down = up * b3World_CastMover(world, wpe__b3_vec(moved), mover, (b3Vec3){0.0f, -up, 0.0f},
                                              b3DefaultQueryFilter(), wpe__mover_accepts, planes);
    const WPE_Vec3 landed = wpe_vec3_add(moved, wpe_vec3(0.0f, -down - B3_LINEAR_SLOP, 0.0f));
    wpe__collide_character(planes, mover, landed);
    const int ground = wpe__find_ground(planes);
    const float feet = start.y + mover->center1.y - mover->radius;
    const bool low_enough = planes->ground_point.y - feet <= height + B3_LINEAR_SLOP;
    return ground >= 0 && !planes->dynamic[ground] && low_enough ? landed : slid;
}

WPE_Result wpe_character_move(WPE_Physics* physics, WPE_Entity entity, WPE_Vec3 velocity, float dt,
                              WPE_Vec3* out_velocity) {
    if (!physics) return WPE_RESULT_ERROR_NULL_ARG;
    WPE_CharacterController* c = wpe_ecs_get_component(physics->ecs, entity, WPE_COMP_CHARACTER);
    if (!c || !wpe_ecs_has_component(physics->ecs, entity, WPE_COMP_TRANSFORM))
        return WPE_RESULT_ERROR_NOT_FOUND;
    if (!wpe__finite3(velocity) || !(dt > 0.0f) || !isfinite(dt) || !wpe__character_valid(c)) {
        wpe_log_error("wpe_character_move: invalid velocity, dt or controller settings (entity 0x%08x)",
                      entity);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    wpe__Pose pose;
    if (!wpe__world_pose(physics->ecs, entity, &pose)) return WPE_RESULT_ERROR_INVALID_ARG;

    const b3Capsule mover = wpe__character_mover(c);
    const float min_up = cosf(c->max_slope * (WPE_PI / 180.0f)) - 1e-4f;
    const WPE_Vec3 start = wpe_vec3_add(pose.position, c->offset);
    WPE_Vec3 delta = wpe_vec3_scale(velocity, dt);
    /* Standing on walkable ground, a downward velocity (gravity) mustn't slide it downhill: the ground
     * holds it, and snapping follows the ground down instead. */
    const bool held = c->grounded && velocity.y <= 0.0f;
    if (held) delta.y = 0.0f;

    wpe__Planes* planes = wpe_alloc(sizeof *planes);
    if (!planes) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    planes->physics = physics;
    planes->self = entity;
    planes->min_up = min_up;
    WPE_Vec3 position = wpe__slide(planes, &mover, start, wpe_vec3_add(start, delta));
    if (held && c->step_height > 0.0f)
        position = wpe__try_step(planes, &mover, c->step_height, start, delta, position);

    /* What it touches now, and the ground among it. */
    wpe__collide_character(planes, &mover, position);
    int ground = wpe__find_ground(planes);
    if (ground < 0 && held && c->snap_distance > 0.0f) {
        /* Walked off a slope or a step: follow the ground down if it's near. */
        const b3Vec3 drop = {0.0f, -c->snap_distance, 0.0f};
        const float fraction = b3World_CastMover(physics->world, wpe__b3_vec(position), &mover, drop,
                                                 b3DefaultQueryFilter(), wpe__mover_accepts, planes);
        if (fraction < 1.0f) {
            /* Down to touching (a slop inside, where the solver keeps resting characters). */
            const WPE_Vec3 below =
                wpe_vec3_add(position, wpe_vec3(0.0f, -c->snap_distance * fraction - B3_LINEAR_SLOP, 0.0f));
            wpe__collide_character(planes, &mover, below);
            ground = wpe__find_ground(planes);
            if (ground >= 0) {
                position = below;
            } else {
                wpe__collide_character(planes, &mover, position);
            }
        }
    }

    /* Velocity: the parts into what it touches go; walkable ground (and an edge it stands on) stops the
     * fall without sliding. Walls first: clipping against a steep slope alone would turn a run into it
     * into a climb. */
    WPE_Vec3 v = velocity;
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < planes->count; ++i) {
            if (planes->wall[i] != (pass == 0) || planes->pushed[i]) continue;
            if (planes->wall[i] && ground >= 0 && i == ground + 1) continue; /* The edge it stands on. */
            const WPE_Vec3 n = i == ground || planes->planes[i].plane.normal.y >= min_up
                                   ? wpe_vec3(0.0f, 1.0f, 0.0f)
                                   : wpe__vec(planes->planes[i].plane.normal);
            const float into = wpe_vec3_dot(v, n);
            if (into < 0.0f) v = wpe_vec3_sub(v, wpe_vec3_scale(n, into));
        }
    }

    c->grounded = ground >= 0;
    c->ground_normal = ground >= 0 ? planes->ground_normal : wpe_vec3(0.0f, 1.0f, 0.0f);
    c->ground = ground >= 0 ? planes->ground_entity : WPE_ENTITY_NULL;
    wpe_free(planes);
    wpe__set_world_position(physics, entity, wpe_vec3_add(pose.position, wpe_vec3_sub(position, start)));
    if (out_velocity) *out_velocity = v;
    return WPE_RESULT_OK;
}

/* Body of an entity's created rigid body. */
static WPE_Result wpe__body_of(const WPE_Physics* physics, WPE_Entity entity, b3BodyId* out_body,
                               WPE_BodyType* out_type) {
    if (!physics) return WPE_RESULT_ERROR_NULL_ARG;
    const WPE_RigidBody* rb = wpe_ecs_get_component(physics->ecs, entity, WPE_COMP_PHYSICS_RIGIDBODY);
    if (!rb || rb->wpe__state != WPE__STATE_CREATED) return WPE_RESULT_ERROR_NOT_FOUND;
    *out_body = b3LoadBodyId(rb->wpe__body);
    *out_type = rb->type;
    return WPE_RESULT_OK;
}

WPE_Result wpe_physics_apply_force(WPE_Physics* physics, WPE_Entity entity, WPE_Vec3 force) {
    b3BodyId body;
    WPE_BodyType type;
    const WPE_Result result = wpe__body_of(physics, entity, &body, &type);
    if (result != WPE_RESULT_OK) return result;
    if (!wpe__finite3(force)) return WPE_RESULT_ERROR_INVALID_ARG;
    if (type != WPE_BODY_DYNAMIC) return WPE_RESULT_ERROR_INVALID_STATE;
    b3Body_ApplyForceToCenter(body, wpe__b3_vec(force), true);
    return WPE_RESULT_OK;
}

WPE_Result wpe_physics_apply_impulse(WPE_Physics* physics, WPE_Entity entity, WPE_Vec3 impulse) {
    b3BodyId body;
    WPE_BodyType type;
    const WPE_Result result = wpe__body_of(physics, entity, &body, &type);
    if (result != WPE_RESULT_OK) return result;
    if (!wpe__finite3(impulse)) return WPE_RESULT_ERROR_INVALID_ARG;
    if (type != WPE_BODY_DYNAMIC) return WPE_RESULT_ERROR_INVALID_STATE;
    b3Body_ApplyLinearImpulseToCenter(body, wpe__b3_vec(impulse), true);
    return WPE_RESULT_OK;
}

WPE_Result wpe_physics_set_velocity(WPE_Physics* physics, WPE_Entity entity, WPE_Vec3 velocity) {
    b3BodyId body;
    WPE_BodyType type;
    const WPE_Result result = wpe__body_of(physics, entity, &body, &type);
    if (result != WPE_RESULT_OK) return result;
    if (!wpe__finite3(velocity)) return WPE_RESULT_ERROR_INVALID_ARG;
    if (type == WPE_BODY_STATIC) return WPE_RESULT_ERROR_INVALID_STATE;
    b3Body_SetLinearVelocity(body, wpe__b3_vec(velocity));
    if (type == WPE_BODY_DYNAMIC) b3Body_SetAwake(body, true);
    return WPE_RESULT_OK;
}

WPE_Result wpe_physics_get_velocity(const WPE_Physics* physics, WPE_Entity entity, WPE_Vec3* out_velocity) {
    if (!out_velocity) return WPE_RESULT_ERROR_NULL_ARG;
    b3BodyId body;
    WPE_BodyType type;
    const WPE_Result result = wpe__body_of(physics, entity, &body, &type);
    if (result != WPE_RESULT_OK) return result;
    *out_velocity = wpe__vec(b3Body_GetLinearVelocity(body));
    return WPE_RESULT_OK;
}

/* ---- Debug drawing -------------------------------------------------------------------------------- */

typedef struct wpe__Draw {
    WPE_PhysicsLineFn line;
    void* user_data;
    WPE_Vec3 position;
    WPE_Quat rotation;
    uint32_t color;
} wpe__Draw;

/* Draws a body-space segment. */
static void wpe__draw_line(const wpe__Draw* draw, WPE_Vec3 a, WPE_Vec3 b) {
    draw->line(wpe_vec3_add(draw->position, wpe_quat_rotate_vec3(draw->rotation, a)),
               wpe_vec3_add(draw->position, wpe_quat_rotate_vec3(draw->rotation, b)), draw->color,
               draw->user_data);
}

/* Draws the arc center + radius * (cos(t) * u + sin(t) * v) for t in [start, end] (radians). */
static void wpe__draw_arc(const wpe__Draw* draw, WPE_Vec3 center, WPE_Vec3 u, WPE_Vec3 v, float radius,
                          float start, float end) {
    const int segments = (int)ceilf(WPE__CIRCLE_SEGMENTS * (end - start) / (2.0f * WPE_PI));
    WPE_Vec3 previous = wpe_vec3_add(center, wpe_vec3_add(wpe_vec3_scale(u, radius * cosf(start)),
                                                          wpe_vec3_scale(v, radius * sinf(start))));
    for (int i = 1; i <= segments; ++i) {
        const float t = start + (end - start) * (float)i / (float)segments;
        const WPE_Vec3 point = wpe_vec3_add(
            center, wpe_vec3_add(wpe_vec3_scale(u, radius * cosf(t)), wpe_vec3_scale(v, radius * sinf(t))));
        wpe__draw_line(draw, previous, point);
        previous = point;
    }
}

static void wpe__draw_shape(const wpe__Draw* draw, b3ShapeId shape) {
    const WPE_Vec3 x = wpe_vec3(1.0f, 0.0f, 0.0f);
    const WPE_Vec3 y = wpe_vec3(0.0f, 1.0f, 0.0f);
    const WPE_Vec3 z = wpe_vec3(0.0f, 0.0f, 1.0f);
    switch (b3Shape_GetType(shape)) {
    case b3_sphereShape: {
        const b3Sphere sphere = b3Shape_GetSphere(shape);
        const WPE_Vec3 c = wpe__vec(sphere.center);
        wpe__draw_arc(draw, c, x, y, sphere.radius, 0.0f, 2.0f * WPE_PI);
        wpe__draw_arc(draw, c, y, z, sphere.radius, 0.0f, 2.0f * WPE_PI);
        wpe__draw_arc(draw, c, z, x, sphere.radius, 0.0f, 2.0f * WPE_PI);
        break;
    }
    case b3_capsuleShape: {
        const b3Capsule capsule = b3Shape_GetCapsule(shape);
        const WPE_Vec3 c1 = wpe__vec(capsule.center1);
        const WPE_Vec3 c2 = wpe__vec(capsule.center2);
        const WPE_Vec3 axis = wpe_vec3_normalize(wpe_vec3_sub(c2, c1));
        const WPE_Vec3 u = wpe_vec3_normalize(wpe_vec3_cross(axis, fabsf(axis.x) < 0.9f ? x : y));
        const WPE_Vec3 v = wpe_vec3_cross(axis, u);
        const float r = capsule.radius;
        wpe__draw_arc(draw, c1, u, v, r, 0.0f, 2.0f * WPE_PI);
        wpe__draw_arc(draw, c2, u, v, r, 0.0f, 2.0f * WPE_PI);
        wpe__draw_arc(draw, c2, u, axis, r, 0.0f, WPE_PI);
        wpe__draw_arc(draw, c2, v, axis, r, 0.0f, WPE_PI);
        wpe__draw_arc(draw, c1, u, wpe_vec3_scale(axis, -1.0f), r, 0.0f, WPE_PI);
        wpe__draw_arc(draw, c1, v, wpe_vec3_scale(axis, -1.0f), r, 0.0f, WPE_PI);
        const WPE_Vec3 sides[4] = {wpe_vec3_scale(u, r), wpe_vec3_scale(u, -r), wpe_vec3_scale(v, r),
                                   wpe_vec3_scale(v, -r)};
        for (int i = 0; i < 4; ++i)
            wpe__draw_line(draw, wpe_vec3_add(c1, sides[i]), wpe_vec3_add(c2, sides[i]));
        break;
    }
    case b3_hullShape: {
        const b3HullData* hull = b3Shape_GetHull(shape);
        const b3Vec3* points = b3GetHullPoints(hull);
        const b3HullHalfEdge* edges = b3GetHullEdges(hull);
        for (int i = 0; i < hull->edgeCount; ++i) {
            if (edges[i].twin < i) continue; /* Each edge once. */
            wpe__draw_line(draw, wpe__vec(points[edges[i].origin]),
                           wpe__vec(points[edges[edges[i].twin].origin]));
        }
        break;
    }
    case b3_meshShape: {
        const b3Mesh mesh = b3Shape_GetMesh(shape);
        const b3Vec3* vertices = b3GetMeshVertices(mesh.data);
        const b3MeshTriangle* triangles = b3GetMeshTriangles(mesh.data);
        const WPE_Vec3 scale = wpe__vec(mesh.scale);
        for (int i = 0; i < mesh.data->triangleCount; ++i) {
            const WPE_Vec3 a = wpe_vec3_mul(wpe__vec(vertices[triangles[i].index1]), scale);
            const WPE_Vec3 b = wpe_vec3_mul(wpe__vec(vertices[triangles[i].index2]), scale);
            const WPE_Vec3 c = wpe_vec3_mul(wpe__vec(vertices[triangles[i].index3]), scale);
            wpe__draw_line(draw, a, b);
            wpe__draw_line(draw, b, c);
            wpe__draw_line(draw, c, a);
        }
        break;
    }
    default:
        break;
    }
}

void wpe_physics_debug_draw(const WPE_Physics* physics, WPE_PhysicsLineFn line, void* user_data) {
    if (!physics || !line) return;
    WPE_EcsPoolView view;
    if (wpe_ecs_pool_view(physics->ecs, WPE_COMP_PHYSICS_COLLIDER, &view) != WPE_RESULT_OK) return;
    for (uint32_t i = 0; i < view.count; ++i) {
        const WPE_Collider* collider =
            (const WPE_Collider*)((const uint8_t*)view.data + (size_t)i * view.stride);
        if (collider->wpe__state != WPE__STATE_CREATED) continue;
        const b3ShapeId shape = b3LoadShapeId(collider->wpe__shape);
        const b3BodyId body = b3Shape_GetBody(shape);
        const b3WorldTransform xf = b3Body_GetTransform(body);
        uint32_t color = 0x7FBF7Fu; /* static: green */
        if (collider->is_trigger) {
            color = 0xFFD24Au;
        } else if (b3Body_GetType(body) == b3_kinematicBody) {
            color = 0x7F9FFFu;
        } else if (b3Body_GetType(body) == b3_dynamicBody) {
            color = b3Body_IsAwake(body) ? 0xFF8C66u : 0x999999u;
        }
        const wpe__Draw draw = {
            .line = line,
            .user_data = user_data,
            .position = wpe__vec(xf.p),
            .rotation = wpe__quat(xf.q),
            .color = color,
        };
        wpe__draw_shape(&draw, shape);
    }
    /* Character capsules: cyan. */
    if (wpe_ecs_pool_view(physics->ecs, WPE_COMP_CHARACTER, &view) != WPE_RESULT_OK) return;
    for (uint32_t i = 0; i < view.count; ++i) {
        const WPE_CharacterController* c =
            (const WPE_CharacterController*)((const uint8_t*)view.data + (size_t)i * view.stride);
        if (c->wpe__state != WPE__STATE_CREATED) continue;
        const b3BodyId body = b3LoadBodyId(c->wpe__body);
        b3ShapeId shape;
        if (b3Body_GetShapes(body, &shape, 1) != 1) continue;
        const b3WorldTransform xf = b3Body_GetTransform(body);
        const wpe__Draw draw = {
            .line = line,
            .user_data = user_data,
            .position = wpe__vec(xf.p),
            .rotation = wpe__quat(xf.q),
            .color = 0x66E0FFu,
        };
        wpe__draw_shape(&draw, shape);
    }
}

WPE_PhysicsStats wpe_physics_stats(const WPE_Physics* physics) {
    if (!physics) return (WPE_PhysicsStats){0};
    return (WPE_PhysicsStats){
        .bodies = physics->body_count,
        .awake_bodies = (uint32_t)b3World_GetAwakeBodyCount(physics->world),
        .events = physics->event_count,
        .memory = (size_t)b3GetByteCount(),
    };
}
