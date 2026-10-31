add_library(wpe_scene STATIC scene.c game_feel.c)
add_library(wpe::scene ALIAS wpe_scene)

target_link_libraries(wpe_scene PUBLIC wpe::core wpe::math wpe::ecs)
wpe_configure_target(wpe_scene)
