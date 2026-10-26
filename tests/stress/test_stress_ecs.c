#include "wpe_test.h"

#include <stdio.h>
#include <time.h>

#include "scene/scene.h"

/* Deterministic xorshift32 so failures are reproducible. */
static uint32_t g_rng_state = 0x12345678u;

static uint32_t rng_next(void) {
    uint32_t x = g_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return g_rng_state = x;
}

static double now_ms(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

typedef struct Payload {
    uint32_t owner; /* Model slot that owns this component; validates data integrity. */
    uint32_t serial;
} Payload;

enum { COMP_A = WPE_COMP_USER_FIRST, COMP_B, COMP_C, COMP_TYPES = 3 };
enum { MODEL_SLOTS = 4096, OPERATIONS = 300000 };

/* Reference model mirroring what the ECS should contain. */
typedef struct ModelEntity {
    WPE_Entity entity;
    bool alive;
    bool has[COMP_TYPES];
    uint32_t serial[COMP_TYPES];
} ModelEntity;

static ModelEntity g_model[MODEL_SLOTS];

static void verify_model(WPE_Ecs* ecs) {
    uint32_t alive = 0, counts[COMP_TYPES] = {0};
    for (uint32_t i = 0; i < MODEL_SLOTS; ++i) {
        const ModelEntity* m = &g_model[i];
        if (!m->alive) continue;
        ++alive;
        WPE_TEST_ASSERT(wpe_ecs_is_alive(ecs, m->entity));
        for (int t = 0; t < COMP_TYPES; ++t) {
            const Payload* p = wpe_ecs_get_component(ecs, m->entity, COMP_A + t);
            WPE_TEST_ASSERT(m->has[t] == (p != NULL));
            if (p) {
                WPE_TEST_ASSERT_EQ_UINT(i, p->owner);
                WPE_TEST_ASSERT_EQ_UINT(m->serial[t], p->serial);
                ++counts[t];
            }
        }
    }
    WPE_TEST_ASSERT_EQ_UINT(alive, wpe_ecs_entity_count(ecs));
    for (int t = 0; t < COMP_TYPES; ++t)
        WPE_TEST_ASSERT_EQ_UINT(counts[t], wpe_ecs_component_count(ecs, COMP_A + t));
}

static void test_stress_ecs_random_operations_match_model(void) {
    WPE_Ecs* ecs = NULL;
    WPE_TEST_ASSERT_OK(wpe_ecs_create(&ecs));
    for (int t = 0; t < COMP_TYPES; ++t) {
        WPE_TEST_ASSERT_OK(wpe_ecs_register_component(
            ecs, COMP_A + t,
            &(WPE_ComponentDesc){.name = "Payload", .size = sizeof(Payload), .alignment = alignof(Payload)}));
    }
    memset(g_model, 0, sizeof g_model);

    uint32_t serial = 0;
    for (uint32_t op = 0; op < OPERATIONS; ++op) {
        const uint32_t slot = rng_next() % MODEL_SLOTS;
        ModelEntity* m = &g_model[slot];
        const int t = (int)(rng_next() % COMP_TYPES);

        if (!m->alive) {
            m->entity = wpe_ecs_spawn(ecs);
            WPE_TEST_ASSERT(m->entity != WPE_ENTITY_NULL);
            m->alive = true;
            memset(m->has, 0, sizeof m->has);
            continue;
        }

        switch (rng_next() % 4) {
        case 0:
            WPE_TEST_ASSERT_OK(wpe_ecs_despawn(ecs, m->entity));
            WPE_TEST_ASSERT(!wpe_ecs_is_alive(ecs, m->entity));
            m->alive = false;
            break;
        case 1:
        case 2:
            if (m->has[t]) {
                WPE_TEST_ASSERT_OK(wpe_ecs_remove_component(ecs, m->entity, COMP_A + t));
                m->has[t] = false;
            } else {
                const Payload payload = {.owner = slot, .serial = ++serial};
                WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, m->entity, COMP_A + t, &payload, NULL));
                m->has[t] = true;
                m->serial[t] = payload.serial;
            }
            break;
        default: {
            Payload* p = wpe_ecs_get_component(ecs, m->entity, COMP_A + t);
            WPE_TEST_ASSERT(m->has[t] == (p != NULL));
            if (p) p->serial = m->serial[t] = ++serial;
            break;
        }
        }

        if (op % 50000 == 0) verify_model(ecs);
    }
    verify_model(ecs);

    /* A query over A and B must visit exactly the model's matching entities. */
    uint32_t expected = 0;
    for (uint32_t i = 0; i < MODEL_SLOTS; ++i)
        expected += g_model[i].alive && g_model[i].has[0] && g_model[i].has[1];
    uint32_t visited = 0;
    WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(COMP_A) | WPE_ECS_MASK(COMP_B)});
    while (wpe_ecs_iter_next(&it)) {
        const Payload* a = wpe_ecs_iter_get(&it, COMP_A);
        WPE_TEST_ASSERT(g_model[a->owner].entity == it.entity);
        ++visited;
    }
    WPE_TEST_ASSERT_EQ_UINT(expected, visited);
    wpe_ecs_destroy(ecs);
}

static void test_stress_scene_10k_transform_update(void) {
    WPE_Ecs* ecs = NULL;
    WPE_TEST_ASSERT_OK(wpe_ecs_create(&ecs));
    WPE_TEST_ASSERT_OK(wpe_scene_register_components(ecs));

    /* 100 roots with 99 children each: 10,000 transformed entities. */
    enum { ROOTS = 100, CHILDREN = 99 };
    for (int r = 0; r < ROOTS; ++r) {
        const WPE_Entity root = wpe_ecs_spawn(ecs);
        WPE_Transform t = wpe_transform_identity();
        t.position = wpe_vec3((float)r, 0, 0);
        t.rotation = wpe_quat_from_euler_xyz(wpe_vec3(0.01f * (float)r, 0.2f, 0.3f));
        WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, root, WPE_COMP_TRANSFORM, &t, NULL));
        for (int c = 0; c < CHILDREN; ++c) {
            const WPE_Entity child = wpe_ecs_spawn(ecs);
            t.position = wpe_vec3(0, (float)c, 0);
            WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, child, WPE_COMP_TRANSFORM, &t, NULL));
            WPE_TEST_ASSERT_OK(wpe_scene_set_parent(ecs, child, root, false));
        }
    }
    WPE_TEST_ASSERT_EQ_UINT(ROOTS * (CHILDREN + 1), wpe_ecs_component_count(ecs, WPE_COMP_TRANSFORM));

    enum { FRAMES = 20 };
    double best = 1e30;
    for (int frame = 0; frame < FRAMES; ++frame) {
        const double start = now_ms();
        WPE_TEST_ASSERT_OK(wpe_scene_update_transforms(ecs));
        const double elapsed = now_ms() - start;
        if (elapsed < best) best = elapsed;
    }
    fprintf(stderr, "    10k transform update: best %.3f ms over %d frames\n", best, FRAMES);

#if !defined(WPE_DEBUG)
    /* Spec budget: a whole frame is 16 ms; transform propagation must be a small fraction of it. */
    WPE_TEST_ASSERT_MSG(best < 4.0, "10k transform update took %.3f ms (budget 4 ms)", best);
#endif
    wpe_ecs_destroy(ecs);
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_stress_ecs_random_operations_match_model),
              WPE_TEST_CASE(test_stress_scene_10k_transform_update))
