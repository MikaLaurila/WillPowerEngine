# miniaudio, compiled once without the engine's strict warnings and always optimized (it mixes on
# the audio thread, so a slow Debug build would glitch).
add_library(wpe_audio_third_party STATIC "${WPE_MINIAUDIO_SOURCE}")
target_link_libraries(wpe_audio_third_party PUBLIC wpe_dep_miniaudio)
if(NOT MSVC)
    target_compile_options(wpe_audio_third_party PRIVATE -O2 -w)
endif()
if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(wpe_audio_third_party PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

add_library(wpe_audio STATIC audio.c)
add_library(wpe::audio ALIAS wpe_audio)

target_link_libraries(wpe_audio
    PUBLIC wpe::core wpe::math wpe::ecs wpe::scene wpe::assets
    PRIVATE wpe_audio_third_party)
wpe_configure_target(wpe_audio)
