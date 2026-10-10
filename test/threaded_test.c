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
#include "jls/reader.h"
#include "jls/threaded_writer.h"
#include "jls/format.h"
#include "jls/statistics.h"
#include "jls/time.h"
#include "jls/ec.h"
#include "jls/backend.h"
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

static void test_validation(void **state) {
    (void) state;
    struct jls_twr_s * wr = NULL;
    float data[10] = {0};
    const char str[] = "hello";
    struct jls_signal_def_s signal_300 = SIGNAL_5;
    signal_300.signal_id = 300;

    assert_int_equal(0, jls_twr_open(&wr, filename));
    assert_int_equal(0, jls_twr_source_def(wr, &SOURCE_3));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_twr_signal_def(wr, &signal_300));
    assert_int_equal(0, jls_twr_signal_def(wr, &SIGNAL_5));

    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_twr_fsr_f32(wr, 300, 0, data, 10));
    assert_int_equal(JLS_ERROR_NOT_FOUND, jls_twr_fsr_f32(wr, 7, 0, data, 10));
    assert_int_equal(JLS_ERROR_NOT_SUPPORTED, jls_twr_fsr_f32(wr, 0, 0, data, 10));
    assert_int_equal(JLS_ERROR_TOO_BIG, jls_twr_fsr_f32(wr, 5, 0, data, 0x40000000U));
    assert_int_equal(JLS_ERROR_NOT_FOUND, jls_twr_fsr_omit_data(wr, 7, 1));
    assert_int_equal(JLS_ERROR_NOT_SUPPORTED, jls_twr_fsr_omit_data(wr, 0, 1));
    assert_int_equal(JLS_ERROR_NOT_FOUND, jls_twr_utc(wr, 7, 0, 0));
    assert_int_equal(JLS_ERROR_NOT_SUPPORTED, jls_twr_utc(wr, 0, 0, 0));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_twr_annotation(wr, 300, 0, 0.0f, JLS_ANNOTATION_TYPE_TEXT, 0,
                                                                     JLS_STORAGE_TYPE_STRING, (const uint8_t *) str, sizeof(str)));
    assert_int_equal(JLS_ERROR_NOT_FOUND, jls_twr_annotation(wr, 7, 0, 0.0f, JLS_ANNOTATION_TYPE_TEXT, 0,
                                                             JLS_STORAGE_TYPE_STRING, (const uint8_t *) str, sizeof(str)));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_twr_annotation(wr, 5, 0, 0.0f, (enum jls_annotation_type_e) 99, 0,
                                                                     JLS_STORAGE_TYPE_STRING, (const uint8_t *) str, sizeof(str)));
    // STRING payloads must be NUL-terminated within data_size
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_twr_annotation(wr, 5, 0, 0.0f, JLS_ANNOTATION_TYPE_TEXT, 0,
                                                                     JLS_STORAGE_TYPE_STRING, (const uint8_t *) str, 3));
    // storage_type must be defined and chunk_meta[15:12] is reserved
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_twr_user_data(wr, 1, (enum jls_storage_type_e) 0x102, (const uint8_t *) str, sizeof(str)));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_twr_user_data(wr, 0x1001, JLS_STORAGE_TYPE_BINARY, (const uint8_t *) str, 2));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_twr_annotation(wr, 5, 0, 0.0f, JLS_ANNOTATION_TYPE_TEXT, 0,
                                                                     JLS_STORAGE_TYPE_INVALID, NULL, 0));
    // messages larger than the ring buffer, without reading data
    assert_int_equal(JLS_ERROR_TOO_BIG, jls_twr_user_data(wr, 1, JLS_STORAGE_TYPE_BINARY, (const uint8_t *) str, 0xfffffff0U));
    assert_int_equal(JLS_ERROR_TOO_BIG, jls_twr_annotation(wr, 5, 0, 0.0f, JLS_ANNOTATION_TYPE_USER, 0,
                                                           JLS_STORAGE_TYPE_BINARY, (const uint8_t *) str, 0xfffffff0U));
    assert_int_equal(JLS_ERROR_TOO_BIG, jls_twr_fsr(wr, 5, 0, data, 64U * 1024 * 1024 / 4));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_twr_user_data(wr, 1, JLS_STORAGE_TYPE_BINARY, NULL, 4));

    // valid calls still work, including on the predefined VSR signal 0
    assert_int_equal(0, jls_twr_fsr_f32(wr, 5, 0, data, 10));
    assert_int_equal(0, jls_twr_annotation(wr, 0, 0, 0.0f, JLS_ANNOTATION_TYPE_TEXT, 0,
                                           JLS_STORAGE_TYPE_STRING, (const uint8_t *) str, sizeof(str)));
    assert_int_equal(0, jls_twr_user_data(wr, 1, JLS_STORAGE_TYPE_BINARY, NULL, 0));
    assert_int_equal(0, jls_twr_close(wr));
    remove(filename);
}

static void test_null_arguments(void **state) {
    (void) state;
    struct jls_twr_s * wr = NULL;
    float data[10] = {0};
    const char str[] = "hello";
    struct jls_signal_def_s signal_6 = SIGNAL_5;
    signal_6.signal_id = 6;
    signal_6.data_type = JLS_DATATYPE_F64;

    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_twr_open(NULL, filename));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_twr_open(&wr, NULL));
    assert_int_equal(0, jls_twr_flags_get(NULL));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_twr_flags_set(NULL, 0));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_twr_flush(NULL));
    assert_int_equal(0, jls_twr_close(NULL));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_twr_source_def(NULL, &SOURCE_3));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_twr_signal_def(NULL, &SIGNAL_5));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_twr_user_data(NULL, 1, JLS_STORAGE_TYPE_BINARY, NULL, 0));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_twr_fsr(NULL, 5, 0, data, 10));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_twr_fsr_f32(NULL, 5, 0, data, 10));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_twr_fsr_omit_data(NULL, 5, 1));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_twr_annotation(NULL, 5, 0, 0.0f, JLS_ANNOTATION_TYPE_TEXT, 0,
                                                                     JLS_STORAGE_TYPE_STRING, (const uint8_t *) str, sizeof(str)));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_twr_utc(NULL, 5, 0, 0));

    assert_int_equal(0, jls_twr_open(&wr, filename));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_twr_source_def(wr, NULL));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_twr_signal_def(wr, NULL));
    assert_int_equal(0, jls_twr_source_def(wr, &SOURCE_3));
    assert_int_equal(0, jls_twr_signal_def(wr, &SIGNAL_5));
    assert_int_equal(0, jls_twr_signal_def(wr, &signal_6));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_twr_fsr(wr, 5, 0, NULL, 10));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_twr_fsr_f32(wr, 5, 0, NULL, 10));
    // a float buffer for an f64 signal is too small
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_twr_fsr_f32(wr, 6, 0, data, 10));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_twr_annotation(wr, 5, 0, 0.0f, JLS_ANNOTATION_TYPE_USER, 0,
                                                                     JLS_STORAGE_TYPE_BINARY, NULL, 4));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_twr_annotation(wr, 5, 0, 0.0f, JLS_ANNOTATION_TYPE_TEXT, 0,
                                                                     JLS_STORAGE_TYPE_STRING, NULL, 0));
    assert_int_equal(0, jls_twr_fsr_f32(wr, 5, 0, data, 10));
    assert_int_equal(0, jls_twr_close(wr));
    remove(filename);
}

static int32_t on_user_data_hello(void * user_data, uint16_t chunk_meta, enum jls_storage_type_e storage_type,
                                  uint8_t * data, uint32_t data_size) {
    if (1 == chunk_meta) {
        assert_int_equal(JLS_STORAGE_TYPE_STRING, storage_type);
        assert_int_equal(6, data_size);
        assert_string_equal("hello", (const char *) data);
        *((int *) user_data) += 1;
    }
    return 0;
}

static int32_t on_annotation_hello(void * user_data, const struct jls_annotation_s * annotation) {
    assert_int_equal(JLS_STORAGE_TYPE_STRING, annotation->storage_type);
    assert_string_equal("hello", (const char *) annotation->data);
    *((int *) user_data) += 1;
    return 0;
}

// STRING and JSON data_size 0 means strlen(data) + 1, as for jls_wr_*.
static void test_string_size_zero(void **state) {
    (void) state;
    struct jls_twr_s * wr = NULL;
    const char str[] = "hello";
    assert_int_equal(0, jls_twr_open(&wr, filename));
    assert_int_equal(0, jls_twr_source_def(wr, &SOURCE_3));
    assert_int_equal(0, jls_twr_signal_def(wr, &SIGNAL_5));
    assert_int_equal(0, jls_twr_user_data(wr, 1, JLS_STORAGE_TYPE_STRING, (const uint8_t *) str, 0));
    assert_int_equal(0, jls_twr_annotation(wr, 5, 0, 0.0f, JLS_ANNOTATION_TYPE_TEXT, 0,
                                           JLS_STORAGE_TYPE_STRING, (const uint8_t *) str, 0));
    assert_int_equal(0, jls_twr_close(wr));

    struct jls_rd_s * rd = NULL;
    int user_data_count = 0;
    int annotation_count = 0;
    assert_int_equal(0, jls_rd_open(&rd, filename));
    assert_int_equal(0, jls_rd_user_data(rd, on_user_data_hello, &user_data_count));
    assert_int_equal(0, jls_rd_annotations(rd, 5, 0, on_annotation_hello, &annotation_count));
    assert_int_equal(1, user_data_count);
    assert_int_equal(1, annotation_count);
    jls_rd_close(rd);
    remove(filename);
}

// W12: the first async write error latches and every later call returns it.
static void test_async_error_latch(void **state) {
    (void) state;
    struct jls_twr_s * wr = NULL;
    float * data = calloc(1000, sizeof(float));
    const char str[] = "hello";
    assert_int_equal(0, jls_twr_open(&wr, filename));
    assert_int_equal(0, jls_twr_source_def(wr, &SOURCE_3));
    assert_int_equal(0, jls_twr_signal_def(wr, &SIGNAL_5));
    assert_int_equal(0, jls_twr_flush(wr));
    jls_bk_write_limit_set(16384);  // the thread is idle: no race
    for (int i = 0; i < 100; ++i) {
        if (jls_twr_fsr_f32(wr, 5, i * 1000, data, 1000)) {
            break;
        }
    }
    assert_int_equal(JLS_ERROR_IO, jls_twr_flush(wr));
    jls_bk_write_limit_set(-1);  // space freed: later writes would succeed
    assert_int_equal(JLS_ERROR_IO, jls_twr_flush(wr));
    assert_int_equal(JLS_ERROR_IO, jls_twr_fsr_f32(wr, 5, 0, data, 1000));
    assert_int_equal(JLS_ERROR_IO, jls_twr_fsr(wr, 5, 0, data, 1000));
    assert_int_equal(JLS_ERROR_IO, jls_twr_fsr_omit_data(wr, 5, 1));
    assert_int_equal(JLS_ERROR_IO, jls_twr_utc(wr, 5, 0, 0));
    assert_int_equal(JLS_ERROR_IO, jls_twr_user_data(wr, 1, JLS_STORAGE_TYPE_STRING, (const uint8_t *) str, 0));
    assert_int_equal(JLS_ERROR_IO, jls_twr_annotation(wr, 5, 0, 0.0f, JLS_ANNOTATION_TYPE_TEXT, 0,
                                                      JLS_STORAGE_TYPE_STRING, (const uint8_t *) str, 0));
    assert_int_equal(JLS_ERROR_IO, jls_twr_source_def(wr, &SOURCE_3));
    assert_int_equal(JLS_ERROR_IO, jls_twr_close(wr));
    free(data);
    remove(filename);
}

#if !defined(_WIN32)
#include <pthread.h>
#include <unistd.h>

struct close_arg_s {
    struct jls_twr_s * wr;
    pthread_mutex_t mutex;
    int32_t rc;
    int done;
};

static void * close_thread(void * arg) {
    struct close_arg_s * c = (struct close_arg_s *) arg;
    int32_t rc = jls_twr_close(c->wr);
    pthread_mutex_lock(&c->mutex);
    c->rc = rc;
    c->done = 1;
    pthread_mutex_unlock(&c->mutex);
    return NULL;
}

// W7: close completes with the ring full and the writer thread blocked.
// Close sets quit_req rather than sending a message, so it cannot time out.
static void test_close_ring_full(void **state) {
    (void) state;
    struct jls_twr_s * wr = NULL;
    const uint32_t length = 1000000;
    float * data = calloc(length, sizeof(float));
    assert_int_equal(0, jls_twr_open(&wr, filename));
    assert_int_equal(0, jls_twr_source_def(wr, &SOURCE_3));
    assert_int_equal(0, jls_twr_signal_def(wr, &SIGNAL_5));
    struct jls_bkt_s * bk = *((struct jls_bkt_s **) wr);  // bk is the required first member
    jls_bkt_process_lock(bk);  // block the writer thread
    assert_int_equal(0, jls_twr_flags_set(wr, JLS_TWR_FLAG_DROP_ON_OVERFLOW));
    int64_t sample_id = 0;
    for (uint32_t sz = length; sz; sz /= 10) {  // fill the ring, never blocking
        while (0 == jls_twr_fsr_f32(wr, 5, sample_id, data, sz)) {
            sample_id += sz;
        }
    }

    struct close_arg_s c = {.wr = wr, .rc = -1, .done = 0};
    pthread_mutex_init(&c.mutex, NULL);
    pthread_t thread;
    assert_int_equal(0, pthread_create(&thread, NULL, close_thread, &c));
    usleep(200000);  // let close start while the writer thread is blocked
    jls_bkt_process_unlock(bk);
    int done = 0;
    for (int i = 0; !done && (i < 300); ++i) {
        usleep(100000);
        pthread_mutex_lock(&c.mutex);
        done = c.done;
        pthread_mutex_unlock(&c.mutex);
    }
    assert_true(done);  // otherwise close hangs forever
    pthread_join(thread, NULL);
    assert_int_equal(0, c.rc);
    pthread_mutex_destroy(&c.mutex);

    struct jls_rd_s * rd = NULL;
    int64_t samples = 0;
    assert_int_equal(0, jls_rd_open(&rd, filename));
    assert_int_equal(0, jls_rd_fsr_length(rd, 5, &samples));
    assert_int_equal(sample_id, samples);  // every queued message was written
    jls_rd_close(rd);
    free(data);
    remove(filename);
}
#endif

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_data),
            cmocka_unit_test(test_validation),
            cmocka_unit_test(test_null_arguments),
            cmocka_unit_test(test_string_size_zero),
            cmocka_unit_test(test_async_error_latch),
#if !defined(_WIN32)
            cmocka_unit_test(test_close_ring_full),
#endif
    };

    const char * filter = getenv("JLS_TEST_FILTER");  // run a subset, such as for negative controls
    if (filter) {
        cmocka_set_test_filter(filter);
    }
    return cmocka_run_group_tests(tests, NULL, NULL);
}
