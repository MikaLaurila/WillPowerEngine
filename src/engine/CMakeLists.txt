add_library(wpe_engine STATIC
    engine.c
    export.c
    json_read.c
    prefab.c
    project.c
    scene_file.c
    scene_file_ui.c)
add_library(wpe::engine ALIAS wpe_engine)

target_link_libraries(wpe_engine
    PUBLIC wpe::core wpe::math wpe::ecs wpe::scene wpe::jobs wpe::renderer wpe::platform wpe::input wpe::vfs
           wpe::assets wpe::graphics wpe::physics wpe::audio wpe::ui wpe::script)
wpe_configure_target(wpe_engine)
