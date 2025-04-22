# Copyright 2014-2025 Jetperch LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required (VERSION 3.10)
set(PARENT_PROJECT_DIR ${PROJECT_SOURCE_DIR})
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)  # universal2
project(JLS
        VERSION 0.13.3
        LANGUAGES C)
SET(PROJECT_PREFIX JLS)
SET(VERSION_STRING "${PROJECT_VERSION}")
SET(VERSION_UNDERSCORE "${PROJECT_VERSION_MAJOR}_${PROJECT_VERSION_MINOR}_${PROJECT_VERSION_PATCH}")

set(CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})

if (NOT PARENT_PROJECT_DIR)
    set(JLS_TOPLEVEL 1)
else()
    set(JLS_TOPLEVEL 0)
endif()

option(JLS_DOCS "Use Doxygen to create the HTML based Host API documentation" OFF)
option(JLS_UNIT_TEST "Build the JLS unit tests" ON)
option(JLS_EXAMPLES "Build the JLS examples" ON)
option(BUILD_SHARED_LIBS "Build using shared libraries" OFF)
option(JLS_OPTIMIZE_CRC "Perform CRC optimizations" ON)

function (SET_FILENAME _filename)
    get_filename_component(b ${_filename} NAME)
    set_source_files_properties(${_filename} PROPERTIES
        COMPILE_DEFINITIONS "__FILENAME__=\"${b}\"")
endfunction (SET_FILENAME)

message(STATUS "CMAKE_SYSTEM_PROCESSOR = ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "CMAKE_C_COMPILER_ID = ${CMAKE_C_COMPILER_ID}")

if (NOT JLS_OPTIMIZE_CRC)
    message(STATUS "skip crc optimizations")
    add_definitions(-DJLS_OPTIMIZE_CRC_DISABLE=1)
elseif (WIN32)
    message(STATUS "windows")
    # msse4.2 always enabled
elseif (APPLE)
    message(STATUS "macOS universal2")
    add_definitions(-msse4.2 -mcrc)
elseif (UNIX)
    include(CheckCCompilerFlag)
    check_c_compiler_flag("-msse4.2" FEATURE_MSSE_SUPPORTED)
    check_c_compiler_flag("-mcrc" FEATURE_CRC_SUPPORTED)
    if (FEATURE_MSSE_SUPPORTED)
        add_definitions(-msse4.2)
    elseif (FEATURE_CRC_SUPPORTED)
        add_definitions(-mcrc)
    else()
        add_definitions(-DJLS_OPTIMIZE_CRC_DISABLE=1)
    endif()
endif()

if (JLS_TOPLEVEL)
    if (CMAKE_C_COMPILER_ID STREQUAL "GNU")
        add_definitions(-Wall -Wextra -Wpedantic -Werror -fPIC)
        if (WIN32)
            add_definitions(-Wno-pedantic-ms-format)
        endif (WIN32)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu99")
    elseif (CMAKE_C_COMPILER_ID MATCHES "^(Apple)?Clang$")
        add_definitions(-Wall -Wextra -Wpedantic -Werror -fPIC)
    elseif (CMAKE_CXX_COMPILER_ID STREQUAL "Intel") # Intel C++
    elseif (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")  # Visual Studio C++
    endif()
endif()
remove_definitions(-D__cplusplus)

set(JLS_SOURCE_PATH ${CMAKE_CURRENT_SOURCE_DIR}
    CACHE INTERNAL "JLS source path" FORCE
)
set(JLS_INCLUDE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        CACHE INTERNAL "JLS include paths" FORCE
        )
include_directories(${JLS_INCLUDE})

add_subdirectory(src)
add_subdirectory(example)

if(JLS_UNIT_TEST AND JLS_TOPLEVEL AND NOT CMAKE_CROSSCOMPILING)
    add_subdirectory(third-party)
    enable_testing()
    add_subdirectory(test)
endif()

if(JLS_DOCS AND JLS_TOPLEVEL)
    add_subdirectory(doc)
endif()
