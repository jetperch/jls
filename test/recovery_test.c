/*
 * Copyright 2021-2026 Jetperch LLC
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
 * Regression tests for crash recovery, from
 * doc/plans/completed/recovery_defects.md.
 * Each test targets findings F1-F9 with deterministic damage.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include "jls/backend.h"
#include "jls/core.h"
#include "jls/ec.h"
#include "jls/format.h"
#include "jls/raw.h"
#include "jls/time.h"
#include "jls/reader.h"
#include "jls/writer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <sys/stat.h>
#endif


static const char * SRC_PATH = "recovery_test_src.jls";
static const char * TST_PATH = "recovery_test_tmp.jls";

#define SAMPLE_COUNT (30000)
#define WINDOW_SIZE (937)
// samples per UTC entry; must not divide SAMPLE_COUNT into a multiple of
// utc_decimate_factor so an unsummarized UTC tail exists (F3)
#define UTC_PERIOD (97)

static const struct jls_source_def_s SOURCE_3 = {
        .source_id = 3,
        .name = "source 3",
        .vendor = "vendor",
        .model = "model",
        .version = "1.0.0",
        .serial_number = "0001",
};

static const struct jls_signal_def_s SIGNAL_5 = {
        .signal_id = 5,
        .source_id = 3,
        .signal_type = JLS_SIGNAL_TYPE_FSR,
        .data_type = JLS_DATATYPE_F32,
        .sample_rate = 100000,
        .samples_per_data = 1000,
        .sample_decimate_factor = 100,
        .entries_per_summary = 200,
        .summary_decimate_factor = 100,
        .annotation_decimate_factor = 100,
        .utc_decimate_factor = 100,
        .name = "current",
        .units = "A",
};

// small intervals so a run of fully-omitted level-1 index chunks is cheap
// to generate: 64 samples per data chunk, 640 samples per level-1 index
static const struct jls_signal_def_s SIGNAL_7 = {
        .signal_id = 7,
        .source_id = 3,
        .signal_type = JLS_SIGNAL_TYPE_FSR,
        .data_type = JLS_DATATYPE_U8,
        .sample_rate = 1000000,
        .samples_per_data = 64,
        .sample_decimate_factor = 32,
        .entries_per_summary = 20,
        .summary_decimate_factor = 10,
        .annotation_decimate_factor = 100,
        .utc_decimate_factor = 100,
        .name = "gpi",
        .units = "",
};

static const struct jls_signal_def_s SIGNAL_9 = {
        .signal_id = 9,
        .source_id = 3,
        .signal_type = JLS_SIGNAL_TYPE_FSR,
        .data_type = JLS_DATATYPE_I16,
        .sample_rate = 100000,
        .samples_per_data = 1000,
        .sample_decimate_factor = 100,
        .entries_per_summary = 200,
        .summary_decimate_factor = 100,
        .annotation_decimate_factor = 100,
        .utc_decimate_factor = 100,
        .name = "aux",
        .units = "",
};

// Release a crashed writer without touching its file: with no write budget,
// every close-time write fails, so jls_wr_close only frees memory.
static void wr_abandon(struct jls_wr_s * wr) {
    jls_bk_write_limit_set(0);
    (void) jls_wr_close(wr);
    jls_bk_write_limit_set(-1);
}

enum gen_close_e {
    GEN_CLOSE,
    GEN_SKIP_CLOSE,
};

static float * signal_f32_;     // generated signal 5 samples
static int16_t * signal_i16_;   // generated signal 9 samples

static void signals_generate(void) {
    if (NULL == signal_f32_) {
        signal_f32_ = malloc(sizeof(float) * SAMPLE_COUNT);
        signal_i16_ = malloc(sizeof(int16_t) * SAMPLE_COUNT);
        assert_non_null(signal_f32_);
        assert_non_null(signal_i16_);
        for (int64_t i = 0; i < SAMPLE_COUNT; ++i) {
            signal_f32_[i] = (float) ((i % 1000) - 500) / 500.0f;
            signal_i16_[i] = (int16_t) ((i * 7) & 0x7fff);
        }
    }
}

/**
 * @brief Generate the test file.
 *
 * @param path The output path.
 * @param secondary Also write signal 9.
 * @param ts_en Write UTC entries, annotations, and user data.
 * @param gen_close GEN_CLOSE or GEN_SKIP_CLOSE (simulated crash).
 * @param write_budget Backend write budget in bytes, or -1 for unlimited.
 *      When the budget runs out, remaining writes fail as in a crash.
 */
static void gen(const char * path, int secondary, int ts_en,
                enum gen_close_e gen_close, int64_t write_budget) {
    struct jls_wr_s * wr = NULL;
    signals_generate();
    jls_bk_write_limit_set(write_budget);
    if (jls_wr_open(&wr, path)) {
        goto exit;  // budget exhausted during open: file is a valid prefix
    }
    if (jls_wr_source_def(wr, &SOURCE_3)) { goto exit; }
    if (jls_wr_signal_def(wr, &SIGNAL_5)) { goto exit; }
    if (secondary && jls_wr_signal_def(wr, &SIGNAL_9)) { goto exit; }
    if (ts_en && jls_wr_user_data(wr, 0x21, JLS_STORAGE_TYPE_STRING,
                                  (const uint8_t *) "hello", 6)) { goto exit; }

    int64_t utc = JLS_TIME_YEAR;
    for (int64_t sample_id = 0; sample_id < SAMPLE_COUNT; sample_id += WINDOW_SIZE) {
        int64_t len = SAMPLE_COUNT - sample_id;
        if (len > WINDOW_SIZE) {
            len = WINDOW_SIZE;
        }
        if (jls_wr_fsr_f32(wr, 5, sample_id, signal_f32_ + sample_id, (uint32_t) len)) { goto exit; }
        if (secondary && jls_wr_fsr(wr, 9, sample_id, signal_i16_ + sample_id, (uint32_t) len)) { goto exit; }
        if (ts_en) {
            for (int64_t s = sample_id; s < (sample_id + len); ++s) {
                if (0 == (s % UTC_PERIOD)) {
                    if (jls_wr_utc(wr, 5, s,
                                   utc + JLS_COUNTER_TO_TIME(s, SIGNAL_5.sample_rate))) { goto exit; }
                }
                if (0 == (s % 5000)) {
                    if (jls_wr_annotation(wr, 5, s, 0.0f, JLS_ANNOTATION_TYPE_TEXT, 0,
                                          JLS_STORAGE_TYPE_STRING,
                                          (const uint8_t *) "marker", 7)) { goto exit; }
                }
            }
        }
    }

    if (gen_close == GEN_CLOSE) {
        assert_int_equal(0, jls_wr_close(wr));
        jls_bk_write_limit_set(-1);
        return;
    }

exit:
    wr_abandon(wr);  // simulated crash; NULL if open failed
}

static void file_copy_truncate(const char * src, const char * dst, int64_t length) {
    FILE * fs = fopen(src, "rb");
    FILE * fd = fopen(dst, "wb");
    uint8_t buf[4096];
    assert_non_null(fs);
    assert_non_null(fd);
    while (length > 0) {
        size_t sz = sizeof(buf);
        if ((int64_t) sz > length) {
            sz = (size_t) length;
        }
        sz = fread(buf, 1, sz, fs);
        if (0 == sz) {
            break;
        }
        assert_int_equal(sz, fwrite(buf, 1, sz, fd));
        length -= (int64_t) sz;
    }
    fclose(fs);
    fclose(fd);
}

static int64_t file_size(const char * path) {
    FILE * f = fopen(path, "rb");
    assert_non_null(f);
    fseek(f, 0, SEEK_END);
    int64_t sz = ftell(f);
    fclose(f);
    return sz;
}

struct scan_info_s {
    int64_t defs_end;           // end of the initial definition chunks
    int64_t data5_intact;       // signal 5 samples in chunks ending <= limit
    int64_t data5_first;        // offset of the first signal 5 data chunk
    int64_t utc_intact;         // signal 5 UTC entries in chunks ending <= limit
};

// Walk the physical chunks to find what a damaged copy must still recover.
static void scan_chunks(const char * path, int64_t limit, struct scan_info_s * info) {
    struct jls_raw_s * raw = NULL;
    struct jls_chunk_header_s hdr;
    static uint8_t payload[1U << 20];
    memset(info, 0, sizeof(*info));
    int32_t rc = jls_raw_open(&raw, path, "r");
    assert_true((0 == rc) || (JLS_ERROR_TRUNCATED == rc));
    while (1) {
        if (jls_raw_rd_header(raw, &hdr)) {
            break;
        }
        int64_t pos = jls_raw_chunk_tell(raw);
        int64_t pos_next = pos + sizeof(hdr);
        if (hdr.payload_length) {
            pos_next += ((hdr.payload_length + 4 + 7) / 8) * 8;
        }
        uint8_t track_chunk = hdr.tag & 7;
        if ((0 == info->defs_end)
                && ((hdr.tag & JLS_TRACK_TAG_FLAG)
                    ? ((JLS_TRACK_CHUNK_DATA == track_chunk) || (JLS_TRACK_CHUNK_INDEX == track_chunk)
                       || (JLS_TRACK_CHUNK_SUMMARY == track_chunk))
                    : (JLS_TAG_END == hdr.tag))) {
            info->defs_end = pos;
        }
        if (pos_next <= limit) {
            if ((JLS_TAG_TRACK_FSR_DATA == hdr.tag) && (5 == hdr.chunk_meta)) {
                if (0 == jls_raw_rd_payload(raw, sizeof(payload), payload)) {
                    struct jls_fsr_data_s * d = (struct jls_fsr_data_s *) payload;
                    info->data5_intact = d->header.timestamp + d->header.entry_count;
                    if (0 == info->data5_first) {
                        info->data5_first = pos;
                    }
                }
            } else if ((JLS_TAG_TRACK_UTC_DATA == hdr.tag) && (5 == hdr.chunk_meta)) {
                ++info->utc_intact;
            }
        }
        if (jls_raw_chunk_seek(raw, pos) || jls_raw_chunk_next(raw)) {
            break;
        }
    }
    jls_raw_close(raw);
}

struct count_s {
    int64_t count;
};

static int32_t on_utc(void * user_data, const struct jls_utc_summary_entry_s * utc, uint32_t size) {
    struct count_s * c = (struct count_s *) user_data;
    (void) utc;
    c->count += size;
    return 0;
}

static int32_t on_annotation(void * user_data, const struct jls_annotation_s * annotation) {
    struct count_s * c = (struct count_s *) user_data;
    (void) annotation;
    ++c->count;
    return 0;
}

static int32_t on_user_data(void * user_data, uint16_t chunk_meta, enum jls_storage_type_e storage_type,
                            uint8_t * data, uint32_t data_size) {
    struct count_s * c = (struct count_s *) user_data;
    (void) chunk_meta;
    (void) storage_type;
    (void) data;
    (void) data_size;
    ++c->count;
    return 0;
}

// Open the damaged copy and verify recovery: F4/F5/F6/F8 regression core.
static void verify_recovered(const char * path, const struct scan_info_s * info) {
    struct jls_rd_s * rd = NULL;
    int64_t samples[2] = {-1, -1};
    static float data[SAMPLE_COUNT];

    for (int open_iter = 0; open_iter < 2; ++open_iter) {  // F5: reopen must match
        assert_int_equal(0, jls_rd_open(&rd, path));
        assert_int_equal(0, jls_rd_fsr_length(rd, 5, &samples[open_iter]));
        // recovery must keep every sample stored in an intact data chunk
        assert_true(samples[open_iter] >= info->data5_intact);
        assert_true(samples[open_iter] <= SAMPLE_COUNT);
        if (samples[open_iter] > 0) {
            assert_int_equal(0, jls_rd_fsr_f32(rd, 5, 0, data, samples[open_iter]));
            assert_memory_equal(signal_f32_, data, sizeof(float) * (size_t) samples[open_iter]);
        }
        jls_rd_close(rd);
        rd = NULL;
    }
    assert_int_equal(samples[0], samples[1]);  // idempotent repair
}

// F4 + F5 + F6 + F8: truncation sweep across chunk boundaries.
// The window-scan off-by-one (F4) recurred every 1000 bytes, so the
// sweep steps by primes to cover many alignments.
static void test_truncate_sweep(void ** state) {
    (void) state;
    struct scan_info_s info;
    gen(SRC_PATH, 1, 1, GEN_SKIP_CLOSE, -1);
    int64_t fend = file_size(SRC_PATH);
    scan_chunks(SRC_PATH, fend, &info);
    assert_true(info.defs_end > 0);
    assert_true(info.data5_intact > 0);

    for (int64_t offset = info.defs_end + 1; offset < fend; offset += 977) {
        file_copy_truncate(SRC_PATH, TST_PATH, offset);
        scan_chunks(TST_PATH, offset, &info);
        verify_recovered(TST_PATH, &info);
    }
    remove(TST_PATH);
    remove(SRC_PATH);
}

// F1: truncation that removes every complete data chunk must still open.
static void test_truncate_all_data_lost(void ** state) {
    (void) state;
    struct scan_info_s info;
    struct jls_rd_s * rd = NULL;
    int64_t samples = -1;
    gen(SRC_PATH, 0, 0, GEN_SKIP_CLOSE, -1);
    scan_chunks(SRC_PATH, file_size(SRC_PATH), &info);
    assert_true(info.data5_first > 0);
    // keep the data chunk header, remove its payload: no data survives
    file_copy_truncate(SRC_PATH, TST_PATH, info.data5_first + 8);
    for (int open_iter = 0; open_iter < 2; ++open_iter) {
        assert_int_equal(0, jls_rd_open(&rd, TST_PATH));
        assert_int_equal(0, jls_rd_fsr_length(rd, 5, &samples));
        assert_int_equal(0, samples);
        jls_rd_close(rd);
        rd = NULL;
    }
    remove(TST_PATH);
    remove(SRC_PATH);
}

// F3: UTC entries after the last summary must survive a crash.
static void test_utc_annotation_user_data_after_crash(void ** state) {
    (void) state;
    struct count_s count;
    struct jls_rd_s * rd = NULL;
    gen(SRC_PATH, 0, 1, GEN_SKIP_CLOSE, -1);
    struct scan_info_s info;
    scan_chunks(SRC_PATH, file_size(SRC_PATH), &info);
    // all written UTC entries are in intact chunks
    assert_int_equal((SAMPLE_COUNT + UTC_PERIOD - 1) / UTC_PERIOD, info.utc_intact);

    assert_int_equal(0, jls_rd_open(&rd, SRC_PATH));
    count.count = 0;
    assert_int_equal(0, jls_rd_utc(rd, 5, 0, on_utc, &count));
    assert_int_equal(info.utc_intact, count.count);
    count.count = 0;
    assert_int_equal(0, jls_rd_annotations(rd, 5, 0, on_annotation, &count));
    assert_int_equal(SAMPLE_COUNT / 5000, count.count);
    count.count = 0;
    assert_int_equal(0, jls_rd_user_data(rd, on_user_data, &count));
    assert_int_equal(1, count.count);
    jls_rd_close(rd);
    remove(SRC_PATH);
}

// F3 + orphan relink: stop the writer after every write near the file
// end and verify each crash prefix recovers.
static void test_write_prefix_sweep(void ** state) {
    (void) state;
    struct scan_info_s info;
    gen(SRC_PATH, 1, 1, GEN_SKIP_CLOSE, -1);
    int64_t total = jls_bk_write_total();
    gen(SRC_PATH, 1, 1, GEN_SKIP_CLOSE, -1);
    total = jls_bk_write_total() - total;  // bytes to write the full file

    for (int64_t budget = total - 4000; budget <= total; budget += 97) {
        gen(TST_PATH, 1, 1, GEN_SKIP_CLOSE, budget);
        scan_chunks(TST_PATH, file_size(TST_PATH), &info);
        if (0 == info.data5_intact) {
            continue;  // budget too small to write any data
        }
        verify_recovered(TST_PATH, &info);
    }
    remove(TST_PATH);
    remove(SRC_PATH);
}

// Write a crash file with only signal 5 and the requested length.
static void gen_crash_n(const char * path, int64_t sample_count) {
    struct jls_wr_s * wr = NULL;
    signals_generate();
    assert_int_equal(0, jls_wr_open(&wr, path));
    assert_int_equal(0, jls_wr_source_def(wr, &SOURCE_3));
    assert_int_equal(0, jls_wr_signal_def(wr, &SIGNAL_5));
    for (int64_t sample_id = 0; sample_id < sample_count; sample_id += WINDOW_SIZE) {
        int64_t len = sample_count - sample_id;
        if (len > WINDOW_SIZE) {
            len = WINDOW_SIZE;
        }
        assert_int_equal(0, jls_wr_fsr_f32(wr, 5, sample_id, signal_f32_, (uint32_t) len));
    }
    wr_abandon(wr);  // simulated crash
}

// Truncation recovery cost must track the crash tail, not the file size:
// each chunk is visited at most once and data payloads outside the crash
// window are never read.
static void test_recovery_traversal_bound(void ** state) {
    (void) state;
    struct jls_rd_s * rd = NULL;
    int64_t hops[2];
    int64_t payload[2];
    int64_t sample_counts[2] = {SAMPLE_COUNT, 30 * SAMPLE_COUNT};

    for (int i = 0; i < 2; ++i) {
        gen_crash_n(TST_PATH, sample_counts[i]);
        int64_t h = jls_raw_rd_header_total();
        int64_t p = jls_raw_rd_payload_total();
        assert_int_equal(0, jls_rd_open(&rd, TST_PATH));
        hops[i] = jls_raw_rd_header_total() - h;
        payload[i] = jls_raw_rd_payload_total() - p;
        jls_rd_close(rd);
        remove(TST_PATH);
    }
    // 30x the data (870 extra data chunks, ~3.5 MB): recovery may read the
    // extra level-1 index/summary chunks but never the extra data chunks
    assert_true(hops[1] < (hops[0] + 400));
    assert_true(payload[1] < (payload[0] + (1 << 20)));
}

// Write a crash file: a varying u8 prefix, then a constant (omitted) tail.
static void gen_crash_omit(const char * path, int64_t prefix_samples, int64_t tail_samples) {
    struct jls_wr_s * wr = NULL;
    uint8_t buf[640];
    assert_int_equal(0, jls_wr_open(&wr, path));
    assert_int_equal(0, jls_wr_source_def(wr, &SOURCE_3));
    assert_int_equal(0, jls_wr_signal_def(wr, &SIGNAL_7));
    assert_int_equal(0, jls_wr_fsr_omit_data(wr, 7, 1));
    int64_t total = prefix_samples + tail_samples;
    for (int64_t sample_id = 0; sample_id < total; sample_id += (int64_t) sizeof(buf)) {
        int64_t len = total - sample_id;
        if (len > (int64_t) sizeof(buf)) {
            len = (int64_t) sizeof(buf);
        }
        for (int64_t i = 0; i < len; ++i) {
            int64_t s = sample_id + i;
            buf[i] = (s < prefix_samples) ? (uint8_t) (s * 13) : 0x42;
        }
        assert_int_equal(0, jls_wr_fsr(wr, 7, sample_id, buf, (uint32_t) len));
    }
    wr_abandon(wr);  // simulated crash
}

// A long trailing run of fully-omitted (constant fsr_omit) intervals must
// not force recovery to walk the data chain from its head: cost tracks
// min(omitted tail, data chunks), never the data prefix length.
static void test_recovery_traversal_bound_omitted_tail(void ** state) {
    (void) state;
    struct jls_rd_s * rd = NULL;
    int64_t hops[2];
    int64_t prefix_counts[2] = {64000, 640000};
    const int64_t tail = 64000;  // 100 fully-omitted level-1 index chunks
    uint8_t buf[640];

    for (int i = 0; i < 2; ++i) {
        gen_crash_omit(TST_PATH, prefix_counts[i], tail);
        int64_t h = jls_raw_rd_header_total();
        assert_int_equal(0, jls_rd_open(&rd, TST_PATH));
        hops[i] = jls_raw_rd_header_total() - h;

        // prefix data intact
        assert_int_equal(0, jls_rd_fsr(rd, 7, 1000, buf, (int64_t) sizeof(buf)));
        for (int64_t j = 0; j < (int64_t) sizeof(buf); ++j) {
            assert_int_equal((uint8_t) ((1000 + j) * 13), buf[j]);
        }
        // omitted tail reconstructs the constant value
        assert_int_equal(0, jls_rd_fsr(rd, 7, prefix_counts[i] + 1000, buf, (int64_t) sizeof(buf)));
        for (int64_t j = 0; j < (int64_t) sizeof(buf); ++j) {
            assert_int_equal(0x42, buf[j]);
        }
        jls_rd_close(rd);
        remove(TST_PATH);
    }
    // 10x the data prefix (9000 extra data chunks): recovery cost must not
    // scale with it
    assert_true(hops[1] < (hops[0] + 400));
}

// Find the first chunk with the requested tag, or 0 if absent.
static int64_t find_chunk_offset(const char * path, uint8_t tag) {
    struct jls_raw_s * raw = NULL;
    struct jls_chunk_header_s hdr;
    int64_t offset = 0;
    int32_t rc = jls_raw_open(&raw, path, "r");
    if (rc && (rc != JLS_ERROR_TRUNCATED)) {
        return 0;
    }
    while (0 == jls_raw_rd_header(raw, &hdr)) {
        if (hdr.tag == tag) {
            offset = jls_raw_chunk_tell(raw);
            break;
        }
        if (jls_raw_chunk_next(raw)) {
            break;
        }
    }
    jls_raw_close(raw);
    return offset;
}

static void flip_byte(const char * path, int64_t offset) {
    FILE * f = fopen(path, "r+b");
    assert_non_null(f);
    uint8_t b = 0;
    assert_int_equal(0, fseek(f, (long) offset, SEEK_SET));
    assert_int_equal(1, fread(&b, 1, 1, f));
    b ^= 0xff;
    assert_int_equal(0, fseek(f, (long) offset, SEEK_SET));
    assert_int_equal(1, fwrite(&b, 1, 1, f));
    fclose(f);
}

static uint8_t * file_read_all(const char * path, int64_t * length) {
    *length = file_size(path);
    uint8_t * data = malloc((size_t) *length);
    assert_non_null(data);
    FILE * f = fopen(path, "rb");
    assert_non_null(f);
    assert_int_equal((size_t) *length, fread(data, 1, (size_t) *length, f));
    fclose(f);
    return data;
}

// wr_abandon must leave the crash-state file byte-for-byte unchanged,
// including unflushed fsr data and the unwritten end chunk.
static void test_wr_abandon_preserves_file(void ** state) {
    (void) state;
    struct jls_wr_s * wr = NULL;
    int64_t len0 = 0;
    int64_t len1 = 0;
    signals_generate();
    assert_int_equal(0, jls_wr_open(&wr, TST_PATH));
    assert_int_equal(0, jls_wr_source_def(wr, &SOURCE_3));
    assert_int_equal(0, jls_wr_signal_def(wr, &SIGNAL_5));
    assert_int_equal(0, jls_wr_fsr_f32(wr, 5, 0, signal_f32_, SAMPLE_COUNT));
    assert_int_equal(0, jls_wr_utc(wr, 5, 0, JLS_TIME_YEAR));
    uint8_t * before = file_read_all(TST_PATH, &len0);
    wr_abandon(wr);
    uint8_t * after = file_read_all(TST_PATH, &len1);
    assert_int_equal(len0, len1);
    assert_memory_equal(before, after, (size_t) len0);
    free(before);
    free(after);
    remove(TST_PATH);
}

// Interior corruption in a properly closed file: open must fail with
// JLS_ERROR_MESSAGE_INTEGRITY and must not modify the file.
static void test_open_clean_interior_corruption(void ** state) {
    (void) state;
    struct jls_rd_s * rd = NULL;
    gen(TST_PATH, 1, 1, GEN_CLOSE, -1);
    int64_t offset = find_chunk_offset(TST_PATH, JLS_TAG_SIGNAL_DEF);
    assert_true(offset > 0);
    flip_byte(TST_PATH, offset + sizeof(struct jls_chunk_header_s) + 8);  // payload byte

    int64_t length_before = 0;
    uint8_t * before = file_read_all(TST_PATH, &length_before);
    assert_int_equal(JLS_ERROR_MESSAGE_INTEGRITY, jls_rd_open(&rd, TST_PATH));
    int64_t length_after = 0;
    uint8_t * after = file_read_all(TST_PATH, &length_after);
    assert_int_equal(length_before, length_after);
    assert_memory_equal(before, after, (size_t) length_before);
    free(before);
    free(after);
    remove(TST_PATH);
}

// Torn track HEAD payload in a crashed file: truncation recovery cannot
// proceed; open must fail with JLS_ERROR_MESSAGE_INTEGRITY (offline repair).
static void test_open_crash_torn_head(void ** state) {
    (void) state;
    struct jls_rd_s * rd = NULL;
    gen(TST_PATH, 0, 0, GEN_SKIP_CLOSE, -1);
    int64_t offset = find_chunk_offset(TST_PATH, JLS_TAG_TRACK_FSR_HEAD);
    assert_true(offset > 0);
    flip_byte(TST_PATH, offset + sizeof(struct jls_chunk_header_s) + 4);  // payload byte
    assert_int_equal(JLS_ERROR_MESSAGE_INTEGRITY, jls_rd_open(&rd, TST_PATH));
    remove(TST_PATH);
}

// Recovery write failure (e.g. disk full): open must fail with
// JLS_ERROR_TRUNCATED.
static void test_open_crash_recovery_write_failure(void ** state) {
    (void) state;
    struct jls_rd_s * rd = NULL;
    gen(TST_PATH, 0, 0, GEN_SKIP_CLOSE, -1);
    jls_bk_write_limit_set(0);
    assert_int_equal(JLS_ERROR_TRUNCATED, jls_rd_open(&rd, TST_PATH));
    jls_bk_write_limit_set(-1);
    remove(TST_PATH);
}

#if !defined(_WIN32)
// Read-only truncated file: recovery cannot reopen for write; open must
// fail with exactly JLS_ERROR_TRUNCATED.
static void test_open_crash_read_only(void ** state) {
    (void) state;
    struct jls_rd_s * rd = NULL;
    gen(TST_PATH, 0, 0, GEN_SKIP_CLOSE, -1);
    assert_int_equal(0, chmod(TST_PATH, 0444));
    FILE * f = fopen(TST_PATH, "r+b");
    if (NULL != f) {
        fclose(f);  // running with CAP_DAC_OVERRIDE (root): cannot test
    } else {
        assert_int_equal(JLS_ERROR_TRUNCATED, jls_rd_open(&rd, TST_PATH));
        // the same file opens once writable again
        assert_int_equal(0, chmod(TST_PATH, 0644));
        assert_int_equal(0, jls_rd_open(&rd, TST_PATH));
        jls_rd_close(rd);
    }
    chmod(TST_PATH, 0644);
    remove(TST_PATH);
}
#endif

// F9: failed opens must not crash and must fail with an error code.
static void test_open_damaged_defs(void ** state) {
    (void) state;
    struct jls_rd_s * rd = NULL;
    gen(SRC_PATH, 0, 0, GEN_CLOSE, -1);
    for (int64_t offset = 1; offset < 600; offset += 41) {
        file_copy_truncate(SRC_PATH, TST_PATH, offset);
        rd = NULL;
        int32_t rc = jls_rd_open(&rd, TST_PATH);
        if (0 == rc) {
            jls_rd_close(rd);
        }
        rc = jls_rd_open(&rd, TST_PATH);  // and again (idempotent)
        if (0 == rc) {
            jls_rd_close(rd);
        }
    }
    remove(TST_PATH);
    remove(SRC_PATH);
}

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_truncate_sweep),
            cmocka_unit_test(test_truncate_all_data_lost),
            cmocka_unit_test(test_utc_annotation_user_data_after_crash),
            cmocka_unit_test(test_write_prefix_sweep),
            cmocka_unit_test(test_recovery_traversal_bound),
            cmocka_unit_test(test_recovery_traversal_bound_omitted_tail),
            cmocka_unit_test(test_open_clean_interior_corruption),
            cmocka_unit_test(test_wr_abandon_preserves_file),
            cmocka_unit_test(test_open_crash_torn_head),
            cmocka_unit_test(test_open_crash_recovery_write_failure),
#if !defined(_WIN32)
            cmocka_unit_test(test_open_crash_read_only),
#endif
            cmocka_unit_test(test_open_damaged_defs),
    };

    int rc = cmocka_run_group_tests(tests, NULL, NULL);
    free(signal_f32_);
    free(signal_i16_);
    return rc;
}
