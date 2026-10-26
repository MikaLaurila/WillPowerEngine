# `format` and `lint` targets. Both are optional tools: when missing, the target still exists
# and fails with an explanatory message instead of silently succeeding.

file(GLOB_RECURSE WPE_FORMAT_SOURCES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/src/*.c"
    "${PROJECT_SOURCE_DIR}/src/*.h"
    "${PROJECT_SOURCE_DIR}/tests/*.c"
    "${PROJECT_SOURCE_DIR}/tests/*.h")

find_program(WPE_CLANG_FORMAT NAMES clang-format)
if(WPE_CLANG_FORMAT)
    add_custom_target(format
        COMMAND ${WPE_CLANG_FORMAT} -i --style=file ${WPE_FORMAT_SOURCES}
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        COMMENT "Formatting sources with clang-format")
    add_custom_target(format_check
        COMMAND ${WPE_CLANG_FORMAT} --dry-run --Werror --style=file ${WPE_FORMAT_SOURCES}
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        COMMENT "Checking formatting with clang-format")
else()
    add_custom_target(format
        COMMAND ${CMAKE_COMMAND} -E echo "clang-format not found; install it to use the format target"
        COMMAND ${CMAKE_COMMAND} -E false)
endif()

find_program(WPE_CLANG_TIDY NAMES clang-tidy)
if(WPE_CLANG_TIDY)
    file(GLOB_RECURSE WPE_LINT_SOURCES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/*.c")
    add_custom_target(lint
        COMMAND ${WPE_CLANG_TIDY} -p ${PROJECT_BINARY_DIR} --quiet ${WPE_LINT_SOURCES}
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        COMMENT "Running clang-tidy")
else()
    add_custom_target(lint
        COMMAND ${CMAKE_COMMAND} -E echo "clang-tidy not found; install it to use the lint target"
        COMMAND ${CMAKE_COMMAND} -E false)
endif()
