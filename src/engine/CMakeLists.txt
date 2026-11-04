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

# The `willpowerengine` Claude skill with the docs it points to, as new projects get it (project_skill.c).
set(WPE_SKILL_DOCS assets.md editor.md navigation.md networking.md runtime.md scenes.md scripting.md)
set(WPE_SKILL_SOURCE "${PROJECT_SOURCE_DIR}/.claude/skills/willpowerengine/SKILL.md")
list(TRANSFORM WPE_SKILL_DOCS PREPEND "${PROJECT_SOURCE_DIR}/docs/" OUTPUT_VARIABLE WPE_SKILL_DOC_PATHS)
set(WPE_SKILL_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/engine/skill_data.h")
string(REPLACE ";" "," WPE_SKILL_DOC_LIST "${WPE_SKILL_DOCS}") # Commas: a ';' wouldn't survive the command line.
add_custom_command(
    OUTPUT "${WPE_SKILL_HEADER}"
    COMMAND "${CMAKE_COMMAND}" "-DSKILL=${WPE_SKILL_SOURCE}" "-DDOCS_DIR=${PROJECT_SOURCE_DIR}/docs"
            "-DDOCS=${WPE_SKILL_DOC_LIST}" "-DVERSION=${PROJECT_VERSION}" "-DOUTPUT=${WPE_SKILL_HEADER}"
            -P "${PROJECT_SOURCE_DIR}/cmake/WpeEmbedSkill.cmake"
    DEPENDS "${WPE_SKILL_SOURCE}" ${WPE_SKILL_DOC_PATHS} "${PROJECT_SOURCE_DIR}/cmake/WpeEmbedSkill.cmake"
    COMMENT "Embedding the willpowerengine skill"
    VERBATIM)

add_library(wpe_engine STATIC
    engine.c
    export.c
    json_read.c
    new_project.c
    prefab.c
    profiler.c
    project.c
    project_skill.c
    replication.c
    scene_file.c
    scene_file_ui.c
    "${WPE_TEMPLATES_HEADER}"
    "${WPE_SKILL_HEADER}")
add_library(wpe::engine ALIAS wpe_engine)

target_link_libraries(wpe_engine
    PUBLIC wpe::core wpe::math wpe::ecs wpe::scene wpe::jobs wpe::renderer wpe::platform wpe::input wpe::vfs
           wpe::assets wpe::graphics wpe::physics wpe::navigation wpe::audio wpe::ui wpe::script wpe::network)
target_include_directories(wpe_engine PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
wpe_configure_target(wpe_engine)
