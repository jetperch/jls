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
#include "jls/datatype.h"
#include "jls/format.h"
#include "jls/ec.h"
#include <stdio.h>
#include <string.h>
#include <float.h>

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))

static void test_u1(void **state) {
    (void) state;
    double dst[16];
    uint8_t u1[] = {1, 1, 1, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1, 0, 1};
    uint8_t src[] = {0, 0};
    for (size_t i = 0; i < sizeof(u1); ++i) {
        src[i >> 3] |= u1[i] << (i & 0x07);
    }
    assert_int_equal(0, jls_dt_buffer_to_f64(src, JLS_DATATYPE_U1, dst, 16));
    for (size_t i = 0; i < sizeof(u1); ++i) {
        assert_float_equal((double) u1[i], dst[i], 1e-15);
    }
}

static void test_u4(void **state) {
    (void) state;
    double dst[16];
    uint8_t u4[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    uint8_t src[] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (size_t i = 0; i < sizeof(u4); ++i) {
        src[i >> 1] |= u4[i] << (4 * (i & 1));
    }
    assert_int_equal(0, jls_dt_buffer_to_f64(src, JLS_DATATYPE_U4, dst, 16));
    for (size_t i = 0; i < sizeof(u4); ++i) {
        assert_float_equal((double) u4[i], dst[i], 1e-15);
    }
}

#define VALIDATE(src, datatype_) \
    double dst[ARRAY_SIZE(src)]; \
    assert_int_equal(0, jls_dt_buffer_to_f64(src, datatype_, dst, ARRAY_SIZE(src))); \
    for (size_t i = 0; i < ARRAY_SIZE(src); ++i) { \
        assert_float_equal((double) src[i], dst[i], 1e-15); \
    }

static void test_u8(void **state) {
    (void) state;
    uint8_t u8[] = {0, 2, 4, 8, 16, 32, 64, 128, 1, 3, 7, 15, 31, 63, 127, 255};
    VALIDATE(u8, JLS_DATATYPE_U8);
}

static void test_u16(void **state) {
    (void) state;
    uint16_t u16[] = {0, 2, 4, 8, 16, 32, 64, 128, 256, 0x0100, 0x0200, 0x0400, 0x0800, 0x1000, 0x2000, 0x4000,
                      0x0001, 0x000f, 0x00ff, 0x0fff, 0xffff};
    VALIDATE(u16, JLS_DATATYPE_U16);
}

static void test_u32(void **state) {
    (void) state;
    uint32_t u32[] = {0, 0x00000008, 0x00000080, 0x00000800, 0x00008000, 0x00080000, 0x00080000, 0x00800000, 0x08000000, 0x80000000,
                      0x0000000f, 0x000000ff, 0x00000fff, 0x0000ffff, 0x000fffff, 0x00ffffff, 0x0fffffff, 0xffffffff};
    VALIDATE(u32, JLS_DATATYPE_U32);
}

static void test_u64(void **state) {
    (void) state;
    uint64_t u64[128];
    uint64_t mask = 0;
    for (size_t i = 0; i < 64; ++i) {
        u64[i] = ((uint64_t) 1) << i;
        u64[i + 64] = mask;
        mask = (mask << 1) | 1;
    }
    VALIDATE(u64, JLS_DATATYPE_U64);
}

static void test_i4(void **state) {
    (void) state;
    double dst[16];
    int8_t i4[] = {0, 1, 2, 3, 4, 5, 6, 7, -1, -2, -3, -4, -5, -6, -7, -8};
    uint8_t src[] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (size_t i = 0; i < sizeof(i4); ++i) {
        src[i >> 1] |= ((uint8_t) (i4[i] & 0x0f)) << (4 * (i & 1));
    }
    assert_int_equal(0, jls_dt_buffer_to_f64(src, JLS_DATATYPE_I4, dst, 16));
    for (size_t i = 0; i < sizeof(i4); ++i) {
        assert_float_equal((double) i4[i], dst[i], 1e-15);
    }
}

static void test_i8(void **state) {
    (void) state;
    int8_t i8[] = {0, 16, 32, 64, 127, -1, -16, -32, -64, -127, -128};
    VALIDATE(i8, JLS_DATATYPE_I8);
}

static void test_i16(void **state) {
    (void) state;
    int16_t i16[] = {0, 7, 255, 2057, 32767, -7, -255, -2057, -2058};
    VALIDATE(i16, JLS_DATATYPE_I16);
}

static void test_i32(void **state) {
    (void) state;
    int32_t i32[64];
    int32_t x = 1;
    memset(i32, 0, sizeof(i32));
    for (size_t i = 0; i < 32; ++i) {
        i32[1 + i] = x;
        i32[32 + i] = -x;
    }
    VALIDATE(i32, JLS_DATATYPE_I32);
}

static void test_i64(void **state) {
    (void) state;
    int64_t i64[128];
    int64_t x = 1;
    memset(i64, 0, sizeof(i64));
    for (size_t i = 0; i < 64; ++i) {
        i64[1 + i] = x;
        i64[64 + i] = -x;
    }
    VALIDATE(i64, JLS_DATATYPE_I64);
}

static void test_f32(void **state) {
    (void) state;
    float src[] = {0.0f, 1.0f, -1.0f, FLT_MAX, -FLT_MAX};
    VALIDATE(src, JLS_DATATYPE_F32);
}

static void test_f64(void **state) {
    (void) state;
    double src[] = {0.0f, 1.0f, -1.0f, DBL_MAX, -DBL_MAX};
    VALIDATE(src, JLS_DATATYPE_F64);
}

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_u1),
            cmocka_unit_test(test_u4),
            cmocka_unit_test(test_u8),
            cmocka_unit_test(test_u16),
            cmocka_unit_test(test_u32),
            cmocka_unit_test(test_u64),

            cmocka_unit_test(test_i4),
            cmocka_unit_test(test_i8),
            cmocka_unit_test(test_i16),
            cmocka_unit_test(test_i32),
            cmocka_unit_test(test_i64),

            cmocka_unit_test(test_f32),
            cmocka_unit_test(test_f64),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}

