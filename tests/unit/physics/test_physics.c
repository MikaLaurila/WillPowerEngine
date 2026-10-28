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
    WPE_TEST_ASSERT(
        !wpe_physics_raycast(f.physics, wpe_vec3(0, 10, 0), wpe_vec3(0, -1, 0), 100.0f, NULL, NULL));
    WPE_TEST_ASSERT_OK(wpe_physics_step(f.physics, DT));

    WPE_RaycastHit hit = {0};
    WPE_TEST_ASSERT(
        wpe_physics_raycast(f.physics, wpe_vec3(1, 10, 0), wpe_vec3(0, -3, 0), 100.0f, NULL, &hit));
    WPE_TEST_ASSERT_EQ_UINT(ground, hit.entity); /* The trigger is ignored. */
    WPE_TEST_ASSERT_NEAR(10.0, hit.distance, 1e-3);
    WPE_TEST_ASSERT_NEAR(0.0, hit.point.y, 1e-3);
    WPE_TEST_ASSERT_NEAR(1.0, hit.normal.y, 1e-3);

    WPE_TEST_ASSERT(
        !wpe_physics_raycast(f.physics, wpe_vec3(0, 10, 0), wpe_vec3(0, -1, 0), 5.0f, NULL, &hit));
    WPE_TEST_ASSERT(
        !wpe_physics_raycast(f.physics, wpe_vec3(0, 10, 0), wpe_vec3(0, 1, 0), 100.0f, NULL, &hit));
    WPE_TEST_ASSERT(
        !wpe_physics_raycast(f.physics, wpe_vec3(0, 10, 0), wpe_vec3(0, 0, 0), 100.0f, NULL, &hit));
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
    WPE_TEST_ASSERT(wpe_physics_raycast(f.physics, wpe_vec3(7, 5, 3), wpe_vec3(0, -1, 0), 10.0f, NULL, &hit));
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

static void test_physics_create_bodies_and_initial_velocity(void) {
    Fixture f = make_world(GRAVITY);
    const WPE_Entity ground = spawn_ground(f.ecs);
    WPE_RigidBody body = wpe_rigidbody_default(WPE_BODY_DYNAMIC);
    body.initial_velocity = wpe_vec3(3.0f, 0.0f, 0.0f);
    const WPE_Entity ball = spawn_at(f.ecs, wpe_vec3(0.0f, 5.0f, 0.0f));
    add_body(f.ecs, ball, body);
    add_collider(f.ecs, ball, wpe_collider_default(WPE_COLLIDER_SPHERE));

    /* Before any step there are no bodies yet... */
    WPE_Vec3 velocity;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND, wpe_physics_get_velocity(f.physics, ball, &velocity));
    WPE_RaycastHit hit;
    WPE_TEST_ASSERT(
        !wpe_physics_raycast(f.physics, wpe_vec3(0, 2, 0), wpe_vec3(0, -1, 0), 10.0f, NULL, &hit));
    /* ...creating them makes the body usable immediately, already moving at its initial velocity. */
    WPE_TEST_ASSERT_OK(wpe_physics_create_bodies(f.physics));
    WPE_TEST_ASSERT_OK(wpe_physics_get_velocity(f.physics, ball, &velocity));
    WPE_TEST_ASSERT_NEAR(3.0, velocity.x, 1e-6);
    WPE_TEST_ASSERT(wpe_physics_raycast(f.physics, wpe_vec3(0, 2, 0), wpe_vec3(0, -1, 0), 10.0f, NULL, &hit));
    WPE_TEST_ASSERT_EQ_UINT(ground, hit.entity);
    WPE_TEST_ASSERT_EQ_UINT(2, wpe_physics_stats(f.physics).bodies);
    WPE_TEST_ASSERT_OK(wpe_physics_create_bodies(f.physics)); /* Nothing left to create. */
    WPE_TEST_ASSERT_EQ_UINT(2, wpe_physics_stats(f.physics).bodies);
    run(&f, 0.5f);
    WPE_TEST_ASSERT(transform_of(f.ecs, ball)->position.x > 1.0f);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NULL_ARG, wpe_physics_create_bodies(NULL));
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

/* Edits without simulating (the editor's edit mode): sync follows moves, rescales and new colliders. */
static void test_physics_sync_follows_edits(void) {
    Fixture f = make_world(GRAVITY);
    const WPE_Entity wall = spawn_at(f.ecs, wpe_vec3(0, 0, 0));
    add_collider(f.ecs, wall, wpe_collider_default(WPE_COLLIDER_BOX)); /* 1 x 1 x 1. */
    WPE_TEST_ASSERT_OK(wpe_physics_sync(f.physics));
    WPE_RaycastHit hit;
    WPE_TEST_ASSERT(wpe_physics_raycast(f.physics, wpe_vec3(0, 5, 0), wpe_vec3(0, -1, 0), 10.0f, NULL, &hit));
    WPE_TEST_ASSERT_NEAR(0.5, hit.point.y, 1e-4);

    /* Moved and scaled: the body follows and the shape is rebuilt for the new size. */
    transform_of(f.ecs, wall)->position = wpe_vec3(4, 0, 0);
    transform_of(f.ecs, wall)->scale = wpe_vec3(1, 3, 1);
    WPE_TEST_ASSERT_OK(wpe_physics_sync(f.physics));
    WPE_TEST_ASSERT(
        !wpe_physics_raycast(f.physics, wpe_vec3(0, 5, 0), wpe_vec3(0, -1, 0), 10.0f, NULL, &hit));
    WPE_TEST_ASSERT(wpe_physics_raycast(f.physics, wpe_vec3(4, 5, 0), wpe_vec3(0, -1, 0), 10.0f, NULL, &hit));
    WPE_TEST_ASSERT_NEAR(1.5, hit.point.y, 1e-4);

    /* Kinematic bodies are placed too (an edit, not a move with velocity), and new colliders appear. */
    WPE_Entity platform = spawn_at(f.ecs, wpe_vec3(-4, 0, 0));
    add_body(f.ecs, platform, wpe_rigidbody_default(WPE_BODY_KINEMATIC));
    add_collider(f.ecs, platform, wpe_collider_default(WPE_COLLIDER_BOX));
    WPE_TEST_ASSERT_OK(wpe_physics_sync(f.physics));
    transform_of(f.ecs, platform)->position = wpe_vec3(-4, 2, 0);
    WPE_TEST_ASSERT_OK(wpe_physics_sync(f.physics));
    WPE_TEST_ASSERT(
        wpe_physics_raycast(f.physics, wpe_vec3(-4, 5, 0), wpe_vec3(0, -1, 0), 10.0f, NULL, &hit));
    WPE_TEST_ASSERT_EQ_UINT(platform, hit.entity);
    WPE_TEST_ASSERT_NEAR(2.5, hit.point.y, 1e-4);
    WPE_Vec3 velocity;
    WPE_TEST_ASSERT_OK(wpe_physics_get_velocity(f.physics, platform, &velocity));
    WPE_TEST_ASSERT_NEAR(0.0, wpe_vec3_length(velocity), 1e-6);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NULL_ARG, wpe_physics_sync(NULL));
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

/* ---- Queries ---------------------------------------------------------------------------------------- */

static WPE_Entity spawn_static(WPE_Ecs* ecs, WPE_Vec3 position, WPE_Collider collider) {
    const WPE_Entity e = spawn_at(ecs, position);
    add_collider(ecs, e, collider);
    return e;
}

static bool contains(const WPE_Entity* entities, uint32_t count, WPE_Entity e) {
    for (uint32_t i = 0; i < count; ++i)
        if (entities[i] == e) return true;
    return false;
}

static void test_physics_shape_casts_overlaps_and_filters(void) {
    Fixture f = make_world(GRAVITY);
    const WPE_Entity ground = spawn_ground(f.ecs);
    WPE_Collider box = wpe_collider_default(WPE_COLLIDER_BOX);
    box.size = wpe_vec3(2.0f, 2.0f, 2.0f);
    const WPE_Entity wall = spawn_static(f.ecs, wpe_vec3(5.0f, 1.0f, 0.0f), box); /* x 4..6, y 0..2 */
    WPE_Collider sphere = wpe_collider_default(WPE_COLLIDER_SPHERE);
    sphere.is_trigger = true;
    const WPE_Entity trigger = spawn_static(f.ecs, wpe_vec3(-5.0f, 1.0f, 0.0f), sphere); /* radius 1 */
    WPE_TEST_ASSERT_OK(wpe_physics_create_bodies(f.physics));

    /* Rays: filters skip an entity, or see triggers. */
    WPE_RaycastHit hit;
    WPE_TEST_ASSERT(
        wpe_physics_raycast(f.physics, wpe_vec3(5, 10, 0), wpe_vec3(0, -1, 0), 20.0f, NULL, &hit));
    WPE_TEST_ASSERT_EQ_UINT(wall, hit.entity);
    WPE_TEST_ASSERT_NEAR(2.0, hit.point.y, 1e-3);
    WPE_TEST_ASSERT(wpe_physics_raycast(f.physics, wpe_vec3(5, 10, 0), wpe_vec3(0, -1, 0), 20.0f,
                                        &(WPE_QueryFilter){.ignore = wall}, &hit));
    WPE_TEST_ASSERT_EQ_UINT(ground, hit.entity);
    WPE_TEST_ASSERT(
        wpe_physics_raycast(f.physics, wpe_vec3(-5, 10, 0), wpe_vec3(0, -1, 0), 20.0f, NULL, &hit));
    WPE_TEST_ASSERT_EQ_UINT(ground, hit.entity);
    WPE_TEST_ASSERT(wpe_physics_raycast(f.physics, wpe_vec3(-5, 10, 0), wpe_vec3(0, -1, 0), 20.0f,
                                        &(WPE_QueryFilter){.include_triggers = true}, &hit));
    WPE_TEST_ASSERT_EQ_UINT(trigger, hit.entity);
    WPE_TEST_ASSERT_NEAR(2.0, hit.point.y, 1e-3);

    /* Shape casts stop where the swept shape first touches: the wall's face is at x = 4. */
    const WPE_QueryShape ball = {
        .shape = WPE_COLLIDER_SPHERE, .size = {{0.5f, 0, 0}}, .rotation = wpe_quat_identity()};
    WPE_TEST_ASSERT(
        wpe_physics_shape_cast(f.physics, &ball, wpe_vec3(0, 1, 0), wpe_vec3(2, 0, 0), 10.0f, NULL, &hit));
    WPE_TEST_ASSERT_EQ_UINT(wall, hit.entity);
    WPE_TEST_ASSERT_NEAR(3.5, hit.distance, 1e-2);
    WPE_TEST_ASSERT_NEAR(-1.0, hit.normal.x, 1e-3);
    WPE_TEST_ASSERT_NEAR(4.0, hit.point.x, 1e-2);
    WPE_TEST_ASSERT(
        !wpe_physics_shape_cast(f.physics, &ball, wpe_vec3(0, 1, 0), wpe_vec3(1, 0, 0), 3.0f, NULL, &hit));
    /* A box turned 45 degrees leads with its edge; a capsule lying along X with its cap. */
    const WPE_QueryShape turned = {.shape = WPE_COLLIDER_BOX,
                                   .size = {{1, 1, 1}},
                                   .rotation = wpe_quat_from_axis_angle(wpe_vec3(0, 1, 0), WPE_PI / 4.0f)};
    WPE_TEST_ASSERT(
        wpe_physics_shape_cast(f.physics, &turned, wpe_vec3(0, 1, 0), wpe_vec3(1, 0, 0), 10.0f, NULL, &hit));
    WPE_TEST_ASSERT_NEAR(4.0 - 0.5 * 1.41421356, hit.distance, 1e-2);
    const WPE_QueryShape lying = {.shape = WPE_COLLIDER_CAPSULE,
                                  .size = {{0.25f, 2.0f, 0}},
                                  .rotation = wpe_quat_from_axis_angle(wpe_vec3(0, 0, 1), WPE_PI / 2.0f)};
    WPE_TEST_ASSERT(
        wpe_physics_shape_cast(f.physics, &lying, wpe_vec3(0, 1, 0), wpe_vec3(1, 0, 0), 10.0f, NULL, &hit));
    WPE_TEST_ASSERT_NEAR(3.0, hit.distance, 1e-2);
    /* Starting inside something hits it at once. */
    WPE_TEST_ASSERT(
        wpe_physics_shape_cast(f.physics, &ball, wpe_vec3(5, 1, 0), wpe_vec3(0, 1, 0), 10.0f, NULL, &hit));
    WPE_TEST_ASSERT_EQ_UINT(wall, hit.entity);
    WPE_TEST_ASSERT_NEAR(0.0, hit.distance, 1e-3);

    /* Overlaps list each entity once; triggers only when asked; the count can exceed the capacity. */
    const WPE_QueryShape big = {
        .shape = WPE_COLLIDER_SPHERE, .size = {{1.5f, 0, 0}}, .rotation = wpe_quat_identity()};
    WPE_Entity found[8];
    uint32_t count = wpe_physics_overlap(f.physics, &big, wpe_vec3(5, 1, 0), NULL, found, 8);
    WPE_TEST_ASSERT_EQ_UINT(2, count);
    WPE_TEST_ASSERT(contains(found, count, wall) && contains(found, count, ground));
    WPE_TEST_ASSERT_EQ_UINT(2, wpe_physics_overlap(f.physics, &big, wpe_vec3(5, 1, 0), NULL, found, 1));
    WPE_TEST_ASSERT_EQ_UINT(2, wpe_physics_overlap(f.physics, &big, wpe_vec3(5, 1, 0), NULL, NULL, 0));
    count = wpe_physics_overlap(f.physics, &big, wpe_vec3(5, 1, 0), &(WPE_QueryFilter){.ignore = ground},
                                found, 8);
    WPE_TEST_ASSERT(count == 1 && found[0] == wall);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_physics_overlap(f.physics, &ball, wpe_vec3(-5, 1.2f, 0), NULL, found, 8));
    count = wpe_physics_overlap(f.physics, &ball, wpe_vec3(-5, 1.2f, 0),
                                &(WPE_QueryFilter){.include_triggers = true}, found, 8);
    WPE_TEST_ASSERT(count == 1 && found[0] == trigger);

    /* Shapes that can't be queried. */
    const WPE_QueryShape flat = {.shape = WPE_COLLIDER_SPHERE, .rotation = wpe_quat_identity()};
    WPE_TEST_ASSERT(
        !wpe_physics_shape_cast(f.physics, &flat, wpe_vec3(0, 1, 0), wpe_vec3(1, 0, 0), 10.0f, NULL, &hit));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_physics_overlap(f.physics, &flat, wpe_vec3(5, 1, 0), NULL, found, 8));
    const WPE_QueryShape mesh = {
        .shape = WPE_COLLIDER_MESH, .size = {{1, 1, 1}}, .rotation = wpe_quat_identity()};
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_physics_overlap(f.physics, &mesh, wpe_vec3(5, 1, 0), NULL, found, 8));
    destroy_world(&f);
}

/* ---- Character controllers --------------------------------------------------------------------------- */

static WPE_Entity spawn_character(Fixture* f, WPE_Vec3 feet) {
    const WPE_Entity e = spawn_at(f->ecs, feet);
    const WPE_CharacterController c = wpe_character_default();
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(f->ecs, e, WPE_COMP_CHARACTER, &c, NULL));
    return e;
}

static const WPE_CharacterController* character_of(const Fixture* f, WPE_Entity e) {
    const WPE_CharacterController* c = wpe_ecs_get_component(f->ecs, e, WPE_COMP_CHARACTER);
    WPE_TEST_ASSERT(c != NULL);
    return c;
}

/* Moves a character the way a game does: gravity added each frame, the returned velocity kept, and
 * a horizontal walking velocity imposed. Steps the world after each move when `step` is set. */
static WPE_Vec3 walk(Fixture* f, WPE_Entity e, WPE_Vec3 velocity, WPE_Vec3 horizontal, float seconds,
                     bool step) {
    const int frames = (int)(seconds / DT + 0.5f);
    for (int i = 0; i < frames; ++i) {
        velocity = wpe_vec3(horizontal.x, velocity.y - 9.81f * DT, horizontal.z);
        WPE_TEST_ASSERT_OK(wpe_character_move(f->physics, e, velocity, DT, &velocity));
        if (step) WPE_TEST_ASSERT_OK(wpe_physics_step(f->physics, DT));
    }
    return velocity;
}

static void test_physics_character_lands_walks_and_slides(void) {
    Fixture f = make_world(GRAVITY);
    const WPE_Entity ground = spawn_ground(f.ecs);
    WPE_Collider box = wpe_collider_default(WPE_COLLIDER_BOX);
    box.size = wpe_vec3(1.0f, 4.0f, 20.0f);
    spawn_static(f.ecs, wpe_vec3(6.0f, 2.0f, 0.0f), box); /* A wall with its face at x = 5.5. */
    const WPE_Entity e = spawn_character(&f, wpe_vec3(0.0f, 2.0f, 0.0f));
    WPE_TEST_ASSERT_OK(wpe_physics_create_bodies(f.physics));
    WPE_TEST_ASSERT(!character_of(&f, e)->grounded);

    /* Falls and lands: the downward speed is gone, and it stands on the ground. */
    WPE_Vec3 v = walk(&f, e, wpe_vec3(0, 0, 0), wpe_vec3(0, 0, 0), 1.0f, false);
    WPE_TEST_ASSERT_NEAR(0.0, transform_of(f.ecs, e)->position.y, 0.02);
    WPE_TEST_ASSERT(character_of(&f, e)->grounded);
    WPE_TEST_ASSERT_EQ_UINT(ground, character_of(&f, e)->ground);
    WPE_TEST_ASSERT_NEAR(1.0, character_of(&f, e)->ground_normal.y, 1e-3);
    WPE_TEST_ASSERT_NEAR(0.0, v.y, 1e-4);

    /* Walks at its speed, staying on the ground. */
    v = walk(&f, e, v, wpe_vec3(3, 0, 0), 1.0f, false);
    WPE_TEST_ASSERT_NEAR(3.0, transform_of(f.ecs, e)->position.x, 0.05);
    WPE_TEST_ASSERT_NEAR(0.0, transform_of(f.ecs, e)->position.y, 0.02);
    WPE_TEST_ASSERT(character_of(&f, e)->grounded);

    /* Into the wall at an angle: stopped in X (radius 0.4 from the face), sliding on in Z. */
    v = walk(&f, e, v, wpe_vec3(3, 0, 3), 1.0f, false);
    WPE_TEST_ASSERT_NEAR(5.1, transform_of(f.ecs, e)->position.x, 0.02);
    WPE_TEST_ASSERT(transform_of(f.ecs, e)->position.z > 2.5f);
    WPE_TEST_ASSERT_NEAR(0.0, v.x, 1e-3);
    WPE_TEST_ASSERT_NEAR(3.0, v.z, 1e-3);
    WPE_TEST_ASSERT(character_of(&f, e)->grounded);

    /* Bad arguments. */
    wpe_log_set_stderr_enabled(false);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG,
                           wpe_character_move(f.physics, e, wpe_vec3(NAN, 0, 0), DT, NULL));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_character_move(f.physics, e, v, 0.0f, NULL));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND, wpe_character_move(f.physics, ground, v, DT, NULL));
    wpe_log_set_stderr_enabled(true);
    destroy_world(&f);
}

/* A static ramp rising toward +X at `degrees`, its surface crossing y = 0 near x = 3. */
static void spawn_ramp(Fixture* f, float degrees) {
    const float angle = degrees * (WPE_PI / 180.0f);
    WPE_Collider box = wpe_collider_default(WPE_COLLIDER_BOX);
    box.size = wpe_vec3(12.0f, 1.0f, 6.0f);
    const WPE_Vec3 surface_up = wpe_vec3(-sinf(angle), cosf(angle), 0.0f);
    const WPE_Vec3 along = wpe_vec3(cosf(angle), sinf(angle), 0.0f);
    /* Center: from the point (3, 0, 0) on the surface, half the length up the ramp and half the thickness in.
     */
    const WPE_Vec3 center =
        wpe_vec3_add(wpe_vec3(3.0f, 0.0f, 0.0f),
                     wpe_vec3_sub(wpe_vec3_scale(along, 6.0f), wpe_vec3_scale(surface_up, 0.5f)));
    const WPE_Entity ramp = spawn_static(f->ecs, center, box);
    transform_of(f->ecs, ramp)->rotation = wpe_quat_from_axis_angle(wpe_vec3(0, 0, 1), angle);
}

static void test_physics_character_slopes(void) {
    /* Walkable: it walks up the slope, and standing on it doesn't slide down. */
    Fixture f = make_world(GRAVITY);
    spawn_ground(f.ecs);
    spawn_ramp(&f, 30.0f);
    WPE_Entity e = spawn_character(&f, wpe_vec3(0.0f, 0.5f, 0.0f));
    WPE_TEST_ASSERT_OK(wpe_physics_create_bodies(f.physics));
    WPE_Vec3 v = walk(&f, e, wpe_vec3(0, 0, 0), wpe_vec3(0, 0, 0), 0.5f, false);
    v = walk(&f, e, v, wpe_vec3(3, 0, 0), 2.0f, false);
    const WPE_Vec3 up_slope = transform_of(f.ecs, e)->position;
    WPE_TEST_ASSERT(up_slope.y > 1.0f);
    WPE_TEST_ASSERT(character_of(&f, e)->grounded);
    WPE_TEST_ASSERT_NEAR(cos(30.0 * WPE_PI / 180.0), character_of(&f, e)->ground_normal.y, 1e-2);
    walk(&f, e, v, wpe_vec3(0, 0, 0), 1.0f, false);
    WPE_TEST_ASSERT_NEAR(up_slope.x, transform_of(f.ecs, e)->position.x, 0.01);
    WPE_TEST_ASSERT_NEAR(up_slope.y, transform_of(f.ecs, e)->position.y, 0.01);

    /* Walking back down keeps it on the slope (snapping), every frame. */
    for (int i = 0; i < 30; ++i) {
        v = wpe_vec3(-3.0f, v.y - 9.81f * DT, 0.0f);
        WPE_TEST_ASSERT_OK(wpe_character_move(f.physics, e, v, DT, &v));
        WPE_TEST_ASSERT(character_of(&f, e)->grounded);
    }
    WPE_TEST_ASSERT(transform_of(f.ecs, e)->position.y < up_slope.y - 0.5f);
    destroy_world(&f);

    /* Too steep: it can't walk up, and isn't standing on it. */
    f = make_world(GRAVITY);
    spawn_ground(f.ecs);
    spawn_ramp(&f, 60.0f);
    e = spawn_character(&f, wpe_vec3(0.0f, 0.5f, 0.0f));
    WPE_TEST_ASSERT_OK(wpe_physics_create_bodies(f.physics));
    v = walk(&f, e, wpe_vec3(0, 0, 0), wpe_vec3(0, 0, 0), 0.5f, false);
    v = walk(&f, e, v, wpe_vec3(3, 0, 0), 2.0f, false);
    WPE_TEST_ASSERT(transform_of(f.ecs, e)->position.y < 0.05f);
    WPE_TEST_ASSERT(transform_of(f.ecs, e)->position.x < 3.0f);
    WPE_TEST_ASSERT(character_of(&f, e)->grounded); /* On the flat ground in front of it. */
    WPE_TEST_ASSERT_NEAR(1.0, character_of(&f, e)->ground_normal.y, 1e-3);
    WPE_TEST_ASSERT(v.y <= 0.0f);
    destroy_world(&f);
}

/* Five static steps 0.2 tall and 0.5 deep, rising toward +X from x = 2, the last a 4-deep landing. */
static void spawn_stairs(Fixture* f) {
    for (int i = 0; i < 5; ++i) {
        const float depth = i < 4 ? 0.5f : 4.0f;
        WPE_Collider box = wpe_collider_default(WPE_COLLIDER_BOX);
        box.size = wpe_vec3(depth, 0.2f * (float)(i + 1), 4.0f);
        spawn_static(f->ecs, wpe_vec3(2.0f + 0.5f * (float)i + depth * 0.5f, 0.1f * (float)(i + 1), 0.0f),
                     box);
    }
}

static void test_physics_character_steps(void) {
    /* Climbs stairs to the top, standing on each step. */
    Fixture f = make_world(GRAVITY);
    spawn_ground(f.ecs);
    spawn_stairs(&f);
    WPE_Entity e = spawn_character(&f, wpe_vec3(0.0f, 0.0f, 0.0f));
    WPE_TEST_ASSERT_OK(wpe_physics_create_bodies(f.physics));
    WPE_Vec3 v = walk(&f, e, wpe_vec3(0, 0, 0), wpe_vec3(0, 0, 0), 0.2f, false);
    walk(&f, e, v, wpe_vec3(2, 0, 0), 2.5f, false);
    WPE_TEST_ASSERT(transform_of(f.ecs, e)->position.x > 4.3f);
    WPE_TEST_ASSERT_NEAR(1.0, transform_of(f.ecs, e)->position.y, 0.02);
    WPE_TEST_ASSERT(character_of(&f, e)->grounded);
    destroy_world(&f);

    /* Without step_height they stop it. */
    f = make_world(GRAVITY);
    spawn_ground(f.ecs);
    spawn_stairs(&f);
    e = spawn_character(&f, wpe_vec3(0.0f, 0.0f, 0.0f));
    ((WPE_CharacterController*)wpe_ecs_get_component(f.ecs, e, WPE_COMP_CHARACTER))->step_height = 0.0f;
    WPE_TEST_ASSERT_OK(wpe_physics_create_bodies(f.physics));
    v = walk(&f, e, wpe_vec3(0, 0, 0), wpe_vec3(0, 0, 0), 0.2f, false);
    walk(&f, e, v, wpe_vec3(2, 0, 0), 2.5f, false);
    WPE_TEST_ASSERT(transform_of(f.ecs, e)->position.x < 1.7f); /* The rounded bottom against the edge. */
    WPE_TEST_ASSERT_NEAR(0.0, transform_of(f.ecs, e)->position.y, 0.02);
    destroy_world(&f);

    /* A ledge taller than step_height stops it too. */
    f = make_world(GRAVITY);
    spawn_ground(f.ecs);
    WPE_Collider ledge = wpe_collider_default(WPE_COLLIDER_BOX);
    ledge.size = wpe_vec3(1.0f, 0.5f, 4.0f);
    spawn_static(f.ecs, wpe_vec3(2.5f, 0.25f, 0.0f), ledge);
    e = spawn_character(&f, wpe_vec3(0.0f, 0.0f, 0.0f));
    WPE_TEST_ASSERT_OK(wpe_physics_create_bodies(f.physics));
    v = walk(&f, e, wpe_vec3(0, 0, 0), wpe_vec3(0, 0, 0), 0.2f, false);
    walk(&f, e, v, wpe_vec3(2, 0, 0), 2.0f, false);
    WPE_TEST_ASSERT_NEAR(1.6, transform_of(f.ecs, e)->position.x, 0.02);
    WPE_TEST_ASSERT_NEAR(0.0, transform_of(f.ecs, e)->position.y, 0.02);
    destroy_world(&f);
}

static void test_physics_character_jumps_into_a_ceiling(void) {
    Fixture f = make_world(GRAVITY);
    spawn_ground(f.ecs);
    WPE_Collider slab = wpe_collider_default(WPE_COLLIDER_BOX);
    slab.size = wpe_vec3(10.0f, 0.5f, 10.0f);
    spawn_static(f.ecs, wpe_vec3(0.0f, 2.75f, 0.0f), slab); /* Its underside at y = 2.5. */
    const WPE_Entity e = spawn_character(&f, wpe_vec3(0.0f, 0.0f, 0.0f));
    WPE_TEST_ASSERT_OK(wpe_physics_create_bodies(f.physics));
    WPE_Vec3 v = walk(&f, e, wpe_vec3(0, 0, 0), wpe_vec3(0, 0, 0), 0.2f, false);
    WPE_TEST_ASSERT(character_of(&f, e)->grounded);
    /* A jump: the head (1.8 up) stops at the slab, and the upward speed is gone at once. */
    v.y = 8.0f;
    float highest = 0.0f;
    bool bumped = false;
    for (int i = 0; i < 60; ++i) {
        const float before = v.y;
        WPE_TEST_ASSERT_OK(wpe_character_move(f.physics, e, v, DT, &v));
        const float y = transform_of(f.ecs, e)->position.y;
        highest = y > highest ? y : highest;
        if (before > 0.0f && v.y == 0.0f) bumped = true;
        v.y -= 9.81f * DT;
    }
    WPE_TEST_ASSERT(bumped);
    WPE_TEST_ASSERT(highest > 0.6f && highest < 0.71f);
    WPE_TEST_ASSERT(character_of(&f, e)->grounded); /* Back down. */
    WPE_TEST_ASSERT_NEAR(0.0, transform_of(f.ecs, e)->position.y, 0.02);
    destroy_world(&f);
}

static void test_physics_character_pushes_bodies_and_enters_triggers(void) {
    Fixture f = make_world(GRAVITY);
    spawn_ground(f.ecs);
    const WPE_Entity crate =
        spawn_dynamic(f.ecs, wpe_vec3(3.0f, 0.5f, 0.0f), wpe_collider_default(WPE_COLLIDER_BOX));
    WPE_Collider zone = wpe_collider_default(WPE_COLLIDER_BOX);
    zone.size = wpe_vec3(1.0f, 2.0f, 1.0f);
    zone.is_trigger = true;
    const WPE_Entity trigger = spawn_static(f.ecs, wpe_vec3(0.0f, 1.0f, -3.0f), zone);
    const WPE_Entity e = spawn_character(&f, wpe_vec3(0.0f, 0.0f, 0.0f));
    run(&f, 0.5f); /* Creates the bodies; the crate settles. */

    /* Rays from inside the capsule hit it, unless told to ignore it. */
    WPE_RaycastHit hit;
    WPE_TEST_ASSERT(wpe_physics_raycast(f.physics, wpe_vec3(0, 1, 0), wpe_vec3(0, -1, 0), 5.0f, NULL, &hit));
    WPE_TEST_ASSERT_EQ_UINT(e, hit.entity);
    WPE_TEST_ASSERT(wpe_physics_raycast(f.physics, wpe_vec3(0, 1, 0), wpe_vec3(0, -1, 0), 5.0f,
                                        &(WPE_QueryFilter){.ignore = e}, &hit));
    WPE_TEST_ASSERT(hit.entity != e);

    /* Walking into the crate pushes it along. */
    walk(&f, e, wpe_vec3(0, 0, 0), wpe_vec3(2, 0, 0), 2.0f, true);
    WPE_TEST_ASSERT(transform_of(f.ecs, crate)->position.x > 4.0f);
    WPE_TEST_ASSERT(transform_of(f.ecs, e)->position.x > 2.5f);

    /* A crate pinned against a wall stops it (sunk in no deeper than a few centimeters). */
    WPE_Collider block = wpe_collider_default(WPE_COLLIDER_BOX);
    block.size = wpe_vec3(1.0f, 3.0f, 6.0f);
    spawn_static(f.ecs, wpe_vec3(transform_of(f.ecs, crate)->position.x + 1.5f, 1.5f, 0.0f), block);
    walk(&f, e, wpe_vec3(0, 0, 0), wpe_vec3(2, 0, 0), 3.0f, true);
    const float crate_face = transform_of(f.ecs, crate)->position.x - 0.5f;
    WPE_TEST_ASSERT(transform_of(f.ecs, e)->position.x + 0.4f < crate_face + 0.08f);

    /* Walking through the trigger enters it (the character isn't blocked). */
    transform_of(f.ecs, e)->position = wpe_vec3(0.0f, 0.0f, 0.0f);
    run(&f, DT);
    bool entered = false;
    const int frames = (int)(2.0f / DT);
    WPE_Vec3 v = wpe_vec3(0, 0, 0);
    for (int i = 0; i < frames && !entered; ++i) {
        v = wpe_vec3(0.0f, v.y - 9.81f * DT, -2.0f);
        WPE_TEST_ASSERT_OK(wpe_character_move(f.physics, e, v, DT, &v));
        WPE_TEST_ASSERT_OK(wpe_physics_step(f.physics, DT));
        entered = has_event(&f, WPE_PHYSICS_TRIGGER_ENTER, trigger, e);
    }
    WPE_TEST_ASSERT(entered);
    destroy_world(&f);
}

static void test_physics_character_component_lifecycle(void) {
    Fixture f = make_world(GRAVITY);
    const WPE_Entity e = spawn_character(&f, wpe_vec3(0.0f, 0.0f, 0.0f));
    WPE_TEST_ASSERT_OK(wpe_physics_create_bodies(f.physics));
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_physics_stats(f.physics).bodies);
    /* A copy gets its own body. */
    const WPE_Entity copy = spawn_at(f.ecs, wpe_vec3(3.0f, 0.0f, 0.0f));
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(f.ecs, copy, WPE_COMP_CHARACTER, character_of(&f, e), NULL));
    WPE_TEST_ASSERT_OK(wpe_physics_create_bodies(f.physics));
    WPE_TEST_ASSERT_EQ_UINT(2, wpe_physics_stats(f.physics).bodies);
    /* Invalid settings are reported and leave it without a body until refreshed. */
    WPE_CharacterController* c = wpe_ecs_get_component(f.ecs, copy, WPE_COMP_CHARACTER);
    c->radius = 0.0f;
    wpe_log_set_stderr_enabled(false);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_physics_refresh(f.physics, copy));
    wpe_log_set_stderr_enabled(true);
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_physics_stats(f.physics).bodies);
    c = wpe_ecs_get_component(f.ecs, copy, WPE_COMP_CHARACTER);
    c->radius = 0.3f;
    WPE_TEST_ASSERT_OK(wpe_physics_refresh(f.physics, copy));
    WPE_TEST_ASSERT_EQ_UINT(2, wpe_physics_stats(f.physics).bodies);
    /* Removing the component, or despawning, releases the body. */
    WPE_TEST_ASSERT_OK(wpe_ecs_remove_component(f.ecs, e, WPE_COMP_CHARACTER));
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_physics_stats(f.physics).bodies);
    WPE_TEST_ASSERT_OK(wpe_ecs_despawn(f.ecs, copy));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_physics_stats(f.physics).bodies);
    /* And physics can go with characters still in the world. */
    spawn_character(&f, wpe_vec3(0.0f, 0.0f, 0.0f));
    WPE_TEST_ASSERT_OK(wpe_physics_create_bodies(f.physics));
    destroy_world(&f);
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_physics_shape_casts_overlaps_and_filters),
              WPE_TEST_CASE(test_physics_character_lands_walks_and_slides),
              WPE_TEST_CASE(test_physics_character_slopes), WPE_TEST_CASE(test_physics_character_steps),
              WPE_TEST_CASE(test_physics_character_jumps_into_a_ceiling),
              WPE_TEST_CASE(test_physics_character_pushes_bodies_and_enters_triggers),
              WPE_TEST_CASE(test_physics_character_component_lifecycle),
              WPE_TEST_CASE(test_physics_create_validates_world),
              WPE_TEST_CASE(test_physics_create_bodies_and_initial_velocity),
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
              WPE_TEST_CASE(test_physics_sync_follows_edits),
              WPE_TEST_CASE(test_physics_degenerate_transform_is_reported_once),
              WPE_TEST_CASE(test_physics_debug_draw))
