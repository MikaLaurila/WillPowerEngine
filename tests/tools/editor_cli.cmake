# Drives wpe_editor headless as an agent would: a batch of JSON-lines commands edits a scratch
# project and saves a scene, one-shot commands read it back, the editor UI is captured to PNG, and a
# failing command makes the process fail.
#
# cmake -DWPE_EDITOR=<editor> -DWORK=<scratch directory> -P editor_cli.cmake

foreach(var WPE_EDITOR WORK)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "${var} is not set")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/project/scenes" "${WORK}/project/meshes")
file(WRITE "${WORK}/project/project.json"
     "{\"format\": \"wpe.project\", \"version\": 1, \"name\": \"CLI\", \"start_scene\": \"scenes/start.scene\"}\n")
file(WRITE "${WORK}/project/meshes/box.prim" "{\"shape\": \"box\"}\n")
file(WRITE "${WORK}/project/scenes/start.scene" "{\"format\": \"wpe.scene\", \"version\": 1}\n")
file(WRITE "${WORK}/commands.jsonl"
"{\"cmd\": \"create_entity\", \"entity\": {\"name\": \"crate\", \"mesh\": {\"model\": \"meshes/box.prim\"}, \"transform\": {\"position\": [0, 1, 0]}}}
{\"cmd\": \"create_entity\", \"entity\": {\"name\": \"sun\", \"transform\": {\"euler\": [-50, 30, 0]}, \"light\": {\"type\": \"directional\"}}}
{\"cmd\": \"set_environment\", \"value\": {\"ambient\": [0.3, 0.3, 0.35]}}
{\"cmd\": \"save_scene\", \"path\": \"scenes/made.scene\"}
{\"cmd\": \"screenshot\", \"target\": \"editor\", \"path\": \"${WORK}/editor.png\"}
{\"cmd\": \"screenshot\", \"path\": \"${WORK}/game.png\"}
")

function(run expect_success)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(expect_success AND NOT code EQUAL 0)
        message(FATAL_ERROR "command failed (${code}): ${ARGN}\n${out}\n${err}")
    elseif(NOT expect_success AND code EQUAL 0)
        message(FATAL_ERROR "command should have failed: ${ARGN}\n${out}")
    endif()
    set(output "${out}" PARENT_SCOPE)
endfunction()

run(TRUE "${WPE_EDITOR}" "${WORK}/project" --run "${WORK}/commands.jsonl")
foreach(file project/scenes/made.scene editor.png game.png)
    if(NOT EXISTS "${WORK}/${file}")
        message(FATAL_ERROR "${file} was not written")
    endif()
endforeach()
file(READ "${WORK}/project/scenes/made.scene" scene)
if(NOT scene MATCHES "\"crate\"" OR scene MATCHES "editor camera")
    message(FATAL_ERROR "the saved scene is wrong:\n${scene}")
endif()

# The saved scene opens again; listing shows both entities.
run(TRUE "${WPE_EDITOR}" "${WORK}/project" --scene scenes/made.scene --command "{\"cmd\": \"list_entities\"}")
if(NOT output MATCHES "\"crate\"" OR NOT output MATCHES "\"sun\"")
    message(FATAL_ERROR "list_entities did not show the saved entities:\n${output}")
endif()

# Errors make the process fail.
run(FALSE "${WPE_EDITOR}" "${WORK}/project" --command "{\"cmd\": \"get_entity\", \"id\": 999}")
run(FALSE "${WPE_EDITOR}" "${WORK}/project" --command "{\"cmd\": \"no_such_command\"}")
message(STATUS "editor command line OK")
