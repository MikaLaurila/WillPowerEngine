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
