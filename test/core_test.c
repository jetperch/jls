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
 * Regression tests for the core read, signal definition and recovery
 * findings in doc/plans/design_review_2026_10.md (D*, R3, C1-C8, C15, C19).
 *
 * Most tests write a valid file, patch chunk fields, and then re-sign the
 * chunk CRCs so that the reader sees hostile-but-CRC-valid content.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include "jls/backend.h"
#include "jls/core.h"
#include "jls/crc32c.h"
#include "jls/ec.h"
#include "jls/format.h"
#include "jls/raw.h"
#include "jls/reader.h"
#include "jls/writer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define ARRAY_SIZE(x) ( sizeof(x) / sizeof((x)[0]) )
static const char * PATH = "core_test_tmp.jls";
#define META_L1 ((1 << 12) | 1)  // signal 1, level 1

static const struct jls_source_def_s SOURCE_1 = {
        .source_id = 1,
        .name = "source 1",
        .vendor = "vendor",
        .model = "model",
        .version = "1.0.0",
        .serial_number = "0001",
};

// aligns to samples_per_data=1040, sample_decimate_factor=104:
// 20 data chunks per level-1 index
static const struct jls_signal_def_s SIGNAL_1 = {
        .signal_id = 1,
        .source_id = 1,
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


// --- file patch helpers ---

struct file_s {
    uint8_t * data;
    size_t size;
    size_t alloc;
};

static void file_load(const char * path, struct file_s * f, size_t extra) {
    FILE * fp = fopen(path, "rb");
    assert_non_null(fp);
    fseek(fp, 0, SEEK_END);
    f->size = (size_t) ftell(fp);
    fseek(fp, 0, SEEK_SET);
    f->alloc = f->size + extra;
    f->data = calloc(1, f->alloc);
    assert_non_null(f->data);
    assert_int_equal(f->size, fread(f->data, 1, f->size, fp));
    fclose(fp);
}

static void file_save(const char * path, struct file_s * f) {
    FILE * fp = fopen(path, "wb");
    assert_non_null(fp);
    assert_int_equal(f->size, fwrite(f->data, 1, f->size, fp));
    fclose(fp);
    free(f->data);
    f->data = NULL;
}

static uint32_t payload_size_on_disk(uint32_t payload_length) {
    return payload_length ? (((payload_length + 4 + 7) / 8) * 8) : 0;
}

// Walk the chunks in file order and return the n-th match, or NULL.
static struct jls_chunk_header_s * chunk_nth(struct file_s * f, uint8_t tag, uint16_t meta, int n) {
    size_t offset = sizeof(struct jls_file_header_s);
    while ((offset + sizeof(struct jls_chunk_header_s)) <= f->size) {
        struct jls_chunk_header_s * h = (struct jls_chunk_header_s *) (f->data + offset);
        if ((h->tag == tag) && (h->chunk_meta == meta) && (0 == n--)) {
            return h;
        }
        offset += sizeof(*h) + payload_size_on_disk(h->payload_length);
    }
    return NULL;
}

// Recompute the header and payload CRCs after a patch.
static void chunk_resign(struct jls_chunk_header_s * h) {
    if (h->payload_length) {
        uint8_t * payload = (uint8_t *) (h + 1);
        uint32_t crc32 = jls_crc32c(payload, h->payload_length);
        memcpy(payload + payload_size_on_disk(h->payload_length) - 4, &crc32, sizeof(crc32));
    }
    h->crc32 = jls_crc32c_hdr(h);
}

static void * chunk_payload(struct jls_chunk_header_s * h) {
    return (void *) (h + 1);
}

// --- file generation ---

static void gen_f32(const char * path, const struct jls_signal_def_s * def, int64_t n, int close) {
    struct jls_wr_s * wr = NULL;
    float * x = malloc(sizeof(float) * (size_t) n);
    assert_non_null(x);
    for (int64_t i = 0; i < n; ++i) {
        x[i] = (float) (i % 1000);
    }
    assert_int_equal(0, jls_wr_open(&wr, path));
    assert_int_equal(0, jls_wr_source_def(wr, &SOURCE_1));
    assert_int_equal(0, jls_wr_user_data(wr, 0x21, JLS_STORAGE_TYPE_STRING, (const uint8_t *) "hello", 6));
    assert_int_equal(0, jls_wr_signal_def(wr, def));
    assert_int_equal(0, jls_wr_fsr_f32(wr, def->signal_id, 0, x, (uint32_t) n));
    free(x);
    if (close) {
        assert_int_equal(0, jls_wr_close(wr));
    } else {
        // simulated crash: abandon the writer, close only the file descriptor
        struct jls_core_s * core = (struct jls_core_s *) wr;
        jls_bk_fclose(jls_raw_backend(core->raw));
    }
}

static struct jls_signal_def_s signal_def_typed(uint32_t data_type, uint32_t spd, uint32_t sdf,
                                                uint32_t eps, uint32_t sumdf) {
    struct jls_signal_def_s def = SIGNAL_1;
    def.data_type = data_type;
    def.samples_per_data = spd;
    def.sample_decimate_factor = sdf;
    def.entries_per_summary = eps;
    def.summary_decimate_factor = sumdf;
    return def;
}

// --- D1, D2, D4: signal definition align & validate ---

static void test_signal_def_align_wrap_rejected(void ** state) {
    (void) state;
    struct jls_wr_s * wr = NULL;
    assert_int_equal(0, jls_wr_open(&wr, PATH));
    assert_int_equal(0, jls_wr_source_def(wr, &SOURCE_1));
    struct jls_signal_def_s def;

    // 32-bit round up used to wrap to 0, then divide by zero
    def = SIGNAL_1; def.samples_per_data = 0xFFFFFFF0U;
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_wr_signal_def(wr, &def));
    def = SIGNAL_1; def.sample_decimate_factor = 0xFFFFFFF0U;
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_wr_signal_def(wr, &def));
    def = SIGNAL_1; def.entries_per_summary = 0xFFFFFFF0U;
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_wr_signal_def(wr, &def));
    def = SIGNAL_1; def.summary_decimate_factor = 0xFFFFFFF0U;
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_wr_signal_def(wr, &def));
    def = SIGNAL_1; def.annotation_decimate_factor = 0xFFFFFFF0U;
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_wr_signal_def(wr, &def));
    def = SIGNAL_1; def.utc_decimate_factor = 0xFFFFFFF0U;
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_wr_signal_def(wr, &def));
    // 32 MiB data chunk exceeds the cap
    def = SIGNAL_1; def.data_type = JLS_DATATYPE_F64; def.samples_per_data = 1U << 22;
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_wr_signal_def(wr, &def));

    // still usable after the rejections
    def = SIGNAL_1;
    assert_int_equal(0, jls_wr_signal_def(wr, &def));
    assert_int_equal(0, jls_wr_close(wr));
    remove(PATH);
}

static void test_signal_def_align_sweep(void ** state) {
    (void) state;
    const uint32_t data_types[] = {
        JLS_DATATYPE_U1, JLS_DATATYPE_U4, JLS_DATATYPE_U8, JLS_DATATYPE_U16, JLS_DATATYPE_U24,
        JLS_DATATYPE_U32, JLS_DATATYPE_U64, JLS_DATATYPE_I4, JLS_DATATYPE_I8, JLS_DATATYPE_I16,
        JLS_DATATYPE_I24, JLS_DATATYPE_I32, JLS_DATATYPE_I64, JLS_DATATYPE_F32, JLS_DATATYPE_F64,
    };
    const uint32_t spd[] = {0, 1, 9, 10, 100, 1000, 8191, 65536, 1U << 21, 1U << 22, (1U << 22) + 1, 0xFFFFFFFFU};
    const uint32_t sdf[] = {0, 1, 7, 100, 1000, 4096, 0xFFFFFFFFU};
    const uint32_t eps[] = {0, 1, 10, 100, 640, 1U << 19, (1U << 19) + 1, 0xFFFFFFFFU};
    const uint32_t sumdf[] = {0, 1, 2, 10, 20, 1000, 0xFFFFFFFFU};
    int pass = 0;
    for (size_t a = 0; a < ARRAY_SIZE(data_types); ++a) {
        for (size_t b = 0; b < ARRAY_SIZE(spd); ++b) {
            for (size_t c = 0; c < ARRAY_SIZE(sdf); ++c) {
                for (size_t d = 0; d < ARRAY_SIZE(eps); ++d) {
                    for (size_t e = 0; e < ARRAY_SIZE(sumdf); ++e) {
                        struct jls_signal_def_s def = signal_def_typed(data_types[a], spd[b], sdf[c], eps[d], sumdf[e]);
                        def.annotation_decimate_factor = 0;
                        def.utc_decimate_factor = 1;
                        if (0 == jls_core_signal_def_align(&def)) {
                            ++pass;
                            assert_int_equal(0, jls_core_signal_def_validate_fsr(&def));
                            assert_true(def.sample_decimate_factor >= 10);
                            assert_true(def.summary_decimate_factor >= 10);
                            assert_int_equal(100, def.annotation_decimate_factor);
                            assert_int_equal(10, def.utc_decimate_factor);
                        }
                    }
                }
            }
        }
    }
    assert_true(pass > 1000);

    // D4: defaults apply to every data type, including 24-bit
    for (size_t a = 0; a < ARRAY_SIZE(data_types); ++a) {
        struct jls_signal_def_s def = signal_def_typed(data_types[a], 0, 0, 0, 0);
        def.annotation_decimate_factor = 0;
        def.utc_decimate_factor = 0;
        assert_int_equal(0, jls_core_signal_def_align(&def));
        assert_true(def.samples_per_data >= 8192);
        assert_int_equal(100, def.annotation_decimate_factor);
        assert_int_equal(100, def.utc_decimate_factor);
    }
}

static void test_signal_def_validate_fsr(void ** state) {
    (void) state;
    struct jls_signal_def_s base = SIGNAL_1;
    assert_int_equal(0, jls_core_signal_def_align(&base));
    assert_int_equal(0, jls_core_signal_def_validate_fsr(&base));
    struct jls_signal_def_s def;

    def = base; def.samples_per_data = 0;
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_core_signal_def_validate_fsr(&def));
    def = base; def.samples_per_data = 1U << 23;
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_core_signal_def_validate_fsr(&def));
    def = base; def.sample_decimate_factor = 0;
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_core_signal_def_validate_fsr(&def));
    def = base; def.sample_decimate_factor = 7;  // not a divisor of samples_per_data
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_core_signal_def_validate_fsr(&def));
    def = base; def.entries_per_summary = 1;  // < entries per data chunk
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_core_signal_def_validate_fsr(&def));
    def = base; def.entries_per_summary = 0;
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_core_signal_def_validate_fsr(&def));
    def = base; def.summary_decimate_factor = 1;
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_core_signal_def_validate_fsr(&def));
    def = base; def.summary_decimate_factor = 0;
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_core_signal_def_validate_fsr(&def));
    def = base; def.summary_decimate_factor = 7;  // not a divisor of entries_per_summary
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_core_signal_def_validate_fsr(&def));
    def = base; def.data_type = JLS_DATATYPE_U1; def.sample_decimate_factor = 4;  // half byte
    def.samples_per_data = 1000; def.entries_per_summary = 250 * 4;
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_core_signal_def_validate_fsr(&def));

    assert_int_equal(1, jls_core_fsr_step_size(&base, 0));
    assert_int_equal(base.sample_decimate_factor, jls_core_fsr_step_size(&base, 1));
    assert_int_equal(INT64_MAX, jls_core_fsr_step_size(&base, JLS_SUMMARY_LEVEL_COUNT - 1));
}

// Patch the signal 1 definition u32 fields: [0]=data_type [1]=rate
// [2]=samples_per_data [3]=sample_decimate_factor [4]=entries_per_summary
// [5]=summary_decimate_factor
static void signal_def_patch(const char * path, int field, uint32_t value) {
    struct file_s f;
    file_load(path, &f, 0);
    struct jls_chunk_header_s * h = chunk_nth(&f, JLS_TAG_SIGNAL_DEF, 1, 0);
    assert_non_null(h);
    uint32_t * u32 = (uint32_t *) ((uint8_t *) chunk_payload(h) + 4);
    u32[field] = value;
    chunk_resign(h);
    file_save(path, &f);
}

static void test_signal_def_hostile_read(void ** state) {
    (void) state;
    struct {
        int field;
        uint32_t value;
    } cases[] = {
        {2, 0}, {3, 0}, {4, 1}, {5, 0}, {5, 1}, {2, 1U << 29}, {3, 7}, {4, 0xFFFFFFFFU},
    };
    for (size_t i = 0; i < ARRAY_SIZE(cases); ++i) {
        gen_f32(PATH, &SIGNAL_1, 50000, 1);
        signal_def_patch(PATH, cases[i].field, cases[i].value);
        struct jls_rd_s * rd = NULL;
        assert_int_equal(0, jls_rd_open(&rd, PATH));
        struct jls_signal_def_s * signals = NULL;
        uint16_t count = 0;
        assert_int_equal(0, jls_rd_signals(rd, &signals, &count));
        assert_int_equal(1, count);  // only the predefined signal 0
        float y[16];
        double stats[4 * JLS_SUMMARY_FSR_COUNT];
        int64_t length = 0;
        assert_int_equal(JLS_ERROR_NOT_FOUND, jls_rd_fsr_length(rd, 1, &length));
        assert_int_equal(JLS_ERROR_NOT_FOUND, jls_rd_fsr_f32(rd, 1, 1000, y, ARRAY_SIZE(y)));
        assert_int_equal(JLS_ERROR_NOT_FOUND, jls_rd_fsr_statistics(rd, 1, 0, 10000, stats, 4));
        jls_rd_close(rd);
    }
    remove(PATH);
}

// D5: a second SIGNAL_DEF for a defined signal must not retype it.
static void test_signal_def_duplicate_read(void ** state) {
    (void) state;
    struct jls_signal_def_s def2 = SIGNAL_1;
    def2.signal_id = 2;
    def2.data_type = JLS_DATATYPE_U8;
    float x[2000];
    for (size_t i = 0; i < ARRAY_SIZE(x); ++i) {
        x[i] = (float) i;
    }
    uint8_t x8[100] = {0};
    struct jls_wr_s * wr = NULL;
    assert_int_equal(0, jls_wr_open(&wr, PATH));
    assert_int_equal(0, jls_wr_source_def(wr, &SOURCE_1));
    assert_int_equal(0, jls_wr_signal_def(wr, &SIGNAL_1));
    assert_int_equal(0, jls_wr_signal_def(wr, &def2));
    assert_int_equal(0, jls_wr_fsr_f32(wr, 1, 0, x, ARRAY_SIZE(x)));
    assert_int_equal(0, jls_wr_fsr(wr, 2, 0, x8, ARRAY_SIZE(x8)));
    assert_int_equal(0, jls_wr_close(wr));

    struct file_s f;
    file_load(PATH, &f, 0);
    struct jls_chunk_header_s * h = chunk_nth(&f, JLS_TAG_SIGNAL_DEF, 2, 0);
    assert_non_null(h);
    h->chunk_meta = 1;  // now a duplicate signal 1 def with data_type U8
    chunk_resign(h);
    file_save(PATH, &f);

    struct jls_rd_s * rd = NULL;
    assert_int_equal(0, jls_rd_open(&rd, PATH));
    struct jls_signal_def_s def;
    assert_int_equal(0, jls_rd_signal(rd, 1, &def));
    assert_int_equal(JLS_DATATYPE_F32, def.data_type);
    assert_int_equal(JLS_ERROR_NOT_FOUND, jls_rd_signal(rd, 2, &def));
    float y[ARRAY_SIZE(x)];
    assert_int_equal(0, jls_rd_fsr_f32(rd, 1, 0, y, ARRAY_SIZE(y)));
    assert_memory_equal(x, y, sizeof(x));
    jls_rd_close(rd);
    remove(PATH);
}


int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_signal_def_align_wrap_rejected),
            cmocka_unit_test(test_signal_def_align_sweep),
            cmocka_unit_test(test_signal_def_validate_fsr),
            cmocka_unit_test(test_signal_def_hostile_read),
            cmocka_unit_test(test_signal_def_duplicate_read),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
