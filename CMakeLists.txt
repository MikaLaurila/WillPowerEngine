cmake_minimum_required(VERSION 3.28)

project(WillPowerEngine
    VERSION 0.1.0
    DESCRIPTION "Multithreaded software rasterizer 3D game engine"
    LANGUAGES C CXX)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not supported. Use: cmake -S . -B build -G Ninja")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(WPE_BUILD_TESTS "Build the WillPowerEngine test suites" ON)
option(SANITIZERS "Build with address and undefined-behavior sanitizers (GCC/Clang, non-MinGW)" OFF)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(WpeCompilerOptions)
include(WpeTooling)
include(WpeDependencies)

add_subdirectory(src)
add_subdirectory(runtime)
add_subdirectory(editor)
add_subdirectory(tools)

option(WPE_BUILD_SAMPLES "Build the sample applications" ON)
if(WPE_BUILD_SAMPLES)
    add_subdirectory(samples)
endif()

if(WPE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
