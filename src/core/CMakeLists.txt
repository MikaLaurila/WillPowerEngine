configure_file(version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/core/version.h" @ONLY)

add_library(wpe_core STATIC
    alloc.c
    arena.c
    debug.c
    log.c
    result.c)
add_library(wpe::core ALIAS wpe_core)

# Public includes are rooted at src/ so headers are referenced as "core/log.h".
target_include_directories(wpe_core PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")

wpe_configure_target(wpe_core)
