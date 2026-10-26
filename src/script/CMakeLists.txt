add_library(wpe_script STATIC
    script_api.c
    script_host.c)
add_library(wpe::script ALIAS wpe_script)

target_link_libraries(wpe_script
    PUBLIC wpe::core wpe::ecs wpe::scene wpe::graphics wpe::assets wpe::platform
    PRIVATE wpe_dep_luau)
wpe_configure_target(wpe_script)
