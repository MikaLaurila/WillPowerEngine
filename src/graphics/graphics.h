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
#include "scene/scene.h"

typedef struct WPE_MeshRenderer {
    WPE_AssetHandle model_asset; /* WPE_ASSET_MODEL; the component holds a reference. */
    const WPE_Model* model;      /* Resolved from model_asset by the add hook (read-only). */
    uint32_t mesh;               /* Index into model->meshes. */
    uint32_t skin;               /* Index into model->skins, or WPE_MODEL_NONE. */
    WPE_Entity* joints; /* Owned (freed by the component's remove hook): one entity per skin joint. */
    uint32_t joint_count;
    bool visible;
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
} WPE_Animator;

/** How a billboard turns. */
typedef enum WPE_BillboardMode {
    WPE_BILLBOARD_FIXED = 0,      /* A double-sided quad in the entity's XY plane, turned by its transform. */
    WPE_BILLBOARD_AXIS_ALIGNED,   /* Turns about the entity's own Y axis to face the camera. */
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
 * whose normal is the entity's +Z).
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

/** @return A perspective camera with a 60 degree vertical FOV, near 0.1, far 500. @thread_safety Any thread
 */
WPE_Camera wpe_camera_default(void);

/** @return A white light of the given type: intensity 1, range 10, cones 20/30 degrees. @thread_safety Any
 * thread */
WPE_Light wpe_light_default(WPE_LightType type);

/**
 * Registers WPE_COMP_MESH, WPE_COMP_CAMERA, WPE_COMP_LIGHT, WPE_COMP_ANIMATOR and WPE_COMP_BILLBOARD. Scene
 * components
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
