# Compiler settings applied to every first-party target via wpe_configure_target().
# Third-party dependencies are deliberately excluded so their warnings never break our build.

add_library(wpe_compile_options INTERFACE)

if(MSVC)
    target_compile_options(wpe_compile_options INTERFACE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(wpe_compile_options INTERFACE
        -Wall
        -Wextra
        -Werror
        -Wpedantic
        -Wshadow
        -Wvla
        -Wundef
        -Wcast-align
        -Wformat=2
        -Wnull-dereference
        $<$<COMPILE_LANGUAGE:C>:-Wstrict-prototypes>
        $<$<COMPILE_LANGUAGE:C>:-Wmissing-prototypes>)
endif()

target_compile_definitions(wpe_compile_options INTERFACE
    $<$<CONFIG:Debug>:WPE_DEBUG=1>
    $<$<CONFIG:RelWithDebInfo>:WPE_DEBUG=1>)

if(SANITIZERS)
    if(MSVC OR MINGW)
        message(WARNING "SANITIZERS=ON is not supported by this toolchain; ignoring. "
                        "Leak detection still runs through the wpe_alloc tracker in every test.")
    else()
        target_compile_options(wpe_compile_options INTERFACE
            -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
        target_link_options(wpe_compile_options INTERFACE -fsanitize=address,undefined)
    endif()
endif()

# Applies the project-wide compile options to a first-party target.
function(wpe_configure_target target)
    target_link_libraries(${target} PRIVATE wpe_compile_options)
endfunction()
