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

static int64_t file_size(const char * path) {
    FILE * fp = fopen(path, "rb");
    assert_non_null(fp);
    fseek(fp, 0, SEEK_END);
    int64_t sz = (int64_t) ftell(fp);
    fclose(fp);
    return sz;
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

static int chunk_count(struct file_s * f, uint8_t tag, uint16_t meta) {
    int count = 0;
    while (chunk_nth(f, tag, meta, count)) {
        ++count;
    }
    return count;
}

static struct jls_chunk_header_s * chunk_last(struct file_s * f) {
    size_t offset = sizeof(struct jls_file_header_s);
    struct jls_chunk_header_s * last = NULL;
    while ((offset + sizeof(struct jls_chunk_header_s)) <= f->size) {
        last = (struct jls_chunk_header_s *) (f->data + offset);
        offset += sizeof(*last) + payload_size_on_disk(last->payload_length);
    }
    return last;
}

static int64_t chunk_offset(struct file_s * f, struct jls_chunk_header_s * h) {
    return (int64_t) ((uint8_t *) h - f->data);
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

// Append a chunk at the end of the file buffer.
static struct jls_chunk_header_s * chunk_append(struct file_s * f, uint8_t tag, uint16_t meta,
        uint64_t item_prev, const void * payload, uint32_t payload_length) {
    size_t sz = sizeof(struct jls_chunk_header_s) + payload_size_on_disk(payload_length);
    assert_true((f->size + sz) <= f->alloc);
    struct jls_chunk_header_s * h = (struct jls_chunk_header_s *) (f->data + f->size);
    memset(h, 0, sz);
    h->item_prev = item_prev;
    h->tag = tag;
    h->chunk_meta = meta;
    h->payload_length = payload_length;
    memcpy(h + 1, payload, payload_length);
    chunk_resign(h);
    f->size += sz;
    return h;
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

// Write packed sub-byte or byte samples starting at sample_id.
static void gen_packed(const char * path, const struct jls_signal_def_s * def, int64_t sample_id,
                       const uint8_t * data, uint32_t n, int omit) {
    struct jls_wr_s * wr = NULL;
    assert_int_equal(0, jls_wr_open(&wr, path));
    assert_int_equal(0, jls_wr_source_def(wr, &SOURCE_1));
    assert_int_equal(0, jls_wr_signal_def(wr, def));
    if (omit) {
        assert_int_equal(0, jls_wr_fsr_omit_data(wr, def->signal_id, 1));
    }
    assert_int_equal(0, jls_wr_fsr(wr, def->signal_id, sample_id, data, n));
    assert_int_equal(0, jls_wr_close(wr));
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


// --- C1: u1/u4 reads ---

static int sample_get(const uint8_t * b, int bits, int64_t i) {
    if (bits == 1) {
        return (b[i >> 3] >> (i & 7)) & 1;
    }
    return (b[i >> 1] >> ((i & 1) * 4)) & 0xf;
}

// Read [start, start + length) into an exact-size heap buffer (ASan
// catches any overflow) and compare against the reference.
static void packed_read_check(struct jls_rd_s * rd, const uint8_t * ref, int bits, int64_t start, int64_t length) {
    size_t sz = (size_t) ((length * bits + 7) / 8);
    uint8_t * y = malloc(sz);
    assert_non_null(y);
    memset(y, 0xA5, sz);
    assert_int_equal(0, jls_rd_fsr(rd, 1, start, y, length));
    for (int64_t i = 0; i < length; ++i) {
        if (sample_get(y, bits, i) != sample_get(ref, bits, start + i)) {
            fail_msg("bits=%d start=%d length=%d: mismatch at %d", bits, (int) start, (int) length, (int) i);
        }
    }
    uint32_t tail_bits = (uint32_t) ((length * bits) & 7);
    if (tail_bits) {
        assert_int_equal(0, y[sz - 1] >> tail_bits);  // unused high bits cleared
    }
    free(y);
}

static void packed_sweep(int bits, uint32_t spd, uint32_t sdf, uint32_t n) {
    const int64_t first_sample_ids[] = {0, 1, 3, 5, 7};
    uint32_t data_type = (bits == 1) ? JLS_DATATYPE_U1 : JLS_DATATYPE_U4;
    struct jls_signal_def_s def = signal_def_typed(data_type, spd, sdf, 10, 10);
    size_t nbytes = ((size_t) n * bits + 7) / 8;
    uint8_t * ref = malloc(nbytes + 8);  // slack: writer may over-read the source (W9)
    assert_non_null(ref);
    srand(1);
    for (size_t i = 0; i < nbytes + 8; ++i) {
        ref[i] = (uint8_t) rand();
    }

    for (size_t k = 0; k < ARRAY_SIZE(first_sample_ids); ++k) {
        gen_packed(PATH, &def, first_sample_ids[k], ref, n, 0);
        struct jls_rd_s * rd = NULL;
        assert_int_equal(0, jls_rd_open(&rd, PATH));
        struct jls_signal_def_s def_rd;
        assert_int_equal(0, jls_rd_signal(rd, 1, &def_rd));
        assert_int_equal(spd, def_rd.samples_per_data);  // > 3 data chunks
        int64_t samples = 0;
        assert_int_equal(0, jls_rd_fsr_length(rd, 1, &samples));
        assert_int_equal(n, samples);

        // every start, short lengths & lengths crossing chunk boundaries
        const int64_t lengths[] = {1, 2, 3, 7, 8, 9, 15, 16, 17, spd - 1, spd, spd + 1, 2 * spd + 3};
        for (int64_t start = 0; start < n; ++start) {
            for (size_t j = 0; j < ARRAY_SIZE(lengths); ++j) {
                if (lengths[j] <= (n - start)) {
                    packed_read_check(rd, ref, bits, start, lengths[j]);
                }
            }
            packed_read_check(rd, ref, bits, start, n - start);
        }
        for (int i = 0; i < 500; ++i) {
            int64_t start = rand() % n;
            int64_t length = 1 + (rand() % (n - start));
            packed_read_check(rd, ref, bits, start, length);
        }
        jls_rd_close(rd);
    }
    free(ref);
    remove(PATH);
}

static void test_fsr_u1_sweep(void ** state) {
    (void) state;
    packed_sweep(1, 256, 256, 256 * 3 + 32);
}

static void test_fsr_u4_sweep(void ** state) {
    (void) state;
    packed_sweep(4, 64, 64, 64 * 3 + 8);
}

static void test_fsr_u4_exhaustive(void ** state) {
    (void) state;
    const uint32_t n = 64 * 2 + 10;
    struct jls_signal_def_s def = signal_def_typed(JLS_DATATYPE_U4, 64, 64, 10, 10);
    uint8_t ref[n / 2 + 8];
    for (size_t i = 0; i < sizeof(ref); ++i) {
        ref[i] = (uint8_t) (i * 37 + 11);
    }
    gen_packed(PATH, &def, 3, ref, n, 0);
    struct jls_rd_s * rd = NULL;
    assert_int_equal(0, jls_rd_open(&rd, PATH));
    for (int64_t start = 0; start < n; ++start) {
        for (int64_t length = 1; length <= (n - start); ++length) {
            packed_read_check(rd, ref, 4, start, length);
        }
    }
    jls_rd_close(rd);
    remove(PATH);
}

// The final partial output byte used to underflow the copy size into a
// huge memcpy over the caller's 1-byte buffer.
static void test_fsr_u1_tail_read_bounds(void ** state) {
    (void) state;
    uint8_t ref[16 + 8];
    memset(ref, 0x5A, sizeof(ref));
    struct jls_signal_def_s def = signal_def_typed(JLS_DATATYPE_U1, 0, 0, 0, 0);
    gen_packed(PATH, &def, 0, ref, 100, 0);
    struct jls_rd_s * rd = NULL;
    assert_int_equal(0, jls_rd_open(&rd, PATH));
    uint8_t * y = malloc(1);
    assert_int_equal(0, jls_rd_fsr(rd, 1, 97, y, 1));
    assert_int_equal(0, jls_rd_fsr(rd, 1, 90, y, 6));
    assert_int_equal(sample_get(ref, 1, 90) | (sample_get(ref, 1, 91) << 1), y[0] & 3);
    free(y);
    y = malloc(2);
    assert_int_equal(0, jls_rd_fsr(rd, 1, 89, y, 10));
    free(y);
    jls_rd_close(rd);
    remove(PATH);
}


// --- C2: data chunk timestamp must cover the read ---

static void test_fsr_data_timestamp_corrupt(void ** state) {
    (void) state;
    const uint32_t types[] = {JLS_DATATYPE_U1, JLS_DATATYPE_F32};
    const int64_t timestamps[] = {0, 600};  // chunk 2 starts before / after the read
    for (size_t t = 0; t < ARRAY_SIZE(types); ++t) {
        for (size_t k = 0; k < ARRAY_SIZE(timestamps); ++k) {
            int bits = jls_datatype_parse_size(types[t]);
            struct jls_signal_def_s def = signal_def_typed(types[t], 256, 256, 10, 10);
            const uint32_t n = 256 * 4;
            uint8_t * x = calloc(1, (n * bits) / 8 + 8);
            assert_non_null(x);
            gen_packed(PATH, &def, 0, x, n, 0);
            free(x);
            struct file_s f;
            file_load(PATH, &f, 0);
            struct jls_chunk_header_s * h = chunk_nth(&f, JLS_TAG_TRACK_FSR_DATA, 1, 2);
            assert_non_null(h);
            struct jls_payload_header_s * p = chunk_payload(h);
            assert_int_equal(512, p->timestamp);
            p->timestamp = timestamps[k];
            chunk_resign(h);
            file_save(PATH, &f);

            struct jls_rd_s * rd = NULL;
            assert_int_equal(0, jls_rd_open(&rd, PATH));
            uint64_t y[8];
            assert_int_equal(0, jls_rd_fsr(rd, 1, 300, y, 16));
            assert_int_equal(JLS_ERROR_MESSAGE_INTEGRITY, jls_rd_fsr(rd, 1, 520, y, 16));
            if (0 == timestamps[k]) {
                assert_int_equal(JLS_ERROR_MESSAGE_INTEGRITY, jls_rd_fsr(rd, 1, 601, y, 16));
            }
            assert_int_equal(JLS_ERROR_MESSAGE_INTEGRITY, jls_rd_fsr(rd, 1, 500, y, 16));
            jls_rd_close(rd);
        }
    }
    remove(PATH);
}


// --- C3, C4: omitted chunk reconstruction ---

static void gen_omit_u8(uint8_t value) {
    struct jls_signal_def_s def = signal_def_typed(JLS_DATATYPE_U8, 64, 32, 20, 10);
    uint8_t x[64 * 8 + 10];
    memset(x, value, sizeof(x));
    x[sizeof(x) - 1] = value + 1;  // keep the final chunk: length from data
    gen_packed(PATH, &def, 0, x, sizeof(x), 1);
}

static void test_fsr_omit_reconstruct_corrupt(void ** state) {
    (void) state;
    for (int mode = 0; mode < 4; ++mode) {
        gen_omit_u8(7);
        struct file_s f;
        file_load(PATH, &f, 0);
        struct jls_chunk_header_s * h = chunk_nth(&f, JLS_TAG_TRACK_FSR_SUMMARY, META_L1, 0);
        assert_non_null(h);
        struct jls_payload_header_s * p = chunk_payload(h);
        switch (mode) {
            case 0: p->timestamp = 1LL << 40; break;         // s_index hugely negative
            case 1: p->timestamp = -65540LL * 1024; break;   // s_index past the summary
            case 2: p->entry_count = 0xFFFFFFF0U; break;     // past the payload
            case 3: p->entry_count = 2; break;               // omitted chunk 3 has no summary
            default: break;
        }
        chunk_resign(h);
        file_save(PATH, &f);

        struct jls_rd_s * rd = NULL;
        assert_int_equal(0, jls_rd_open(&rd, PATH));
        uint8_t y[16];
        int32_t rc = jls_rd_fsr(rd, 1, 64 * 3 + 5, y, sizeof(y));
        if (rc != JLS_ERROR_MESSAGE_INTEGRITY) { fail_msg("mode %d rc %d", mode, rc); }
        jls_rd_close(rd);
    }
    remove(PATH);
}

static void test_fsr_omit_reconstruct_types(void ** state) {
    (void) state;
    struct {
        uint32_t data_type;
        uint8_t fill;    // packed constant sample byte
    } cases[] = {
        {JLS_DATATYPE_U1, 0xff},
        {JLS_DATATYPE_U4, 0xaa},
        {JLS_DATATYPE_I4, 0xdd},    // -3
        {JLS_DATATYPE_U8, 200},
        {JLS_DATATYPE_I8, 0xfd},    // -3
        {JLS_DATATYPE_I8, 0x80},    // -128
    };
    for (size_t i = 0; i < ARRAY_SIZE(cases); ++i) {
        int bits = jls_datatype_parse_size(cases[i].data_type);
        struct jls_signal_def_s def = signal_def_typed(cases[i].data_type, 256, 256, 20, 10);
        const uint32_t n = 256 * 8;
        size_t sz = (n * bits) / 8;
        uint8_t * x = malloc(sz + 8);
        uint8_t * y = malloc(sz);
        assert_non_null(x);
        assert_non_null(y);
        memset(x, cases[i].fill, sz + 8);
        gen_packed(PATH, &def, 0, x, n, 1);

        struct file_s f;
        file_load(PATH, &f, 0);
        assert_true(chunk_count(&f, JLS_TAG_TRACK_FSR_DATA, 1) < 4);  // chunks were omitted
        free(f.data);

        struct jls_rd_s * rd = NULL;
        assert_int_equal(0, jls_rd_open(&rd, PATH));
        memset(y, 0x11, sz);
        assert_int_equal(0, jls_rd_fsr(rd, 1, 0, y, n));
        assert_memory_equal(x, y, sz);
        assert_int_equal(0, jls_rd_fsr(rd, 1, 256 * 5 + 8, y, 16));
        assert_memory_equal(x, y, (16 * bits) / 8);
        jls_rd_close(rd);
        free(x);
        free(y);
    }
    remove(PATH);
}


// --- C5: level-1 index & summary validation ---

static void test_fsr_level1_corrupt(void ** state) {
    (void) state;
    for (int mode = 0; mode < 4; ++mode) {
        gen_f32(PATH, &SIGNAL_1, 50000, 1);
        struct file_s f;
        file_load(PATH, &f, 0);
        uint8_t tag = (mode < 2) ? JLS_TAG_TRACK_FSR_INDEX : JLS_TAG_TRACK_FSR_SUMMARY;
        struct jls_chunk_header_s * h = chunk_nth(&f, tag, META_L1, 0);
        assert_non_null(h);
        struct jls_payload_header_s * p = chunk_payload(h);
        switch (mode) {
            case 0: p->entry_size_bits = 32; break;
            case 1: p->entry_count = 1000; break;           // > entries_per_summary / entries_per_data
            case 2: p->entry_size_bits = 64; break;
            case 3: p->entry_count = 100000; break;
            default: break;
        }
        chunk_resign(h);
        file_save(PATH, &f);

        struct jls_rd_s * rd = NULL;
        assert_int_equal(0, jls_rd_open(&rd, PATH));
        float y[16];
        assert_int_equal(JLS_ERROR_MESSAGE_INTEGRITY, jls_rd_fsr_f32(rd, 1, 100, y, ARRAY_SIZE(y)));
        assert_int_equal(0, jls_rd_fsr_f32(rd, 1, 30000, y, ARRAY_SIZE(y)));  // second index unaffected
        jls_rd_close(rd);
    }
    remove(PATH);
}


// --- C15: argument bounds without signed overflow ---

static void test_fsr_read_bounds_overflow(void ** state) {
    (void) state;
    gen_f32(PATH, &SIGNAL_1, 1000, 1);
    struct jls_rd_s * rd = NULL;
    assert_int_equal(0, jls_rd_open(&rd, PATH));
    float y[10];
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_rd_fsr_f32(rd, 1, INT64_MAX - 3, y, 10));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_rd_fsr_f32(rd, 1, 5, y, INT64_MAX));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_rd_fsr_f32(rd, 1, 995, y, 10));
    assert_int_equal(0, jls_rd_fsr_f32(rd, 1, 990, y, 10));
    jls_rd_close(rd);
    remove(PATH);
}


// --- R3: chunks larger than the default 1 MiB read buffer ---

static int32_t on_user_data_size(void * user_data, uint16_t chunk_meta, enum jls_storage_type_e storage_type,
                                 uint8_t * data, uint32_t data_size) {
    (void) storage_type;
    (void) data;
    if (0x22 == chunk_meta) {
        *((uint32_t *) user_data) = data_size;
    }
    return 0;
}

static void test_rd_chunk_large(void ** state) {
    (void) state;
    // payloads within 12 bytes of the buffer size used to loop forever
    const uint32_t sizes[] = {(1U << 20) - 4, (1U << 20) - 3, (1U << 20), (1U << 20) + 100, 3U << 20};
    uint8_t * x = calloc(1, 3U << 20);
    assert_non_null(x);
    for (size_t i = 0; i < ARRAY_SIZE(sizes); ++i) {
        struct jls_wr_s * wr = NULL;
        assert_int_equal(0, jls_wr_open(&wr, PATH));
        assert_int_equal(0, jls_wr_user_data(wr, 0x22, JLS_STORAGE_TYPE_BINARY, x, sizes[i]));
        assert_int_equal(0, jls_wr_close(wr));
        struct jls_rd_s * rd = NULL;
        assert_int_equal(0, jls_rd_open(&rd, PATH));
        uint32_t size = 0;
        assert_int_equal(0, jls_rd_user_data(rd, on_user_data_size, &size));
        assert_int_equal(sizes[i], size);
        jls_rd_close(rd);
    }
    free(x);

    // a data chunk > 1 MiB
    struct jls_signal_def_s def = signal_def_typed(JLS_DATATYPE_F32, 1U << 19, 1024, 1024, 16);
    gen_f32(PATH, &def, 3 << 19, 1);
    struct jls_rd_s * rd = NULL;
    assert_int_equal(0, jls_rd_open(&rd, PATH));
    float y[4];
    assert_int_equal(0, jls_rd_fsr_f32(rd, 1, (1 << 20) + 7, y, 4));
    assert_true(y[0] == (float) (((1 << 20) + 7) % 1000));
    jls_rd_close(rd);
    remove(PATH);
}


// --- C6, C7, C8, C19: truncation recovery on open ---

// C6: a crash tail of tiny data chunks used to overflow the rebuilt index.
static void test_recovery_tiny_data_chunks(void ** state) {
    (void) state;
    const int count = 2000;
    gen_f32(PATH, &SIGNAL_1, 30000, 1);
    struct file_s f;
    file_load(PATH, &f, (size_t) count * 64);
    int n = chunk_count(&f, JLS_TAG_TRACK_FSR_DATA, 1);
    struct jls_chunk_header_s * last = chunk_nth(&f, JLS_TAG_TRACK_FSR_DATA, 1, n - 1);
    struct jls_payload_header_s * lp = chunk_payload(last);
    int64_t timestamp = lp->timestamp + lp->entry_count;
    int64_t prev = chunk_offset(&f, last);
    for (int i = 0; i < count; ++i) {
        uint8_t payload[sizeof(struct jls_fsr_data_s) + sizeof(float)];
        struct jls_fsr_data_s * d = (struct jls_fsr_data_s *) payload;
        d->header.timestamp = timestamp + i;
        d->header.entry_count = 1;
        d->header.entry_size_bits = 32;
        d->header.rsv16 = 0;
        d->data[0] = 1.0f;
        int64_t offset = (int64_t) f.size;
        struct jls_chunk_header_s * p = (struct jls_chunk_header_s *) (f.data + prev);
        p->item_next = (uint64_t) offset;
        chunk_resign(p);
        chunk_append(&f, JLS_TAG_TRACK_FSR_DATA, 1, (uint64_t) prev, payload, sizeof(payload));
        prev = offset;
    }
    file_save(PATH, &f);

    struct jls_rd_s * rd = NULL;
    int32_t rc = jls_rd_open(&rd, PATH);
    if (0 == rc) {
        float y[16];
        jls_rd_fsr_f32(rd, 1, 100, y, ARRAY_SIZE(y));
        jls_rd_close(rd);
    }
    remove(PATH);
}

// C7: recovery must bound the untrusted index entry_count by the payload.
static void test_recovery_index_entry_count(void ** state) {
    (void) state;
    gen_f32(PATH, &SIGNAL_1, 50000, 1);
    struct file_s f;
    file_load(PATH, &f, 64);
    int n = chunk_count(&f, JLS_TAG_TRACK_FSR_INDEX, META_L1);
    assert_true(n >= 2);
    for (int i = 0; i < n; ++i) {
        struct jls_chunk_header_s * h = chunk_nth(&f, JLS_TAG_TRACK_FSR_INDEX, META_L1, i);
        assert_non_null(h);
        struct jls_payload_header_s * p = chunk_payload(h);
        p->entry_count = 131072 + 2;
        chunk_resign(h);
    }
    uint8_t ud[8] = {0};
    chunk_append(&f, JLS_TAG_USER_DATA, 0, 0, ud, sizeof(ud));  // not closed: recover
    file_save(PATH, &f);

    struct jls_rd_s * rd = NULL;
    int32_t rc = jls_rd_open(&rd, PATH);
    if (0 == rc) {
        jls_rd_close(rd);
    }
    remove(PATH);
}

// C8: failed recovery must not append summaries built from damaged data.
static void test_recovery_failure_no_append(void ** state) {
    (void) state;
    gen_f32(PATH, &SIGNAL_1, 90000, 0);  // crash: 4 level-1 chunks, no level 2 chunk
    struct file_s f;
    file_load(PATH, &f, 0);
    int n = chunk_count(&f, JLS_TAG_TRACK_FSR_INDEX, META_L1);
    assert_true(n >= 3);
    struct jls_chunk_header_s * h = chunk_nth(&f, JLS_TAG_TRACK_FSR_INDEX, META_L1, n - 1);
    struct jls_payload_header_s * p = chunk_payload(h);
    p->entry_size_bits = 32;
    chunk_resign(h);
    file_save(PATH, &f);
    int64_t size = file_size(PATH);

    struct jls_rd_s * rd = NULL;
    assert_int_equal(JLS_ERROR_MESSAGE_INTEGRITY, jls_rd_open(&rd, PATH));
    assert_int_equal(size, file_size(PATH));
    remove(PATH);
}

// C19: the orphan relink must only join chunks of the same chain.
static void test_recovery_relink_same_chain(void ** state) {
    (void) state;
    gen_f32(PATH, &SIGNAL_1, 30000, 0);
    struct file_s f;
    file_load(PATH, &f, 0);
    struct jls_chunk_header_s * src = chunk_nth(&f, JLS_TAG_SOURCE_DEF, 1, 0);
    assert_non_null(src);
    assert_int_equal(0, src->item_next);  // final chunk in the source chain
    struct jls_chunk_header_s * tail = chunk_last(&f);
    assert_int_not_equal(JLS_TAG_SOURCE_DEF, tail->tag);
    tail->item_prev = (uint64_t) chunk_offset(&f, src);  // foreign chain
    chunk_resign(tail);
    int64_t src_offset = chunk_offset(&f, src);
    file_save(PATH, &f);

    // a relink would point the source chain at the tail chunk
    struct jls_rd_s * rd = NULL;
    assert_int_equal(0, jls_rd_open(&rd, PATH));
    jls_rd_close(rd);
    file_load(PATH, &f, 0);
    src = (struct jls_chunk_header_s *) (f.data + src_offset);
    assert_int_equal(JLS_TAG_SOURCE_DEF, src->tag);
    assert_int_equal(0, src->item_next);
    free(f.data);
    remove(PATH);
}


int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_signal_def_align_wrap_rejected),
            cmocka_unit_test(test_signal_def_align_sweep),
            cmocka_unit_test(test_signal_def_validate_fsr),
            cmocka_unit_test(test_signal_def_hostile_read),
            cmocka_unit_test(test_signal_def_duplicate_read),
            cmocka_unit_test(test_fsr_u1_sweep),
            cmocka_unit_test(test_fsr_u4_sweep),
            cmocka_unit_test(test_fsr_u4_exhaustive),
            cmocka_unit_test(test_fsr_u1_tail_read_bounds),
            cmocka_unit_test(test_fsr_data_timestamp_corrupt),
            cmocka_unit_test(test_fsr_omit_reconstruct_corrupt),
            cmocka_unit_test(test_fsr_omit_reconstruct_types),
            cmocka_unit_test(test_fsr_level1_corrupt),
            cmocka_unit_test(test_fsr_read_bounds_overflow),
            cmocka_unit_test(test_rd_chunk_large),
            cmocka_unit_test(test_recovery_tiny_data_chunks),
            cmocka_unit_test(test_recovery_index_entry_count),
            cmocka_unit_test(test_recovery_failure_no_append),
            cmocka_unit_test(test_recovery_relink_same_chain),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
