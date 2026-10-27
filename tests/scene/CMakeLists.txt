wpe_add_test(integration_full_scene SOURCES test_full_scene.c LIBS wpe::engine LABELS integration scene)
target_compile_definitions(test_integration_full_scene PRIVATE WPE_TEST_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets")
set_tests_properties(integration_full_scene PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" TIMEOUT 600)
