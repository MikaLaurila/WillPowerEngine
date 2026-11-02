# Multiplayer from the command line, over real UDP on this machine (127.0.0.1 only):
#   1. wpe_runtime --host (a headless dedicated server) and wpe_runtime --connect play together;
#   2. a different game is refused, and the joining runtime fails;
#   3. the editor's play_network hosts the saved scene and starts a client runtime that joins it.
#
# cmake -DWPE_RUNTIME=<runtime> -DWPE_EDITOR=<editor> -DWORK=<scratch directory> -P network_cli.cmake

foreach(var WPE_RUNTIME WPE_EDITOR WORK)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "${var} is not set")
    endif()
endforeach()

# Ports of their own, so this test can run beside the others.
set(PORT 47960)
file(REMOVE_RECURSE "${WORK}")
foreach(game a b)
    file(MAKE_DIRECTORY "${WORK}/${game}/scenes" "${WORK}/${game}/scripts")
    file(WRITE "${WORK}/${game}/project.json"
         "{\"format\": \"wpe.project\", \"version\": 1, \"name\": \"Net CLI\", \"start_scene\": \"scenes/start.scene\",\n"
         " \"display\": {\"width\": 64, \"height\": 48},\n"
         " \"network\": {\"port\": ${PORT}, \"discovery_port\": 47961}}\n")
    file(WRITE "${WORK}/${game}/scenes/start.scene" "{\"format\": \"wpe.scene\", \"version\": 1}\n")
    file(WRITE "${WORK}/${game}/scripts/rules.luau" "return {game = \"${game}\"}\n") # b is another game
endforeach()

# 1. Host and player run at once: in a pipeline, so the host's output is the one captured (the last
#    command's). The player runs ~1 s at 60 frames per second; the host ~4 s, to outlast it.
execute_process(
    COMMAND "${WPE_RUNTIME}" --project "${WORK}/a" --headless --connect 127.0.0.1:${PORT} --name Alice --local
            --net-sim latency=40,jitter=10,loss=5 --frames 60 --max-fps 60
    COMMAND "${WPE_RUNTIME}" --project "${WORK}/a" --headless --host ${PORT} --local --frames 240
    RESULTS_VARIABLE codes OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT codes STREQUAL "0;0")
    message(FATAL_ERROR "the runtimes failed (${codes}):\n${out}\n${err}")
endif()
set(log "${out}${err}") # The engine logs to stderr.
if(NOT log MATCHES "hosting on port ${PORT}" OR NOT log MATCHES "Alice joined \\(player 1\\)")
    message(FATAL_ERROR "the player did not join the dedicated server:\n${out}\n${err}")
endif()

# 2. Another game (its script differs) is refused, and the runtime that tried says so by failing.
execute_process(
    COMMAND "${WPE_RUNTIME}" --project "${WORK}/b" --headless --connect 127.0.0.1:${PORT} --local --frames 60
            --max-fps 60
    COMMAND "${WPE_RUNTIME}" --project "${WORK}/a" --headless --host ${PORT} --local --frames 240
    RESULTS_VARIABLE codes OUTPUT_VARIABLE out ERROR_VARIABLE err)
list(GET codes 0 player)
list(GET codes 1 host)
if(player EQUAL 0 OR NOT host EQUAL 0 OR NOT "${out}${err}" MATCHES "could not join the game: different game" OR "${out}${err}" MATCHES "joined \\(player")
    message(FATAL_ERROR "another game was let in (${codes}):\n${out}\n${err}")
endif()

# Bad command lines fail at once.
foreach(args "--host;--connect;127.0.0.1" "--net-sim;latency=fast" "--net-sim;loss=95" "--host;99999")
    execute_process(COMMAND "${WPE_RUNTIME}" --project "${WORK}/a" --headless --frames 1 ${args}
                    RESULT_VARIABLE code OUTPUT_QUIET ERROR_QUIET)
    if(code EQUAL 0)
        message(FATAL_ERROR "wpe_runtime accepted ${args}")
    endif()
endforeach()

# 3. The editor hosts the saved scene and starts one headless client runtime (from its own bin directory).
file(WRITE "${WORK}/commands.jsonl"
"{\"cmd\": \"play_network\", \"clients\": 1, \"headless\": true, \"latency_ms\": 20}
{\"cmd\": \"net_wait\", \"players\": 2, \"timeout\": 30}
{\"cmd\": \"stop\"}
{\"cmd\": \"net_status\"}
")
execute_process(COMMAND "${WPE_EDITOR}" "${WORK}/a" --run "${WORK}/commands.jsonl"
                RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT code EQUAL 0)
    message(FATAL_ERROR "play_network failed (${code}):\n${out}\n${err}")
endif()
if(NOT out MATCHES "\"name\": \"Player 1\"" OR NOT out MATCHES "\"session\": \"hosting\""
   OR NOT out MATCHES "\"session\": \"offline\",[^}]*\"clients_running\": 0")
    message(FATAL_ERROR "the editor's game was not joined, or did not end with stop:\n${out}\n${err}")
endif()

# An unsaved scene can't be played over the network: the others would load the file.
file(WRITE "${WORK}/unsaved.jsonl"
"{\"cmd\": \"create_entity\", \"entity\": {\"name\": \"new\"}}
{\"cmd\": \"play_network\", \"clients\": 1, \"headless\": true}
")
execute_process(COMMAND "${WPE_EDITOR}" "${WORK}/a" --run "${WORK}/unsaved.jsonl"
                RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(code EQUAL 0 OR NOT "${out}${err}" MATCHES "save the scene first")
    message(FATAL_ERROR "play_network played an unsaved scene:\n${out}\n${err}")
endif()
message(STATUS "multiplayer command line OK")
