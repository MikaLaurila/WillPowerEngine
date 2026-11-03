#define WPE_LOG_MODULE "navigation"
#include <math.h>
#include <string.h>

#include "assets/model.h"
#include "core/alloc.h"
#include "core/log.h"
#include "navigation/nav_internal.h"
#include "navigation/navigation.h"
#include "physics/physics.h"
#include "scene/scene.h"

#define WPE__MIN_EXTENT 0.001f /* As physics.c: the smallest half extent or radius a shape gets. */
#define WPE__SPHERE_RINGS 8u   /* Latitude bands of a tessellated sphere (and of each capsule cap). */
#define WPE__SPHERE_SEGMENTS 16u

WPE_Result wpe_nav_geometry_create(WPE_NavGeometry** out_geometry) {
    if (!out_geometry) return WPE_RESULT_ERROR_NULL_ARG;
    *out_geometry = wpe_alloc_zeroed(1, sizeof **out_geometry);
    return *out_geometry ? WPE_RESULT_OK : WPE_RESULT_ERROR_OUT_OF_MEMORY;
}

void wpe_nav_geometry_destroy(WPE_NavGeometry* geometry) {
    if (!geometry) return;
    wpe_free(geometry->vertices);
    wpe_free(geometry->triangles);
    wpe_free(geometry);
}

uint32_t wpe_nav_geometry_triangle_count(const WPE_NavGeometry* geometry) {
    return geometry ? geometry->triangle_count : 0;
}

bool wpe_nav_geometry_bounds(const WPE_NavGeometry* geometry, WPE_Vec3* out_min, WPE_Vec3* out_max) {
    if (!geometry || !out_min || !out_max || geometry->triangle_count == 0) return false;
    *out_min = geometry->min;
    *out_max = geometry->max;
    return true;
}

static bool wpe__reserve(void** array, uint32_t* capacity, uint32_t needed, size_t element_size) {
    if (needed <= *capacity) return true;
    uint32_t grown = *capacity ? *capacity : 256u;
    while (grown < needed) {
        if (grown > UINT32_MAX / 2u) return false;
        grown *= 2u;
    }
    void* bigger = wpe_realloc(*array, (size_t)grown * element_size);
    if (!bigger) return false;
    *array = bigger;
    *capacity = grown;
    return true;
}

static bool wpe__finite(WPE_Vec3 v) {
    return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

static float wpe__determinant3(const WPE_Mat4* m) {
    const WPE_Vec3 x = wpe_vec3(m->m[0][0], m->m[0][1], m->m[0][2]);
    const WPE_Vec3 y = wpe_vec3(m->m[1][0], m->m[1][1], m->m[1][2]);
    const WPE_Vec3 z = wpe_vec3(m->m[2][0], m->m[2][1], m->m[2][2]);
    return wpe_vec3_dot(x, wpe_vec3_cross(y, z));
}

WPE_Result wpe_nav_geometry_add_triangles(WPE_NavGeometry* geometry, const WPE_Vec3* vertices,
                                          uint32_t vertex_count, const uint32_t* indices,
                                          uint32_t index_count, const WPE_Mat4* transform) {
    if (!geometry || (vertex_count && !vertices) || (index_count && !indices))
        return WPE_RESULT_ERROR_NULL_ARG;
    const uint32_t triangles = index_count / 3u;
    for (uint32_t i = 0; i < triangles * 3u; ++i) {
        if (indices[i] >= vertex_count) {
            wpe_log_error("triangle index %u is out of range (%u vertices)", indices[i], vertex_count);
            return WPE_RESULT_ERROR_INVALID_ARG;
        }
    }
    for (uint32_t v = 0; v < vertex_count; ++v) {
        if (!wpe__finite(vertices[v])) {
            wpe_log_error("vertex %u is not finite", v);
            return WPE_RESULT_ERROR_INVALID_ARG;
        }
    }
    if (geometry->vertex_count > UINT32_MAX - vertex_count ||
        geometry->triangle_count > UINT32_MAX - triangles ||
        !wpe__reserve((void**)&geometry->vertices, &geometry->vertex_capacity,
                      geometry->vertex_count + vertex_count, sizeof *geometry->vertices) ||
        !wpe__reserve((void**)&geometry->triangles, &geometry->triangle_capacity,
                      geometry->triangle_count + triangles, sizeof *geometry->triangles)) {
        wpe_log_error("out of memory adding %u triangles to the navigation geometry", triangles);
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }
    /* A mirroring transform turns counter-clockwise triangles clockwise: swap two corners back. */
    const bool mirrored = transform && wpe__determinant3(transform) < 0.0f;
    const uint32_t base = geometry->vertex_count;
    for (uint32_t v = 0; v < vertex_count; ++v) {
        const WPE_Vec3 p = transform ? wpe_mat4_transform_point(transform, vertices[v]) : vertices[v];
        if (!wpe__finite(p)) {
            wpe_log_error("vertex %u is not finite after the transform", v);
            return WPE_RESULT_ERROR_INVALID_ARG; /* Nothing was counted yet. */
        }
        geometry->vertices[base + v] = p;
    }
    for (size_t t = 0; t < triangles; ++t) {
        uint32_t a = base + indices[t * 3u], b = base + indices[t * 3u + 1u], c = base + indices[t * 3u + 2u];
        if (mirrored) {
            const uint32_t swap = b;
            b = c;
            c = swap;
        }
        const WPE_Vec3 pa = geometry->vertices[a], pb = geometry->vertices[b], pc = geometry->vertices[c];
        const WPE_Vec3 normal = wpe_vec3_cross(wpe_vec3_sub(pb, pa), wpe_vec3_sub(pc, pa));
        if (!(wpe_vec3_length_sq(normal) > 1e-14f)) continue; /* Degenerate: no area. */
        geometry->triangles[geometry->triangle_count++] = (wpe__NavTriangle){.v = {a, b, c}};
        const WPE_Vec3 corners[3] = {pa, pb, pc};
        for (int k = 0; k < 3; ++k) {
            if (!geometry->has_bounds) {
                geometry->min = corners[k];
                geometry->max = corners[k];
                geometry->has_bounds = true;
            }
            for (int axis = 0; axis < 3; ++axis) {
                geometry->min.v[axis] = fminf(geometry->min.v[axis], corners[k].v[axis]);
                geometry->max.v[axis] = fmaxf(geometry->max.v[axis], corners[k].v[axis]);
            }
        }
    }
    geometry->vertex_count += vertex_count;
    return WPE_RESULT_OK;
}

WPE_Result wpe_nav_geometry_add_box(WPE_NavGeometry* geometry, WPE_Vec3 center, WPE_Vec3 half_extents,
                                    const WPE_Mat4* transform) {
    if (!geometry) return WPE_RESULT_ERROR_NULL_ARG;
    if (!wpe__finite(center) || !wpe__finite(half_extents)) {
        wpe_log_error("a navigation box needs a finite center and size");
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    const WPE_Vec3 h = wpe_vec3(fabsf(half_extents.x), fabsf(half_extents.y), fabsf(half_extents.z));
    WPE_Vec3 corners[8];
    for (uint32_t i = 0; i < 8; ++i) {
        corners[i] = wpe_vec3(center.x + ((i & 1u) ? h.x : -h.x), center.y + ((i & 2u) ? h.y : -h.y),
                              center.z + ((i & 4u) ? h.z : -h.z));
    }
    /* Corner i has +X when bit 0 is set, +Y bit 1, +Z bit 2. Faces wind counter-clockwise seen from
     * outside. */
    static const uint32_t indices[36] = {
        2, 6, 7, 2, 7, 3, /* +Y (top) */
        0, 1, 5, 0, 5, 4, /* -Y */
        1, 3, 7, 1, 7, 5, /* +X */
        0, 4, 6, 0, 6, 2, /* -X */
        4, 5, 7, 4, 7, 6, /* +Z */
        0, 2, 3, 0, 3, 1, /* -Z */
    };
    return wpe_nav_geometry_add_triangles(geometry, corners, 8, indices, 36, transform);
}

/* A sphere-like solid around `center`: two caps of `radius` joined by a cylinder of `half_segment`
 * (0: a sphere), axis along Y, transformed by `transform`. */
static WPE_Result wpe__add_capsule(WPE_NavGeometry* geometry, WPE_Vec3 center, float radius,
                                   float half_segment, const WPE_Mat4* transform) {
    enum { RINGS = WPE__SPHERE_RINGS, SEGMENTS = WPE__SPHERE_SEGMENTS };
    /* Rows of vertices from the top pole to the bottom one: the top cap's rings, then the bottom cap's
     * (the cylinder is the band between the two equators). */
    enum { ROWS = RINGS + 2 };
    WPE_Vec3 vertices[ROWS * SEGMENTS];
    uint32_t indices[(ROWS - 1) * SEGMENTS * 6];
    const float pi = 3.14159265358979f;
    for (uint32_t row = 0; row < ROWS; ++row) {
        /* Rows 0..RINGS/2 belong to the top cap (polar angle 0..90 degrees), the rest to the bottom. */
        const bool top = row <= RINGS / 2u;
        const uint32_t ring = top ? row : row - 1u;
        const float polar = pi * (float)ring / (float)RINGS;
        const float y = cosf(polar) * radius + (top ? half_segment : -half_segment);
        const float r = sinf(polar) * radius;
        for (uint32_t s = 0; s < SEGMENTS; ++s) {
            const float azimuth = 2.0f * pi * (float)s / (float)SEGMENTS;
            vertices[row * SEGMENTS + s] =
                wpe_vec3(center.x + r * cosf(azimuth), center.y + y, center.z - r * sinf(azimuth));
        }
    }
    uint32_t n = 0;
    for (uint32_t row = 0; row + 1 < ROWS; ++row) {
        for (uint32_t s = 0; s < SEGMENTS; ++s) {
            const uint32_t s1 = (s + 1u) % SEGMENTS;
            const uint32_t a = row * SEGMENTS + s, b = row * SEGMENTS + s1;
            const uint32_t c = (row + 1u) * SEGMENTS + s, d = (row + 1u) * SEGMENTS + s1;
            /* Counter-clockwise from outside (azimuth turns counter-clockwise seen from above). Pole rows
             * have coincident vertices: their degenerate triangles are skipped when added. */
            const uint32_t quad[6] = {a, c, d, a, d, b};
            memcpy(&indices[n], quad, sizeof quad);
            n += 6;
        }
    }
    return wpe_nav_geometry_add_triangles(geometry, vertices, ROWS * SEGMENTS, indices, n, transform);
}

static WPE_Result wpe__add_model_mesh(WPE_NavGeometry* geometry, const WPE_Model* model,
                                      const WPE_Collider* c, const WPE_Mat4* transform) {
    const WPE_Mesh* mesh = &model->meshes[c->mesh];
    for (uint32_t p = 0; p < mesh->primitive_count; ++p) {
        const WPE_MeshPrimitive* primitive = &model->primitives[mesh->first_primitive + p];
        /* Vertex positions plus the collider offset; one primitive at a time keeps the copy small. */
        WPE_Vec3* points =
            primitive->vertex_count ? wpe_alloc(primitive->vertex_count * sizeof *points) : NULL;
        if (primitive->vertex_count && !points) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
        for (uint32_t v = 0; v < primitive->vertex_count; ++v)
            points[v] = wpe_vec3_add(model->vertices[primitive->vertex_offset + v].position, c->offset);
        const WPE_Result result = wpe_nav_geometry_add_triangles(geometry, points, primitive->vertex_count,
                                                                 model->indices + primitive->index_offset,
                                                                 primitive->index_count, transform);
        wpe_free(points);
        if (result != WPE_RESULT_OK) return result;
    }
    return WPE_RESULT_OK;
}

static float wpe__at_least(float value, float minimum) {
    return value > minimum ? value : minimum;
}

/* One collider, built as physics.c builds its shape: world position and rotation from the entity,
 * the (positive) world scale baked into the size and offset. */
static WPE_Result wpe__add_collider(WPE_NavGeometry* geometry, const WPE_Ecs* ecs,
                                    const WPE_AssetManager* assets, WPE_Entity entity,
                                    const WPE_Collider* c) {
    WPE_Mat4 world;
    WPE_Vec3 position, scale;
    WPE_Quat rotation;
    if (!wpe_scene_compute_world_matrix(ecs, entity, &world) ||
        !wpe_mat4_decompose(&world, &position, &rotation, &scale) || !wpe__finite(position) ||
        !wpe__finite(scale)) {
        wpe_log_warn("collider of entity 0x%08x has no usable transform; navigation skips it", entity);
        return WPE_RESULT_OK;
    }
    const WPE_Mat4 pose = wpe_mat4_from_trs(position, rotation, wpe_vec3(1.0f, 1.0f, 1.0f));
    const WPE_Vec3 offset = wpe_vec3_mul(c->offset, scale);
    switch (c->shape) {
    case WPE_COLLIDER_BOX: {
        const WPE_Vec3 half = wpe_vec3(wpe__at_least(fabsf(c->size.x * scale.x) * 0.5f, WPE__MIN_EXTENT),
                                       wpe__at_least(fabsf(c->size.y * scale.y) * 0.5f, WPE__MIN_EXTENT),
                                       wpe__at_least(fabsf(c->size.z * scale.z) * 0.5f, WPE__MIN_EXTENT));
        return wpe_nav_geometry_add_box(geometry, offset, half, &pose);
    }
    case WPE_COLLIDER_SPHERE: {
        const float largest = fmaxf(fmaxf(fabsf(scale.x), fabsf(scale.y)), fabsf(scale.z));
        return wpe__add_capsule(geometry, offset, wpe__at_least(fabsf(c->size.x) * largest, WPE__MIN_EXTENT),
                                0.0f, &pose);
    }
    case WPE_COLLIDER_CAPSULE: {
        const float radius =
            wpe__at_least(fabsf(c->size.x) * fmaxf(fabsf(scale.x), fabsf(scale.z)), WPE__MIN_EXTENT);
        const float half_segment = fabsf(c->size.y * scale.y) * 0.5f - radius;
        return wpe__add_capsule(geometry, offset, radius,
                                half_segment > WPE__MIN_EXTENT ? half_segment : 0.0f, &pose);
    }
    case WPE_COLLIDER_CONVEX:
    case WPE_COLLIDER_MESH: {
        const WPE_Model* model = assets ? wpe_asset_model(assets, c->model) : NULL;
        if (!model || c->mesh >= model->mesh_count) {
            wpe_log_warn("mesh collider of entity 0x%08x has no loaded model or a bad mesh index; navigation "
                         "skips it",
                         entity);
            return WPE_RESULT_OK;
        }
        /* Vertices are (position + offset) * scale, as physics builds them. */
        const WPE_Mat4 scaled = wpe_mat4_from_trs(position, rotation, scale);
        return wpe__add_model_mesh(geometry, model, c, &scaled);
    }
    }
    wpe_log_warn("collider of entity 0x%08x has an unknown shape; navigation skips it", entity);
    return WPE_RESULT_OK;
}

WPE_Result wpe_nav_geometry_add_colliders(WPE_NavGeometry* geometry, WPE_Ecs* ecs,
                                          const WPE_AssetManager* assets, uint32_t* out_added) {
    if (out_added) *out_added = 0;
    if (!geometry || !ecs) return WPE_RESULT_ERROR_NULL_ARG;
    if (!wpe_ecs_is_registered(ecs, WPE_COMP_PHYSICS_COLLIDER) ||
        !wpe_ecs_is_registered(ecs, WPE_COMP_PHYSICS_RIGIDBODY)) {
        wpe_log_error("navigation needs the physics components registered (wpe_physics_create)");
        return WPE_RESULT_ERROR_INVALID_STATE;
    }
    uint32_t added = 0;
    WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_PHYSICS_COLLIDER)});
    while (wpe_ecs_iter_next(&it)) {
        const WPE_Collider* collider = wpe_ecs_iter_get(&it, WPE_COMP_PHYSICS_COLLIDER);
        if (collider->is_trigger) continue;
        const WPE_RigidBody* body = wpe_ecs_get_component(ecs, it.entity, WPE_COMP_PHYSICS_RIGIDBODY);
        if (body && body->type != WPE_BODY_STATIC) continue; /* Moving obstacles, not level geometry. */
        const uint32_t before = geometry->triangle_count;
        const WPE_Result result = wpe__add_collider(geometry, ecs, assets, it.entity, collider);
        if (result != WPE_RESULT_OK) {
            wpe_log_error("cannot add the collider of entity 0x%08x to the navigation geometry: %s",
                          it.entity, wpe_result_to_string(result));
            return result;
        }
        if (geometry->triangle_count > before) ++added;
    }
    if (out_added) *out_added = added;
    return WPE_RESULT_OK;
}
