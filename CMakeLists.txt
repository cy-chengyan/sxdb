# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.20)

project(sxdb VERSION 0.1.0 LANGUAGES C)

# C17 by default. CI also builds with -DCMAKE_C_STANDARD=23 to stay forward compatible.
if(NOT DEFINED CMAKE_C_STANDARD)
    set(CMAKE_C_STANDARD 17)
endif()
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(SXDB_BUILD_TESTS "Build the unit tests" ON)
option(SXDB_BUILD_TOOLS "Build the command-line tools" ON)
option(SXDB_WERROR "Treat compiler warnings as errors" OFF)
set(SXDB_SANITIZE "" CACHE STRING
    "Comma-separated sanitizers to enable, for example address,undefined or thread")

include(cmake/CompilerFlags.cmake)

add_subdirectory(src)

if(SXDB_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(SXDB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

add_custom_target(check-source
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/check-source.sh ${CMAKE_SOURCE_DIR}
    COMMENT "Checking source tree rules"
    VERBATIM)
