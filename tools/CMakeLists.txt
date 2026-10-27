add_executable(wpe_pak pak/main.c)
target_link_libraries(wpe_pak PRIVATE wpe::vfs)
wpe_configure_target(wpe_pak)
