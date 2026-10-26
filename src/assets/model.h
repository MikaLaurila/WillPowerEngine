/**
 * @file model.h
 * @brief Imported 3D models: meshes, materials, textures, node hierarchy, skins and animations.
 *
 * A WPE_Model is an immutable, renderer-ready representation of a glTF 2.0 asset (.gltf or .glb).
 * All arrays are owned by the model and freed by wpe_model_destroy(); indices between arrays are
 * plain uint32 (WPE_MODEL_NONE when absent).
 *
 * Materials use the engine's traditional workflow (diffuse / specular / glossiness). glTF
 * metallic-roughness materials keep their PBR inputs (WPE_MATERIAL_METALLIC_ROUGHNESS) and are
 * converted per pixel by the material shader:
 *   diffuse = base * (1 - metallic), specular = mix(0.04, base, metallic), gloss = 1 - roughness.
 *
 * Color factors are stored sRGB-encoded (glTF stores them linear) so they combine directly with
 * sRGB texels in the engine's gamma-space shading.
 *
 * Supported glTF extensions: KHR_texture_transform, KHR_materials_emissive_strength,
 * KHR_materials_pbrSpecularGlossiness, KHR_materials_unlit, KHR_materials_transmission
 * (approximated as alpha blending). Assets that *require* any other extension are rejected.
 *
 * - Dependencies: core, math, renderer (textures), platform (file I/O), jobs (optional parallel
 *   texture decoding), cgltf + stb_image (private)
 * - Thread affinity: load/destroy on any one thread; a loaded model is read-only and may be read
 *   from any thread
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_ASSETS_MODEL_H
#define WPE_ASSETS_MODEL_H

#include <stdint.h>

#include "core/arena.h"
#include "core/result.h"
#include "jobs/jobs.h"
#include "math/wpe_math.h"
#include "renderer/texture.h"

#define WPE_MODEL_NONE UINT32_MAX
#define WPE_MODEL_NAME_MAX 64u

/** Vertex layout shared by all imported meshes (also the material shader's input). */
typedef struct WPE_MeshVertex {
    WPE_Vec3 position;
    WPE_Vec3 normal;
    WPE_Vec2 uv0;
    WPE_Vec2 uv1;
    float tangent[4]; /* xyz tangent, w = bitangent sign; all zero when the asset has none. */
    float color[4];   /* COLOR_0, or opaque white. */
    uint16_t joints[4];
    float weights[4]; /* Sum to 1 for skinned meshes, all zero otherwise. */
} WPE_MeshVertex;

typedef struct WPE_Aabb {
    WPE_Vec3 min;
    WPE_Vec3 max;
} WPE_Aabb;

/** A draw-able part of a mesh with a single material. Indices are relative to its vertex range. */
typedef struct WPE_MeshPrimitive {
    uint32_t vertex_offset; /* Into WPE_Model::vertices. */
    uint32_t vertex_count;
    uint32_t index_offset; /* Into WPE_Model::indices. */
    uint32_t index_count;
    uint32_t material; /* Index into WPE_Model::materials (a default material exists when unset). */
    WPE_Aabb bounds;   /* Object space. */
} WPE_MeshPrimitive;

typedef struct WPE_Mesh {
    char name[WPE_MODEL_NAME_MAX];
    uint32_t first_primitive;
    uint32_t primitive_count;
    WPE_Aabb bounds;
} WPE_Mesh;

typedef enum WPE_AlphaMode {
    WPE_ALPHA_OPAQUE = 0,
    WPE_ALPHA_MASK,  /* Discard below alpha_cutoff. */
    WPE_ALPHA_BLEND, /* Alpha blending (or dithering if the renderer is configured so). */
} WPE_AlphaMode;

typedef enum WPE_MaterialWorkflow {
    WPE_MATERIAL_SPECULAR_GLOSSINESS = 0, /* Traditional: diffuse / specular / glossiness. */
    WPE_MATERIAL_METALLIC_ROUGHNESS,      /* PBR inputs converted per pixel. */
    WPE_MATERIAL_UNLIT,                   /* Diffuse color only, no lighting. */
} WPE_MaterialWorkflow;

/** Reference to a texture with its sampler, UV set and KHR_texture_transform. */
typedef struct WPE_TextureRef {
    uint32_t texture;  /* Index into WPE_Model::textures, or WPE_MODEL_NONE. */
    uint32_t texcoord; /* 0 = uv0, 1 = uv1. */
    WPE_Sampler sampler;
    WPE_Vec2 offset;
    WPE_Vec2 scale;
    float rotation;
} WPE_TextureRef;

typedef struct WPE_Material {
    char name[WPE_MODEL_NAME_MAX];
    WPE_MaterialWorkflow workflow;

    WPE_Vec4 diffuse_factor; /* Diffuse (or PBR base color) RGBA. */
    WPE_TextureRef diffuse_texture;

    /* WPE_MATERIAL_SPECULAR_GLOSSINESS */
    WPE_Vec3 specular_factor;
    float glossiness_factor;                    /* 0 = rough, 1 = mirror-like. */
    WPE_TextureRef specular_glossiness_texture; /* RGB specular, A glossiness. */

    /* WPE_MATERIAL_METALLIC_ROUGHNESS */
    float metallic_factor;
    float roughness_factor;
    WPE_TextureRef metallic_roughness_texture; /* G roughness, B metallic (glTF layout). */

    WPE_TextureRef normal_texture;
    float normal_scale;
    WPE_TextureRef occlusion_texture; /* R channel. */
    float occlusion_strength;
    WPE_Vec3 emissive_factor;
    WPE_TextureRef emissive_texture;

    WPE_AlphaMode alpha_mode;
    float alpha_cutoff;
    bool double_sided;
} WPE_Material;

typedef struct WPE_ModelNode {
    char name[WPE_MODEL_NAME_MAX];
    uint32_t parent;      /* WPE_MODEL_NONE for roots. */
    uint32_t first_child; /* Children form a linked list through next_sibling. */
    uint32_t next_sibling;
    uint32_t mesh;
    uint32_t skin;
    WPE_Vec3 translation;
    WPE_Quat rotation;
    WPE_Vec3 scale;
} WPE_ModelNode;

typedef struct WPE_Skin {
    char name[WPE_MODEL_NAME_MAX];
    uint32_t first_joint; /* Range in WPE_Model::skin_joints / inverse_bind_matrices. */
    uint32_t joint_count;
} WPE_Skin;

typedef enum WPE_AnimationPath {
    WPE_ANIM_TRANSLATION = 0,
    WPE_ANIM_ROTATION,
    WPE_ANIM_SCALE,
} WPE_AnimationPath;

typedef enum WPE_AnimationInterpolation {
    WPE_INTERP_LINEAR = 0,
    WPE_INTERP_STEP,
    WPE_INTERP_CUBIC_SPLINE, /* Values stored as (in-tangent, value, out-tangent) triplets. */
} WPE_AnimationInterpolation;

typedef struct WPE_AnimationChannel {
    uint32_t node;
    WPE_AnimationPath path;
    WPE_AnimationInterpolation interpolation;
    uint32_t key_count;
    const float* times;  /* key_count ascending times in seconds. */
    const float* values; /* key_count * components (x3 for cubic spline); 3 or 4 components by path. */
} WPE_AnimationChannel;

typedef struct WPE_Animation {
    char name[WPE_MODEL_NAME_MAX];
    uint32_t first_channel;
    uint32_t channel_count;
    float duration; /* Seconds; latest key time of any channel. */
} WPE_Animation;

typedef struct WPE_Model {
    WPE_MeshVertex* vertices;
    uint32_t vertex_count;
    uint32_t* indices;
    uint32_t index_count;
    WPE_MeshPrimitive* primitives;
    uint32_t primitive_count;
    WPE_Mesh* meshes;
    uint32_t mesh_count;
    WPE_Material* materials; /* The last entry is the default material for primitives without one. */
    uint32_t material_count;
    WPE_Texture** textures; /* Entries are NULL for images that failed to decode (logged); treat as absent. */
    uint32_t texture_count;
    WPE_ModelNode* nodes;
    uint32_t node_count;
    uint32_t* root_nodes; /* Roots of the default scene (or all parentless nodes). */
    uint32_t root_count;
    WPE_Skin* skins;
    uint32_t skin_count;
    uint32_t* skin_joints;           /* Node indices. */
    WPE_Mat4* inverse_bind_matrices; /* Parallel to skin_joints. */
    WPE_Animation* animations;
    uint32_t animation_count;
    WPE_AnimationChannel* channels;
    uint32_t channel_count;

    WPE_Arena wpe__memory; /* Owns every array above except the textures themselves. */
} WPE_Model;

typedef struct WPE_ModelLoadOptions {
    WPE_JobSystem* jobs;       /* Optional: decode textures in parallel. */
    uint32_t max_texture_size; /* 0 = keep original size; otherwise downscale to fit. */
} WPE_ModelLoadOptions;

/**
 * Loads a .gltf or .glb file (external buffers/images are resolved relative to the file).
 *
 * @param path UTF-8 path; must not be NULL.
 * @param options Optional (NULL = defaults: no jobs, max_texture_size 0).
 * @param out_model Receives the model; release with wpe_model_destroy().
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_IO; WPE_RESULT_ERROR_PARSE
 *         (malformed or invalid glTF); WPE_RESULT_ERROR_UNSUPPORTED (required extension or
 *         feature not supported); WPE_RESULT_ERROR_OUT_OF_MEMORY. Failures are logged.
 *
 * @thread_safety Any thread; uses `options->jobs` (which must be owned by the calling thread)
 */
[[nodiscard]] WPE_Result wpe_model_load(const char* path, const WPE_ModelLoadOptions* options,
                                        WPE_Model** out_model);

/** Frees a model and its textures (ownership consumed); NULL is a no-op. @thread_safety Any thread */
void wpe_model_destroy(WPE_Model* model);

/** @return The node's local transform as a matrix. @thread_safety Any thread */
WPE_Mat4 wpe_model_node_local_matrix(const WPE_ModelNode* node);

#endif /* WPE_ASSETS_MODEL_H */
