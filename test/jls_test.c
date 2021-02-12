/*
 * Copyright 2014-2017 Jetperch LLC
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
#include "jls/writer.h"
#include "jls/format.h"
#include "jls/time.h"
#include "jls/ec.h"
#include <stdio.h>
#include <string.h>


#define SKIP_BASIC 1

const char * filename = "tmp.jls";
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
        .utc_rate_auto = 0,
        .name = "signal 5",
        .si_units = "A",
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
        .utc_rate_auto = 0,
        .name = "signal 6",
        .si_units = "V",
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
}

static void test_wr_source_duplicate(void **state) {
    (void) state;
    struct jls_wr_s * wr = NULL;
    // struct jls_rd_s * rd = NULL;
    assert_int_equal(0, jls_wr_open(&wr, filename));
    assert_int_equal(0, jls_wr_source_def(wr, &SOURCE_1));
    assert_int_equal(JLS_ERROR_ALREADY_EXISTS, jls_wr_source_def(wr, &SOURCE_1));
    assert_int_equal(0, jls_wr_close(wr));
}

static void test_annotation(void **state) {
    (void) state;
    int64_t now = jls_now();
    struct jls_wr_s * wr = NULL;
    struct jls_rd_s * rd = NULL;
    assert_int_equal(0, jls_wr_open(&wr, filename));
    assert_int_equal(0, jls_wr_annotation(wr, 0, now + 0 * JLS_TIME_MILLISECOND,
                                          JLS_ANNOTATION_TYPE_TEXT, JLS_STORAGE_TYPE_STRING,
                                          (const uint8_t *) STRING_1, 0));
    assert_int_equal(0, jls_wr_annotation(wr, 0, now + 1 * JLS_TIME_MILLISECOND,
                                          JLS_ANNOTATION_TYPE_MARKER, JLS_STORAGE_TYPE_STRING,
                                          (const uint8_t *) "1", 0));
    assert_int_equal(0, jls_wr_annotation(wr, 0, now + 2 * JLS_TIME_MILLISECOND,
                                          JLS_ANNOTATION_TYPE_USER, JLS_STORAGE_TYPE_BINARY,
                                          USER_DATA_1, sizeof(USER_DATA_1)));
    assert_int_equal(0, jls_wr_annotation(wr, 0, now + 3 * JLS_TIME_MILLISECOND,
                                          JLS_ANNOTATION_TYPE_USER, JLS_STORAGE_TYPE_STRING,
                                          (const uint8_t *) STRING_1, 0));
    assert_int_equal(0, jls_wr_annotation(wr, 0, now + 4 * JLS_TIME_MILLISECOND,
                                          JLS_ANNOTATION_TYPE_USER, JLS_STORAGE_TYPE_JSON,
                                          (const uint8_t *) JSON_1, 0));
    assert_int_equal(0, jls_wr_close(wr));

    assert_int_equal(0, jls_rd_open(&rd, filename));
    struct jls_rd_annotation_s * annotations;
    uint32_t count = 0;
    assert_int_equal(0, jls_rd_annotations(rd, 0, &annotations, &count));
    // assert_int_equal(5, count); // todo

    // todo test
    jls_rd_close(rd);
}

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

    struct jls_rd_user_data_s user_data;
    assert_int_equal(0, jls_rd_user_data_next(rd, &user_data));
    assert_int_equal(CHUNK_META_1, user_data.chunk_meta);
    assert_int_equal(JLS_STORAGE_TYPE_BINARY, user_data.storage_type);
    assert_int_equal(sizeof(USER_DATA_1), user_data.data_size);
    assert_memory_equal(USER_DATA_1, user_data.data, sizeof(sizeof(USER_DATA_1)));

    assert_int_equal(0, jls_rd_user_data_next(rd, &user_data));
    assert_int_equal(CHUNK_META_2, user_data.chunk_meta);
    assert_int_equal(JLS_STORAGE_TYPE_STRING, user_data.storage_type);
    assert_int_equal(sizeof(STRING_1), user_data.data_size);
    assert_memory_equal(STRING_1, user_data.data, sizeof(sizeof(STRING_1)));

    assert_int_equal(0, jls_rd_user_data_next(rd, &user_data));
    assert_int_equal(CHUNK_META_3, user_data.chunk_meta);
    assert_int_equal(JLS_STORAGE_TYPE_JSON, user_data.storage_type);
    assert_int_equal(sizeof(JSON_1), user_data.data_size);
    assert_memory_equal(JSON_1, user_data.data, sizeof(sizeof(JSON_1)));

    assert_int_equal(JLS_ERROR_EMPTY, jls_rd_user_data_next(rd, &user_data));

    assert_int_equal(0, jls_rd_user_data_prev(rd, &user_data));
    assert_int_equal(CHUNK_META_2, user_data.chunk_meta);
    assert_int_equal(0, jls_rd_user_data_prev(rd, &user_data));
    assert_int_equal(CHUNK_META_1, user_data.chunk_meta);
    assert_int_equal(JLS_ERROR_EMPTY, jls_rd_user_data_prev(rd, &user_data));

    assert_int_equal(0, jls_rd_user_data_next(rd, &user_data));
    assert_int_equal(0, jls_rd_user_data_next(rd, &user_data));
    assert_int_equal(0, jls_rd_user_data_reset(rd));
    assert_int_equal(0, jls_rd_user_data_next(rd, &user_data));
    assert_int_equal(CHUNK_META_1, user_data.chunk_meta);

    jls_rd_close(rd);
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
    assert_int_equal(SIGNAL_5.utc_rate_auto, signals[1].utc_rate_auto);
    assert_string_equal(SIGNAL_5.name, signals[1].name);
    assert_string_equal(SIGNAL_5.si_units, signals[1].si_units);
    assert_string_equal(SIGNAL_6.name, signals[2].name);

    jls_rd_close(rd);
}

static void test_wr_signal_without_source(void **state) {
    (void) state;
    struct jls_wr_s * wr = NULL;
    assert_int_equal(0, jls_wr_open(&wr, filename));
    assert_int_equal(JLS_ERROR_NOT_FOUND, jls_wr_signal_def(wr, &SIGNAL_6));
    assert_int_equal(0, jls_wr_close(wr));
}

static void test_wr_signal_duplicate(void **state) {
    (void) state;
    struct jls_wr_s * wr = NULL;
    assert_int_equal(0, jls_wr_open(&wr, filename));
    assert_int_equal(0, jls_wr_source_def(wr, &SOURCE_3));
    assert_int_equal(0, jls_wr_signal_def(wr, &SIGNAL_6));
    assert_int_equal(JLS_ERROR_ALREADY_EXISTS, jls_wr_signal_def(wr, &SIGNAL_6));
    assert_int_equal(0, jls_wr_close(wr));
}
#endif

struct triangle_waveform_s {
    float y_scale;
    int32_t incr;
    int32_t steps;
    int32_t value;
};

void triangle_fill(struct triangle_waveform_s * self, float * data, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (self->value <= 0) {
            self->incr = 1;
        } else if (self->value >= self->steps) {
            self->incr = -1;
        }
        self->value += self->incr;
        data[i] = self->value * self->y_scale;
    }
}

#define WINDOW_SIZE (937)

static void test_data(void **state) {
    (void) state;
    struct jls_wr_s * wr = NULL;
    assert_int_equal(0, jls_wr_open(&wr, filename));
    assert_int_equal(0, jls_wr_source_def(wr, &SOURCE_3));
    assert_int_equal(0, jls_wr_signal_def(wr, &SIGNAL_5));

    struct triangle_waveform_s twav = {
            .y_scale = 0.001f,
            .incr = 1,
            .steps = 1000,
            .value = 0,
    };
    float data[WINDOW_SIZE]; // random number
    uint64_t sample_id = 0;

    for (int i = 0; i < 1000; ++i) {
        triangle_fill(&twav, data, WINDOW_SIZE);
        assert_int_equal(0, jls_wr_fsr_f32(wr, 5, sample_id, data, WINDOW_SIZE));
        sample_id += WINDOW_SIZE;
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
    assert_int_equal(WINDOW_SIZE * 1000, samples);
    jls_rd_close(rd);
}

int main(void) {
    const struct CMUnitTest tests[] = {
#if !SKIP_BASIC
            cmocka_unit_test(test_source),
            cmocka_unit_test(test_wr_source_duplicate),
            cmocka_unit_test(test_annotation),
            cmocka_unit_test(test_user_data),
            cmocka_unit_test(test_signal),
            cmocka_unit_test(test_wr_signal_without_source),
            cmocka_unit_test(test_wr_signal_duplicate),
#endif
            cmocka_unit_test(test_data),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
