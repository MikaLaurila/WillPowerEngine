add_executable(spinning_cubes spinning_cubes/main.c)
target_link_libraries(spinning_cubes PRIVATE wpe::core wpe::jobs wpe::scene wpe::renderer wpe::platform)
wpe_configure_target(spinning_cubes)

add_executable(model_viewer model_viewer/main.c)
target_link_libraries(model_viewer PRIVATE wpe::graphics wpe::platform)
target_compile_definitions(model_viewer PRIVATE WPE_SAMPLE_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets")
wpe_configure_target(model_viewer)
