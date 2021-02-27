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

#include "jls/threaded_writer.h"
#include "jls/ec.h"
#include "jls/reader.h"
#include "jls/raw.h"
#include "jls/time.h"

#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>


#define ARRAY_SIZE(x) ( sizeof(x) / sizeof((x)[0]) )


static const char usage_str[] =
"Utility to test JLS file performance.\n"
"usage: performance <command>\n"
"For help, performance <command> --help\n"
"\n"
"Generate a JLS file.\n"
"  generate <filename> [--<opt1> <value> ...]\n"
"    <filename>                     The output file path.\n"
"    --name                         The signal name.\n"
"    --units                        The SI units.\n"
"    --sample_rate                  The sample rate in Hz.\n"
"    --length                       The JLS file length in samples.\n"
"    --samples_per_data             The samples per data chunk.\n"
"    --sample_decimate_factor       The samples per summary entry.\n"
"    --entries_per_summary          The entries per summary chunk.\n"
"    --summary_decimate_factor      The summaries per summary entry.\n"
"\n"
"Profile JLS read performance.\n"
"  profile <filename>\n"
"    <filename>                     The input file path.\n"
"\n"
"Print the JLS tag structure.\n"
"  print <filename> [--<opt1> <value> ...]\n"
"    <filename>                     The input file path.\n"
"    --level                        Skip values below level.\n"
"\n"
"Copyright 2021 Jetperch LLC, Apache 2.0 license\n"
"\n";


#define RPE(x)  do {                        \
    int32_t rc__ = (x);                     \
    if (rc__) {                             \
        printf("error %d: " #x "\n", rc__); \
        return rc__;                        \
    }                                       \
} while (0)

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
        .sample_rate = 1000000,
        .samples_per_data = 100000,
        .sample_decimate_factor = 1000,
        .entries_per_summary = 20000,
        .summary_decimate_factor = 100,
        .annotation_decimate_factor = 100,
        .utc_decimate_factor = 100,
        .name = "current",
        .units = "A",
};

static int _isspace(char c) {
    if ((c == ' ') || ((c >= 9) && (c <= 13))) {
        return 1;
    }
    return 0;
}

static int cstr_to_i64(const char * src, int64_t * value) {
    int64_t v = 0;
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
    struct jls_twr_s * wr = NULL;
    gen_triangle(1000, data, data_length);
    RPE(jls_twr_open(&wr, filename));
    RPE(jls_twr_source_def(wr, &SOURCE_1));
    RPE(jls_twr_signal_def(wr, signal));
    int64_t sample_id = 0;
    int32_t rc = 0;
    printf("Generate %" PRIi64 " = %.1e\n", duration, (double) duration);

    while (duration > 0) {
        if (data_length > duration) {
            data_length = duration;
        }
        while (1) {
            rc = jls_twr_fsr_f32(wr, signal->signal_id, sample_id, data, (uint32_t) data_length);
            if (rc != JLS_ERROR_NOT_ENOUGH_MEMORY) {
                break;
            }
            Sleep(1);
        }
        sample_id += data_length;
        duration -= data_length;
    }
    RPE(jls_twr_close(wr));
    return 0;
}

static int32_t json_signal_def(struct jls_rd_s * rd, uint16_t signal_id, FILE * json) {
    int64_t length = 0;
    struct jls_signal_def_s signal_def;
    RPE(jls_rd_signal(rd, signal_id, &signal_def));
    if (signal_def.signal_type == JLS_SIGNAL_TYPE_FSR) {
        RPE(jls_rd_fsr_length(rd, signal_id, &length));
        printf("Length = %" PRIi64 " samples (%.0e)\n", length, (double) length);
    } else {
        // todo get VSR length
    }
    fprintf(json, "\n  \"info\": {");
    fprintf(json, "\n    \"signal_id\": %d,", (int) signal_def.signal_id);
    fprintf(json, "\n    \"source_id\": %d,", (int) signal_def.source_id);
    fprintf(json, "\n    \"signal_type\": %d,", (int) signal_def.signal_type);
    fprintf(json, "\n    \"signal_type_str\": \"%s\",", (signal_def.signal_type == JLS_SIGNAL_TYPE_FSR) ? "FSR" : "VSR");
    fprintf(json, "\n    \"data_type\": %d,", (int) signal_def.data_type);
    fprintf(json, "\n    \"sample_rate\": %" PRIu32 ",", signal_def.sample_rate);
    fprintf(json, "\n    \"samples_per_data\": %" PRIu32 ",", signal_def.samples_per_data);
    fprintf(json, "\n    \"sample_decimate_factor\": %" PRIu32 ",", signal_def.sample_decimate_factor);
    fprintf(json, "\n    \"entries_per_summary\": %" PRIu32 ",", signal_def.entries_per_summary);
    fprintf(json, "\n    \"summary_decimate_factor\": %" PRIu32 ",", signal_def.summary_decimate_factor);
    fprintf(json, "\n    \"annotation_decimate_factor\": %" PRIu32 ",", signal_def.annotation_decimate_factor);
    fprintf(json, "\n    \"utc_decimate_factor\": %" PRIu32 ",", signal_def.utc_decimate_factor);
    fprintf(json, "\n    \"name\": \"%s\",", signal_def.name);
    fprintf(json, "\n    \"units\": \"%s\",", signal_def.units);
    fprintf(json, "\n    \"length\": %" PRIi64, length);
    fprintf(json, "\n  }");
    fflush(json);
    return 0;
}

static int32_t profile_fsr_signal(struct jls_rd_s * rd, uint16_t signal_id, FILE * json) {
    int64_t length = 0;
    static float data[100000];
    const int64_t read_lengths[] = {1, 100, 1000, 10000, 100000};
    RPE(jls_rd_fsr_length(rd, signal_id, &length));
    fprintf(json, "\n{");
    json_signal_def(rd, signal_id, json);

    fprintf(json, ",\n  \"read_samples\": [");
    for (uint8_t read_idx = 0; read_idx < ARRAY_SIZE(read_lengths); ++read_idx) {
        int64_t sample_count = read_lengths[read_idx];
        int64_t step_count = 100;
        int64_t step_sz = (length - 1 - sample_count) / step_count;
        int64_t t_start = jls_time_rel();
        for (int64_t sample = 0; sample < length; sample += step_sz) {
            RPE(jls_rd_fsr_f32(rd, signal_id, sample, data, sample_count));
        }
        int64_t t_end = jls_time_rel();
        double t_duration = JLS_TIME_TO_F64(t_end - t_start);
        printf("Read %" PRIi64 " samples: %g seconds\n", sample_count, t_duration / step_count);
        fflush(stdout);
        if (read_idx) {
            fprintf(json, ",");
        }
        fprintf(json, "\n    [%" PRIi64 ", %g]", sample_count, t_duration / step_count);
    }
    fprintf(json, "\n  ]");

    fprintf(json, ",\n  \"read_summary\": [");
    int summary_counter = 0;
    const int64_t summary_lengths[] = {1, 11, 479, 1117, 11939};
    int64_t increment = 1;
    printf("#signal,increment,length,time\n");
    while (increment < length) {
        for (uint8_t summary_idx = 0; summary_idx < ARRAY_SIZE(summary_lengths); ++summary_idx) {
            int64_t samples = summary_lengths[summary_idx];
            int64_t count = length / increment - samples;
            if (count <= 0) {
                count = 1;
            }
            if (count > 100) {
                count = 100;
            }
            int64_t iter_count = 0;
            int64_t t_start = jls_time_rel();
            int64_t offset_sz = (length - increment - 1) / count;
            for (int64_t sample = 0; sample < (length - increment); sample += offset_sz) {
                int64_t max_len = (length - sample) / increment;
                int64_t data_length = (max_len < samples) ? max_len : samples;
                RPE(jls_rd_fsr_f32_statistics(rd, signal_id, sample, increment, data, data_length));
                ++iter_count;
            }
            int64_t t_end = jls_time_rel();
            double t_duration = JLS_TIME_TO_F64(t_end - t_start);
            printf("%d,%" PRIi64 ",%" PRIi64",%g\n", (int) signal_id, increment,
                   samples, t_duration / iter_count);
            fflush(stdout);
            if (summary_counter) {
                fprintf(json, ",");
            }
            fprintf(json, "\n    [%d, %" PRIi64 ", %" PRIi64", %g]", (int) signal_id,
                    increment, samples, t_duration / iter_count);
            ++summary_counter;
        }
        increment *= 3;
    }
    fprintf(json, "\n  ]");

    fprintf(json, "\n}");
    return 0;
}

static int32_t profile_vsr_signal(struct jls_rd_s * rd, uint16_t signal_id, FILE * json) {
    printf("Not yet implemented, skip\n");
    fprintf(json, "\n{");
    json_signal_def(rd, signal_id, json);
    fprintf(json, "\n}");
    return 0;
}

static int32_t profile(const char * filename) {
    struct jls_rd_s * rd = NULL;
    struct jls_signal_def_s * signals;
    char json_filename[1024];
    snprintf(json_filename, sizeof(json_filename), "%s.json", filename);
    FILE * json = fopen(json_filename, "wt");
    if (!json) {
        printf("Could not open JSON output file: %s\n", json_filename);
        return 1;
    }
    uint16_t signals_count = 0;
    RPE(jls_rd_open(&rd, filename));
    fprintf(json, "{");

    RPE(jls_rd_signals(rd, &signals, &signals_count));
    fprintf(json, "\n\"signals\": [");
    for (uint16_t signal_idx = 0; signal_idx < signals_count; ++signal_idx) {
        struct jls_signal_def_s * s = signals + signal_idx;
        if (signal_idx) {
            fprintf(json, ",");
        }
        switch (s->signal_type) {
            case JLS_SIGNAL_TYPE_FSR:
                printf("\nProfile FSR signal %d: %d\n", (int) signal_idx, (int) s->signal_id);
                RPE(profile_fsr_signal(rd, s->signal_id, json));
                break;
            case JLS_SIGNAL_TYPE_VSR:
                printf("\nProfile VSR signal %d: %d\n", (int) signal_idx, (int) s->signal_id);
                RPE(profile_vsr_signal(rd, s->signal_id, json));
                break;
            default:
                printf("\nProfile signal %d: %d\n", (int) signal_idx, (int) s->signal_id);
                break;
        }
    }
    fprintf(json, "\n]");

    fprintf(json, "\n}\n");
    jls_rd_close(rd);
    return 0;
}

static int32_t print(const char * filename, uint32_t level) {
    struct jls_raw_s * raw = NULL;
    struct jls_chunk_header_s hdr;
    RPE(jls_raw_open(&raw, filename, "r"));
    uint8_t hdr_level = 0;
    printf("print(%s, %d)\n", filename, (int) level);
    while (1) {
        if (jls_raw_rd_header(raw, &hdr)) {
            break;
        }
        hdr_level = hdr.chunk_meta >> 12;
        if (hdr.tag & 0x20) {
            if (hdr_level >= level) {
                printf("%" PRIi64 " %s (lvl=%d, prev=%" PRIi64 ", next=%" PRIi64 ") %" PRIu32 "\n",
                       jls_raw_chunk_tell(raw), jls_tag_to_name(hdr.tag),
                       (int) hdr_level, hdr.item_prev, hdr.item_next, hdr.payload_length);
                fflush(stdout);
            }
        } else {
            printf("%" PRIi64 " %s (prev=%" PRIi64 ", next=%" PRIi64 ")\n",
                   jls_raw_chunk_tell(raw), jls_tag_to_name(hdr.tag),
                   hdr.item_prev, hdr.item_next);
            fflush(stdout);
        }
        if (jls_raw_chunk_next(raw)) {
            break;
        }
    }
    jls_raw_close(raw);
    return 0;
}

static int usage() {
    printf("%s", usage_str);
    return 1;
}

#define UNSUPPORTED() \
    printf("Unsupported argument: %s\n", argv[0]); \
    return usage();

#define SKIP_ARGS(N) do {               \
    int n = (N);                        \
    if (n > argc) {                     \
        printf("SKIP_ARGS error\n");    \
        return usage();                 \
    }                                   \
    argc -= n;                          \
    argv += n;                          \
} while (0)

#define SKIP_REQUIRED() do { \
    argc -= required_args;   \
    argv += required_args;   \
    required_args = 0;       \
} while(0)

#define REQUIRE_ARGS(N) do { \
    required_args = (N);                \
    if (argc < required_args) {         \
        printf("REQUIRE_ARGS error\n"); \
        return usage();                 \
    }                                   \
} while(0)

#define REQUIRE_FILENAME() \
    if (!filename) { \
        printf("Must specify filename\n"); \
        return usage(); \
    }

int main(int argc, char * argv[]) {
    SKIP_ARGS(1); // skip our name
    int required_args = 0;
    struct jls_signal_def_s signal_def = SIGNAL_1;

    char * filename = 0;

    if (!argc) {
        return usage();
    }
    REQUIRE_ARGS(1);

    if (strcmp(argv[0], "generate") == 0) {
        int64_t length = 1000000;
        SKIP_REQUIRED();
        while (argc) {
            if (argv[0][0] != '-') {
                REQUIRE_ARGS(1);
                if (filename) {
                    return usage();
                }
                filename = argv[0];
            } else if (0 == strcmp("--filename", argv[0])) {
                REQUIRE_ARGS(2);
                filename = argv[1];
            } else if (0 == strcmp("--name", argv[0])) {
                REQUIRE_ARGS(2);
                signal_def.name = argv[1];
            } else if (0 == strcmp("--units", argv[0])) {
                REQUIRE_ARGS(2);
                signal_def.units = argv[1];
            } else if (0 == strcmp("--sample_rate", argv[0])) {
                REQUIRE_ARGS(2);
                RPE(cstr_to_u32(argv[1], &signal_def.sample_rate));
            } else if (0 == strcmp("--length", argv[0])) {
                REQUIRE_ARGS(2);
                RPE(cstr_to_i64(argv[1], &length));
            } else if (0 == strcmp("--samples_per_data", argv[0])) {
                REQUIRE_ARGS(2);
                RPE(cstr_to_u32(argv[1], &signal_def.samples_per_data));
            } else if (0 == strcmp("--sample_decimate_factor", argv[0])) {
                REQUIRE_ARGS(2);
                RPE(cstr_to_u32(argv[1], &signal_def.sample_decimate_factor));
            } else if (0 == strcmp("--entries_per_summary", argv[0])) {
                REQUIRE_ARGS(2);
                RPE(cstr_to_u32(argv[1], &signal_def.entries_per_summary));
            } else if (0 == strcmp("--summary_decimate_factor", argv[0])) {
                REQUIRE_ARGS(2);
                RPE(cstr_to_u32(argv[1], &signal_def.summary_decimate_factor));
            } else {
                UNSUPPORTED();
            }
            SKIP_REQUIRED();
        }
        REQUIRE_FILENAME();
        int64_t t_start = jls_time_rel();
        if (generate_jls(filename, &signal_def, length)) {
            printf("Failed to generate file.\n");
            return 1;
        }
        int64_t t_end = jls_time_rel();
        double t_duration = JLS_TIME_TO_F64(t_end - t_start);
        printf("Throughput: %g samples per second\n", length / t_duration);

    } else if (strcmp(argv[0], "profile") == 0) {
        SKIP_REQUIRED();
        while (argc) {
            if (argv[0][0] != '-') {
                REQUIRE_ARGS(1);
                if (filename) {
                    return usage();
                }
                filename = argv[0];
            } else if (0 == strcmp("--filename", argv[0])) {
                REQUIRE_ARGS(2);
                filename = argv[1];
            } else {
                UNSUPPORTED();
            }
            SKIP_REQUIRED();
        }
        REQUIRE_FILENAME();
        if (profile(filename)) {
            printf("Failed to complete profile\n");
            return 1;
        }
    } else if (strcmp(argv[0], "print") == 0) {
        uint32_t level = 0;
        SKIP_REQUIRED();
        while (argc) {
            if (argv[0][0] != '-') {
                REQUIRE_ARGS(1);
                if (filename) {
                    return usage();
                }
                filename = argv[0];
            } else if (0 == strcmp("--filename", argv[0])) {
                REQUIRE_ARGS(2);
                filename = argv[1];
            } else if (0 == strcmp("--level", argv[0])) {
                REQUIRE_ARGS(2);
                RPE(cstr_to_u32(argv[1], &level));
            } else {
                UNSUPPORTED();
            }
            SKIP_REQUIRED();
        }
        REQUIRE_FILENAME();
        if (print(filename, level)) {
            printf("Failed to complete profile\n");
            return 1;
        }
    } else if ((strcmp(argv[0], "help") == 0) || (strcmp(argv[0], "--help") == 0)) {
        usage();
        return 0;
    } else {
        printf("Unsupported command: %s\n", argv[0]);
        return usage();
    }
    return 0;
}
