# Third-party dependencies, fetched at configure time and pinned by archive hash.
#
# Each dependency is exposed as an INTERFACE/imported target named wpe_dep_<name>. Include
# directories are marked SYSTEM so dependency headers never trigger our -Werror warnings.
# Only wrapper modules may link these targets; public engine headers must not include them.

include(FetchContent)

# cglm v0.9.7 (MIT) — header-only usage. SOURCE_SUBDIR points at a directory with no
# CMakeLists.txt so FetchContent only downloads the sources instead of building the library.
FetchContent_Declare(cglm
    URL https://github.com/recp/cglm/archive/refs/tags/v0.9.7.tar.gz
    URL_HASH SHA256=cc0862eee2885d2a2b4870eb29b97c6e9b9ebc7c66e8854e1fa8f3d18d9d8475
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    SOURCE_SUBDIR wpe_header_only)
FetchContent_MakeAvailable(cglm)

add_library(wpe_dep_cglm INTERFACE)
target_include_directories(wpe_dep_cglm SYSTEM INTERFACE "${cglm_SOURCE_DIR}/include")

# SDL3 3.4.8 (zlib) — built as a static library. Used for windowing, input, threads and PNG I/O.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.8/SDL3-3.4.8.tar.gz
    URL_HASH SHA256=e9fff7467fb60f037e6708da18b25560649e4c63edc2a69bb871b960d9cbfbba
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    SYSTEM)
FetchContent_MakeAvailable(SDL3)

add_library(wpe_dep_sdl3 INTERFACE)
target_link_libraries(wpe_dep_sdl3 INTERFACE SDL3::SDL3-static)

# cgltf v1.15 (MIT) — glTF 2.0 / GLB parser. Header-only; the implementation is compiled once in
# src/assets/third_party/ with engine allocator hooks.
FetchContent_Declare(cgltf
    URL https://github.com/jkuhlmann/cgltf/archive/refs/tags/v1.15.tar.gz
    URL_HASH SHA256=84e352092e5cd6aab7f66de62ddb66beb5e6f18d412ca9d12950d7a55bfef25a
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    SOURCE_SUBDIR wpe_header_only)
FetchContent_MakeAvailable(cgltf)
add_library(wpe_dep_cgltf INTERFACE)
target_include_directories(wpe_dep_cgltf SYSTEM INTERFACE "${cgltf_SOURCE_DIR}")

# stb_image (public domain / MIT) at commit 013ac3b (2024-05-31) — JPEG and PNG decoding.
FetchContent_Declare(stb
    URL https://github.com/nothings/stb/archive/013ac3beddff3dbffafd5177e7972067cd2b5083.tar.gz
    URL_HASH SHA256=b01aa93e1a968aed55f43e072c98ee401d2f20e897aabdb1a166c7166886ed11
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    SOURCE_SUBDIR wpe_header_only)
FetchContent_MakeAvailable(stb)
add_library(wpe_dep_stb INTERFACE)
target_include_directories(wpe_dep_stb SYSTEM INTERFACE "${stb_SOURCE_DIR}")

# Luau 0.742 (MIT) — scripting VM and compiler (C++17). Only Luau.VM and Luau.Compiler are built
# (EXCLUDE_FROM_ALL keeps analysis/CLI targets out of the build).
set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_WEB OFF CACHE BOOL "" FORCE)
set(LUAU_EXTERN_C OFF CACHE BOOL "" FORCE) # Its public definitions are C++-only; see below.
FetchContent_Declare(luau
    URL https://github.com/luau-lang/luau/archive/refs/tags/0.742.tar.gz
    URL_HASH SHA256=c065ac4e0a7f147a5db0429163d8d4d61cd18281a83195185823d9810f0e8284
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    EXCLUDE_FROM_ALL
    SYSTEM)
FetchContent_MakeAvailable(luau)
# Errors must unwind with longjmp, not C++ exceptions: engine API functions called from scripts
# are C code, and exceptions cannot safely propagate through C frames.
target_compile_definitions(Luau.VM PUBLIC LUA_USE_LONGJMP=1)
# Give the API C linkage when compiling Luau itself only. C consumers see the default `extern`
# declarations from luaconf.h/luacode.h, which match these C-linkage definitions.
target_compile_definitions(Luau.VM PRIVATE "LUA_API=extern \"C\"") # LUALIB_API derives from LUA_API
target_compile_definitions(Luau.Compiler PRIVATE "LUACODE_API=extern \"C\"")
if(MINGW)
    # Luau marks internals with ELF symbol visibility, which PE/COFF targets ignore; silence the noise.
    foreach(luau_target Luau.VM Luau.Compiler Luau.Ast Luau.Common)
        if(TARGET ${luau_target})
            target_compile_options(${luau_target} PRIVATE -Wno-attributes)
        endif()
    endforeach()
endif()

add_library(wpe_dep_luau INTERFACE)
target_link_libraries(wpe_dep_luau INTERFACE Luau.VM Luau.Compiler)

# Box3D v0.1.0 (MIT) — rigid body physics (C17). Samples/tests/benchmarks are disabled.
set(BOX3D_SAMPLES OFF CACHE BOOL "" FORCE)
set(BOX3D_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BOX3D_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(BOX3D_DOCS OFF CACHE BOOL "" FORCE)
set(BOX3D_VALIDATE OFF CACHE BOOL "" FORCE)
set(BOX3D_PROFILE OFF CACHE BOOL "" FORCE)
set(BOX3D_COMPILE_WARNING_AS_ERROR OFF CACHE BOOL "" FORCE)
FetchContent_Declare(box3d
    URL https://github.com/erincatto/box3d/archive/refs/tags/v0.1.0.tar.gz
    URL_HASH SHA256=df232c7618c0d0d3927b798044559ee56eabadeb9d8ff9dc526d4b384d7b415d
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    EXCLUDE_FROM_ALL
    SYSTEM)
FetchContent_MakeAvailable(box3d)

add_library(wpe_dep_box3d INTERFACE)
target_link_libraries(wpe_dep_box3d INTERFACE box3d::box3d)

# zstd v1.5.7 (BSD) — .pak archive compression. Only the static library is built (no programs,
# legacy formats or threading).
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zstd
    URL https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
    URL_HASH SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    SOURCE_SUBDIR build/cmake
    EXCLUDE_FROM_ALL
    SYSTEM)
FetchContent_MakeAvailable(zstd)
add_library(wpe_dep_zstd INTERFACE)
target_link_libraries(wpe_dep_zstd INTERFACE libzstd_static)
target_include_directories(wpe_dep_zstd SYSTEM INTERFACE "${zstd_SOURCE_DIR}/lib")

# miniaudio 0.11.25 (public domain / MIT-0) — audio playback and decoding (WAV, FLAC, MP3). Only
# the single-file library is used (its CMake project is not built); src/audio compiles miniaudio.c.
FetchContent_Declare(miniaudio
    URL https://github.com/mackron/miniaudio/archive/refs/tags/0.11.25.tar.gz
    URL_HASH SHA256=b900edcffe979816e2560a0580b9b1216d674b4f17fbadeca8f777a7f8ab0274
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    SOURCE_SUBDIR wpe_header_only)
FetchContent_MakeAvailable(miniaudio)
add_library(wpe_dep_miniaudio INTERFACE)
target_include_directories(wpe_dep_miniaudio SYSTEM INTERFACE "${miniaudio_SOURCE_DIR}")
# These change miniaudio's struct layouts, so every user must see the same set. The engine mixes
# sounds from its own data sources, so the resource manager (and its job threads) is not needed.
target_compile_definitions(wpe_dep_miniaudio INTERFACE
    MA_NO_RESOURCE_MANAGER MA_NO_ENCODING MA_NO_GENERATION)
set(WPE_MINIAUDIO_SOURCE "${miniaudio_SOURCE_DIR}/miniaudio.c")

# Dear ImGui v1.92.9b, docking branch (MIT), with its C binding cimgui 1.92.9bdock (MIT): the
# editor's user interface. cimgui's archive leaves its imgui/ submodule empty, so Dear ImGui comes
# from its own archive at the matching tag; both are copied into one generated tree with the layout
# cimgui.cpp expects. Compiled without the engine's warnings, like the other third-party code.
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b-docking.tar.gz
    URL_HASH SHA256=90ded916bd57db2e0e171b6b098940a47c6f5042725dcdc67fb19940ca8bfdcc
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    SOURCE_SUBDIR wpe_header_only)
FetchContent_Declare(cimgui
    URL https://github.com/cimgui/cimgui/archive/refs/tags/1.92.9bdock.tar.gz
    URL_HASH SHA256=7478134b33ca858140858bdf95e37ab8f0dfdbbe92a149523094f2c723aeb7c7
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    SOURCE_SUBDIR wpe_header_only)
FetchContent_MakeAvailable(imgui cimgui)
set(WPE_CIMGUI_DIR "${CMAKE_BINARY_DIR}/cimgui")
file(COPY "${cimgui_SOURCE_DIR}/cimgui.cpp" "${cimgui_SOURCE_DIR}/cimgui.h" "${cimgui_SOURCE_DIR}/cimconfig.h"
     DESTINATION "${WPE_CIMGUI_DIR}")
file(GLOB WPE_IMGUI_FILES "${imgui_SOURCE_DIR}/*.h" "${imgui_SOURCE_DIR}/*.cpp")
file(COPY ${WPE_IMGUI_FILES} DESTINATION "${WPE_CIMGUI_DIR}/imgui")
add_library(wpe_dep_cimgui STATIC
    "${WPE_CIMGUI_DIR}/cimgui.cpp"
    "${WPE_CIMGUI_DIR}/imgui/imgui.cpp"
    "${WPE_CIMGUI_DIR}/imgui/imgui_demo.cpp" # cimgui binds the demo and style-editor windows too
    "${WPE_CIMGUI_DIR}/imgui/imgui_draw.cpp"
    "${WPE_CIMGUI_DIR}/imgui/imgui_tables.cpp"
    "${WPE_CIMGUI_DIR}/imgui/imgui_widgets.cpp")
target_include_directories(wpe_dep_cimgui SYSTEM PUBLIC "${WPE_CIMGUI_DIR}")
# C users need the struct and enum definitions from cimgui.h.
target_compile_definitions(wpe_dep_cimgui INTERFACE CIMGUI_DEFINE_ENUMS_AND_STRUCTS)
set_target_properties(wpe_dep_cimgui PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
    target_compile_options(wpe_dep_cimgui PRIVATE -w)
endif()
