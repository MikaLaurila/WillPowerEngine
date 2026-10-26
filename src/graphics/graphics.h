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
 * - WPE_COMP_ANIMATOR (WPE_Animator): plays one of a model's animations on the entities created by
 *   wpe_model_instantiate().
 *
 * Asset lifetime: components hold borrowed WPE_Model pointers. A model must outlive every entity
 * that references it.
 * NOTE(limitation): pointers are replaced by ref-counted asset handles with the asset system (pak
 * milestone).
 *
 * - Dependencies: core, math, ecs, scene, renderer, assets
 * - Thread affinity: same as the owning WPE_Ecs world
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_GRAPHICS_GRAPHICS_H
#define WPE_GRAPHICS_GRAPHICS_H

#include "assets/model.h"
#include "ecs/ecs.h"
#include "scene/scene.h"

typedef struct WPE_MeshRenderer {
    const WPE_Model* model; /* Borrowed. */
    uint32_t mesh;          /* Index into model->meshes. */
    uint32_t skin;          /* Index into model->skins, or WPE_MODEL_NONE. */
    WPE_Entity* joints;     /* Owned (freed by the component's remove hook): one entity per skin joint. */
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

typedef struct WPE_Animator {
    const WPE_Model* model; /* Borrowed. */
    uint32_t animation;     /* Index into model->animations. */
    float time;             /* Current playback position in seconds. */
    float speed;            /* Playback rate multiplier (negative plays backwards). */
    bool playing;
    bool loop;
    WPE_Entity* node_entities; /* Owned (freed by the remove hook): entity per model node. */
    uint32_t node_count;
} WPE_Animator;

/** @return A perspective camera with a 60 degree vertical FOV, near 0.1, far 500. @thread_safety Any thread
 */
WPE_Camera wpe_camera_default(void);

/**
 * Registers WPE_COMP_MESH, WPE_COMP_CAMERA and WPE_COMP_ANIMATOR. Scene components must be
 * registered too (wpe_scene_register_components()).
 *
 * @thread_safety Thread owning the world
 */
[[nodiscard]] WPE_Result wpe_graphics_register_components(WPE_Ecs* ecs);

typedef struct WPE_InstantiateOptions {
    uint32_t animation; /* Animation to attach (WPE_MODEL_NONE for none). Attached paused at time 0. */
    bool play;          /* Start the attached animation immediately (looping). */
} WPE_InstantiateOptions;

/**
 * Creates entities for a model's default scene: a root entity named "model" with one entity per
 * node beneath it, carrying Transform, Hierarchy, Name and, for mesh nodes, WPE_MeshRenderer.
 * Skins are bound to the created joint entities.
 *
 * @param options Optional (NULL = no animator).
 * @param out_root Receives the root entity; destroy the instance with wpe_scene_destroy_recursive().
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_INVALID_ARG (bad animation
 *         index), WPE_RESULT_ERROR_PARSE (skin references joints outside the model), or an ECS
 *         error. On failure every entity created so far is destroyed.
 *
 * @thread_safety Thread owning the world
 */
[[nodiscard]] WPE_Result wpe_model_instantiate(WPE_Ecs* ecs, const WPE_Model* model,
                                               const WPE_InstantiateOptions* options, WPE_Entity* out_root);

#endif /* WPE_GRAPHICS_GRAPHICS_H */
