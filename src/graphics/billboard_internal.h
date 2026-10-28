/**
 * @file billboard_internal.h
 * @brief Private: the billboard shader shared by billboard.c, particles.c and scene_render.c, and the
 * particle hooks. Not a public API.
 */
#ifndef WPE_GRAPHICS_BILLBOARD_INTERNAL_H
#define WPE_GRAPHICS_BILLBOARD_INTERNAL_H

#include "graphics/graphics.h"
#include "graphics/material_shader.h"

/* A billboard corner in world space. */
typedef struct wpe__BillboardVertex {
    WPE_Vec3 position;
    WPE_Vec3 normal;
    float u, v;
    float color[4];
} wpe__BillboardVertex;

typedef struct wpe__BillboardUniforms {
    WPE_Mat4 view_projection;
    WPE_Vec3 camera_position;
    const WPE_Texture* texture; /* NULL: white. Borrowed until the frame ends. */
    WPE_FogParams fog;
    bool cutout;   /* Discard texels with alpha below 0.5. */
    bool additive; /* Fog fades the color to nothing instead of to the fog color. */
    bool lit;
    WPE_Vec3 ambient;
    bool has_sun;
    WPE_Vec3 sun_direction;              /* Direction the light travels (normalized). */
    WPE_Vec3 sun_color;                  /* Pre-multiplied by intensity. */
    const WPE_ShadowSampler* sun_shadow; /* NULL = unshadowed. Borrowed until the frame ends. */
    WPE_Mat4 shadow_matrix;
    WPE_ShaderLight lights[WPE_MAX_OBJECT_LIGHTS];
    uint32_t light_count;
} wpe__BillboardUniforms;

/* The billboard program (static storage): vertices are wpe__BillboardVertex, uniforms
 * wpe__BillboardUniforms. Particles draw through it too. */
const WPE_ShaderProgram* wpe__shader_billboard(void);

/* Particle emitter hooks (particles.c), registered by wpe_graphics_register_components(); `user_data` is the
 * asset manager. */
void wpe__particles_on_add(WPE_Ecs* ecs, WPE_Entity entity, void* component, void* user_data);
void wpe__particles_on_remove(WPE_Ecs* ecs, WPE_Entity entity, void* component, void* user_data);

#endif /* WPE_GRAPHICS_BILLBOARD_INTERNAL_H */
