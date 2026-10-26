#define WPE_LOG_MODULE "graphics"
#include "graphics/scene_render.h"

#include <math.h>
#include <string.h>

#include "core/alloc.h"
#include "core/arena.h"
#include "core/debug.h"
#include "core/log.h"

struct WPE_SceneRenderer {
    WPE_Arena frame; /* Joint palettes; valid until the next wpe_scene_render(). */
};

WPE_SceneRenderSettings wpe_scene_render_settings_default(void) {
    return (WPE_SceneRenderSettings){
        .light = {.direction = wpe_vec3_normalize(wpe_vec3(-0.4f, -1.0f, -0.6f)),
                  .color = wpe_vec3(0.85f, 0.85f, 0.80f),
                  .ambient = wpe_vec3(0.25f, 0.26f, 0.30f)},
        .frustum_culling = true,
    };
}

WPE_Result wpe_scene_renderer_create(WPE_SceneRenderer** out_scene_renderer) {
    if (!out_scene_renderer) return WPE_RESULT_ERROR_NULL_ARG;
    WPE_SceneRenderer* sr = wpe_alloc_zeroed(1, sizeof *sr);
    if (!sr) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    wpe_arena_init(&sr->frame, (size_t)64 * 1024);
    *out_scene_renderer = sr;
    return WPE_RESULT_OK;
}

void wpe_scene_renderer_destroy(WPE_SceneRenderer* scene_renderer) {
    if (!scene_renderer) return;
    wpe_arena_release(&scene_renderer->frame);
    wpe_free(scene_renderer);
}

bool wpe_scene_camera_matrices(const WPE_Ecs* ecs, WPE_Entity camera, float aspect, WPE_Mat4* out_view,
                               WPE_Mat4* out_projection) {
    if (!ecs || !out_view || !out_projection || !(aspect > 0.0f)) return false;
    const WPE_Transform* t = wpe_ecs_get_component(ecs, camera, WPE_COMP_TRANSFORM);
    const WPE_Camera* c = wpe_ecs_get_component(ecs, camera, WPE_COMP_CAMERA);
    if (!t || !c) return false;
    if (!(c->near_z > 0.0f) || !(c->far_z > c->near_z)) return false;

    /* The camera's world matrix may carry scale; strip it so the view is a rigid transform. */
    WPE_Vec3 position, scale;
    WPE_Quat rotation;
    if (!wpe_mat4_decompose(&t->world, &position, &rotation, &scale)) return false;
    const WPE_Mat4 rigid = wpe_mat4_from_trs(position, rotation, wpe_vec3(1, 1, 1));
    if (!wpe_mat4_inverse(&rigid, out_view)) return false;

    if (c->orthographic) {
        if (!(c->ortho_height > 0.0f)) return false;
        const float half_h = c->ortho_height * 0.5f, half_w = half_h * aspect;
        *out_projection = wpe_mat4_orthographic(-half_w, half_w, -half_h, half_h, c->near_z, c->far_z);
    } else {
        if (!(c->fov_y > 0.0f && c->fov_y < WPE_PI)) return false;
        *out_projection = wpe_mat4_perspective(c->fov_y, aspect, c->near_z, c->far_z);
    }
    return true;
}

/* ---- Culling -------------------------------------------------------------------------------------- */

typedef struct wpe__Frustum {
    WPE_Vec4 planes[6]; /* a*x + b*y + c*z + d >= 0 inside. */
} wpe__Frustum;

/* Extracts clip planes in the space that `m` maps to clip space (Gribb-Hartmann, OpenGL depth). */
static wpe__Frustum wpe__frustum_from_matrix(const WPE_Mat4* m) {
    WPE_Vec4 row[4];
    for (int r = 0; r < 4; ++r)
        row[r] = wpe_vec4(m->m[0][r], m->m[1][r], m->m[2][r], m->m[3][r]);
    wpe__Frustum f;
    for (int axis = 0; axis < 3; ++axis) {
        for (int sign = 0; sign < 2; ++sign) {
            const float s = sign ? -1.0f : 1.0f;
            f.planes[axis * 2 + sign] = wpe_vec4(row[3].x + s * row[axis].x, row[3].y + s * row[axis].y,
                                                 row[3].z + s * row[axis].z, row[3].w + s * row[axis].w);
        }
    }
    return f;
}

/* Conservative test: false only when the box is entirely outside one plane. */
static bool wpe__aabb_visible(const wpe__Frustum* f, const WPE_Aabb* box) {
    for (int p = 0; p < 6; ++p) {
        const WPE_Vec4 pl = f->planes[p];
        /* The box corner farthest along the plane normal. */
        const float x = pl.x >= 0.0f ? box->max.x : box->min.x;
        const float y = pl.y >= 0.0f ? box->max.y : box->min.y;
        const float z = pl.z >= 0.0f ? box->max.z : box->min.z;
        if (pl.x * x + pl.y * y + pl.z * z + pl.w < 0.0f) return false;
    }
    return true;
}

/* ---- Submission ----------------------------------------------------------------------------------- */

/* Maps a non-negative float to an order-preserving 32-bit key. */
static uint32_t wpe__depth_key(float depth) {
    if (!(depth > 0.0f)) return 0;
    uint32_t bits;
    memcpy(&bits, &depth, sizeof bits);
    return bits;
}

static float wpe__material_gloss(const WPE_Material* m) {
    return m->workflow == WPE_MATERIAL_METALLIC_ROUGHNESS ? 1.0f - m->roughness_factor : m->glossiness_factor;
}

/* Computes world-space skinning matrices for a skinned mesh renderer. */
static const WPE_Mat4* wpe__build_palette(WPE_SceneRenderer* sr, const WPE_Ecs* ecs,
                                          const WPE_MeshRenderer* mesh) {
    const WPE_Model* model = mesh->model;
    const WPE_Skin* skin = &model->skins[mesh->skin];
    const uint32_t count = mesh->joint_count < skin->joint_count ? mesh->joint_count : skin->joint_count;
    WPE_Mat4* palette = wpe_arena_alloc(&sr->frame, (size_t)(count ? count : 1) * sizeof *palette, 16);
    if (!palette) return NULL;
    for (uint32_t j = 0; j < count; ++j) {
        const WPE_Transform* joint = wpe_ecs_get_component(ecs, mesh->joints[j], WPE_COMP_TRANSFORM);
        const WPE_Mat4 joint_world = joint ? joint->world : wpe_mat4_identity();
        palette[j] = wpe_mat4_mul(&joint_world, &model->inverse_bind_matrices[skin->first_joint + j]);
    }
    return palette;
}

WPE_Result wpe_scene_render(WPE_SceneRenderer* sr, WPE_Renderer* renderer, const WPE_Ecs* ecs,
                            WPE_Entity camera, const WPE_SceneRenderSettings* settings,
                            WPE_SceneRenderStats* out_stats) {
    if (!sr || !renderer || !ecs) return WPE_RESULT_ERROR_NULL_ARG;
    const WPE_SceneRenderSettings defaults = wpe_scene_render_settings_default();
    if (!settings) settings = &defaults;
    WPE_SceneRenderStats stats = {0};
    wpe_arena_reset(&sr->frame);

    const WPE_Framebuffer* fb = wpe_renderer_framebuffer(renderer);
    WPE_Mat4 view, projection;
    if (!wpe_scene_camera_matrices(ecs, camera, (float)fb->width / (float)fb->height, &view, &projection)) {
        wpe_log_error("entity 0x%08x is not a valid camera (needs Transform and a valid Camera)", camera);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    const WPE_Mat4 view_projection = wpe_mat4_mul(&projection, &view);
    const WPE_Transform* camera_transform = wpe_ecs_get_component(ecs, camera, WPE_COMP_TRANSFORM);
    const WPE_Vec3 camera_position = wpe_vec3(
        camera_transform->world.m[3][0], camera_transform->world.m[3][1], camera_transform->world.m[3][2]);

    WPE_EcsIter it = wpe_ecs_query(
        (WPE_Ecs*)ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_MESH) | WPE_ECS_MASK(WPE_COMP_TRANSFORM)});
    WPE_Result result = WPE_RESULT_OK;
    while (result == WPE_RESULT_OK && wpe_ecs_iter_next(&it)) {
        const WPE_MeshRenderer* mesh = wpe_ecs_iter_get(&it, WPE_COMP_MESH);
        const WPE_Transform* transform = wpe_ecs_iter_get(&it, WPE_COMP_TRANSFORM);
        if (!mesh->visible || !mesh->model || mesh->mesh >= mesh->model->mesh_count) continue;
        const WPE_Model* model = mesh->model;
        const WPE_Mesh* mesh_data = &model->meshes[mesh->mesh];

        /* Skinned vertices are transformed to world space by the palette; the node transform is unused. */
        const bool skinned = mesh->skin != WPE_MODEL_NONE && mesh->skin < model->skin_count && mesh->joints;
        const WPE_Mat4* palette = skinned ? wpe__build_palette(sr, ecs, mesh) : NULL;
        if (skinned && !palette) {
            result = WPE_RESULT_ERROR_OUT_OF_MEMORY;
            break;
        }
        const WPE_Mat4 world = skinned ? wpe_mat4_identity() : transform->world;
        const WPE_Mat4 mvp = wpe_mat4_mul(&view_projection, &world);
        WPE_Mat4 inverse_world, normal_matrix = wpe_mat4_identity();
        if (wpe_mat4_inverse(&world, &inverse_world)) {
            for (int c = 0; c < 4; ++c) {
                for (int r = 0; r < 4; ++r)
                    normal_matrix.m[c][r] = inverse_world.m[r][c];
            }
        }
        /* A mirroring transform flips triangle winding, so the culled face must flip too. */
        const float det = wpe_vec3_dot(wpe_vec3_cross(wpe_vec3(world.m[0][0], world.m[0][1], world.m[0][2]),
                                                      wpe_vec3(world.m[1][0], world.m[1][1], world.m[1][2])),
                                       wpe_vec3(world.m[2][0], world.m[2][1], world.m[2][2]));
        const wpe__Frustum frustum = wpe__frustum_from_matrix(&mvp);
        const WPE_Mat4 model_view = wpe_mat4_mul(&view, &world);

        for (uint32_t p = 0; p < mesh_data->primitive_count && result == WPE_RESULT_OK; ++p) {
            const WPE_MeshPrimitive* prim = &model->primitives[mesh_data->first_primitive + p];
            if (settings->frustum_culling && !skinned && !wpe__aabb_visible(&frustum, &prim->bounds)) {
                ++stats.primitives_culled;
                continue;
            }
            const WPE_Material* material =
                &model->materials[prim->material < model->material_count ? prim->material
                                                                         : model->material_count - 1];

            WPE_RenderState state = wpe_render_state_default();
            state.cull =
                material->double_sided ? WPE_CULL_NONE : (det < 0.0f ? WPE_CULL_FRONT : WPE_CULL_BACK);
            bool transparent = false;
            if (material->alpha_mode == WPE_ALPHA_BLEND) {
                if (settings->dither_transparency) {
                    state.blend = WPE_BLEND_ALPHA_DITHER;
                } else {
                    state.blend = WPE_BLEND_ALPHA;
                    state.depth_write = false;
                    transparent = true;
                }
            }

            const WPE_Vec3 center = wpe_vec3_scale(wpe_vec3_add(prim->bounds.min, prim->bounds.max), 0.5f);
            const float depth = -wpe_mat4_transform_point(&model_view, center).z;
            const uint32_t depth_key = wpe__depth_key(depth);
            const uint64_t sort_key = transparent
                                          ? (UINT64_C(1) << 63) | ((uint64_t)(UINT32_MAX - depth_key) << 30)
                                          : (uint64_t)depth_key << 30;

            const WPE_MaterialUniforms uniforms = {
                .mvp = mvp,
                .world = world,
                .normal_matrix = normal_matrix,
                .camera_position = camera_position,
                .light = settings->light,
                .shininess = wpe_material_shininess(wpe__material_gloss(material)),
                .model = model,
                .material = material,
                .joint_palette = palette,
                .joint_count = skinned ? mesh->joint_count : 0,
            };
            result = wpe_renderer_submit(renderer, &(WPE_DrawDesc){
                                                       .program = wpe_shader_material(),
                                                       .uniforms = &uniforms,
                                                       .uniforms_size = sizeof uniforms,
                                                       .vertices = &model->vertices[prim->vertex_offset],
                                                       .vertex_stride = sizeof(WPE_MeshVertex),
                                                       .vertex_count = prim->vertex_count,
                                                       .indices = &model->indices[prim->index_offset],
                                                       .index_count = prim->index_count,
                                                       .state = state,
                                                       .sort_key = sort_key,
                                                   });
            if (result == WPE_RESULT_OK) ++stats.primitives_submitted;
        }
    }
    if (out_stats) *out_stats = stats;
    return result;
}
