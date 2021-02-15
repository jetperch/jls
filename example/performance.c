/*
 * Copyright 2021 Jetperch LLC
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

#include "jls/writer.h"
#include "jls/reader.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>


#define RPE(x)  do {                        \
    int32_t rc__ = (x);                     \
    if (rc__) {                             \
        printf("error %d: " #x "\n", rc__); \
        return rc__;                        \
    }                                       \
} while (0)

const char usage_str[] =
"usage: performance <command>\n"
"\n"
"Available commands:\n"
"    generate\n"
"        [--sample_rate <rate>]             In Hz\n"
"        [--length <samples>]\n"
"        [--samples_per_data <spd>]\n"
"        [--sample_decimate_factor <sdf>]\n"
"        [--entries_per_summary <eps>]\n"
"        [--summary_decimate_factor <sdf>]\n"
"        filename\n"
"    profile\n"
"\n";

const struct jls_source_def_s SOURCE_1 = {
        .source_id = 1,
        .name = "performance",
        .vendor = "jls",
        .model = "",
        .version = "",
        .serial_number = "",
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
        .utc_rate_auto = 0,
        .name = "performance_1",
        .si_units = "A",
};

static int _isspace(char c) {
    if ((c == ' ') || ((c >= 9) && (c <= 13))) {
        return 1;
    }
    return 0;
}

static int cstr_to_i64(const char * src, int64_t * value) {
    uint32_t v = 0;
    if ((NULL == src) || (NULL == value)) {
        return 1;
    }
    while (*src && _isspace((uint8_t) *src)) {
        ++src;
    }
    if (!*src) { // empty string.
        return 1;
    }
    while ((*src >= '0') && (*src <= '9')) {
        v = v * 10 + (*src - '0');
        ++src;
    }
    while (*src) {
        if (!_isspace((uint8_t) *src++)) { // did not parse full string
            return 1;
        }
    }
    *value = v;
    return 0;
}

static int cstr_to_u32(const char * src, uint32_t * value) {
    int64_t v = 0;
    int rv = cstr_to_i64(src, &v);
    if ((v < 0) || (v > (1LL << 32))) {
        return 1;
    }
    *value = (uint32_t) v;
    return rv;
}

/**
 * @brief Generate a triangle waveform.
 *
 * @param period The waveform period in sample.
 * @param data[out] The sample data.
 * @param data_length The length of data in float32 samples.
 *
 * Triangle waveforms are much faster to compute than sinusoids,
 * and they still have enough variation for test purposes.
 */
static void gen_triangle(uint32_t period, float * data, int64_t data_length) {
    int64_t v_max = (period + 1) / 2;
    float offset = v_max / 2.0f;
    float gain = 2.0f / v_max;
    int64_t v = v_max / 2;
    int64_t incr = 1;
    for (int64_t i = 0; i < data_length; ++i) {
        data[i] = gain * (v - offset);
        if (v <= 0) {
            incr = 1;
        } else if (v >= v_max) {
            incr = -1;
        }
        v += incr;
    }
}

static int32_t generate_jls(const char * filename, const struct jls_signal_def_s * signal, int64_t duration) {
    int64_t data_length = 1000000;
    float * data = malloc((size_t) data_length * sizeof(float));
    struct jls_wr_s * wr = NULL;
    gen_triangle(1000, data, data_length);
    RPE(jls_wr_open(&wr, filename));
    RPE(jls_wr_source_def(wr, &SOURCE_1));
    RPE(jls_wr_signal_def(wr, signal));
    int64_t sample_id = 0;

    while (duration > 0) {
        if (data_length > duration) {
            data_length = duration;
        }
        RPE(jls_wr_fsr_f32(wr, 1, sample_id, data, (uint32_t) data_length));
        sample_id += data_length;
        duration -= data_length;
    }
    RPE(jls_wr_close(wr));
    return 0;
}


static int usage() {
    printf("%s", usage_str);
    return 1;
}

#define SKIP_ARGS(N) do {               \
    int n = (N);                        \
    if (n > argc) {                     \
        printf("SKIP_ARGS error\n");    \
        return usage();                 \
    }                                   \
    argc -= n;                          \
    argv += n;                          \
} while (0)


int main(int argc, char * argv[]) {
    SKIP_ARGS(1); // skip our name
    struct jls_signal_def_s signal_def = SIGNAL_1;

    char * filename = 0;
    uint32_t sample_rate = 1000000;
    int64_t length= sample_rate;


    if (!argc) {
        return usage();
    } else if (strcmp(argv[0], "generate") == 0) {
        SKIP_ARGS(1);
        while (argc) {
            if (argv[0][0] != '-') {
                if (filename) {
                    return usage();
                }
                filename = argv[0];
                SKIP_ARGS(1);
            } else if (0 == strcmp("--sample_rate", argv[0])) {
                RPE(cstr_to_u32(argv[1], &sample_rate));
                SKIP_ARGS(2);
            } else if (0 == strcmp("--length", argv[0])) {
                RPE(cstr_to_i64(argv[1], &length));
                SKIP_ARGS(2);

            } else if (0 == strcmp("--samples_per_data", argv[0])) {

            }
                "        [--sample_rate <rate>]             In Hz\n"
                "        [--length <samples>]\n"
                "        [--samples_per_data <spd>]\n"
                "        [--sample_decimate_factor <sdf>]\n"
                "        [--entries_per_summary <eps>]\n"
                "        [--summary_decimate_factor <sdf>]\n"
        }
    } else if (strcmp(argv[0], "profile") == 0) {

    } else {
        return usage();
    }

    if (generate_jls("out.jls", &SIGNAL_1, 1000000000)) {
        printf("Failed to generate file.\n");
        return 1;
    }

    return 0;
}
