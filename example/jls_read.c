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
#include "jls/backend.h"

#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>


#define ARRAY_SIZE(x) ( sizeof(x) / sizeof((x)[0]) )


static const char usage_str[] =
"Read a JLS file.\n"
"usage: jls_read <command>\n"
"For help, jls_read <command> --help\n"
"\n"
"Display JLS file information.\n"
"  info <filename>\n"
"    <filename>                     The input file path.\n"
"\n"
"Display a statistic.\n"
"  statistic <filename> <signal_id> <start> <incr> <len>\n"
"    <filename>                     The input file path.\n"
"    <signal_id>                    The signal id.\n"
"    <start>                        The starting sample.\n"
"    <incr>                         The increment per statistic, in samples.\n"
"    <len>                          The number of statistics to retrieve.\n"
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

static int info(const char * filename) {
    struct jls_rd_s * rd = NULL;
    struct jls_signal_def_s * signals;
    uint16_t signals_count = 0;
    RPE(jls_rd_open(&rd, filename));
    RPE(jls_rd_signals(rd, &signals, &signals_count));
    printf("\nsignals:\n");
    for (uint16_t signal_idx = 0; signal_idx < signals_count; ++signal_idx) {
        int64_t samples = 0;
        struct jls_signal_def_s * s = signals + signal_idx;
        jls_rd_fsr_length(rd, s->signal_id, &samples);
        printf("    %d %s, %" PRId64 " samples\n", s->signal_id, s->name, samples);
    }
    jls_rd_close(rd);
    return 0;
}

static int statistic(const char * filename, uint16_t signal_id, int64_t start, int64_t incr, int64_t len) {
    struct jls_rd_s * rd = NULL;
    if (len < 0) {
        return 1;
    }
    RPE(jls_rd_open(&rd, filename));
    double * data = calloc((size_t) len * 4, sizeof(double));
    RPE(jls_rd_fsr_statistics(rd, signal_id, start, incr, data, len));
    for (int64_t k = 0; k < len; ++k) {
        double * data_k = data + k * 4;
        printf("mean = %f, std=%f, min=%f, max=%f\n",
               data_k[JLS_SUMMARY_FSR_MEAN], data_k[JLS_SUMMARY_FSR_STD],
               data_k[JLS_SUMMARY_FSR_MIN], data_k[JLS_SUMMARY_FSR_MAX]);
    }
    free(data);
    return 0;
}

int main(int argc, char * argv[]) {
    SKIP_ARGS(1); // skip our name
    int required_args = 0;
    char * filename = 0;

    if (!argc) {
        return usage();
    }
    REQUIRE_ARGS(1);

    if (strcmp(argv[0], "info") == 0) {
        SKIP_REQUIRED();
        REQUIRE_ARGS(1);
        filename = argv[0];
        if (info(filename)) {
            printf("Failed to complete info\n");
            return 1;
        }
    } else if (strcmp(argv[0], "statistic") == 0) {
        SKIP_REQUIRED();
        REQUIRE_ARGS(5);
        filename = argv[0];
        int64_t signal_id = 0;
        int64_t start = 0;
        int64_t incr = 0;
        int64_t len = 0;
        RPE(cstr_to_i64(argv[1], &signal_id));
        RPE(cstr_to_i64(argv[2], &start));
        RPE(cstr_to_i64(argv[3], &incr));
        RPE(cstr_to_i64(argv[4], &len));
        if (statistic(filename, (uint16_t) signal_id, start, incr, len)) {
            printf("Failed to complete statistics\n");
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
