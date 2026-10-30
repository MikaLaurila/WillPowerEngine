/**
 * @file graphics.h
 * @brief Scene-level graphics: mesh/camera/animator components, model instantiation, animation,
 *        material shading, scene rendering and 2D drawing.
 *
 * Components registered by wpe_graphics_register_components():
 * - WPE_COMP_MESH (WPE_MeshRenderer): draws one mesh of a WPE_Model with its materials. Skinned
 *   meshes reference the joint entities that drive them.
 * - WPE_COMP_CAMERA (WPE_Camera): projection parameters; the entity's Transform places the camera
 *   (it looks down its local -Z axis with +Y up).
 * - WPE_COMP_LIGHT (WPE_Light): directional, point or spot light placed by the entity's Transform.
 * - WPE_COMP_ANIMATOR (WPE_Animator): plays one of a model's animations on the entities created by
 *   wpe_model_instantiate().
 * - WPE_COMP_BILLBOARD (WPE_Billboard): a textured quad (3D sprite) that can turn toward the camera,
 *   with sprite-sheet frames, cutout/alpha/additive blending and optional lighting. Drawn by
 *   wpe_scene_render(); its image reference is held from the add hook to the remove hook.
 *   NOTE(limitation): billboards do not cast shadows, and their images are sampled without mipmaps.
 * - WPE_COMP_PARTICLES (WPE_ParticleEmitter): emits and simulates particles (wpe_particles_update()),
 *   drawn by wpe_scene_render() as screen-facing quads through the billboard shader, one draw per
 *   emitter. Its particle array is owned by the component (freed by its remove hook).
 * - WPE_COMP_TRAIL (WPE_Trail): a ribbon between two moving entities (a blade's base and tip) through where
 *   they were lately (wpe_trail_update()), drawn by wpe_scene_render() through the billboard shader.
 * - WPE_COMP_SLASH (WPE_Slash): a slash sweeping along a Bezier curve (wpe_slash_update()), drawn by
 *   wpe_scene_render() as a camera-facing ribbon, thin at both ends.
 *
 * Asset lifetime: mesh renderers and animators reference their model by asset handle
 * (`model_asset`). Their add hook takes a reference and resolves the cached `model` pointer; their
 * remove hook releases it, so a model lives as long as any entity uses it. Set `model_asset` before
 * adding the component and treat `model` as read-only; to switch models, remove and re-add the
 * component. An invalid handle leaves `model` NULL and the component is ignored.
 *
 * - Dependencies: core, math, ecs, scene, renderer, assets (asset manager)
 * - Thread affinity: same as the owning WPE_Ecs world
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_GRAPHICS_GRAPHICS_H
#define WPE_GRAPHICS_GRAPHICS_H

#include "assets/asset_manager.h"
#include "assets/model.h"
#include "ecs/ecs.h"
#include "graphics/draw2d.h"
#include "jobs/jobs.h"
#include "scene/scene.h"

typedef struct WPE_MeshRenderer {
    WPE_AssetHandle model_asset; /* WPE_ASSET_MODEL; the component holds a reference. */
    const WPE_Model* model;      /* Resolved from model_asset by the add hook (read-only). */
    uint32_t mesh;               /* Index into model->meshes. */
    uint32_t skin;               /* Index into model->skins, or WPE_MODEL_NONE. */
    WPE_Entity* joints; /* Owned (freed by the component's remove hook): one entity per skin joint. */
    uint32_t joint_count;
    bool visible;
    bool unlit; /* Its own colors (texture x material), no lighting; the sun's shadow may still darken it. */
    bool cast_shadows;    /* Drawn into the sun's shadow map (default true). */
    bool receive_shadows; /* Darkened by the sun's shadow (default true). */
    bool double_sided;    /* Both faces drawn and lit even if the material is single-sided (default false). */
} WPE_MeshRenderer;

typedef struct WPE_Camera {
    float fov_y;        /* Vertical field of view in radians (perspective). */
    float near_z;       /* Near plane distance, > 0. */
    float far_z;        /* Far plane distance, > near_z. */
    bool orthographic;  /* Use ortho_height instead of fov_y. */
    float ortho_height; /* Visible world-space height for orthographic cameras. */
} WPE_Camera;

typedef enum WPE_LightType {
    WPE_LIGHT_DIRECTIONAL = 0, /* Shines along the entity's local -Z axis; position ignored. */
    WPE_LIGHT_POINT,           /* Omnidirectional from the entity's position. */
    WPE_LIGHT_SPOT,            /* Cone along the entity's local -Z axis. */
} WPE_LightType;

typedef struct WPE_Light {
    WPE_LightType type;
    WPE_Vec3 color; /* Linear-ish RGB in [0, 1] (gamma space shading); multiplied by intensity. */
    float intensity;
    float range;       /* Point/spot: distance at which the light fades to zero (> 0). */
    float inner_cone;  /* Spot: half-angle in radians of full intensity. */
    float outer_cone;  /* Spot: half-angle in radians where intensity reaches zero (>= inner_cone). */
    bool cast_shadows; /* Directional only (the main sun). */
} WPE_Light;

typedef struct WPE_Animator {
    WPE_AssetHandle model_asset; /* WPE_ASSET_MODEL; the component holds a reference. */
    const WPE_Model* model;      /* Resolved from model_asset by the add hook (read-only). */
    uint32_t animation;          /* Index into model->animations. */
    float time;                  /* Current playback position in seconds. */
    float speed;                 /* Playback rate multiplier (negative plays backwards). */
    bool playing;
    bool loop;
    WPE_Entity* node_entities; /* Owned (freed by the remove hook): entity per model node. */
    uint32_t node_count;

    /* Crossfade (wpe_animator_play with a fade): the previous animation keeps playing underneath
     * while `animation` fades in over fade_duration seconds. Not fading while fade_duration is 0. */
    uint32_t from_animation;
    float from_time;
    float from_speed;
    bool from_loop;
    float fade;          /* Seconds into the fade. */
    float fade_duration; /* 0: no crossfade. */
} WPE_Animator;

/** How a billboard turns. */
typedef enum WPE_BillboardMode {
    WPE_BILLBOARD_FIXED = 0,    /* A double-sided quad in the entity's XY plane, turned by its transform. */
    WPE_BILLBOARD_AXIS_ALIGNED, /* Faces the camera from any side (above too), kept upright (no tilt or roll).
                                 */
    WPE_BILLBOARD_AXIS_ALIGNED_Y, /* Turns about the world's Y axis to face the camera (trees, characters). */
    WPE_BILLBOARD_SCREEN_ALIGNED, /* Always faces the screen (particles, icons). */
} WPE_BillboardMode;

/** How a billboard's pixels combine with what is behind them. */
typedef enum WPE_BillboardBlend {
    WPE_BILLBOARD_CUTOUT = 0, /* Opaque where alpha >= 0.5, cut out elsewhere; no sorting needed. */
    WPE_BILLBOARD_ALPHA,      /* Alpha blended, drawn back to front after opaque geometry. */
    WPE_BILLBOARD_ADDITIVE,   /* Added on top (glows, fire, sparks), drawn after opaque geometry. */
} WPE_BillboardBlend;

/** A textured quad in the world (a 3D sprite), placed by the entity's transform. */
typedef struct WPE_Billboard {
    WPE_AssetHandle image;      /* WPE_ASSET_IMAGE, or WPE_ASSET_NONE for a plain colored quad. */
    const WPE_Texture* texture; /* Resolved from `image` by the add hook (read-only). */
    WPE_Vec2 size;              /* World units (times the entity's world scale). */
    WPE_Vec2 pivot;             /* The point of the quad at the entity's position ((0.5, 0.5) its center,
                                   (0.5, 0) the middle of its bottom edge). */
    float rotation;             /* Radians, in the quad's plane. */
    WPE_Vec4 color;             /* RGBA (0..1) multiplying the image. */
    WPE_Rect source;            /* Region of the image in texels; zero width or height = all of it. */
    /* Sprite-sheet animation: the region is a grid of `columns` x `rows` frames, read left to right, top
     * to bottom. `frame_count` frames are used (0 = all of the grid). */
    uint32_t columns, rows, frame_count;
    uint32_t frame;   /* The frame shown. */
    float fps;        /* Frames per second advanced by wpe_billboard_update() (0 = still). */
    bool loop;        /* Start over after the last frame (else stop on it). */
    float frame_time; /* Time into the current frame (read-only). */
    WPE_BillboardMode mode;
    WPE_BillboardBlend blend;
    bool lit; /* Lit by the scene's lights and the sun's shadow (default: unlit, the image's own colors). */
    bool visible;
} WPE_Billboard;

/** @return A white, unlit, visible 1x1 screen-aligned cutout billboard centered on its entity, without an
 * image or frames. @thread_safety Any thread */
WPE_Billboard wpe_billboard_default(void);

/**
 * World-space corners of a billboard as seen from a camera, counter-clockwise when looked at: bottom-left,
 * bottom-right, top-right, top-left; and the quad's normal, which faces the camera (except FIXED quads,
 * whose normal is the entity's +Z). Only FIXED quads follow the entity's rotation; the others use its
 * position and scale.
 *
 * @param world The entity's world matrix. @param camera_world The camera's world matrix.
 * @return false for an empty quad (zero size or scale).
 *
 * @thread_safety Any thread
 */
bool wpe_billboard_corners(const WPE_Billboard* billboard, const WPE_Mat4* world,
                           const WPE_Mat4* camera_world, WPE_Vec3 out_corners[4], WPE_Vec3* out_normal);

/** Advances the sprite-sheet animation of every billboard with fps > 0. @thread_safety Thread owning the
 * world */
void wpe_billboard_update(WPE_Ecs* ecs, float dt);

/** Where new particles appear, around the emitter's origin in its own space. */
typedef enum WPE_ParticleShape {
    WPE_PARTICLES_POINT = 0, /* At the origin. */
    WPE_PARTICLES_SPHERE,    /* Anywhere inside a sphere of radius shape_size.x. */
    WPE_PARTICLES_BOX,       /* Anywhere inside a box of full extents shape_size. */
} WPE_ParticleShape;

/** One live particle (in world space, or the emitter's space when not world_space). */
typedef struct WPE_Particle {
    WPE_Vec3 position;
    WPE_Vec3 velocity;
    float age, lifetime;  /* Seconds. */
    float rotation, spin; /* Radians, radians per second. */
} WPE_Particle;

/**
 * Emits and simulates particles drawn as screen-facing quads, many per draw. The settings are plain
 * fields; the runtime state at the end belongs to the module (read-only).
 */
typedef struct WPE_ParticleEmitter {
    /* Look. */
    WPE_AssetHandle image;      /* WPE_ASSET_IMAGE, or WPE_ASSET_NONE for plain squares. */
    const WPE_Texture* texture; /* Resolved from `image` by the add hook (read-only). */
    uint32_t columns, rows,
        frame_count; /* A sprite sheet played over each particle's life (0 frames = all). */
    WPE_BillboardBlend blend;
    bool lit;
    WPE_Vec4 color_start, color_end; /* RGBA (0..1) over each particle's life. */
    float size_start, size_end;      /* World units over each particle's life. */
    /* Emission. */
    bool playing;           /* Emitting (particles already out live on when it stops). */
    float duration;         /* Seconds of emission per cycle (0 = endless). */
    bool loop;              /* Start a new cycle (and its burst) after `duration`, else stop playing. */
    float rate;             /* Particles per second while playing. */
    uint32_t burst;         /* Particles emitted at once when a cycle starts. */
    uint32_t max_particles; /* At most this many alive (1..100000); new ones are dropped beyond it. */
    bool world_space;       /* Particles stay where they were emitted; else they move with the emitter. */
    uint32_t seed;          /* Random seed (0 = from the entity), so a scene plays the same every time. */
    WPE_ParticleShape shape;
    WPE_Vec3 shape_size;
    /* Motion. */
    float lifetime_min, lifetime_max; /* Seconds. */
    float speed_min, speed_max;       /* Initial speed, world units per second. */
    WPE_Vec3 direction;               /* Emission direction in the emitter's space. */
    float spread;                     /* Half-angle (radians) of the cone around `direction`. */
    WPE_Vec3 gravity;                 /* Acceleration in world space (units per second squared). */
    float drag;                       /* Fraction of velocity lost per second (0..). */
    float spin_min, spin_max;         /* Radians per second. */
    bool random_rotation;             /* Start each particle at a random angle. */
    /* Runtime state (read-only; reset by the add hook, freed by the remove hook). */
    WPE_Particle* particles;
    uint32_t count, capacity;
    float time;        /* Into the current cycle. */
    float accumulator; /* Fraction of a particle owed by `rate`. */
    uint64_t random;
    bool started; /* The current cycle's burst has been emitted. */
} WPE_ParticleEmitter;

/** @return A small endless fountain: 20 white particles a second rising in a 15-degree cone for one second,
 * shrinking and fading out, alpha blended in world space. @thread_safety Any thread */
WPE_ParticleEmitter wpe_particle_emitter_default(void);

/**
 * Advances every emitter by `dt`: cycles, bursts and steady emission, then motion, aging and removal of
 * expired particles. Large emitters are integrated on the job system; results do not depend on it.
 *
 * @param jobs Optional (NULL = this thread).
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or WPE_RESULT_ERROR_OUT_OF_MEMORY (an emitter could
 *         not grow to max_particles; logged).
 *
 * @thread_safety Thread owning the world (and the job system)
 */
WPE_Result wpe_particles_update(WPE_Ecs* ecs, WPE_JobSystem* jobs, float dt);

/** Emits `count` particles now (within max_particles), whether or not the emitter plays. @return The number
 * emitted. @thread_safety Thread owning the world */
uint32_t wpe_particles_emit(WPE_Ecs* ecs, WPE_Entity entity, uint32_t count);

/** Starts the emitter over: no particles, a fresh cycle with its burst, playing. @thread_safety Thread owning
 * the world */
void wpe_particles_restart(WPE_Ecs* ecs, WPE_Entity entity);

/** Removes every live particle and starts the cycle over (its burst comes again); the settings stay, playing
 * included. @thread_safety Thread owning the world */
void wpe_particles_clear(WPE_Ecs* ecs, WPE_Entity entity);

/**
 * Advances one emitter as if it played and looped, then puts `playing` and `loop` back: the editor shows a
 * selected emitter this way without changing the scene.
 *
 * @return As wpe_particles_update().
 *
 * @thread_safety Thread owning the world
 */
WPE_Result wpe_particles_preview(WPE_Ecs* ecs, WPE_Entity entity, float dt);

#define WPE_TRAIL_MAX_SAMPLES 96u

/** Where a trail's two edges were, and how long ago (seconds). */
typedef struct WPE_TrailSample {
    WPE_Vec3 base, tip;
    float age;
} WPE_TrailSample;

/**
 * A weapon trail: while `emitting`, it records each frame where the entities `base` and `tip` are, and draws
 * a ribbon between the two edges through the recent places, smoothed into curves between samples, fading from
 * color_start (the newest) to color_end (the oldest, `lifetime` seconds ago).
 */
typedef struct WPE_Trail {
    WPE_Entity base, tip;            /* The edges, e.g. points on a blade (WPE_ENTITY_NULL: no trail). */
    bool emitting;                   /* Recording (a swing); off, the trail fades away. */
    float lifetime;                  /* Seconds a recorded place lasts: the trail's length in time. */
    WPE_Vec4 color_start, color_end; /* RGBA at the newest and the oldest end. */
    float width_end;    /* At the oldest end the ribbon narrows toward the tip to this share (0..1). */
    uint32_t smoothing; /* Curve segments between samples (1..8; 1 = straight). */
    WPE_BillboardBlend blend;
    bool visible;
    /* Runtime state (read-only; reset by the add hook). Newest first. */
    WPE_TrailSample samples[WPE_TRAIL_MAX_SAMPLES];
    uint32_t sample_count;
} WPE_Trail;

/** @return An additive, emitting, pale blue trail lasting 0.2 s, narrowing to 30% with 4 curve segments,
 * without edges yet. @thread_safety Any thread */
WPE_Trail wpe_trail_default(void);

/**
 * Advances every trail: ages its samples and drops those past their lifetime, then, while emitting, records
 * where its edges are now (their world transforms, so call it after wpe_scene_update_transforms()).
 *
 * @thread_safety Thread owning the world
 */
void wpe_trail_update(WPE_Ecs* ecs, float dt);

/** Forgets every recorded place (a teleport, a new swing that shouldn't join the last). @thread_safety Thread
 * owning the world */
void wpe_trail_clear(WPE_Ecs* ecs, WPE_Entity entity);
/**
 * A slash: a cubic Bezier curve (four control points in the entity's space) along which a glowing ribbon
 * sweeps when played. The visible piece is `length` of the curve long: its head runs from the start to the
 * end over `duration` seconds and its tail follows until the slash is gone. The ribbon faces the camera and
 * is `width` wide in its middle, thin at both ends; its color goes from color_tail to color_head.
 */
typedef struct WPE_Slash {
    WPE_Vec3 points[4];              /* Control points in the entity's space (start, two handles, end). */
    float width;                     /* Widest, in the middle of the visible piece (world units). */
    float length;                    /* The visible piece's share of the curve (0..1]. */
    float duration;                  /* Seconds for the whole sweep. */
    WPE_Vec4 color_head, color_tail; /* RGBA at the leading and trailing ends. */
    WPE_BillboardBlend blend;
    uint32_t segments; /* Quads along the visible piece (2..64). */
    WPE_Entity head;   /* Optional: a child of the entity moved to the head as it sweeps (sparks, a light). */
    bool visible;
    bool playing; /* Sweeping; it stops (and disappears) at the end. */
    float time;   /* Seconds into the sweep. */
} WPE_Slash;

/** @return A 1.2 m crescent in the entity's XY plane, 0.25 m wide, sweeping in 0.3 s with half of it visible,
 * additive white-blue, not playing. @thread_safety Any thread */
WPE_Slash wpe_slash_default(void);

/** @return The point at `t` (0..1) along a slash's curve, in the entity's space. @thread_safety Any thread */
WPE_Vec3 wpe_slash_point(const WPE_Slash* slash, float t);

/** Plays a slash from the start. @thread_safety Thread owning the world */
void wpe_slash_play(WPE_Ecs* ecs, WPE_Entity entity);

/**
 * Advances playing slashes and moves each one's `head` entity to where its head is.
 *
 * @thread_safety Thread owning the world
 */
void wpe_slash_update(WPE_Ecs* ecs, float dt);
/** @return A visible, lit, single-sided (as the material says) mesh renderer of `mesh` in `model` that casts
 * and receives shadows, without a skin. @thread_safety Any thread */
WPE_MeshRenderer wpe_mesh_renderer_default(WPE_AssetHandle model, uint32_t mesh);

/** @return A perspective camera with a 60 degree vertical FOV, near 0.1, far 500. @thread_safety Any thread
 */
WPE_Camera wpe_camera_default(void);

/** @return A white light of the given type: intensity 1, range 10, cones 20/30 degrees. @thread_safety Any
 * thread */
WPE_Light wpe_light_default(WPE_LightType type);

/**
 * Registers WPE_COMP_MESH, WPE_COMP_CAMERA, WPE_COMP_LIGHT, WPE_COMP_ANIMATOR, WPE_COMP_BILLBOARD and
 * WPE_COMP_PARTICLES, WPE_COMP_TRAIL and WPE_COMP_SLASH. Scene components
 * must be registered too (wpe_scene_register_components()).
 *
 * @param assets Borrowed; must outlive the world (component hooks release model references).
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or a registration error.
 *
 * @thread_safety Thread owning the world (which must also own the asset manager)
 */
[[nodiscard]] WPE_Result wpe_graphics_register_components(WPE_Ecs* ecs, WPE_AssetManager* assets);

typedef struct WPE_InstantiateOptions {
    uint32_t animation; /* Animation to attach (WPE_MODEL_NONE for none). Attached paused at time 0. */
    bool play;          /* Start the attached animation immediately (looping). */
} WPE_InstantiateOptions;

/**
 * Creates entities for a model's default scene: a root entity named "model" with one entity per
 * node beneath it, carrying Transform, Hierarchy, Name and, for mesh nodes, WPE_MeshRenderer.
 * Skins are bound to the created joint entities.
 *
 * @param assets The manager given to wpe_graphics_register_components().
 * @param model A WPE_ASSET_MODEL handle. Every created mesh renderer (and the animator) holds its
 *              own reference, so the caller may release theirs afterwards.
 * @param options Optional (NULL = no animator).
 * @param out_root Receives the root entity; destroy the instance with wpe_scene_destroy_recursive().
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_INVALID_ARG (not a model handle,
 *         or bad animation index), WPE_RESULT_ERROR_PARSE (skin references joints outside the
 *         model), or an ECS error. On failure every entity created so far is destroyed.
 *
 * @thread_safety Thread owning the world
 */
[[nodiscard]] WPE_Result wpe_model_instantiate(WPE_Ecs* ecs, WPE_AssetManager* assets, WPE_AssetHandle model,
                                               const WPE_InstantiateOptions* options, WPE_Entity* out_root);

#endif /* WPE_GRAPHICS_GRAPHICS_H */
