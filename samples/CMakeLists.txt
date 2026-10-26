add_executable(spinning_cubes spinning_cubes/main.c)
target_link_libraries(spinning_cubes PRIVATE wpe::core wpe::jobs wpe::scene wpe::renderer wpe::platform)
wpe_configure_target(spinning_cubes)
