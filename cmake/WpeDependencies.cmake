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
