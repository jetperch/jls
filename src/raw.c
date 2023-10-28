/*
 * Copyright 2021-2022 Jetperch LLC
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
#include "jls/time.h"
#include "jls/ec.h"
#include "jls/log.h"
#include "jls/crc32c.h"
#include "jls/version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "jls/backend.h"


#define CRC_SIZE (4)
#define HEADER_ALIGN (8)            // must be power of 2 and greater than CRC_SIZE
#define SCAN_SIZE (4096)
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
    struct jls_bkf_s backend;
    struct jls_chunk_header_s hdr;  // the current chunk header.
    int64_t offset;                 // the offset for the current chunk
    uint32_t last_payload_length;   // the payload length for the last chunk in the file.
    uint8_t write_en;
    union jls_version_u version;
};

static inline void invalidate_current_chunk(struct jls_raw_s * self) {
    self->hdr.tag = JLS_TAG_INVALID;
}

static inline uint32_t payload_size_on_disk(uint32_t payload_size) {
    if (!payload_size) {
        return 0;
    }
    uint8_t pad = (uint8_t) ((payload_size + CRC_SIZE) & (HEADER_ALIGN - 1));
    if (pad != 0) {
        pad = HEADER_ALIGN - pad;
    }
    return payload_size + pad + CRC_SIZE;
}

static int32_t wr_file_header(struct jls_raw_s * self) {
    int32_t rc = 0;
    int64_t pos = jls_bk_ftell(&self->backend);
    jls_bk_fseek(&self->backend, 0L, SEEK_END);
    int64_t file_sz = jls_bk_ftell(&self->backend);
    jls_bk_fseek(&self->backend, 0L, SEEK_SET);

    struct jls_file_header_s hdr = {
            .identification = JLS_HEADER_IDENTIFICATION,
            .length = file_sz,
            .version = {.u32 = JLS_FORMAT_VERSION_U32},
            .crc32 = 0,
    };
    hdr.crc32 = jls_crc32c((uint8_t *) &hdr, sizeof(hdr) - 4);
    RLE(jls_bk_fwrite(&self->backend, &hdr, sizeof(hdr)));
    if (pos != 0) {
        jls_bk_fseek(&self->backend, pos, SEEK_SET);
    } else {
        self->offset = self->backend.fpos;
    }
    return rc;
}

static int32_t rd_file_header(struct jls_raw_s * self, struct jls_file_header_s * hdr) {
    if (jls_bk_fread(&self->backend, hdr, sizeof(*hdr))) {
        JLS_LOGE("could not read file header");
        return JLS_ERROR_UNSUPPORTED_FILE;
    }
    uint32_t crc32 = jls_crc32c((uint8_t *) hdr, sizeof(*hdr) - 4);
    if (crc32 != hdr->crc32) {
        JLS_LOGE("file header crc mismatch: 0x%08x != 0x%08x", crc32, hdr->crc32);
        return JLS_ERROR_UNSUPPORTED_FILE;
    }

    if (0 != memcmp(FILE_HDR, hdr->identification, sizeof(FILE_HDR))) {
        JLS_LOGE("invalid file header identification");
        return JLS_ERROR_UNSUPPORTED_FILE;
    }

    if (hdr->version.s.major > JLS_FORMAT_VERSION_MAJOR) {
        JLS_LOGE("unsupported file format: %d > %d", (int) hdr->version.s.major, JLS_FORMAT_VERSION_MAJOR);
        return JLS_ERROR_UNSUPPORTED_FILE;
    } else if (hdr->version.s.major < JLS_FORMAT_VERSION_MAJOR) {
        JLS_LOGI("old file format: %d < %d", (int) hdr->version.s.major, JLS_FORMAT_VERSION_MAJOR);
    }
    self->version.u32 = hdr->version.u32;
    return 0;
}

static void fend_get(struct jls_raw_s * self) {
    int64_t pos = jls_bk_ftell(&self->backend);
    if (jls_bk_fseek(&self->backend, 0, SEEK_END)) {
        JLS_LOGE("seek to end failed");
    } else {
        self->backend.fend = self->backend.fpos;
        jls_bk_fseek(&self->backend, pos, SEEK_SET);
    }
}

static int32_t read_verify(struct jls_raw_s * self) {
    struct jls_file_header_s file_hdr;
    if (!self->backend.fd) {
        return JLS_ERROR_IO;
    }
    int32_t rc = rd_file_header(self, &file_hdr);
    self->offset = self->backend.fpos;
    if (!rc) {
        fend_get(self);
    }
    if (0 == file_hdr.length) {
        JLS_LOGW("file header length 0, not closed gracefully");
        rc = JLS_ERROR_TRUNCATED;
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
    self->backend.fd = -1;
    ROE(jls_bk_fopen(&self->backend, path, mode));

    switch (mode[0]) {
        case 'w':
            self->write_en = 1;
            rc = wr_file_header(self);
            self->offset = self->backend.fpos;
            self->version.u32 = JLS_FORMAT_VERSION_U32;
            break;
        case 'r':
            rc = read_verify(self);
            break;
        case 'a':
            rc = jls_bk_fseek(&self->backend, 0, SEEK_SET);
            if (0 == rc) {
                self->write_en = 1;
                rc = read_verify(self);
            }

            if ((rc == 0) || (rc == JLS_ERROR_TRUNCATED)) {
                if (self->version.u32 != JLS_FORMAT_VERSION_U32) {
                    JLS_LOGE("cannot append, different format versions");
                    rc = JLS_ERROR_UNSUPPORTED_FILE;
                }
            }
            break;
        default:
            rc = JLS_ERROR_PARAMETER_INVALID;
    }

    if (rc && (rc != JLS_ERROR_TRUNCATED)) {
        jls_bk_fclose(&self->backend);
        free(self);
    } else {
        *instance = self;
    }
    return rc;
}

int32_t jls_raw_close(struct jls_raw_s * self) {
    if (self) {
        if ((self->backend.fd != -1) && (self->write_en)) {
            wr_file_header(self);
        }
        jls_bk_fclose(&self->backend);
        free(self);
    }
    return 0;
}

union jls_version_u jls_raw_version(struct jls_raw_s * self) {
    return self->version;
}

struct jls_bkf_s * jls_raw_backend(struct jls_raw_s * self) {
    if (self->backend.fd == -1) {
        return NULL;
    }
    return &self->backend;
}

int32_t jls_raw_wr(struct jls_raw_s * self, struct jls_chunk_header_s * hdr, const uint8_t * payload) {
    JLS_LOGD3("wr @ %" PRId64 " : %d %s", jls_raw_chunk_tell(self), (int) hdr->tag, jls_tag_to_name(hdr->tag));
    RLE(jls_raw_wr_header(self, hdr));
    RLE(jls_raw_wr_payload(self, hdr->payload_length, payload));
    invalidate_current_chunk(self);
    self->offset = self->backend.fpos;
    return 0;
}

int32_t jls_raw_wr_header(struct jls_raw_s * self, struct jls_chunk_header_s * hdr) {
    if (self->backend.fpos >= self->backend.fend) {
        hdr->payload_prev_length = self->last_payload_length;
    }
    hdr->crc32 = jls_crc32c_hdr(hdr);
    if (self->offset != self->backend.fpos) {
        invalidate_current_chunk(self);
        RLE(jls_bk_fseek(&self->backend, self->offset, SEEK_SET));
    }
    if (jls_bk_fwrite(&self->backend, hdr, sizeof(*hdr))) {
        return JLS_ERROR_IO;
    }
    self->hdr = *hdr;
    return 0;
}

int32_t jls_raw_wr_payload(struct jls_raw_s * self, uint32_t payload_length, const uint8_t * payload) {
    if (!self) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    struct jls_chunk_header_s * hdr = &self->hdr;
    if (hdr->tag == JLS_TAG_INVALID) {
        RLE(jls_raw_rd_header(self, hdr));
    }
    if (!payload_length) {
        return 0;  // no action necessary
    }
    if (!payload) {
        return JLS_ERROR_PARAMETER_INVALID;
    }

    uint8_t footer[CRC_SIZE + HEADER_ALIGN];
    memset(footer, 0, sizeof(footer));
    uint8_t pad = (uint8_t) ((hdr->payload_length + CRC_SIZE) & (HEADER_ALIGN - 1));
    if (pad != 0) {
        pad = HEADER_ALIGN - pad;
    }
    uint32_t crc32 = jls_crc32c(payload, hdr->payload_length);
    footer[pad + 0] = crc32 & 0xff;
    footer[pad + 1] = (crc32 >> 8) & 0xff;
    footer[pad + 2] = (crc32 >> 16) & 0xff;
    footer[pad + 3] = (crc32 >> 24) & 0xff;

    RLE(jls_bk_fwrite(&self->backend, payload, hdr->payload_length));
    RLE(jls_bk_fwrite(&self->backend, footer, pad + CRC_SIZE));
    if (self->backend.fpos >= self->backend.fend) {
        self->last_payload_length = payload_length;
    }
    return 0;
}

int32_t jls_raw_rd(struct jls_raw_s * self, struct jls_chunk_header_s * hdr, uint32_t payload_length_max, uint8_t * payload) {
    RLE(jls_raw_rd_header(self, hdr));
    JLS_LOGD1("rd %" PRId64 " : %d %s", self->offset, (int) hdr->tag, jls_tag_to_name(hdr->tag));
    RLE(jls_raw_rd_payload(self, payload_length_max, payload));
    return 0;
}

int32_t jls_raw_rd_header(struct jls_raw_s * self, struct jls_chunk_header_s * hdr) {
    struct jls_chunk_header_s * h = &self->hdr;
    if (hdr) {
        hdr->tag = JLS_TAG_INVALID;
    }
    if (self->hdr.tag == JLS_TAG_INVALID) {
        if (self->backend.fpos >= self->backend.fend) {
            JLS_LOGE("fpos %" PRIi64 " >= end %" PRIi64, self->backend.fpos, self->backend.fend);
            invalidate_current_chunk(self);
            return JLS_ERROR_EMPTY;
        }
        if (self->offset != self->backend.fpos) {
            if (jls_bk_fseek(&self->backend, self->offset, SEEK_SET)) {
                JLS_LOGE("seek failed");
                invalidate_current_chunk(self);
                return JLS_ERROR_IO;
            }
        }
        self->offset = self->backend.fpos;
        if (jls_bk_fread(&self->backend, (uint8_t *) h, sizeof(*h))) {
            invalidate_current_chunk(self);
            return JLS_ERROR_EMPTY;
        }
        uint32_t crc32 = jls_crc32c_hdr(h);
        if (crc32 != h->crc32) {
            JLS_LOGW("chunk header fpos=%" PRIi64 " crc error: %u != %u",
                     self->backend.fpos, crc32, h->crc32);
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
    if (0 == hdr->payload_length) {
        invalidate_current_chunk(self);
        self->offset = self->backend.fpos;
        return 0;
    }

    uint32_t rd_size = payload_size_on_disk(hdr->payload_length);

    if (rd_size > payload_length_max) {
        return JLS_ERROR_TOO_BIG;
    }

    int64_t pos = self->offset + sizeof(struct jls_chunk_header_s);
    if (pos != self->backend.fpos) {
        jls_bk_fseek(&self->backend, pos, SEEK_SET);
        self->backend.fpos = pos;
    }

    RLE(jls_bk_fread(&self->backend, (uint8_t *) payload, rd_size));
    crc32_calc = jls_crc32c(payload, hdr->payload_length);
    crc32_file = ((uint32_t)payload[rd_size - 4])
        | (((uint32_t)payload[rd_size - 3]) << 8)
        | (((uint32_t)payload[rd_size - 2]) << 16)
        | (((uint32_t)payload[rd_size - 1]) << 24);
    if (crc32_calc != crc32_file) {
        JLS_LOGE("crc32 mismatch: 0x%08x != 0x%08x", crc32_file, crc32_calc);
        return JLS_ERROR_MESSAGE_INTEGRITY;
    }
    invalidate_current_chunk(self);
    self->offset = self->backend.fpos;
    return 0;
}

int32_t jls_raw_chunk_seek(struct jls_raw_s * self, int64_t offset) {
    invalidate_current_chunk(self);
    if (offset == 0) {
        JLS_LOGW("seek to 0");
        return JLS_ERROR_IO;
    }
    if (jls_bk_fseek(&self->backend, offset, SEEK_SET)) {
        return JLS_ERROR_IO;
    }
    self->offset = self->backend.fpos;
    return 0;
}

int32_t jls_raw_chunk_scan(struct jls_raw_s * self) {
    uint8_t buffer[SCAN_SIZE];
    uint8_t * b;
    invalidate_current_chunk(self);
    int64_t offset = jls_raw_chunk_tell(self);
    RLE(jls_bk_fseek(&self->backend, 0L, SEEK_END));
    int64_t offset_end = jls_bk_ftell(&self->backend);
    if (offset & (HEADER_ALIGN - 1)) {
        // headers are aligned to 8-byte boundaries.
        offset += HEADER_ALIGN - (offset & (HEADER_ALIGN - 1));
    }

    while (offset < offset_end) {
        if (jls_bk_fseek(&self->backend, offset, SEEK_SET)) {
            return JLS_ERROR_IO;
        }
        b = buffer;
        size_t sz = sizeof(buffer);
        if ((offset + (int64_t) sz) > offset_end) {
            sz = offset_end - offset;
        }
        size_t sz_block = sz;
        jls_bk_fread(&self->backend, buffer, (unsigned const) sz);
        while (sz >= sizeof(struct jls_chunk_header_s)) {
            struct jls_chunk_header_s * hdr = (struct jls_chunk_header_s *) b;
            uint32_t crc32 = jls_crc32c_hdr(hdr);
            if (crc32 == hdr->crc32) {
                return jls_raw_chunk_seek(self, offset);
            }
            sz -= HEADER_ALIGN;
            b += HEADER_ALIGN;
            offset += HEADER_ALIGN;
        }
        offset += sz_block - sizeof(struct jls_chunk_header_s) + 8;
    }
    return JLS_ERROR_NOT_FOUND;
}

int32_t jls_raw_seek_end(struct jls_raw_s * self) {
    invalidate_current_chunk(self);
    if (jls_bk_fseek(&self->backend, 0, SEEK_END)) {
        return JLS_ERROR_IO;
    }
    self->offset = self->backend.fpos;
    return 0;
}

int64_t jls_raw_chunk_tell(struct jls_raw_s * self) {
    return self->offset;
}

int32_t jls_raw_flush(struct jls_raw_s * self) {
    return jls_bk_fflush(&self->backend);
}

int32_t jls_raw_chunk_next(struct jls_raw_s * self) {
    RLE(jls_raw_rd_header(self, NULL));  // ensure that we have the header
    invalidate_current_chunk(self);
    int64_t offset = self->offset;
    int64_t pos = offset;
    pos += sizeof(struct jls_chunk_header_s) + payload_size_on_disk(self->hdr.payload_length);
    if (pos > self->backend.fend) {
        return JLS_ERROR_EMPTY;
    }
    if (pos != self->backend.fpos) {
        // sequential access
        if (jls_bk_fseek(&self->backend, pos, SEEK_SET)) {
            return JLS_ERROR_EMPTY;
        }
    }
    self->offset = self->backend.fpos;
    return 0;
}

int32_t jls_raw_chunk_prev(struct jls_raw_s * self) {
    if (self->backend.fpos >= self->backend.fend) {
        invalidate_current_chunk(self);
        return JLS_ERROR_NOT_FOUND;
    }
    RLE(jls_raw_rd_header(self, NULL));  // ensure that we have the header
    invalidate_current_chunk(self);
    int64_t offset = self->offset;
    int64_t pos = offset;
    pos -= sizeof(struct jls_chunk_header_s) + payload_size_on_disk(self->hdr.payload_prev_length);
    if (pos < (int64_t) sizeof(struct jls_file_header_s)) {
        return JLS_ERROR_EMPTY;
    }
    if (pos != self->backend.fpos) {
        // sequential access
        jls_bk_fseek(&self->backend, pos, SEEK_SET);
    }
    self->offset = self->backend.fpos;
    return 0;
}

int32_t jls_raw_item_next(struct jls_raw_s * self) {
    RLE(jls_raw_rd_header(self, NULL));  // ensure that we have the header
    int64_t offset = self->hdr.item_next;
    int64_t pos = offset;
    if ((pos == 0) || (pos > self->backend.fend)) {
        return JLS_ERROR_EMPTY;
    }

    invalidate_current_chunk(self);
    if (jls_bk_fseek(&self->backend, pos, SEEK_SET)) {
        return JLS_ERROR_EMPTY;
    }
    self->offset = self->backend.fpos;
    return 0;
}

int32_t jls_raw_item_prev(struct jls_raw_s * self) {
    if (self->backend.fpos >= self->backend.fend) {
        invalidate_current_chunk(self);
        return JLS_ERROR_NOT_FOUND;
    }
    RLE(jls_raw_rd_header(self, NULL));  // ensure that we have the header
    int64_t pos = self->hdr.item_prev;
    if ((pos == 0) || (pos == self->offset)) {
        return JLS_ERROR_EMPTY;
    }
    invalidate_current_chunk(self);
    RLE(jls_bk_fseek(&self->backend, pos, SEEK_SET));
    self->offset = self->backend.fpos;
    return 0;
}

int64_t jls_raw_chunk_tell_end(struct jls_raw_s * self) {
    int64_t starting_pos = jls_raw_chunk_tell(self);
    int64_t end_pos = self->backend.fend - sizeof(struct jls_chunk_header_s);
    if (end_pos < (int64_t) sizeof(struct jls_file_header_s)) {
        end_pos = 0;
    } else if (jls_raw_chunk_seek(self, end_pos)) {
        JLS_LOGW("seek to end failed");
        end_pos = 0;
    } else if (jls_raw_rd_header(self, NULL)) {
        JLS_LOGW("end chunk not found");
        end_pos = 0;
    } else if (self->hdr.tag != JLS_TAG_END) {
        end_pos = 0;
    }
    if (jls_raw_chunk_seek(self, starting_pos)) {
        JLS_LOGW("seek to starting_pos failed");
        end_pos = 0;
    }
    return end_pos;
}

const char * jls_tag_to_name(uint8_t tag) {
    switch (tag) {
        case JLS_TAG_INVALID:                   return "invalid";
        case JLS_TAG_SOURCE_DEF:                return "source_def";
        case JLS_TAG_SIGNAL_DEF:                return "signal_def";
        case JLS_TAG_TRACK_FSR_DEF:             return "track_fsr_def";
        case JLS_TAG_TRACK_FSR_HEAD:            return "track_fsr_head";
        case JLS_TAG_TRACK_FSR_DATA:            return "track_fsr_data";
        case JLS_TAG_TRACK_FSR_INDEX:           return "track_fsr_index";
        case JLS_TAG_TRACK_FSR_SUMMARY:         return "track_fsr_summary";
        case JLS_TAG_TRACK_VSR_DEF:             return "track_vsr_def";
        case JLS_TAG_TRACK_VSR_HEAD:            return "track_vsr_head";
        case JLS_TAG_TRACK_VSR_DATA:            return "track_vsr_data";
        case JLS_TAG_TRACK_VSR_INDEX:           return "track_vsr_index";
        case JLS_TAG_TRACK_VSR_SUMMARY:         return "track_vsr_summary";
        case JLS_TAG_TRACK_ANNOTATION_DEF:      return "track_annotation_def";
        case JLS_TAG_TRACK_ANNOTATION_HEAD:     return "track_annotation_head";
        case JLS_TAG_TRACK_ANNOTATION_DATA:     return "track_annotation_data";
        case JLS_TAG_TRACK_ANNOTATION_INDEX:    return "track_annotation_index";
        case JLS_TAG_TRACK_ANNOTATION_SUMMARY:  return "track_annotation_summary";
        case JLS_TAG_TRACK_UTC_DEF:             return "track_utc_def";
        case JLS_TAG_TRACK_UTC_HEAD:            return "track_utc_head";
        case JLS_TAG_TRACK_UTC_DATA:            return "track_utc_data";
        case JLS_TAG_TRACK_UTC_INDEX:           return "track_utc_index";
        case JLS_TAG_TRACK_UTC_SUMMARY:         return "track_utc_summary";
        case JLS_TAG_USER_DATA:                 return "user_data";
        case JLS_TAG_END:                       return "end";
        default:                                return "unknown";
    }
}

const char * jls_version_str(void) {
    return JLS_VERSION_STR;
}

uint32_t jls_version_u32(void) {
    return JLS_VERSION_U32;
}
