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
#include <stdlib.h>
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

static char * str_pattern(size_t len, unsigned int seed) {
    char * s = malloc(len + 1);
    assert_non_null(s);
    for (size_t i = 0; i < len; ++i) {
        s[i] = (char) ('A' + ((i + seed) % 26));
    }
    s[len] = 0;
    return s;
}

static void test_string_save_large(void **state) {
    (void) state;
    size_t len = JLS_BUF_STRING_SIZE + (JLS_BUF_STRING_SIZE / 2);
    char * big = str_pattern(len, 0);
    char * saved = NULL;
    char * small_saved = NULL;
    struct jls_buf_s * b = jls_buf_alloc();
    assert_int_equal(0, jls_buf_string_save(b, "small", &small_saved));
    assert_int_equal(0, jls_buf_string_save(b, big, &saved));
    assert_string_equal(big, saved);
    assert_int_equal(0, jls_buf_string_save(b, "after", &saved));
    assert_string_equal("after", saved);
    assert_string_equal("small", small_saved);
    jls_buf_free(b);
    free(big);
}

static void test_realloc_grows_and_rebases(void **state) {
    (void) state;
    uint32_t u32 = 0;
    struct jls_buf_s * b = jls_buf_alloc();
    assert_int_equal(JLS_BUF_DEFAULT_SIZE, b->alloc_size);
    assert_int_equal(0, jls_buf_wr_u32(b, 0x12345678));
    assert_int_equal(0, jls_buf_realloc(b, JLS_BUF_DEFAULT_SIZE + 1));
    assert_int_equal(2 * JLS_BUF_DEFAULT_SIZE, b->alloc_size);  // doubles
    assert_ptr_equal(b->start + 4, b->cur);
    assert_ptr_equal(b->start + 4, b->end);
    assert_int_equal(0, jls_buf_realloc(b, 5 * JLS_BUF_DEFAULT_SIZE));
    assert_int_equal(8 * JLS_BUF_DEFAULT_SIZE, b->alloc_size);

    // writes grow past the initial allocation through the rebased cursor
    jls_buf_reset(b);
    size_t count = (3 * JLS_BUF_DEFAULT_SIZE) / sizeof(u32);
    struct jls_buf_s * c = jls_buf_alloc();
    for (uint32_t i = 0; i < count; ++i) {
        assert_int_equal(0, jls_buf_wr_u32(c, i));
    }
    assert_int_equal(count * sizeof(u32), jls_buf_length(c));
    c->cur = c->start;
    for (uint32_t i = 0; i < count; ++i) {
        assert_int_equal(0, jls_buf_rd_u32(c, &u32));
        assert_int_equal(i, u32);
    }
    jls_buf_free(c);
    jls_buf_free(b);
}

static void test_copy(void **state) {
    (void) state;
    uint32_t u32 = 0;
    struct jls_buf_s * a = jls_buf_alloc();
    struct jls_buf_s * b = jls_buf_alloc();
    assert_int_equal(0, jls_buf_wr_u32(a, 7));
    assert_int_equal(0, jls_buf_wr_u32(a, 9));
    assert_int_equal(0, jls_buf_copy(b, a));
    assert_int_equal(8, jls_buf_length(b));
    assert_ptr_equal(b->start, b->cur);
    assert_ptr_equal(b->start + 8, b->end);
    assert_int_equal(0, jls_buf_rd_u32(b, &u32));
    assert_int_equal(7, u32);
    assert_int_equal(0, jls_buf_wr_u32(b, 11));  // overwrites the second value
    jls_buf_free(a);
    jls_buf_free(b);
}

static void test_rd_str_straddle(void **state) {
    (void) state;
    // many strings totalling > 2 strings pages, plus one string > 1 page
    const size_t count = 3000;
    const size_t big_len = JLS_BUF_STRING_SIZE + 100;
    char * strs[3001];
    struct jls_buf_s * b = jls_buf_alloc();
    for (size_t i = 0; i < count; ++i) {
        strs[i] = str_pattern(700 + (i % 13), (unsigned int) i);
        assert_int_equal(0, jls_buf_wr_str(b, strs[i]));
    }
    strs[count] = str_pattern(big_len, 5);
    assert_int_equal(0, jls_buf_wr_str(b, strs[count]));

    b->cur = b->start;
    const char * rd[3001];
    for (size_t i = 0; i <= count; ++i) {
        assert_int_equal(0, jls_buf_rd_str(b, &rd[i]));
        assert_string_equal(strs[i], rd[i]);
    }
    const char * tail = NULL;
    assert_int_equal(JLS_ERROR_EMPTY, jls_buf_rd_str(b, &tail));
    for (size_t i = 0; i <= count; ++i) {
        assert_string_equal(strs[i], rd[i]);  // earlier strings stay valid
        free(strs[i]);
    }
    jls_buf_free(b);
}

static void test_rd_str_unterminated(void **state) {
    (void) state;
    const char * str = NULL;
    struct jls_buf_s * b = jls_buf_alloc();
    assert_int_equal(0, jls_buf_wr_bin(b, "abc", 3));
    b->cur = b->start;
    assert_int_equal(JLS_ERROR_EMPTY, jls_buf_rd_str(b, &str));
    assert_null(str);

    // terminated at the buffer end without the unit separator
    jls_buf_reset(b);
    assert_int_equal(0, jls_buf_wr_bin(b, "abc", 4));
    b->cur = b->start;
    assert_int_equal(0, jls_buf_rd_str(b, &str));
    assert_string_equal("abc", str);
    assert_ptr_equal(b->end, b->cur);
    jls_buf_free(b);
}

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_empty),
            cmocka_unit_test(test_string_save),
            cmocka_unit_test(test_wr_rd),
            cmocka_unit_test(test_string_save_large),
            cmocka_unit_test(test_realloc_grows_and_rebases),
            cmocka_unit_test(test_copy),
            cmocka_unit_test(test_rd_str_straddle),
            cmocka_unit_test(test_rd_str_unterminated),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}

