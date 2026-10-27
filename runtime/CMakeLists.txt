add_executable(wpe_runtime main.c)
target_link_libraries(wpe_runtime PRIVATE wpe::engine)
wpe_configure_target(wpe_runtime)
set_target_properties(wpe_runtime PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
if(WIN32)
    # A game opens a window, not a console; started from a terminal it still logs there
    # (wpe_platform_attach_console).
    set_target_properties(wpe_runtime PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
# Exported games are this executable: link the compiler runtimes statically so nothing besides
# the operating system's own libraries has to ship with them.
if(MINGW)
    # Also covers winpthread; Windows system DLLs (and the Universal CRT) stay dynamic.
    target_link_options(wpe_runtime PRIVATE -static)
elseif(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND NOT SANITIZERS)
    target_link_options(wpe_runtime PRIVATE -static-libgcc -static-libstdc++)
endif()
