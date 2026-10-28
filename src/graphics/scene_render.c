#define WPE_LOG_MODULE "graphics"
#include "graphics/scene_render.h"

#include <math.h>
#include <string.h>

#include "core/alloc.h"
#include "core/arena.h"
#include "core/debug.h"
#include "core/log.h"
#include "graphics/billboard_internal.h"

struct WPE_SceneRenderer {
    WPE_Arena frame;           /* Per-call data; valid until the next wpe_scene_render(). */
    WPE_Renderer* shadow_pass; /* Depth-only renderer for the sun's shadow map (created on demand). */
    WPE_ShadowSampler shadow;
    bool has_shadow;
};

/* Per-mesh-entity data computed once and shared by the shadow and main passes. */
typedef struct wpe__MeshInstance {
    const WPE_MeshRenderer* mesh;
    WPE_Mat4 world; /* Identity for skinned meshes (palette is world space). */
    const WPE_Mat4* palette;
    WPE_Vec3 sphere_center; /* World-space bounding sphere for light selection. */
    float sphere_radius;
} wpe__MeshInstance;

WPE_SceneRenderSettings wpe_scene_render_settings_default(void) {
    return (WPE_SceneRenderSettings){
        .ambient = wpe_vec3(0.25f, 0.26f, 0.30f),
        .fog = {.mode = WPE_FOG_NONE,
                .color = wpe_vec3(0.5f, 0.55f, 0.6f),
                .start = 20.0f,
                .end = 100.0f,
                .density = 0.02f},
        .shadows = {.enabled = true,
                    .map_size = 1024,
                    .distance = 30.0f,
                    .caster_extension = 50.0f,
                    .bias = 0.0015f},
        .frustum_culling = true,
        .subdivision_levels = 3,
        .subdivision_edge_pixels = 24.0f,
        .default_sun = true,
        .default_sun_direction = wpe_vec3_normalize(wpe_vec3(-0.4f, -1.0f, -0.6f)),
        .default_sun_color = wpe_vec3(0.85f, 0.85f, 0.80f),
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
    wpe_renderer_destroy(scene_renderer->shadow_pass);
    wpe_arena_release(&scene_renderer->frame);
    wpe_free(scene_renderer);
}

const WPE_ShadowSampler* wpe_scene_renderer_shadow_map(const WPE_SceneRenderer* scene_renderer) {
    return scene_renderer && scene_renderer->has_shadow ? &scene_renderer->shadow : NULL;
}

/* Rigid camera-to-world matrix (scale stripped). */
static bool wpe__camera_rigid_world(const WPE_Transform* t, WPE_Mat4* out) {
    WPE_Vec3 position, scale;
    WPE_Quat rotation;
    if (!wpe_mat4_decompose(&t->world, &position, &rotation, &scale)) return false;
    *out = wpe_mat4_from_trs(position, rotation, wpe_vec3(1, 1, 1));
    return true;
}

bool wpe_scene_camera_matrices(const WPE_Ecs* ecs, WPE_Entity camera, float aspect, WPE_Mat4* out_view,
                               WPE_Mat4* out_projection) {
    if (!ecs || !out_view || !out_projection || !(aspect > 0.0f)) return false;
    const WPE_Transform* t = wpe_ecs_get_component(ecs, camera, WPE_COMP_TRANSFORM);
    const WPE_Camera* c = wpe_ecs_get_component(ecs, camera, WPE_COMP_CAMERA);
    if (!t || !c) return false;
    if (!(c->near_z > 0.0f) || !(c->far_z > c->near_z)) return false;

    WPE_Mat4 rigid;
    if (!wpe__camera_rigid_world(t, &rigid) || !wpe_mat4_inverse(&rigid, out_view)) return false;
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
        const float x = pl.x >= 0.0f ? box->max.x : box->min.x;
        const float y = pl.y >= 0.0f ? box->max.y : box->min.y;
        const float z = pl.z >= 0.0f ? box->max.z : box->min.z;
        if (pl.x * x + pl.y * y + pl.z * z + pl.w < 0.0f) return false;
    }
    return true;
}

/* ---- Helpers -------------------------------------------------------------------------------------- */

static uint32_t wpe__depth_key(float depth) {
    if (!(depth > 0.0f)) return 0;
    uint32_t bits;
    memcpy(&bits, &depth, sizeof bits);
    return bits;
}

static float wpe__material_gloss(const WPE_Material* m) {
    return m->workflow == WPE_MATERIAL_METALLIC_ROUGHNESS ? 1.0f - m->roughness_factor : m->glossiness_factor;
}

static const WPE_Material* wpe__primitive_material(const WPE_Model* model, const WPE_MeshPrimitive* prim) {
    return &model->materials[prim->material < model->material_count ? prim->material
                                                                    : model->material_count - 1];
}

static float wpe__determinant3(const WPE_Mat4* m) {
    return wpe_vec3_dot(wpe_vec3_cross(wpe_vec3(m->m[0][0], m->m[0][1], m->m[0][2]),
                                       wpe_vec3(m->m[1][0], m->m[1][1], m->m[1][2])),
                        wpe_vec3(m->m[2][0], m->m[2][1], m->m[2][2]));
}

static WPE_Vec3 wpe__forward(const WPE_Mat4* world) {
    return wpe_vec3_normalize(wpe_mat4_transform_dir(world, wpe_vec3(0, 0, -1)));
}

/* Computes world-space skinning matrices for a skinned mesh renderer. */
static WPE_Mat4* wpe__build_palette(WPE_SceneRenderer* sr, const WPE_Ecs* ecs, const WPE_MeshRenderer* mesh) {
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

/* Collects visible mesh entities with their world matrices, palettes and bounding spheres. */
static WPE_Result wpe__gather_meshes(WPE_SceneRenderer* sr, const WPE_Ecs* ecs, wpe__MeshInstance** out,
                                     uint32_t* out_count) {
    const uint32_t capacity = wpe_ecs_component_count(ecs, WPE_COMP_MESH);
    wpe__MeshInstance* instances =
        wpe_arena_alloc(&sr->frame, (size_t)(capacity ? capacity : 1) * sizeof *instances, 16);
    if (!instances) return WPE_RESULT_ERROR_OUT_OF_MEMORY;

    uint32_t count = 0;
    WPE_EcsIter it = wpe_ecs_query(
        (WPE_Ecs*)ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_MESH) | WPE_ECS_MASK(WPE_COMP_TRANSFORM)});
    while (wpe_ecs_iter_next(&it)) {
        const WPE_MeshRenderer* mesh = wpe_ecs_iter_get(&it, WPE_COMP_MESH);
        const WPE_Transform* transform = wpe_ecs_iter_get(&it, WPE_COMP_TRANSFORM);
        if (!mesh->visible || !mesh->model || mesh->mesh >= mesh->model->mesh_count) continue;
        wpe__MeshInstance* inst = &instances[count];
        *inst = (wpe__MeshInstance){.mesh = mesh, .world = transform->world};
        const WPE_Aabb bounds = mesh->model->meshes[mesh->mesh].bounds;
        const WPE_Vec3 local_center = wpe_vec3_scale(wpe_vec3_add(bounds.min, bounds.max), 0.5f);
        const float local_radius = wpe_vec3_length(wpe_vec3_sub(bounds.max, local_center));

        const bool skinned =
            mesh->skin != WPE_MODEL_NONE && mesh->skin < mesh->model->skin_count && mesh->joints;
        if (skinned) {
            WPE_Mat4* palette = wpe__build_palette(sr, ecs, mesh);
            if (!palette) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
            inst->palette = palette;
            inst->world = wpe_mat4_identity();
            /* Sphere around the joints, padded by the mesh's own extent. */
            WPE_Vec3 sum = wpe_vec3(0, 0, 0);
            for (uint32_t j = 0; j < mesh->joint_count; ++j) {
                sum = wpe_vec3_add(sum, wpe_vec3(palette[j].m[3][0], palette[j].m[3][1], palette[j].m[3][2]));
            }
            inst->sphere_center =
                mesh->joint_count
                    ? wpe_vec3_scale(sum, 1.0f / (float)mesh->joint_count)
                    : wpe_vec3(transform->world.m[3][0], transform->world.m[3][1], transform->world.m[3][2]);
            float radius = local_radius;
            for (uint32_t j = 0; j < mesh->joint_count; ++j) {
                const WPE_Transform* joint = wpe_ecs_get_component(ecs, mesh->joints[j], WPE_COMP_TRANSFORM);
                if (!joint) continue;
                const float d = wpe_vec3_length(
                    wpe_vec3_sub(wpe_vec3(joint->world.m[3][0], joint->world.m[3][1], joint->world.m[3][2]),
                                 inst->sphere_center));
                radius = d + local_radius * 0.25f > radius ? d + local_radius * 0.25f : radius;
            }
            inst->sphere_radius = radius;
        } else {
            inst->sphere_center = wpe_mat4_transform_point(&inst->world, local_center);
            const float sx =
                wpe_vec3_length(wpe_vec3(inst->world.m[0][0], inst->world.m[0][1], inst->world.m[0][2]));
            const float sy =
                wpe_vec3_length(wpe_vec3(inst->world.m[1][0], inst->world.m[1][1], inst->world.m[1][2]));
            const float sz =
                wpe_vec3_length(wpe_vec3(inst->world.m[2][0], inst->world.m[2][1], inst->world.m[2][2]));
            inst->sphere_radius = local_radius * fmaxf(sx, fmaxf(sy, sz));
        }
        ++count;
    }
    *out = instances;
    *out_count = count;
    return WPE_RESULT_OK;
}

/* ---- Lights --------------------------------------------------------------------------------------- */

typedef struct wpe__Sun {
    bool present;
    bool cast_shadows;
    WPE_Vec3 direction;
    WPE_Vec3 color;
} wpe__Sun;

static WPE_Result wpe__gather_lights(WPE_SceneRenderer* sr, const WPE_Ecs* ecs,
                                     const WPE_SceneRenderSettings* settings, wpe__Sun* out_sun,
                                     WPE_ShaderLight** out_lights, uint32_t* out_count) {
    const uint32_t capacity = wpe_ecs_component_count(ecs, WPE_COMP_LIGHT);
    WPE_ShaderLight* lights =
        wpe_arena_alloc(&sr->frame, (size_t)(capacity ? capacity : 1) * sizeof *lights, 0);
    if (!lights) return WPE_RESULT_ERROR_OUT_OF_MEMORY;

    wpe__Sun sun = {0};
    uint32_t count = 0;
    WPE_EcsIter it = wpe_ecs_query((WPE_Ecs*)ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_LIGHT) |
                                                                        WPE_ECS_MASK(WPE_COMP_TRANSFORM)});
    while (wpe_ecs_iter_next(&it)) {
        const WPE_Light* light = wpe_ecs_iter_get(&it, WPE_COMP_LIGHT);
        const WPE_Transform* t = wpe_ecs_iter_get(&it, WPE_COMP_TRANSFORM);
        const WPE_Vec3 color = wpe_vec3_scale(light->color, light->intensity);
        if (light->type == WPE_LIGHT_DIRECTIONAL) {
            /* Prefer a shadow-casting sun; otherwise keep the first directional light. */
            if (!sun.present || (light->cast_shadows && !sun.cast_shadows)) {
                sun = (wpe__Sun){.present = true,
                                 .cast_shadows = light->cast_shadows,
                                 .direction = wpe__forward(&t->world),
                                 .color = color};
            }
            continue;
        }
        if (!(light->range > 0.0f)) continue;
        const float outer = light->outer_cone > light->inner_cone ? light->outer_cone : light->inner_cone;
        lights[count++] = (WPE_ShaderLight){
            .position = wpe_vec3(t->world.m[3][0], t->world.m[3][1], t->world.m[3][2]),
            .direction = wpe__forward(&t->world),
            .color = color,
            .range = light->range,
            .cos_inner = cosf(light->inner_cone),
            .cos_outer = cosf(outer),
            .spot = light->type == WPE_LIGHT_SPOT,
        };
    }
    if (capacity == 0 && settings->default_sun) {
        sun = (wpe__Sun){.present = true,
                         .direction = wpe_vec3_normalize(settings->default_sun_direction),
                         .color = settings->default_sun_color};
    }
    *out_sun = sun;
    *out_lights = lights;
    *out_count = count;
    return WPE_RESULT_OK;
}

/* Picks the most influential lights for a bounding sphere (deterministic: ties keep world order). */
static uint32_t wpe__select_lights(const WPE_ShaderLight* lights, uint32_t count, WPE_Vec3 center,
                                   float radius, WPE_ShaderLight out[WPE_MAX_OBJECT_LIGHTS]) {
    float scores[WPE_MAX_OBJECT_LIGHTS];
    uint32_t chosen = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const float distance = wpe_vec3_length(wpe_vec3_sub(lights[i].position, center)) - radius;
        const float attenuation = wpe_light_attenuation(distance > 0.0f ? distance : 0.0f, lights[i].range);
        if (!(attenuation > 0.0f)) continue;
        const float score = attenuation * (lights[i].color.x + lights[i].color.y + lights[i].color.z);
        uint32_t slot = chosen < WPE_MAX_OBJECT_LIGHTS ? chosen++ : WPE_MAX_OBJECT_LIGHTS;
        if (slot == WPE_MAX_OBJECT_LIGHTS) {
            /* Replace the weakest selected light if this one is stronger. */
            uint32_t weakest = 0;
            for (uint32_t k = 1; k < WPE_MAX_OBJECT_LIGHTS; ++k)
                weakest = scores[k] < scores[weakest] ? k : weakest;
            if (!(score > scores[weakest])) continue;
            slot = weakest;
        }
        out[slot] = lights[i];
        scores[slot] = score;
    }
    return chosen;
}

/* ---- Shadows -------------------------------------------------------------------------------------- */

/*
 * Fits an orthographic light projection around a bounding sphere of the camera's view slice. The
 * sphere's size does not change as the camera rotates, and its center is snapped to whole shadow
 * texels, so the shadow map does not shimmer when the camera moves.
 */
static bool wpe__fit_shadow(const WPE_Ecs* ecs, WPE_Entity camera, float aspect, WPE_Vec3 sun_direction,
                            const WPE_ShadowSettings* shadows, WPE_Mat4* out_light_view_projection) {
    const WPE_Transform* ct = wpe_ecs_get_component(ecs, camera, WPE_COMP_TRANSFORM);
    const WPE_Camera* cam = wpe_ecs_get_component(ecs, camera, WPE_COMP_CAMERA);
    WPE_Mat4 camera_world;
    if (!wpe__camera_rigid_world(ct, &camera_world)) return false;

    const float near_z = cam->near_z;
    const float far_z = fminf(cam->far_z, fmaxf(shadows->distance, near_z * 2.0f));
    WPE_Vec3 corners[8];
    for (int i = 0; i < 8; ++i) {
        const float z = (i & 4) ? far_z : near_z;
        const float half_h = cam->orthographic ? cam->ortho_height * 0.5f : z * tanf(cam->fov_y * 0.5f);
        const float half_w = half_h * aspect;
        const WPE_Vec3 view_point = wpe_vec3((i & 1) ? half_w : -half_w, (i & 2) ? half_h : -half_h, -z);
        corners[i] = wpe_mat4_transform_point(&camera_world, view_point);
    }
    WPE_Vec3 center = wpe_vec3(0, 0, 0);
    for (int i = 0; i < 8; ++i)
        center = wpe_vec3_add(center, corners[i]);
    center = wpe_vec3_scale(center, 1.0f / 8.0f);
    float radius = 0.0f;
    for (int i = 0; i < 8; ++i)
        radius = fmaxf(radius, wpe_vec3_length(wpe_vec3_sub(corners[i], center)));
    radius = ceilf(radius * 16.0f) / 16.0f; /* Quantize so tiny camera changes keep the same extent. */
    if (!(radius > 0.0f)) return false;

    /* Light view looking along the sun direction from the origin. */
    const WPE_Vec3 dir = wpe_vec3_normalize(sun_direction);
    const WPE_Vec3 up = fabsf(dir.y) > 0.99f ? wpe_vec3(0, 0, 1) : wpe_vec3(0, 1, 0);
    const WPE_Mat4 light_view = wpe_mat4_look_at(wpe_vec3(0, 0, 0), dir, up);

    WPE_Vec3 c = wpe_mat4_transform_point(&light_view, center);
    const float texel = 2.0f * radius / (float)shadows->map_size;
    c.x = floorf(c.x / texel) * texel;
    c.y = floorf(c.y / texel) * texel;
    /* View-space z is negative in front of the light; casters may lie up to caster_extension closer to it. */
    const float near_plane = -(c.z + radius + shadows->caster_extension);
    const float far_plane = -(c.z - radius);
    const WPE_Mat4 projection =
        wpe_mat4_orthographic(c.x - radius, c.x + radius, c.y - radius, c.y + radius, near_plane, far_plane);
    *out_light_view_projection = wpe_mat4_mul(&projection, &light_view);
    return true;
}

static WPE_Result wpe__render_shadow_map(WPE_SceneRenderer* sr, WPE_JobSystem* jobs,
                                         const wpe__MeshInstance* instances, uint32_t instance_count,
                                         const WPE_Mat4* light_view_projection,
                                         const WPE_SceneRenderSettings* settings,
                                         WPE_SceneRenderStats* stats) {
    const uint32_t size = settings->shadows.map_size;
    if (!sr->shadow_pass) {
        const WPE_Result created = wpe_renderer_create(
            &(WPE_RendererDesc){.width = size, .height = size, .jobs = jobs}, &sr->shadow_pass);
        if (created != WPE_RESULT_OK) return created;
    } else {
        const WPE_Result resized = wpe_renderer_resize(sr->shadow_pass, size, size);
        if (resized != WPE_RESULT_OK) return resized;
    }

    WPE_Result result =
        wpe_renderer_begin_frame(sr->shadow_pass, &(WPE_ClearDesc){.clear_depth = true, .depth = 1.0f});
    for (uint32_t i = 0; i < instance_count && result == WPE_RESULT_OK; ++i) {
        const wpe__MeshInstance* inst = &instances[i];
        const WPE_Model* model = inst->mesh->model;
        const WPE_Mesh* mesh = &model->meshes[inst->mesh->mesh];
        const WPE_Mat4 mvp = wpe_mat4_mul(light_view_projection, &inst->world);
        const wpe__Frustum frustum = wpe__frustum_from_matrix(&mvp);
        const bool mirrored = wpe__determinant3(&inst->world) < 0.0f;

        for (uint32_t p = 0; p < mesh->primitive_count && result == WPE_RESULT_OK; ++p) {
            const WPE_MeshPrimitive* prim = &model->primitives[mesh->first_primitive + p];
            const WPE_Material* material = wpe__primitive_material(model, prim);
            /* Smoothly blended surfaces do not cast; dithered ones behave like cut-outs and do. */
            if (material->alpha_mode == WPE_ALPHA_BLEND && !settings->dither_transparency) continue;
            if (!inst->palette && !wpe__aabb_visible(&frustum, &prim->bounds)) continue;

            /* Front faces are culled for casters, which moves acne-prone depth to back faces. */
            WPE_RenderState state = wpe_render_state_default();
            state.depth_only = true;
            state.cull = material->double_sided ? WPE_CULL_NONE : (mirrored ? WPE_CULL_BACK : WPE_CULL_FRONT);
            const bool masked = material->alpha_mode != WPE_ALPHA_OPAQUE;
            const WPE_MaterialUniforms uniforms = {.mvp = mvp,
                                                   .model = model,
                                                   .material = material,
                                                   .joint_palette = inst->palette,
                                                   .joint_count =
                                                       inst->palette ? inst->mesh->joint_count : 0};
            result =
                wpe_renderer_submit(sr->shadow_pass, &(WPE_DrawDesc){
                                                         .program = wpe_shader_material_depth(masked),
                                                         .uniforms = &uniforms,
                                                         .uniforms_size = sizeof uniforms,
                                                         .vertices = &model->vertices[prim->vertex_offset],
                                                         .vertex_stride = sizeof(WPE_MeshVertex),
                                                         .vertex_count = prim->vertex_count,
                                                         .indices = &model->indices[prim->index_offset],
                                                         .index_count = prim->index_count,
                                                         .state = state,
                                                     });
            if (result == WPE_RESULT_OK) ++stats->shadow_casters;
        }
    }
    const WPE_Result end = wpe_renderer_end_frame(sr->shadow_pass);
    if (result != WPE_RESULT_OK) return result;
    if (end != WPE_RESULT_OK) return end;

    const WPE_Framebuffer* fb = wpe_renderer_framebuffer(sr->shadow_pass);
    sr->shadow = (WPE_ShadowSampler){.depth = fb->depth, .size = fb->width, .bias = settings->shadows.bias};
    sr->has_shadow = true;
    return WPE_RESULT_OK;
}

/* ---- Billboards ----------------------------------------------------------------------------------- */

/* What every billboard of the main pass shares. */
typedef struct wpe__BillboardPass {
    const WPE_Mat4* view;
    const WPE_Mat4* view_projection;
    WPE_Mat4 camera_world; /* Rigid. */
    WPE_Vec3 camera_position;
    const WPE_SceneRenderSettings* settings;
    const wpe__Sun* sun;
    const WPE_ShaderLight* lights;
    uint32_t light_count;
    const WPE_Mat4* shadow_matrix;
} wpe__BillboardPass;

/* The texture coordinates of the billboard's current frame: (u0, v0) top-left, (u1, v1) bottom-right. */
static void wpe__billboard_uvs(const WPE_Billboard* b, float* u0, float* v0, float* u1, float* v1) {
    const float tw = b->texture ? (float)b->texture->levels[0].width : 1.0f;
    const float th = b->texture ? (float)b->texture->levels[0].height : 1.0f;
    WPE_Rect r = b->source;
    if (!(r.width > 0.0f) || !(r.height > 0.0f)) r = (WPE_Rect){0.0f, 0.0f, tw, th};
    const uint32_t columns = b->columns ? b->columns : 1, rows = b->rows ? b->rows : 1;
    const uint32_t grid = columns * rows;
    const uint32_t count = b->frame_count && b->frame_count < grid ? b->frame_count : grid;
    const uint32_t frame = b->frame < count ? b->frame : count - 1;
    const float cw = r.width / (float)columns, ch = r.height / (float)rows;
    const uint32_t column = frame % columns, row = frame / columns; /* Whole cells. */
    *u0 = (r.x + (float)column * cw) / tw;
    *v0 = (r.y + (float)row * ch) / th;
    *u1 = *u0 + cw / tw;
    *v1 = *v0 + ch / th;
}

static WPE_Result wpe__render_billboards(WPE_SceneRenderer* sr, WPE_Renderer* renderer, const WPE_Ecs* ecs,
                                         const wpe__BillboardPass* pass, WPE_SceneRenderStats* stats) {
    static const uint32_t indices[6] = {0, 1, 2, 0, 2, 3};
    const wpe__Frustum frustum = wpe__frustum_from_matrix(pass->view_projection);
    WPE_Result result = WPE_RESULT_OK;
    WPE_EcsIter it = wpe_ecs_query((WPE_Ecs*)ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_BILLBOARD) |
                                                                        WPE_ECS_MASK(WPE_COMP_TRANSFORM)});
    while (wpe_ecs_iter_next(&it) && result == WPE_RESULT_OK) {
        const WPE_Billboard* b = wpe_ecs_iter_get(&it, WPE_COMP_BILLBOARD);
        const WPE_Transform* t = wpe_ecs_iter_get(&it, WPE_COMP_TRANSFORM);
        WPE_Vec3 corners[4], normal;
        if (!b->visible || !wpe_billboard_corners(b, &t->world, &pass->camera_world, corners, &normal))
            continue;
        WPE_Aabb box = {.min = corners[0], .max = corners[0]};
        for (int c = 1; c < 4; ++c) {
            box.min = wpe_vec3(fminf(box.min.x, corners[c].x), fminf(box.min.y, corners[c].y),
                               fminf(box.min.z, corners[c].z));
            box.max = wpe_vec3(fmaxf(box.max.x, corners[c].x), fmaxf(box.max.y, corners[c].y),
                               fmaxf(box.max.z, corners[c].z));
        }
        if (pass->settings->frustum_culling && !wpe__aabb_visible(&frustum, &box)) {
            ++stats->billboards_culled;
            continue;
        }
        /* The corners live in the frame arena: the renderer reads them until the frame ends. */
        wpe__BillboardVertex* vertices = wpe_arena_alloc(&sr->frame, 4 * sizeof *vertices, 0);
        if (!vertices) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
        float u0, v0, u1, v1;
        wpe__billboard_uvs(b, &u0, &v0, &u1, &v1);
        const float us[4] = {u0, u1, u1, u0}, vs[4] = {v1, v1, v0, v0};
        for (int c = 0; c < 4; ++c) {
            vertices[c] =
                (wpe__BillboardVertex){.position = corners[c], .normal = normal, .u = us[c], .v = vs[c]};
            for (int i = 0; i < 4; ++i)
                vertices[c].color[i] = b->color.v[i];
        }

        const WPE_Vec3 center = wpe_vec3_scale(wpe_vec3_add(box.min, box.max), 0.5f);
        wpe__BillboardUniforms uniforms = {
            .view_projection = *pass->view_projection,
            .camera_position = pass->camera_position,
            .texture = b->texture,
            .fog = pass->settings->fog,
            .cutout = b->blend == WPE_BILLBOARD_CUTOUT,
            .additive = b->blend == WPE_BILLBOARD_ADDITIVE,
            .lit = b->lit,
        };
        if (b->lit) {
            uniforms.ambient = pass->settings->ambient;
            uniforms.has_sun = pass->sun->present;
            uniforms.sun_direction = pass->sun->direction;
            uniforms.sun_color = pass->sun->present ? pass->sun->color : wpe_vec3(0, 0, 0);
            uniforms.sun_shadow = sr->has_shadow ? &sr->shadow : NULL;
            uniforms.shadow_matrix = *pass->shadow_matrix;
            uniforms.light_count =
                wpe__select_lights(pass->lights, pass->light_count, center,
                                   wpe_vec3_length(wpe_vec3_sub(box.max, center)), uniforms.lights);
        }

        /* Cut-outs are opaque; blended billboards go back to front with the other transparent geometry. */
        WPE_RenderState state = wpe_render_state_default();
        state.cull = WPE_CULL_NONE;
        bool transparent = false;
        if (b->blend == WPE_BILLBOARD_ADDITIVE) {
            state.blend = WPE_BLEND_ADDITIVE;
            state.depth_write = false;
            transparent = true;
        } else if (b->blend == WPE_BILLBOARD_ALPHA) {
            if (pass->settings->dither_transparency) {
                state.blend = WPE_BLEND_ALPHA_DITHER;
            } else {
                state.blend = WPE_BLEND_ALPHA;
                state.depth_write = false;
                transparent = true;
            }
        }
        const uint32_t depth_key = wpe__depth_key(-wpe_mat4_transform_point(pass->view, center).z);
        const uint64_t sort_key = transparent
                                      ? (UINT64_C(1) << 63) | ((uint64_t)(UINT32_MAX - depth_key) << 30)
                                      : (uint64_t)depth_key << 30;
        result = wpe_renderer_submit(renderer, &(WPE_DrawDesc){.program = wpe__shader_billboard(),
                                                               .uniforms = &uniforms,
                                                               .uniforms_size = sizeof uniforms,
                                                               .vertices = vertices,
                                                               .vertex_stride = sizeof *vertices,
                                                               .vertex_count = 4,
                                                               .indices = indices,
                                                               .index_count = 6,
                                                               .state = state,
                                                               .sort_key = sort_key});
        if (result == WPE_RESULT_OK) ++stats->billboards_submitted;
    }
    return result;
}

/* ---- Main pass ------------------------------------------------------------------------------------ */

WPE_Result wpe_scene_render(WPE_SceneRenderer* sr, WPE_Renderer* renderer, const WPE_Ecs* ecs,
                            WPE_Entity camera, const WPE_SceneRenderSettings* settings,
                            WPE_SceneRenderStats* out_stats) {
    if (!sr || !renderer || !ecs) return WPE_RESULT_ERROR_NULL_ARG;
    const WPE_SceneRenderSettings defaults = wpe_scene_render_settings_default();
    if (!settings) settings = &defaults;
    WPE_SceneRenderStats stats = {0};
    wpe_arena_reset(&sr->frame);
    sr->has_shadow = false;

    const WPE_Framebuffer* fb = wpe_renderer_framebuffer(renderer);
    const float aspect = (float)fb->width / (float)fb->height;
    WPE_Mat4 view, projection;
    if (!wpe_scene_camera_matrices(ecs, camera, aspect, &view, &projection)) {
        wpe_log_error("entity 0x%08x is not a valid camera (needs Transform and a valid Camera)", camera);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    const WPE_Mat4 view_projection = wpe_mat4_mul(&projection, &view);
    const WPE_Transform* camera_transform = wpe_ecs_get_component(ecs, camera, WPE_COMP_TRANSFORM);
    const WPE_Vec3 camera_position = wpe_vec3(
        camera_transform->world.m[3][0], camera_transform->world.m[3][1], camera_transform->world.m[3][2]);

    wpe__MeshInstance* instances = NULL;
    uint32_t instance_count = 0;
    WPE_Result result = wpe__gather_meshes(sr, ecs, &instances, &instance_count);
    wpe__Sun sun = {0};
    WPE_ShaderLight* lights = NULL;
    uint32_t light_count = 0;
    if (result == WPE_RESULT_OK) result = wpe__gather_lights(sr, ecs, settings, &sun, &lights, &light_count);
    stats.lights = light_count;

    /* Sun shadow map. */
    WPE_Mat4 shadow_matrix = wpe_mat4_identity();
    if (result == WPE_RESULT_OK && sun.present && sun.cast_shadows && settings->shadows.enabled &&
        settings->shadows.map_size > 0 && settings->shadows.map_size <= WPE_RENDERER_MAX_DIMENSION) {
        WPE_Mat4 light_view_projection;
        if (wpe__fit_shadow(ecs, camera, aspect, sun.direction, &settings->shadows, &light_view_projection)) {
            result = wpe__render_shadow_map(sr, wpe_renderer_jobs(renderer), instances, instance_count,
                                            &light_view_projection, settings, &stats);
            /* NDC -> shadow-map (u, v, depth): v grows downward like framebuffer rows. */
            const WPE_Mat4 to_texture = {
                .m = {{0.5f, 0, 0, 0}, {0, -0.5f, 0, 0}, {0, 0, 0.5f, 0}, {0.5f, 0.5f, 0.5f, 1}}};
            shadow_matrix = wpe_mat4_mul(&to_texture, &light_view_projection);
        }
    }

    for (uint32_t i = 0; i < instance_count && result == WPE_RESULT_OK; ++i) {
        const wpe__MeshInstance* inst = &instances[i];
        const WPE_Model* model = inst->mesh->model;
        const WPE_Mesh* mesh_data = &model->meshes[inst->mesh->mesh];
        const WPE_Mat4 mvp = wpe_mat4_mul(&view_projection, &inst->world);
        WPE_Mat4 inverse_world, normal_matrix = wpe_mat4_identity();
        if (wpe_mat4_inverse(&inst->world, &inverse_world)) {
            for (int c = 0; c < 4; ++c) {
                for (int r = 0; r < 4; ++r)
                    normal_matrix.m[c][r] = inverse_world.m[r][c];
            }
        }
        /* A mirroring transform flips triangle winding, so the culled face must flip too. */
        const bool mirrored = wpe__determinant3(&inst->world) < 0.0f;
        const wpe__Frustum frustum = wpe__frustum_from_matrix(&mvp);
        const WPE_Mat4 model_view = wpe_mat4_mul(&view, &inst->world);

        WPE_MaterialUniforms uniforms = {
            .mvp = mvp,
            .world = inst->world,
            .normal_matrix = normal_matrix,
            .camera_position = camera_position,
            .ambient = settings->ambient,
            .has_sun = sun.present,
            .sun_direction = sun.direction,
            .sun_color = sun.color,
            .sun_shadow = sr->has_shadow ? &sr->shadow : NULL,
            .shadow_matrix = shadow_matrix,
            .fog = settings->fog,
            .model = model,
            .joint_palette = inst->palette,
            .joint_count = inst->palette ? inst->mesh->joint_count : 0,
        };
        uniforms.light_count = wpe__select_lights(lights, light_count, inst->sphere_center,
                                                  inst->sphere_radius, uniforms.lights);

        for (uint32_t p = 0; p < mesh_data->primitive_count && result == WPE_RESULT_OK; ++p) {
            const WPE_MeshPrimitive* prim = &model->primitives[mesh_data->first_primitive + p];
            if (settings->frustum_culling && !inst->palette && !wpe__aabb_visible(&frustum, &prim->bounds)) {
                ++stats.primitives_culled;
                continue;
            }
            const WPE_Material* material = wpe__primitive_material(model, prim);

            WPE_RenderState state = wpe_render_state_default();
            state.cull = material->double_sided ? WPE_CULL_NONE : (mirrored ? WPE_CULL_FRONT : WPE_CULL_BACK);
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
            const uint32_t depth_key = wpe__depth_key(-wpe_mat4_transform_point(&model_view, center).z);
            const uint64_t sort_key = transparent
                                          ? (UINT64_C(1) << 63) | ((uint64_t)(UINT32_MAX - depth_key) << 30)
                                          : (uint64_t)depth_key << 30;

            uniforms.material = material;
            uniforms.shininess = wpe_material_shininess(wpe__material_gloss(material));
            result = wpe_renderer_submit(renderer,
                                         &(WPE_DrawDesc){
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
                                             .subdivision_levels = settings->subdivision_levels,
                                             .subdivision_edge_pixels = settings->subdivision_edge_pixels,
                                         });
            if (result == WPE_RESULT_OK) ++stats.primitives_submitted;
        }
    }
    if (result == WPE_RESULT_OK && wpe_ecs_component_count(ecs, WPE_COMP_BILLBOARD) > 0) {
        wpe__BillboardPass pass = {.view = &view,
                                   .view_projection = &view_projection,
                                   .camera_position = camera_position,
                                   .settings = settings,
                                   .sun = &sun,
                                   .lights = lights,
                                   .light_count = light_count,
                                   .shadow_matrix = &shadow_matrix};
        if (wpe__camera_rigid_world(camera_transform, &pass.camera_world))
            result = wpe__render_billboards(sr, renderer, ecs, &pass, &stats);
    }
    if (out_stats) *out_stats = stats;
    return result;
}
