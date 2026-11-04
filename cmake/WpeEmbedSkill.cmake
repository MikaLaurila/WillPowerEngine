# Writes a C header embedding the `willpowerengine` Claude skill as new projects get it (wpe_project_create,
# `wpe_editor <dir> --init`, `--update-skill`), run at build time by src/engine/CMakeLists.txt:
#   cmake -DSKILL=<SKILL.md> -DDOCS_DIR=<docs> -DDOCS=<a.md,b.md,c.md> -DVERSION=<x.y.z> -DOUTPUT=<header>
#         -P WpeEmbedSkill.cmake
#
# The project's copy: SKILL.md with its `docs/` paths pointing at `references/` (the docs travel with it),
# and `engine_version` and `skill_id` (a hash of the engine version, the skill and the docs) added to its
# front matter's metadata; then references/<doc>.md for each doc. The header defines the files as byte
# arrays (with a trailing 0) in wpe__skill_files[] = {path, bytes, size}, and WPE__SKILL_ID.

foreach(wpe_var SKILL DOCS_DIR DOCS VERSION OUTPUT)
    if(NOT ${wpe_var})
        message(FATAL_ERROR "WpeEmbedSkill.cmake needs ${wpe_var}")
    endif()
endforeach()
string(REPLACE "," ";" DOCS "${DOCS}")

# Line endings are LF whatever the checkout has (a Windows checkout may have CRLF), so the copy and its id are the
# same on every machine.
# The id: changes whenever the engine version, the skill or any of the docs does.
file(READ "${SKILL}" wpe_skill)
string(REPLACE "\r" "" wpe_skill "${wpe_skill}")
set(wpe_everything "${VERSION}\n${wpe_skill}")
foreach(wpe_doc IN LISTS DOCS)
    file(READ "${DOCS_DIR}/${wpe_doc}" wpe_text)
    string(REPLACE "\r" "" wpe_text "${wpe_text}")
    string(APPEND wpe_everything "\n${wpe_doc}\n${wpe_text}")
endforeach()
string(SHA1 wpe_hash "${wpe_everything}")
string(SUBSTRING "${wpe_hash}" 0 16 wpe_id)

# SKILL.md as installed: references/ instead of docs/, and the stamp in its metadata.
string(REPLACE "docs/" "references/" wpe_skill "${wpe_skill}")
string(FIND "${wpe_skill}" "\nmetadata:\n" wpe_at)
if(wpe_at EQUAL -1)
    message(FATAL_ERROR "${SKILL}: its front matter needs a `metadata:` block for the version stamp")
endif()
string(REPLACE "\nmetadata:\n" "\nmetadata:\n  engine_version: \"${VERSION}\"\n  skill_id: \"${wpe_id}\"\n" wpe_skill
       "${wpe_skill}")

set(wpe_arrays "")
set(wpe_table "")
set(wpe_index 0)
macro(wpe_embed path text_var)
    file(WRITE "${OUTPUT}.part" "${${text_var}}")
    file(READ "${OUTPUT}.part" wpe_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," wpe_bytes "${wpe_hex}")
    # LF only (file(WRITE) writes CRLF on Windows, and a checkout may too); whole bytes, so nothing misaligns.
    string(REPLACE "0x0d,0x0a," "0x0a," wpe_bytes "${wpe_bytes}")
    string(REGEX MATCHALL "0x" wpe_count "${wpe_bytes}")
    list(LENGTH wpe_count wpe_size)
    string(APPEND wpe_arrays "static const unsigned char wpe__skill_${wpe_index}[] = {${wpe_bytes}0};\n")
    string(APPEND wpe_table "    {\"${path}\", wpe__skill_${wpe_index}, ${wpe_size}u},\n")
    math(EXPR wpe_index "${wpe_index} + 1")
endmacro()

wpe_embed("SKILL.md" wpe_skill)
foreach(wpe_doc IN LISTS DOCS)
    file(READ "${DOCS_DIR}/${wpe_doc}" wpe_text)
    wpe_embed("references/${wpe_doc}" wpe_text)
endforeach()
file(REMOVE "${OUTPUT}.part")

file(CONFIGURE OUTPUT "${OUTPUT}" CONTENT "/* Generated from the willpowerengine skill and docs/ by cmake/WpeEmbedSkill.cmake. Do not edit. */
#define WPE__SKILL_ID \"@wpe_id@\"
@wpe_arrays@
static const WPE__SkillFile wpe__skill_files[] = {
@wpe_table@};
" @ONLY)
