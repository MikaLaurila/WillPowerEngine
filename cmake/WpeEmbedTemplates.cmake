# Writes a C header embedding every file of the project templates (templates/<name>/...), run at build
# time by src/engine/CMakeLists.txt: cmake -DTEMPLATES_DIR=<dir> -DOUTPUT=<header> -P WpeEmbedTemplates.cmake
#
# The header defines one byte array per file (with a trailing 0, so empty files are valid C) and the
# table wpe__template_files[] of {template, path, bytes, size}, sorted by template, then path. Files and
# directories whose names start with '.' are skipped.

if(NOT TEMPLATES_DIR OR NOT OUTPUT)
    message(FATAL_ERROR "WpeEmbedTemplates.cmake needs TEMPLATES_DIR and OUTPUT")
endif()

file(GLOB_RECURSE wpe_files LIST_DIRECTORIES false RELATIVE "${TEMPLATES_DIR}" "${TEMPLATES_DIR}/*")
list(SORT wpe_files)

set(wpe_arrays "")
set(wpe_table "")
set(wpe_index 0)
foreach(wpe_file IN LISTS wpe_files)
    if(wpe_file MATCHES "(^|/)\\.")
        continue()
    endif()
    if(NOT wpe_file MATCHES "^([A-Za-z0-9_-]+)/([A-Za-z0-9_./-]+)$")
        message(FATAL_ERROR "template file '${wpe_file}': names may use letters, digits, '_', '-' and '.'")
    endif()
    set(wpe_template "${CMAKE_MATCH_1}")
    set(wpe_path "${CMAKE_MATCH_2}")
    file(READ "${TEMPLATES_DIR}/${wpe_file}" wpe_hex HEX)
    string(LENGTH "${wpe_hex}" wpe_hex_length)
    math(EXPR wpe_size "${wpe_hex_length} / 2")
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," wpe_bytes "${wpe_hex}")
    string(APPEND wpe_arrays "static const unsigned char wpe__template_${wpe_index}[] = {${wpe_bytes}0};\n")
    string(APPEND wpe_table
           "    {\"${wpe_template}\", \"${wpe_path}\", wpe__template_${wpe_index}, ${wpe_size}u},\n")
    math(EXPR wpe_index "${wpe_index} + 1")
endforeach()
if(wpe_index EQUAL 0)
    message(FATAL_ERROR "no template files in '${TEMPLATES_DIR}'")
endif()

file(CONFIGURE OUTPUT "${OUTPUT}" CONTENT "/* Generated from templates/ by cmake/WpeEmbedTemplates.cmake. Do not edit. */
@wpe_arrays@
static const WPE__TemplateFile wpe__template_files[] = {
@wpe_table@};
" @ONLY)
