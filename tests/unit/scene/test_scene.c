#include "wpe_test.h"

#include "scene/scene.h"

#define EPS 1e-4

static WPE_Ecs* make_scene(void) {
    WPE_Ecs* ecs = NULL;
    WPE_TEST_ASSERT_OK(wpe_ecs_create(&ecs));
    WPE_TEST_ASSERT_OK(wpe_scene_register_components(ecs));
    return ecs;
}

static WPE_Entity spawn_at(WPE_Ecs* ecs, WPE_Vec3 position) {
    const WPE_Entity e = wpe_ecs_spawn(ecs);
    WPE_Transform t = wpe_transform_identity();
    t.position = position;
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, e, WPE_COMP_TRANSFORM, &t, NULL));
    return e;
}

static WPE_Transform* transform_of(WPE_Ecs* ecs, WPE_Entity e) {
    WPE_Transform* t = wpe_ecs_get_component(ecs, e, WPE_COMP_TRANSFORM);
    WPE_TEST_ASSERT(t != NULL);
    return t;
}

static WPE_Vec3 world_position(WPE_Ecs* ecs, WPE_Entity e) {
    const WPE_Transform* t = transform_of(ecs, e);
    return wpe_vec3(t->world.m[3][0], t->world.m[3][1], t->world.m[3][2]);
}

static void assert_vec3_near(WPE_Vec3 expected, WPE_Vec3 actual) {
    WPE_TEST_ASSERT_NEAR(expected.x, actual.x, EPS);
    WPE_TEST_ASSERT_NEAR(expected.y, actual.y, EPS);
    WPE_TEST_ASSERT_NEAR(expected.z, actual.z, EPS);
}

/* Collects an entity's children in list order. */
static uint32_t children_of(WPE_Ecs* ecs, WPE_Entity parent, WPE_Entity* out, uint32_t max) {
    const WPE_Hierarchy* node = wpe_ecs_get_component(ecs, parent, WPE_COMP_HIERARCHY);
    uint32_t count = 0;
    for (WPE_Entity c = node ? node->first_child : WPE_ENTITY_NULL; c != WPE_ENTITY_NULL && count < max;) {
        out[count++] = c;
        const WPE_Hierarchy* child = wpe_ecs_get_component(ecs, c, WPE_COMP_HIERARCHY);
        c = child->next_sibling;
    }
    if (node) WPE_TEST_ASSERT_EQ_UINT(node->child_count, count);
    return count;
}

static void test_scene_register_twice_fails(void) {
    WPE_Ecs* ecs = make_scene();
    wpe_log_set_stderr_enabled(false);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_ALREADY_EXISTS, wpe_scene_register_components(ecs));
    wpe_log_set_stderr_enabled(true);
    WPE_TEST_ASSERT_EQ_STR("Transform", wpe_ecs_component_name(ecs, WPE_COMP_TRANSFORM));
    WPE_TEST_ASSERT_EQ_STR("Hierarchy", wpe_ecs_component_name(ecs, WPE_COMP_HIERARCHY));
    WPE_TEST_ASSERT_EQ_STR("Name", wpe_ecs_component_name(ecs, WPE_COMP_NAME));
    wpe_ecs_destroy(ecs);
}

static void test_scene_set_parent_links_children_in_order(void) {
    WPE_Ecs* ecs = make_scene();
    const WPE_Entity parent = wpe_ecs_spawn(ecs);
    WPE_Entity kids[3];
    for (int i = 0; i < 3; ++i) {
        kids[i] = wpe_ecs_spawn(ecs);
        WPE_TEST_ASSERT_OK(wpe_scene_set_parent(ecs, kids[i], parent, false));
        WPE_TEST_ASSERT(wpe_scene_get_parent(ecs, kids[i]) == parent);
    }

    WPE_Entity listed[4];
    WPE_TEST_ASSERT_EQ_UINT(3, children_of(ecs, parent, listed, 4));
    for (int i = 0; i < 3; ++i)
        WPE_TEST_ASSERT(listed[i] == kids[i]);

    /* Detach the middle child. */
    WPE_TEST_ASSERT_OK(wpe_scene_set_parent(ecs, kids[1], WPE_ENTITY_NULL, false));
    WPE_TEST_ASSERT_EQ_UINT(2, children_of(ecs, parent, listed, 4));
    WPE_TEST_ASSERT(listed[0] == kids[0] && listed[1] == kids[2]);
    WPE_TEST_ASSERT(wpe_scene_get_parent(ecs, kids[1]) == WPE_ENTITY_NULL);

    /* Re-parenting to the same parent is a no-op. */
    WPE_TEST_ASSERT_OK(wpe_scene_set_parent(ecs, kids[0], parent, false));
    WPE_TEST_ASSERT_EQ_UINT(2, children_of(ecs, parent, listed, 4));
    wpe_ecs_destroy(ecs);
}

static void test_scene_set_parent_rejects_cycles_and_dead_entities(void) {
    WPE_Ecs* ecs = make_scene();
    const WPE_Entity a = wpe_ecs_spawn(ecs);
    const WPE_Entity b = wpe_ecs_spawn(ecs);
    const WPE_Entity c = wpe_ecs_spawn(ecs);
    WPE_TEST_ASSERT_OK(wpe_scene_set_parent(ecs, b, a, false));
    WPE_TEST_ASSERT_OK(wpe_scene_set_parent(ecs, c, b, false));
    WPE_TEST_ASSERT(wpe_scene_is_ancestor(ecs, a, c));
    WPE_TEST_ASSERT(!wpe_scene_is_ancestor(ecs, c, a));

    wpe_log_set_stderr_enabled(false);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_scene_set_parent(ecs, a, c, false));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_scene_set_parent(ecs, a, a, false));
    wpe_log_set_stderr_enabled(true);

    const WPE_Entity dead = wpe_ecs_spawn(ecs);
    WPE_TEST_ASSERT_OK(wpe_ecs_despawn(ecs, dead));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND, wpe_scene_set_parent(ecs, a, dead, false));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND, wpe_scene_set_parent(ecs, dead, a, false));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NULL_ARG, wpe_scene_set_parent(NULL, a, b, false));
    WPE_TEST_ASSERT(wpe_scene_get_parent(ecs, a) == WPE_ENTITY_NULL);
    wpe_ecs_destroy(ecs);
}

static void test_scene_update_transforms_propagates_through_hierarchy(void) {
    WPE_Ecs* ecs = make_scene();
    const WPE_Entity root = spawn_at(ecs, wpe_vec3(10, 0, 0));
    const WPE_Entity child = spawn_at(ecs, wpe_vec3(0, 5, 0));
    const WPE_Entity grandchild = spawn_at(ecs, wpe_vec3(1, 0, 0));
    WPE_TEST_ASSERT_OK(wpe_scene_set_parent(ecs, child, root, false));
    WPE_TEST_ASSERT_OK(wpe_scene_set_parent(ecs, grandchild, child, false));

    /* Rotate the root 90 degrees about Z and scale it by 2. */
    WPE_Transform* rt = transform_of(ecs, root);
    rt->rotation = wpe_quat_from_axis_angle(wpe_vec3(0, 0, 1), WPE_PI / 2);
    rt->scale = wpe_vec3(2, 2, 2);

    WPE_TEST_ASSERT_OK(wpe_scene_update_transforms(ecs));
    assert_vec3_near(wpe_vec3(10, 0, 0), world_position(ecs, root));
    /* child local (0,5,0) -> scaled (0,10,0) -> rotated (-10,0,0) -> +root (0,0,0). */
    assert_vec3_near(wpe_vec3(0, 0, 0), world_position(ecs, child));
    /* grandchild local (1,0,0) in child space -> (2,0,0) scaled -> rotated (0,2,0). */
    assert_vec3_near(wpe_vec3(0, 2, 0), world_position(ecs, grandchild));

    WPE_Mat4 computed;
    WPE_TEST_ASSERT(wpe_scene_compute_world_matrix(ecs, grandchild, &computed));
    assert_vec3_near(world_position(ecs, grandchild),
                     wpe_vec3(computed.m[3][0], computed.m[3][1], computed.m[3][2]));
    wpe_ecs_destroy(ecs);
}

static void test_scene_untransformed_nodes_are_transparent(void) {
    WPE_Ecs* ecs = make_scene();
    const WPE_Entity root = spawn_at(ecs, wpe_vec3(3, 0, 0));
    const WPE_Entity group = wpe_ecs_spawn(ecs); /* No transform. */
    const WPE_Entity leaf = spawn_at(ecs, wpe_vec3(0, 1, 0));
    WPE_TEST_ASSERT_OK(wpe_scene_set_parent(ecs, group, root, false));
    WPE_TEST_ASSERT_OK(wpe_scene_set_parent(ecs, leaf, group, false));

    const WPE_Entity loose_group = wpe_ecs_spawn(ecs); /* Untransformed root with a child. */
    const WPE_Entity loose_leaf = spawn_at(ecs, wpe_vec3(0, 0, 7));
    WPE_TEST_ASSERT_OK(wpe_scene_set_parent(ecs, loose_leaf, loose_group, false));

    WPE_TEST_ASSERT_OK(wpe_scene_update_transforms(ecs));
    assert_vec3_near(wpe_vec3(3, 1, 0), world_position(ecs, leaf));
    assert_vec3_near(wpe_vec3(0, 0, 7), world_position(ecs, loose_leaf));
    wpe_ecs_destroy(ecs);
}

static void test_scene_set_parent_keep_world(void) {
    WPE_Ecs* ecs = make_scene();
    const WPE_Entity parent = spawn_at(ecs, wpe_vec3(5, 0, 0));
    WPE_Transform* pt = transform_of(ecs, parent);
    pt->rotation = wpe_quat_from_axis_angle(wpe_vec3(0, 1, 0), 0.7f);
    pt->scale = wpe_vec3(2, 2, 2);

    const WPE_Entity child = spawn_at(ecs, wpe_vec3(1, 2, 3));
    transform_of(ecs, child)->rotation = wpe_quat_from_euler_xyz(wpe_vec3(0.3f, -1.1f, 0.4f));
    transform_of(ecs, child)->scale = wpe_vec3(1, 5, 0.5f);
    WPE_Mat4 before;
    WPE_TEST_ASSERT(wpe_scene_compute_world_matrix(ecs, child, &before));

    WPE_TEST_ASSERT_OK(wpe_scene_set_parent(ecs, child, parent, true));
    WPE_Mat4 after;
    WPE_TEST_ASSERT(wpe_scene_compute_world_matrix(ecs, child, &after));
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r)
            WPE_TEST_ASSERT_NEAR(before.m[c][r], after.m[c][r], 1e-3);
    }

    /* Detaching with keep_world restores the original local transform. */
    WPE_TEST_ASSERT_OK(wpe_scene_set_parent(ecs, child, WPE_ENTITY_NULL, true));
    assert_vec3_near(wpe_vec3(1, 2, 3), transform_of(ecs, child)->position);
    wpe_ecs_destroy(ecs);
}

static void test_scene_keep_world_nonuniform_parent_preserves_position(void) {
    WPE_Ecs* ecs = make_scene();
    const WPE_Entity parent = spawn_at(ecs, wpe_vec3(5, 0, 0));
    transform_of(ecs, parent)->rotation = wpe_quat_from_axis_angle(wpe_vec3(0, 1, 0), 0.7f);
    transform_of(ecs, parent)->scale = wpe_vec3(2, 3, 4);
    const WPE_Entity child = spawn_at(ecs, wpe_vec3(1, 2, 3));
    transform_of(ecs, child)->rotation = wpe_quat_from_axis_angle(wpe_vec3(1, 0, 0), 0.5f);

    WPE_TEST_ASSERT_OK(wpe_scene_set_parent(ecs, child, parent, true));
    WPE_TEST_ASSERT_OK(wpe_scene_update_transforms(ecs));
    WPE_TEST_ASSERT_NEAR(1.0, world_position(ecs, child).x, 1e-3);
    WPE_TEST_ASSERT_NEAR(2.0, world_position(ecs, child).y, 1e-3);
    WPE_TEST_ASSERT_NEAR(3.0, world_position(ecs, child).z, 1e-3);
    wpe_ecs_destroy(ecs);
}

static void test_scene_keep_world_fails_cleanly_on_singular_parent(void) {
    WPE_Ecs* ecs = make_scene();
    const WPE_Entity parent = spawn_at(ecs, wpe_vec3(0, 0, 0));
    transform_of(ecs, parent)->scale = wpe_vec3(0, 1, 1);
    const WPE_Entity child = spawn_at(ecs, wpe_vec3(1, 1, 1));

    wpe_log_set_stderr_enabled(false);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_STATE, wpe_scene_set_parent(ecs, child, parent, true));
    wpe_log_set_stderr_enabled(true);
    WPE_TEST_ASSERT(wpe_scene_get_parent(ecs, child) == WPE_ENTITY_NULL);
    assert_vec3_near(wpe_vec3(1, 1, 1), transform_of(ecs, child)->position);

    /* Without keep_world the same parenting succeeds; extreme scale must not break updates. */
    WPE_TEST_ASSERT_OK(wpe_scene_set_parent(ecs, child, parent, false));
    WPE_TEST_ASSERT_OK(wpe_scene_update_transforms(ecs));
    assert_vec3_near(wpe_vec3(0, 1, 1), world_position(ecs, child));
    wpe_ecs_destroy(ecs);
}

static void test_scene_despawning_parent_orphans_children(void) {
    WPE_Ecs* ecs = make_scene();
    const WPE_Entity grandparent = wpe_ecs_spawn(ecs);
    const WPE_Entity parent = wpe_ecs_spawn(ecs);
    const WPE_Entity a = wpe_ecs_spawn(ecs);
    const WPE_Entity b = wpe_ecs_spawn(ecs);
    WPE_TEST_ASSERT_OK(wpe_scene_set_parent(ecs, parent, grandparent, false));
    WPE_TEST_ASSERT_OK(wpe_scene_set_parent(ecs, a, parent, false));
    WPE_TEST_ASSERT_OK(wpe_scene_set_parent(ecs, b, parent, false));

    WPE_TEST_ASSERT_OK(wpe_ecs_despawn(ecs, parent));
    WPE_TEST_ASSERT(wpe_scene_get_parent(ecs, a) == WPE_ENTITY_NULL);
    WPE_TEST_ASSERT(wpe_scene_get_parent(ecs, b) == WPE_ENTITY_NULL);
    const WPE_Hierarchy* gp = wpe_ecs_get_component(ecs, grandparent, WPE_COMP_HIERARCHY);
    WPE_TEST_ASSERT(gp->first_child == WPE_ENTITY_NULL);
    WPE_TEST_ASSERT_EQ_UINT(0, gp->child_count);

    /* Removing just the hierarchy component behaves the same way. */
    WPE_TEST_ASSERT_OK(wpe_scene_set_parent(ecs, b, a, false));
    WPE_TEST_ASSERT_OK(wpe_ecs_remove_component(ecs, a, WPE_COMP_HIERARCHY));
    WPE_TEST_ASSERT(wpe_scene_get_parent(ecs, b) == WPE_ENTITY_NULL);
    WPE_TEST_ASSERT_OK(wpe_scene_update_transforms(ecs));
    wpe_ecs_destroy(ecs);
}

static void test_scene_destroy_recursive_removes_subtree_only(void) {
    WPE_Ecs* ecs = make_scene();
    const WPE_Entity keep_root = wpe_ecs_spawn(ecs);
    const WPE_Entity doomed = wpe_ecs_spawn(ecs);
    const WPE_Entity d1 = wpe_ecs_spawn(ecs);
    const WPE_Entity d2 = wpe_ecs_spawn(ecs);
    const WPE_Entity d11 = wpe_ecs_spawn(ecs);
    const WPE_Entity sibling = wpe_ecs_spawn(ecs);
    WPE_TEST_ASSERT_OK(wpe_scene_set_parent(ecs, doomed, keep_root, false));
    WPE_TEST_ASSERT_OK(wpe_scene_set_parent(ecs, sibling, keep_root, false));
    WPE_TEST_ASSERT_OK(wpe_scene_set_parent(ecs, d1, doomed, false));
    WPE_TEST_ASSERT_OK(wpe_scene_set_parent(ecs, d2, doomed, false));
    WPE_TEST_ASSERT_OK(wpe_scene_set_parent(ecs, d11, d1, false));

    WPE_TEST_ASSERT_OK(wpe_scene_destroy_recursive(ecs, doomed));
    WPE_TEST_ASSERT(!wpe_ecs_is_alive(ecs, doomed));
    WPE_TEST_ASSERT(!wpe_ecs_is_alive(ecs, d1));
    WPE_TEST_ASSERT(!wpe_ecs_is_alive(ecs, d2));
    WPE_TEST_ASSERT(!wpe_ecs_is_alive(ecs, d11));
    WPE_TEST_ASSERT(wpe_ecs_is_alive(ecs, keep_root));
    WPE_TEST_ASSERT(wpe_ecs_is_alive(ecs, sibling));

    WPE_Entity listed[2];
    WPE_TEST_ASSERT_EQ_UINT(1, children_of(ecs, keep_root, listed, 2));
    WPE_TEST_ASSERT(listed[0] == sibling);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND, wpe_scene_destroy_recursive(ecs, doomed));
    wpe_ecs_destroy(ecs);
}

static void test_scene_destroy_recursive_deferred(void) {
    WPE_Ecs* ecs = make_scene();
    const WPE_Entity root = spawn_at(ecs, wpe_vec3(0, 0, 0));
    const WPE_Entity child = spawn_at(ecs, wpe_vec3(0, 0, 0));
    WPE_TEST_ASSERT_OK(wpe_scene_set_parent(ecs, child, root, false));

    WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_TRANSFORM)});
    while (wpe_ecs_iter_next(&it)) {
        if (it.entity == root) WPE_TEST_ASSERT_OK(wpe_scene_destroy_recursive_deferred(ecs, root));
    }
    WPE_TEST_ASSERT_EQ_UINT(2, wpe_ecs_entity_count(ecs));
    WPE_TEST_ASSERT_EQ_UINT(2, wpe_ecs_flush_deferred(ecs));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_ecs_entity_count(ecs));
    wpe_ecs_destroy(ecs);
}

static void test_scene_names(void) {
    WPE_Ecs* ecs = make_scene();
    const WPE_Entity e = wpe_ecs_spawn(ecs);
    WPE_TEST_ASSERT(wpe_scene_get_name(ecs, e) == NULL);
    WPE_TEST_ASSERT_OK(wpe_scene_set_name(ecs, e, "Player"));
    WPE_TEST_ASSERT_EQ_STR("Player", wpe_scene_get_name(ecs, e));
    WPE_TEST_ASSERT_OK(wpe_scene_set_name(ecs, e, "Renamed"));
    WPE_TEST_ASSERT_EQ_STR("Renamed", wpe_scene_get_name(ecs, e));

    char long_name[200];
    memset(long_name, 'n', sizeof long_name - 1);
    long_name[sizeof long_name - 1] = '\0';
    WPE_TEST_ASSERT_OK(wpe_scene_set_name(ecs, e, long_name));
    WPE_TEST_ASSERT_EQ_UINT(WPE_NAME_MAX - 1, strlen(wpe_scene_get_name(ecs, e)));

    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NULL_ARG, wpe_scene_set_name(ecs, e, NULL));
    WPE_TEST_ASSERT_OK(wpe_ecs_despawn(ecs, e));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND, wpe_scene_set_name(ecs, e, "x"));
    wpe_ecs_destroy(ecs);
}

static void test_scene_deep_hierarchy_does_not_recurse(void) {
    /* A 50k-deep chain would overflow the call stack with a recursive implementation. */
    WPE_Ecs* ecs = make_scene();
    enum { DEPTH = 50000 };
    WPE_Entity parent = spawn_at(ecs, wpe_vec3(1, 0, 0));
    const WPE_Entity root = parent;
    for (int i = 1; i < DEPTH; ++i) {
        const WPE_Entity e = spawn_at(ecs, wpe_vec3(1, 0, 0));
        WPE_TEST_ASSERT_OK(wpe_scene_set_parent(ecs, e, parent, false));
        parent = e;
    }
    WPE_TEST_ASSERT_OK(wpe_scene_update_transforms(ecs));
    assert_vec3_near(wpe_vec3((float)DEPTH, 0, 0), world_position(ecs, parent));
    WPE_TEST_ASSERT_OK(wpe_scene_destroy_recursive(ecs, root));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_ecs_entity_count(ecs));
    wpe_ecs_destroy(ecs);
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_scene_register_twice_fails),
              WPE_TEST_CASE(test_scene_set_parent_links_children_in_order),
              WPE_TEST_CASE(test_scene_set_parent_rejects_cycles_and_dead_entities),
              WPE_TEST_CASE(test_scene_update_transforms_propagates_through_hierarchy),
              WPE_TEST_CASE(test_scene_untransformed_nodes_are_transparent),
              WPE_TEST_CASE(test_scene_set_parent_keep_world),
              WPE_TEST_CASE(test_scene_keep_world_nonuniform_parent_preserves_position),
              WPE_TEST_CASE(test_scene_keep_world_fails_cleanly_on_singular_parent),
              WPE_TEST_CASE(test_scene_despawning_parent_orphans_children),
              WPE_TEST_CASE(test_scene_destroy_recursive_removes_subtree_only),
              WPE_TEST_CASE(test_scene_destroy_recursive_deferred), WPE_TEST_CASE(test_scene_names),
              WPE_TEST_CASE(test_scene_deep_hierarchy_does_not_recurse))
