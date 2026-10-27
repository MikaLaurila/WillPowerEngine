# Exports the sample project with wpe_export (which finds wpe_runtime next to itself), then runs the
# exported game headless: from the start scene, from the master test scene, and with a screenshot.
# A bare runtime without a game must refuse to start.
#
# cmake -DWPE_EXPORT=<tool> -DWPE_RUNTIME=<runtime> -DPROJECT=<directory> -DWORK=<scratch directory>
#       -DSUFFIX=<executable suffix> -P export_roundtrip.cmake

foreach(var WPE_EXPORT WPE_RUNTIME PROJECT WORK)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "${var} is not set")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

function(run)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "command failed (${code}): ${ARGN}\n${out}\n${err}")
    endif()
endfunction()

set(game "${WORK}/demo_game${SUFFIX}")
run("${WPE_EXPORT}" "${PROJECT}" "${game}")
if(NOT EXISTS "${game}")
    message(FATAL_ERROR "the exported game was not written")
endif()

# The game plays from its embedded archive, without the project directory.
run("${game}" --headless --frames 60 --fixed-dt 0.0333333 --screenshot "${WORK}/demo.png")
run("${game}" --headless --scene scenes/test_full.scene --frames 30 --fixed-dt 0.0333333)
if(NOT EXISTS "${WORK}/demo.png")
    message(FATAL_ERROR "the exported game did not save its screenshot")
endif()

# Exporting an exported game is refused, and a runtime without a game does not start.
execute_process(COMMAND "${WPE_EXPORT}" "${PROJECT}" "${WORK}/again${SUFFIX}" --runtime "${game}"
                RESULT_VARIABLE code OUTPUT_QUIET ERROR_QUIET)
if(code EQUAL 0)
    message(FATAL_ERROR "exporting with an exported game as the runtime was accepted")
endif()
execute_process(COMMAND "${WPE_RUNTIME}" --headless --frames 1 RESULT_VARIABLE code OUTPUT_QUIET ERROR_QUIET)
if(code EQUAL 0)
    message(FATAL_ERROR "a runtime without a game started")
endif()

file(SIZE "${game}" size)
message(STATUS "export round trip OK (${size} bytes)")
