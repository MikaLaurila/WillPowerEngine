add_library(wpe_graphics STATIC
    animation.c
    billboard.c
    components.c
    debug_lines.c
    draw2d.c
    material_shader.c
    particles.c
    postfx.c
    scene_render.c
    sky.c
    trail.c)
add_library(wpe::graphics ALIAS wpe_graphics)

target_link_libraries(wpe_graphics PUBLIC wpe::core wpe::math wpe::ecs wpe::scene wpe::renderer wpe::assets)
wpe_configure_target(wpe_graphics)

# Shaders run per vertex/pixel; keep them optimized in Debug builds as well.
if(NOT MSVC)
    set_source_files_properties(material_shader.c billboard.c draw2d.c postfx.c sky.c PROPERTIES COMPILE_OPTIONS "-O2")
endif()
