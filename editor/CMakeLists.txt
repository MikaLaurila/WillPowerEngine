# The editor: a library with everything but main() (so tests can drive it) and the executable.
# Unlike engine modules it may use SDL directly (OS events to the UI) and cimgui.
add_library(wpe_editor_core STATIC
    code_text.c
    editor.c
    editor_commands.c
    editor_ui.c
    script_panel.c
    ui_backend.c)
target_compile_definitions(wpe_editor_core PRIVATE WPE_EXE_SUFFIX="${CMAKE_EXECUTABLE_SUFFIX}")
target_include_directories(wpe_editor_core PUBLIC "${PROJECT_SOURCE_DIR}")
target_link_libraries(wpe_editor_core
    PUBLIC wpe::engine
    PRIVATE wpe_dep_cimgui wpe_dep_sdl3)
wpe_configure_target(wpe_editor_core)

add_executable(wpe_editor main.c)
target_link_libraries(wpe_editor PRIVATE wpe_editor_core)
wpe_configure_target(wpe_editor)
# A console program on every platform: agents and scripts run it from terminals, and shells (notably
# PowerShell) only wait for and capture the output of console programs.
set_target_properties(wpe_editor PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
