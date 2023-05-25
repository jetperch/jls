# Copyright 2014-2022 Jetperch LLC
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

cmake_minimum_required (VERSION 3.1)
set(PARENT_PROJECT_DIR ${PROJECT_SOURCE_DIR})
project(JLS
        VERSION 0.6.4
        LANGUAGES C)
SET(PROJECT_PREFIX JLS)
SET(VERSION_STRING "${PROJECT_VERSION}")
SET(VERSION_UNDERSCORE "${PROJECT_VERSION_MAJOR}_${PROJECT_VERSION_MINOR}_${PROJECT_VERSION_PATCH}")

if (NOT PARENT_PROJECT_DIR)
    set(JLS_TOPLEVEL 1)
else()
    set(JLS_TOPLEVEL 0)
endif()

option(JLS_DOCS "Use Doxygen to create the HTML based Host API documentation" OFF)
option(JLS_UNIT_TEST "Build the JLS unit tests" ON)
option(JLS_EXAMPLES "Build the JLS examples" ON)
option(BUILD_SHARED_LIBS "Build using shared libraries" OFF)

function (SET_FILENAME _filename)
    get_filename_component(b ${_filename} NAME)
    set_source_files_properties(${_filename} PROPERTIES
        COMPILE_DEFINITIONS "__FILENAME__=\"${b}\"")
endfunction (SET_FILENAME)

message(STATUS "CMAKE_SYSTEM_PROCESSOR = ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "CMAKE_C_COMPILER_ID = ${CMAKE_C_COMPILER_ID}")

if (CMAKE_SYSTEM_PROCESSOR STREQUAL "armv7l")
    message(STATUS "platform=ARMv7")
    # no extra defines, using software crc
elseif ((CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64") OR (CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64"))
    message(STATUS "platform=arm64")
    add_definitions(-march=armv8-a+crc+simd)
elseif (CMAKE_C_COMPILER_ID STREQUAL "GNU")
    message(STATUS "complier=GNU")
    add_definitions(-msse4.2)
elseif (CMAKE_C_COMPILER_ID MATCHES "^(Apple)?Clang$")
    message(STATUS "complier=Clang")
    add_definitions(-msse4.2)
elseif (CMAKE_C_COMPILER_ID STREQUAL "Intel")
    message(STATUS "complier=Intel C++")
elseif (CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    message(STATUS "complier=Visual Studio C++")
else()
    message(STATUS "complier=Not identified : ${CMAKE_C_COMPILER_ID}")
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
    FIND_PACKAGE(Doxygen)
    if (NOT DOXYGEN_FOUND)
        message(FATAL_ERROR
                "Doxygen is needed to build the documentation. Please install it")
    endif()
    #-- Configure the Template Doxyfile for our specific project
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/doc/Doxyfile.in
                   ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile  @ONLY IMMEDIATE)
    #-- Add a custom target to run Doxygen when ever the project is built
    add_custom_target(docs ALL
                      COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
                      SOURCES ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
                      DEPENDS jls)
    install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/doc/html/ DESTINATION doc)
endif()
