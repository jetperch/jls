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
#include "jls/reader.h"
#include "jls/writer.h"
#include "jls/format.h"
#include "jls/time.h"
#include "jls/ec.h"
#include <stdio.h>
#include <string.h>


const char * filename = "tmp.jls";
const uint8_t USER_DATA_1[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
const uint16_t CHUNK_META_1 = 0x1234;
const uint8_t USER_DATA_2[] = {0x11, 0x22, 0xab, 0x34, 0x45, 0x46, 0x42, 42, 42, 3, 7};
const uint16_t CHUNK_META_2 = 0xBEEF;


const struct jls_source_def_s SOURCE_1 = {
        .source_id = 1,
        .name = "source 1",
        .vendor = "vendor",
        .model = "model",
        .version = "version",
        .serial_number = "serial_number",
};

const struct jls_source_def_s SOURCE_2 = {
        .source_id = 2,
        .name = "source 2",
        .vendor = "vendor",
        .model = "model",
        .version = "version",
        .serial_number = "serial_number",
};

static void test_source(void **state) {
    (void) state;
    struct jls_wr_s * wr = NULL;
    struct jls_rd_s * rd = NULL;
    assert_int_equal(0, jls_wr_open(&wr, filename));
    assert_int_equal(0, jls_wr_source_def(wr, &SOURCE_1));
    assert_int_equal(0, jls_wr_source_def(wr, &SOURCE_2));
    assert_int_equal(0, jls_wr_close(wr));

    assert_int_equal(0, jls_rd_open(&rd, filename));
    jls_rd_close(rd);
}

static void test_wr_source_duplicate(void **state) {
    (void) state;
    struct jls_wr_s * wr = NULL;
    struct jls_rd_s * rd = NULL;
    assert_int_equal(0, jls_wr_open(&wr, filename));
    assert_int_equal(0, jls_wr_source_def(wr, &SOURCE_1));
    assert_int_equal(JLS_ERROR_ALREADY_EXISTS, jls_wr_source_def(wr, &SOURCE_1));
    assert_int_equal(0, jls_wr_close(wr));
}

static void test_annotation(void **state) {
    (void) state;
    int64_t now = jls_now();
    struct jls_wr_s * wr = NULL;
    struct jls_rd_s * rd = NULL;
    assert_int_equal(0, jls_wr_open(&wr, filename));
    assert_int_equal(0, jls_wr_vsr_annotation(wr, 0, now - JLS_TIME_SECOND,
                                              JLS_ANNOTATION_TYPE_TEXT, JLS_STORAGE_TYPE_STRING, "hello there", 0));
    assert_int_equal(0, jls_wr_close(wr));

    assert_int_equal(0, jls_rd_open(&rd, filename));
    jls_rd_close(rd);
}

static void test_user_data(void **state) {
    (void) state;
    struct jls_wr_s * wr = NULL;
    struct jls_rd_s * rd = NULL;
    assert_int_equal(0, jls_wr_open(&wr, filename));
    assert_int_equal(0, jls_wr_user_data(wr, CHUNK_META_1, JLS_STORAGE_TYPE_BINARY, USER_DATA_1, sizeof(USER_DATA_1)));
    assert_int_equal(0, jls_wr_user_data(wr, CHUNK_META_2, JLS_STORAGE_TYPE_BINARY, USER_DATA_2, sizeof(USER_DATA_2)));
    assert_int_equal(0, jls_wr_close(wr));

    assert_int_equal(0, jls_rd_open(&rd, filename));
    jls_rd_close(rd);
}

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_source),
            cmocka_unit_test(test_wr_source_duplicate),
            cmocka_unit_test(test_annotation),
            cmocka_unit_test(test_user_data),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
