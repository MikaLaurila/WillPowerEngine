add_library(wpe_ui STATIC
    ui.c)
add_library(wpe::ui ALIAS wpe_ui)

target_link_libraries(wpe_ui PUBLIC wpe::core wpe::math wpe::ecs wpe::scene wpe::renderer wpe::assets wpe::graphics
                                    wpe::input)
wpe_configure_target(wpe_ui)
