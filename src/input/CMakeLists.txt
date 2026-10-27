add_library(wpe_input STATIC input.c)
add_library(wpe::input ALIAS wpe_input)

target_link_libraries(wpe_input
    PUBLIC wpe::core wpe::platform
    PRIVATE wpe_dep_sdl3)
wpe_configure_target(wpe_input)
