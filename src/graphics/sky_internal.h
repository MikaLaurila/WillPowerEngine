/**
 * @file sky_internal.h
 * @brief Private: the sky pass of the scene renderer (sky.c). Not a public API.
 */
#ifndef WPE_GRAPHICS_SKY_INTERNAL_H
#define WPE_GRAPHICS_SKY_INTERNAL_H

#include "graphics/scene_render.h"

/* Submits the sky (nothing in WPE_SKY_COLOR mode, or a panorama without its image): one triangle over the
 * whole screen at the far plane, depth-tested so it fills only what geometry left empty. `sun_direction` is
 * the direction the sun's light travels. */
WPE_Result wpe__render_sky(WPE_Renderer* renderer, const WPE_SkySettings* sky,
                           const WPE_Mat4* view_projection, bool has_sun, WPE_Vec3 sun_direction);

#endif /* WPE_GRAPHICS_SKY_INTERNAL_H */
