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


set(objects
        $<TARGET_OBJECTS:jls_objlib>)

set(dependencies
        jls_objlib
        cmocka)
include_directories(${CMOCKA_INCLUDE})

function (ADD_CMOCKA_TEST _testName)
    SET_FILENAME("${_testName}.c")
    add_executable(${_testName} "${_testName}.c" ${objects})
    add_dependencies(${_testName} ${dependencies})
    target_link_libraries(${_testName} ${JLS_LIBS} cmocka)
    add_test(${_testName} ${CMAKE_CURRENT_BINARY_DIR}/${_testName})
endfunction (ADD_CMOCKA_TEST)

ADD_CMOCKA_TEST(crc32c_test)
ADD_CMOCKA_TEST(raw_test)
ADD_CMOCKA_TEST(jls_test)
ADD_CMOCKA_TEST(statistics_test)
ADD_CMOCKA_TEST(threaded_test)
