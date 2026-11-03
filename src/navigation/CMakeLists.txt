add_library(wpe_navigation STATIC nav_build.c nav_flow.c nav_geometry.c nav_grid.c nav_path.c)
add_library(wpe::navigation ALIAS wpe_navigation)

target_link_libraries(wpe_navigation
    PUBLIC wpe::core wpe::math wpe::ecs wpe::scene wpe::assets wpe::physics)
wpe_configure_target(wpe_navigation)
