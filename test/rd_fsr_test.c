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
#include "jls/rd_fsr.h"
#include "jls/time.h"


#define SECOND  JLS_TIME_SECOND
#define MINUTE  JLS_TIME_MINUTE
#define HOUR    JLS_TIME_HOUR
#define YEAR    JLS_TIME_YEAR


static void test_empty(void **state) {
    (void) state;
    struct jls_rd_fsr_s * s = jls_rd_fsr_alloc(1000.0);
    assert_int_equal(0, jls_rd_fsr_sample_id_to_utc(s, 1000));
    assert_int_equal(0, jls_rd_fsr_utc_to_sample_id(s, YEAR));
    jls_rd_fsr_utc_free(s);
}

static void test_single(void **state) {
    (void) state;
    struct jls_rd_fsr_s * s = jls_rd_fsr_alloc(1000.0);
    jls_rd_fsr_add(s, 1000, YEAR);
    assert_int_equal(YEAR, jls_rd_fsr_sample_id_to_utc(s, 1000));
    assert_int_equal(YEAR + SECOND, jls_rd_fsr_sample_id_to_utc(s, 2000));

    assert_int_equal(1000, jls_rd_fsr_utc_to_sample_id(s, YEAR));
    assert_int_equal(2000, jls_rd_fsr_utc_to_sample_id(s, YEAR + SECOND));

    jls_rd_fsr_utc_free(s);
}

static void test_interp2(void **state) {
    struct jls_rd_fsr_s * s = jls_rd_fsr_alloc(20.0);  // inaccurate
    jls_rd_fsr_add(s, 1000, YEAR);
    jls_rd_fsr_add(s, 2000, YEAR + SECOND);

    assert_int_equal(YEAR, jls_rd_fsr_sample_id_to_utc(s, 1000));           // exact 0
    assert_int_equal(YEAR + SECOND, jls_rd_fsr_sample_id_to_utc(s, 2000));  // exact 1
    assert_int_equal(YEAR + SECOND/2, jls_rd_fsr_sample_id_to_utc(s, 1500));  // interp
    assert_int_equal(YEAR - SECOND/2, jls_rd_fsr_sample_id_to_utc(s, 500));  // below range
    assert_int_equal(YEAR + 3*SECOND/2, jls_rd_fsr_sample_id_to_utc(s, 2500));  // above range

    assert_int_equal(1000, jls_rd_fsr_utc_to_sample_id(s, YEAR));
    assert_int_equal(2000, jls_rd_fsr_utc_to_sample_id(s, YEAR + SECOND));
    assert_int_equal(1500, jls_rd_fsr_utc_to_sample_id(s, YEAR + SECOND / 2));
    assert_int_equal(500, jls_rd_fsr_utc_to_sample_id(s, YEAR - SECOND / 2));  // below range
    assert_int_equal(2500, jls_rd_fsr_utc_to_sample_id(s, YEAR + 3 * SECOND / 2));  // above range
}

static void test_interpN(void **state) {
    struct jls_rd_fsr_s *s = jls_rd_fsr_alloc(20.0);  // inaccurate
    jls_rd_fsr_add(s, 1000, YEAR);
    jls_rd_fsr_add(s, 2000, YEAR + SECOND);  // 1000 samples/second
    jls_rd_fsr_add(s, 4000, YEAR + 2 * SECOND);  // 2000 samples/second
    jls_rd_fsr_add(s, 4100, YEAR + 3 * SECOND);  // 100 samples/second

    assert_int_equal(YEAR + 0 * SECOND, jls_rd_fsr_sample_id_to_utc(s, 1000));  // exact 0
    assert_int_equal(YEAR + 1 * SECOND, jls_rd_fsr_sample_id_to_utc(s, 2000));  // exact 1
    assert_int_equal(YEAR + 2 * SECOND, jls_rd_fsr_sample_id_to_utc(s, 4000));  // exact 2
    assert_int_equal(YEAR + 3 * SECOND, jls_rd_fsr_sample_id_to_utc(s, 4100));  // exact 3
    assert_int_equal(YEAR + SECOND / 2, jls_rd_fsr_sample_id_to_utc(s, 1500));  // interp
    assert_int_equal(YEAR + 3 * SECOND / 2, jls_rd_fsr_sample_id_to_utc(s, 3000));  // interp
    assert_int_equal(YEAR + 5 * SECOND / 2, jls_rd_fsr_sample_id_to_utc(s, 4050));  // interp
    assert_int_equal(YEAR - SECOND / 2, jls_rd_fsr_sample_id_to_utc(s, 500));  // below range
    assert_int_equal(YEAR + 7 * SECOND / 2, jls_rd_fsr_sample_id_to_utc(s, 4150));  // below range


}

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_empty),
            cmocka_unit_test(test_single),
            cmocka_unit_test(test_interp2),
            cmocka_unit_test(test_interpN),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
