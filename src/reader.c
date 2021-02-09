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
#include "jls/format.h"
#include "jls/ec.h"
#include "jls/log.h"
#include "crc32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK_BUFFER_SIZE  (1 << 24)
static const uint8_t FILE_HDR[] = JLS_HEADER_IDENTIFICATION;

struct source_s {
    struct jls_source_def_s def;
};

struct signal_s {
    struct jls_signal_def_s def;
};

#define ROE(x)  do {                        \
    int32_t rc__ = (x);                     \
    if (rc__) {                             \
        return rc__;                        \
    }                                       \
} while (0)


struct jls_rd_s {
    struct source_s * sources[256];
    struct signal_s * signals[JLS_SIGNAL_COUNT];
    struct jls_chunk_header_s hdr;  // the header for the current chunk
    uint8_t * chunk_buffer;         // storage for the
    size_t chunk_buffer_sz;
    FILE * f;
};

static int32_t rd_file_header(FILE * file, struct jls_file_header_s * hdr) {
    if (sizeof(*hdr) != fread((uint8_t *) hdr, 1, sizeof(*hdr), file)) {
        JLS_LOGE("could not read file header");
        return JLS_ERROR_UNSUPPORTED_FILE;
    }
    uint32_t crc32 = jls_crc32(0, (uint8_t *) hdr, sizeof(*hdr) - 4);
    if (crc32 != hdr->crc32) {
        JLS_LOGE("file header crc mismatch: 0x%08x != 0x%08x", crc32, hdr->crc32);
        return JLS_ERROR_UNSUPPORTED_FILE;
    }

    if (0 != memcmp(FILE_HDR, hdr->identification, sizeof(FILE_HDR))) {
        JLS_LOGE("invalid file header identification");
        return JLS_ERROR_UNSUPPORTED_FILE;
    }

    if (hdr->version.s.major != JLS_FORMAT_VERSION_MAJOR) {
        JLS_LOGE("unsupported file format: %d", (int) hdr->version.s.major);
        return JLS_ERROR_UNSUPPORTED_FILE;
    }

    if (0 == hdr->length) {
        JLS_LOGW("file header length 0, not closed gracefully");
    }

    return 0;
}

static int32_t rd_header(struct jls_rd_s * self) {
    struct jls_chunk_header_s * hdr = &self->hdr;
    if (sizeof(hdr) != fread((uint8_t *) hdr, 1, sizeof(*hdr), self->f)) {
        JLS_LOGE("could not read chunk header");
        return JLS_ERROR_IO;
    }
    uint32_t crc32 = jls_crc32(0, (uint8_t *) hdr, sizeof(*hdr) - 4);
    if (crc32 != hdr->crc32) {
        return JLS_ERROR_MESSAGE_INTEGRITY;
    }
    return 0;
}

static inline uint32_t payload_size_on_disk(uint32_t payload_size) {
    uint8_t pad = (uint8_t) ((payload_size + 4) & 7);
    if (pad != 0) {
        pad = 8 - pad;
    }
    return payload_size + pad + 4;
}

static int32_t rd_payload(struct jls_rd_s * self) {
    uint32_t crc32_file;
    uint32_t crc32_calc;

    struct jls_chunk_header_s * hdr = &self->hdr;
    if ((hdr->payload_length + 16) > self->chunk_buffer_sz) {
        self->chunk_buffer_sz = hdr->payload_length + 1024;
                JLS_LOGE("increase chunk buffer to %d bytes", self->chunk_buffer_sz);
        self->chunk_buffer = realloc(self->chunk_buffer, self->chunk_buffer_sz);
        if (!self->chunk_buffer) {
            self->chunk_buffer_sz = 0;
            return JLS_ERROR_NOT_ENOUGH_MEMORY;
        }
    }

    uint8_t pad = (uint8_t) ((hdr->payload_length + 4) & 7);
    if (pad != 0) {
        pad = 8 - pad;
    }

    uint32_t rd_size = hdr->payload_length + pad + sizeof(crc32_file);
    if (rd_size != fread((uint8_t *) self->chunk_buffer, 1, rd_size, self->f)) {
        JLS_LOGE("could not read chunk payload");
        return JLS_ERROR_IO;
    }
    uint32_t k = hdr->payload_length + pad;
    crc32_calc = jls_crc32(0, self->chunk_buffer, hdr->payload_length);
    crc32_file = ((uint32_t) self->chunk_buffer[k])
            | (((uint32_t) self->chunk_buffer[k + 1]) << 8)
            | (((uint32_t) self->chunk_buffer[k + 1]) << 8)
            | (((uint32_t) self->chunk_buffer[k + 1]) << 8);
    if (crc32_calc != crc32_file) {
        JLS_LOGE("crc32 mismatch: 0x%08x != 0x%08x", crc32_file, crc32_calc);
        return JLS_ERROR_MESSAGE_INTEGRITY;
    }
    return 0;
}

static int32_t chunk_next(struct jls_rd_s * self) {
    struct jls_chunk_header_s * hdr = &self->hdr;
    long int offset = payload_size_on_disk(hdr->payload_length);
    offset += sizeof(*hdr);
    if (fseek(self->f, offset, SEEK_CUR)) {
        JLS_LOGW("could not seek next chunk");
        return JLS_ERROR_EMPTY;
    }
    return rd_header(self);
}

static int32_t chunk_first(struct jls_rd_s * self) {
    if (fseek(self->f, sizeof(struct jls_file_header_s), SEEK_CUR)) {
        JLS_LOGW("could not seek first chunk");
        return JLS_ERROR_EMPTY;
    }
    return rd_header(self);
}

static int32_t chunk_prev(struct jls_rd_s * self) {
    struct jls_chunk_header_s * hdr = &self->hdr;
    long int offset = payload_size_on_disk(hdr->payload_prev_length);
    offset += sizeof(*hdr);
    if (fseek(self->f, -offset, SEEK_CUR)) {
        JLS_LOGW("could not seek previous chunk");
        return JLS_ERROR_EMPTY;
    }
    return rd_header(self);
}

static int32_t item_next(struct jls_rd_s * self) {
    if (fseek(self->f, (long int) self->hdr.item_next, SEEK_SET)) {
        JLS_LOGW("could not seek next item");
        return JLS_ERROR_EMPTY;
    }
    return rd_header(self);
}

static int32_t item_prev(struct jls_rd_s * self) {
    if (fseek(self->f, (long int) self->hdr.item_prev, SEEK_CUR)) {
        JLS_LOGW("could not seek previous item");
        return JLS_ERROR_EMPTY;
    }
    return rd_header(self);
}

static int32_t chunk_first_def_by_tag(struct jls_rd_s * self, enum jls_tag_e tag) {
    ROE(chunk_first(self));
    while (self->hdr.tag != tag) {
        ROE(chunk_next(self));
        if (self->hdr.tag == JLS_TAG_FSR_DATA) {
            JLS_LOGW("tag %d not found", tag);
            return JLS_ERROR_NOT_FOUND;
        }
    }
    return 0;
}

int32_t jls_rd_open(struct jls_rd_s ** instance, const char * path) {
    if (!instance || !path) {
        return JLS_ERROR_PARAMETER_INVALID;
    }

    FILE * f = fopen(path, "rb");
    if (!f) {
        return JLS_ERROR_IO;
    }
    struct jls_file_header_s hdr;
    int rc = rd_file_header(f, &hdr);
    if (rc) {
        fclose(f);
        return rc;
    }

    struct jls_rd_s * self = calloc(1, sizeof(struct jls_rd_s));
    if (!self) {
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    self->f = f;
    self->chunk_buffer = malloc(CHUNK_BUFFER_SIZE);
    if (!self->chunk_buffer) {
        jls_rd_close(self);
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    self->chunk_buffer_sz = CHUNK_BUFFER_SIZE;

    return 0;
}

void jls_rd_close(struct jls_rd_s * self) {
    if (self) {
        if (self->f) {
            fclose(self->f);
            self->f = NULL;
        }
        if (self->chunk_buffer) {
            free(self->chunk_buffer);
            self->chunk_buffer_sz = 0;
            self->chunk_buffer = 0;
        }
        free(self);
    }
}
