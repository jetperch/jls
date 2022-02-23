/*
 * Copyright 2014-2022 Jetperch LLC
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
#include "jls/crc32c.h"
#include "jls/format.h"
#include <stdio.h>
#include <string.h>


static void test_bytes(void **state) {
    (void) state;
    // https://reveng.sourceforge.io/crc-catalogue/17plus.htm#crc.cat.crc-32c
    uint8_t data[] = "123456789";
    assert_int_equal(0xe3069283, jls_crc32c(data, sizeof(data) - 1));
}


static void test_hdr(void **state) {
    (void) state;
    struct jls_chunk_header_s hdr = {
            .item_next = 1,
            .item_prev = 2,
            .tag = 3,
            .rsv0_u8 = 0,
            .chunk_meta = 4,
            .payload_length = 5,
            .payload_prev_length = 6,
            .crc32 = 0,
    };
    uint32_t c = jls_crc32c((uint8_t *) &hdr, sizeof(hdr) - 4);
    assert_int_equal(c, jls_crc32c_hdr(&hdr));
}

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_bytes),
            cmocka_unit_test(test_hdr),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}

