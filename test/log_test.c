/*
 * Copyright 2026 Jetperch LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include "jls/log.h"


static void test_level_to_str(void **state) {
    (void) state;
    assert_string_equal("OFF", jls_log_level_to_str(JLS_LOG_LEVEL_OFF));
    assert_string_equal("EMERGENCY", jls_log_level_to_str(JLS_LOG_LEVEL_EMERGENCY));
    assert_string_equal("ALERT", jls_log_level_to_str(JLS_LOG_LEVEL_ALERT));
    assert_string_equal("CRITICAL", jls_log_level_to_str(JLS_LOG_LEVEL_CRITICAL));
    assert_string_equal("ERROR", jls_log_level_to_str(JLS_LOG_LEVEL_ERROR));
    assert_string_equal("WARN", jls_log_level_to_str(JLS_LOG_LEVEL_WARNING));
    assert_string_equal("NOTICE", jls_log_level_to_str(JLS_LOG_LEVEL_NOTICE));
    assert_string_equal("INFO", jls_log_level_to_str(JLS_LOG_LEVEL_INFO));
    assert_string_equal("DEBUG", jls_log_level_to_str(JLS_LOG_LEVEL_DEBUG1));
    assert_string_equal("DEBUG2", jls_log_level_to_str(JLS_LOG_LEVEL_DEBUG2));
    assert_string_equal("DEBUG3", jls_log_level_to_str(JLS_LOG_LEVEL_DEBUG3));
    assert_string_equal("ALL", jls_log_level_to_str(JLS_LOG_LEVEL_ALL));
    assert_string_equal("ALL", jls_log_level_to_str(JLS_LOG_LEVEL_ALL + 1));
}

static void test_level_to_char(void **state) {
    (void) state;
    assert_int_equal('*', jls_log_level_to_char(JLS_LOG_LEVEL_OFF));
    assert_int_equal('E', jls_log_level_to_char(JLS_LOG_LEVEL_ERROR));
    assert_int_equal('N', jls_log_level_to_char(JLS_LOG_LEVEL_NOTICE));
    assert_int_equal('I', jls_log_level_to_char(JLS_LOG_LEVEL_INFO));
    assert_int_equal('.', jls_log_level_to_char(JLS_LOG_LEVEL_ALL));
}

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_level_to_str),
            cmocka_unit_test(test_level_to_char),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
