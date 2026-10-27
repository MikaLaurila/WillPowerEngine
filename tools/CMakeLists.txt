# Command-line tools and the runtime share one output directory, so wpe_export finds wpe_runtime
# next to itself.
set(WPE_BIN_DIR "${PROJECT_BINARY_DIR}/bin")

add_executable(wpe_pak pak/main.c)
target_link_libraries(wpe_pak PRIVATE wpe::vfs)
wpe_configure_target(wpe_pak)
set_target_properties(wpe_pak PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${WPE_BIN_DIR}")

add_executable(wpe_export export/main.c)
target_link_libraries(wpe_export PRIVATE wpe::engine)
target_compile_definitions(wpe_export PRIVATE WPE_EXE_SUFFIX="${CMAKE_EXECUTABLE_SUFFIX}")
wpe_configure_target(wpe_export)
set_target_properties(wpe_export PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${WPE_BIN_DIR}")
