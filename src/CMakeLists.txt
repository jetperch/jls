# Copyright 2014-2017 Jetperch LLC
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

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/version.h.in
        ${PROJECT_SOURCE_DIR}/include/jls/version.h)

set(SOURCES
        crc32c_intel_sse4.c
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

foreach(f IN LISTS SOURCES)
    SET_FILENAME("${f}")
endforeach()

add_library(jls_objlib OBJECT ${SOURCES})
add_library(jls STATIC $<TARGET_OBJECTS:jls_objlib>)
add_dependencies(jls jls_objlib)
