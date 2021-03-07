# Copyright 2014-2021 Jetperch LLC
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
        ec.c
        log.c
        msg_ring_buffer.c
        raw.c
        reader.c
        statistics.c
        threaded_writer.c
        wf_f32.c
        writer.c
)

if (WIN32)
    set(SOURCES ${SOURCES} backend_win.c crc32c_intel_sse4.c)
    set(JLS_LIBS jls PARENT_SCOPE)
elseif (APPLE)
    # todo handle arm
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
add_library(jls STATIC $<TARGET_OBJECTS:jls_objlib>)
add_dependencies(jls jls_objlib)
