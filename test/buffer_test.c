/*
 * Copyright 2023 Jetperch LLC
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
#include "jls/buffer.h"
#include "jls/ec.h"
#include <stdio.h>
#include <string.h>


static void test_empty(void **state) {
    (void) state;
    struct jls_buf_s * b = jls_buf_alloc();
    assert_non_null(b);
    assert_int_equal(0, jls_buf_length(b));

    uint8_t u8;
    uint16_t u16;
    uint32_t u32;
    const char * cstr;

    assert_int_equal(JLS_ERROR_EMPTY, jls_buf_rd_skip(b, 1));
    assert_int_equal(JLS_ERROR_EMPTY, jls_buf_rd_u8(b, &u8));
    assert_int_equal(JLS_ERROR_EMPTY, jls_buf_rd_u16(b, &u16));
    assert_int_equal(JLS_ERROR_EMPTY, jls_buf_rd_u32(b, &u32));
    assert_int_equal(JLS_ERROR_EMPTY, jls_buf_rd_str(b, &cstr));

    jls_buf_reset(b);
    assert_int_equal(0, jls_buf_length(b));

    jls_buf_free(b);
}

static void test_string_save(void **state) {
    (void) state;
    const char * str1 = "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    char * str2 = NULL;
    struct jls_buf_s * b = jls_buf_alloc();
    assert_non_null(b);
    assert_null(b->strings_tail);
    assert_int_equal(0, jls_buf_string_save(b, str1, &str2));
    assert_non_null(b->strings_tail);
    assert_non_null(b->strings_head);
    struct jls_buf_strings_s * s = b->strings_tail;
    while (s == b->strings_tail) {
        assert_int_equal(0, jls_buf_string_save(b, str1, &str2));
        assert_string_equal(str1, str2);
    }
    jls_buf_free(b);
}

static void test_wr_rd(void **state) {
    (void) state;
    const char * stra = "hello world!";
    uint8_t u8a = 42;
    uint16_t u16a = 4342;
    uint32_t u32a = 1353254;
    float f32a = 234.25f;
    int64_t i64a = -347891574383495LL;

    const char * strb = NULL;
    uint8_t u8b = 0;
    uint16_t u16b = 0;
    uint32_t u32b = 0;
    //float f32b = 0.0f;
    //int64_t i64b = 0;

    struct jls_buf_s * b = jls_buf_alloc();
    assert_int_equal(0, jls_buf_wr_zero(b, 32));
    assert_int_equal(0, jls_buf_wr_str(b, stra));
    assert_int_equal(0, jls_buf_wr_bin(b, &u32a, sizeof(u32a)));
    assert_int_equal(0, jls_buf_wr_u8(b, u8a));
    assert_int_equal(0, jls_buf_wr_u16(b, u16a));
    assert_int_equal(0, jls_buf_wr_u32(b, u32a));
    assert_int_equal(0, jls_buf_wr_f32(b, f32a));
    assert_int_equal(0, jls_buf_wr_i64(b, i64a));

    assert_int_equal(0x45, b->length);
    b->cur = b->start;
    assert_int_equal(0, jls_buf_rd_skip(b, 32));
    assert_int_equal(0, jls_buf_rd_str(b, &strb));  assert_string_equal(strb, stra);
    assert_int_equal(0, jls_buf_rd_skip(b, sizeof(u32a)));
    assert_int_equal(0, jls_buf_rd_u8(b, &u8b));  assert_int_equal(u8b, u8a);
    assert_int_equal(0, jls_buf_rd_u16(b, &u16b));  assert_int_equal(u16b, u16a);
    assert_int_equal(0, jls_buf_rd_u32(b, &u32b));  assert_int_equal(u32b, u32a);
    assert_int_equal(0, jls_buf_rd_skip(b, sizeof(float)));
    assert_int_equal(0, jls_buf_rd_skip(b, sizeof(i64a)));

    assert_int_equal(JLS_ERROR_EMPTY, jls_buf_rd_skip(b, 1));
    assert_int_equal(JLS_ERROR_EMPTY, jls_buf_rd_u8(b, &u8b));
    assert_int_equal(JLS_ERROR_EMPTY, jls_buf_rd_u16(b, &u16b));
    assert_int_equal(JLS_ERROR_EMPTY, jls_buf_rd_u32(b, &u32b));
    assert_int_equal(JLS_ERROR_EMPTY, jls_buf_rd_str(b, &strb));

    jls_buf_free(b);
}

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_empty),
            cmocka_unit_test(test_string_save),
            cmocka_unit_test(test_wr_rd),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}

