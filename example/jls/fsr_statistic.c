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
#include "jls_util_prv.h"
#include "cstr.h"
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>


static int usage(void) {
    printf("usage: jls fsr_statistic <jls_path> <signal_id> <start> <increment> <count>\n"
           "  jls_path        The path to the JLS input file.\n"
           "  signal_id       The signal_id to extract.\n"
           "  start           The starting sample id to read.\n"
           "  increment       The number of samples that form a single output summary.\n"
           "  count           The number of statistics points to populate.\n"
           "\n");
    return 1;
}

int on_fsr_statistics(struct app_s * self, int argc, char * argv[]) {
    struct jls_rd_s * rd = NULL;
    char * jls_path = NULL;
    uint16_t signal_id = 0;
    int64_t start = 0;
    int64_t increment = 0;
    int64_t count = 0;
    int pos_arg = 0;
    (void) self;

    while (argc) {
        if (argv[0][0] != '-') {
            switch (pos_arg) {
                case 0:
                    jls_path = argv[0];
                    break;
                case 1:
                    if (jls_cstr_to_u16(argv[0], &signal_id)) {
                        printf("Invalid signal_id\n");
                        return usage();
                    }
                    break;
                case 2:
                    if (jls_cstr_to_i64(argv[0], &start)) {
                        printf("Invalid start\n");
                        return usage();
                    }
                    break;
                case 3:
                    if (jls_cstr_to_i64(argv[0], &increment)) {
                        printf("Invalid increment\n");
                        return usage();
                    }
                    break;
                case 4:
                    if (jls_cstr_to_i64(argv[0], &count)) {
                        printf("Invalid count\n");
                        return usage();
                    }
                    break;
                default:
                    printf("Too many positional arguments\n");
                    return usage();
            }
            ARG_CONSUME();
            ++pos_arg;
        } else {
            return usage();
        }
    }
    if (pos_arg != 5) {
        return usage();
    }

    ROE(jls_rd_open(&rd, jls_path));
    double * data = malloc(count * JLS_SUMMARY_FSR_COUNT * sizeof(double));
    if (NULL == data) {
        printf("could not allocate data.\n");
    } else {
        int32_t rc = jls_rd_fsr_statistics(rd, signal_id, start, increment, data, count);
        if (rc) {
            printf("jls_rd_fsr_statistics returned %" PRIi32 ": %s\n", rc, jls_error_code_name(rc));
        } else {
            for (int64_t i = 0; i < count; ++i) {
                double * d = data + JLS_SUMMARY_FSR_COUNT * i;
                printf("%f,%f,%f,%f\n", d[0], d[1], d[2], d[3]);
            }
        }
    }
    jls_rd_close(rd);
    return 0;
}
