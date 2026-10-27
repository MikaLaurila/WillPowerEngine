#define WPE_LOG_MODULE "graphics"
#include <string.h>

#include "core/alloc.h"
#include "core/debug.h"
#include "core/log.h"
#include "graphics/graphics.h"

/* ---- Hooks ---------------------------------------------------------------------------------------- */

/* Takes the component's reference to its model and resolves the pointer (NULL if invalid). */
static const WPE_Model* wpe__acquire_model(WPE_AssetManager* assets, WPE_AssetHandle handle) {
    const WPE_Model* model = wpe_asset_model(assets, handle);
    if (model) {
        wpe_asset_retain(assets, handle);
    } else if (handle != WPE_ASSET_NONE) {
        wpe_log_error("component references 0x%08x, which is not a loaded model", handle);
    }
    return model;
}

static void wpe__mesh_renderer_on_add(WPE_Ecs* ecs, WPE_Entity entity, void* component, void* user_data) {
    (void)ecs;
    (void)entity;
    WPE_MeshRenderer* mesh = component;
    mesh->model = wpe__acquire_model(user_data, mesh->model_asset);
}

static void wpe__mesh_renderer_on_remove(WPE_Ecs* ecs, WPE_Entity entity, void* component, void* user_data) {
    (void)ecs;
    (void)entity;
    WPE_MeshRenderer* mesh = component;
    if (mesh->model) wpe_asset_release(user_data, mesh->model_asset);
    mesh->model = NULL;
    wpe_free(mesh->joints);
    mesh->joints = NULL;
    mesh->joint_count = 0;
}

static void wpe__animator_on_add(WPE_Ecs* ecs, WPE_Entity entity, void* component, void* user_data) {
    (void)ecs;
    (void)entity;
    WPE_Animator* animator = component;
    animator->model = wpe__acquire_model(user_data, animator->model_asset);
}

static void wpe__animator_on_remove(WPE_Ecs* ecs, WPE_Entity entity, void* component, void* user_data) {
    (void)ecs;
    (void)entity;
    WPE_Animator* animator = component;
    if (animator->model) wpe_asset_release(user_data, animator->model_asset);
    animator->model = NULL;
    wpe_free(animator->node_entities);
    animator->node_entities = NULL;
    animator->node_count = 0;
}

WPE_Camera wpe_camera_default(void) {
    return (WPE_Camera){.fov_y = WPE_PI / 3.0f, .near_z = 0.1f, .far_z = 500.0f, .ortho_height = 10.0f};
}

WPE_Light wpe_light_default(WPE_LightType type) {
    return (WPE_Light){.type = type,
                       .color = wpe_vec3(1, 1, 1),
                       .intensity = 1.0f,
                       .range = 10.0f,
                       .inner_cone = 20.0f * WPE_PI / 180.0f,
                       .outer_cone = 30.0f * WPE_PI / 180.0f};
}

WPE_Result wpe_graphics_register_components(WPE_Ecs* ecs, WPE_AssetManager* assets) {
    if (!ecs || !assets) return WPE_RESULT_ERROR_NULL_ARG;
    const struct {
        WPE_ComponentType type;
        WPE_ComponentDesc desc;
    } components[] = {
        {WPE_COMP_MESH,
         {.name = "MeshRenderer",
          .size = sizeof(WPE_MeshRenderer),
          .alignment = alignof(WPE_MeshRenderer),
          .on_add = wpe__mesh_renderer_on_add,
          .on_remove = wpe__mesh_renderer_on_remove,
          .hook_user_data = assets}},
        {WPE_COMP_CAMERA, {.name = "Camera", .size = sizeof(WPE_Camera), .alignment = alignof(WPE_Camera)}},
        {WPE_COMP_LIGHT, {.name = "Light", .size = sizeof(WPE_Light), .alignment = alignof(WPE_Light)}},
        {WPE_COMP_ANIMATOR,
         {.name = "Animator",
          .size = sizeof(WPE_Animator),
          .alignment = alignof(WPE_Animator),
          .on_add = wpe__animator_on_add,
          .on_remove = wpe__animator_on_remove,
          .hook_user_data = assets}},
    };
    for (size_t i = 0; i < sizeof components / sizeof components[0]; ++i) {
        const WPE_Result result = wpe_ecs_register_component(ecs, components[i].type, &components[i].desc);
        if (result != WPE_RESULT_OK) {
            wpe_log_error("failed to register component '%s': %s", components[i].desc.name,
                          wpe_result_to_string(result));
            return result;
        }
    }
    return WPE_RESULT_OK;
}

/* ---- Instantiation -------------------------------------------------------------------------------- */

static WPE_Result wpe__spawn_node_entity(WPE_Ecs* ecs, const WPE_ModelNode* node, WPE_Entity parent,
                                         WPE_Entity* out) {
    const WPE_Entity e = wpe_ecs_spawn(ecs);
    if (e == WPE_ENTITY_NULL) return WPE_RESULT_ERROR_CAPACITY;
    *out = e;

    WPE_Transform t = wpe_transform_identity();
    t.position = node->translation;
    t.rotation = node->rotation;
    t.scale = node->scale;
    WPE_Result result = wpe_ecs_add_component(ecs, e, WPE_COMP_TRANSFORM, &t, NULL);
    if (result == WPE_RESULT_OK && node->name[0]) result = wpe_scene_set_name(ecs, e, node->name);
    if (result == WPE_RESULT_OK) result = wpe_scene_set_parent(ecs, e, parent, false);
    return result;
}

/* Binds a skinned mesh renderer to the entities of its joints. */
static WPE_Result wpe__bind_skin(WPE_Ecs* ecs, const WPE_Model* model, const WPE_Entity* node_entities,
                                 WPE_Entity entity, uint32_t skin_index) {
    const WPE_Skin* skin = &model->skins[skin_index];
    WPE_Entity* joints = wpe_alloc_zeroed(skin->joint_count ? skin->joint_count : 1, sizeof *joints);
    if (!joints) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    for (uint32_t j = 0; j < skin->joint_count; ++j) {
        const uint32_t node = model->skin_joints[skin->first_joint + j];
        if (node >= model->node_count) {
            wpe_free(joints);
            return WPE_RESULT_ERROR_PARSE;
        }
        joints[j] = node_entities[node];
    }
    WPE_MeshRenderer* mesh = wpe_ecs_get_component(ecs, entity, WPE_COMP_MESH);
    wpe_assert(mesh != NULL);
    mesh->skin = skin_index;
    mesh->joints = joints;
    mesh->joint_count = skin->joint_count;
    return WPE_RESULT_OK;
}

/* Verifies that every vertex of a skinned mesh references a joint the skin provides. */
static bool wpe__skin_covers_mesh(const WPE_Model* model, uint32_t mesh_index, uint32_t skin_index) {
    const WPE_Mesh* mesh = &model->meshes[mesh_index];
    const uint32_t joint_count = model->skins[skin_index].joint_count;
    for (uint32_t p = 0; p < mesh->primitive_count; ++p) {
        const WPE_MeshPrimitive* prim = &model->primitives[mesh->first_primitive + p];
        for (uint32_t v = 0; v < prim->vertex_count; ++v) {
            const WPE_MeshVertex* vertex = &model->vertices[prim->vertex_offset + v];
            for (int j = 0; j < 4; ++j) {
                if (vertex->weights[j] > 0.0f && vertex->joints[j] >= joint_count) return false;
            }
        }
    }
    return true;
}

WPE_Result wpe_model_instantiate(WPE_Ecs* ecs, WPE_AssetManager* assets, WPE_AssetHandle model_asset,
                                 const WPE_InstantiateOptions* options, WPE_Entity* out_root) {
    if (!ecs || !assets || !out_root) return WPE_RESULT_ERROR_NULL_ARG;
    *out_root = WPE_ENTITY_NULL;
    const WPE_Model* model = wpe_asset_model(assets, model_asset);
    if (!model) {
        wpe_log_error("cannot instantiate 0x%08x: not a loaded model", model_asset);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    const WPE_InstantiateOptions defaults = {.animation = WPE_MODEL_NONE};
    if (!options) options = &defaults;
    if (options->animation != WPE_MODEL_NONE && options->animation >= model->animation_count) {
        wpe_log_error("animation index %u out of range (%u animations)", options->animation,
                      model->animation_count);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }

    WPE_Entity* node_entities =
        wpe_alloc_zeroed(model->node_count ? model->node_count : 1, sizeof *node_entities);
    uint32_t* stack = wpe_alloc((model->node_count ? model->node_count : 1) * sizeof *stack);
    if (!node_entities || !stack) {
        wpe_free(node_entities);
        wpe_free(stack);
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }

    WPE_Entity root = wpe_ecs_spawn(ecs);
    WPE_Result result = root != WPE_ENTITY_NULL ? WPE_RESULT_OK : WPE_RESULT_ERROR_CAPACITY;
    if (result == WPE_RESULT_OK) {
        const WPE_Transform t = wpe_transform_identity();
        result = wpe_ecs_add_component(ecs, root, WPE_COMP_TRANSFORM, &t, NULL);
    }
    if (result == WPE_RESULT_OK) result = wpe_scene_set_name(ecs, root, "model");

    /* Depth-first over the default scene so parents exist before their children. */
    for (uint32_t r = 0; r < model->root_count && result == WPE_RESULT_OK; ++r) {
        uint32_t top = 0;
        stack[top++] = model->root_nodes[r];
        while (top > 0 && result == WPE_RESULT_OK) {
            const uint32_t n = stack[--top];
            const WPE_ModelNode* node = &model->nodes[n];
            if (node_entities[n] != WPE_ENTITY_NULL) continue; /* Malformed graphs may revisit a node. */
            const WPE_Entity parent = node->parent != WPE_MODEL_NONE ? node_entities[node->parent] : root;
            result = wpe__spawn_node_entity(ecs, node, parent != WPE_ENTITY_NULL ? parent : root,
                                            &node_entities[n]);
            if (result == WPE_RESULT_OK && node->mesh != WPE_MODEL_NONE) {
                const WPE_MeshRenderer mesh = {
                    .model_asset = model_asset, .mesh = node->mesh, .skin = WPE_MODEL_NONE, .visible = true};
                result = wpe_ecs_add_component(ecs, node_entities[n], WPE_COMP_MESH, &mesh, NULL);
            }
            for (uint32_t c = node->first_child; c != WPE_MODEL_NONE && top < model->node_count;
                 c = model->nodes[c].next_sibling) {
                stack[top++] = c;
            }
        }
    }

    /* Skins need all joint entities, so bind them after the hierarchy exists. */
    for (uint32_t n = 0; n < model->node_count && result == WPE_RESULT_OK; ++n) {
        const WPE_ModelNode* node = &model->nodes[n];
        if (node->skin == WPE_MODEL_NONE || node->mesh == WPE_MODEL_NONE ||
            node_entities[n] == WPE_ENTITY_NULL)
            continue;
        if (!wpe__skin_covers_mesh(model, node->mesh, node->skin)) {
            wpe_log_error("skin %u does not provide all joints used by mesh %u", node->skin, node->mesh);
            result = WPE_RESULT_ERROR_PARSE;
            break;
        }
        result = wpe__bind_skin(ecs, model, node_entities, node_entities[n], node->skin);
    }

    if (result == WPE_RESULT_OK && options->animation != WPE_MODEL_NONE) {
        const WPE_Animator animator = {.model_asset = model_asset,
                                       .animation = options->animation,
                                       .speed = 1.0f,
                                       .playing = options->play,
                                       .loop = true,
                                       .node_entities = node_entities,
                                       .node_count = model->node_count};
        result = wpe_ecs_add_component(ecs, root, WPE_COMP_ANIMATOR, &animator, NULL);
        if (result == WPE_RESULT_OK) node_entities = NULL; /* Owned by the animator now. */
    }

    wpe_free(node_entities);
    wpe_free(stack);
    if (result != WPE_RESULT_OK) {
        if (root != WPE_ENTITY_NULL) wpe_scene_destroy_recursive(ecs, root);
        return result;
    }
    *out_root = root;
    return WPE_RESULT_OK;
}
