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
#include <inttypes.h>


#define TAU_F (6.283185307179586f)
const char * filename = "jls_test_fsr_omit_tmp.jls";


const struct jls_source_def_s SOURCE_1 = {
        .source_id = 1,
        .name = "source 1",
        .vendor = "vendor 1",
        .model = "model 1",
        .version = "version 1",
        .serial_number = "serial_number 1",
};

const struct jls_signal_def_s SIGNAL_1 = {
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

const struct jls_signal_def_s SIGNAL_3 = {
        .signal_id = 3,
        .source_id = 1,
        .signal_type = JLS_SIGNAL_TYPE_FSR,
        .data_type = JLS_DATATYPE_U4,
        .sample_rate = 2000000,
        .samples_per_data = 65536,
        .sample_decimate_factor = 1024,
        .entries_per_summary = 1280,
        .summary_decimate_factor = 20,
        .annotation_decimate_factor = 100,
        .utc_decimate_factor = 100,
        .name = "current_range",
        .units = "",
};
#define WINDOW_SIZE (937)


static uint64_t random_next(uint64_t random) {
    return ((random ^ (random >> 8)) + 1) * 2654435761ULL;
}


float * gen_jls(int64_t sample_count) {
    float threshold = 500e-6f;
    uint32_t period_samples = SIGNAL_1.sample_rate;
    uint32_t window_size = (uint32_t) (period_samples / 10);
    float * y = malloc(sizeof(float) * (size_t) sample_count);
    if (!y) {
        return NULL;
    }

    uint64_t random = 0;
    uint64_t r1;
    uint64_t r2;
    float f1;
    float f2;
    float k;
    float g;
    float sigma = 1e-6f;

    for (int64_t i = 0; i < sample_count; i += 2) {
        r1 = random = random_next(random);
        r2 = random = random_next(random);
        f1 = (r1 & 0xffffffff) / (float) 0xffffffff;
        f2 = (r2 & 0xffffffff) / (float) 0xffffffff;

        if (0 == ((i / period_samples) & 1)) {
            k = 1.0f;
        } else {
            k = 100e-6f;
        }
        // https://en.wikipedia.org/wiki/Box%E2%80%93Muller_transform
        g = sigma * sqrtf(-2.0f * logf(f1));
        f2 *= TAU_F;
        y[i + 0] = k + g * cosf(f2);
        y[i + 1] = k + g * sinf(f2);
    }

    struct jls_wr_s * wr = NULL;
    assert_int_equal(0, jls_wr_open(&wr, filename));
    assert_int_equal(0, jls_wr_source_def(wr, &SOURCE_1));
    assert_int_equal(0, jls_wr_signal_def(wr, &SIGNAL_1));

    for (int64_t sample_id = 0; sample_id < sample_count; sample_id += window_size) {
        int64_t y_end = sample_id + sample_count - 1;
        y_end = (y_end < sample_count) ? y_end : sample_count;
        uint32_t omit = (y[sample_id] < threshold) && (y[y_end] < threshold) ? 1 : 0;
        assert_int_equal(0, jls_wr_fsr_omit_data(wr, 1, omit));
        assert_int_equal(0, jls_wr_fsr_f32(wr, 1, sample_id, y + sample_id, window_size));
    }

    assert_int_equal(0, jls_wr_close(wr));
    return y;
}

static void test_samples(void **state) {
    (void) state;
    int64_t sample_count = 4 * ((int64_t) SIGNAL_1.sample_rate);
    float * signal = gen_jls(sample_count);
    assert_non_null(signal);

    float * y = malloc(sizeof(float) * (size_t) sample_count);
    assert_non_null(y);

    struct jls_rd_s * rd = NULL;
    assert_int_equal(0, jls_rd_open(&rd, filename));
    int64_t samples = 0;
    assert_int_equal(0, jls_rd_fsr_length(rd, 1, &samples));
    assert_int_equal(sample_count, samples);  // rounded up based upon signal_def
    assert_int_equal(0, jls_rd_fsr_f32(rd, 1, 0, y, sample_count));

    for (int64_t i = 0; i < sample_count; ++i) {
        // printf("%" PRIi64 "\n", i);
        assert_float_equal(signal[i], y[i], 10e-6);
    }

    assert_int_equal(0, jls_rd_fsr_f32(rd, 1, 150000, y, 10));  // only omitted samples
    for (int64_t i = 0; i < 10; ++i) {
        assert_float_equal(signal[150000 + i], y[i], 10e-6);
    }

    free(signal);
    free(y);
    remove(filename);
}

static void test_summary(void **state) {
    (void) state;
    int64_t sample_count = 4 * ((int64_t) SIGNAL_1.sample_rate);
    float * signal = gen_jls(sample_count);
    assert_non_null(signal);

    float * y = malloc(sizeof(float) * (size_t) sample_count);
    assert_non_null(y);

    struct jls_rd_s * rd = NULL;
    assert_int_equal(0, jls_rd_open(&rd, filename));
    int64_t samples = 0;
    assert_int_equal(0, jls_rd_fsr_length(rd, 1, &samples));
    assert_int_equal(sample_count, samples);  // rounded up based upon signal_def

    double data[100][4];
    assert_int_equal(0, jls_rd_fsr_statistics(rd, 1, 0, 2, &data[0][0], 100));
    assert_int_equal(0, jls_rd_fsr_statistics(rd, 1, 0, 2000, &data[0][0], 100));

    free(signal);
    free(y);
    // remove(filename);
}

static void test_u4(void **state) {
    (void) state;
    struct jls_wr_s * wr = NULL;
    size_t sz_samples = 6000000;
    size_t sz_bytes = sz_samples / 2;
    uint8_t * data = malloc(sz_bytes);
    data = memset(data, 0x33, sz_bytes);
    for (size_t i = 1000000; i < 1010000; ++i) {
        data[i] = 0x44;
    }
    for (size_t i = 2000000; i < 2010000; ++i) {
        data[i] = 0x44;
    }

    assert_int_equal(0, jls_wr_open(&wr, filename));
    assert_int_equal(0, jls_wr_source_def(wr, &SOURCE_1));
    assert_int_equal(0, jls_wr_signal_def(wr, &SIGNAL_3));
    assert_int_equal(0, jls_wr_fsr(wr, 3, 0, data, sz_samples));
    assert_int_equal(0, jls_wr_close(wr));

    struct jls_rd_s * rd = NULL;
    assert_int_equal(0, jls_rd_open(&rd, filename));

    uint8_t u4_fsr[16];
    memset(u4_fsr, 0, sizeof(u4_fsr));
    assert_int_equal(0, jls_rd_fsr(rd, 3, 65534, u4_fsr, 16));
    for (int64_t i = 0; i < 8; ++i) {
        // printf("%lld 0x%02x\n", 65534 + i * 2, u4_fsr[i]);
        assert_true((u4_fsr[i] & 0x0f) >= 3);
    }

    memset(u4_fsr, 0, sizeof(u4_fsr));
    assert_int_equal(0, jls_rd_fsr(rd, 3, 65535, u4_fsr, 16));
    for (int64_t i = 0; i < 8; ++i) {
        // printf("%lld 0x%02x\n", 65534 + i * 2, u4_fsr[i]);
        assert_true((u4_fsr[i] & 0x0f) >= 3);
    }

    int64_t len = 2200;
    double * f64_stats = malloc(len * 4 * sizeof(double));
    assert_int_equal(0, jls_rd_fsr_statistics(rd, 3, 0, 1000, f64_stats, len));
    for (int64_t i = 0; i < len; ++i) {
        assert_true(f64_stats[4 * i] > 2.5);
    }
}

static void on_log_recv(const char * msg) {
    printf("%s", msg);
}

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_samples),
            cmocka_unit_test(test_summary),
            cmocka_unit_test(test_u4),
    };

    jls_log_register(on_log_recv);
    int rc = cmocka_run_group_tests(tests, NULL, NULL);
    jls_log_unregister();
    return rc;
}
