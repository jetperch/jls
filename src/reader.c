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

#include "jls/reader.h"
#include "jls/raw.h"
#include "jls/format.h"
#include "jls/ec.h"
#include "jls/log.h"
#include "crc32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAYLOAD_BUFFER_SIZE_DEFAULT (1 << 25)   // 32 MB
#define STRING_BUFFER_SIZE_DEFAULT (1 << 20)    // 1 MB


#define ROE(x)  do {                        \
    int32_t rc__ = (x);                     \
    if (rc__) {                             \
        return rc__;                        \
    }                                       \
} while (0)

#define RLE(x)  do {                        \
    int32_t rc__ = (x);                     \
    if (rc__) {                             \
        JLS_LOGE("error %d: " #x, rc__);    \
        return rc__;                        \
    }                                       \
} while (0)

#if 0
struct source_s {
    struct jls_source_def_s def;
};

struct signal_s {
    struct jls_signal_def_s def;
};

struct jls_rd_s {
    struct source_s * sources[256];
    struct signal_s * signals[JLS_SIGNAL_COUNT];
    struct jls_chunk_header_s hdr;  // the header for the current chunk
    uint8_t * chunk_buffer;         // storage for the
    size_t chunk_buffer_sz;
    FILE * f;
};
#endif

struct jls_rd_s {
    struct jls_raw_s * raw;
    struct jls_source_def_s sources[256];

    uint8_t * payload_buffer;
    size_t payload_buffer_size;

    char * string_buffer;
    size_t string_buffer_size;
};

static int32_t scan(struct jls_rd_s * self) {
    int32_t rc = 0;
    struct jls_chunk_header_s hdr;
    while (1) {
        rc = jls_raw_rd(self->raw, &hdr, self->payload_buffer_size, self->payload_buffer);
        if (rc == JLS_ERROR_TOO_BIG) {
            size_t sz_new = self->payload_buffer_size;
            while (sz_new < hdr.payload_length) {
                sz_new *= 2;
            }
            uint8_t * ptr = realloc(self->payload_buffer, sz_new);
            if (!ptr) {
                return JLS_ERROR_NOT_ENOUGH_MEMORY;
            }
        } else if (rc == JLS_ERROR_EMPTY) {
            return 0;
        } else if (rc) {
            return rc;
        }

        JLS_LOGI("tag %d : %s", hdr.tag, jls_tag_to_name(hdr.tag));

        switch (hdr.tag) {
            case JLS_TAG_SOURCE_DEF:
                break;
            default:
                break;
        }
    }
}

int32_t jls_rd_open(struct jls_rd_s ** instance, const char * path) {
    if (!instance) {
        return JLS_ERROR_PARAMETER_INVALID;
    }

    struct jls_rd_s *self = calloc(1, sizeof(struct jls_rd_s));
    if (!self) {
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }

    self->payload_buffer = malloc(PAYLOAD_BUFFER_SIZE_DEFAULT);
    self->string_buffer = malloc(STRING_BUFFER_SIZE_DEFAULT);
    if (!self->payload_buffer || !self->string_buffer) {
        jls_rd_close(self);
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    self->payload_buffer_size = PAYLOAD_BUFFER_SIZE_DEFAULT;
    self->string_buffer_size = STRING_BUFFER_SIZE_DEFAULT;

    int32_t rc = jls_raw_open(&self->raw, path, "r");
    if (rc) {
        jls_rd_close(self);
        return rc;
    }

    ROE(scan(self));

    *instance = self;
    return rc;
}

void jls_rd_close(struct jls_rd_s * self) {
    if (self) {
        jls_raw_close(self->raw);
        if (self->string_buffer) {
            free(self->string_buffer);
            self->string_buffer = NULL;
        }
        if (self->payload_buffer) {
            free(self->payload_buffer);
            self->payload_buffer = NULL;
        }
        self->raw = NULL;
        free(self);
    }
}
