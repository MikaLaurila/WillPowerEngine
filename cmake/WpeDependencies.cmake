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
