add_library(wpe_vfs STATIC
    pak.c
    vfs.c)
add_library(wpe::vfs ALIAS wpe_vfs)

target_link_libraries(wpe_vfs
    PUBLIC wpe::core wpe::platform
    PRIVATE wpe_dep_zstd)
wpe_configure_target(wpe_vfs)
