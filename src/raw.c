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

#include "jls/raw.h"
#include "jls/format.h"
#include "jls/ec.h"
#include "jls/log.h"
#include "crc32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define CHUNK_BUFFER_SIZE  (1 << 24)
static const uint8_t FILE_HDR[] = JLS_HEADER_IDENTIFICATION;
#define FSEEK64 _fseeki64
#define FTELL64 _ftelli64


#define RLE(x)  do {                        \
    int32_t rc__ = (x);                     \
    if (rc__) {                             \
        JLS_LOGE("error %d: " #x, rc__);    \
        return rc__;                        \
    }                                       \
} while (0)

static inline uint32_t payload_size_on_disk(uint32_t payload_size) {
    uint8_t pad = (uint8_t) ((payload_size + 4) & 7);
    if (pad != 0) {
        pad = 8 - pad;
    }
    return payload_size + pad + 4;
}

static int32_t wr_file_header(struct jls_raw_s * self) {
    int32_t rc = 0;
    int64_t pos = FTELL64(self->f);
    FSEEK64(self->f, 0L, SEEK_END);
    int64_t file_sz = FTELL64(self->f);
    FSEEK64(self->f, 0L, SEEK_SET);

    struct jls_file_header_s hdr = {
            .identification = JLS_HEADER_IDENTIFICATION,
            .length = file_sz,
            .version = {.u32 = JLS_FORMAT_VERSION_U32},
            .crc32 = 0,
    };
    hdr.crc32 = jls_crc32(0, (uint8_t *) &hdr, sizeof(hdr) - 4);
    size_t hdr_sz = fwrite(&hdr, 1, sizeof(hdr), self->f);
    if (hdr_sz != sizeof(hdr)) {
        rc = JLS_ERROR_IO;
    }
    if (pos != 0) {
        FSEEK64(self->f, pos, SEEK_SET);
    } else {
        self->fpos = sizeof(hdr);
        self->offset = self->fpos;
    }
    return rc;
}

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

static int32_t read_verify(struct jls_raw_s * self) {
    struct jls_file_header_s file_hdr;
    struct jls_chunk_header_s hdr;
    if (!self->f) {
        return JLS_ERROR_IO;
    }
    int32_t rc = rd_file_header(self->f, &file_hdr);
    self->offset = sizeof(file_hdr);
    self->fpos = self->offset;
    if (!rc) {
        rc = jls_raw_rd_header(self, &hdr);
        if (rc == JLS_ERROR_EMPTY) {
            self->hdr.tag = JLS_TAG_INVALID;
            rc = 0;
        }
    }
    return rc;
}

int32_t jls_raw_open(struct jls_raw_s ** instance, const char * path, const char * mode) {
    int32_t rc = 0;

    if (!instance || !path || !mode) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    *instance = NULL;

    struct jls_raw_s * self = calloc(1, sizeof(struct jls_raw_s));
    if (!self) {
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }

    switch (mode[0]) {
        case 'w':
            self->f = fopen(path, "wb+");
            self->write_en = 1;
            if (!self->f) {
                rc = JLS_ERROR_IO;
            } else {
                rc = wr_file_header(self);
                self->offset = sizeof(struct jls_file_header_s);
                self->fpos = self->offset;
            }
            break;
        case 'r':
            self->f = fopen(path, "rb");
            rc = read_verify(self);
            break;
        case 'a':
            self->f = fopen(path, "rb+");
            self->write_en = 1;
            rc = read_verify(self);
            if (FSEEK64(self->f, 0, SEEK_END)) {
                rc = JLS_ERROR_IO;
            } else {
                self->fpos = FTELL64(self->f);
                self->offset = self->fpos;
                self->hdr.tag = JLS_TAG_INVALID;
            }
            break;
        default:
            rc = JLS_ERROR_PARAMETER_INVALID;
    }

    if (rc) {
        if (self->f) {
            fclose(self->f);
            self->f = NULL;
        }
        free(self);
    } else {
        *instance = self;
    }
    return rc;
}

int32_t jls_raw_close(struct jls_raw_s * self) {
    if (self) {
        if (self->f) {
            if (self->write_en) {
                wr_file_header(self);
            }
            fclose(self->f);
            self->f = NULL;
        }
        free(self);
    }
    return 0;
}

int32_t jls_raw_wr(struct jls_raw_s * self, uint8_t tag, uint16_t chuck_meta, uint32_t payload_length, const uint8_t * payload) {
    RLE(jls_raw_wr_header(self, tag, chuck_meta, payload_length));
    RLE(jls_raw_wr_payload(self, payload_length, payload));
    return 0;
}
int32_t jls_raw_wr_header(struct jls_raw_s * self, uint8_t tag, uint16_t chuck_meta, uint32_t payload_length) {
    struct jls_chunk_header_s * hdr = &self->hdr;
    self->payload_length_prev = self->hdr.payload_length;
    hdr->item_next = 0;
    hdr->item_prev = 0;
    hdr->tag = tag;
    hdr->rsv0_u8 = 0;
    hdr->chuck_meta = chuck_meta;
    hdr->payload_length = payload_length;
    hdr->payload_prev_length = self->payload_length_prev;
    hdr->crc32 = jls_crc32(0, (uint8_t *) hdr, sizeof(*hdr) - 4);
    size_t sz = fwrite(hdr, 1, sizeof(*hdr), self->f);
    self->fpos += sz;
    if (sz != sizeof(*hdr)) {
        JLS_LOGE("could not write chunk header: %zu != %zu", sizeof(*hdr), sz);
        return JLS_ERROR_IO;
    }
    return 0;
}

int32_t jls_raw_wr_payload(struct jls_raw_s * self, uint32_t payload_length, const uint8_t * payload) {
    struct jls_chunk_header_s * hdr = &self->hdr;
    size_t sz;
    if (payload_length != hdr->payload_length) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    uint8_t footer[11] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t pad = (uint8_t) ((hdr->payload_length + 4) & 7);
    if (pad != 0) {
        pad = 8 - pad;
    }
    uint32_t crc32 = jls_crc32(0, payload, hdr->payload_length);
    footer[pad + 0] = crc32 & 0xff;
    footer[pad + 1] = (crc32 >> 8) & 0xff;
    footer[pad + 2] = (crc32 >> 16) & 0xff;
    footer[pad + 3] = (crc32 >> 24) & 0xff;

    sz = fwrite(payload, 1, hdr->payload_length, self->f);
    self->fpos += sz;
    if (sz != hdr->payload_length) {
        return JLS_ERROR_IO;
    }
    sz = fwrite(footer, 1, pad + 4, self->f);
    self->fpos += sz;
    if (sz != pad + 4) {
        return JLS_ERROR_IO;
    }
    return 0;
}

int32_t jls_raw_rd(struct jls_raw_s * self, struct jls_chunk_header_s * hdr, uint32_t payload_length_max, uint8_t * payload) {
    if (self->fpos == (self->offset + sizeof(struct jls_chunk_header_s))) {
        // normal case, header already read.
    } else {
        RLE(jls_raw_rd_header(self, hdr));
    }
    RLE(jls_raw_rd_payload(self, payload_length_max, payload));
    return 0;
}

int32_t jls_raw_rd_header(struct jls_raw_s * self, struct jls_chunk_header_s * hdr) {
    if (!hdr) {
        hdr = &self->hdr;
    }
    self->offset = self->fpos;
    size_t sz = fread((uint8_t *) hdr, 1, sizeof(*hdr), self->f);
    self->fpos += sz;
    if (!sz) {
        self->hdr.tag = JLS_TAG_INVALID;
        return JLS_ERROR_EMPTY;
    }
    if (sizeof(*hdr) != sz) {
        JLS_LOGE("could not read chunk header: %zu != %zu", sizeof(*hdr), sz);
        return JLS_ERROR_IO;
    }
    uint32_t crc32 = jls_crc32(0, (uint8_t *) hdr, sizeof(*hdr) - 4);
    if (crc32 != hdr->crc32) {
        JLS_LOGE("chunk header crc error: %u != %u", crc32, hdr->crc32);
        return JLS_ERROR_MESSAGE_INTEGRITY;
    }
    self->hdr = *hdr;
    return 0;
}

int32_t jls_raw_rd_payload(struct jls_raw_s * self, uint32_t payload_length_max, uint8_t * payload) {
    uint32_t crc32_file;
    uint32_t crc32_calc;
    struct jls_chunk_header_s * hdr = &self->hdr;
    uint32_t rd_size = payload_size_on_disk(hdr->payload_length);
    size_t fread_sz;

    if (rd_size > payload_length_max) {
        return JLS_ERROR_TOO_BIG;
    }

    int64_t pos = self->offset + sizeof(struct jls_chunk_header_s);
    if (pos != self->fpos) {
        FSEEK64(self->f, pos, SEEK_SET);
        self->fpos = pos;
    }

    fread_sz = fread((uint8_t *) payload, 1, rd_size, self->f);
    self->fpos += fread_sz;
    if (rd_size != fread_sz) {
        JLS_LOGE("could not read chunk payload");
        return JLS_ERROR_IO;
    }
    crc32_calc = jls_crc32(0, payload, hdr->payload_length);
    crc32_file = ((uint32_t) payload[rd_size - 4])
                 | (((uint32_t) payload[rd_size - 3]) << 8)
                 | (((uint32_t) payload[rd_size - 2]) << 16)
                 | (((uint32_t) payload[rd_size - 1]) << 24);
    if (crc32_calc != crc32_file) {
        JLS_LOGE("crc32 mismatch: 0x%08x != 0x%08x", crc32_file, crc32_calc);
        return JLS_ERROR_MESSAGE_INTEGRITY;
    }
    return 0;
}

int32_t jls_raw_chunk_next(struct jls_raw_s * self, struct jls_chunk_header_s * hdr) {
    if (hdr) {
        hdr->tag = JLS_TAG_INVALID;
    }
    if (self->hdr.tag == JLS_TAG_INVALID) {
        return JLS_ERROR_EMPTY;
    }
    int64_t offset = self->offset;
    int64_t pos = offset;
    pos += sizeof(struct jls_chunk_header_s) + payload_size_on_disk(self->hdr.payload_length);
    if (pos != self->fpos) {
        // sequential access
        if (FSEEK64(self->f, pos, SEEK_SET)) {
            return JLS_ERROR_EMPTY;
        }
        self->fpos = FTELL64(self->f);
    }
    self->payload_length_prev = self->hdr.payload_length;
    int32_t rc = jls_raw_rd_header(self, hdr);
    return rc;
}

int32_t jls_raw_chunk_prev(struct jls_raw_s * self, struct jls_chunk_header_s * hdr) {
    if (hdr) {
        hdr->tag = JLS_TAG_INVALID;
    }
    int64_t offset = self->offset;
    int64_t pos = offset;
    pos -= sizeof(struct jls_chunk_header_s) + payload_size_on_disk(self->hdr.payload_prev_length);
    if (pos < sizeof(struct jls_file_header_s)) {
        return JLS_ERROR_EMPTY;
    }
    if (pos != self->fpos) {
        // sequential access
        FSEEK64(self->f, pos, SEEK_SET);
        self->fpos = pos;
    }

    int32_t rc = jls_raw_rd_header(self, hdr);
    if (rc == JLS_ERROR_EMPTY) {
        self->offset = offset;
        self->payload_length_prev = 0;
    } else {
        self->payload_length_prev = self->hdr.payload_length;
    }
    return rc;
}
