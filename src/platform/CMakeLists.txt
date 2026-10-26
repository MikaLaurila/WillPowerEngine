add_library(wpe_platform STATIC platform.c)
add_library(wpe::platform ALIAS wpe_platform)

target_link_libraries(wpe_platform
    PUBLIC wpe::core wpe::renderer
    PRIVATE wpe_dep_sdl3)
wpe_configure_target(wpe_platform)
