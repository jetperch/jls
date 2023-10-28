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
#include "jls/copy.h"
#include "jls_util_prv.h"
#include <stdio.h>
#include <inttypes.h>


#define PAYLOAD_MAX_SIZE (32U * 1024U * 1024U)


static int usage(void) {
    printf("usage: jls copy <src> <dst>\n");
    return 1;
}

int32_t msg_fn(void * user_data, const char * msg) {
    (void) user_data;
    printf("%s\n", msg);
    return 0;
}

int32_t progress_fn(void * user_data, double progress) {
    (void) user_data;
    (void) progress;
    return 0;
}

int on_copy(struct app_s * self, int argc, char * argv[]) {
    char * src;
    char * dst;
    int pos_arg = 0;
    (void) self;

    while (argc) {
        if (argv[0][0] != '-') {
            if (pos_arg == 0) {
                src = argv[0];
            } else if (pos_arg == 1) {
                dst = argv[0];
            } else {
                return usage();
            }
            ARG_CONSUME();
            ++pos_arg;
        } else {
            return usage();
        }
    }
    if (pos_arg != 2) {
        return usage();
    }

    int32_t rc = jls_copy(src, dst, msg_fn, NULL, progress_fn, NULL);
    if (rc) {
        printf("ERROR: %d %s : %s\n", rc, jls_error_code_name(rc), jls_error_code_description(rc));
    }
    return rc;
}
