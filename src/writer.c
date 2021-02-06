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
#include "jls/format.h"
#include "jls/ec.h"
#include "crc32.h"
#include <stdio.h>
#include <stdlib.h>

struct jls_wr_s {
    FILE * f;
};


int32_t jls_wr_header(struct jls_wr_s * self, uint64_t length) {
    struct jls_file_header_s hdr = {
            .identification = JLS_HEADER_IDENTIFICATION,
            .length = length,
            .version = JLS_FORMAT_VERSION_U32,
            .crc32 = 0,
    };
    hdr.crc32 = jls_crc32(0, (uint8_t *) &hdr, sizeof(hdr) - 4);
    size_t sz = fwrite(&hdr, 1, sizeof(hdr), self->f);
    if (sz != sizeof(hdr)) {
        return JLS_ERROR_IO;
    }
    return 0;
}

int32_t jls_wr_open(struct jls_wr_s ** instance, const char * path) {
    int32_t rc;

    if (!instance || !path) {
        return JLS_ERROR_PARAMETER_INVALID;
    }

    FILE * f = fopen(path, "wb+");
    if (!f) {
        return JLS_ERROR_IO;
    }

    struct jls_wr_s * self = calloc(1, sizeof(struct jls_wr_s));
    if (!self) {
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    self->f = f;
    rc = jls_wr_header(self, 0);
    if (rc) {
        return rc;
    }

    *instance = self;
    return 0;
}

int32_t jls_wr_close(struct jls_wr_s * self) {
    if (self) {
        if (self->f) {
            fseek(self->f, 0L, SEEK_END);
            size_t sz = ftell(self->f);
            fseek(self->f, 0L, SEEK_SET);
            jls_wr_header(self, sz);
            fclose(self->f);
            self->f = NULL;
        }
        free(self);
    }
    return 0;
}
