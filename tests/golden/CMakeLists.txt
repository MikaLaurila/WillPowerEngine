wpe_add_test(renderer_golden SOURCES test_golden.c LIBS wpe::renderer wpe::platform wpe::graphics LABELS golden renderer)
target_include_directories(test_renderer_golden PRIVATE "${PROJECT_SOURCE_DIR}/samples/common")
target_compile_definitions(test_renderer_golden PRIVATE
    WPE_GOLDEN_REFERENCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/references"
    WPE_GOLDEN_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets")
# Failure artifacts (<name>_actual.png) land in the test's working directory.
set_tests_properties(renderer_golden PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
