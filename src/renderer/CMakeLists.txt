add_library(wpe_renderer STATIC
    geometry.c
    raster.c
    renderer.c
    shaders.c
    texture.c)
add_library(wpe::renderer ALIAS wpe_renderer)

target_link_libraries(wpe_renderer PUBLIC wpe::core wpe::math wpe::jobs)
wpe_configure_target(wpe_renderer)

# The rasterizer's inner loops dominate frame time; always optimize them, even in Debug builds,
# so Debug stays usable for interactive work. Assertions remain enabled (WPE_DEBUG is unaffected).
if(NOT MSVC)
    set_source_files_properties(geometry.c raster.c texture.c PROPERTIES COMPILE_OPTIONS "-O2")
endif()
