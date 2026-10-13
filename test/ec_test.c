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
#include "jls/ec.h"
#include <string.h>


#define EC_CHECK(NAME, TEXT) \
    assert_string_equal(#NAME, jls_error_code_name(JLS_ERROR_ ## NAME)); \
    assert_string_equal(TEXT, jls_error_code_description(JLS_ERROR_ ## NAME));

static void test_names(void **state) {
    (void) state;
    JLS_ERROR_CODES(EC_CHECK);
    assert_int_equal(0, JLS_SUCCESS);
    assert_string_equal("PARAMETER_INVALID", jls_error_code_name(JLS_ERROR_PARAMETER_INVALID));
}

static void test_unknown(void **state) {
    (void) state;
    assert_string_equal("UNKNOWN", jls_error_code_name(JLS_ERROR_CODE_COUNT));
    assert_string_equal("UNKNOWN", jls_error_code_name(-1));
    assert_string_equal("Unknown error", jls_error_code_description(JLS_ERROR_CODE_COUNT));
    assert_string_equal("Unknown error", jls_error_code_description(-1));
}

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_names),
            cmocka_unit_test(test_unknown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
