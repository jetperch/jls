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
#include "jls/raw.h"
#include "jls/backend.h"
#include "jls/format.h"
#include "jls/ec.h"
#include <stdio.h>
#include <string.h>


const char * filename = "raw_test_tmp.jls";
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
    assert_int_equal(JLS_FORMAT_VERSION_U32, jls_raw_version(j).u32);
    assert_int_equal(32, jls_bk_ftell(jls_raw_backend(j)));
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

static void test_read_accounting(void **state) {
    (void) state;
    struct jls_raw_s * j = NULL;
    struct jls_chunk_header_s hdr;
    uint8_t data[sizeof(PAYLOAD1) + 16];
    assert_int_equal(0, jls_raw_open(&j, filename, "w"));
    assert_int_equal(0, jls_raw_wr(j, hdr_set(&hdr, JLS_TAG_USER_DATA, 0, sizeof(PAYLOAD1)), PAYLOAD1));
    assert_int_equal(0, jls_raw_close(j));

    int64_t hdr_total = jls_raw_rd_header_total();
    int64_t payload_total = jls_raw_rd_payload_total();
    assert_int_equal(0, jls_raw_open(&j, filename, "r"));
    assert_int_equal(0, jls_raw_rd(j, &hdr, sizeof(data), data));
    assert_int_equal(0, jls_raw_close(j));
    assert_int_equal(hdr_total + 1, jls_raw_rd_header_total());
    // payload bytes are read rounded up to the on-disk size including CRC
    assert_true(jls_raw_rd_payload_total() >= (payload_total + (int64_t) sizeof(PAYLOAD1)));
    remove(filename);
}

static void construct_n_chunks(void) {
    struct jls_raw_s * j = NULL;
    struct jls_chunk_header_s hdr;
    assert_int_equal(0, jls_raw_open(&j, filename, "w"));
    for (size_t i = 0; i < sizeof(PAYLOAD1); ++i) {
        // printf("chunk %d: %d\n", i, (int32_t) jls_raw_chunk_tell(j));
        hdr_set(&hdr, JLS_TAG_USER_DATA, 0, (uint32_t) (sizeof(PAYLOAD1) - i));
        assert_int_equal(0, jls_raw_wr(j, &hdr, PAYLOAD1 + i));
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
    for (size_t i = 0; i < sizeof(PAYLOAD1); ++i) {
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
    assert_int_equal(0, jls_raw_chunk_next(j));
    assert_int_equal(0, jls_raw_rd(j, &hdr, sizeof(data), data));
    assert_memory_equal(PAYLOAD1 + 1, data, sizeof(PAYLOAD1) - 1);

    assert_int_equal(0, jls_raw_chunk_prev(j));
    assert_int_equal(0, jls_raw_chunk_prev(j));
    assert_int_equal(0, jls_raw_rd(j, &hdr, sizeof(data), data));
    assert_memory_equal(PAYLOAD1, data, sizeof(PAYLOAD1));
    assert_int_equal(0, jls_raw_chunk_prev(j));
    assert_int_equal(JLS_ERROR_EMPTY, jls_raw_chunk_prev(j));

    assert_int_equal(0, jls_raw_open(&j, filename, "r"));
    for (size_t i = 0; i < sizeof(PAYLOAD1) - 1; ++i) {
        assert_int_equal(0, jls_raw_chunk_next(j));
    }
    assert_int_equal(0, jls_raw_rd(j, &hdr, sizeof(data), data));
    assert_memory_equal(PAYLOAD1 + sizeof(PAYLOAD1) - 1, data, 1);
    assert_int_equal(JLS_ERROR_EMPTY, jls_raw_chunk_next(j));
    assert_int_equal(JLS_ERROR_EMPTY, jls_raw_chunk_next(j));
    assert_int_equal(JLS_ERROR_NOT_FOUND, jls_raw_chunk_prev(j));

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

    assert_int_equal(0, jls_raw_chunk_next(j));
    int64_t pos2 = jls_raw_chunk_tell(j);
    assert_int_not_equal(pos1, pos2);

    assert_int_equal(0, jls_raw_chunk_seek(j, pos1));
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
        hdr.chunk_meta = i + 1;
        offset[i & 1] = pos_cur;
        assert_int_equal(0, jls_raw_wr(j, &hdr, &i));
        pos_next = jls_raw_chunk_tell(j);
        if (hdr.item_prev) {
            jls_raw_chunk_seek(j, hdr.item_prev);
            jls_raw_rd_header(j, &hdr_prev);
            hdr_prev.item_next = pos_cur;
            jls_raw_wr_header(j, &hdr_prev);
            jls_raw_chunk_seek(j, pos_next);
        }
    }
    assert_int_equal(0, jls_raw_chunk_seek(j, pos_start));
    assert_int_equal(0, jls_raw_item_next(j));
    jls_raw_rd_header(j, &hdr);
    assert_int_equal(3, hdr.chunk_meta);
    assert_int_equal(0, jls_raw_item_prev(j));
    jls_raw_rd_header(j, &hdr);
    assert_int_equal(1, hdr.chunk_meta);
    assert_int_equal(JLS_ERROR_EMPTY, jls_raw_item_prev(j));

    assert_int_equal(0, jls_raw_item_next(j));
    assert_int_equal(JLS_ERROR_EMPTY, jls_raw_item_next(j));
    assert_int_equal(JLS_ERROR_EMPTY, jls_raw_item_next(j));

    assert_int_equal(0, jls_raw_close(j));
    remove(filename);
}

static void test_tag_to_name(void **state) {
    (void) state;
    assert_string_equal("end", jls_tag_to_name(JLS_TAG_END));
    assert_string_equal("invalid", jls_tag_to_name(JLS_TAG_INVALID));
}

static void test_chunks_scan(void **state) {
    (void) state;

    struct jls_raw_s *j = NULL;
    construct_n_chunks();

    assert_int_equal(0, jls_raw_open(&j, filename, "r"));
    assert_int_equal(0, jls_raw_chunk_next(j));
    int64_t pos1 = jls_raw_chunk_tell(j);
    assert_int_equal(0, jls_raw_chunk_next(j));
    int64_t pos2 = jls_raw_chunk_tell(j);
    assert_int_equal(0, jls_raw_chunk_seek(j, pos1 + 9));
    assert_int_equal(0, jls_raw_chunk_scan(j));
    assert_int_equal(pos2, jls_raw_chunk_tell(j));

    assert_int_equal(0, jls_raw_close(j));
    remove(filename);
}

// Write a lone chunk header (valid CRC) followed by pad_bytes of zeros.
static void construct_hdr_only(uint32_t payload_length, uint32_t pad_bytes) {
    struct jls_raw_s * j = NULL;
    struct jls_chunk_header_s hdr;
    uint8_t zeros[256];
    memset(zeros, 0, sizeof(zeros));
    assert_true(pad_bytes <= sizeof(zeros));
    assert_int_equal(0, jls_raw_open(&j, filename, "w"));
    assert_int_equal(0, jls_raw_wr_header(j, hdr_set(&hdr, JLS_TAG_USER_DATA, 0, payload_length)));
    if (pad_bytes) {
        assert_int_equal(0, jls_bk_fwrite(jls_raw_backend(j), zeros, pad_bytes));
    }
    assert_int_equal(0, jls_raw_close(j));
}

static void test_payload_size_on_disk(void **state) {
    (void) state;
    assert_int_equal(0, jls_raw_payload_size_on_disk(0));
    assert_int_equal(8, jls_raw_payload_size_on_disk(1));
    assert_int_equal(8, jls_raw_payload_size_on_disk(4));
    assert_int_equal(16, jls_raw_payload_size_on_disk(5));
    assert_int_equal(0x100000000LL, jls_raw_payload_size_on_disk(0xFFFFFFF5U));
    assert_int_equal(0x100000000LL, jls_raw_payload_size_on_disk(0xFFFFFFFCU));
    assert_int_equal(0x100000008LL, jls_raw_payload_size_on_disk(0xFFFFFFFDU));
    assert_int_equal(0x100000008LL, jls_raw_payload_size_on_disk(0xFFFFFFFFU));
}

static void test_payload_length_wrap(void **state) {
    (void) state;
    struct jls_raw_s * j = NULL;
    struct jls_chunk_header_s hdr;
    static uint8_t data[1 << 20];
    for (uint32_t length = 0xFFFFFFF5U; length != 0; ++length) {
        construct_hdr_only(length, 64);
        assert_int_equal(0, jls_raw_open(&j, filename, "r"));
        assert_int_equal(JLS_ERROR_TRUNCATED, jls_raw_rd(j, &hdr, sizeof(data), data));
        assert_int_equal(0, jls_raw_close(j));
    }
    remove(filename);
}

static void test_payload_past_file_end(void **state) {
    (void) state;
    struct jls_raw_s * j = NULL;
    struct jls_chunk_header_s hdr;
    uint8_t data[64];
    construct_hdr_only(4096, 64);  // torn: header valid, payload missing
    assert_int_equal(0, jls_raw_open(&j, filename, "r"));
    assert_int_equal(JLS_ERROR_TRUNCATED, jls_raw_rd_header(j, &hdr));
    assert_int_equal(JLS_ERROR_TRUNCATED, jls_raw_rd(j, &hdr, sizeof(data), data));
    assert_int_equal(0, jls_raw_close(j));
    remove(filename);
}

static void test_rd_too_big_retry(void **state) {
    (void) state;
    struct jls_raw_s * j = NULL;
    struct jls_chunk_header_s hdr;
    uint8_t data[sizeof(PAYLOAD1) + 8];
    assert_int_equal(0, jls_raw_open(&j, filename, "w"));
    assert_int_equal(0, jls_raw_wr(j, hdr_set(&hdr, JLS_TAG_USER_DATA, 0, sizeof(PAYLOAD1)), PAYLOAD1));
    assert_int_equal(0, jls_raw_close(j));

    assert_int_equal(0, jls_raw_open(&j, filename, "r"));
    // the payload fits, but the CRC and padding do not
    assert_int_equal(JLS_ERROR_TOO_BIG, jls_raw_rd(j, &hdr, sizeof(PAYLOAD1), data));
    assert_int_equal(sizeof(PAYLOAD1), hdr.payload_length);
    uint32_t sz = (uint32_t) jls_raw_payload_size_on_disk(hdr.payload_length);
    assert_true(sz <= sizeof(data));
    assert_int_equal(0, jls_raw_rd_payload(j, sz, data));
    assert_memory_equal(PAYLOAD1, data, sizeof(PAYLOAD1));
    assert_int_equal(0, jls_raw_close(j));
    remove(filename);
}

static void test_chunks_scan_window(void **state) {
    (void) state;
    struct jls_raw_s * j = NULL;
    struct jls_chunk_header_s hdr;
    uint8_t garbage[8192];
    memset(garbage, 0xA5, sizeof(garbage));
    assert_int_equal(0, jls_raw_open(&j, filename, "w"));
    // the valid chunk starts 4104 bytes in: past the first 4096-byte scan window
    assert_int_equal(0, jls_bk_fwrite(jls_raw_backend(j), garbage, 4096 + 8));
    assert_int_equal(0, jls_raw_seek_end(j));
    int64_t target = jls_raw_chunk_tell(j);
    assert_int_equal(0, jls_raw_wr(j, hdr_set(&hdr, JLS_TAG_USER_DATA, 0, sizeof(PAYLOAD1)), PAYLOAD1));
    assert_int_equal(0, jls_bk_fwrite(jls_raw_backend(j), garbage, sizeof(garbage)));
    assert_int_equal(0, jls_raw_close(j));

    assert_int_equal(0, jls_raw_open(&j, filename, "r"));
    assert_int_equal(0, jls_raw_chunk_seek(j, sizeof(struct jls_file_header_s) + 1));
    assert_int_equal(0, jls_raw_chunk_scan(j));
    assert_int_equal(target, jls_raw_chunk_tell(j));
    assert_int_equal(0, jls_raw_close(j));
    remove(filename);
}

static void test_chunks_scan_short_tail(void **state) {
    (void) state;
    struct jls_raw_s * j = NULL;
    construct_hdr_only(8, 16);  // chunk header + 16-byte torn payload
    assert_int_equal(0, jls_raw_open(&j, filename, "r"));
    // 24 bytes remain: shorter than a chunk header
    assert_int_equal(0, jls_raw_chunk_seek(j, sizeof(struct jls_file_header_s) + 24));
    assert_int_equal(JLS_ERROR_NOT_FOUND, jls_raw_chunk_scan(j));
    // 40 bytes remain, no valid header
    assert_int_equal(0, jls_raw_chunk_seek(j, sizeof(struct jls_file_header_s) + 8));
    assert_int_equal(JLS_ERROR_NOT_FOUND, jls_raw_chunk_scan(j));
    assert_int_equal(0, jls_raw_close(j));
    remove(filename);
}

static void test_wr_payload_length_mismatch(void **state) {
    (void) state;
    struct jls_raw_s * j = NULL;
    struct jls_chunk_header_s hdr;
    uint8_t data[sizeof(PAYLOAD1) + 16];
    uint8_t payload2[sizeof(PAYLOAD1) + 8];
    memset(payload2, 0x77, sizeof(payload2));
    construct_n_chunks();

    assert_int_equal(0, jls_raw_open(&j, filename, "a"));
    int64_t pos1 = jls_raw_chunk_tell(j);
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_raw_wr_payload(j, sizeof(payload2), payload2));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_raw_wr_payload(j, 4, payload2));
    assert_int_equal(0, jls_raw_chunk_seek(j, pos1));
    assert_int_equal(0, jls_raw_wr_payload(j, sizeof(PAYLOAD1), payload2));  // in place
    assert_int_equal(0, jls_raw_close(j));

    assert_int_equal(0, jls_raw_open(&j, filename, "r"));
    assert_int_equal(0, jls_raw_rd(j, &hdr, sizeof(data), data));
    assert_memory_equal(payload2, data, sizeof(PAYLOAD1));
    for (size_t i = 1; i < sizeof(PAYLOAD1); ++i) {
        assert_int_equal(0, jls_raw_rd(j, &hdr, sizeof(data), data));  // successors intact
        assert_memory_equal(PAYLOAD1 + i, data, sizeof(PAYLOAD1) - i);
    }
    assert_int_equal(0, jls_raw_close(j));
    remove(filename);
}

static void test_open_not_jls(void **state) {
    (void) state;
    struct jls_raw_s * j = NULL;
    uint8_t zeros[4096];
    memset(zeros, 0, sizeof(zeros));
    FILE * f = fopen(filename, "wb");
    assert_non_null(f);
    assert_int_equal(sizeof(zeros), fwrite(zeros, 1, sizeof(zeros), f));
    fclose(f);
    assert_int_equal(JLS_ERROR_UNSUPPORTED_FILE, jls_raw_open(&j, filename, "r"));
    assert_null(j);
    assert_int_equal(JLS_ERROR_UNSUPPORTED_FILE, jls_raw_open(&j, filename, "a"));
    assert_null(j);

    f = fopen(filename, "wb");  // shorter than the file header
    assert_non_null(f);
    assert_int_equal(10, fwrite(zeros, 1, 10, f));
    fclose(f);
    assert_int_equal(JLS_ERROR_UNSUPPORTED_FILE, jls_raw_open(&j, filename, "r"));
    assert_null(j);
    remove(filename);
}

static void test_append_links_prev_length(void **state) {
    (void) state;
    struct jls_raw_s * j = NULL;
    struct jls_chunk_header_s hdr;
    uint8_t data[sizeof(PAYLOAD1) + 16];
    construct_n_chunks();

    // read the last chunk, as tail truncation recovery does, then append
    assert_int_equal(0, jls_raw_open(&j, filename, "a"));
    for (size_t i = 0; i < sizeof(PAYLOAD1) - 1; ++i) {
        assert_int_equal(0, jls_raw_chunk_next(j));
    }
    int64_t last = jls_raw_chunk_tell(j);
    assert_int_equal(0, jls_raw_rd(j, &hdr, sizeof(data), data));
    assert_int_equal(1, hdr.payload_length);
    assert_int_equal(0, jls_raw_seek_end(j));
    int64_t appended = jls_raw_chunk_tell(j);
    assert_int_equal(0, jls_raw_wr(j, hdr_set(&hdr, JLS_TAG_USER_DATA, 0, 0), NULL));
    assert_int_equal(0, jls_raw_close(j));

    assert_int_equal(0, jls_raw_open(&j, filename, "r"));
    assert_int_equal(0, jls_raw_chunk_seek(j, appended));
    assert_int_equal(0, jls_raw_rd_header(j, &hdr));
    assert_int_equal(1, hdr.payload_prev_length);
    assert_int_equal(0, jls_raw_chunk_seek(j, appended));
    assert_int_equal(0, jls_raw_chunk_prev(j));
    assert_int_equal(last, jls_raw_chunk_tell(j));
    assert_int_equal(0, jls_raw_close(j));

    // rewrite the (empty payload) last chunk header in place
    assert_int_equal(0, jls_raw_open(&j, filename, "a"));
    assert_int_equal(0, jls_raw_chunk_seek(j, appended));
    assert_int_equal(0, jls_raw_rd_header(j, &hdr));  // now at the file end
    hdr.item_next = 1234;
    assert_int_equal(0, jls_raw_wr_header(j, &hdr));
    assert_int_equal(0, jls_raw_close(j));

    assert_int_equal(0, jls_raw_open(&j, filename, "r"));
    assert_int_equal(0, jls_raw_chunk_seek(j, appended));
    assert_int_equal(0, jls_raw_rd_header(j, &hdr));
    assert_int_equal(1234, hdr.item_next);
    assert_int_equal(1, hdr.payload_prev_length);  // unchanged
    assert_int_equal(0, jls_raw_close(j));
    remove(filename);
}

static void test_close_and_flush_errors(void **state) {
    (void) state;
    struct jls_raw_s * j = NULL;
    assert_int_equal(0, jls_raw_open(&j, filename, "w"));
    jls_bk_write_limit_set(0);  // the file header update on close fails
    assert_int_equal(JLS_ERROR_IO, jls_raw_close(j));
    jls_bk_write_limit_set(-1);

    assert_int_equal(0, jls_raw_open(&j, filename, "w"));
    struct jls_bkf_s * bk = jls_raw_backend(j);
    int fd = bk->fd;
    bk->fd = -2;  // invalid descriptor: fsync fails
    assert_int_equal(JLS_ERROR_IO, jls_raw_flush(j));
    bk->fd = fd;
    assert_int_equal(0, jls_raw_flush(j));
    assert_int_equal(0, jls_raw_close(j));
    remove(filename);
}

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_invalid_open),
            cmocka_unit_test(test_open_write),
            cmocka_unit_test(test_one_chunk),
            cmocka_unit_test(test_read_accounting),
            cmocka_unit_test(test_n_chunks),
            cmocka_unit_test(test_chunks_nav),
            cmocka_unit_test(test_seek),
            cmocka_unit_test(test_items_nav),
            cmocka_unit_test(test_tag_to_name),
            cmocka_unit_test(test_chunks_scan),
            cmocka_unit_test(test_payload_size_on_disk),
            cmocka_unit_test(test_payload_length_wrap),
            cmocka_unit_test(test_payload_past_file_end),
            cmocka_unit_test(test_rd_too_big_retry),
            cmocka_unit_test(test_chunks_scan_window),
            cmocka_unit_test(test_chunks_scan_short_tail),
            cmocka_unit_test(test_wr_payload_length_mismatch),
            cmocka_unit_test(test_open_not_jls),
            cmocka_unit_test(test_append_links_prev_length),
            cmocka_unit_test(test_close_and_flush_errors),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
