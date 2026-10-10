/*
 * Copyright 2026 Jetperch LLC
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

/*
 * Writer path tests.  Written data is checked by parsing the raw chunks,
 * independent of the reader.  Caller buffers are exact-size heap
 * allocations so ASan flags any over-read.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include "jls/writer.h"
#include "jls/reader.h"
#include "jls/raw.h"
#include "jls/backend.h"
#include "jls/format.h"
#include "jls/ec.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char * filename = "writer_test_tmp.jls";

static const struct jls_source_def_s SOURCE_1 = {
        .source_id = 1,
        .name = "s",
        .vendor = "v",
        .model = "m",
        .version = "1",
        .serial_number = "n",
};

static struct jls_signal_def_s signal_def(uint32_t data_type) {
    struct jls_signal_def_s s = {
            .signal_id = 1,
            .source_id = 1,
            .signal_type = JLS_SIGNAL_TYPE_FSR,
            .data_type = data_type,
            .sample_rate = 1000,
            .name = "x",
            .units = "u",
    };
    return s;
}

static struct jls_wr_s * wr_open(const struct jls_signal_def_s * s) {
    struct jls_wr_s * wr = NULL;
    assert_int_equal(0, jls_wr_open(&wr, filename));
    assert_int_equal(0, jls_wr_source_def(wr, &SOURCE_1));
    assert_int_equal(0, jls_wr_signal_def(wr, s));
    return wr;
}

static uint32_t sample_get(const uint8_t * p, uint64_t idx, uint8_t bits) {
    uint32_t v = 0;
    for (uint8_t b = 0; b < bits; ++b) {
        uint64_t k = idx * bits + b;
        v |= ((uint32_t) ((p[k >> 3] >> (k & 7)) & 1)) << b;
    }
    return v;
}

static void sample_set(uint8_t * p, uint64_t idx, uint8_t bits, uint32_t v) {
    for (uint8_t b = 0; b < bits; ++b) {
        uint64_t k = idx * bits + b;
        if ((v >> b) & 1) {
            p[k >> 3] |= (uint8_t) (1 << (k & 7));
        } else {
            p[k >> 3] &= (uint8_t) ~(1 << (k & 7));
        }
    }
}

/// Pack samples [start, start + count) of value_fn into an exact-size heap buffer.
static uint8_t * pack(uint32_t (*value_fn)(uint64_t), uint64_t start, uint32_t count, uint8_t bits) {
    size_t sz = ((size_t) count * bits + 7) / 8;
    uint8_t * p = calloc(1, sz ? sz : 1);
    for (uint32_t i = 0; i < count; ++i) {
        sample_set(p, i, bits, value_fn(start + i));
    }
    return p;
}

/*
 * Reconstruct level 0 sample data for signal 1 from the raw data chunks.
 * Returns a buffer of length samples packed at bits per sample.
 */
static uint8_t * raw_fsr_read(uint8_t bits, int64_t * start, uint64_t * length) {
    struct jls_raw_s * raw = NULL;
    struct jls_chunk_header_s hdr;
    uint32_t payload_max = 1 << 20;
    uint8_t * payload = malloc(payload_max);
    uint8_t * out = NULL;
    uint64_t out_len = 0;
    int64_t out_start = 0;
    assert_int_equal(0, jls_raw_open(&raw, filename, "r"));
    while (0 == jls_raw_rd(raw, &hdr, payload_max, payload)) {
        if ((JLS_TAG_TRACK_FSR_DATA != hdr.tag) || (1 != hdr.chunk_meta)) {
            continue;
        }
        struct jls_fsr_data_s * d = (struct jls_fsr_data_s *) payload;
        assert_int_equal(bits, d->header.entry_size_bits);
        if (!out) {
            out_start = d->header.timestamp;
        }
        assert_int_equal(out_start + (int64_t) out_len, d->header.timestamp);  // contiguous
        uint64_t len_new = out_len + d->header.entry_count;
        out = realloc(out, (size_t) ((len_new * bits + 7) / 8 + 1));
        for (uint32_t i = 0; i < d->header.entry_count; ++i) {
            sample_set(out, out_len + i, bits, sample_get((uint8_t *) d->data, i, bits));
        }
        out_len = len_new;
    }
    jls_raw_close(raw);
    free(payload);
    *start = out_start;
    *length = out_len;
    return out;
}

static uint32_t value_u1(uint64_t i) {
    return (uint32_t) (((i * 7) ^ (i >> 3)) & 1);
}

static uint32_t value_u4(uint64_t i) {
    return (uint32_t) ((i * 5 + 3) & 0xf);
}

static uint32_t value_u8(uint64_t i) {
    return (uint32_t) ((i * 37 + 11) & 0xff);
}

static uint32_t value_u16(uint64_t i) {
    return (uint32_t) ((i * 7919 + 1) & 0xffff);
}

static void check_samples(uint32_t (*value_fn)(uint64_t), uint8_t bits, int64_t start_expect,
                          uint64_t length_expect) {
    int64_t start = 0;
    uint64_t length = 0;
    uint8_t * out = raw_fsr_read(bits, &start, &length);
    assert_int_equal(start_expect, start);
    assert_int_equal(length_expect, length);
    for (uint64_t i = 0; i < length; ++i) {
        uint32_t expect = value_fn((uint64_t) (start + (int64_t) i));
        if (expect != sample_get(out, i, bits)) {
            fail_msg("sample %" PRIu64 ": expect %u, got %u", i, expect, sample_get(out, i, bits));
        }
    }
    free(out);
}

// W8, W9: sub-byte tails survive close; split writes never over-read.
static void sub_byte_split(uint32_t data_type, uint32_t (*value_fn)(uint64_t), uint32_t n_max) {
    uint8_t bits = (uint8_t) (data_type >> 8);
    for (int64_t start = 0; start < 2; ++start) {
        for (uint32_t n = 1; n <= n_max; ++n) {
            for (uint32_t split = 0; split <= n; ++split) {
                struct jls_signal_def_s s = signal_def(data_type);
                struct jls_wr_s * wr = wr_open(&s);
                uint8_t * a = pack(value_fn, start, split, bits);
                uint8_t * b = pack(value_fn, start + split, n - split, bits);
                assert_int_equal(0, jls_wr_fsr(wr, 1, start, a, split));
                assert_int_equal(0, jls_wr_fsr(wr, 1, start + split, b, n - split));
                free(a);
                free(b);
                assert_int_equal(0, jls_wr_close(wr));
                check_samples(value_fn, bits, start, n);
            }
        }
    }
    remove(filename);
}

static void test_u1_tail_and_split(void **state) {
    (void) state;
    sub_byte_split(JLS_DATATYPE_U1, value_u1, 17);
}

static void test_u4_tail_and_split(void **state) {
    (void) state;
    sub_byte_split(JLS_DATATYPE_U4, value_u4, 5);
}

// Odd-sized writes that cross several data chunks.
static void chunked(uint32_t data_type, uint32_t (*value_fn)(uint64_t)) {
    uint8_t bits = (uint8_t) (data_type >> 8);
    struct jls_signal_def_s s = signal_def(data_type);
    s.samples_per_data = 1024;
    struct jls_wr_s * wr = wr_open(&s);
    uint64_t sample_id = 3;
    uint32_t sizes[] = {1, 7, 13, 1000, 3, 2049, 5, 333, 1};
    for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); ++k) {
        uint8_t * p = pack(value_fn, sample_id, sizes[k], bits);
        assert_int_equal(0, jls_wr_fsr(wr, 1, (int64_t) sample_id, p, sizes[k]));
        free(p);
        sample_id += sizes[k];
    }
    assert_int_equal(0, jls_wr_close(wr));
    check_samples(value_fn, bits, 3, sample_id - 3);
    remove(filename);
}

static void test_chunked(void **state) {
    (void) state;
    chunked(JLS_DATATYPE_U1, value_u1);
    chunked(JLS_DATATYPE_U4, value_u4);
    chunked(JLS_DATATYPE_U8, value_u8);
    chunked(JLS_DATATYPE_U16, value_u16);
}

// W2, W10: duplicate (overlapping) writes append only the new samples.
static void duplicates(uint32_t data_type, uint32_t (*value_fn)(uint64_t)) {
    uint8_t bits = (uint8_t) (data_type >> 8);
    for (uint32_t n1 = 1; n1 < 20; n1 += 3) {
        for (uint32_t ffwd = 1; ffwd <= n1; ++ffwd) {
            for (uint32_t n2 = 1; n2 < 20; n2 += 4) {
                struct jls_signal_def_s s = signal_def(data_type);
                struct jls_wr_s * wr = wr_open(&s);
                uint8_t * a = pack(value_fn, 0, n1, bits);
                int64_t s2 = (int64_t) n1 - ffwd;
                uint8_t * b = pack(value_fn, (uint64_t) s2, n2, bits);
                assert_int_equal(0, jls_wr_fsr(wr, 1, 0, a, n1));
                assert_int_equal(0, jls_wr_fsr(wr, 1, s2, b, n2));
                free(a);
                free(b);
                assert_int_equal(0, jls_wr_close(wr));
                uint64_t end = ((s2 + n2) > n1) ? (uint64_t) (s2 + n2) : n1;
                check_samples(value_fn, bits, 0, end);
            }
        }
    }
    remove(filename);
}

static void test_duplicates(void **state) {
    (void) state;
    duplicates(JLS_DATATYPE_U1, value_u1);
    duplicates(JLS_DATATYPE_U4, value_u4);
    duplicates(JLS_DATATYPE_U8, value_u8);
    duplicates(JLS_DATATYPE_U16, value_u16);
}

static uint32_t gap_value;  // 0 inside the skipped range

static uint32_t value_gap_u16(uint64_t i) {
    return ((i >= 4) && (i < (4 + gap_value))) ? 0 : value_u16(i);
}

static uint32_t value_gap_u1(uint64_t i) {
    return ((i >= 4) && (i < (4 + gap_value))) ? 0 : value_u1(i);
}

// W3: skipped samples are zero filled, never copied from out of bounds.
static void skip_check(uint32_t data_type, uint32_t (*value_fn)(uint64_t), uint32_t (*expect_fn)(uint64_t)) {
    uint8_t bits = (uint8_t) (data_type >> 8);
    uint32_t gaps[] = {1, 7, 20000, 100003};
    for (size_t k = 0; k < sizeof(gaps) / sizeof(gaps[0]); ++k) {
        gap_value = gaps[k];
        struct jls_signal_def_s s = signal_def(data_type);
        struct jls_wr_s * wr = wr_open(&s);
        uint8_t * a = pack(value_fn, 0, 4, bits);
        uint8_t * b = pack(value_fn, 4 + gap_value, 9, bits);
        assert_int_equal(0, jls_wr_fsr(wr, 1, 0, a, 4));
        assert_int_equal(0, jls_wr_fsr(wr, 1, 4 + gap_value, b, 9));
        free(a);
        free(b);
        assert_int_equal(0, jls_wr_close(wr));
        check_samples(expect_fn, bits, 0, 4 + gap_value + 9);
    }
    remove(filename);
}

static void test_skip(void **state) {
    (void) state;
    skip_check(JLS_DATATYPE_U16, value_u16, value_gap_u16);
    skip_check(JLS_DATATYPE_U1, value_u1, value_gap_u1);
}

static void test_skip_f32_nan(void **state) {
    (void) state;
    struct jls_signal_def_s s = signal_def(JLS_DATATYPE_F32);
    struct jls_wr_s * wr = wr_open(&s);
    float * d = malloc(4 * sizeof(float));
    for (int i = 0; i < 4; ++i) {
        d[i] = (float) (i + 1);
    }
    assert_int_equal(0, jls_wr_fsr_f32(wr, 1, 0, d, 4));
    assert_int_equal(0, jls_wr_fsr_f32(wr, 1, 10004, d, 4));
    assert_int_equal(0, jls_wr_close(wr));
    free(d);

    int64_t start = 0;
    uint64_t length = 0;
    float * out = (float *) raw_fsr_read(32, &start, &length);
    assert_int_equal(10008, length);
    assert_true(1.0f == out[0]);
    for (int i = 4; i < 10004; ++i) {
        assert_true(isnan(out[i]));
    }
    assert_true(4.0f == out[10007]);
    free(out);
    remove(filename);
}

// A huge forward skip is rejected without latching an error.
static void test_skip_too_big(void **state) {
    (void) state;
    struct jls_signal_def_s s = signal_def(JLS_DATATYPE_F32);
    struct jls_wr_s * wr = wr_open(&s);
    float d[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    assert_int_equal(0, jls_wr_fsr_f32(wr, 1, 0, d, 4));
    assert_int_equal(JLS_ERROR_TOO_BIG, jls_wr_fsr_f32(wr, 1, 4 + (1LL << 40), d, 4));
    assert_int_equal(0, jls_wr_fsr_f32(wr, 1, 4, d, 4));
    assert_int_equal(0, jls_wr_close(wr));
    remove(filename);
}

// D3: the sample buffer size does not wrap in 32 bits.
static void test_f64_large_samples_per_data(void **state) {
    (void) state;
    struct jls_signal_def_s s = signal_def(JLS_DATATYPE_F64);
    s.samples_per_data = 1U << 26;
    s.sample_decimate_factor = 1U << 20;
    s.entries_per_summary = 64;
    s.summary_decimate_factor = 16;
    struct jls_wr_s * wr = NULL;
    assert_int_equal(0, jls_wr_open(&wr, filename));
    assert_int_equal(0, jls_wr_source_def(wr, &SOURCE_1));
    if (0 == jls_wr_signal_def(wr, &s)) {  // structural caps may reject this
        double * d = calloc(64, sizeof(double));
        int32_t rc = jls_wr_fsr(wr, 1, 0, d, 64);
        assert_true((0 == rc) || (JLS_ERROR_NOT_ENOUGH_MEMORY == rc));
        free(d);
    }
    jls_wr_close(wr);
    remove(filename);
}

// W18: NULL name and units are stored as empty strings.
static void test_signal_def_null_strings(void **state) {
    (void) state;
    struct jls_signal_def_s s = signal_def(JLS_DATATYPE_F32);
    s.name = NULL;
    s.units = NULL;
    struct jls_wr_s * wr = wr_open(&s);
    assert_int_equal(0, jls_wr_close(wr));

    struct jls_rd_s * rd = NULL;
    struct jls_signal_def_s s_rd;
    assert_int_equal(0, jls_rd_open(&rd, filename));
    assert_int_equal(0, jls_rd_signal(rd, 1, &s_rd));
    assert_string_equal("", s_rd.name);
    assert_string_equal("", s_rd.units);
    jls_rd_close(rd);
    remove(filename);
}

// W11: storage_type must be defined; chunk_meta[15:12] is reserved.
static void test_payload_validation(void **state) {
    (void) state;
    struct jls_signal_def_s s = signal_def(JLS_DATATYPE_F32);
    struct jls_wr_s * wr = wr_open(&s);
    const uint8_t str[] = "hi";
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID,
                     jls_wr_user_data(wr, 0x1000, JLS_STORAGE_TYPE_BINARY, str, 2));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID,
                     jls_wr_user_data(wr, 1, (enum jls_storage_type_e) 0x102, str, 2));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID,
                     jls_wr_annotation(wr, 1, 0, 0.0f, JLS_ANNOTATION_TYPE_TEXT, 0,
                                       (enum jls_storage_type_e) 0x102, str, 0));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID,
                     jls_wr_annotation(wr, 1, 0, 0.0f, JLS_ANNOTATION_TYPE_TEXT, 0,
                                       JLS_STORAGE_TYPE_INVALID, NULL, 0));
    // validation errors do not latch
    assert_int_equal(0, jls_wr_user_data(wr, 0x0fff, JLS_STORAGE_TYPE_BINARY, str, 2));
    assert_int_equal(0, jls_wr_close(wr));
    remove(filename);
}

static int32_t count_cbk(void * user_data, const struct jls_utc_summary_entry_s * utc, uint32_t size) {
    (void) utc;
    *((uint32_t *) user_data) += size;
    return 0;
}

// D4, W14: reaching the top timeseries level is not an error.
static void test_utc_top_level(void **state) {
    (void) state;
    struct jls_signal_def_s s = signal_def(JLS_DATATYPE_F32);
    s.utc_decimate_factor = 2;  // may be clamped up by signal_def_align
    struct jls_wr_s * wr = wr_open(&s);
    const uint32_t count = 70000;
    for (uint32_t i = 0; i < count; ++i) {
        assert_int_equal(0, jls_wr_utc(wr, 1, i, i));
    }
    assert_int_equal(0, jls_wr_close(wr));

    struct jls_rd_s * rd = NULL;
    uint32_t total = 0;
    assert_int_equal(0, jls_rd_open(&rd, filename));
    assert_int_equal(0, jls_rd_utc(rd, 1, 0, count_cbk, &total));
    assert_int_equal(count, total);
    jls_rd_close(rd);
    remove(filename);
}

/// Count the entries in the raw summary and index chunks for a track level.
static void raw_ts_counts(uint8_t tag_index, uint8_t tag_summary, uint8_t level,
                          uint32_t * index_entries, uint32_t * summary_entries) {
    struct jls_raw_s * raw = NULL;
    struct jls_chunk_header_s hdr;
    uint32_t payload_max = 1 << 20;
    uint8_t * payload = malloc(payload_max);
    uint16_t meta = (uint16_t) (1 | (level << 12));
    *index_entries = 0;
    *summary_entries = 0;
    assert_int_equal(0, jls_raw_open(&raw, filename, "r"));
    while (0 == jls_raw_rd(raw, &hdr, payload_max, payload)) {
        struct jls_payload_header_s * p = (struct jls_payload_header_s *) payload;
        if ((tag_index == hdr.tag) && (meta == hdr.chunk_meta)) {
            *index_entries += p->entry_count;
        } else if ((tag_summary == hdr.tag) && (meta == hdr.chunk_meta)) {
            *summary_entries += p->entry_count;
        }
    }
    jls_raw_close(raw);
    free(payload);
}

// W14: close adds to both the parent index and summary.
static void test_anno_close_counts(void **state) {
    (void) state;
    struct jls_signal_def_s s = signal_def(JLS_DATATYPE_F32);
    s.annotation_decimate_factor = 10;
    struct jls_wr_s * wr = wr_open(&s);
    for (int i = 0; i < 15; ++i) {
        assert_int_equal(0, jls_wr_annotation(wr, 1, i, 0.0f, JLS_ANNOTATION_TYPE_VERTICAL_MARKER, 0,
                                              JLS_STORAGE_TYPE_STRING, (const uint8_t *) "a", 0));
    }
    assert_int_equal(0, jls_wr_close(wr));
    uint32_t index_entries = 0;
    uint32_t summary_entries = 0;
    raw_ts_counts(JLS_TAG_TRACK_ANNOTATION_INDEX, JLS_TAG_TRACK_ANNOTATION_SUMMARY, 2,
                  &index_entries, &summary_entries);
    assert_int_equal(2, index_entries);
    assert_int_equal(index_entries, summary_entries);
    remove(filename);
}

// W13: summary std is finite wherever the mean is finite, after a NaN gap.
static void test_summary_std_after_gap(void **state) {
    (void) state;
    struct jls_signal_def_s s = signal_def(JLS_DATATYPE_F32);
    s.samples_per_data = 1024;
    s.sample_decimate_factor = 128;
    s.entries_per_summary = 40;
    s.summary_decimate_factor = 10;
    struct jls_wr_s * wr = wr_open(&s);
    const uint32_t n = 100000;
    float * d = malloc(n * sizeof(float));
    for (uint32_t i = 0; i < n; ++i) {
        d[i] = (float) (i & 7);
    }
    assert_int_equal(0, jls_wr_fsr_f32(wr, 1, 0, d, 1000));
    assert_int_equal(0, jls_wr_fsr_f32(wr, 1, 1500, d, n));  // 500 sample NaN gap
    assert_int_equal(0, jls_wr_close(wr));
    free(d);

    struct jls_raw_s * raw = NULL;
    struct jls_chunk_header_s hdr;
    uint32_t payload_max = 1 << 20;
    uint8_t * payload = malloc(payload_max);
    uint32_t checked = 0;
    assert_int_equal(0, jls_raw_open(&raw, filename, "r"));
    while (0 == jls_raw_rd(raw, &hdr, payload_max, payload)) {
        if ((JLS_TAG_TRACK_FSR_SUMMARY != hdr.tag) || (1 != (hdr.chunk_meta & 0x0fff))
                || ((hdr.chunk_meta >> 12) < 2)) {
            continue;
        }
        struct jls_fsr_f32_summary_s * p = (struct jls_fsr_f32_summary_s *) payload;
        for (uint32_t i = 0; i < p->header.entry_count; ++i) {
            if (isfinite(p->data[i][JLS_SUMMARY_FSR_MEAN])) {
                assert_true(isfinite(p->data[i][JLS_SUMMARY_FSR_STD]));
                ++checked;
            }
        }
    }
    jls_raw_close(raw);
    free(payload);
    assert_true(checked > 0);
    remove(filename);
}

// W6: a signal_def IO error latches, so retry and writes cannot crash.
static void test_signal_def_io_error(void **state) {
    (void) state;
    for (int64_t limit = 0; limit < 2000; limit += 16) {
        struct jls_signal_def_s s = signal_def(JLS_DATATYPE_F32);
        struct jls_wr_s * wr = NULL;
        assert_int_equal(0, jls_wr_open(&wr, filename));
        assert_int_equal(0, jls_wr_source_def(wr, &SOURCE_1));
        jls_bk_write_limit_set(limit);
        int32_t rc = jls_wr_signal_def(wr, &s);
        jls_bk_write_limit_set(-1);
        float d[4] = {0};
        if (rc) {
            assert_int_equal(rc, jls_wr_signal_def(wr, &s));
            assert_int_equal(rc, jls_wr_fsr_f32(wr, 1, 0, d, 4));
            assert_int_equal(rc, jls_wr_close(wr));
        } else {
            assert_int_equal(0, jls_wr_fsr_f32(wr, 1, 0, d, 4));
            assert_int_equal(0, jls_wr_close(wr));
        }
    }
    remove(filename);
}

// W16: an open that fails after creating the file releases everything.
static void test_open_io_error(void **state) {
    (void) state;
    for (int64_t limit = 0; limit < 1024; limit += 32) {
        struct jls_wr_s * wr = NULL;
        jls_bk_write_limit_set(limit);
        int32_t rc = jls_wr_open(&wr, filename);
        jls_bk_write_limit_set(-1);
        if (rc) {
            assert_null(wr);
        } else {
            assert_int_equal(0, jls_wr_close(wr));
        }
    }
    remove(filename);
}

enum fault_mode_e {
    FAULT_FSR,
    FAULT_UTC,
    FAULT_ANNO,
    FAULT_U1,
};

static int32_t fault_write(struct jls_wr_s * wr, enum fault_mode_e mode, int i, const float * d) {
    switch (mode) {
        case FAULT_FSR: return jls_wr_fsr_f32(wr, 1, (int64_t) i * 256, d, 256);
        case FAULT_UTC: return jls_wr_utc(wr, 1, i, i);
        case FAULT_ANNO: return jls_wr_annotation(wr, 1, i, 0.0f, JLS_ANNOTATION_TYPE_VERTICAL_MARKER, 0,
                                                  JLS_STORAGE_TYPE_STRING, (const uint8_t *) "a", 0);
        case FAULT_U1: return jls_wr_fsr(wr, 2, (int64_t) i * 77, d, 77);
        default: return JLS_ERROR_PARAMETER_INVALID;
    }
}

/*
 * W1, W12: sweep a transient write failure through every write, then let
 * writes succeed again.  The first error latches: every later call and
 * close return it, and the in-memory buffers never overflow.
 */
static void io_fault_sweep(enum fault_mode_e mode, int64_t limit_max, int64_t limit_step) {
    struct jls_signal_def_s s = signal_def(JLS_DATATYPE_F32);
    s.samples_per_data = 1024;
    s.sample_decimate_factor = 128;
    s.entries_per_summary = 40;
    s.summary_decimate_factor = 10;
    s.utc_decimate_factor = 10;
    s.annotation_decimate_factor = 10;
    struct jls_signal_def_s s2 = signal_def(JLS_DATATYPE_U1);
    s2.signal_id = 2;
    s2.samples_per_data = 512;
    s2.sample_decimate_factor = 256;
    s2.entries_per_summary = 20;
    s2.summary_decimate_factor = 10;
    float * d = calloc(256, sizeof(float));
    for (int64_t limit = 0; limit < limit_max; limit += limit_step) {
        struct jls_wr_s * wr = wr_open(&s);
        assert_int_equal(0, jls_wr_signal_def(wr, &s2));
        jls_bk_write_limit_set(limit);
        int32_t err = 0;
        for (int i = 0; i < 3000; ++i) {
            int32_t rc = fault_write(wr, mode, i, d);
            if (err) {
                assert_int_equal(err, rc);
            } else if (rc) {
                err = rc;
                jls_bk_write_limit_set(-1);  // space freed: later writes would succeed
                assert_int_equal(err, jls_wr_flush(wr));
            }
        }
        jls_bk_write_limit_set(-1);
        int32_t rc = jls_wr_close(wr);
        if (err) {
            assert_int_equal(err, rc);
        }
    }
    free(d);
    remove(filename);
}

static void test_io_fault_fsr(void **state) {
    (void) state;
    io_fault_sweep(FAULT_FSR, 400000, 997);
}

static void test_io_fault_utc(void **state) {
    (void) state;
    io_fault_sweep(FAULT_UTC, 200000, 499);
}

static void test_io_fault_anno(void **state) {
    (void) state;
    io_fault_sweep(FAULT_ANNO, 250000, 499);
}

static void test_io_fault_u1(void **state) {
    (void) state;
    io_fault_sweep(FAULT_U1, 60000, 97);
}

// W12: a close-time write failure is returned.
static void test_close_io_error(void **state) {
    (void) state;
    struct jls_signal_def_s s = signal_def(JLS_DATATYPE_F32);
    struct jls_wr_s * wr = wr_open(&s);
    float d[100] = {0};
    assert_int_equal(0, jls_wr_fsr_f32(wr, 1, 0, d, 100));  // buffered, written on close
    jls_bk_write_limit_set(0);
    int32_t rc = jls_wr_close(wr);
    jls_bk_write_limit_set(-1);
    assert_int_equal(JLS_ERROR_IO, rc);
    remove(filename);
}

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_u1_tail_and_split),
            cmocka_unit_test(test_u4_tail_and_split),
            cmocka_unit_test(test_chunked),
            cmocka_unit_test(test_duplicates),
            cmocka_unit_test(test_skip),
            cmocka_unit_test(test_skip_f32_nan),
            cmocka_unit_test(test_skip_too_big),
            cmocka_unit_test(test_f64_large_samples_per_data),
            cmocka_unit_test(test_signal_def_null_strings),
            cmocka_unit_test(test_payload_validation),
            cmocka_unit_test(test_utc_top_level),
            cmocka_unit_test(test_anno_close_counts),
            cmocka_unit_test(test_summary_std_after_gap),
            cmocka_unit_test(test_signal_def_io_error),
            cmocka_unit_test(test_open_io_error),
            cmocka_unit_test(test_io_fault_fsr),
            cmocka_unit_test(test_io_fault_utc),
            cmocka_unit_test(test_io_fault_anno),
            cmocka_unit_test(test_io_fault_u1),
            cmocka_unit_test(test_close_io_error),
    };

    const char * filter = getenv("JLS_TEST_FILTER");  // run a subset, such as for negative controls
    if (filter) {
        cmocka_set_test_filter(filter);
    }
    return cmocka_run_group_tests(tests, NULL, NULL);
}
