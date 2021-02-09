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


#define BUFFER_SIZE (1 << 20)


struct chunk_s {
    struct jls_chunk_header_s hdr;
    int64_t offset;
};

struct track_info_s {
    struct chunk_s head;
    struct chunk_s index[JLS_SUMMARY_LEVEL_COUNT];
    struct chunk_s data;
    struct chunk_s summary[JLS_SUMMARY_LEVEL_COUNT];
};

struct signal_info_s {
    struct jls_chunk_header_s def_hdr;
    struct track_info_s tracks;
};

struct jls_wr_s {
    struct jls_raw_s * raw;
    uint8_t buffer[BUFFER_SIZE];
    struct chunk_s source_info[256];
    int16_t source_mra;  // most recently added

    struct signal_info_s signal_info[JLS_SIGNAL_COUNT];
    int16_t signal_mra;  // most recently added
    uint32_t payload_prev_length;
};


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
    wr->source_mra = -1;
    wr->signal_mra = -1;
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

struct buf_s {
    uint8_t * cur;
    uint8_t * start;
    uint8_t * end;
};

int32_t buf_add_str(struct buf_s * buf, const char * cstr) {
    uint8_t * end = buf->end - 2;
    while (buf->cur < end) {
        *buf->cur++ = *cstr++;
        if (!*cstr) {
            *buf->cur++ = 0x1f;
            return 0;
        }
    }
    JLS_LOGE("buffer to small");
    return JLS_ERROR_NOT_ENOUGH_MEMORY;
}

int32_t jls_wr_source_def(struct jls_wr_s * self, const struct jls_source_def_s * source) {
    if (!self || !source) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    if (self->source_info[source->source_id].offset) {
        JLS_LOGE("Duplicate source: %d", (int) source->source_id);
        return JLS_ERROR_PARAMETER_INVALID;
    }

    self->buffer[0] = source->source_id;
    for (int i = 1; i < 64; ++i) {
        self->buffer[i] = 0;  // reserve space for future use.
    }

    struct buf_s buf = {
            .start = self->buffer,
            .cur = self->buffer + 64,
            .end = self->buffer + BUFFER_SIZE,
    };
    ROE(buf_add_str(&buf, source->name));
    ROE(buf_add_str(&buf, source->vendor));
    ROE(buf_add_str(&buf, source->model));
    ROE(buf_add_str(&buf, source->version));
    ROE(buf_add_str(&buf, source->serial_number));

    uint32_t payload_length = buf.end - buf.start;

    struct chunk_s * chunk = &self->source_info[source->source_id];
    chunk->hdr.item_next = 0;  // update later
    chunk->hdr.item_prev = (self->source_mra < 0) ? 0 : self->source_info[self->source_mra].offset;
    chunk->hdr.tag = JLS_TAG_SOURCE_DEF;
    chunk->hdr.rsv0_u8 = 0;
    chunk->hdr.chunk_meta = source->source_id;
    chunk->hdr.payload_length = payload_length;
    chunk->hdr.payload_prev_length = self->payload_prev_length;

    chunk->offset = jls_raw_chunk_tell(self->raw);
    ROE(jls_raw_wr(self->raw, &chunk->hdr, self->buffer));
    self->payload_prev_length = payload_length;
    if (self->source_mra >= 0) {
        int64_t pos = jls_raw_chunk_tell(self->raw);
        chunk = &self->source_info[self->source_mra];
        chunk->hdr.item_next = chunk->offset;
        ROE(jls_raw_chunk_seek(self->raw, chunk->offset));
        ROE(jls_raw_wr_header(self->raw, &chunk->hdr));
        ROE(jls_raw_chunk_seek(self->raw, pos));
    }
    self->source_mra = source->source_id;
    return 0;
}
