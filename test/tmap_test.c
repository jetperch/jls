/*
 * Copyright 2023-2025 Jetperch LLC
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
#include "jls/tmap.h"
#include "jls/time.h"
#include <math.h>
#include <stdint.h>


#define SECOND  JLS_TIME_SECOND
#define MINUTE  JLS_TIME_MINUTE
#define HOUR    JLS_TIME_HOUR
#define YEAR    JLS_TIME_YEAR


static void test_empty(void **state) {
    (void) state;
    int64_t v;
    struct jls_utc_summary_entry_s entry;
    struct jls_tmap_s * s = jls_tmap_alloc(1000.0);
    assert_int_equal(0, jls_tmap_length(s));
    assert_int_equal(JLS_ERROR_UNAVAILABLE, jls_tmap_sample_id_to_timestamp(s, 1000, &v));
    assert_int_equal(JLS_ERROR_UNAVAILABLE, jls_tmap_timestamp_to_sample_id(s, YEAR, &v));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_tmap_get(s, 0, &entry));
    jls_tmap_free(s);
}

static void test_single(void **state) {
    (void) state;
    int64_t v;
    struct jls_utc_summary_entry_s entry;
    struct jls_tmap_s * s = jls_tmap_alloc(1000.0);
    jls_tmap_add(s, 1000, YEAR);
    assert_int_equal(1, jls_tmap_length(s));
    assert_int_equal(0, jls_tmap_sample_id_to_timestamp(s, 1000, &v)); assert_int_equal(YEAR, v);
    assert_int_equal(0, jls_tmap_sample_id_to_timestamp(s, 2000, &v)); assert_int_equal(YEAR + SECOND, v);

    assert_int_equal(0, jls_tmap_timestamp_to_sample_id(s, YEAR, &v)); assert_int_equal(1000, v);
    assert_int_equal(0, jls_tmap_timestamp_to_sample_id(s, YEAR + SECOND, &v)); assert_int_equal(2000, v);

    assert_int_equal(0, jls_tmap_get(s, 0, &entry));
    assert_int_equal(1000, entry.sample_id);
    assert_int_equal(YEAR, entry.timestamp);

    jls_tmap_free(s);
}

static void test_interp2(void **state) {
    (void) state;
    int64_t v;
    struct jls_tmap_s * s = jls_tmap_alloc(20.0);  // inaccurate
    jls_tmap_add(s, 1000, YEAR);
    jls_tmap_add(s, 2000, YEAR + SECOND);

    assert_int_equal(0, jls_tmap_sample_id_to_timestamp(s, 1000, &v)); assert_int_equal(YEAR, v);          // exact 0
    assert_int_equal(0, jls_tmap_sample_id_to_timestamp(s, 2000, &v)); assert_int_equal(YEAR + SECOND, v);  // exact 1
    assert_int_equal(0, jls_tmap_sample_id_to_timestamp(s, 1500, &v)); assert_int_equal(YEAR + SECOND / 2, v);  // interp
    assert_int_equal(0, jls_tmap_sample_id_to_timestamp(s, 500, &v)); assert_int_equal(YEAR - SECOND / 2, v);  // below range
    assert_int_equal(0, jls_tmap_sample_id_to_timestamp(s, 2500, &v)); assert_int_equal(YEAR + 3 * SECOND / 2, v);  // above range

    assert_int_equal(0, jls_tmap_timestamp_to_sample_id(s, YEAR, &v)); assert_int_equal(1000, v);
    assert_int_equal(0, jls_tmap_timestamp_to_sample_id(s, YEAR + SECOND, &v)); assert_int_equal(2000, v);
    assert_int_equal(0, jls_tmap_timestamp_to_sample_id(s, YEAR + SECOND / 2, &v)); assert_int_equal(1500, v);
    assert_int_equal(0, jls_tmap_timestamp_to_sample_id(s, YEAR - SECOND / 2, &v)); assert_int_equal(500, v);  // below range
    assert_int_equal(0, jls_tmap_timestamp_to_sample_id(s, YEAR + 3 * SECOND / 2, &v)); assert_int_equal(2500, v);  // above range
}

static void test_interpN(void **state) {
    (void) state;
    int64_t v;
    struct jls_tmap_s *s = jls_tmap_alloc(20.0);  // inaccurate
    jls_tmap_add(s, 1000, YEAR);
    jls_tmap_add(s, 2000, YEAR + SECOND);  // 1000 samples/second
    jls_tmap_add(s, 4000, YEAR + 2 * SECOND);  // 2000 samples/second
    jls_tmap_add(s, 4100, YEAR + 3 * SECOND);  // 100 samples/second

    assert_int_equal(0, jls_tmap_sample_id_to_timestamp(s, 1000, &v)); assert_int_equal(YEAR + 0 * SECOND, v);  // exact 0
    assert_int_equal(0, jls_tmap_sample_id_to_timestamp(s, 2000, &v)); assert_int_equal(YEAR + 1 * SECOND, v);  // exact 1
    assert_int_equal(0, jls_tmap_sample_id_to_timestamp(s, 4000, &v)); assert_int_equal(YEAR + 2 * SECOND, v);  // exact 2
    assert_int_equal(0, jls_tmap_sample_id_to_timestamp(s, 4100, &v)); assert_int_equal(YEAR + 3 * SECOND, v);  // exact 3
    assert_int_equal(0, jls_tmap_sample_id_to_timestamp(s, 1500, &v)); assert_int_equal(YEAR + SECOND / 2, v);  // interp
    assert_int_equal(0, jls_tmap_sample_id_to_timestamp(s, 3000, &v)); assert_int_equal(YEAR + 3 * SECOND / 2, v);  // interp
    assert_int_equal(0, jls_tmap_sample_id_to_timestamp(s, 4050, &v)); assert_int_equal(YEAR + 5 * SECOND / 2, v);  // interp
    assert_int_equal(0, jls_tmap_sample_id_to_timestamp(s, 500, &v)); assert_int_equal(YEAR - SECOND / 2, v);  // below range
    assert_int_equal(0, jls_tmap_sample_id_to_timestamp(s, 4150, &v)); assert_int_equal(YEAR + 7 * SECOND / 2, v);  // below range
}

static void test_interp_full_allocation(void **state) {
    (void) state;
    int64_t v;
    const int64_t count = 1000;  // ENTRIES_ALLOC_INIT, so x[count] is past the allocation
    struct jls_tmap_s * s = jls_tmap_alloc(1000.0);
    for (int64_t i = 0; i < count; ++i) {
        jls_tmap_add(s, i * 1000, YEAR + i * SECOND);
    }
    assert_int_equal(count, jls_tmap_length(s));
    assert_int_equal(0, jls_tmap_sample_id_to_timestamp(s, (count - 1) * 1000, &v));
    assert_int_equal(YEAR + (count - 1) * SECOND, v);
    assert_int_equal(0, jls_tmap_sample_id_to_timestamp(s, count * 1000, &v));
    assert_int_equal(YEAR + count * SECOND, v);
    assert_int_equal(0, jls_tmap_timestamp_to_sample_id(s, YEAR + count * SECOND, &v));
    assert_int_equal(count * 1000, v);
    jls_tmap_free(s);
}

// Growth past the initial allocation keeps every entry (realloc paths).
static void test_grow(void **state) {
    (void) state;
    int64_t v;
    const int64_t count = 5000;
    struct jls_tmap_s * s = jls_tmap_alloc(1000.0);
    for (int64_t i = 0; i < count; ++i) {
        assert_int_equal(0, jls_tmap_add(s, i * 1000, YEAR + i * SECOND));
    }
    assert_int_equal(count, jls_tmap_length(s));
    assert_int_equal(0, jls_tmap_sample_id_to_timestamp(s, 4321500, &v));
    assert_int_equal(YEAR + 4321 * SECOND + SECOND / 2, v);
    assert_int_equal(0, jls_tmap_timestamp_to_sample_id(s, YEAR + 4999 * SECOND, &v));
    assert_int_equal(4999000, v);
    jls_tmap_free(s);
}

// Non-increasing UTC breaks the timestamp search and divides by zero.
static void test_utc_not_monotonic(void **state) {
    (void) state;
    int64_t v;
    struct jls_utc_summary_entry_s entry;
    struct jls_tmap_s * s = jls_tmap_alloc(1000.0);
    assert_int_equal(0, jls_tmap_add(s, 1000, YEAR));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_tmap_add(s, 2000, YEAR));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_tmap_add(s, 3000, YEAR - SECOND));
    assert_int_equal(1, jls_tmap_length(s));
    assert_int_equal(0, jls_tmap_add(s, 2000, YEAR + SECOND));
    assert_int_equal(0, jls_tmap_add(s, 2000, YEAR + 2 * SECOND));  // overwrite last
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_tmap_add(s, 2000, YEAR));  // overwrite checks prior
    assert_int_equal(2, jls_tmap_length(s));
    assert_int_equal(0, jls_tmap_get(s, 1, &entry));
    assert_int_equal(YEAR + 2 * SECOND, entry.timestamp);
    assert_int_equal(0, jls_tmap_timestamp_to_sample_id(s, YEAR + SECOND, &v));
    assert_int_equal(1500, v);
    jls_tmap_free(s);
}

// Hostile extremes saturate instead of overflowing (UBSan aborts on UB).
static void test_extreme_values(void **state) {
    (void) state;
    int64_t v;
    struct jls_tmap_s * s = jls_tmap_alloc(1000.0);
    assert_int_equal(0, jls_tmap_add(s, 0, 0));
    assert_int_equal(0, jls_tmap_sample_id_to_timestamp(s, INT64_MAX, &v));
    assert_int_equal(INT64_MAX, v);
    assert_int_equal(0, jls_tmap_sample_id_to_timestamp(s, INT64_MIN, &v));
    assert_int_equal(INT64_MIN, v);
    jls_tmap_free(s);

    s = jls_tmap_alloc(1000.0);
    assert_int_equal(0, jls_tmap_add(s, INT64_MIN, INT64_MIN));
    assert_int_equal(0, jls_tmap_add(s, INT64_MAX, INT64_MIN + 1));
    assert_int_equal(0, jls_tmap_sample_id_to_timestamp(s, 0, &v));
    assert_int_equal(0, jls_tmap_timestamp_to_sample_id(s, INT64_MAX, &v));
    assert_int_equal(INT64_MAX, v);
    assert_int_equal(0, jls_tmap_timestamp_to_sample_id(s, INT64_MIN, &v));
    assert_int_equal(INT64_MIN, v);
    jls_tmap_free(s);

    s = jls_tmap_alloc(NAN);  // invalid rate: single-entry conversion unavailable
    assert_int_equal(0, jls_tmap_add(s, 0, 0));
    assert_int_equal(JLS_ERROR_UNAVAILABLE, jls_tmap_sample_id_to_timestamp(s, 1, &v));
    assert_int_equal(JLS_ERROR_UNAVAILABLE, jls_tmap_timestamp_to_sample_id(s, 1, &v));
    jls_tmap_free(s);
}

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_empty),
            cmocka_unit_test(test_single),
            cmocka_unit_test(test_interp2),
            cmocka_unit_test(test_interpN),
            cmocka_unit_test(test_interp_full_allocation),
            cmocka_unit_test(test_grow),
            cmocka_unit_test(test_utc_not_monotonic),
            cmocka_unit_test(test_extreme_values),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
