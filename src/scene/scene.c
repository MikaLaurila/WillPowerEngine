#define WPE_LOG_MODULE "scene"
#include "scene/scene.h"

#include <stdio.h>

#include "core/alloc.h"
#include "core/debug.h"
#include "core/log.h"

/* ---- Helpers ------------------------------------------------------------------------------------- */

static WPE_Hierarchy* wpe__scene_hierarchy(const WPE_Ecs* ecs, WPE_Entity entity) {
    return wpe_ecs_get_component(ecs, entity, WPE_COMP_HIERARCHY);
}

static WPE_Mat4 wpe__scene_local_matrix(const WPE_Transform* t) {
    return wpe_mat4_from_trs(t->position, t->rotation, t->scale);
}

/* Unlinks `entity` from its parent's child list. The entity's own children are untouched. */
static void wpe__scene_unlink_from_parent(const WPE_Ecs* ecs, WPE_Entity entity, WPE_Hierarchy* node) {
    if (node->parent == WPE_ENTITY_NULL) return;

    WPE_Hierarchy* parent = wpe__scene_hierarchy(ecs, node->parent);
    wpe_assert(parent != NULL);
    if (parent) {
        if (parent->first_child == entity) parent->first_child = node->next_sibling;
        if (parent->child_count > 0) --parent->child_count;
    }

    WPE_Hierarchy* prev = wpe__scene_hierarchy(ecs, node->prev_sibling);
    if (prev) prev->next_sibling = node->next_sibling;
    WPE_Hierarchy* next = wpe__scene_hierarchy(ecs, node->next_sibling);
    if (next) next->prev_sibling = node->prev_sibling;

    node->parent = WPE_ENTITY_NULL;
    node->prev_sibling = WPE_ENTITY_NULL;
    node->next_sibling = WPE_ENTITY_NULL;
}

/*
 * on_remove hook for WPE_COMP_HIERARCHY: keeps the forest consistent when a node disappears by
 * unlinking it from its parent and turning its children into roots. Only edits component data.
 */
static void wpe__scene_hierarchy_on_remove(WPE_Ecs* ecs, WPE_Entity entity, void* component,
                                           void* user_data) {
    (void)user_data;
    WPE_Hierarchy* node = component;
    wpe__scene_unlink_from_parent(ecs, entity, node);

    WPE_Entity child = node->first_child;
    while (child != WPE_ENTITY_NULL) {
        WPE_Hierarchy* child_node = wpe__scene_hierarchy(ecs, child);
        if (!child_node) break;
        const WPE_Entity next = child_node->next_sibling;
        child_node->parent = WPE_ENTITY_NULL;
        child_node->prev_sibling = WPE_ENTITY_NULL;
        child_node->next_sibling = WPE_ENTITY_NULL;
        child = next;
    }
    node->first_child = WPE_ENTITY_NULL;
    node->child_count = 0;
}

static WPE_Result wpe__scene_ensure_hierarchy(WPE_Ecs* ecs, WPE_Entity entity, WPE_Hierarchy** out_node) {
    *out_node = wpe__scene_hierarchy(ecs, entity);
    if (*out_node) return WPE_RESULT_OK;
    return wpe_ecs_add_component(ecs, entity, WPE_COMP_HIERARCHY, NULL, (void**)out_node);
}

/* Upper bound on the number of entities that can appear in any subtree or the whole forest. */
static uint32_t wpe__scene_node_bound(const WPE_Ecs* ecs) {
    return wpe_ecs_component_count(ecs, WPE_COMP_HIERARCHY) +
           wpe_ecs_component_count(ecs, WPE_COMP_TRANSFORM) + 1;
}

/*
 * Collects `root` and its descendants in pre-order (parents before children).
 * On success *out_nodes is an owned array the caller frees with wpe_free().
 */
static WPE_Result wpe__scene_collect_subtree(const WPE_Ecs* ecs, WPE_Entity root, WPE_Entity** out_nodes,
                                             uint32_t* out_count) {
    const uint32_t bound = wpe__scene_node_bound(ecs);
    WPE_Entity* nodes = wpe_alloc((size_t)bound * sizeof *nodes);
    WPE_Entity* stack = wpe_alloc((size_t)bound * sizeof *stack);
    if (!nodes || !stack) {
        wpe_free(nodes);
        wpe_free(stack);
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }

    uint32_t count = 0;
    uint32_t top = 0;
    stack[top++] = root;
    while (top > 0) {
        const WPE_Entity entity = stack[--top];
        nodes[count++] = entity;
        const WPE_Hierarchy* node = wpe__scene_hierarchy(ecs, entity);
        for (WPE_Entity child = node ? node->first_child : WPE_ENTITY_NULL; child != WPE_ENTITY_NULL;) {
            wpe_assert(top < bound);
            stack[top++] = child;
            const WPE_Hierarchy* child_node = wpe__scene_hierarchy(ecs, child);
            child = child_node ? child_node->next_sibling : WPE_ENTITY_NULL;
        }
    }

    wpe_free(stack);
    *out_nodes = nodes;
    *out_count = count;
    return WPE_RESULT_OK;
}

/* World matrix inherited from the nearest transformed ancestor, using cached world matrices. */
static WPE_Mat4 wpe__scene_inherited_world(const WPE_Ecs* ecs, const WPE_Hierarchy* node) {
    for (WPE_Entity ancestor = node ? node->parent : WPE_ENTITY_NULL; ancestor != WPE_ENTITY_NULL;) {
        const WPE_Transform* t = wpe_ecs_get_component(ecs, ancestor, WPE_COMP_TRANSFORM);
        if (t) return t->world;
        const WPE_Hierarchy* ancestor_node = wpe__scene_hierarchy(ecs, ancestor);
        ancestor = ancestor_node ? ancestor_node->parent : WPE_ENTITY_NULL;
    }
    return wpe_mat4_identity();
}

/* ---- Public API ---------------------------------------------------------------------------------- */

WPE_Transform wpe_transform_identity(void) {
    return (WPE_Transform){
        .position = wpe_vec3(0.0f, 0.0f, 0.0f),
        .rotation = wpe_quat_identity(),
        .scale = wpe_vec3(1.0f, 1.0f, 1.0f),
        .world = wpe_mat4_identity(),
    };
}

WPE_Result wpe_scene_register_components(WPE_Ecs* ecs) {
    if (!ecs) return WPE_RESULT_ERROR_NULL_ARG;

    const struct {
        WPE_ComponentType type;
        WPE_ComponentDesc desc;
    } components[] = {
        {WPE_COMP_TRANSFORM,
         {.name = "Transform", .size = sizeof(WPE_Transform), .alignment = alignof(WPE_Transform)}},
        {WPE_COMP_HIERARCHY,
         {.name = "Hierarchy",
          .size = sizeof(WPE_Hierarchy),
          .alignment = alignof(WPE_Hierarchy),
          .on_remove = wpe__scene_hierarchy_on_remove}},
        {WPE_COMP_NAME, {.name = "Name", .size = sizeof(WPE_Name), .alignment = alignof(WPE_Name)}},
    };

    for (size_t i = 0; i < sizeof components / sizeof components[0]; ++i) {
        const WPE_Result result = wpe_ecs_register_component(ecs, components[i].type, &components[i].desc);
        if (result != WPE_RESULT_OK) {
            wpe_log_error("failed to register component '%s': %s", components[i].desc.name,
                          wpe_result_to_string(result));
            return result;
        }
    }
    return WPE_RESULT_OK;
}

bool wpe_scene_is_ancestor(const WPE_Ecs* ecs, WPE_Entity ancestor, WPE_Entity entity) {
    if (!ecs || ancestor == WPE_ENTITY_NULL) return false;
    for (WPE_Entity e = wpe_scene_get_parent(ecs, entity); e != WPE_ENTITY_NULL;
         e = wpe_scene_get_parent(ecs, e)) {
        if (e == ancestor) return true;
    }
    return false;
}

WPE_Entity wpe_scene_get_parent(const WPE_Ecs* ecs, WPE_Entity entity) {
    if (!ecs) return WPE_ENTITY_NULL;
    const WPE_Hierarchy* node = wpe__scene_hierarchy(ecs, entity);
    return node ? node->parent : WPE_ENTITY_NULL;
}

WPE_Result wpe_scene_set_parent(WPE_Ecs* ecs, WPE_Entity child, WPE_Entity parent, bool keep_world) {
    if (!ecs) return WPE_RESULT_ERROR_NULL_ARG;
    if (!wpe_ecs_is_alive(ecs, child)) return WPE_RESULT_ERROR_NOT_FOUND;
    if (parent != WPE_ENTITY_NULL) {
        if (!wpe_ecs_is_alive(ecs, parent)) return WPE_RESULT_ERROR_NOT_FOUND;
        /* Only an entity with children can be an ancestor; skipping the walk keeps leaf parenting O(1). */
        const WPE_Hierarchy* child_node = wpe__scene_hierarchy(ecs, child);
        const bool has_children = child_node && child_node->first_child != WPE_ENTITY_NULL;
        if (parent == child || (has_children && wpe_scene_is_ancestor(ecs, child, parent))) {
            wpe_log_error("cannot parent entity 0x%08x under 0x%08x: would create a cycle", child, parent);
            return WPE_RESULT_ERROR_INVALID_ARG;
        }
    }
    if (wpe_scene_get_parent(ecs, child) == parent) return WPE_RESULT_OK;

    /* Resolve keep_world before touching anything so failure leaves the scene unchanged. */
    WPE_Transform* transform = wpe_ecs_get_component(ecs, child, WPE_COMP_TRANSFORM);
    WPE_Vec3 new_position, new_scale;
    WPE_Quat new_rotation;
    const bool rewrite_local = keep_world && transform != NULL;
    if (rewrite_local) {
        WPE_Mat4 child_world, parent_world, parent_inverse;
        wpe_scene_compute_world_matrix(ecs, child, &child_world);
        parent_world = wpe_mat4_identity();
        if (parent != WPE_ENTITY_NULL) wpe_scene_compute_world_matrix(ecs, parent, &parent_world);

        if (!wpe_mat4_inverse(&parent_world, &parent_inverse)) {
            wpe_log_error("cannot keep world transform: new parent matrix is singular");
            return WPE_RESULT_ERROR_INVALID_STATE;
        }
        const WPE_Mat4 local = wpe_mat4_mul(&parent_inverse, &child_world);
        if (!wpe_mat4_decompose(&local, &new_position, &new_rotation, &new_scale)) {
            wpe_log_error("cannot keep world transform: resulting local matrix is degenerate");
            return WPE_RESULT_ERROR_INVALID_STATE;
        }
    }

    WPE_Hierarchy* child_node;
    WPE_Result result = wpe__scene_ensure_hierarchy(ecs, child, &child_node);
    if (result != WPE_RESULT_OK) return result;

    WPE_Hierarchy* parent_node = NULL;
    if (parent != WPE_ENTITY_NULL) {
        result = wpe__scene_ensure_hierarchy(ecs, parent, &parent_node);
        if (result != WPE_RESULT_OK) return result;
        /* Adding the parent's component may have moved the child's within the pool. */
        child_node = wpe__scene_hierarchy(ecs, child);
    }

    wpe__scene_unlink_from_parent(ecs, child, child_node);

    if (parent_node) {
        child_node->parent = parent;
        if (parent_node->first_child == WPE_ENTITY_NULL) {
            parent_node->first_child = child;
        } else {
            WPE_Entity last = parent_node->first_child;
            WPE_Hierarchy* last_node = wpe__scene_hierarchy(ecs, last);
            while (last_node->next_sibling != WPE_ENTITY_NULL) {
                last = last_node->next_sibling;
                last_node = wpe__scene_hierarchy(ecs, last);
            }
            last_node->next_sibling = child;
            child_node->prev_sibling = last;
        }
        ++parent_node->child_count;
    }

    if (rewrite_local) {
        transform = wpe_ecs_get_component(ecs, child, WPE_COMP_TRANSFORM);
        transform->position = new_position;
        transform->rotation = new_rotation;
        transform->scale = new_scale;
    }
    return WPE_RESULT_OK;
}

WPE_Result wpe_scene_destroy_recursive(WPE_Ecs* ecs, WPE_Entity entity) {
    if (!ecs) return WPE_RESULT_ERROR_NULL_ARG;
    if (!wpe_ecs_is_alive(ecs, entity)) return WPE_RESULT_ERROR_NOT_FOUND;

    WPE_Entity* nodes;
    uint32_t count;
    const WPE_Result result = wpe__scene_collect_subtree(ecs, entity, &nodes, &count);
    if (result != WPE_RESULT_OK) return result;

    /* Reverse pre-order despawns children before their parents. */
    for (uint32_t i = count; i-- > 0;) {
        const WPE_Result despawned = wpe_ecs_despawn(ecs, nodes[i]);
        wpe_assert(despawned == WPE_RESULT_OK);
        (void)despawned;
    }
    wpe_free(nodes);
    return WPE_RESULT_OK;
}

WPE_Result wpe_scene_destroy_recursive_deferred(WPE_Ecs* ecs, WPE_Entity entity) {
    if (!ecs) return WPE_RESULT_ERROR_NULL_ARG;
    if (!wpe_ecs_is_alive(ecs, entity)) return WPE_RESULT_ERROR_NOT_FOUND;

    WPE_Entity* nodes;
    uint32_t count;
    WPE_Result result = wpe__scene_collect_subtree(ecs, entity, &nodes, &count);
    if (result != WPE_RESULT_OK) return result;

    for (uint32_t i = count; i-- > 0 && result == WPE_RESULT_OK;) {
        result = wpe_ecs_despawn_deferred(ecs, nodes[i]);
    }
    wpe_free(nodes);
    return result;
}

bool wpe_scene_compute_world_matrix(const WPE_Ecs* ecs, WPE_Entity entity, WPE_Mat4* out_world) {
    if (!ecs || !out_world || !wpe_ecs_is_alive(ecs, entity)) return false;

    WPE_Mat4 world = wpe_mat4_identity();
    for (WPE_Entity e = entity; e != WPE_ENTITY_NULL; e = wpe_scene_get_parent(ecs, e)) {
        const WPE_Transform* t = wpe_ecs_get_component(ecs, e, WPE_COMP_TRANSFORM);
        if (!t) continue;
        const WPE_Mat4 local = wpe__scene_local_matrix(t);
        world = wpe_mat4_mul(&local, &world);
    }
    *out_world = world;
    return true;
}

WPE_Result wpe_scene_update_transforms(WPE_Ecs* ecs) {
    if (!ecs) return WPE_RESULT_ERROR_NULL_ARG;

    /* Every node is pushed exactly once, so the forest size bounds the stack. */
    const uint32_t bound = wpe__scene_node_bound(ecs);
    WPE_Entity* stack = wpe_alloc((size_t)bound * sizeof *stack);
    if (!stack) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    uint32_t top = 0;

    /* Roots: transformed entities without a parent, plus untransformed hierarchy roots. */
    WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_TRANSFORM)});
    while (wpe_ecs_iter_next(&it)) {
        if (wpe_scene_get_parent(ecs, it.entity) == WPE_ENTITY_NULL) stack[top++] = it.entity;
    }
    it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_HIERARCHY),
                                           .none = WPE_ECS_MASK(WPE_COMP_TRANSFORM)});
    while (wpe_ecs_iter_next(&it)) {
        const WPE_Hierarchy* node = wpe_ecs_iter_get(&it, WPE_COMP_HIERARCHY);
        if (node->parent == WPE_ENTITY_NULL && node->first_child != WPE_ENTITY_NULL) stack[top++] = it.entity;
    }

    while (top > 0) {
        const WPE_Entity entity = stack[--top];
        const WPE_Hierarchy* node = wpe__scene_hierarchy(ecs, entity);

        WPE_Transform* t = wpe_ecs_get_component(ecs, entity, WPE_COMP_TRANSFORM);
        if (t) {
            const WPE_Mat4 inherited = wpe__scene_inherited_world(ecs, node);
            const WPE_Mat4 local = wpe__scene_local_matrix(t);
            t->world = wpe_mat4_mul(&inherited, &local);
        }

        for (WPE_Entity child = node ? node->first_child : WPE_ENTITY_NULL; child != WPE_ENTITY_NULL;) {
            wpe_assert(top < bound);
            stack[top++] = child;
            const WPE_Hierarchy* child_node = wpe__scene_hierarchy(ecs, child);
            child = child_node ? child_node->next_sibling : WPE_ENTITY_NULL;
        }
    }

    wpe_free(stack);
    return WPE_RESULT_OK;
}

WPE_Result wpe_scene_set_name(WPE_Ecs* ecs, WPE_Entity entity, const char* name) {
    if (!ecs || !name) return WPE_RESULT_ERROR_NULL_ARG;
    if (!wpe_ecs_is_alive(ecs, entity)) return WPE_RESULT_ERROR_NOT_FOUND;

    WPE_Name* component = wpe_ecs_get_component(ecs, entity, WPE_COMP_NAME);
    if (!component) {
        const WPE_Result result = wpe_ecs_add_component(ecs, entity, WPE_COMP_NAME, NULL, (void**)&component);
        if (result != WPE_RESULT_OK) return result;
    }
    snprintf(component->value, sizeof component->value, "%s", name);
    return WPE_RESULT_OK;
}

const char* wpe_scene_get_name(const WPE_Ecs* ecs, WPE_Entity entity) {
    const WPE_Name* component = wpe_ecs_get_component(ecs, entity, WPE_COMP_NAME);
    return component ? component->value : NULL;
}
