add_library(wpe_physics STATIC physics.c)
add_library(wpe::physics ALIAS wpe_physics)

target_link_libraries(wpe_physics
    PUBLIC wpe::core wpe::math wpe::ecs wpe::scene wpe::assets
    PRIVATE wpe_dep_box3d)
wpe_configure_target(wpe_physics)
