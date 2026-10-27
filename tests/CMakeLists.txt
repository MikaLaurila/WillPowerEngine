add_library(wpe_test_framework STATIC framework/wpe_test.c)
target_include_directories(wpe_test_framework PUBLIC framework)
target_link_libraries(wpe_test_framework PUBLIC wpe::core)
wpe_configure_target(wpe_test_framework)

add_custom_target(wpe_tests)

# Registers a test executable as a CTest test.
#   wpe_add_test(<ctest name> SOURCES <files...> [LIBS <libs...>] [LABELS <labels...>])
# Name tests "<module>_<area>" so `ctest -R "^<module>_"` selects a whole module.
function(wpe_add_test name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBS;LABELS" ${ARGN})
    add_executable(test_${name} ${ARG_SOURCES})
    target_link_libraries(test_${name} PRIVATE wpe_test_framework ${ARG_LIBS})
    wpe_configure_target(test_${name})
    add_test(NAME ${name} COMMAND test_${name})
    set_tests_properties(${name} PROPERTIES LABELS "${ARG_LABELS}" TIMEOUT 120)
    add_dependencies(wpe_tests test_${name})
endfunction()

add_subdirectory(unit)
add_subdirectory(scene)
add_subdirectory(stress)
add_subdirectory(golden)

# Tool round trip: pack the sample assets, verify, extract and compare.
add_test(NAME tools_pak_roundtrip
    COMMAND "${CMAKE_COMMAND}" -DWPE_PAK=$<TARGET_FILE:wpe_pak> -DSOURCE=${PROJECT_SOURCE_DIR}/assets
            -DWORK=${CMAKE_CURRENT_BINARY_DIR}/pak_roundtrip -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/pak_roundtrip.cmake)
set_tests_properties(tools_pak_roundtrip PROPERTIES LABELS "tools;vfs" TIMEOUT 300)
add_dependencies(wpe_tests wpe_pak)

# Export round trip: export the sample project and play the exported executable headless.
add_test(NAME tools_export_roundtrip
    COMMAND "${CMAKE_COMMAND}" -DWPE_EXPORT=$<TARGET_FILE:wpe_export> -DWPE_RUNTIME=$<TARGET_FILE:wpe_runtime>
            -DPROJECT=${PROJECT_SOURCE_DIR}/assets -DWORK=${CMAKE_CURRENT_BINARY_DIR}/export_roundtrip
            -DSUFFIX=${CMAKE_EXECUTABLE_SUFFIX} -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/export_roundtrip.cmake)
set_tests_properties(tools_export_roundtrip PROPERTIES LABELS "tools;engine" TIMEOUT 600)
add_dependencies(wpe_tests wpe_export wpe_runtime)

# The editor's command line, driven like an agent would drive it.
add_test(NAME tools_editor_cli
    COMMAND "${CMAKE_COMMAND}" -DWPE_EDITOR=$<TARGET_FILE:wpe_editor> -DWORK=${CMAKE_CURRENT_BINARY_DIR}/editor_cli
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/editor_cli.cmake)
set_tests_properties(tools_editor_cli PROPERTIES LABELS "tools;editor" TIMEOUT 600)
add_dependencies(wpe_tests wpe_editor)

# Builds every test executable, then runs the whole suite.
add_custom_target(test_all
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -C $<CONFIG>
    WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
    DEPENDS wpe_tests
    USES_TERMINAL)
