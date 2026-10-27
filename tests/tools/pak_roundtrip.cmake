# Packs a directory with wpe_pak, verifies and lists the archive, extracts it again and checks
# that every extracted file matches its source byte for byte. Packing twice must produce
# identical archives (sorted input order, deterministic compression).
#
# cmake -DWPE_PAK=<tool> -DSOURCE=<directory> -DWORK=<scratch directory> -P pak_roundtrip.cmake

foreach(var WPE_PAK SOURCE WORK)
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

run("${WPE_PAK}" create "${WORK}/a.pak" "${SOURCE}")
run("${WPE_PAK}" create "${WORK}/b.pak" "${SOURCE}" --level 9)
run("${WPE_PAK}" verify "${WORK}/a.pak")
run("${WPE_PAK}" list "${WORK}/a.pak")
run("${WPE_PAK}" extract "${WORK}/a.pak" "${WORK}/out")

file(SHA256 "${WORK}/a.pak" hash_a)
file(SHA256 "${WORK}/b.pak" hash_b)
if(NOT hash_a STREQUAL hash_b)
    message(FATAL_ERROR "packing the same directory twice gave different archives")
endif()

file(GLOB_RECURSE sources RELATIVE "${SOURCE}" "${SOURCE}/*")
list(LENGTH sources count)
if(count EQUAL 0)
    message(FATAL_ERROR "no source files found in ${SOURCE}")
endif()
foreach(file IN LISTS sources)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${SOURCE}/${file}" "${WORK}/out/${file}"
                    RESULT_VARIABLE different)
    if(different)
        message(FATAL_ERROR "extracted file differs: ${file}")
    endif()
endforeach()

# Corruption detection is covered by the vfs unit tests.
message(STATUS "pak round trip OK (${count} files)")
