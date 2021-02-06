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
#include "jls/writer.h"
#include "jls/format.h"
#include "jls/ec.h"
#include <stdio.h>
#include <string.h>


static const char * filename = "tmp.jls";

const struct jls_source_def_s SOURCE1 = {
        .source_id = 1,
        .name = "name",
        .vendor = "vendor",
        .model = "model",
        .version = "version",
        .serial_number = "serial_number",
};


static const uint8_t FILE_HDR[] = JLS_HEADER_IDENTIFICATION;

static void test_invalid_open(void **state) {
    (void) state;
    struct jls_wr_s * wr = NULL;
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_wr_open(0, filename));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_wr_open(&wr, 0));
    assert_int_equal(JLS_ERROR_IO, jls_wr_open(&wr, "C:\\/__this_path_should_never__/exist.jls"));
}

static void test_open(void **state) {
    (void) state;
    struct jls_wr_s * wr = NULL;
    assert_int_equal(0, jls_wr_open(&wr, filename));
    assert_int_equal(0, jls_wr_close(wr));

    FILE * f = fopen(filename, "rb");
    fseek(f, 0L, SEEK_END);
    assert_int_equal(32, ftell(f));
    assert_non_null(f);
    fseek(f, 0L, SEEK_SET);

    struct jls_file_header_s hdr;
    assert_int_equal(sizeof(hdr), fread((uint8_t *) &hdr, 1, sizeof(hdr), f));
    assert_int_equal(0, memcmp(FILE_HDR, hdr.identification, sizeof(FILE_HDR)));
    assert_int_equal(JLS_FORMAT_VERSION_U32, hdr.version);
    assert_int_equal(32, hdr.length);

    remove(filename);
}

static void test_source(void **state) {
    (void) state;
    struct jls_wr_s * wr = NULL;
    assert_int_equal(0, jls_wr_open(&wr, filename));
    assert_int_equal(0, jls_wr_source_def(wr, &SOURCE1));
    assert_int_equal(0, jls_wr_close(wr));
}

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_invalid_open),
            cmocka_unit_test(test_open),
            cmocka_unit_test(test_source),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
