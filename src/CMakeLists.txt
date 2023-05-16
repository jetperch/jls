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

include_directories(../include_prv)

set(SOURCES
        bit_shift.c
        datatype.c
        ec.c
        log.c
        msg_ring_buffer.c
        raw.c
        rd_fsr.c
        reader.c
        statistics.c
        threaded_writer.c
        wr_fsr.c
        wr_ts.c
        writer.c
)

if (CMAKE_SYSTEM_PROCESSOR STREQUAL "armv7l")
    set(SOURCES ${SOURCES} backend_posix.c crc32c_sw.c)
    set(JLS_LIBS jls m pthread PARENT_SCOPE)
elseif ((CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64") OR (CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64"))
    set(SOURCES ${SOURCES} backend_posix.c crc32c_arm_neon.c)
    set(JLS_LIBS jls m pthread PARENT_SCOPE)
elseif (WIN32)
    set(SOURCES ${SOURCES} backend_win.c crc32c_intel_sse4.c)
    set(JLS_LIBS jls PARENT_SCOPE)
    if (BUILD_SHARED_LIBS)
        add_definitions(-DJLS_EXPORT=1)
    endif()
elseif (APPLE)
    set(SOURCES ${SOURCES} backend_posix.c crc32c_intel_sse4.c)
    set(JLS_LIBS jls m pthread PARENT_SCOPE)
elseif (UNIX)
    set(SOURCES ${SOURCES} backend_posix.c crc32c_intel_sse4.c)
    set(JLS_LIBS jls m pthread PARENT_SCOPE)
else()
    message(FATAL_ERROR "Unsupported platform")
endif()

foreach(f IN LISTS SOURCES)
    SET_FILENAME("${f}")
endforeach()


add_library(jls_objlib OBJECT ${SOURCES})
add_library(jls $<TARGET_OBJECTS:jls_objlib>)
add_dependencies(jls jls_objlib)
