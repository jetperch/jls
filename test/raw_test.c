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
#include "jls/raw.h"
#include "jls/format.h"
#include "jls/ec.h"
#include <stdio.h>
#include <string.h>


const char * filename = "tmp.jls";
static const uint8_t FILE_HDR[] = JLS_HEADER_IDENTIFICATION;
static const uint8_t PAYLOAD1[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};


static void test_invalid_open(void **state) {
    (void) state;
    struct jls_raw_s * j = NULL;
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_raw_open(0, filename, 0));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_raw_open(&j, 0, 0));
    assert_int_equal(JLS_ERROR_IO, jls_raw_open(&j, "C:\\/__this_path_should_never__/exist.jls", "w"));
    assert_int_equal(JLS_ERROR_IO, jls_raw_open(&j, "C:\\/__this_path_should_never__/exist.jls", "w"));
}

static void test_open_write(void **state) {
    (void) state;
    struct jls_raw_s * j = NULL;
    assert_int_equal(0, jls_raw_open(&j, filename, "w"));
    assert_int_equal(0, jls_raw_close(j));

    FILE * f = fopen(filename, "rb");
    fseek(f, 0L, SEEK_END);
    assert_int_equal(32, ftell(f));
    assert_non_null(f);
    fseek(f, 0L, SEEK_SET);

    struct jls_file_header_s hdr;
    assert_int_equal(sizeof(hdr), fread((uint8_t *) &hdr, 1, sizeof(hdr), f));
    assert_int_equal(0, memcmp(FILE_HDR, hdr.identification, sizeof(FILE_HDR)));
    assert_int_equal(JLS_FORMAT_VERSION_U32, hdr.version.u32);
    assert_int_equal(32, hdr.length);

    remove(filename);
}

static struct jls_chunk_header_s * hdr_set(struct jls_chunk_header_s * hdr, enum jls_tag_e tag, uint16_t chunk_meta, uint32_t payload_length) {
    hdr->tag = tag;
    hdr->chunk_meta = chunk_meta;
    hdr->payload_length = payload_length;
    hdr->payload_prev_length = 0;
    hdr->item_next = 0;
    hdr->item_prev = 0;
    hdr->rsv0_u8 = 0;
    return hdr;
}

static void test_one_chunk(void **state) {
    (void) state;
    struct jls_raw_s * j = NULL;
    struct jls_chunk_header_s hdr;
    uint8_t data[sizeof(PAYLOAD1) + 16];
    assert_int_equal(0, jls_raw_open(&j, filename, "w"));
    assert_int_equal(0, jls_raw_wr(j, hdr_set(&hdr, JLS_TAG_USER_DATA, 0, sizeof(PAYLOAD1)), PAYLOAD1));
    assert_int_equal(0, jls_raw_close(j));

    assert_int_equal(0, jls_raw_open(&j, filename, "r"));
    assert_int_equal(0, jls_raw_rd(j, &hdr, sizeof(data), data));
    assert_memory_equal(PAYLOAD1, data, sizeof(PAYLOAD1));
    assert_int_equal(0, jls_raw_close(j));
    remove(filename);
}

static void construct_n_chunks() {
    struct jls_raw_s * j = NULL;
    struct jls_chunk_header_s hdr;
    uint32_t payload_prev_length = 0;
    uint32_t payload_length = 0;
    assert_int_equal(0, jls_raw_open(&j, filename, "w"));
    for (int i = 0; i < sizeof(PAYLOAD1); ++i) {
        // printf("chunk %d: %d\n", i, (int32_t) jls_raw_chunk_tell(j));
        payload_length = sizeof(PAYLOAD1) - i;
        hdr_set(&hdr, JLS_TAG_USER_DATA, 0, payload_length);
        hdr.payload_prev_length = payload_prev_length;
        assert_int_equal(0, jls_raw_wr(j, &hdr, PAYLOAD1 + i));
        payload_prev_length = payload_length;
    }
    assert_int_equal(0, jls_raw_close(j));
}

static void test_n_chunks(void **state) {
    (void) state;
    struct jls_raw_s * j = NULL;
    struct jls_chunk_header_s hdr;
    uint8_t data[sizeof(PAYLOAD1) + 16];
    construct_n_chunks();

    assert_int_equal(0, jls_raw_open(&j, filename, "r"));
    for (int i = 0; i < sizeof(PAYLOAD1); ++i) {
        assert_int_equal(0, jls_raw_rd(j, &hdr, sizeof(data), data));
        assert_memory_equal(PAYLOAD1 + i, data, sizeof(PAYLOAD1) - i);
    }
    assert_int_equal(0, jls_raw_close(j));
    remove(filename);
}

static void test_chunks_nav(void **state) {
    (void) state;

    struct jls_raw_s * j = NULL;
    struct jls_chunk_header_s hdr;
    uint8_t data[sizeof(PAYLOAD1) + 16];
    construct_n_chunks();

    assert_int_equal(0, jls_raw_open(&j, filename, "r"));
    assert_int_equal(0, jls_raw_chunk_next(j, NULL));
    assert_int_equal(0, jls_raw_rd(j, &hdr, sizeof(data), data));
    assert_memory_equal(PAYLOAD1 + 1, data, sizeof(PAYLOAD1) - 1);

    assert_int_equal(0, jls_raw_chunk_prev(j, NULL));
    assert_int_equal(0, jls_raw_chunk_prev(j, NULL));
    assert_int_equal(0, jls_raw_rd(j, &hdr, sizeof(data), data));
    assert_memory_equal(PAYLOAD1, data, sizeof(PAYLOAD1));
    assert_int_equal(0, jls_raw_chunk_prev(j, NULL));
    assert_int_equal(JLS_ERROR_EMPTY, jls_raw_chunk_prev(j, NULL));

    assert_int_equal(0, jls_raw_open(&j, filename, "r"));
    for (int i = 0; i < sizeof(PAYLOAD1) - 1; ++i) {
        assert_int_equal(0, jls_raw_chunk_next(j, NULL));
    }
    int64_t pos1 = jls_raw_chunk_tell(j);
    assert_int_equal(0, jls_raw_rd(j, &hdr, sizeof(data), data));
    assert_memory_equal(PAYLOAD1 + sizeof(PAYLOAD1) - 1, data, 1);
    assert_int_equal(JLS_ERROR_EMPTY, jls_raw_chunk_next(j, NULL));
    assert_int_equal(JLS_ERROR_EMPTY, jls_raw_chunk_next(j, NULL));
    assert_int_equal(JLS_ERROR_NOT_FOUND, jls_raw_chunk_prev(j, NULL));

    assert_int_equal(0, jls_raw_close(j));
    remove(filename);
}

static void test_seek(void **state) {
    (void) state;

    struct jls_raw_s * j = NULL;
    struct jls_chunk_header_s hdr;
    uint8_t data[sizeof(PAYLOAD1) + 16];
    construct_n_chunks();

    assert_int_equal(0, jls_raw_open(&j, filename, "r"));
    int64_t pos1 = jls_raw_chunk_tell(j);
    assert_int_equal(sizeof(struct jls_file_header_s), pos1);

    assert_int_equal(0, jls_raw_chunk_next(j, NULL));
    int64_t pos2 = jls_raw_chunk_tell(j);
    assert_int_not_equal(pos1, pos2);

    assert_int_equal(0, jls_raw_chunk_seek(j, pos1, &hdr));
    assert_int_equal(0, jls_raw_rd(j, &hdr, sizeof(data), data));
    assert_memory_equal(PAYLOAD1, data, sizeof(PAYLOAD1));

    assert_int_equal(0, jls_raw_close(j));
    remove(filename);
}

static void test_items_nav(void **state) {
    (void) state;
    int64_t offset[2] = {0, 0};
    int64_t pos_cur = 0;
    int64_t pos_next = 0;

    struct jls_raw_s * j = NULL;
    struct jls_chunk_header_s hdr;
    struct jls_chunk_header_s hdr_prev;
    assert_int_equal(0, jls_raw_open(&j, filename, "w"));
    int64_t pos_start = jls_raw_chunk_tell(j);

    for (uint8_t i = 0; i < 4; ++i) {
        pos_cur = jls_raw_chunk_tell(j);
        hdr_set(&hdr, JLS_TAG_USER_DATA, i, 1);
        hdr.payload_prev_length = 1;
        hdr.item_prev = offset[i & 1];
        assert_int_equal(0, jls_raw_wr(j, &hdr, &i));
        pos_next = jls_raw_chunk_tell(j);
        if (hdr.item_prev) {
            jls_raw_chunk_seek(j, hdr.item_prev, &hdr_prev);
            hdr_prev.item_next = pos_cur;
            jls_raw_wr_header(j, &hdr_prev);
            jls_raw_chunk_seek(j, pos_next, NULL);
        }
    }
    assert_int_equal(0, jls_raw_chunk_seek(j, pos_start, &hdr));
    assert_int_equal(0, jls_raw_item_next(j, &hdr));
    assert_int_equal(2, hdr.chunk_meta);
    assert_int_equal(0, jls_raw_item_prev(j, &hdr));
    assert_int_equal(0, hdr.chunk_meta);
    assert_int_equal(JLS_ERROR_EMPTY, jls_raw_item_prev(j, &hdr));

    assert_int_equal(0, jls_raw_item_next(j, &hdr));
    assert_int_equal(0, jls_raw_item_next(j, &hdr));
    assert_int_equal(JLS_ERROR_EMPTY, jls_raw_item_next(j, &hdr));

    assert_int_equal(0, jls_raw_close(j));
    remove(filename);
}

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_invalid_open),
            cmocka_unit_test(test_open_write),
            cmocka_unit_test(test_one_chunk),
            cmocka_unit_test(test_n_chunks),
            cmocka_unit_test(test_chunks_nav),
            cmocka_unit_test(test_seek),
            cmocka_unit_test(test_items_nav),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
