add_library(wpe_math STATIC wpe_math.c)
add_library(wpe::math ALIAS wpe_math)

target_link_libraries(wpe_math
    PUBLIC wpe::core
    PRIVATE wpe_dep_cglm)

# libm is separate from libc on Linux.
if(UNIX)
    target_link_libraries(wpe_math PUBLIC m)
endif()

wpe_configure_target(wpe_math)
