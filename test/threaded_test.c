/*
 * Copyright 2014-2021 Jetperch LLC
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
#include "jls/reader.h"
#include "jls/threaded_writer.h"
#include "jls/format.h"
#include "jls/statistics.h"
#include "jls/time.h"
#include "jls/ec.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char * filename = "threaded_test_tmp.jls";

const struct jls_source_def_s SOURCE_3 = {
        .source_id = 3,
        .name = "source 3",
        .vendor = "vendor",
        .model = "model",
        .version = "version",
        .serial_number = "serial_number",
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
        .name = "signal 5",
        .units = "A",
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

static void test_data(void **state) {
    (void) state;
    struct jls_twr_s * wr = NULL;
    const int64_t sample_count = WINDOW_SIZE * 1000;
    float * signal = gen_triangle(1000, sample_count);
    assert_non_null(signal);

    assert_int_equal(0, jls_twr_open(&wr, filename));
    assert_int_equal(0, jls_twr_source_def(wr, &SOURCE_3));
    assert_int_equal(0, jls_twr_signal_def(wr, &SIGNAL_5));

    for (int sample_id = 0; sample_id < sample_count; sample_id += WINDOW_SIZE) {
        assert_int_equal(0, jls_twr_fsr_f32(wr, 5, sample_id, signal + sample_id, WINDOW_SIZE));
    }

    assert_int_equal(0, jls_twr_close(wr));

    struct jls_rd_s * rd = NULL;
    assert_int_equal(0, jls_rd_open(&rd, filename));
    struct jls_signal_def_s * signals = NULL;
    uint16_t count = 0;
    assert_int_equal(0, jls_rd_signals(rd, &signals, &count));
    assert_int_equal(2, count);
    assert_int_equal(0, signals[0].signal_id);
    assert_int_equal(5, signals[1].signal_id);
    int64_t samples = 0;
    assert_int_equal(0, jls_rd_fsr_length(rd, 5, &samples));
    assert_int_equal(sample_count, samples);

    // get entire first data chunk.
    float data[2000];
    assert_int_equal(0, jls_rd_fsr_f32(rd, 5, 0, data, 1000));
    assert_memory_equal(signal, data, 1000 * sizeof(float));

    // get span over 2nd - 4th data chunk.
    assert_int_equal(0, jls_rd_fsr_f32(rd, 5, 1999, data, 1002));
    assert_memory_equal(signal + 1999, data, 1002 * sizeof(float));

    // get last few samples
    assert_int_equal(0, jls_rd_fsr_f32(rd, 5, sample_count - 5, data, 5));
    assert_memory_equal(signal + sample_count - 5, data, 5 * sizeof(float));

    jls_rd_close(rd);
    free(signal);
    remove(filename);
}

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_data),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
