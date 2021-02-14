/*
 * Copyright 2014-2017 Jetperch LLC
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
#include "jls/statistics.h"


static void test_initialize(void **state) {
    (void) state;
    struct jls_statistics_s s;
    jls_statistics_reset(&s);
    assert_int_equal(0, s.k);
    assert_float_equal(0.0, s.mean, 0.0);
    assert_float_equal(0.0, s.s, 0.0);
}

static void test_add_zero_once(void **state) {
    (void) state;
    struct jls_statistics_s s;
    jls_statistics_reset(&s);
    jls_statistics_add(&s, 0.0);
    assert_int_equal(1, s.k);
    assert_float_equal(0.0, s.mean, 0.0);
    assert_float_equal(0.0, s.min, 0.0);
    assert_float_equal(0.0, s.max, 0.0);
    assert_float_equal(0.0, s.s, 0.0);
}

static void test_add_zero_twice(void **state) {
    (void) state;
    struct jls_statistics_s s;
    jls_statistics_reset(&s);
    jls_statistics_add(&s, 0.0);
    jls_statistics_add(&s, 0.0);
    assert_int_equal(2, s.k);
    assert_float_equal(0.0, s.mean, 0.0);
    assert_float_equal(0.0, s.min, 0.0);
    assert_float_equal(0.0, s.max, 0.0);
    assert_float_equal(0.0, s.s, 0.0);
}

static void test_add_f64_data(void **state) {
    (void) state;
    double data[] = {0.0, 1.0, 2.0};
    struct jls_statistics_s s1;
    struct jls_statistics_s s2;
    jls_statistics_reset(&s1);
    for (int i = 0; i < 3; ++i) {
        jls_statistics_add(&s1, data[i]);
    }
    jls_statistics_compute_f64(&s2, data, 3);
    assert_int_equal(3, s1.k);
    assert_int_equal(3, s2.k);
    assert_float_equal(s1.mean, s2.mean, 0.0);
    assert_float_equal(s1.min, s2.min, 0.0);
    assert_float_equal(s1.max, s2.max, 0.0);
    assert_float_equal(s1.s, s2.s, 0.0);
}

static void test_combine_empty(void **state) {
    (void) state;
    struct jls_statistics_s s1;
    struct jls_statistics_s s2;
    struct jls_statistics_s t;
    jls_statistics_reset(&s1);
    jls_statistics_reset(&s2);
    jls_statistics_combine(&t, &s1, &s2);
    assert_int_equal(0, t.k);
}

static void test_combine_a_empty(void **state) {
    (void) state;
    struct jls_statistics_s s1;
    struct jls_statistics_s s2;
    struct jls_statistics_s t;
    jls_statistics_reset(&s1);
    jls_statistics_reset(&s2);
    jls_statistics_add(&s2, 1.0);
    jls_statistics_combine(&t, &s1, &s2);
    assert_int_equal(1, t.k);
    assert_float_equal(1.0, t.mean, 0.0);
}

static void test_combine_b_empty(void **state) {
    (void) state;
    struct jls_statistics_s s1;
    struct jls_statistics_s s2;
    struct jls_statistics_s t;
    jls_statistics_reset(&s1);
    jls_statistics_reset(&s2);
    jls_statistics_add(&s1, 1.0);
    jls_statistics_combine(&t, &s1, &s2);
    assert_int_equal(1, t.k);
    assert_float_equal(1.0, t.mean, 0.0);
}

static void test_combine_f32(void **state) {
    (void) state;
    struct jls_statistics_s s1;
    struct jls_statistics_s s2;
    struct jls_statistics_s ref;
    struct jls_statistics_s t;
    float data[] = {0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 3.4f, 9.8f, -0.2f, 0.5f, 5.4f, 9.9f, 0.3f, 0.1f, -2.0f, -10.0f};
    size_t data_len = sizeof(data) / sizeof(data[0]);
    jls_statistics_compute_f32(&ref, data, data_len);

    for (size_t i = 0; i < data_len; ++i) {
        jls_statistics_reset(&s1);
        jls_statistics_reset(&s2);
        for (size_t k = 0; k < data_len; ++k) {
            jls_statistics_add((k < i) ? &s1 : &s2, data[k]);
        }
        jls_statistics_combine(&t, &s1, &s2);
        assert_int_equal(data_len, t.k);
        assert_float_equal(ref.mean, t.mean, 0.0);
        assert_float_equal(ref.min, t.min, 0.0);
        assert_float_equal(ref.max, t.max, 0.0);
        assert_float_equal(ref.s, t.s, 0.0);
    }
}

static void test_combine_f64(void **state) {
    (void) state;
    struct jls_statistics_s s1;
    struct jls_statistics_s s2;
    struct jls_statistics_s ref;
    struct jls_statistics_s t;
    double data[] = {0.0, 1.0, 2.0, 3.0, 4.0, 3.4, 9.8, -0.2, 0.5, 5.4, 9.9, 0.3, 0.1, -2.0, -10};
    size_t data_len = sizeof(data) / sizeof(data[0]);
    jls_statistics_compute_f64(&ref, data, data_len);

    for (size_t i = 0; i < data_len; ++i) {
        jls_statistics_reset(&s1);
        jls_statistics_reset(&s2);
        for (size_t k = 0; k < data_len; ++k) {
            jls_statistics_add((k < i) ? &s1 : &s2, data[k]);
        }
        jls_statistics_combine(&t, &s1, &s2);
        assert_int_equal(data_len, t.k);
        assert_float_equal(ref.mean, t.mean, 0.0);
        assert_float_equal(ref.min, t.min, 0.0);
        assert_float_equal(ref.max, t.max, 0.0);
        assert_float_equal(ref.s, t.s, 0.0);
    }
}

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_initialize),
            cmocka_unit_test(test_add_zero_once),
            cmocka_unit_test(test_add_zero_twice),
            cmocka_unit_test(test_add_f64_data),
            cmocka_unit_test(test_combine_empty),
            cmocka_unit_test(test_combine_a_empty),
            cmocka_unit_test(test_combine_b_empty),
            cmocka_unit_test(test_combine_f32),
            cmocka_unit_test(test_combine_f64),
            //cmocka_unit_test(),
            //cmocka_unit_test(),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
