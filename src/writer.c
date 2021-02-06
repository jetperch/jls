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
#include "jls/raw.h"
#include "jls/format.h"
#include "jls/ec.h"
#include "jls/log.h"
#include "crc32.h"
#include <stdio.h>
#include <stdlib.h>

struct jls_wr_s {
    struct jls_raw_s * raw;
};


#define ROE(x)  do {                        \
    int32_t rc__ = (x);                     \
    if (rc__) {                             \
        JLS_LOGE("error %d: " #x, rc__);    \
        return rc__;                        \
    }                                       \
} while (0)


int32_t jls_wr_open(struct jls_wr_s ** instance, const char * path) {
    if (!instance) {
        return JLS_ERROR_PARAMETER_INVALID;
    }

    struct jls_wr_s * wr = calloc(1, sizeof(struct jls_wr_s));
    if (!wr) {
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }

    int32_t rc = jls_raw_open(&wr->raw, path, "w");
    if (rc) {
        free(wr);
    } else {
        *instance = wr;
    }
    return rc;
}

int32_t jls_wr_close(struct jls_wr_s * self) {
    if (self) {
        int32_t rc = jls_raw_close(self->raw);
        free(self);
        return rc;
    }
    return 0;
}

int32_t jls_wr_source_def(struct jls_wr_s * self, const struct jls_source_def_s * source) {
    if (!self || !source) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    return jls_raw_wr(self->raw, JLS_TAG_SOURCE_DEF, 0, sizeof(*source), (const uint8_t *) source);
}
