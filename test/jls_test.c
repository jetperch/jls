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
#include "jls/ec.h"
#include <stdio.h>
#include <string.h>


const char * filename = "tmp.jls";

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
    assert_int_equal(0, jls_rd_open(&rd, filename));
    jls_rd_close(rd);
}

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_source),
            cmocka_unit_test(test_wr_source_duplicate),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
