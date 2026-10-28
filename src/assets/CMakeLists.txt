# Third-party decoders, compiled once without the engine's strict warnings.
add_library(wpe_assets_third_party STATIC
    third_party/cgltf_impl.c
    third_party/stb_image_impl.c)
target_link_libraries(wpe_assets_third_party
    PUBLIC wpe_dep_cgltf wpe_dep_stb
    PRIVATE wpe::core)

# The built-in font (assets/fonts/small.png) as a byte array; reconfigures when the image changes, and
# file(CONFIGURE) rewrites the header only when its content does.
set(WPE_BUILTIN_FONT "${PROJECT_SOURCE_DIR}/assets/fonts/small.png")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${WPE_BUILTIN_FONT}")
file(READ "${WPE_BUILTIN_FONT}" wpe_font_hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," wpe_font_bytes "${wpe_font_hex}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/assets/builtin_font_data.h"
     CONTENT "/* Generated from assets/fonts/small.png by src/assets/CMakeLists.txt. Do not edit. */
static const unsigned char wpe__builtin_font_png[] = {${wpe_font_bytes}};
" @ONLY)

add_library(wpe_assets STATIC
    asset_manager.c
    builtin_font.c
    font.c
    image.c
    model.c
    procedural.c)
add_library(wpe::assets ALIAS wpe_assets)

target_link_libraries(wpe_assets
    PUBLIC wpe::core wpe::math wpe::renderer wpe::jobs wpe::platform wpe::vfs
    PRIVATE wpe_assets_third_party)
target_include_directories(wpe_assets PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
wpe_configure_target(wpe_assets)
