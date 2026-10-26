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
add_subdirectory(stress)

# Builds every test executable, then runs the whole suite.
add_custom_target(test_all
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -C $<CONFIG>
    WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
    DEPENDS wpe_tests
    USES_TERMINAL)
