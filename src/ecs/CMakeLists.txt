add_library(wpe_ecs STATIC ecs.c)
add_library(wpe::ecs ALIAS wpe_ecs)

target_link_libraries(wpe_ecs PUBLIC wpe::core)
wpe_configure_target(wpe_ecs)
