wpe_add_test(integration_script_scene SOURCES test_script_scene.c LIBS wpe::script LABELS integration script)
target_compile_definitions(test_integration_script_scene PRIVATE WPE_TEST_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets")
