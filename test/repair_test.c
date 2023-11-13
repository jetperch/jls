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
#include "jls/backend.h"
#include "jls/core.h"
#include "jls/ec.h"
#include "jls/log.h"
#include "jls/raw.h"
#include "jls/time.h"
#include "jls/reader.h"
#include "jls/writer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define SKIP_BASIC 0
#define SKIP_REALWORLD 1

#define ARRAY_SIZE(x) ( sizeof(x) / sizeof((x)[0]) )
const char * filename = "jls_test_tmp.jls";
const uint8_t USER_DATA_1[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
const uint16_t CHUNK_META_1 = 0x0123;
const uint16_t CHUNK_META_2 = 0x0BEE;
const uint16_t CHUNK_META_3 = 0x0ABC;
const char STRING_1[] = "hello world";
const char JSON_1[] = "{\"hello\": \"world\"}";


enum gen_close_e {
    GEN_CLOSE,
    GEN_SKIP_CLOSE
};


const struct jls_source_def_s SOURCE_1 = {
        .source_id = 1,
        .name = "source 1",
        .vendor = "vendor 1",
        .model = "model 1",
        .version = "version 1",
        .serial_number = "serial_number 1",
};

const struct jls_source_def_s SOURCE_3 = {
        .source_id = 3,
        .name = "source 3",
        .vendor = "vendor 3",
        .model = "model 3",
        .version = "version 3",
        .serial_number = "serial_number 3",
};

const struct jls_signal_def_s SIGNAL_5 = {
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

const struct jls_signal_def_s SIGNAL_6 = {
        .signal_id = 6,
        .source_id = 3,
        .signal_type = JLS_SIGNAL_TYPE_VSR,
        .data_type = JLS_DATATYPE_F32,
        .sample_rate = 0,
        .samples_per_data = 1000000,
        .sample_decimate_factor = 100,
        .entries_per_summary = 200,
        .summary_decimate_factor = 100,
        .annotation_decimate_factor = 100,
        .utc_decimate_factor = 100,
        .name = "voltage",
        .units = "V",
};

const struct jls_signal_def_s SIGNAL_9_U1 = {
        .signal_id = 9,
        .source_id = 3,
        .signal_type = JLS_SIGNAL_TYPE_FSR,
        .data_type = JLS_DATATYPE_U1,
        .sample_rate = 100000,
        .samples_per_data = 1000,
        .sample_decimate_factor = 100,
        .entries_per_summary = 200,
        .summary_decimate_factor = 100,
        .annotation_decimate_factor = 100,
        .utc_decimate_factor = 100,
        .name = "gpi[0]",
        .units = "",
};

#define WINDOW_SIZE (937)

float * gen_triangle(uint32_t period_samples, int64_t length_samples) {
    float * y = malloc(sizeof(float) * (size_t) length_samples);
    if (!y) {
        return NULL;
    }
    int64_t v_max = (period_samples + 1) / 2;
    float offset = v_max / 2.0f;
    float gain = 2.0f / v_max;
    int64_t v = v_max / 2;
    int64_t incr = 1;
    for (int64_t i = 0; i < length_samples; ++i) {
        y[i] = gain * (v - offset);
        if (v <= 0) {
            incr = 1;
        } else if (v >= v_max) {
            incr = -1;
        }
        v += incr;
    }
    return y;
}

static float * gen_truncate(int64_t sample_count, size_t truncate, enum gen_close_e gen_close) {
    struct jls_wr_s * wr = NULL;
    float * signal = gen_triangle(1000, sample_count);
    assert_non_null(signal);

    assert_int_equal(0, jls_wr_open(&wr, filename));
    assert_int_equal(0, jls_wr_source_def(wr, &SOURCE_3));
    assert_int_equal(0, jls_wr_signal_def(wr, &SIGNAL_5));

    int64_t utc = JLS_TIME_YEAR;   // one year after start of epoch

    for (int sample_id = 0; sample_id < sample_count; sample_id += WINDOW_SIZE) {
        assert_int_equal(0, jls_wr_fsr_f32(wr, 5, sample_id, signal + sample_id, WINDOW_SIZE));
        assert_int_equal(0, jls_wr_utc(wr, 5, sample_id, utc + JLS_COUNTER_TO_TIME(sample_id, SIGNAL_5.sample_rate)));
    }

    if (gen_close == GEN_CLOSE) {
        assert_int_equal(0, jls_wr_close(wr));
    } else {
        struct jls_core_s * core = (struct jls_core_s *) wr;
        jls_bk_fclose(jls_raw_backend(core->raw));
    }

    if (truncate > 0) {
        struct jls_raw_s * raw = NULL;
        assert_int_equal(0, jls_raw_open(&raw, filename, "a"));
        struct jls_bkf_s * backend = NULL;
        backend = jls_raw_backend(raw);
        int64_t end_pos = backend->fend - (int64_t) truncate;
        assert_non_null(backend);
        assert_int_equal(0, jls_raw_chunk_seek(raw, end_pos));
        assert_int_equal(0, jls_bk_truncate(backend));
        assert_int_equal(0, jls_raw_close(raw));
    }
    return signal;
}

static void test_truncate_end_only(void **state) {
    (void) state;
    int64_t sample_count = WINDOW_SIZE * 1000;
    gen_truncate(sample_count, sizeof(struct jls_chunk_header_s), GEN_CLOSE);
    struct jls_rd_s * rd = NULL;
    assert_int_equal(0, jls_rd_open(&rd, filename));  // automatically repaired
    int64_t samples = 0;
    assert_int_equal(0, jls_rd_fsr_length(rd, 5, &samples));
    assert_int_equal(sample_count, samples);
    remove(filename);
}

static void test_truncate_summary(void **state) {
    (void) state;
    int64_t sample_count = WINDOW_SIZE * 1000;
    double signal_mean = 0.0;

    float * signal = gen_truncate(sample_count, 15 * sizeof(struct jls_chunk_header_s), GEN_CLOSE);
    for (int64_t i = 0; i < sample_count; ++i) {
        signal_mean += signal[i];
    }
    signal_mean = signal_mean / sample_count;

    struct jls_rd_s * rd = NULL;
    double data[4];
    assert_int_equal(0, jls_rd_open(&rd, filename));  // automatically repaired
    int64_t samples = 0;
    assert_int_equal(0, jls_rd_fsr_length(rd, 5, &samples));
    assert_int_equal(sample_count, samples);
    assert_int_equal(0, jls_rd_fsr_statistics(rd, 5, 0, sample_count, data, 1));
    assert_float_equal(signal_mean, data[0], 1e-9);
    remove(filename);
}

static void test_truncate_samples(void **state) {
    (void) state;
    int64_t sample_count = WINDOW_SIZE * 1000;
    int64_t sample_count_truncated = 0x1e780;
    double signal_mean = 0.0;

    float * signal = gen_truncate(sample_count, 3500000, GEN_CLOSE);
    for (int64_t i = 0; i < sample_count_truncated; ++i) {
        signal_mean += signal[i];
    }
    signal_mean = signal_mean / sample_count_truncated;

    struct jls_rd_s * rd = NULL;
    double data[4];
    assert_int_equal(0, jls_rd_open(&rd, filename));  // automatically repaired
    int64_t samples = 0;
    assert_int_equal(0, jls_rd_fsr_length(rd, 5, &samples));
    assert_int_equal(sample_count_truncated, samples);
    assert_int_equal(0, jls_rd_fsr_statistics(rd, 5, 0, sample_count_truncated, data, 1));
    assert_float_equal(signal_mean, data[0], 1e-9);
    remove(filename);
}

static void test_truncate_samples_unclosed(void **state) {
    (void) state;
    int64_t sample_count = WINDOW_SIZE * 1000;
    int64_t sample_count_truncated = 0xe4840;
    double signal_mean = 0.0;

    float * signal = gen_truncate(sample_count, 0, GEN_SKIP_CLOSE);
    for (int64_t i = 0; i < sample_count_truncated; ++i) {
        signal_mean += signal[i];
    }
    signal_mean = signal_mean / sample_count_truncated;

    struct jls_rd_s * rd = NULL;
    double data[4];
    assert_int_equal(0, jls_rd_open(&rd, filename));  // automatically repaired
    int64_t samples = 0;
    assert_int_equal(0, jls_rd_fsr_length(rd, 5, &samples));
    assert_int_equal(sample_count_truncated, samples);
    assert_int_equal(0, jls_rd_fsr_statistics(rd, 5, 0, sample_count_truncated, data, 1));
    assert_float_equal(signal_mean, data[0], 1e-9);
    remove(filename);
}


static void on_log_recv(const char * msg) {
    printf("%s", msg);
}

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_truncate_end_only),
            cmocka_unit_test(test_truncate_summary),
            cmocka_unit_test(test_truncate_samples),
            cmocka_unit_test(test_truncate_samples_unclosed),
    };

    jls_log_register(on_log_recv);
    int rc = cmocka_run_group_tests(tests, NULL, NULL);
    jls_log_unregister();
    return rc;
}
