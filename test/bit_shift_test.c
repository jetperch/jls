/*
 * Copyright 2022 Jetperch LLC
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
#include "jls/bit_shift.h"
#include "jls/ec.h"
#include <stdio.h>
#include <string.h>


#define U32_INIT_01 {0x01084210, 0x50a04321, 0xffeeddcc, 0xbbaa9988, 0x77665544, 0x33221100}
static const uint32_t U32_01[] = U32_INIT_01;


static void test_0(void **state) {
    (void) state;
    uint32_t data_u32[] = U32_INIT_01;
    assert_int_equal(0, jls_bit_shift_array_right(0, data_u32, sizeof(data_u32)));
    assert_memory_equal(U32_01, data_u32, sizeof(U32_01));
}

static void test_n(void **state) {
    (void) state;
    for (int i = 1; i < 9; ++i) {
        uint32_t data_u32[] = U32_INIT_01;
        assert_int_equal(0, jls_bit_shift_array_right(i, data_u32, sizeof(data_u32)));
        assert_int_equal((U32_01[0] >> i) | (U32_01[1] << (32 - i)), data_u32[0]);
    }
}

static void test_9(void **state) {
    (void) state;
    uint32_t data_u32[] = U32_INIT_01;
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_bit_shift_array_right(9, data_u32, sizeof(data_u32)));
}

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_0),
            cmocka_unit_test(test_n),
            cmocka_unit_test(test_9),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}

