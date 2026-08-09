/*
 * Copyright 2026 Jetperch LLC
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
#include "jls/repair.h"
#include "jls_util_prv.h"
#include <stdio.h>
#include <stdbool.h>
#include <string.h>


static int usage(void) {
    printf("usage: jls repair <path> [--no-progress]\n"
           "\n"
           "Repair a damaged JLS file in place.  Not needed after a\n"
           "writer crash or power loss: opening the file recovers\n"
           "simple truncation automatically.  Use this when opening\n"
           "fails with MESSAGE_INTEGRITY.  For read-only media, use\n"
           "'jls copy' instead.\n");
    return 1;
}

static int32_t repair_msg_fn(void * user_data, const char * msg) {
    (void) user_data;
    printf("\n%s\n", msg);
    return quit_ ? 1 : 0;
}

static int32_t repair_progress_fn(void * user_data, double progress) {
    (void) user_data;
    char line[256];
    int bar_len = 50;
    for (int i = 0; i < bar_len; ++i) {
        line[i] = (progress >= (i / (double) (bar_len - 1))) ? '=' : '-';
    }
    line[bar_len] = 0;
    printf("%s %.1f%%\r", line, progress * 100);
    fflush(stdout);
    return quit_ ? 1 : 0;
}

int on_repair(struct app_s * self, int argc, char * argv[]) {
    char * path = NULL;
    (void) self;
    bool no_progress = false;

    while (argc) {
        if (argv[0][0] != '-') {
            if (NULL != path) {
                return usage();
            }
            path = argv[0];
            ARG_CONSUME();
        } else if (0 == strcmp("--no-progress", argv[0])) {
            no_progress = true;
            ARG_CONSUME();
        } else {
            return usage();
        }
    }
    if (NULL == path) {
        return usage();
    }

    int32_t rc = jls_repair(path,
                            repair_msg_fn, NULL,
                            no_progress ? NULL : repair_progress_fn, NULL);
    printf("\n");
    if (rc) {
        printf("ERROR: %d %s : %s\n", rc, jls_error_code_name(rc), jls_error_code_description(rc));
    }
    return rc;
}
