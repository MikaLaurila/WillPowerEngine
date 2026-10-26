#define WPE_LOG_MODULE "physics"
#include "physics/physics.h"

#include <box3d/box3d.h>
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
static b3Vec3* wpe__mesh_points(const WPE_Collider* collider, WPE_Vec3 scale, uint32_t* out_count) {
    const WPE_Model* model = collider->model;
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
static b3MeshData* wpe__build_mesh(const WPE_Collider* collider) {
    const WPE_Model* model = collider->model;
    const WPE_Mesh* mesh = &model->meshes[collider->mesh];
    uint32_t point_count = 0;
    b3Vec3* points = wpe__mesh_points(collider, wpe_vec3(1.0f, 1.0f, 1.0f), &point_count);
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

static WPE_Result wpe__validate_collider(const WPE_Collider* collider, b3BodyType body_type) {
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
        if (!collider->model || collider->mesh >= collider->model->mesh_count) {
            wpe_log_error("convex/mesh collider needs a model and a valid mesh index");
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
static WPE_Result wpe__create_shape(WPE_Entity entity, WPE_Collider* collider, b3BodyId body,
                                    WPE_Vec3 scale) {
    const WPE_Result valid = wpe__validate_collider(collider, b3Body_GetType(body));
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
        b3Vec3* points = wpe__mesh_points(collider, scale, &count);
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
        b3MeshData* mesh = wpe__build_mesh(collider);
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
    return WPE_RESULT_OK;
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
                !isfinite(rb->gravity_scale)) {
                wpe_log_error("rigid body of entity 0x%08x has invalid damping or gravity scale", entity);
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
        collider->wpe__state =
            wpe__create_shape(entity, collider, b3LoadBodyId(rb->wpe__body), pose.scale) == WPE_RESULT_OK
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
    if (wpe__create_shape(entity, collider, body, pose.scale) != WPE_RESULT_OK) {
        wpe__destroy_b3_body(physics, b3StoreBodyId(body));
        collider->wpe__state = WPE__STATE_FAILED;
        return;
    }
    collider->wpe__static_body = b3StoreBodyId(body);
    collider->wpe__state = WPE__STATE_CREATED;
}

/* Follows game-code moves of an entity (see file comment of physics.h, step 2). */
static void wpe__sync_body(WPE_Physics* physics, WPE_Entity entity, uint64_t stored, WPE_RigidBody* rb,
                           float dt) {
    if (!wpe_ecs_has_component(physics->ecs, entity, WPE_COMP_TRANSFORM)) return;
    wpe__Pose pose;
    if (!wpe__world_pose(physics->ecs, entity, &pose)) return;
    const b3BodyId body = b3LoadBodyId(stored);
    const bool moved = wpe__pose_differs(&pose, b3Body_GetTransform(body));

    if (rb && rb->type == WPE_BODY_KINEMATIC) {
        if (moved) {
            const b3WorldTransform target = {.p = wpe__b3_vec(pose.position),
                                             .q = wpe__b3_quat(pose.rotation)};
            b3Body_SetTargetTransform(body, target, dt, true);
            rb->wpe__target_move = true;
        } else if (rb->wpe__target_move) {
            /* Arrived: stop instead of coasting with the velocity of the last move. */
            b3Body_SetLinearVelocity(body, (b3Vec3){0.0f, 0.0f, 0.0f});
            b3Body_SetAngularVelocity(body, (b3Vec3){0.0f, 0.0f, 0.0f});
            rb->wpe__target_move = false;
        }
        return;
    }
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

static void wpe__collider_on_remove(WPE_Ecs* ecs, WPE_Entity entity, void* component, void* user_data) {
    (void)ecs;
    (void)entity;
    wpe__release_collider(user_data, component, false);
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
                                                                 .on_remove = wpe__collider_on_remove,
                                                                 .hook_user_data = physics});
        if (result != WPE_RESULT_OK) wpe_ecs_unregister_component(physics->ecs, WPE_COMP_PHYSICS_RIGIDBODY);
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
    wpe__remove_all(physics->ecs, WPE_COMP_PHYSICS_COLLIDER);
    wpe__remove_all(physics->ecs, WPE_COMP_PHYSICS_RIGIDBODY);
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

WPE_Result wpe_physics_refresh(WPE_Physics* physics, WPE_Entity entity) {
    if (!physics) return WPE_RESULT_ERROR_NULL_ARG;
    if (!wpe_ecs_is_alive(physics->ecs, entity)) return WPE_RESULT_ERROR_NOT_FOUND;
    WPE_Collider* collider = wpe_ecs_get_component(physics->ecs, entity, WPE_COMP_PHYSICS_COLLIDER);
    WPE_RigidBody* rb = wpe_ecs_get_component(physics->ecs, entity, WPE_COMP_PHYSICS_RIGIDBODY);
    if (!collider && !rb) return WPE_RESULT_ERROR_NOT_FOUND;
    if (collider) wpe__release_collider(physics, collider, false);
    if (rb) wpe__release_rigidbody(physics, entity, rb);
    wpe__ensure_created(physics, entity);
    if ((rb && rb->wpe__state != WPE__STATE_CREATED) ||
        (collider && collider->wpe__state != WPE__STATE_CREATED))
        return WPE_RESULT_ERROR_INVALID_ARG;
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

typedef struct wpe__RayContext {
    b3ShapeId shape;
    b3Vec3 point;
    b3Vec3 normal;
    float fraction;
} wpe__RayContext;

/* Keeps the closest non-trigger hit (returning the fraction clips the ray; -1 ignores the shape). */
static float wpe__ray_callback(b3ShapeId shape, b3Pos point, b3Vec3 normal, float fraction, uint64_t material,
                               int triangle, int child, void* context) {
    (void)material;
    (void)triangle;
    (void)child;
    if (b3Shape_IsSensor(shape)) return -1.0f;
    wpe__RayContext* ray = context;
    ray->shape = shape;
    ray->point = point;
    ray->normal = normal;
    ray->fraction = fraction;
    return fraction;
}

bool wpe_physics_raycast(const WPE_Physics* physics, WPE_Vec3 origin, WPE_Vec3 direction, float max_distance,
                         WPE_RaycastHit* out_hit) {
    if (!physics || !wpe__finite3(origin) || !wpe__finite3(direction) || !(max_distance > 0.0f) ||
        !isfinite(max_distance))
        return false;
    const float length = wpe_vec3_length(direction);
    if (!(length > 0.0f)) return false;
    const WPE_Vec3 translation = wpe_vec3_scale(direction, max_distance / length);
    wpe__RayContext ray = {.shape = b3_nullShapeId, .fraction = 1.0f};
    b3World_CastRay(physics->world, wpe__b3_vec(origin), wpe__b3_vec(translation), b3DefaultQueryFilter(),
                    wpe__ray_callback, &ray);
    if (B3_IS_NULL(ray.shape)) return false;
    if (out_hit) {
        *out_hit = (WPE_RaycastHit){
            .entity = wpe__shape_entity(physics, ray.shape),
            .point = wpe__vec(ray.point),
            .normal = wpe__vec(ray.normal),
            .distance = ray.fraction * max_distance,
        };
    }
    return true;
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
