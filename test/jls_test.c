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
#include "jls.h"
#include "jls/writer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define SKIP_BASIC 0
#define SKIP_REALWORLD 1

const char * filename = "jls_test_tmp.jls";
const uint8_t USER_DATA_1[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
const uint16_t CHUNK_META_1 = 0x0123;
const uint8_t USER_DATA_2[] = {0x11, 0x22, 0xab, 0x34, 0x45, 0x46, 0x42, 42, 42, 3, 7};
const uint16_t CHUNK_META_2 = 0x0BEE;
const uint16_t CHUNK_META_3 = 0x0ABC;
const char STRING_1[] = "hello world";
const char JSON_1[] = "{\"hello\": \"world\"}";


const struct jls_source_def_s SOURCE_1 = {
        .source_id = 1,
        .name = "source 1",
        .vendor = "vendor",
        .model = "model",
        .version = "version",
        .serial_number = "serial_number",
};

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
        .name = "signal 6",
        .units = "V",
};

#if !SKIP_BASIC
static void test_source(void **state) {
    (void) state;
    struct jls_wr_s * wr = NULL;
    struct jls_rd_s * rd = NULL;
    assert_int_equal(0, jls_wr_open(&wr, filename));
    assert_int_equal(0, jls_wr_source_def(wr, &SOURCE_3));
    assert_int_equal(0, jls_wr_source_def(wr, &SOURCE_1));
    assert_int_equal(0, jls_wr_close(wr));

    assert_int_equal(0, jls_rd_open(&rd, filename));

    struct jls_source_def_s * sources = NULL;
    uint16_t count = 0;
    assert_int_equal(0, jls_rd_sources(rd, &sources, &count));
    assert_int_equal(3, count);
    assert_int_equal(0, sources[0].source_id);
    assert_int_equal(1, sources[1].source_id);
    assert_int_equal(3, sources[2].source_id);
    assert_string_equal(SOURCE_1.name, sources[1].name);
    assert_string_equal(SOURCE_1.vendor, sources[1].vendor);
    assert_string_equal(SOURCE_1.model, sources[1].model);
    assert_string_equal(SOURCE_1.version, sources[1].version);
    assert_string_equal(SOURCE_1.serial_number, sources[1].serial_number);
    assert_string_equal(SOURCE_3.name, sources[2].name);
    jls_rd_close(rd);
    remove(filename);
}

static void test_wr_source_duplicate(void **state) {
    (void) state;
    struct jls_wr_s * wr = NULL;
    // struct jls_rd_s * rd = NULL;
    assert_int_equal(0, jls_wr_open(&wr, filename));
    assert_int_equal(0, jls_wr_source_def(wr, &SOURCE_1));
    assert_int_equal(JLS_ERROR_ALREADY_EXISTS, jls_wr_source_def(wr, &SOURCE_1));
    assert_int_equal(0, jls_wr_close(wr));
    remove(filename);
}

static int32_t on_annotation(void * user_data, const struct jls_annotation_s * annotation) {
    (void) user_data;
    int64_t timestamp = annotation->timestamp;
    uint8_t annotation_type = annotation->annotation_type;
    uint8_t storage_type = annotation->storage_type;
    uint8_t group_id = annotation->group_id;
    float y = annotation->y;
    uint32_t data_size = annotation->data_size;
    const uint8_t * data = annotation->data;

    check_expected(timestamp);
    check_expected(annotation_type);
    check_expected(group_id);
    check_expected(storage_type);
    if (isfinite(y)) {
        check_expected(y);
    } else {
        int y_nan = 1;
        check_expected(y_nan);
    }
    check_expected(data_size);
    check_expected_ptr(data);
    return 0;
}

#define expect_annotation(timestamp_, y_value_, annotation_type_, group_id_, storage_type_, data_, data_size_) \
    expect_value(on_annotation, timestamp, timestamp_);                                   \
    if (isfinite(y_value_)) {                                                             \
        expect_value(on_annotation, y, y_value_);                                         \
    } else {                                                                              \
        expect_value(on_annotation, y_nan, 1);                                            \
    }                                                                                     \
    expect_value(on_annotation, annotation_type, annotation_type_);                       \
    expect_value(on_annotation, group_id, group_id_);                                     \
    expect_value(on_annotation, storage_type, storage_type_);                             \
    expect_value(on_annotation, data_size, data_size_);                                   \
    expect_memory(on_annotation, data, data_, data_size_)

static void test_annotation(void **state) {
    (void) state;
    int64_t now = jls_now();
    struct jls_wr_s * wr = NULL;
    struct jls_rd_s * rd = NULL;
    assert_int_equal(0, jls_wr_open(&wr, filename));
    assert_int_equal(0, jls_wr_annotation(wr, 0, now + 0 * JLS_TIME_MILLISECOND, NAN,
                                          JLS_ANNOTATION_TYPE_TEXT, 0, JLS_STORAGE_TYPE_STRING,
                                          (const uint8_t *) STRING_1, 0));
    assert_int_equal(0, jls_wr_annotation(wr, 0, now + 1 * JLS_TIME_MILLISECOND, 1.0f,
                                          JLS_ANNOTATION_TYPE_VERTICAL_MARKER, 1, JLS_STORAGE_TYPE_STRING,
                                          (const uint8_t *) "1", 0));
    assert_int_equal(0, jls_wr_annotation(wr, 0, now + 2 * JLS_TIME_MILLISECOND, 2.0f,
                                          JLS_ANNOTATION_TYPE_USER, 2, JLS_STORAGE_TYPE_BINARY,
                                          USER_DATA_1, sizeof(USER_DATA_1)));
    assert_int_equal(0, jls_wr_annotation(wr, 0, now + 3 * JLS_TIME_MILLISECOND, 3.0f,
                                          JLS_ANNOTATION_TYPE_USER, 3, JLS_STORAGE_TYPE_STRING,
                                          (const uint8_t *) STRING_1, 0));
    assert_int_equal(0, jls_wr_annotation(wr, 0, now + 4 * JLS_TIME_MILLISECOND, 4.0f,
                                          JLS_ANNOTATION_TYPE_USER, 4, JLS_STORAGE_TYPE_JSON,
                                          (const uint8_t *) JSON_1, 0));
    assert_int_equal(0, jls_wr_close(wr));

    assert_int_equal(0, jls_rd_open(&rd, filename));

    expect_annotation(now + 0 * JLS_TIME_MILLISECOND, NAN,
                      JLS_ANNOTATION_TYPE_TEXT, 0, JLS_STORAGE_TYPE_STRING,
                      (const uint8_t *) STRING_1, sizeof(STRING_1));
    expect_annotation(now + 1 * JLS_TIME_MILLISECOND, 1.0f,
                      JLS_ANNOTATION_TYPE_VERTICAL_MARKER, 1, JLS_STORAGE_TYPE_STRING,
                      (const uint8_t *) "1", 2);
    expect_annotation(now + 2 * JLS_TIME_MILLISECOND, 2.0f,
                      JLS_ANNOTATION_TYPE_USER, 2, JLS_STORAGE_TYPE_BINARY,
                      USER_DATA_1, sizeof(USER_DATA_1));
    expect_annotation(now + 3 * JLS_TIME_MILLISECOND, 3.0f,
                      JLS_ANNOTATION_TYPE_USER, 3, JLS_STORAGE_TYPE_STRING,
                      (const uint8_t *) STRING_1, sizeof(STRING_1));
    expect_annotation(now + 4 * JLS_TIME_MILLISECOND, 4.0f,
                      JLS_ANNOTATION_TYPE_USER, 4, JLS_STORAGE_TYPE_JSON,
                      (const uint8_t *) JSON_1, sizeof(JSON_1));
    assert_int_equal(0, jls_rd_annotations(rd, 0, 0, on_annotation, NULL));

    jls_rd_close(rd);
    remove(filename);
}

static void test_annotation_seek(void **state) {
    (void) state;
    struct jls_wr_s * wr = NULL;
    struct jls_rd_s * rd = NULL;
    assert_int_equal(0, jls_wr_open(&wr, filename));
    assert_int_equal(0, jls_wr_annotation(wr, 0, 0, NAN,
                                          JLS_ANNOTATION_TYPE_TEXT, 0, JLS_STORAGE_TYPE_STRING,
                                          (const uint8_t *) STRING_1, 0));
    assert_int_equal(0, jls_wr_annotation(wr, 0, 1, NAN,
                                          JLS_ANNOTATION_TYPE_TEXT, 0, JLS_STORAGE_TYPE_STRING,
                                          (const uint8_t *) STRING_1, 0));
    assert_int_equal(0, jls_wr_annotation(wr, 0, JLS_TIME_SECOND, NAN,
                                          JLS_ANNOTATION_TYPE_TEXT, 0, JLS_STORAGE_TYPE_STRING,
                                          (const uint8_t *) STRING_1, 0));
    assert_int_equal(0, jls_wr_close(wr));

    assert_int_equal(0, jls_rd_open(&rd, filename));
    expect_annotation(JLS_TIME_SECOND, NAN,
                      JLS_ANNOTATION_TYPE_TEXT, 0, JLS_STORAGE_TYPE_STRING,
                      (const uint8_t *) STRING_1, sizeof(STRING_1));
    assert_int_equal(0, jls_rd_annotations(rd, 0, JLS_TIME_SECOND, on_annotation, NULL));

    jls_rd_close(rd);
    remove(filename);
}

static void test_hmarker(void **state) {
    (void) state;
    struct jls_wr_s * wr = NULL;
    struct jls_rd_s * rd = NULL;
    assert_int_equal(0, jls_wr_open(&wr, filename));
    assert_int_equal(0, jls_wr_annotation(wr, 0, 0, 0.0f,
                                          JLS_ANNOTATION_TYPE_HORIZONTAL_MARKER, 0, JLS_STORAGE_TYPE_STRING,
                                          (const uint8_t *) "1a", 3));
    assert_int_equal(0, jls_wr_annotation(wr, 0, 0, 1.0f,
                                          JLS_ANNOTATION_TYPE_HORIZONTAL_MARKER, 1, JLS_STORAGE_TYPE_STRING,
                                          (const uint8_t *) "1b", 3));
    assert_int_equal(0, jls_wr_close(wr));

    assert_int_equal(0, jls_rd_open(&rd, filename));

    expect_annotation(0, 0.0f,
                      JLS_ANNOTATION_TYPE_HORIZONTAL_MARKER, 0, JLS_STORAGE_TYPE_STRING,
                      (const uint8_t *) "1a", 3);
    expect_annotation(0, 1.0f,
                      JLS_ANNOTATION_TYPE_HORIZONTAL_MARKER, 1, JLS_STORAGE_TYPE_STRING,
                      (const uint8_t *) "1b", 3);
    assert_int_equal(0, jls_rd_annotations(rd, 0, 0, on_annotation, NULL));

    jls_rd_close(rd);
    remove(filename);
}

static int32_t on_user_data(void * user_data,
                            uint16_t chunk_meta, enum jls_storage_type_e storage_type,
                            uint8_t * data, uint32_t data_size) {
    (void) user_data;
    check_expected(chunk_meta);
    check_expected(storage_type);
    check_expected(data_size);
    check_expected_ptr(data);
    return 0;
}

#define expect_user_data(chunk_meta_, storage_type_, data_, data_size_) \
    expect_value(on_user_data, chunk_meta, chunk_meta_);                \
    expect_value(on_user_data, storage_type, storage_type_);            \
    expect_value(on_user_data, data_size, data_size_);                  \
    expect_memory(on_user_data, data, data_, data_size_)

static void test_user_data(void **state) {
    (void) state;
    struct jls_wr_s * wr = NULL;
    struct jls_rd_s * rd = NULL;
    assert_int_equal(0, jls_wr_open(&wr, filename));
    assert_int_equal(0, jls_wr_user_data(wr, CHUNK_META_1, JLS_STORAGE_TYPE_BINARY, USER_DATA_1, sizeof(USER_DATA_1)));
    assert_int_equal(0, jls_wr_user_data(wr, CHUNK_META_2, JLS_STORAGE_TYPE_STRING, (const uint8_t *) STRING_1, 0));
    assert_int_equal(0, jls_wr_user_data(wr, CHUNK_META_3, JLS_STORAGE_TYPE_JSON, (const uint8_t *) JSON_1, 0));
    assert_int_equal(0, jls_wr_close(wr));

    assert_int_equal(0, jls_rd_open(&rd, filename));

    expect_user_data(CHUNK_META_1, JLS_STORAGE_TYPE_BINARY, USER_DATA_1, sizeof(USER_DATA_1));
    expect_user_data(CHUNK_META_2, JLS_STORAGE_TYPE_STRING, (const uint8_t *) STRING_1, sizeof(STRING_1));
    expect_user_data(CHUNK_META_3, JLS_STORAGE_TYPE_JSON, (const uint8_t *) JSON_1, sizeof(JSON_1));
    assert_int_equal(0, jls_rd_user_data(rd, on_user_data, NULL));

    jls_rd_close(rd);
    remove(filename);
}

int32_t on_utc(void * user_data, const struct jls_utc_summary_entry_s * utc, uint32_t size) {
    (void) user_data;
    for (uint32_t i = 0; i < size; ++i) {
        int64_t sample_id = utc[i].sample_id;
        int64_t timestamp = utc[i].timestamp;
        check_expected(sample_id);
        check_expected(timestamp);
    }
    return 0;
}

#define expect_utc(sample_id_, timestamp_)          \
    expect_value(on_utc, sample_id, sample_id_);    \
    expect_value(on_utc, timestamp, timestamp_);

static void utc_gen(uint32_t count, int64_t timestamp_start, int64_t timestamp_end) {
    struct jls_wr_s * wr = NULL;
    assert_int_equal(0, jls_wr_open(&wr, filename));
    assert_int_equal(0, jls_wr_source_def(wr, &SOURCE_3));
    assert_int_equal(0, jls_wr_signal_def(wr, &SIGNAL_5));
    for (int64_t i = 0; i < count; ++i) {
        int64_t sample_id = i * 10;
        int64_t timestamp = i * JLS_TIME_SECOND;
        assert_int_equal(0, jls_wr_utc(wr, 5, sample_id, timestamp));
        if ((timestamp >= timestamp_start) && (timestamp < timestamp_end)) {
            expect_utc(sample_id, timestamp);
        }
    }
    assert_int_equal(0, jls_wr_close(wr));
}

static void utc_check(int64_t sample_id) {
    struct jls_rd_s * rd = NULL;
    assert_int_equal(0, jls_rd_open(&rd, filename));
    assert_int_equal(0, jls_rd_utc(rd, 5, sample_id, on_utc, NULL));
    jls_rd_close(rd);
    remove(filename);
}

static void test_utc(void **state) {
    (void) state;
    utc_gen(SIGNAL_5.utc_decimate_factor * 5 + 10, 0, 1000000 * JLS_TIME_SECOND);
    utc_check(0);
}

static void test_utc_seek_first_block(void **state) {
    (void) state;
    utc_gen(SIGNAL_5.utc_decimate_factor * 5 + 10, 50 * JLS_TIME_SECOND, 1000000 * JLS_TIME_SECOND);
    utc_check(500);
}

static void test_utc_seek_second_block_start(void **state) {
    (void) state;
    utc_gen(SIGNAL_5.utc_decimate_factor * 5 + 10, 100 * JLS_TIME_SECOND, 1000000 * JLS_TIME_SECOND);
    utc_check(1000);
}

static void test_utc_seek_second_block_middle(void **state) {
    (void) state;
    utc_gen(SIGNAL_5.utc_decimate_factor * 5 + 10, 150 * JLS_TIME_SECOND, 1000000 * JLS_TIME_SECOND);
    utc_check(1500);
}

static void test_signal(void **state) {
    (void) state;
    struct jls_wr_s * wr = NULL;
    struct jls_rd_s * rd = NULL;
    assert_int_equal(0, jls_wr_open(&wr, filename));
    assert_int_equal(0, jls_wr_source_def(wr, &SOURCE_3));
    assert_int_equal(0, jls_wr_signal_def(wr, &SIGNAL_6));
    assert_int_equal(0, jls_wr_signal_def(wr, &SIGNAL_5));
    assert_int_equal(0, jls_wr_close(wr));

    assert_int_equal(0, jls_rd_open(&rd, filename));
    struct jls_signal_def_s * signals = NULL;
    uint16_t count = 0;
    assert_int_equal(0, jls_rd_signals(rd, &signals, &count));
    assert_int_equal(3, count);
    assert_int_equal(0, signals[0].signal_id);
    assert_int_equal(5, signals[1].signal_id);
    assert_int_equal(6, signals[2].signal_id);
    assert_int_equal(SIGNAL_5.source_id, signals[1].source_id);
    assert_int_equal(SIGNAL_5.signal_type, signals[1].signal_type);
    assert_int_equal(SIGNAL_5.data_type, signals[1].data_type);
    assert_int_equal(SIGNAL_5.sample_rate, signals[1].sample_rate);
    assert_int_equal(SIGNAL_5.samples_per_data, signals[1].samples_per_data);
    assert_int_equal(SIGNAL_5.sample_decimate_factor, signals[1].sample_decimate_factor);
    assert_int_equal(SIGNAL_5.entries_per_summary, signals[1].entries_per_summary);
    assert_int_equal(SIGNAL_5.annotation_decimate_factor, signals[1].annotation_decimate_factor);
    assert_int_equal(SIGNAL_5.utc_decimate_factor, signals[1].utc_decimate_factor);
    assert_string_equal(SIGNAL_5.name, signals[1].name);
    assert_string_equal(SIGNAL_5.units, signals[1].units);
    assert_string_equal(SIGNAL_6.name, signals[2].name);

    jls_rd_close(rd);
    remove(filename);
}

static void test_wr_signal_without_source(void **state) {
    (void) state;
    struct jls_wr_s * wr = NULL;
    assert_int_equal(0, jls_wr_open(&wr, filename));
    assert_int_equal(JLS_ERROR_NOT_FOUND, jls_wr_signal_def(wr, &SIGNAL_6));
    assert_int_equal(0, jls_wr_close(wr));
    remove(filename);
}

static void test_wr_signal_duplicate(void **state) {
    (void) state;
    struct jls_wr_s * wr = NULL;
    assert_int_equal(0, jls_wr_open(&wr, filename));
    assert_int_equal(0, jls_wr_source_def(wr, &SOURCE_3));
    assert_int_equal(0, jls_wr_signal_def(wr, &SIGNAL_6));
    assert_int_equal(JLS_ERROR_ALREADY_EXISTS, jls_wr_signal_def(wr, &SIGNAL_6));
    assert_int_equal(0, jls_wr_close(wr));
    remove(filename);
}
#endif

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

static void test_fsr_f32(void **state) {
    (void) state;
    struct jls_wr_s * wr = NULL;
    const int64_t sample_count = WINDOW_SIZE * 1000;
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

    assert_int_equal(0, jls_wr_close(wr));

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

    // get out of range samples
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_rd_fsr_f32(rd, 5, -25, data, 10));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_rd_fsr_f32(rd, 5, -5, data, 10));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_rd_fsr_f32(rd, 5, sample_count - 5, data, 10));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_rd_fsr_f32(rd, 5, sample_count + 5, data, 10));

    jls_rd_close(rd);
    free(signal);
    remove(filename);
}

static void compare_stats(float * data, float * src, size_t src_length) {
    struct jls_statistics_s s1;
    jls_statistics_reset(&s1);
    jls_statistics_compute_f32(&s1, src, src_length);
    assert_float_equal(s1.mean, data[JLS_SUMMARY_FSR_MEAN], 1e-7);
    assert_float_equal(s1.min, data[JLS_SUMMARY_FSR_MIN], 1e-7);
    assert_float_equal(s1.max, data[JLS_SUMMARY_FSR_MAX], 1e-7);
    float v_std = (float) sqrt(jls_statistics_var(&s1));
    assert_float_equal(v_std, data[JLS_SUMMARY_FSR_STD], 1e-7f + 0.0005f * v_std);
}

static void test_fsr_f32_statistics(void **state) {
    (void) state;
    struct jls_wr_s * wr = NULL;
    const int64_t sample_count = WINDOW_SIZE * 1000;
    float * signal = gen_triangle(1000, sample_count);
    assert_non_null(signal);

    assert_int_equal(0, jls_wr_open(&wr, filename));
    assert_int_equal(0, jls_wr_source_def(wr, &SOURCE_3));
    assert_int_equal(0, jls_wr_signal_def(wr, &SIGNAL_5));
    assert_true(sample_count <= UINT32_MAX);
    assert_int_equal(0, jls_wr_fsr_f32(wr, 5, 0, signal, (uint32_t) sample_count));
    assert_int_equal(0, jls_wr_close(wr));

    struct jls_rd_s * rd = NULL;
    assert_int_equal(0, jls_rd_open(&rd, filename));

    float data[2000][4];
    // within a single data chunk
    assert_int_equal(0, jls_rd_fsr_f32_statistics(rd, 5, 0, 10, data[0], 100));
    compare_stats(data[0], signal, 10);
    compare_stats(data[1], signal + 10, 10);

    // offset from start of chunk
    assert_int_equal(0, jls_rd_fsr_f32_statistics(rd, 5, 15, 10, data[0], 2));
    compare_stats(data[0], signal + 15, 10);
    compare_stats(data[1], signal + 25, 10);

    // span chunk 2 to 3
    assert_int_equal(0, jls_rd_fsr_f32_statistics(rd, 5, 1999, 2, data[0], 2));
    compare_stats(data[0], signal + 1999, 2);
    compare_stats(data[1], signal + 2001, 2);

    // Span chunk 2 through 4
    assert_int_equal(0, jls_rd_fsr_f32_statistics(rd, 5, 1999, 1002, data[0], 2));
    compare_stats(data[0], signal + 1999, 1002);
    compare_stats(data[1], signal + 3001, 1002);

    // Span chunk 2 through 12
    assert_int_equal(0, jls_rd_fsr_f32_statistics(rd, 5, 1999, 10002, data[0], 1));
    compare_stats(data[0], signal + 1999, 10002);

    // Using summaries needing raw samples before and after
    assert_int_equal(0, jls_rd_fsr_f32_statistics(rd, 5, 750, 10000, data[0], 1));
    compare_stats(data[0], signal + 750, 10000);

    // get out of range statistics
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_rd_fsr_f32_statistics(rd, 5, -25, 10, data[0], 1));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_rd_fsr_f32_statistics(rd, 5, -5, 10, data[0], 1));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_rd_fsr_f32_statistics(rd, 5, sample_count - 5, 10, data[0], 1));
    assert_int_equal(JLS_ERROR_PARAMETER_INVALID, jls_rd_fsr_f32_statistics(rd, 5, sample_count + 5, 10, data[0], 1));

    jls_rd_close(rd);
    free(signal);
    remove(filename);
}

#if !SKIP_REALWORLD
static void test_fsr_f32_statistics_real(void **state) {
    (void) state;
    struct jls_rd_s * rd = NULL;
    assert_int_equal(0, jls_rd_open(&rd, "C:\\repos\\Jetperch\\out.jls"));

    float data[596][JLS_SUMMARY_FSR_COUNT];
    assert_int_equal(0, jls_rd_fsr_f32_statistics(rd, 1, 393783914LL, 96563, &data[0][0], 596));
    jls_rd_close(rd);
}

static int32_t on_annotation_real(void * user_data, const struct jls_annotation_s * annotation) {
    int64_t * count = (int64_t *) user_data;
    *count += 1;
    (void) annotation;
    return 0;
}

static void test_fsr_annotation_real(void **state) {
    (void) state;
    int64_t count = 0;
    struct jls_rd_s * rd = NULL;
    assert_int_equal(0, jls_rd_open(&rd, "C:\\repos\\Jetperch\\out.anno.jls"));
    assert_int_equal(0, jls_rd_annotations(rd, 1, 0, on_annotation_real, &count));
    jls_rd_close(rd);
}
#endif


int main(void) {
    const struct CMUnitTest tests[] = {
#if !SKIP_BASIC
            cmocka_unit_test(test_source),
            cmocka_unit_test(test_wr_source_duplicate),
            cmocka_unit_test(test_annotation),
            cmocka_unit_test(test_annotation_seek),
            cmocka_unit_test(test_hmarker),
            cmocka_unit_test(test_user_data),
            cmocka_unit_test(test_utc),
            cmocka_unit_test(test_utc_seek_first_block),
            cmocka_unit_test(test_utc_seek_second_block_start),
            cmocka_unit_test(test_utc_seek_second_block_middle),

            cmocka_unit_test(test_signal),
            cmocka_unit_test(test_wr_signal_without_source),
            cmocka_unit_test(test_wr_signal_duplicate),
#endif
            cmocka_unit_test(test_fsr_f32),
            cmocka_unit_test(test_fsr_f32_statistics),

#if !SKIP_REALWORLD
            cmocka_unit_test(test_fsr_f32_statistics_real),
            cmocka_unit_test(test_fsr_annotation_real)
#endif
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
