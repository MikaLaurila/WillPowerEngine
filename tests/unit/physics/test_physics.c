#include "wpe_test.h"
#include "wpe_test_assets.h"

#include <string.h>

#include "assets/procedural.h"
#include "physics/physics.h"
#include "scene/scene.h"

#define DT (1.0f / 60.0f)
#define GRAVITY wpe_vec3(0.0f, -9.81f, 0.0f)

typedef struct Fixture {
    WPE_TestAssets assets; /* For convex and mesh colliders. */
    WPE_Ecs* ecs;
    WPE_Physics* physics;
} Fixture;

static Fixture make_world(WPE_Vec3 gravity) {
    Fixture f = {.assets = wpe_test_assets_create(NULL, 0)};
    WPE_TEST_ASSERT_OK(wpe_ecs_create(&f.ecs));
    WPE_TEST_ASSERT_OK(wpe_scene_register_components(f.ecs));
    WPE_TEST_ASSERT_OK(wpe_physics_create(
        &(WPE_PhysicsDesc){.ecs = f.ecs, .assets = f.assets.assets, .gravity = gravity}, &f.physics));
    return f;
}

static void destroy_world(Fixture* f) {
    wpe_physics_destroy(f->physics);
    wpe_ecs_destroy(f->ecs);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_asset_manager_stats(f->assets.assets).assets); /* Colliders released. */
    wpe_test_assets_destroy(&f->assets);
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

static void add_collider(WPE_Ecs* ecs, WPE_Entity e, WPE_Collider collider) {
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, e, WPE_COMP_PHYSICS_COLLIDER, &collider, NULL));
}

static void add_body(WPE_Ecs* ecs, WPE_Entity e, WPE_RigidBody body) {
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, e, WPE_COMP_PHYSICS_RIGIDBODY, &body, NULL));
}

/* Static 40 x 1 x 40 ground box whose top face is at y = 0. */
static WPE_Entity spawn_ground(WPE_Ecs* ecs) {
    const WPE_Entity ground = spawn_at(ecs, wpe_vec3(0.0f, -0.5f, 0.0f));
    WPE_Collider collider = wpe_collider_default(WPE_COLLIDER_BOX);
    collider.size = wpe_vec3(40.0f, 1.0f, 40.0f);
    add_collider(ecs, ground, collider);
    return ground;
}

static WPE_Entity spawn_dynamic(WPE_Ecs* ecs, WPE_Vec3 position, WPE_Collider collider) {
    const WPE_Entity e = spawn_at(ecs, position);
    add_body(ecs, e, wpe_rigidbody_default(WPE_BODY_DYNAMIC));
    add_collider(ecs, e, collider);
    return e;
}

static void run(Fixture* f, float seconds) {
    const int steps = (int)(seconds / DT + 0.5f);
    for (int i = 0; i < steps; ++i)
        WPE_TEST_ASSERT_OK(wpe_physics_step(f->physics, DT));
}

/* Whether the last step reported `type` for the (unordered unless trigger) entity pair. */
static bool has_event(const Fixture* f, WPE_PhysicsEventType type, WPE_Entity a, WPE_Entity b) {
    uint32_t count = 0;
    const WPE_PhysicsEvent* events = wpe_physics_events(f->physics, &count);
    for (uint32_t i = 0; i < count; ++i) {
        if (events[i].type != type) continue;
        if (events[i].entity_a == a && events[i].entity_b == b) return true;
        if (type <= WPE_PHYSICS_CONTACT_END && events[i].entity_a == b && events[i].entity_b == a)
            return true;
    }
    return false;
}

static void test_physics_create_validates_world(void) {
    WPE_Ecs* ecs = NULL;
    WPE_TEST_ASSERT_OK(wpe_ecs_create(&ecs));
    WPE_Physics* physics = NULL;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_STATE,
                           wpe_physics_create(&(WPE_PhysicsDesc){.ecs = ecs, .gravity = GRAVITY}, &physics));
    WPE_TEST_ASSERT_OK(wpe_scene_register_components(ecs));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NULL_ARG, wpe_physics_create(NULL, &physics));
    WPE_TEST_ASSERT_EQ_INT(
        WPE_RESULT_ERROR_INVALID_ARG,
        wpe_physics_create(&(WPE_PhysicsDesc){.ecs = ecs, .gravity = wpe_vec3(0.0f, NAN, 0.0f)}, &physics));
    WPE_TEST_ASSERT_OK(wpe_physics_create(&(WPE_PhysicsDesc){.ecs = ecs, .gravity = GRAVITY}, &physics));
    WPE_Physics* second = NULL;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_ALREADY_EXISTS,
                           wpe_physics_create(&(WPE_PhysicsDesc){.ecs = ecs, .gravity = GRAVITY}, &second));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_physics_step(physics, 0.0f));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_physics_step(physics, -DT));
    wpe_physics_destroy(physics);
    /* The components are unregistered again, so physics can be recreated. */
    WPE_TEST_ASSERT(!wpe_ecs_is_registered(ecs, WPE_COMP_PHYSICS_RIGIDBODY));
    WPE_TEST_ASSERT_OK(wpe_physics_create(&(WPE_PhysicsDesc){.ecs = ecs, .gravity = GRAVITY}, &physics));
    wpe_physics_destroy(physics);
    wpe_ecs_destroy(ecs);
}

static void test_physics_box_falls_and_comes_to_rest(void) {
    Fixture f = make_world(GRAVITY);
    spawn_ground(f.ecs);
    const WPE_Entity box =
        spawn_dynamic(f.ecs, wpe_vec3(0.0f, 5.0f, 0.0f), wpe_collider_default(WPE_COLLIDER_BOX));

    run(&f, 0.5f);
    const float y_mid = transform_of(f.ecs, box)->position.y;
    WPE_TEST_ASSERT(y_mid < 5.0f && y_mid > 0.5f); /* Falling, not yet landed. */

    run(&f, 4.0f);
    const WPE_Transform* t = transform_of(f.ecs, box);
    WPE_TEST_ASSERT_NEAR(0.5, t->position.y, 0.02);
    /* A flat face-on-face impact slides it ~1.4 cm (Box3D alone does the same). */
    WPE_TEST_ASSERT_NEAR(0.0, t->position.x, 0.03);
    WPE_TEST_ASSERT_NEAR(1.0, fabsf(t->rotation.w), 1e-3);

    const WPE_PhysicsStats stats = wpe_physics_stats(f.physics);
    WPE_TEST_ASSERT_EQ_UINT(2, stats.bodies);
    WPE_TEST_ASSERT_EQ_UINT(0, stats.awake_bodies); /* Asleep after resting. */
    WPE_TEST_ASSERT(stats.memory > 0);
    destroy_world(&f);
}

static void test_physics_restitution_bounces(void) {
    Fixture f = make_world(GRAVITY);
    spawn_ground(f.ecs);
    WPE_Collider bouncy = wpe_collider_default(WPE_COLLIDER_SPHERE);
    bouncy.size.x = 0.5f;
    bouncy.restitution = 0.9f;
    WPE_Collider dead = bouncy;
    dead.restitution = 0.0f;
    const WPE_Entity a = spawn_dynamic(f.ecs, wpe_vec3(-3.0f, 3.0f, 0.0f), bouncy);
    const WPE_Entity b = spawn_dynamic(f.ecs, wpe_vec3(3.0f, 3.0f, 0.0f), dead);

    /* Track the highest point after the first ground contact. */
    float peak_a = 0.0f;
    float peak_b = 0.0f;
    bool landed = false;
    for (int i = 0; i < 180; ++i) {
        WPE_TEST_ASSERT_OK(wpe_physics_step(f.physics, DT));
        const float ya = transform_of(f.ecs, a)->position.y;
        const float yb = transform_of(f.ecs, b)->position.y;
        if (ya < 0.6f) landed = true;
        if (landed) {
            peak_a = ya > peak_a ? ya : peak_a;
            peak_b = yb > peak_b ? yb : peak_b;
        }
    }
    WPE_TEST_ASSERT(landed);
    WPE_TEST_ASSERT_MSG(peak_a > 2.0f, "bouncy sphere peaked at %f", (double)peak_a);
    WPE_TEST_ASSERT_MSG(peak_b < 0.6f, "dead sphere peaked at %f", (double)peak_b);
    destroy_world(&f);
}

static void test_physics_contact_events(void) {
    Fixture f = make_world(GRAVITY);
    const WPE_Entity ground = spawn_ground(f.ecs);
    const WPE_Entity box =
        spawn_dynamic(f.ecs, wpe_vec3(0.0f, 1.0f, 0.0f), wpe_collider_default(WPE_COLLIDER_BOX));
    bool began = false;
    for (int i = 0; i < 120 && !began; ++i) {
        WPE_TEST_ASSERT_OK(wpe_physics_step(f.physics, DT));
        began = has_event(&f, WPE_PHYSICS_CONTACT_BEGIN, ground, box);
    }
    WPE_TEST_ASSERT(began);

    /* Lift it off the ground: the contact ends. */
    transform_of(f.ecs, box)->position = wpe_vec3(0.0f, 10.0f, 0.0f);
    bool ended = false;
    for (int i = 0; i < 5 && !ended; ++i) {
        WPE_TEST_ASSERT_OK(wpe_physics_step(f.physics, DT));
        ended = has_event(&f, WPE_PHYSICS_CONTACT_END, ground, box);
    }
    WPE_TEST_ASSERT(ended);
    destroy_world(&f);
}

static void test_physics_trigger_enter_and_exit(void) {
    Fixture f = make_world(GRAVITY);
    spawn_ground(f.ecs);
    const WPE_Entity trigger = spawn_at(f.ecs, wpe_vec3(0.0f, 3.0f, 0.0f));
    WPE_Collider zone = wpe_collider_default(WPE_COLLIDER_BOX);
    zone.size = wpe_vec3(4.0f, 1.0f, 4.0f);
    zone.is_trigger = true;
    add_collider(f.ecs, trigger, zone);
    WPE_Collider ball = wpe_collider_default(WPE_COLLIDER_SPHERE);
    ball.size.x = 0.25f;
    const WPE_Entity visitor = spawn_dynamic(f.ecs, wpe_vec3(0.0f, 6.0f, 0.0f), ball);

    int entered_at = -1;
    int exited_at = -1;
    for (int i = 0; i < 180; ++i) {
        WPE_TEST_ASSERT_OK(wpe_physics_step(f.physics, DT));
        if (entered_at < 0 && has_event(&f, WPE_PHYSICS_TRIGGER_ENTER, trigger, visitor)) entered_at = i;
        if (exited_at < 0 && has_event(&f, WPE_PHYSICS_TRIGGER_EXIT, trigger, visitor)) exited_at = i;
        /* Triggers never report contacts. */
        WPE_TEST_ASSERT(!has_event(&f, WPE_PHYSICS_CONTACT_BEGIN, trigger, visitor));
    }
    WPE_TEST_ASSERT(entered_at >= 0);
    WPE_TEST_ASSERT(exited_at > entered_at);
    /* It fell through the trigger onto the ground. */
    WPE_TEST_ASSERT_NEAR(0.25, transform_of(f.ecs, visitor)->position.y, 0.02);
    destroy_world(&f);
}

static void test_physics_raycast(void) {
    Fixture f = make_world(GRAVITY);
    const WPE_Entity ground = spawn_ground(f.ecs);
    const WPE_Entity trigger = spawn_at(f.ecs, wpe_vec3(0.0f, 2.0f, 0.0f));
    WPE_Collider zone = wpe_collider_default(WPE_COLLIDER_BOX);
    zone.is_trigger = true;
    add_collider(f.ecs, trigger, zone);
    /* Bodies only exist after a step. */
    WPE_TEST_ASSERT(!wpe_physics_raycast(f.physics, wpe_vec3(0, 10, 0), wpe_vec3(0, -1, 0), 100.0f, NULL));
    WPE_TEST_ASSERT_OK(wpe_physics_step(f.physics, DT));

    WPE_RaycastHit hit = {0};
    WPE_TEST_ASSERT(wpe_physics_raycast(f.physics, wpe_vec3(1, 10, 0), wpe_vec3(0, -3, 0), 100.0f, &hit));
    WPE_TEST_ASSERT_EQ_UINT(ground, hit.entity); /* The trigger is ignored. */
    WPE_TEST_ASSERT_NEAR(10.0, hit.distance, 1e-3);
    WPE_TEST_ASSERT_NEAR(0.0, hit.point.y, 1e-3);
    WPE_TEST_ASSERT_NEAR(1.0, hit.normal.y, 1e-3);

    WPE_TEST_ASSERT(!wpe_physics_raycast(f.physics, wpe_vec3(0, 10, 0), wpe_vec3(0, -1, 0), 5.0f, &hit));
    WPE_TEST_ASSERT(!wpe_physics_raycast(f.physics, wpe_vec3(0, 10, 0), wpe_vec3(0, 1, 0), 100.0f, &hit));
    WPE_TEST_ASSERT(!wpe_physics_raycast(f.physics, wpe_vec3(0, 10, 0), wpe_vec3(0, 0, 0), 100.0f, &hit));
    destroy_world(&f);
}

static void test_physics_teleport_follows_game_code(void) {
    Fixture f = make_world(GRAVITY);
    spawn_ground(f.ecs);
    const WPE_Entity box =
        spawn_dynamic(f.ecs, wpe_vec3(0.0f, 0.5f, 0.0f), wpe_collider_default(WPE_COLLIDER_BOX));
    run(&f, 3.0f);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_physics_stats(f.physics).awake_bodies);

    /* Moving a sleeping body teleports and wakes it; it then falls from there. */
    transform_of(f.ecs, box)->position = wpe_vec3(5.0f, 3.0f, 0.0f);
    WPE_TEST_ASSERT_OK(wpe_physics_step(f.physics, DT));
    const WPE_Transform* t = transform_of(f.ecs, box);
    WPE_TEST_ASSERT_NEAR(5.0, t->position.x, 1e-3);
    WPE_TEST_ASSERT(t->position.y < 3.0f && t->position.y > 2.9f);
    run(&f, 3.0f);
    WPE_TEST_ASSERT_NEAR(0.5, transform_of(f.ecs, box)->position.y, 0.02);
    destroy_world(&f);
}

static void test_physics_component_removal_releases_bodies(void) {
    Fixture f = make_world(GRAVITY);
    const WPE_Entity ground = spawn_ground(f.ecs);
    const WPE_Entity box =
        spawn_dynamic(f.ecs, wpe_vec3(0.0f, 2.0f, 0.0f), wpe_collider_default(WPE_COLLIDER_BOX));
    WPE_TEST_ASSERT_OK(wpe_physics_step(f.physics, DT));
    WPE_TEST_ASSERT_EQ_UINT(2, wpe_physics_stats(f.physics).bodies);

    /* Without its rigid body the collider becomes level geometry on an implicit static body. */
    const float y = transform_of(f.ecs, box)->position.y;
    WPE_TEST_ASSERT_OK(wpe_ecs_remove_component(f.ecs, box, WPE_COMP_PHYSICS_RIGIDBODY));
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_physics_stats(f.physics).bodies);
    run(&f, 1.0f);
    WPE_TEST_ASSERT_EQ_UINT(2, wpe_physics_stats(f.physics).bodies);
    WPE_TEST_ASSERT_NEAR(y, transform_of(f.ecs, box)->position.y, 1e-6); /* Static now: it does not fall. */

    /* Adding a rigid body back moves the collider onto it. */
    add_body(f.ecs, box, wpe_rigidbody_default(WPE_BODY_DYNAMIC));
    run(&f, 2.0f);
    WPE_TEST_ASSERT_EQ_UINT(2, wpe_physics_stats(f.physics).bodies);
    WPE_TEST_ASSERT_NEAR(0.5, transform_of(f.ecs, box)->position.y, 0.02);

    WPE_TEST_ASSERT_OK(wpe_ecs_despawn(f.ecs, box));
    WPE_TEST_ASSERT_OK(wpe_ecs_remove_component(f.ecs, ground, WPE_COMP_PHYSICS_COLLIDER));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_physics_stats(f.physics).bodies);
    WPE_TEST_ASSERT_OK(wpe_physics_step(f.physics, DT));
    destroy_world(&f);
}

static void test_physics_destroy_with_live_components(void) {
    Fixture f = make_world(GRAVITY);
    spawn_ground(f.ecs);
    for (int i = 0; i < 20; ++i)
        spawn_dynamic(f.ecs, wpe_vec3(0.0f, 1.0f + 1.1f * (float)i, 0.0f),
                      wpe_collider_default(WPE_COLLIDER_BOX));
    run(&f, 0.5f);
    const uint32_t entities = wpe_ecs_entity_count(f.ecs);
    wpe_physics_destroy(f.physics);
    /* Entities survive; only their physics components are removed. */
    WPE_TEST_ASSERT_EQ_UINT(entities, wpe_ecs_entity_count(f.ecs));
    WPE_TEST_ASSERT(!wpe_ecs_is_registered(f.ecs, WPE_COMP_PHYSICS_COLLIDER));
    wpe_ecs_destroy(f.ecs);
    wpe_test_assets_destroy(&f.assets);
}

/* Builds a small pile and returns the final pose bytes of every box. */
static void simulate_pile(WPE_Transform* out, int count) {
    Fixture f = make_world(GRAVITY);
    spawn_ground(f.ecs);
    WPE_Entity boxes[16];
    for (int i = 0; i < count; ++i) {
        const float offset = 0.3f * (float)(i % 3) - 0.3f;
        boxes[i] = spawn_dynamic(f.ecs, wpe_vec3(offset, 1.0f + 1.2f * (float)i, offset * 0.5f),
                                 wpe_collider_default(i % 2 ? WPE_COLLIDER_SPHERE : WPE_COLLIDER_BOX));
    }
    run(&f, 3.0f);
    for (int i = 0; i < count; ++i)
        out[i] = *transform_of(f.ecs, boxes[i]);
    destroy_world(&f);
}

static void test_physics_is_deterministic(void) {
    WPE_Transform first[12];
    WPE_Transform second[12];
    simulate_pile(first, 12);
    simulate_pile(second, 12);
    for (int i = 0; i < 12; ++i) {
        WPE_TEST_ASSERT(memcmp(&first[i].position, &second[i].position, sizeof first[i].position) == 0);
        WPE_TEST_ASSERT(memcmp(&first[i].rotation, &second[i].rotation, sizeof first[i].rotation) == 0);
    }
}

static void test_physics_lock_rotation(void) {
    Fixture f = make_world(GRAVITY);
    spawn_ground(f.ecs);
    /* Tilted boxes land on an edge: a free one topples, a locked one keeps its rotation. */
    const WPE_Quat tilt = wpe_quat_from_axis_angle(wpe_vec3(0, 0, 1), 0.5f);
    const WPE_Entity free_box =
        spawn_dynamic(f.ecs, wpe_vec3(-3, 2, 0), wpe_collider_default(WPE_COLLIDER_BOX));
    const WPE_Entity locked = spawn_at(f.ecs, wpe_vec3(3, 2, 0));
    WPE_RigidBody body = wpe_rigidbody_default(WPE_BODY_DYNAMIC);
    body.lock_rotation = true;
    add_body(f.ecs, locked, body);
    add_collider(f.ecs, locked, wpe_collider_default(WPE_COLLIDER_BOX));
    transform_of(f.ecs, free_box)->rotation = tilt;
    transform_of(f.ecs, locked)->rotation = tilt;
    run(&f, 3.0f);

    const WPE_Quat q_locked = transform_of(f.ecs, locked)->rotation;
    WPE_TEST_ASSERT_NEAR(
        1.0, fabsf(q_locked.x * tilt.x + q_locked.y * tilt.y + q_locked.z * tilt.z + q_locked.w * tilt.w),
        1e-4);
    /* The free box settled flat (rotation a multiple of 90 degrees about Z). */
    const WPE_Vec3 up = wpe_quat_rotate_vec3(transform_of(f.ecs, free_box)->rotation, wpe_vec3(0, 1, 0));
    WPE_TEST_ASSERT(fabsf(up.x) < 0.02f || fabsf(up.y) < 0.02f);
    WPE_TEST_ASSERT_NEAR(0.5, transform_of(f.ecs, free_box)->position.y, 0.02);
    destroy_world(&f);
}

static void test_physics_parented_body_writes_local_transform(void) {
    Fixture f = make_world(GRAVITY);
    spawn_ground(f.ecs);
    const WPE_Entity parent = spawn_at(f.ecs, wpe_vec3(10.0f, 1.0f, 0.0f));
    transform_of(f.ecs, parent)->rotation = wpe_quat_from_axis_angle(wpe_vec3(0, 1, 0), WPE_PI * 0.5f);
    const WPE_Entity child =
        spawn_dynamic(f.ecs, wpe_vec3(0.0f, 3.0f, 0.0f), wpe_collider_default(WPE_COLLIDER_BOX));
    WPE_TEST_ASSERT_OK(wpe_scene_set_parent(f.ecs, child, parent, false));
    run(&f, 3.0f);

    WPE_TEST_ASSERT_OK(wpe_scene_update_transforms(f.ecs));
    const WPE_Transform* t = transform_of(f.ecs, child);
    WPE_TEST_ASSERT_NEAR(10.0, t->world.m[3][0], 0.01); /* World position stayed above the parent. */
    WPE_TEST_ASSERT_NEAR(0.5, t->world.m[3][1], 0.02);
    WPE_TEST_ASSERT_NEAR(-0.5, t->position.y, 0.02); /* Local: relative to the parent at y = 1. */
    WPE_TEST_ASSERT_NEAR(0.0, t->position.x, 0.01);
    destroy_world(&f);
}

static void test_physics_scaled_collider(void) {
    Fixture f = make_world(GRAVITY);
    spawn_ground(f.ecs);
    const WPE_Entity big = spawn_dynamic(f.ecs, wpe_vec3(0, 3, 0), wpe_collider_default(WPE_COLLIDER_BOX));
    transform_of(f.ecs, big)->scale = wpe_vec3(2.0f, 2.0f, 2.0f);
    WPE_Collider sphere = wpe_collider_default(WPE_COLLIDER_SPHERE);
    sphere.size.x = 0.5f;
    sphere.offset = wpe_vec3(0.0f, 1.0f, 0.0f); /* Offset is scaled too. */
    const WPE_Entity ball = spawn_dynamic(f.ecs, wpe_vec3(5, 3, 0), sphere);
    transform_of(f.ecs, ball)->scale = wpe_vec3(0.5f, 0.5f, 0.5f);
    run(&f, 3.0f);
    WPE_TEST_ASSERT_NEAR(1.0, transform_of(f.ecs, big)->position.y, 0.02);
    /* Radius 0.25 centred 0.5 above the origin: the origin rests 0.25 below the ground surface. */
    WPE_TEST_ASSERT_NEAR(-0.25, transform_of(f.ecs, ball)->position.y, 0.02);
    destroy_world(&f);
}

static void test_physics_capsule_rests_upright_with_locked_rotation(void) {
    Fixture f = make_world(GRAVITY);
    spawn_ground(f.ecs);
    const WPE_Entity e = spawn_at(f.ecs, wpe_vec3(0, 2, 0));
    WPE_RigidBody body = wpe_rigidbody_default(WPE_BODY_DYNAMIC);
    body.lock_rotation = true;
    add_body(f.ecs, e, body);
    WPE_Collider capsule = wpe_collider_default(WPE_COLLIDER_CAPSULE);
    capsule.size = wpe_vec3(0.3f, 1.8f, 0.0f);
    add_collider(f.ecs, e, capsule);
    run(&f, 3.0f);
    WPE_TEST_ASSERT_NEAR(0.9, transform_of(f.ecs, e)->position.y, 0.02);
    destroy_world(&f);
}

static void test_physics_mesh_and_convex_colliders(void) {
    Fixture f = make_world(GRAVITY);
    WPE_Model* plane_model = NULL;
    WPE_Model* box_model = NULL;
    WPE_TEST_ASSERT_OK(wpe_model_create_plane(20.0f, 4, NULL, &plane_model));
    WPE_TEST_ASSERT_OK(wpe_model_create_box(wpe_vec3(1.0f, 1.0f, 1.0f), NULL, &box_model));
    const WPE_AssetHandle plane = wpe_test_add_model(f.assets.assets, "test/plane", plane_model);
    const WPE_AssetHandle box = wpe_test_add_model(f.assets.assets, "test/box", box_model);

    const WPE_Entity ground = spawn_at(f.ecs, wpe_vec3(0, 0, 0));
    WPE_Collider mesh = wpe_collider_default(WPE_COLLIDER_MESH);
    mesh.model = plane;
    add_collider(f.ecs, ground, mesh);

    WPE_Collider convex = wpe_collider_default(WPE_COLLIDER_CONVEX);
    convex.model = box;
    const WPE_Entity hull = spawn_dynamic(f.ecs, wpe_vec3(0, 3, 0), convex);

    /* Triangle meshes cannot be dynamic. */
    const WPE_Entity bad = spawn_dynamic(f.ecs, wpe_vec3(5, 3, 0), mesh);
    run(&f, 3.0f);
    /* Every collider component holds its own reference (the rejected mesh collider too). */
    WPE_TEST_ASSERT_EQ_UINT(3, wpe_asset_ref_count(f.assets.assets, plane));
    WPE_TEST_ASSERT_EQ_UINT(2, wpe_asset_ref_count(f.assets.assets, box));
    wpe_asset_release(f.assets.assets, plane);
    wpe_asset_release(f.assets.assets, box);
    /* Rebuilding works from the collider's own reference and does not accumulate references. */
    WPE_TEST_ASSERT_OK(wpe_physics_refresh(f.physics, hull));
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_asset_ref_count(f.assets.assets, box));

    WPE_TEST_ASSERT_NEAR(0.5, transform_of(f.ecs, hull)->position.y, 0.02);
    WPE_TEST_ASSERT_NEAR(3.0, transform_of(f.ecs, bad)->position.y, 1e-5); /* Never simulated... */
    WPE_TEST_ASSERT_EQ_UINT(3, wpe_physics_stats(f.physics).bodies);       /* ...but its rigid body exists. */

    WPE_RaycastHit hit;
    WPE_TEST_ASSERT(wpe_physics_raycast(f.physics, wpe_vec3(7, 5, 3), wpe_vec3(0, -1, 0), 10.0f, &hit));
    WPE_TEST_ASSERT_EQ_UINT(ground, hit.entity);
    WPE_TEST_ASSERT_NEAR(5.0, hit.distance, 1e-3);
    destroy_world(&f);
}

static void test_physics_kinematic_body_follows_transform(void) {
    Fixture f = make_world(GRAVITY);
    spawn_ground(f.ecs);
    const WPE_Entity platform = spawn_at(f.ecs, wpe_vec3(0, 1, 0));
    add_body(f.ecs, platform, wpe_rigidbody_default(WPE_BODY_KINEMATIC));
    WPE_Collider slab = wpe_collider_default(WPE_COLLIDER_BOX);
    slab.size = wpe_vec3(2.0f, 0.2f, 2.0f);
    add_collider(f.ecs, platform, slab);
    const WPE_Entity rider =
        spawn_dynamic(f.ecs, wpe_vec3(0, 1.6f, 0), wpe_collider_default(WPE_COLLIDER_BOX));
    run(&f, 1.0f);
    WPE_TEST_ASSERT_NEAR(1.6, transform_of(f.ecs, rider)->position.y, 0.02);

    /* Raise the platform by game code over one second: it carries the rider up. */
    for (int i = 1; i <= 60; ++i) {
        transform_of(f.ecs, platform)->position.y = 1.0f + (float)i / 60.0f;
        WPE_TEST_ASSERT_OK(wpe_physics_step(f.physics, DT));
    }
    run(&f, 0.5f);
    WPE_TEST_ASSERT_NEAR(2.0, transform_of(f.ecs, platform)->position.y, 1e-3);
    WPE_TEST_ASSERT_NEAR(2.6, transform_of(f.ecs, rider)->position.y, 0.03);
    WPE_Vec3 velocity;
    WPE_TEST_ASSERT_OK(wpe_physics_get_velocity(f.physics, platform, &velocity));
    WPE_TEST_ASSERT_NEAR(0.0, wpe_vec3_length(velocity), 1e-6); /* Stopped after arriving. */

    /* A velocity-driven kinematic body keeps moving and is written back. */
    WPE_TEST_ASSERT_OK(wpe_physics_set_velocity(f.physics, platform, wpe_vec3(1.0f, 0.0f, 0.0f)));
    run(&f, 1.0f);
    WPE_TEST_ASSERT_NEAR(1.0, transform_of(f.ecs, platform)->position.x, 0.02);
    destroy_world(&f);
}

static void test_physics_velocity_force_and_impulse(void) {
    Fixture f = make_world(wpe_vec3(0.0f, 0.0f, 0.0f));
    const WPE_Entity box = spawn_dynamic(f.ecs, wpe_vec3(0, 0, 0), wpe_collider_default(WPE_COLLIDER_BOX));
    const WPE_Entity wall = spawn_at(f.ecs, wpe_vec3(0, 0, 50));
    add_body(f.ecs, wall, wpe_rigidbody_default(WPE_BODY_STATIC));
    add_collider(f.ecs, wall, wpe_collider_default(WPE_COLLIDER_BOX));
    WPE_Vec3 velocity;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND, wpe_physics_get_velocity(f.physics, box, &velocity));
    WPE_TEST_ASSERT_OK(wpe_physics_step(f.physics, DT));

    /* 1 m^3 at density 1: mass 1 kg, so an impulse equals the velocity change. */
    WPE_TEST_ASSERT_OK(wpe_physics_apply_impulse(f.physics, box, wpe_vec3(2.0f, 0.0f, 0.0f)));
    WPE_TEST_ASSERT_OK(wpe_physics_get_velocity(f.physics, box, &velocity));
    WPE_TEST_ASSERT_NEAR(2.0, velocity.x, 1e-4);
    run(&f, 1.0f);
    WPE_TEST_ASSERT_NEAR(2.0 + 0.0, transform_of(f.ecs, box)->position.x, 0.05);

    WPE_TEST_ASSERT_OK(wpe_physics_set_velocity(f.physics, box, wpe_vec3(0.0f, 0.0f, 0.0f)));
    for (int i = 0; i < 60; ++i) {
        WPE_TEST_ASSERT_OK(wpe_physics_apply_force(f.physics, box, wpe_vec3(0.0f, 3.0f, 0.0f)));
        WPE_TEST_ASSERT_OK(wpe_physics_step(f.physics, DT));
    }
    WPE_TEST_ASSERT_OK(wpe_physics_get_velocity(f.physics, box, &velocity));
    WPE_TEST_ASSERT_NEAR(3.0, velocity.y, 0.01); /* a = F / m over one second. */

    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_STATE,
                           wpe_physics_apply_force(f.physics, wall, wpe_vec3(1.0f, 0.0f, 0.0f)));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_STATE,
                           wpe_physics_set_velocity(f.physics, wall, wpe_vec3(1.0f, 0.0f, 0.0f)));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG,
                           wpe_physics_apply_impulse(f.physics, box, wpe_vec3(NAN, 0.0f, 0.0f)));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND,
                           wpe_physics_apply_force(f.physics, WPE_ENTITY_NULL, wpe_vec3(1.0f, 0.0f, 0.0f)));

    wpe_physics_set_gravity(f.physics, wpe_vec3(0.0f, -2.0f, 0.0f));
    WPE_TEST_ASSERT_NEAR(-2.0, wpe_physics_gravity(f.physics).y, 1e-6);
    destroy_world(&f);
}

static void test_physics_refresh_rebuilds_shapes(void) {
    Fixture f = make_world(GRAVITY);
    spawn_ground(f.ecs);
    const WPE_Entity box = spawn_dynamic(f.ecs, wpe_vec3(0, 0.5f, 0), wpe_collider_default(WPE_COLLIDER_BOX));
    run(&f, 1.0f);
    WPE_TEST_ASSERT_NEAR(0.5, transform_of(f.ecs, box)->position.y, 0.02);

    /* Settings are read at creation; refresh applies the new size. */
    WPE_Collider* collider = wpe_ecs_get_component(f.ecs, box, WPE_COMP_PHYSICS_COLLIDER);
    collider->size = wpe_vec3(3.0f, 3.0f, 3.0f);
    transform_of(f.ecs, box)->position.y = 2.0f;
    WPE_TEST_ASSERT_OK(wpe_physics_refresh(f.physics, box));
    run(&f, 2.0f);
    WPE_TEST_ASSERT_NEAR(1.5, transform_of(f.ecs, box)->position.y, 0.02);

    /* Invalid settings fail and are not retried every step. */
    collider = wpe_ecs_get_component(f.ecs, box, WPE_COMP_PHYSICS_COLLIDER);
    collider->density = -1.0f;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_physics_refresh(f.physics, box));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND,
                           wpe_physics_refresh(f.physics, spawn_at(f.ecs, wpe_vec3(0, 0, 0))));
    destroy_world(&f);
}

static void test_physics_degenerate_transform_is_reported_once(void) {
    Fixture f = make_world(GRAVITY);
    const WPE_Entity flat = spawn_dynamic(f.ecs, wpe_vec3(0, 1, 0), wpe_collider_default(WPE_COLLIDER_BOX));
    transform_of(f.ecs, flat)->scale = wpe_vec3(1.0f, 0.0f, 1.0f);
    run(&f, 0.1f);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_physics_stats(f.physics).bodies);
    WPE_TEST_ASSERT_NEAR(1.0, transform_of(f.ecs, flat)->position.y, 1e-6);
    /* Fixing the transform and refreshing brings it to life. */
    transform_of(f.ecs, flat)->scale = wpe_vec3(1.0f, 1.0f, 1.0f);
    WPE_TEST_ASSERT_OK(wpe_physics_refresh(f.physics, flat));
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_physics_stats(f.physics).bodies);
    destroy_world(&f);
}

typedef struct LineCounter {
    uint32_t lines;
    uint32_t colors[4];
    uint32_t color_count;
} LineCounter;

static void count_line(WPE_Vec3 a, WPE_Vec3 b, uint32_t color, void* user_data) {
    (void)a;
    (void)b;
    LineCounter* counter = user_data;
    ++counter->lines;
    for (uint32_t i = 0; i < counter->color_count; ++i)
        if (counter->colors[i] == color) return;
    if (counter->color_count < 4) counter->colors[counter->color_count++] = color;
}

static void test_physics_debug_draw(void) {
    Fixture f = make_world(GRAVITY);
    spawn_ground(f.ecs);
    LineCounter counter = {0};
    wpe_physics_debug_draw(f.physics, count_line, &counter);
    WPE_TEST_ASSERT_EQ_UINT(0, counter.lines); /* Nothing created yet. */

    WPE_TEST_ASSERT_OK(wpe_physics_step(f.physics, DT));
    wpe_physics_debug_draw(f.physics, count_line, &counter);
    WPE_TEST_ASSERT_EQ_UINT(12, counter.lines); /* Box edges. */

    WPE_Collider sphere = wpe_collider_default(WPE_COLLIDER_SPHERE);
    sphere.size.x = 0.5f;
    spawn_dynamic(f.ecs, wpe_vec3(0, 3, 0), sphere);
    WPE_TEST_ASSERT_OK(wpe_physics_step(f.physics, DT));
    counter = (LineCounter){0};
    wpe_physics_debug_draw(f.physics, count_line, &counter);
    WPE_TEST_ASSERT_EQ_UINT(12 + 3 * 16, counter.lines);
    WPE_TEST_ASSERT_EQ_UINT(2, counter.color_count); /* Static and awake dynamic differ. */
    destroy_world(&f);
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_physics_create_validates_world),
              WPE_TEST_CASE(test_physics_box_falls_and_comes_to_rest),
              WPE_TEST_CASE(test_physics_restitution_bounces), WPE_TEST_CASE(test_physics_contact_events),
              WPE_TEST_CASE(test_physics_trigger_enter_and_exit), WPE_TEST_CASE(test_physics_raycast),
              WPE_TEST_CASE(test_physics_teleport_follows_game_code),
              WPE_TEST_CASE(test_physics_component_removal_releases_bodies),
              WPE_TEST_CASE(test_physics_destroy_with_live_components),
              WPE_TEST_CASE(test_physics_is_deterministic), WPE_TEST_CASE(test_physics_lock_rotation),
              WPE_TEST_CASE(test_physics_parented_body_writes_local_transform),
              WPE_TEST_CASE(test_physics_scaled_collider),
              WPE_TEST_CASE(test_physics_capsule_rests_upright_with_locked_rotation),
              WPE_TEST_CASE(test_physics_mesh_and_convex_colliders),
              WPE_TEST_CASE(test_physics_kinematic_body_follows_transform),
              WPE_TEST_CASE(test_physics_velocity_force_and_impulse),
              WPE_TEST_CASE(test_physics_refresh_rebuilds_shapes),
              WPE_TEST_CASE(test_physics_degenerate_transform_is_reported_once),
              WPE_TEST_CASE(test_physics_debug_draw))
