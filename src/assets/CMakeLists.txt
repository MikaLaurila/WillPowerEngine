# Third-party decoders, compiled once without the engine's strict warnings.
add_library(wpe_assets_third_party STATIC
    third_party/cgltf_impl.c
    third_party/stb_image_impl.c)
target_link_libraries(wpe_assets_third_party
    PUBLIC wpe_dep_cgltf wpe_dep_stb
    PRIVATE wpe::core)

add_library(wpe_assets STATIC
    asset_manager.c
    font.c
    image.c
    model.c
    procedural.c)
add_library(wpe::assets ALIAS wpe_assets)

target_link_libraries(wpe_assets
    PUBLIC wpe::core wpe::math wpe::renderer wpe::jobs wpe::platform wpe::vfs
    PRIVATE wpe_assets_third_party)
wpe_configure_target(wpe_assets)
