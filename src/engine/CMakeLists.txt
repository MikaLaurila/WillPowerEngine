# The project templates (templates/<name>/...) as byte arrays for wpe_project_create. Regenerated when a
# template file changes; adding or removing one reconfigures through CONFIGURE_DEPENDS.
file(GLOB_RECURSE WPE_TEMPLATE_FILES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/templates/*")
set(WPE_TEMPLATES_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/engine/templates_data.h")
add_custom_command(
    OUTPUT "${WPE_TEMPLATES_HEADER}"
    COMMAND "${CMAKE_COMMAND}" "-DTEMPLATES_DIR=${PROJECT_SOURCE_DIR}/templates" "-DOUTPUT=${WPE_TEMPLATES_HEADER}"
            -P "${PROJECT_SOURCE_DIR}/cmake/WpeEmbedTemplates.cmake"
    DEPENDS ${WPE_TEMPLATE_FILES} "${PROJECT_SOURCE_DIR}/cmake/WpeEmbedTemplates.cmake"
    COMMENT "Embedding the project templates"
    VERBATIM)

add_library(wpe_engine STATIC
    engine.c
    export.c
    json_read.c
    new_project.c
    prefab.c
    project.c
    scene_file.c
    scene_file_ui.c
    "${WPE_TEMPLATES_HEADER}")
add_library(wpe::engine ALIAS wpe_engine)

target_link_libraries(wpe_engine
    PUBLIC wpe::core wpe::math wpe::ecs wpe::scene wpe::jobs wpe::renderer wpe::platform wpe::input wpe::vfs
           wpe::assets wpe::graphics wpe::physics wpe::audio wpe::ui wpe::script)
target_include_directories(wpe_engine PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
wpe_configure_target(wpe_engine)
