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
 * Tests for the offline jls_repair() operation and its interaction
 * with the jls_rd_open() error contract:
 * - tail truncation recovers automatically (rc 0)
 * - other corruption fails open with JLS_ERROR_MESSAGE_INTEGRITY
 * - jls_repair() restores those files
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
#include "jls/reader.h"
#include "jls/repair.h"
#include "jls/writer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <sys/stat.h>
#endif


static const char * PATH = "repair_offline_test_tmp.jls";

#define SAMPLE_COUNT (30000)
#define WINDOW_SIZE (937)

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

static float * signal_f32_;

static void signals_generate(void) {
    if (NULL == signal_f32_) {
        signal_f32_ = malloc(sizeof(float) * SAMPLE_COUNT);
        assert_non_null(signal_f32_);
        for (int64_t i = 0; i < SAMPLE_COUNT; ++i) {
            signal_f32_[i] = (float) ((i % 1000) - 500) / 500.0f;
        }
    }
}

static void gen(const char * path, int close_file) {
    struct jls_wr_s * wr = NULL;
    signals_generate();
    assert_int_equal(0, jls_wr_open(&wr, path));
    assert_int_equal(0, jls_wr_source_def(wr, &SOURCE_3));
    assert_int_equal(0, jls_wr_signal_def(wr, &SIGNAL_5));
    for (int64_t sample_id = 0; sample_id < SAMPLE_COUNT; sample_id += WINDOW_SIZE) {
        int64_t len = SAMPLE_COUNT - sample_id;
        if (len > WINDOW_SIZE) {
            len = WINDOW_SIZE;
        }
        assert_int_equal(0, jls_wr_fsr_f32(wr, 5, sample_id, signal_f32_ + sample_id, (uint32_t) len));
    }
    if (close_file) {
        assert_int_equal(0, jls_wr_close(wr));
    } else {
        // simulated crash: abandon the writer, close only the file descriptor
        struct jls_core_s * core = (struct jls_core_s *) wr;
        jls_bk_fclose(jls_raw_backend(core->raw));
    }
}

// Find the nth chunk with the requested tag; fills length, returns offset or 0.
static int64_t find_chunk(const char * path, uint8_t tag, int n, uint32_t * payload_length) {
    struct jls_raw_s * raw = NULL;
    struct jls_chunk_header_s hdr;
    int64_t offset = 0;
    int32_t rc = jls_raw_open(&raw, path, "r");
    if (rc && (rc != JLS_ERROR_TRUNCATED)) {
        return 0;
    }
    while (0 == jls_raw_rd_header(raw, &hdr)) {
        if (hdr.tag == tag) {
            if (0 == n--) {
                offset = jls_raw_chunk_tell(raw);
                if (NULL != payload_length) {
                    *payload_length = hdr.payload_length;
                }
                break;
            }
        }
        if (jls_raw_chunk_next(raw)) {
            break;
        }
    }
    jls_raw_close(raw);
    return offset;
}

static void corrupt(const char * path, int64_t offset, int64_t length) {
    FILE * f = fopen(path, "r+b");
    assert_non_null(f);
    assert_int_equal(0, fseek(f, (long) offset, SEEK_SET));
    for (int64_t i = 0; i < length; ++i) {
        assert_int_equal(1, fwrite("\x5a", 1, 1, f));
    }
    fclose(f);
}

static uint64_t file_hash(const char * path) {
    FILE * f = fopen(path, "rb");
    assert_non_null(f);
    uint64_t h = 0xcbf29ce484222325ULL;
    uint8_t b = 0;
    while (1 == fread(&b, 1, 1, f)) {
        h = (h ^ b) * 0x100000001b3ULL;
    }
    fclose(f);
    return h;
}

// Verify an error-free, value-exact read.  A crash-generated file loses
// the buffered tail, so require most (not all) samples to survive.
static void assert_full_read(const char * path) {
    struct jls_rd_s * rd = NULL;
    static float data[SAMPLE_COUNT];
    int64_t samples = 0;
    assert_int_equal(0, jls_rd_open(&rd, path));
    assert_int_equal(0, jls_rd_fsr_length(rd, 5, &samples));
    assert_in_range(samples, SAMPLE_COUNT - 3000, SAMPLE_COUNT);
    assert_int_equal(0, jls_rd_fsr_f32(rd, 5, 0, data, (uint32_t) samples));
    jls_rd_close(rd);
    for (int64_t i = 0; i < samples; ++i) {
        assert_float_equal(signal_f32_[i], data[i], 1e-9);
    }
}

static void test_repair_healthy_is_noop(void ** state) {
    (void) state;
    gen(PATH, 1);
    uint64_t hash = file_hash(PATH);
    assert_int_equal(0, jls_repair(PATH, NULL, NULL, NULL, NULL));
    assert_int_equal(hash, file_hash(PATH));
    remove(PATH);
}

static void test_repair_torn_head(void ** state) {
    (void) state;
    struct jls_rd_s * rd = NULL;
    gen(PATH, 0);
    int64_t offset = find_chunk(PATH, JLS_TAG_TRACK_FSR_HEAD, 0, NULL);
    assert_true(offset > 0);
    corrupt(PATH, offset + sizeof(struct jls_chunk_header_s) + 4, 4);
    assert_int_equal(JLS_ERROR_MESSAGE_INTEGRITY, jls_rd_open(&rd, PATH));
    assert_int_equal(0, jls_repair(PATH, NULL, NULL, NULL, NULL));
    assert_full_read(PATH);

    // repair is idempotent: a second run leaves the file unchanged
    uint64_t hash = file_hash(PATH);
    assert_int_equal(0, jls_repair(PATH, NULL, NULL, NULL, NULL));
    assert_int_equal(hash, file_hash(PATH));
    remove(PATH);
}

// Rewrite the chunk header at offset with a new payload_length (valid CRC).
static void head_length_set(const char * path, int64_t offset, uint32_t payload_length) {
    struct jls_raw_s * raw = NULL;
    struct jls_chunk_header_s hdr;
    assert_int_equal(0, jls_raw_open(&raw, path, "a"));
    assert_int_equal(0, jls_raw_chunk_seek(raw, offset));
    assert_int_equal(0, jls_raw_rd_header(raw, &hdr));
    hdr.payload_length = payload_length;
    assert_int_equal(0, jls_raw_chunk_seek(raw, offset));
    assert_int_equal(0, jls_raw_wr_header(raw, &hdr));
    assert_int_equal(0, jls_raw_close(raw));
}

static void test_repair_head_length_torn(void ** state) {
    (void) state;
    struct jls_rd_s * rd = NULL;
    gen(PATH, 1);
    int64_t offset = find_chunk(PATH, JLS_TAG_TRACK_FSR_HEAD, 0, NULL);
    assert_true(offset > 0);
    // header valid, payload CRC fails, length disagrees with a head payload
    head_length_set(PATH, offset, 4096);
    assert_int_not_equal(0, jls_rd_open(&rd, PATH));
    assert_int_equal(0, jls_repair(PATH, NULL, NULL, NULL, NULL));
    assert_full_read(PATH);
    remove(PATH);
}

static void test_repair_head_length_valid(void ** state) {
    (void) state;
    struct jls_rd_s * rd = NULL;
    struct jls_raw_s * raw = NULL;
    struct jls_chunk_header_s hdr;
    uint8_t payload[16];
    memset(payload, 0, sizeof(payload));
    gen(PATH, 1);
    int64_t offset = find_chunk(PATH, JLS_TAG_TRACK_FSR_HEAD, 0, NULL);
    assert_true(offset > 0);
    // a CRC-valid head with the wrong payload length
    assert_int_equal(0, jls_raw_open(&raw, PATH, "a"));
    assert_int_equal(0, jls_raw_chunk_seek(raw, offset));
    assert_int_equal(0, jls_raw_rd_header(raw, &hdr));
    hdr.payload_length = sizeof(payload);
    assert_int_equal(0, jls_raw_chunk_seek(raw, offset));
    assert_int_equal(0, jls_raw_wr(raw, &hdr, payload));
    assert_int_equal(0, jls_raw_close(raw));
    assert_int_not_equal(0, jls_rd_open(&rd, PATH));

    assert_int_equal(0, jls_repair(PATH, NULL, NULL, NULL, NULL));
    assert_full_read(PATH);
    remove(PATH);
}

static void test_repair_dead_interior_data(void ** state) {
    (void) state;
    struct jls_rd_s * rd = NULL;
    static float data[SAMPLE_COUNT];
    gen(PATH, 1);
    // kill the 6th data chunk; its coverage depends on the aligned
    // samples_per_data, so read it before corrupting
    int64_t offset = find_chunk(PATH, JLS_TAG_TRACK_FSR_DATA, 5, NULL);
    assert_true(offset > 0);
    struct jls_fsr_data_s header;
    FILE * f = fopen(PATH, "rb");
    assert_non_null(f);
    assert_int_equal(0, fseek(f, (long) (offset + sizeof(struct jls_chunk_header_s)), SEEK_SET));
    assert_int_equal(1, fread(&header, sizeof(header), 1, f));
    fclose(f);
    int64_t hole_start = header.header.timestamp;
    int64_t hole_end = hole_start + header.header.entry_count;
    corrupt(PATH, offset + sizeof(struct jls_chunk_header_s) + 16, 64);

    // interior data damage: open succeeds, reads over the hole fail
    assert_int_equal(0, jls_rd_open(&rd, PATH));
    assert_int_equal(JLS_ERROR_MESSAGE_INTEGRITY, jls_rd_fsr_f32(rd, 5, 0, data, SAMPLE_COUNT));
    jls_rd_close(rd);

    assert_int_equal(0, jls_repair(PATH, NULL, NULL, NULL, NULL));

    // the hole reads as summary-reconstructed values; the rest is exact
    assert_int_equal(0, jls_rd_open(&rd, PATH));
    assert_int_equal(0, jls_rd_fsr_f32(rd, 5, 0, data, SAMPLE_COUNT));
    jls_rd_close(rd);
    for (int64_t i = 0; i < SAMPLE_COUNT; ++i) {
        if ((i < hole_start) || (i >= hole_end)) {
            assert_float_equal(signal_f32_[i], data[i], 1e-9);
        }
    }
    remove(PATH);
}

struct progress_state_s {
    int32_t count;
    double last;
    int32_t cancel_after;
};

static int32_t progress_fn(void * user_data, double progress) {
    struct progress_state_s * s = (struct progress_state_s *) user_data;
    assert_true(progress >= s->last);  // monotonic
    assert_true(progress <= 1.0);
    s->last = progress;
    ++s->count;
    return (s->cancel_after && (s->count >= s->cancel_after)) ? 1 : 0;
}

static void test_repair_progress_and_cancel(void ** state) {
    (void) state;
    gen(PATH, 0);
    int64_t offset = find_chunk(PATH, JLS_TAG_TRACK_FSR_HEAD, 0, NULL);
    corrupt(PATH, offset + sizeof(struct jls_chunk_header_s) + 4, 4);

    struct progress_state_s cancel = {.count = 0, .last = 0.0, .cancel_after = 1};
    assert_int_equal(JLS_ERROR_ABORTED, jls_repair(PATH, NULL, NULL, progress_fn, &cancel));

    // a canceled repair can be rerun to completion
    struct progress_state_s collect = {.count = 0, .last = 0.0, .cancel_after = 0};
    assert_int_equal(0, jls_repair(PATH, NULL, NULL, progress_fn, &collect));
    assert_true(collect.count >= 2);  // 0.0 start and 1.0 done
    assert_float_equal(1.0, collect.last, 0.0);
    assert_full_read(PATH);
    remove(PATH);
}

// Write a closed file large enough that progress fires mid-traversal.
static void gen_large(const char * path, int64_t sample_count) {
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
    assert_int_equal(0, jls_wr_close(wr));
}

static int32_t relink_count_fn(void * user_data, const char * msg) {
    int32_t * count = (int32_t *) user_data;
    if (NULL != strstr(msg, "relink")) {
        ++(*count);
    }
    return 0;
}

// Cancel mid-traversal, after relink writes have occurred: repair is not
// atomic (design ruling C5), so the partially-repaired file must repair
// cleanly to a fully readable file on rerun.
static void test_repair_cancel_mid_traversal_reruns(void ** state) {
    (void) state;
    const int64_t sample_count = 4000000;  // ~16 MB: progress fires mid-traversal
    gen_large(PATH, sample_count);
    // kill a data chunk ~1 MB in so a relink write precedes the cancel
    int64_t offset = find_chunk(PATH, JLS_TAG_TRACK_FSR_DATA, 250, NULL);
    assert_true(offset > 0);
    corrupt(PATH, offset + sizeof(struct jls_chunk_header_s) + 4, 4);

    int32_t relinks = 0;
    struct progress_state_s cancel = {.count = 0, .last = 0.0, .cancel_after = 2};
    assert_int_equal(JLS_ERROR_ABORTED,
                     jls_repair(PATH, relink_count_fn, &relinks, progress_fn, &cancel));
    assert_int_equal(2, cancel.count);
    assert_true(cancel.last > 0.0);  // canceled mid-traversal,
    assert_true(cancel.last < 1.0);  // not at start or completion
    assert_true(relinks >= 1);       // a repair write preceded the cancel

    // the partially-repaired file repairs to completion on rerun
    assert_int_equal(0, jls_repair(PATH, NULL, NULL, NULL, NULL));
    struct jls_rd_s * rd = NULL;
    assert_int_equal(0, jls_rd_open(&rd, PATH));
    int64_t samples = 0;
    assert_int_equal(0, jls_rd_fsr_length(rd, 5, &samples));
    assert_int_equal(sample_count, samples);
    float * data = malloc(sizeof(float) * (size_t) sample_count);
    assert_non_null(data);
    assert_int_equal(0, jls_rd_fsr_f32(rd, 5, 0, data, samples));
    for (int64_t i = 0; i < 100; ++i) {  // exact well away from the dead chunk
        assert_float_equal(signal_f32_[i], data[i], 1e-9);
    }
    free(data);
    jls_rd_close(rd);
    remove(PATH);
}

static int32_t msg_cancel_fn(void * user_data, const char * msg) {
    (void) msg;
    int32_t * count = (int32_t *) user_data;
    ++(*count);
    return 1;
}

// msg_fn returning nonzero must cancel with JLS_ERROR_ABORTED, same as
// progress_fn: the CLI relies on it for its quit-on-signal path.
static void test_repair_msg_cancel(void ** state) {
    (void) state;
    gen(PATH, 0);
    int64_t offset = find_chunk(PATH, JLS_TAG_TRACK_FSR_HEAD, 0, NULL);
    corrupt(PATH, offset + sizeof(struct jls_chunk_header_s) + 4, 4);

    int32_t count = 0;
    assert_int_equal(JLS_ERROR_ABORTED, jls_repair(PATH, msg_cancel_fn, &count, NULL, NULL));
    assert_true(count >= 1);

    // a canceled repair can be rerun to completion
    assert_int_equal(0, jls_repair(PATH, NULL, NULL, NULL, NULL));
    assert_full_read(PATH);
    remove(PATH);
}

static void test_repair_missing_file(void ** state) {
    (void) state;
    remove(PATH);
    assert_int_not_equal(0, jls_repair(PATH, NULL, NULL, NULL, NULL));
}

static void test_repair_not_jls(void ** state) {
    (void) state;
    FILE * f = fopen(PATH, "wb");
    assert_non_null(f);
    for (int i = 0; i < 1000; ++i) {
        assert_int_equal(1, fwrite("garbage!", 8, 1, f));
    }
    fclose(f);
    assert_int_equal(JLS_ERROR_UNSUPPORTED_FILE, jls_repair(PATH, NULL, NULL, NULL, NULL));
    remove(PATH);
}

#if !defined(_WIN32)
static void test_repair_read_only(void ** state) {
    (void) state;
    gen(PATH, 0);
    assert_int_equal(0, chmod(PATH, 0444));
    FILE * f = fopen(PATH, "r+b");
    if (NULL != f) {
        fclose(f);  // running with CAP_DAC_OVERRIDE (root): cannot test
    } else {
        assert_int_equal(JLS_ERROR_IO, jls_repair(PATH, NULL, NULL, NULL, NULL));
    }
    chmod(PATH, 0644);
    remove(PATH);
}
#endif

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_repair_healthy_is_noop),
            cmocka_unit_test(test_repair_torn_head),
            cmocka_unit_test(test_repair_head_length_torn),
            cmocka_unit_test(test_repair_head_length_valid),
            cmocka_unit_test(test_repair_dead_interior_data),
            cmocka_unit_test(test_repair_progress_and_cancel),
            cmocka_unit_test(test_repair_msg_cancel),
            cmocka_unit_test(test_repair_cancel_mid_traversal_reruns),
            cmocka_unit_test(test_repair_missing_file),
            cmocka_unit_test(test_repair_not_jls),
#if !defined(_WIN32)
            cmocka_unit_test(test_repair_read_only),
#endif
    };

    int rc = cmocka_run_group_tests(tests, NULL, NULL);
    free(signal_f32_);
    return rc;
}
