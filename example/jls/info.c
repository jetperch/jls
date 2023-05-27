/*
 * Copyright 2023 Jetperch LLC
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

#include "jls.h"
#include "jls/raw.h"
#include "jls_util_prv.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>


#define PAYLOAD_MAX_SIZE (32U * 1024U * 1024U)


static int usage(void) {
    printf("usage: jls info [--verbose] [--chunks] <path>\n");
    return 1;
}

int on_info(struct app_s * self, int argc, char * argv[]) {
    struct jls_rd_s * rd = NULL;
    int verbose = 0;
    int chunks = 0;
    char * path = NULL;
    int pos_arg = 0;

    while (argc) {
        if (argv[0][0] != '-') {
            if (pos_arg == 0) {
                path = argv[0];
            } else {
                return usage();
            }
            ARG_CONSUME();
            ++pos_arg;
        } else if ((0 == strcmp(argv[0], "--verbose")) || (0 == strcmp(argv[0], "-v"))) {
            verbose++;
            ARG_CONSUME();
        } else if ((0 == strcmp(argv[0], "--chunks")) || (0 == strcmp(argv[0], "-c"))) {
            chunks++;
            ARG_CONSUME();
        } else {
            return usage();
        }
    }
    if (pos_arg != 1) {
        return usage();
    }

    ROE(jls_rd_open(&rd, path));

    struct jls_source_def_s * sources;
    uint16_t source_count = 0;
    ROE(jls_rd_sources(rd, &sources, &source_count));
    if (source_count) {
        printf("Sources:\n");
        for (uint16_t i = 0; i < source_count; ++i) {
            printf("  %d:\n", (int) sources[i].source_id);
            printf("    name: %s\n", sources[i].name);
            printf("    vendor: %s\n", sources[i].vendor);
            printf("    model: %s\n", sources[i].model);
            printf("    version: %s\n", sources[i].version);
            printf("    serial_number: %s\n", sources[i].serial_number);
        }
    } else {
        printf("Sources: none found\n");
    }

    struct jls_signal_def_s * signals;
    uint16_t signal_count = 0;
    ROE(jls_rd_signals(rd, &signals, &signal_count));
    if (signal_count) {
        for (uint16_t i = 0; i < signal_count; ++i) {
            printf("  %d:\n", (int) signals[i].signal_id);
            printf("    name: %s\n", signals[i].name);
            printf("    source_id: %" PRIu16 "\n", signals[i].source_id);
            printf("    signal_type: %s\n", signals[i].signal_type ? "VSR" : "FSR");
            printf("    data_type: 0x08%" PRIx32 "\n", signals[i].data_type);
            printf("    sample_rate: %" PRIu32 "\n", signals[i].sample_rate);
            printf("    samples_per_data: %" PRIu32 "\n", signals[i].samples_per_data);
            printf("    sample_decimate_factor: %" PRIu32 "\n", signals[i].sample_decimate_factor);
            printf("    entries_per_summary: %" PRIu32 "\n", signals[i].entries_per_summary);
            printf("    summary_decimate_factor: %" PRIu32 "\n", signals[i].summary_decimate_factor);
            printf("    annotation_decimate_factor: %" PRIu32 "\n", signals[i].annotation_decimate_factor);
            printf("    utc_decimate_factor: %" PRIu32 "\n", signals[i].utc_decimate_factor);
            printf("    sample_id_offset: %" PRId64 "\n", signals[i].sample_id_offset);
            printf("    units: %s\n", signals[i].units);
            if (signals[i].signal_type == JLS_SIGNAL_TYPE_FSR) {
                int64_t length = 0;
                ROE(jls_rd_fsr_length(rd, signals[i].signal_id, &length));
                double duration = length / (double) signals[i].sample_rate;
                printf("    length: %" PRId64 " samples\n", length);
                printf("    duration: %.3f seconds\n", duration);
            }
        }
    } else {
        printf("Signals: none found\n");
    }

    jls_rd_close(rd);

    if (chunks) {
        struct jls_raw_s * raw;
        ROE(jls_raw_open(&raw, path, "r"));

        int32_t rc = 0;
        struct jls_chunk_header_s hdr;

        /*
        uint8_t * payload = malloc(PAYLOAD_MAX_SIZE);
        uint32_t payload_length_max = PAYLOAD_MAX_SIZE;
        if (NULL == payload) {
            printf("Out of memory: could not allocate payload\n");
            payload_length_max = 0;
        }
         */

        int64_t chunk_count = 0;
        while (1) {
            rc = jls_raw_rd_header(raw, &hdr);
            if (0 == rc) {
                if (hdr.tag != JLS_TAG_TRACK_FSR_DATA) {
                    printf("  %s %" PRId32 "\n", jls_tag_to_name(hdr.tag), hdr.payload_length);
                }
                ++chunk_count;
            } else if (rc == JLS_ERROR_EMPTY) {
                break;
            } else if (rc) {
                printf("jls_raw_rd_header failed on chunk %" PRId64 " with %d: %s\n",
                       chunk_count, rc, jls_error_code_name(rc));
                break;
            }
            rc = jls_raw_chunk_next(raw);
            if (0 == rc) {
                continue;
            } else if (rc == JLS_ERROR_EMPTY) {
                break;
            } else {
                printf("jls_raw_chunk_next failed on chunk %" PRId64 " with %d: %s\n",
                       chunk_count, rc, jls_error_code_name(rc));
                break;
            }
        }
        printf("Found %" PRId64 " total chunks\n", chunk_count);
        jls_raw_close(raw);
    }

    return 0;
}
