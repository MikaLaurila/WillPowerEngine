#define WPE_LOG_MODULE "graphics"
#include <math.h>
#include <string.h>

#include "core/alloc.h"
#include "core/log.h"
#include "graphics/billboard_internal.h"

#define WPE__PARTICLE_GRAIN 2048u /* Particles per job chunk; smaller emitters stay on this thread. */
#define WPE__TWO_PI 6.28318530717958647692f

WPE_ParticleEmitter wpe_particle_emitter_default(void) {
    return (WPE_ParticleEmitter){.columns = 1,
                                 .rows = 1,
                                 .blend = WPE_BILLBOARD_ALPHA,
                                 .color_start = wpe_vec4(1, 1, 1, 1),
                                 .color_end = wpe_vec4(1, 1, 1, 0),
                                 .size_start = 0.2f,
                                 .size_end = 0.05f,
                                 .playing = true,
                                 .loop = true,
                                 .rate = 20.0f,
                                 .max_particles = 256,
                                 .world_space = true,
                                 .shape_size = wpe_vec3(1, 1, 1),
                                 .lifetime_min = 1.0f,
                                 .lifetime_max = 1.0f,
                                 .speed_min = 1.0f,
                                 .speed_max = 2.0f,
                                 .direction = wpe_vec3(0, 1, 0),
                                 .spread = 15.0f * WPE_PI / 180.0f};
}

/* ---- Random numbers: xorshift64*, seeded through splitmix64 so nearby seeds differ. ------------------- */

static uint64_t wpe__seed(uint64_t seed) {
    uint64_t z = seed + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return z ? z : 1;
}

static uint64_t wpe__next(uint64_t* state) {
    uint64_t x = *state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * 0x2545F4914F6CDD1Dull;
}

/* Uniform in [0, 1). */
static float wpe__unit(uint64_t* state) {
    return (float)(wpe__next(state) >> 40) / 16777216.0f;
}

static float wpe__between(uint64_t* state, float a, float b) {
    return a + (b - a) * wpe__unit(state);
}

/* ---- Spawning ---------------------------------------------------------------------------------------- */

/* A unit direction within `spread` radians of `axis` (uniform over the cone's cap). */
static WPE_Vec3 wpe__cone(uint64_t* random, WPE_Vec3 axis, float spread) {
    const float cos_theta = 1.0f - wpe__unit(random) * (1.0f - cosf(spread));
    const float sin_theta = sqrtf(fmaxf(0.0f, 1.0f - cos_theta * cos_theta));
    const float phi = wpe__unit(random) * WPE__TWO_PI;
    /* Any two directions perpendicular to the axis. */
    const WPE_Vec3 helper = fabsf(axis.y) < 0.99f ? wpe_vec3(0, 1, 0) : wpe_vec3(1, 0, 0);
    const WPE_Vec3 u = wpe_vec3_normalize(wpe_vec3_cross(helper, axis));
    const WPE_Vec3 v = wpe_vec3_cross(axis, u);
    return wpe_vec3_add(
        wpe_vec3_scale(axis, cos_theta),
        wpe_vec3_add(wpe_vec3_scale(u, sin_theta * cosf(phi)), wpe_vec3_scale(v, sin_theta * sinf(phi))));
}

/* Where a new particle starts, in the emitter's space. */
static WPE_Vec3 wpe__spawn_point(uint64_t* random, const WPE_ParticleEmitter* e) {
    switch (e->shape) {
    case WPE_PARTICLES_SPHERE: {
        /* Uniform in the ball: a random direction at a radius growing with the cube root. */
        const WPE_Vec3 direction = wpe__cone(random, wpe_vec3(0, 1, 0), WPE_PI);
        return wpe_vec3_scale(direction, e->shape_size.x * cbrtf(wpe__unit(random)));
    }
    case WPE_PARTICLES_BOX:
        return wpe_vec3((wpe__unit(random) - 0.5f) * e->shape_size.x,
                        (wpe__unit(random) - 0.5f) * e->shape_size.y,
                        (wpe__unit(random) - 0.5f) * e->shape_size.z);
    case WPE_PARTICLES_POINT:
    default:
        return wpe_vec3(0, 0, 0);
    }
}

/* Makes room for max_particles (keeping the oldest that fit). */
static bool wpe__reserve(WPE_ParticleEmitter* e) {
    const uint32_t wanted = e->max_particles ? e->max_particles : 1;
    if (e->capacity == wanted) return true;
    WPE_Particle* particles = wpe_realloc(e->particles, (size_t)wanted * sizeof *particles);
    if (!particles) {
        wpe_log_error("out of memory for %u particles", wanted);
        return false;
    }
    e->particles = particles;
    e->capacity = wanted;
    if (e->count > wanted) e->count = wanted;
    return true;
}

static uint32_t wpe__emit(WPE_ParticleEmitter* e, const WPE_Mat4* world, uint32_t count) {
    if (!wpe__reserve(e)) return 0;
    const WPE_Vec3 axis =
        wpe_vec3_length(e->direction) > 1e-6f ? wpe_vec3_normalize(e->direction) : wpe_vec3(0, 1, 0);
    uint32_t emitted = 0;
    for (; emitted < count && e->count < e->capacity; ++emitted) {
        WPE_Vec3 position = wpe__spawn_point(&e->random, e);
        WPE_Vec3 velocity = wpe_vec3_scale(wpe__cone(&e->random, axis, e->spread),
                                           wpe__between(&e->random, e->speed_min, e->speed_max));
        if (e->world_space) {
            /* Into the world: positions with the emitter's whole transform, velocities turned (their speed
             * stays in world units). */
            position = wpe_mat4_transform_point(world, position);
            const float speed = wpe_vec3_length(velocity);
            const WPE_Vec3 turned = wpe_mat4_transform_dir(world, velocity);
            velocity = wpe_vec3_length(turned) > 1e-9f ? wpe_vec3_scale(wpe_vec3_normalize(turned), speed)
                                                       : velocity;
        }
        e->particles[e->count++] = (WPE_Particle){
            .position = position,
            .velocity = velocity,
            .lifetime = fmaxf(wpe__between(&e->random, e->lifetime_min, e->lifetime_max), 1e-3f),
            .rotation = e->random_rotation ? wpe__unit(&e->random) * WPE__TWO_PI : 0.0f,
            .spin = wpe__between(&e->random, e->spin_min, e->spin_max),
        };
    }
    return emitted;
}

/* ---- Simulation -------------------------------------------------------------------------------------- */

typedef struct wpe__Integrate {
    WPE_Particle* particles;
    WPE_Vec3 gravity; /* In the particles' space. */
    float damping;    /* Velocity kept this step. */
    float dt;
} wpe__Integrate;

static void wpe__integrate(void* context, uint32_t begin, uint32_t end, uint32_t thread_index) {
    (void)thread_index;
    const wpe__Integrate* s = context;
    for (uint32_t i = begin; i < end; ++i) {
        WPE_Particle* p = &s->particles[i];
        p->age += s->dt;
        p->velocity =
            wpe_vec3_scale(wpe_vec3_add(p->velocity, wpe_vec3_scale(s->gravity, s->dt)), s->damping);
        p->position = wpe_vec3_add(p->position, wpe_vec3_scale(p->velocity, s->dt));
        p->rotation += p->spin * s->dt;
    }
}

static WPE_Result wpe__advance(WPE_ParticleEmitter* e, const WPE_Mat4* world, WPE_JobSystem* jobs, float dt) {
    if (!(dt > 0.0f)) return WPE_RESULT_OK;
    if (!wpe__reserve(e)) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    /* Move what is alive (in order, chunk by chunk, so the result never depends on the threads)... */
    if (e->count > 0) {
        wpe__Integrate s = {.particles = e->particles,
                            .gravity = e->gravity,
                            .damping = fmaxf(0.0f, 1.0f - e->drag * dt),
                            .dt = dt};
        WPE_Mat4 inverse;
        if (!e->world_space && wpe_mat4_inverse(world, &inverse))
            s.gravity = wpe_mat4_transform_dir(&inverse, e->gravity);
        if (jobs && e->count > WPE__PARTICLE_GRAIN) {
            wpe_jobs_parallel_for(jobs, e->count, WPE__PARTICLE_GRAIN, wpe__integrate, &s);
        } else {
            wpe__integrate(&s, 0, e->count, 0);
        }
        /* ...drop the expired ones, keeping the order... */
        uint32_t kept = 0;
        for (uint32_t i = 0; i < e->count; ++i) {
            if (e->particles[i].age < e->particles[i].lifetime) e->particles[kept++] = e->particles[i];
        }
        e->count = kept;
    }
    /* ...then emit: a cycle's burst when it starts, and the steady rate while playing. */
    if (!e->playing) return WPE_RESULT_OK;
    if (!e->started) {
        e->started = true;
        e->time = 0.0f;
        wpe__emit(e, world, e->burst);
    }
    e->time += dt;
    if (e->duration > 0.0f && e->time >= e->duration) {
        if (e->loop) {
            e->time = fmodf(e->time, e->duration);
            wpe__emit(e, world, e->burst);
        } else {
            e->playing = false;
            e->started = false;
            return WPE_RESULT_OK;
        }
    }
    e->accumulator += e->rate * dt;
    const float whole = floorf(e->accumulator);
    if (whole >= 1.0f) {
        e->accumulator -= whole;
        wpe__emit(e, world, whole > 1e6f ? 1000000u : (uint32_t)whole);
    }
    return WPE_RESULT_OK;
}

static WPE_Mat4 wpe__world_of(const WPE_Ecs* ecs, WPE_Entity entity) {
    const WPE_Transform* t = wpe_ecs_get_component(ecs, entity, WPE_COMP_TRANSFORM);
    return t ? t->world : wpe_mat4_identity();
}

WPE_Result wpe_particles_update(WPE_Ecs* ecs, WPE_JobSystem* jobs, float dt) {
    if (!ecs) return WPE_RESULT_ERROR_NULL_ARG;
    WPE_Result result = WPE_RESULT_OK;
    WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_PARTICLES)});
    while (wpe_ecs_iter_next(&it)) {
        const WPE_Mat4 world = wpe__world_of(ecs, it.entity);
        const WPE_Result advanced = wpe__advance(wpe_ecs_iter_get(&it, WPE_COMP_PARTICLES), &world, jobs, dt);
        if (result == WPE_RESULT_OK) result = advanced;
    }
    return result;
}

uint32_t wpe_particles_emit(WPE_Ecs* ecs, WPE_Entity entity, uint32_t count) {
    WPE_ParticleEmitter* e = ecs ? wpe_ecs_get_component(ecs, entity, WPE_COMP_PARTICLES) : NULL;
    if (!e) return 0;
    const WPE_Mat4 world = wpe__world_of(ecs, entity);
    return wpe__emit(e, &world, count);
}

WPE_Result wpe_particles_preview(WPE_Ecs* ecs, WPE_Entity entity, float dt) {
    if (!ecs) return WPE_RESULT_ERROR_NULL_ARG;
    WPE_ParticleEmitter* e = wpe_ecs_get_component(ecs, entity, WPE_COMP_PARTICLES);
    if (!e) return WPE_RESULT_OK;
    /* Played and looped for the occasion; the settings are put back as they were. */
    const bool playing = e->playing, loop = e->loop;
    e->playing = true;
    e->loop = true;
    const WPE_Mat4 world = wpe__world_of(ecs, entity);
    const WPE_Result result = wpe__advance(e, &world, NULL, dt);
    e->playing = playing;
    e->loop = loop;
    return result;
}

void wpe_particles_clear(WPE_Ecs* ecs, WPE_Entity entity) {
    WPE_ParticleEmitter* e = ecs ? wpe_ecs_get_component(ecs, entity, WPE_COMP_PARTICLES) : NULL;
    if (!e) return;
    e->count = 0;
    e->time = 0.0f;
    e->accumulator = 0.0f;
    e->started = false;
}

void wpe_particles_restart(WPE_Ecs* ecs, WPE_Entity entity) {
    WPE_ParticleEmitter* e = ecs ? wpe_ecs_get_component(ecs, entity, WPE_COMP_PARTICLES) : NULL;
    if (!e) return;
    wpe_particles_clear(ecs, entity);
    e->playing = true;
}

/* ---- Hooks (registered in components.c) ------------------------------------------------------------- */

/* The runtime state starts empty whatever the initial data held (a copy of another emitter shares nothing).
 */
void wpe__particles_on_add(WPE_Ecs* ecs, WPE_Entity entity, void* component, void* user_data) {
    (void)ecs;
    WPE_ParticleEmitter* e = component;
    e->particles = NULL;
    e->count = e->capacity = 0;
    e->time = e->accumulator = 0.0f;
    e->started = false;
    e->random = wpe__seed(e->seed ? e->seed : entity);
    e->texture = wpe_asset_image(user_data, e->image);
    if (e->texture) {
        wpe_asset_retain(user_data, e->image);
    } else if (e->image != WPE_ASSET_NONE) {
        wpe_log_error("the particles of entity 0x%08x reference 0x%08x, which is not a loaded image", entity,
                      e->image);
    }
}

void wpe__particles_on_remove(WPE_Ecs* ecs, WPE_Entity entity, void* component, void* user_data) {
    (void)ecs;
    (void)entity;
    WPE_ParticleEmitter* e = component;
    wpe_free(e->particles);
    e->particles = NULL;
    e->count = e->capacity = 0;
    if (e->texture) wpe_asset_release(user_data, e->image);
    e->texture = NULL;
}
