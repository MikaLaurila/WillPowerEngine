# The editor: a library with everything but main() (so tests can drive it) and the executable.
# Unlike engine modules it may use SDL directly (OS events to the UI) and cimgui.

# docs/scripting.md as a byte array (with a trailing 0) for the Script panel's API help; reconfigures when
# it changes, and file(CONFIGURE) rewrites the header only when its content does.
set(WPE_SCRIPTING_DOCS "${PROJECT_SOURCE_DIR}/docs/scripting.md")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${WPE_SCRIPTING_DOCS}")
file(READ "${WPE_SCRIPTING_DOCS}" wpe_docs_hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," wpe_docs_bytes "${wpe_docs_hex}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/editor/scripting_docs_data.h"
     CONTENT "/* Generated from docs/scripting.md by editor/CMakeLists.txt. Do not edit. */
static const unsigned char wpe__scripting_docs[] = {${wpe_docs_bytes}0};
" @ONLY)

add_library(wpe_editor_core STATIC
    api_docs.c
    code_symbols.c
    code_text.c
    editor.c
    editor_commands.c
    editor_ui.c
    script_panel.c
    ui_backend.c)
target_compile_definitions(wpe_editor_core PRIVATE WPE_EXE_SUFFIX="${CMAKE_EXECUTABLE_SUFFIX}")
target_include_directories(wpe_editor_core PUBLIC "${PROJECT_SOURCE_DIR}")
target_include_directories(wpe_editor_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
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
