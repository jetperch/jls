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

#include <io.h>  // windows
#include <fcntl.h>
#include <sys\stat.h>


#define CHUNK_BUFFER_SIZE  (1 << 24)
static const uint8_t FILE_HDR[] = JLS_HEADER_IDENTIFICATION;

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

struct jls_raw_s {
    struct jls_chunk_header_s hdr;  // the current chunk header.
    int64_t offset;                 // the offset for the current chunk
    int64_t fpos;                   // the current file position, to reduce ftell calls.
    int64_t fend;                   // the file end offset.
    int fd;
    uint8_t write_en;
};

static inline void invalidate_current_chunk(struct jls_raw_s * self) {
    self->hdr.tag = JLS_TAG_INVALID;
}

static inline uint32_t payload_size_on_disk(uint32_t payload_size) {
    uint8_t pad = (uint8_t) ((payload_size + 4) & 7);
    if (pad != 0) {
        pad = 8 - pad;
    }
    return payload_size + pad + 4;
}

// https://docs.microsoft.com/en-us/cpp/c-runtime-library/low-level-i-o?view=msvc-160
// The C standard library only gets in the way for JLS.
static int32_t f_open(struct jls_raw_s * self, const char * filename, const char * mode) {
    int oflag;
    int shflag;

    switch (mode[0]) {
        case 'w':
            oflag = _O_BINARY | _O_CREAT | _O_RDWR | _O_RANDOM | _O_TRUNC;
            shflag = _SH_DENYWR;
            break;
        case 'r':
            oflag = _O_BINARY | _O_RDONLY | _O_RANDOM;
            shflag = _SH_DENYNO;
            break;
        case 'a':
            oflag = _O_BINARY | _O_RDWR | _O_RANDOM;
            shflag = _SH_DENYWR;
            break;
        default:
            return JLS_ERROR_PARAMETER_INVALID;
    }
    errno_t err = _sopen_s(&self->fd, filename, oflag, shflag, _S_IREAD | _S_IWRITE);
    if (err != 0) {
        JLS_LOGW("open failed with %d: filename=%s, mode=%s", err, filename, mode);
        return JLS_ERROR_IO;
    }
    return 0;
}

static int32_t f_close(struct jls_raw_s * self) {
    if (self->fd != -1) {
        _close(self->fd);
        self->fd = -1;
    }
    return 0;
}

static int32_t f_write(struct jls_raw_s * self, const void * buffer, unsigned int count) {
    int sz = _write(self->fd, buffer, count);
    if (sz < 0) {
        JLS_LOGE("write failed %d", errno);
        return JLS_ERROR_IO;
    }
    self->fpos += sz;
    if (self->fpos > self->fend) {
        self->fend = self->fpos;
    }
    if (sz != count) {
        JLS_LOGE("write mismatch %d != %d", sz, count);
        return JLS_ERROR_IO;
    }
    return 0;
}

static int32_t f_read(struct jls_raw_s * self, void * const buffer, unsigned const buffer_size) {
    int sz = _read(self->fd, buffer, buffer_size);
    if (sz < 0) {
        JLS_LOGE("read failed %d", errno);
        return JLS_ERROR_IO;
    }
    self->fpos += sz;
    if (sz != buffer_size) {
        JLS_LOGE("write mismatch %d != %d", sz, buffer_size);
        return JLS_ERROR_IO;
    }
    return 0;
}

static int32_t f_seek(struct jls_raw_s * self, int64_t offset, int origin) {
    int64_t pos = _lseeki64(self->fd, offset, origin);
    if (pos < 0) {
        JLS_LOGE("seek fail %d", errno);
        return JLS_ERROR_IO;
    }
    if ((origin == SEEK_SET) && (pos != offset)) {
        JLS_LOGE("seek fail %d", errno);
        return JLS_ERROR_IO;
    }
    self->fpos = pos;
    return 0;
}

static inline int64_t f_tell(struct jls_raw_s * self) {
    return _telli64(self->fd);
}

static int32_t wr_file_header(struct jls_raw_s * self) {
    int32_t rc = 0;
    int64_t pos = f_tell(self);
    f_seek(self, 0L, SEEK_END);
    int64_t file_sz = f_tell(self);
    f_seek(self, 0L, SEEK_SET);

    struct jls_file_header_s hdr = {
            .identification = JLS_HEADER_IDENTIFICATION,
            .length = file_sz,
            .version = {.u32 = JLS_FORMAT_VERSION_U32},
            .crc32 = 0,
    };
    hdr.crc32 = jls_crc32(0, (uint8_t *) &hdr, sizeof(hdr) - 4);
    RLE(f_write(self, &hdr, sizeof(hdr)));
    if (pos != 0) {
        f_seek(self, pos, SEEK_SET);
    } else {
        self->offset = self->fpos;
    }
    return rc;
}

static int32_t rd_file_header(struct jls_raw_s * self, struct jls_file_header_s * hdr) {
    if (f_read(self, hdr, sizeof(*hdr))) {
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

static void fend_get(struct jls_raw_s * self) {
    int64_t pos = f_tell(self);
    if (f_seek(self, 0, SEEK_END)) {
        JLS_LOGE("seek to end failed");
    } else {
        self->fend = f_tell(self);
        f_seek(self, pos, SEEK_SET);
    }
}

static int32_t read_verify(struct jls_raw_s * self) {
    struct jls_file_header_s file_hdr;
    if (!self->fd) {
        return JLS_ERROR_IO;
    }
    int32_t rc = rd_file_header(self, &file_hdr);
    self->offset = self->fpos;
    if (!rc) {
        fend_get(self);
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
    self->fd = -1;
    ROE(f_open(self, path, mode));

    switch (mode[0]) {
        case 'w':
            self->write_en = 1;
            rc = wr_file_header(self);
            self->offset = self->fpos;
            break;
        case 'r':
            rc = read_verify(self);
            break;
        case 'a':
            self->write_en = 1;
            rc = read_verify(self);
            if (f_seek(self, 0, SEEK_END)) {
                rc = JLS_ERROR_IO;
            } else {
                self->offset = self->fpos;
            }
            break;
        default:
            rc = JLS_ERROR_PARAMETER_INVALID;
    }

    if (rc) {
        f_close(self);
        free(self);
    } else {
        *instance = self;
    }
    return rc;
}

int32_t jls_raw_close(struct jls_raw_s * self) {
    if (self) {
        if ((self->fd != -1) && (self->write_en)) {
            wr_file_header(self);
        }
        f_close(self);
        free(self);
    }
    return 0;
}

int32_t jls_raw_wr(struct jls_raw_s * self, struct jls_chunk_header_s * hdr, const uint8_t * payload) {
    RLE(jls_raw_wr_header(self, hdr));
    RLE(jls_raw_wr_payload(self, hdr->payload_length, payload));
    invalidate_current_chunk(self);
    self->offset = self->fpos;
    return 0;
}

int32_t jls_raw_wr_header(struct jls_raw_s * self, struct jls_chunk_header_s * hdr) {
    hdr->crc32 = jls_crc32(0, (uint8_t *) hdr, sizeof(*hdr) - 4);
    if (self->offset != self->fpos) {
        invalidate_current_chunk(self);
        RLE(f_seek(self, self->offset, SEEK_SET));
    }
    if (f_write(self, hdr, sizeof(*hdr))) {
        return JLS_ERROR_IO;
    }
    self->hdr = *hdr;
    return 0;
}

int32_t jls_raw_wr_payload(struct jls_raw_s * self, uint32_t payload_length, const uint8_t * payload) {
    if (!payload_length) {
        return 0;  // no action necessary
    }
    if (!self || !payload) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    struct jls_chunk_header_s * hdr = &self->hdr;

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

    RLE(f_write(self, payload, hdr->payload_length));
    return f_write(self, footer, pad + 4);
}

int32_t jls_raw_rd(struct jls_raw_s * self, struct jls_chunk_header_s * hdr, uint32_t payload_length_max, uint8_t * payload) {
    RLE(jls_raw_rd_header(self, hdr));
    RLE(jls_raw_rd_payload(self, payload_length_max, payload));
    invalidate_current_chunk(self);
    self->offset = self->fpos;
    return 0;
}

int32_t jls_raw_rd_header(struct jls_raw_s * self, struct jls_chunk_header_s * hdr) {
    struct jls_chunk_header_s * h = &self->hdr;
    if (hdr) {
        hdr->tag = JLS_TAG_INVALID;
    }
    if (self->hdr.tag == JLS_TAG_INVALID) {
        if (self->fpos >= self->fend) {
            invalidate_current_chunk(self);
            return JLS_ERROR_EMPTY;
        }
        if (self->offset != self->fpos) {
            if (f_seek(self, self->offset, SEEK_SET)) {
                JLS_LOGE("seek failed");
                invalidate_current_chunk(self);
                return JLS_ERROR_IO;
            }
        }
        self->offset = self->fpos;
        if (f_read(self, (uint8_t *) h, sizeof(*h))) {
            invalidate_current_chunk(self);
            return JLS_ERROR_EMPTY;
        }
        uint32_t crc32 = jls_crc32(0, (uint8_t *) h, sizeof(*h) - 4);
        if (crc32 != h->crc32) {
            JLS_LOGE("chunk header crc error: %u != %u", crc32, h->crc32);
            invalidate_current_chunk(self);
            return JLS_ERROR_MESSAGE_INTEGRITY;
        }
    }
    if (hdr) {
        *hdr = self->hdr;
    }
    return 0;
}

int32_t jls_raw_rd_payload(struct jls_raw_s * self, uint32_t payload_length_max, uint8_t * payload) {
    uint32_t crc32_file;
    uint32_t crc32_calc;
    struct jls_chunk_header_s * hdr = &self->hdr;
    if (hdr->tag == JLS_TAG_INVALID) {
        RLE(jls_raw_rd_header(self, hdr));
    }
    uint32_t rd_size = payload_size_on_disk(hdr->payload_length);

    if (rd_size > payload_length_max) {
        return JLS_ERROR_TOO_BIG;
    }

    int64_t pos = self->offset + sizeof(struct jls_chunk_header_s);
    if (pos != self->fpos) {
        f_seek(self, pos, SEEK_SET);
        self->fpos = pos;
    }

    RLE(f_read(self, (uint8_t *) payload, rd_size));
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

int32_t jls_raw_chunk_seek(struct jls_raw_s * self, int64_t offset) {
    invalidate_current_chunk(self);
    if (f_seek(self, offset, SEEK_SET)) {
        return JLS_ERROR_IO;
    }
    self->offset = self->fpos;
    return 0;
}

int64_t jls_raw_chunk_tell(struct jls_raw_s * self) {
    return self->offset;
}

int32_t jls_raw_chunk_next(struct jls_raw_s * self) {
    RLE(jls_raw_rd_header(self, NULL));  // ensure that we have the header
    invalidate_current_chunk(self);
    int64_t offset = self->offset;
    int64_t pos = offset;
    pos += sizeof(struct jls_chunk_header_s) + payload_size_on_disk(self->hdr.payload_length);
    if (pos > self->fend) {
        return JLS_ERROR_EMPTY;
    }
    if (pos != self->fpos) {
        // sequential access
        if (f_seek(self, pos, SEEK_SET)) {
            return JLS_ERROR_EMPTY;
        }
    }
    self->offset = self->fpos;
    return 0;
}

int32_t jls_raw_chunk_prev(struct jls_raw_s * self) {
    if (self->fpos >= self->fend) {
        invalidate_current_chunk(self);
        return JLS_ERROR_NOT_FOUND;
    }
    RLE(jls_raw_rd_header(self, NULL));  // ensure that we have the header
    invalidate_current_chunk(self);
    int64_t offset = self->offset;
    int64_t pos = offset;
    pos -= sizeof(struct jls_chunk_header_s) + payload_size_on_disk(self->hdr.payload_prev_length);
    if (pos < sizeof(struct jls_file_header_s)) {
        return JLS_ERROR_EMPTY;
    }
    if (pos != self->fpos) {
        // sequential access
        f_seek(self, pos, SEEK_SET);
    }
    self->offset = self->fpos;
    return 0;
}

int32_t jls_raw_item_next(struct jls_raw_s * self) {
    RLE(jls_raw_rd_header(self, NULL));  // ensure that we have the header
    int64_t offset = self->hdr.item_next;
    int64_t pos = offset;
    if ((pos == 0) || (pos > self->fend)) {
        return JLS_ERROR_EMPTY;
    }

    invalidate_current_chunk(self);
    if (f_seek(self, pos, SEEK_SET)) {
        return JLS_ERROR_EMPTY;
    }
    self->offset = self->fpos;
    return 0;
}

int32_t jls_raw_item_prev(struct jls_raw_s * self) {
    if (self->fpos >= self->fend) {
        invalidate_current_chunk(self);
        return JLS_ERROR_NOT_FOUND;
    }
    RLE(jls_raw_rd_header(self, NULL));  // ensure that we have the header
    int64_t offset = self->offset;
    int64_t pos = self->hdr.item_prev;
    if (pos == 0) {
        return JLS_ERROR_EMPTY;
    }
    invalidate_current_chunk(self);
    RLE(f_seek(self, pos, SEEK_SET));
    self->offset = self->fpos;
    return 0;
}