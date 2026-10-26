#include "wpe_test.h"

#include <stdio.h>
#include <time.h>

#include "physics/physics.h"
#include "scene/scene.h"

#define GRID 6
#define LAYERS 5
#define BODIES (GRID * GRID * LAYERS)

/* A pile of mixed boxes and spheres dropped in layers: nothing may tunnel through the ground or
 * blow up, and despawning half the bodies mid-simulation must release exactly those bodies.
 * (Sleeping is covered by the unit tests; a mixed pile keeps jostling for a long time.) */
static void test_stress_physics_pile(void) {
    WPE_Ecs* ecs = NULL;
    WPE_TEST_ASSERT_OK(wpe_ecs_create(&ecs));
    WPE_TEST_ASSERT_OK(wpe_scene_register_components(ecs));
    WPE_Physics* physics = NULL;
    WPE_TEST_ASSERT_OK(wpe_physics_create(
        &(WPE_PhysicsDesc){.ecs = ecs, .gravity = wpe_vec3(0.0f, -9.81f, 0.0f)}, &physics));

    const WPE_Entity ground = wpe_ecs_spawn(ecs);
    WPE_Transform t = wpe_transform_identity();
    t.position.y = -0.5f;
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, ground, WPE_COMP_TRANSFORM, &t, NULL));
    WPE_Collider floor = wpe_collider_default(WPE_COLLIDER_BOX);
    floor.size = wpe_vec3(100.0f, 1.0f, 100.0f);
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, ground, WPE_COMP_PHYSICS_COLLIDER, &floor, NULL));

    WPE_Entity bodies[BODIES];
    for (int i = 0; i < BODIES; ++i) {
        const int x = i % GRID, z = (i / GRID) % GRID, layer = i / (GRID * GRID);
        bodies[i] = wpe_ecs_spawn(ecs);
        t = wpe_transform_identity();
        /* Odd layers are offset by half a cell so the pile interlocks. */
        const float offset = (layer % 2) ? 0.55f : 0.0f;
        t.position = wpe_vec3((float)x * 1.1f + offset, 1.0f + (float)layer * 1.2f, (float)z * 1.1f + offset);
        WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, bodies[i], WPE_COMP_TRANSFORM, &t, NULL));
        const WPE_RigidBody body = wpe_rigidbody_default(WPE_BODY_DYNAMIC);
        WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, bodies[i], WPE_COMP_PHYSICS_RIGIDBODY, &body, NULL));
        WPE_Collider shape = wpe_collider_default(i % 3 ? WPE_COLLIDER_BOX : WPE_COLLIDER_SPHERE);
        shape.size.x = i % 3 ? 1.0f : 0.5f;
        WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, bodies[i], WPE_COMP_PHYSICS_COLLIDER, &shape, NULL));
    }

    const clock_t start = clock();
    const int steps = 300;
    for (int i = 0; i < steps; ++i)
        WPE_TEST_ASSERT_OK(wpe_physics_step(physics, 1.0f / 60.0f));
    const double seconds = (double)(clock() - start) / CLOCKS_PER_SEC;
    printf("    %d bodies: %d steps in %.2f s (%.2f ms/step)\n", BODIES, steps, seconds,
           seconds * 1000.0 / steps);
    WPE_TEST_ASSERT_EQ_UINT(BODIES + 1, wpe_physics_stats(physics).bodies);

    for (int i = 0; i < BODIES; ++i) {
        const WPE_Transform* body = wpe_ecs_get_component(ecs, bodies[i], WPE_COMP_TRANSFORM);
        WPE_TEST_ASSERT_MSG(body->position.y > 0.25f, "body %d sank to y = %f", i, (double)body->position.y);
        WPE_TEST_ASSERT(isfinite(body->position.x) && isfinite(body->position.z));
    }

    /* Wake the pile with a heavy sphere, then despawn half of it mid-flight. */
    const WPE_Entity ball = wpe_ecs_spawn(ecs);
    t = wpe_transform_identity();
    t.position = wpe_vec3(4.0f, 15.0f, 4.0f);
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, ball, WPE_COMP_TRANSFORM, &t, NULL));
    const WPE_RigidBody heavy = wpe_rigidbody_default(WPE_BODY_DYNAMIC);
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, ball, WPE_COMP_PHYSICS_RIGIDBODY, &heavy, NULL));
    WPE_Collider sphere = wpe_collider_default(WPE_COLLIDER_SPHERE);
    sphere.size.x = 1.5f;
    sphere.density = 20.0f;
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, ball, WPE_COMP_PHYSICS_COLLIDER, &sphere, NULL));
    for (int i = 0; i < 90; ++i)
        WPE_TEST_ASSERT_OK(wpe_physics_step(physics, 1.0f / 60.0f));
    WPE_TEST_ASSERT(wpe_physics_stats(physics).awake_bodies > 0);
    for (int i = 0; i < BODIES; i += 2)
        WPE_TEST_ASSERT_OK(wpe_ecs_despawn(ecs, bodies[i]));
    for (int i = 0; i < 60; ++i)
        WPE_TEST_ASSERT_OK(wpe_physics_step(physics, 1.0f / 60.0f));
    WPE_TEST_ASSERT_EQ_UINT(BODIES / 2 + 2, wpe_physics_stats(physics).bodies);

    wpe_physics_destroy(physics);
    wpe_ecs_destroy(ecs);
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_stress_physics_pile))
