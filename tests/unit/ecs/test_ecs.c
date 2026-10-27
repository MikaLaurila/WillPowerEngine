#include "wpe_test.h"

#include "ecs/ecs.h"

typedef struct Position {
    float x, y, z;
} Position;

typedef struct Velocity {
    float dx, dy, dz;
} Velocity;

typedef struct Tag {
    uint32_t value;
} Tag;

enum {
    COMP_POSITION = WPE_COMP_USER_FIRST,
    COMP_VELOCITY,
    COMP_TAG,
};

typedef struct HookLog {
    int adds;
    int removes;
    WPE_Entity last_entity;
    uint32_t last_value;
} HookLog;

static void tag_on_add(WPE_Ecs* ecs, WPE_Entity entity, void* component, void* user_data) {
    (void)ecs;
    HookLog* log = user_data;
    ++log->adds;
    log->last_entity = entity;
    log->last_value = ((const Tag*)component)->value;
}

static void tag_on_remove(WPE_Ecs* ecs, WPE_Entity entity, void* component, void* user_data) {
    (void)ecs;
    HookLog* log = user_data;
    ++log->removes;
    log->last_entity = entity;
    log->last_value = ((const Tag*)component)->value;
}

/* Creates a world with Position, Velocity and Tag registered; Tag reports hooks into `log`. */
static WPE_Ecs* make_world(HookLog* log) {
    WPE_Ecs* ecs = NULL;
    WPE_TEST_ASSERT_OK(wpe_ecs_create(&ecs));
    WPE_TEST_ASSERT_OK(wpe_ecs_register_component(
        ecs, COMP_POSITION,
        &(WPE_ComponentDesc){.name = "Position", .size = sizeof(Position), .alignment = alignof(Position)}));
    WPE_TEST_ASSERT_OK(wpe_ecs_register_component(
        ecs, COMP_VELOCITY,
        &(WPE_ComponentDesc){.name = "Velocity", .size = sizeof(Velocity), .alignment = alignof(Velocity)}));
    WPE_TEST_ASSERT_OK(
        wpe_ecs_register_component(ecs, COMP_TAG,
                                   &(WPE_ComponentDesc){.name = "Tag",
                                                        .size = sizeof(Tag),
                                                        .alignment = alignof(Tag),
                                                        .on_add = log ? tag_on_add : NULL,
                                                        .on_remove = log ? tag_on_remove : NULL,
                                                        .hook_user_data = log}));
    return ecs;
}

/* ---- World and registration --------------------------------------------------------------------- */

static void test_ecs_create_and_destroy_empty(void) {
    WPE_Ecs* ecs = NULL;
    WPE_TEST_ASSERT_OK(wpe_ecs_create(&ecs));
    WPE_TEST_ASSERT(ecs != NULL);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_ecs_entity_count(ecs));
    wpe_ecs_destroy(ecs);
    wpe_ecs_destroy(NULL);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NULL_ARG, wpe_ecs_create(NULL));
}

static void test_ecs_register_validates_descriptors(void) {
    WPE_Ecs* ecs = make_world(NULL);
    wpe_log_set_stderr_enabled(false);

    const WPE_ComponentDesc good = {.name = "Good", .size = 8, .alignment = 4};
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG,
                           wpe_ecs_register_component(ecs, WPE_COMP_NONE, &good));
    WPE_TEST_ASSERT_EQ_INT(
        WPE_RESULT_ERROR_INVALID_ARG,
        wpe_ecs_register_component(ecs, (WPE_ComponentType)WPE_ECS_MAX_COMPONENT_TYPES, &good));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_ALREADY_EXISTS,
                           wpe_ecs_register_component(ecs, COMP_POSITION, &good));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NULL_ARG, wpe_ecs_register_component(ecs, 40, NULL));

    const WPE_ComponentDesc bad[] = {
        {.name = NULL, .size = 8, .alignment = 4},
        {.name = "", .size = 8, .alignment = 4},
        {.name = "ZeroSize", .size = 0, .alignment = 4},
        {.name = "ZeroAlign", .size = 8, .alignment = 0},
        {.name = "NonPow2", .size = 12, .alignment = 3},
        {.name = "TooAligned", .size = 256, .alignment = 256},
        {.name = "SizeNotMultiple", .size = 6, .alignment = 4},
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_ecs_register_component(ecs, 40, &bad[i]));
    }
    WPE_TEST_ASSERT(!wpe_ecs_is_registered(ecs, 40));

    wpe_log_set_stderr_enabled(true);
    WPE_TEST_ASSERT_EQ_STR("Position", wpe_ecs_component_name(ecs, COMP_POSITION));
    WPE_TEST_ASSERT(wpe_ecs_component_name(ecs, 40) == NULL);
    WPE_TEST_ASSERT_EQ_UINT(sizeof(Position), wpe_ecs_component_size(ecs, COMP_POSITION));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_ecs_component_size(ecs, 40));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_ecs_component_size(NULL, COMP_POSITION));

    /* Spawn serials order entities by creation, even when a later entity reuses an older slot. */
    const WPE_Entity first = wpe_ecs_spawn(ecs);
    const WPE_Entity second = wpe_ecs_spawn(ecs);
    WPE_TEST_ASSERT(wpe_ecs_spawn_serial(ecs, first) < wpe_ecs_spawn_serial(ecs, second));
    WPE_TEST_ASSERT_OK(wpe_ecs_despawn(ecs, first));
    const WPE_Entity third = wpe_ecs_spawn(ecs); /* Takes the first one's slot. */
    WPE_TEST_ASSERT(wpe_ecs_spawn_serial(ecs, second) < wpe_ecs_spawn_serial(ecs, third));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_ecs_spawn_serial(ecs, first));
    WPE_TEST_ASSERT_OK(wpe_ecs_despawn(ecs, second));
    WPE_TEST_ASSERT_OK(wpe_ecs_despawn(ecs, third));
    wpe_ecs_destroy(ecs);
}

static void test_ecs_register_truncates_long_names(void) {
    WPE_Ecs* ecs = make_world(NULL);
    const char* long_name = "AVeryLongComponentNameThatExceedsTheLimitByQuiteABit";
    WPE_TEST_ASSERT_OK(wpe_ecs_register_component(
        ecs, 40, &(WPE_ComponentDesc){.name = long_name, .size = 4, .alignment = 4}));
    WPE_TEST_ASSERT_EQ_UINT(WPE_ECS_COMPONENT_NAME_MAX - 1, strlen(wpe_ecs_component_name(ecs, 40)));
    wpe_ecs_destroy(ecs);
}

/* ---- Entities ----------------------------------------------------------------------------------- */

static void test_ecs_spawn_returns_unique_live_entities(void) {
    WPE_Ecs* ecs = make_world(NULL);
    WPE_Entity entities[100];
    for (int i = 0; i < 100; ++i) {
        entities[i] = wpe_ecs_spawn(ecs);
        WPE_TEST_ASSERT(entities[i] != WPE_ENTITY_NULL);
        WPE_TEST_ASSERT(wpe_ecs_is_alive(ecs, entities[i]));
        for (int j = 0; j < i; ++j)
            WPE_TEST_ASSERT(entities[i] != entities[j]);
    }
    WPE_TEST_ASSERT_EQ_UINT(100, wpe_ecs_entity_count(ecs));
    WPE_TEST_ASSERT(!wpe_ecs_is_alive(ecs, WPE_ENTITY_NULL));
    wpe_ecs_destroy(ecs);
}

static void test_ecs_despawn_invalidates_handle_and_reuses_slot(void) {
    WPE_Ecs* ecs = make_world(NULL);
    const WPE_Entity a = wpe_ecs_spawn(ecs);
    WPE_TEST_ASSERT_OK(wpe_ecs_despawn(ecs, a));
    WPE_TEST_ASSERT(!wpe_ecs_is_alive(ecs, a));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND, wpe_ecs_despawn(ecs, a));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_ecs_entity_count(ecs));

    /* The slot is reused with a new generation; the stale handle must not alias the new entity. */
    const WPE_Entity b = wpe_ecs_spawn(ecs);
    WPE_TEST_ASSERT(b != a);
    WPE_TEST_ASSERT_EQ_UINT(a & (WPE_ECS_MAX_ENTITIES - 1), b & (WPE_ECS_MAX_ENTITIES - 1));
    WPE_TEST_ASSERT(!wpe_ecs_is_alive(ecs, a));
    WPE_TEST_ASSERT(wpe_ecs_is_alive(ecs, b));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND, wpe_ecs_despawn(ecs, WPE_ENTITY_NULL));
    wpe_ecs_destroy(ecs);
}

static void test_ecs_exhausted_slot_is_retired(void) {
    WPE_Ecs* ecs = make_world(NULL);
    const WPE_Entity first = wpe_ecs_spawn(ecs);
    const uint32_t index = first & (WPE_ECS_MAX_ENTITIES - 1);

    /* LIFO reuse keeps hitting the same slot until its generation is exhausted. */
    WPE_Entity e = first;
    for (uint32_t generation = 1; generation < WPE_ECS_MAX_GENERATION; ++generation) {
        WPE_TEST_ASSERT_OK(wpe_ecs_despawn(ecs, e));
        e = wpe_ecs_spawn(ecs);
        WPE_TEST_ASSERT_EQ_UINT(index, e & (WPE_ECS_MAX_ENTITIES - 1));
    }
    WPE_TEST_ASSERT_EQ_UINT(WPE_ECS_MAX_GENERATION, e >> WPE_ECS_ENTITY_INDEX_BITS);

    WPE_TEST_ASSERT_OK(wpe_ecs_despawn(ecs, e));
    const WPE_Entity fresh = wpe_ecs_spawn(ecs);
    WPE_TEST_ASSERT(fresh != WPE_ENTITY_NULL);
    WPE_TEST_ASSERT((fresh & (WPE_ECS_MAX_ENTITIES - 1)) != index);
    WPE_TEST_ASSERT(!wpe_ecs_is_alive(ecs, first));
    wpe_ecs_destroy(ecs);
}

static void test_ecs_max_entities_limit(void) {
    WPE_Ecs* ecs = make_world(NULL);
    for (uint32_t i = 0; i < WPE_ECS_MAX_ENTITIES; ++i) {
        if (wpe_ecs_spawn(ecs) == WPE_ENTITY_NULL) WPE_TEST_ASSERT_MSG(false, "spawn %u failed", i);
    }
    WPE_TEST_ASSERT_EQ_UINT(WPE_ECS_MAX_ENTITIES, wpe_ecs_entity_count(ecs));

    wpe_log_set_stderr_enabled(false);
    const WPE_Entity overflow = wpe_ecs_spawn(ecs);
    wpe_log_set_stderr_enabled(true);
    WPE_TEST_ASSERT(overflow == WPE_ENTITY_NULL);
    wpe_ecs_destroy(ecs);
}

/* ---- Components ----------------------------------------------------------------------------------- */

static void test_ecs_entity_with_zero_components(void) {
    WPE_Ecs* ecs = make_world(NULL);
    const WPE_Entity e = wpe_ecs_spawn(ecs);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_ecs_component_mask(ecs, e));
    WPE_TEST_ASSERT(wpe_ecs_get_component(ecs, e, COMP_POSITION) == NULL);
    WPE_TEST_ASSERT(!wpe_ecs_has_component(ecs, e, COMP_POSITION));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND, wpe_ecs_remove_component(ecs, e, COMP_POSITION));
    WPE_TEST_ASSERT_OK(wpe_ecs_despawn(ecs, e));
    wpe_ecs_destroy(ecs);
}

static void test_ecs_add_get_remove_component(void) {
    WPE_Ecs* ecs = make_world(NULL);
    const WPE_Entity e = wpe_ecs_spawn(ecs);

    Position* stored = NULL;
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, e, COMP_POSITION, &(Position){1, 2, 3}, (void**)&stored));
    WPE_TEST_ASSERT(stored != NULL);
    WPE_TEST_ASSERT(wpe_ecs_has_component(ecs, e, COMP_POSITION));
    const Position* fetched = wpe_ecs_get_component(ecs, e, COMP_POSITION);
    WPE_TEST_ASSERT(fetched == stored);
    WPE_TEST_ASSERT_NEAR(2.0, fetched->y, 0);

    Velocity* zeroed = NULL;
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, e, COMP_VELOCITY, NULL, (void**)&zeroed));
    WPE_TEST_ASSERT(zeroed->dx == 0 && zeroed->dy == 0 && zeroed->dz == 0);
    WPE_TEST_ASSERT_EQ_UINT(WPE_ECS_MASK(COMP_POSITION) | WPE_ECS_MASK(COMP_VELOCITY),
                            wpe_ecs_component_mask(ecs, e));

    WPE_TEST_ASSERT_OK(wpe_ecs_remove_component(ecs, e, COMP_POSITION));
    WPE_TEST_ASSERT(!wpe_ecs_has_component(ecs, e, COMP_POSITION));
    WPE_TEST_ASSERT(wpe_ecs_has_component(ecs, e, COMP_VELOCITY));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_ecs_component_count(ecs, COMP_POSITION));
    wpe_ecs_destroy(ecs);
}

static void test_ecs_add_component_error_cases(void) {
    WPE_Ecs* ecs = make_world(NULL);
    const WPE_Entity e = wpe_ecs_spawn(ecs);
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, e, COMP_POSITION, NULL, NULL));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_ALREADY_EXISTS,
                           wpe_ecs_add_component(ecs, e, COMP_POSITION, NULL, NULL));

    wpe_log_set_stderr_enabled(false);
    void* out = (void*)&out;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_ecs_add_component(ecs, e, 50, NULL, &out));
    WPE_TEST_ASSERT(out == NULL);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG,
                           wpe_ecs_add_component(ecs, e, WPE_COMP_NONE, NULL, NULL));
    wpe_log_set_stderr_enabled(true);

    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_ecs_remove_component(ecs, e, 50));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NULL_ARG,
                           wpe_ecs_add_component(NULL, e, COMP_POSITION, NULL, NULL));

    WPE_TEST_ASSERT_OK(wpe_ecs_despawn(ecs, e));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND,
                           wpe_ecs_add_component(ecs, e, COMP_VELOCITY, NULL, NULL));
    WPE_TEST_ASSERT(wpe_ecs_get_component(ecs, e, COMP_POSITION) == NULL);
    WPE_TEST_ASSERT(!wpe_ecs_has_component(ecs, e, (WPE_ComponentType)200));
    wpe_ecs_destroy(ecs);
}

static void test_ecs_swap_remove_keeps_other_components_intact(void) {
    WPE_Ecs* ecs = make_world(NULL);
    WPE_Entity entities[50];
    for (int i = 0; i < 50; ++i) {
        entities[i] = wpe_ecs_spawn(ecs);
        WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, entities[i], COMP_TAG, &(Tag){(uint32_t)i}, NULL));
    }
    for (int i = 0; i < 50; i += 3)
        WPE_TEST_ASSERT_OK(wpe_ecs_remove_component(ecs, entities[i], COMP_TAG));
    for (int i = 0; i < 50; ++i) {
        const Tag* tag = wpe_ecs_get_component(ecs, entities[i], COMP_TAG);
        if (i % 3 == 0) {
            WPE_TEST_ASSERT(tag == NULL);
        } else {
            WPE_TEST_ASSERT(tag != NULL);
            WPE_TEST_ASSERT_EQ_UINT((uint32_t)i, tag->value);
        }
    }
    wpe_ecs_destroy(ecs);
}

static void test_ecs_despawn_removes_all_components(void) {
    WPE_Ecs* ecs = make_world(NULL);
    const WPE_Entity a = wpe_ecs_spawn(ecs);
    const WPE_Entity b = wpe_ecs_spawn(ecs);
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, a, COMP_POSITION, &(Position){1, 1, 1}, NULL));
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, a, COMP_VELOCITY, NULL, NULL));
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, b, COMP_POSITION, &(Position){2, 2, 2}, NULL));

    WPE_TEST_ASSERT_OK(wpe_ecs_despawn(ecs, a));
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_ecs_component_count(ecs, COMP_POSITION));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_ecs_component_count(ecs, COMP_VELOCITY));
    const Position* pb = wpe_ecs_get_component(ecs, b, COMP_POSITION);
    WPE_TEST_ASSERT_NEAR(2.0, pb->x, 0);

    /* A new entity in the recycled slot starts with no components. */
    const WPE_Entity c = wpe_ecs_spawn(ecs);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_ecs_component_mask(ecs, c));
    wpe_ecs_destroy(ecs);
}

static void test_ecs_hooks_run_on_add_remove_despawn_and_destroy(void) {
    HookLog log = {0};
    WPE_Ecs* ecs = make_world(&log);
    const WPE_Entity a = wpe_ecs_spawn(ecs);
    const WPE_Entity b = wpe_ecs_spawn(ecs);
    const WPE_Entity c = wpe_ecs_spawn(ecs);

    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, a, COMP_TAG, &(Tag){7}, NULL));
    WPE_TEST_ASSERT_EQ_INT(1, log.adds);
    WPE_TEST_ASSERT_EQ_UINT(7, log.last_value);
    WPE_TEST_ASSERT(log.last_entity == a);

    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, b, COMP_TAG, &(Tag){8}, NULL));
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, c, COMP_TAG, &(Tag){9}, NULL));

    WPE_TEST_ASSERT_OK(wpe_ecs_remove_component(ecs, a, COMP_TAG));
    WPE_TEST_ASSERT_EQ_INT(1, log.removes);
    WPE_TEST_ASSERT_EQ_UINT(7, log.last_value);

    WPE_TEST_ASSERT_OK(wpe_ecs_despawn(ecs, b));
    WPE_TEST_ASSERT_EQ_INT(2, log.removes);
    WPE_TEST_ASSERT_EQ_UINT(8, log.last_value);

    wpe_ecs_destroy(ecs);
    WPE_TEST_ASSERT_EQ_INT(3, log.removes);
    WPE_TEST_ASSERT_EQ_UINT(9, log.last_value);
}

static WPE_Result g_hook_result;
static WPE_Entity g_hook_victim;

static void structural_change_hook(WPE_Ecs* ecs, WPE_Entity entity, void* component, void* user_data) {
    (void)entity;
    (void)component;
    (void)user_data;
    WPE_TEST_EXPECT_ASSERT(g_hook_result = wpe_ecs_despawn(ecs, g_hook_victim));
    WPE_TEST_EXPECT_ASSERT(wpe_ecs_spawn(ecs));
    /* Deferred despawn is allowed from hooks. */
    g_hook_result = wpe_ecs_despawn_deferred(ecs, g_hook_victim);
}

static void test_ecs_hooks_cannot_make_structural_changes(void) {
    WPE_TEST_SKIP_IF_NO_ASSERTS();
    WPE_Ecs* ecs = NULL;
    WPE_TEST_ASSERT_OK(wpe_ecs_create(&ecs));
    WPE_TEST_ASSERT_OK(wpe_ecs_register_component(ecs, COMP_TAG,
                                                  &(WPE_ComponentDesc){.name = "Tag",
                                                                       .size = sizeof(Tag),
                                                                       .alignment = alignof(Tag),
                                                                       .on_add = structural_change_hook}));

    g_hook_victim = wpe_ecs_spawn(ecs);
    const WPE_Entity e = wpe_ecs_spawn(ecs);
    wpe_log_set_stderr_enabled(false);
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, e, COMP_TAG, NULL, NULL));
    wpe_log_set_stderr_enabled(true);

    WPE_TEST_ASSERT_OK(g_hook_result);
    WPE_TEST_ASSERT(wpe_ecs_is_alive(ecs, g_hook_victim));
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_ecs_flush_deferred(ecs));
    WPE_TEST_ASSERT(!wpe_ecs_is_alive(ecs, g_hook_victim));
    wpe_ecs_destroy(ecs);
}

static void test_ecs_pool_view_is_contiguous(void) {
    WPE_Ecs* ecs = make_world(NULL);
    for (uint32_t i = 0; i < 10; ++i) {
        const WPE_Entity e = wpe_ecs_spawn(ecs);
        WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, e, COMP_TAG, &(Tag){i * 10}, NULL));
    }

    WPE_EcsPoolView view;
    WPE_TEST_ASSERT_OK(wpe_ecs_pool_view(ecs, COMP_TAG, &view));
    WPE_TEST_ASSERT_EQ_UINT(10, view.count);
    WPE_TEST_ASSERT_EQ_UINT(sizeof(Tag), view.stride);
    uint32_t sum = 0;
    for (uint32_t i = 0; i < view.count; ++i) {
        const Tag* tag = (const Tag*)((const unsigned char*)view.data + (size_t)i * view.stride);
        WPE_TEST_ASSERT(tag == wpe_ecs_get_component(ecs, view.entities[i], COMP_TAG));
        sum += tag->value;
    }
    WPE_TEST_ASSERT_EQ_UINT(450, sum);

    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_ecs_pool_view(ecs, 50, &view));
    WPE_TEST_ASSERT_EQ_UINT(0, view.count);
    wpe_ecs_destroy(ecs);
}

/* ---- Queries ------------------------------------------------------------------------------------ */

static void test_ecs_query_all_and_none(void) {
    WPE_Ecs* ecs = make_world(NULL);
    /* i%2: position, i%3: velocity, i%5: tag */
    for (uint32_t i = 0; i < 30; ++i) {
        const WPE_Entity e = wpe_ecs_spawn(ecs);
        if (i % 2 == 0)
            WPE_TEST_ASSERT_OK(
                wpe_ecs_add_component(ecs, e, COMP_POSITION, &(Position){(float)i, 0, 0}, NULL));
        if (i % 3 == 0) WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, e, COMP_VELOCITY, NULL, NULL));
        if (i % 5 == 0) WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, e, COMP_TAG, NULL, NULL));
    }

    /* Position and velocity (i % 6 == 0) but no tag (i % 30 != 0): 6, 12, 18, 24. */
    WPE_EcsIter it =
        wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(COMP_POSITION) | WPE_ECS_MASK(COMP_VELOCITY),
                                          .none = WPE_ECS_MASK(COMP_TAG)});
    int count = 0;
    float sum = 0;
    while (wpe_ecs_iter_next(&it)) {
        const Position* p = wpe_ecs_iter_get(&it, COMP_POSITION);
        WPE_TEST_ASSERT(p != NULL);
        WPE_TEST_ASSERT(wpe_ecs_iter_get(&it, COMP_VELOCITY) != NULL);
        WPE_TEST_ASSERT(wpe_ecs_iter_get(&it, COMP_TAG) == NULL);
        sum += p->x;
        ++count;
    }
    WPE_TEST_ASSERT_EQ_INT(4, count);
    WPE_TEST_ASSERT_NEAR(60.0, sum, 0);
    WPE_TEST_ASSERT(!wpe_ecs_iter_next(&it));
    WPE_TEST_ASSERT(it.entity == WPE_ENTITY_NULL);
    wpe_ecs_destroy(ecs);
}

static void test_ecs_query_empty_mask_visits_all_alive(void) {
    WPE_Ecs* ecs = make_world(NULL);
    WPE_Entity entities[10];
    for (int i = 0; i < 10; ++i)
        entities[i] = wpe_ecs_spawn(ecs);
    WPE_TEST_ASSERT_OK(wpe_ecs_despawn(ecs, entities[3]));
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, entities[5], COMP_TAG, NULL, NULL));

    WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){0});
    int count = 0;
    while (wpe_ecs_iter_next(&it)) {
        WPE_TEST_ASSERT(it.entity != entities[3]);
        ++count;
    }
    WPE_TEST_ASSERT_EQ_INT(9, count);

    it = wpe_ecs_query(ecs, (WPE_EcsQuery){.none = WPE_ECS_MASK(COMP_TAG)});
    count = 0;
    while (wpe_ecs_iter_next(&it))
        ++count;
    WPE_TEST_ASSERT_EQ_INT(8, count);
    wpe_ecs_destroy(ecs);
}

static void test_ecs_query_unregistered_type_matches_nothing(void) {
    WPE_Ecs* ecs = make_world(NULL);
    const WPE_Entity e = wpe_ecs_spawn(ecs);
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, e, COMP_POSITION, NULL, NULL));
    WPE_EcsIter it =
        wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(COMP_POSITION) | WPE_ECS_MASK(50)});
    WPE_TEST_ASSERT(!wpe_ecs_iter_next(&it));
    wpe_ecs_destroy(ecs);
}

static void test_ecs_query_allows_adds_and_skips_new_entries(void) {
    WPE_Ecs* ecs = make_world(NULL);
    for (int i = 0; i < 20; ++i)
        WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, wpe_ecs_spawn(ecs), COMP_TAG, NULL, NULL));
    /* Free a slot so spawning during the slot scan reuses an index below the scan's end. */
    const WPE_Entity spare = wpe_ecs_spawn(ecs);
    WPE_TEST_ASSERT_OK(wpe_ecs_despawn(ecs, spare));

    WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(COMP_TAG)});
    int visited = 0;
    while (wpe_ecs_iter_next(&it)) {
        ++visited;
        /* Spawning and adding force pool growth while iterating. */
        WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, wpe_ecs_spawn(ecs), COMP_TAG, NULL, NULL));
        WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, it.entity, COMP_POSITION, NULL, NULL));
        Tag* tag = wpe_ecs_iter_get(&it, COMP_TAG);
        tag->value = 1;
    }
    WPE_TEST_ASSERT_EQ_INT(20, visited);
    WPE_TEST_ASSERT_EQ_UINT(40, wpe_ecs_component_count(ecs, COMP_TAG));

    it = wpe_ecs_query(ecs, (WPE_EcsQuery){0});
    const uint32_t before = wpe_ecs_entity_count(ecs);
    visited = 0;
    while (wpe_ecs_iter_next(&it)) {
        ++visited;
        WPE_TEST_ASSERT(wpe_ecs_spawn(ecs) != WPE_ENTITY_NULL);
    }
    WPE_TEST_ASSERT_EQ_UINT(before, (uint32_t)visited);
    wpe_ecs_destroy(ecs);
}

static void test_ecs_query_detects_removal_during_iteration(void) {
    WPE_TEST_SKIP_IF_NO_ASSERTS();
    WPE_Ecs* ecs = make_world(NULL);
    for (int i = 0; i < 5; ++i)
        WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, wpe_ecs_spawn(ecs), COMP_TAG, NULL, NULL));

    WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(COMP_TAG)});
    WPE_TEST_ASSERT(wpe_ecs_iter_next(&it));
    WPE_TEST_ASSERT_OK(wpe_ecs_despawn(ecs, it.entity));

    wpe_log_set_stderr_enabled(false);
    WPE_TEST_EXPECT_ASSERT(wpe_ecs_iter_next(&it));
    wpe_log_set_stderr_enabled(true);
    wpe_ecs_destroy(ecs);
}

static void test_ecs_deferred_despawn_during_iteration(void) {
    WPE_Ecs* ecs = make_world(NULL);
    WPE_Entity entities[10];
    for (uint32_t i = 0; i < 10; ++i) {
        entities[i] = wpe_ecs_spawn(ecs);
        WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, entities[i], COMP_TAG, &(Tag){i}, NULL));
    }

    WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(COMP_TAG)});
    int visited = 0;
    while (wpe_ecs_iter_next(&it)) {
        const Tag* tag = wpe_ecs_iter_get(&it, COMP_TAG);
        if (tag->value % 2 == 0) {
            WPE_TEST_ASSERT_OK(wpe_ecs_despawn_deferred(ecs, it.entity));
            WPE_TEST_ASSERT_OK(wpe_ecs_despawn_deferred(ecs, it.entity)); /* Duplicates are harmless. */
        }
        ++visited;
    }
    WPE_TEST_ASSERT_EQ_INT(10, visited);
    WPE_TEST_ASSERT_EQ_UINT(10, wpe_ecs_entity_count(ecs));
    WPE_TEST_ASSERT_EQ_UINT(5, wpe_ecs_flush_deferred(ecs));
    WPE_TEST_ASSERT_EQ_UINT(5, wpe_ecs_entity_count(ecs));
    for (uint32_t i = 0; i < 10; ++i)
        WPE_TEST_ASSERT(wpe_ecs_is_alive(ecs, entities[i]) == (i % 2 == 1));

    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND, wpe_ecs_despawn_deferred(ecs, entities[0]));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_ecs_flush_deferred(ecs));
    wpe_ecs_destroy(ecs);
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_ecs_create_and_destroy_empty),
              WPE_TEST_CASE(test_ecs_register_validates_descriptors),
              WPE_TEST_CASE(test_ecs_register_truncates_long_names),
              WPE_TEST_CASE(test_ecs_spawn_returns_unique_live_entities),
              WPE_TEST_CASE(test_ecs_despawn_invalidates_handle_and_reuses_slot),
              WPE_TEST_CASE(test_ecs_exhausted_slot_is_retired), WPE_TEST_CASE(test_ecs_max_entities_limit),
              WPE_TEST_CASE(test_ecs_entity_with_zero_components),
              WPE_TEST_CASE(test_ecs_add_get_remove_component),
              WPE_TEST_CASE(test_ecs_add_component_error_cases),
              WPE_TEST_CASE(test_ecs_swap_remove_keeps_other_components_intact),
              WPE_TEST_CASE(test_ecs_despawn_removes_all_components),
              WPE_TEST_CASE(test_ecs_hooks_run_on_add_remove_despawn_and_destroy),
              WPE_TEST_CASE(test_ecs_hooks_cannot_make_structural_changes),
              WPE_TEST_CASE(test_ecs_pool_view_is_contiguous), WPE_TEST_CASE(test_ecs_query_all_and_none),
              WPE_TEST_CASE(test_ecs_query_empty_mask_visits_all_alive),
              WPE_TEST_CASE(test_ecs_query_unregistered_type_matches_nothing),
              WPE_TEST_CASE(test_ecs_query_allows_adds_and_skips_new_entries),
              WPE_TEST_CASE(test_ecs_query_detects_removal_during_iteration),
              WPE_TEST_CASE(test_ecs_deferred_despawn_during_iteration))
