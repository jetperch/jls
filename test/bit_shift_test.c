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
#include <stdlib.h>
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
    for (int i = 1; i < 8; ++i) {
        uint32_t data_u32[] = U32_INIT_01;
        assert_int_equal(0, jls_bit_shift_array_right(i, data_u32, sizeof(data_u32)));
        assert_int_equal((U32_01[0] >> i) | (U32_01[1] << (32 - i)), data_u32[0]);
    }
}

static void test_n_last_byte(void **state) {
    (void) state;
    for (int i = 1; i < 8; ++i) {
        uint8_t data[3] = {0x00, 0x00, 0xff};
        assert_int_equal(0, jls_bit_shift_array_right(i, data, sizeof(data)));
        assert_int_equal((uint8_t) (0xff << (8 - i)), data[1]);
        assert_int_equal(0xff >> i, data[2]);  // carry kept in the last byte
    }
}

static int bit_get(const uint8_t * p, uint64_t idx) {
    return (p[idx >> 3] >> (idx & 7)) & 1;
}

static void test_bit_copy(void **state) {
    (void) state;
    // exact-size heap buffers: ASan flags any read past the copied range
    for (uint32_t src_off = 0; src_off < 17; ++src_off) {
        for (uint32_t dst_off = 0; dst_off < 17; ++dst_off) {
            for (uint32_t bits = 1; bits < 40; ++bits) {
                size_t src_sz = (src_off + bits + 7) / 8;
                size_t dst_sz = (dst_off + bits + 7) / 8;
                uint8_t * src = malloc(src_sz);
                uint8_t * dst = malloc(dst_sz);
                for (size_t i = 0; i < src_sz; ++i) {
                    src[i] = (uint8_t) (0x5a ^ (i * 37) ^ src_off);
                }
                memset(dst, 0xa5, dst_sz);
                jls_bit_copy(dst, dst_off, src, src_off, bits);
                for (uint32_t i = 0; i < dst_off; ++i) {
                    assert_int_equal(1 & (0xa5 >> (i & 7)), bit_get(dst, i));  // preserved
                }
                for (uint32_t i = 0; i < bits; ++i) {
                    assert_int_equal(bit_get(src, src_off + i), bit_get(dst, dst_off + i));
                }
                for (uint64_t i = dst_off + bits; i < dst_sz * 8; ++i) {
                    assert_int_equal(0, bit_get(dst, i));  // cleared
                }
                free(src);
                free(dst);
            }
        }
    }
}

static void test_8(void **state) {
    (void) state;
    uint32_t data_u32[] = U32_INIT_01;
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_bit_shift_array_right(8, data_u32, sizeof(data_u32)));
}

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_0),
            cmocka_unit_test(test_n),
            cmocka_unit_test(test_n_last_byte),
            cmocka_unit_test(test_bit_copy),
            cmocka_unit_test(test_8),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}

