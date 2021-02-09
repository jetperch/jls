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
    uint16_t signal_id;
    uint8_t track_type;  // enum jls_track_type_e

    struct chunk_s def;
    struct chunk_s head;
    struct chunk_s index[JLS_SUMMARY_LEVEL_COUNT];
    struct chunk_s data;
    struct chunk_s summary[JLS_SUMMARY_LEVEL_COUNT];
};

struct signal_info_s {
    struct chunk_s def;
    struct track_info_s tracks[4];   // index jls_track_type_e
};

struct buf_s {
    uint8_t * cur;
    uint8_t * start;
    uint8_t * end;
};

struct jls_wr_s {
    struct jls_raw_s * raw;
    uint8_t buffer[BUFFER_SIZE];
    struct buf_s buf;

    struct chunk_s source_info[256];
    struct chunk_s source_mra;  // most recently added

    uint64_t utc_bitmap;
    struct chunk_s utc_mra;

    struct signal_info_s signal_info[JLS_SIGNAL_COUNT];
    struct chunk_s signal_mra;

    struct chunk_s track_def_mra;
    struct chunk_s track_head_mra;

    struct chunk_s user_data_mra;

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

    for (uint16_t signal_id = 0; signal_id < JLS_SIGNAL_COUNT; ++signal_id) {
        for (uint8_t track_type = 0; track_type < 4; ++track_type) {
            struct track_info_s * t = &wr->signal_info[signal_id].tracks[track_type];
            t->signal_id = signal_id;
            t->track_type = track_type;
        }
    }
    struct signal_info_s signal_info[JLS_SIGNAL_COUNT];


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

static void buf_reset(struct jls_wr_s * self) {
    self->buf.start = self->buffer;
    self->buf.cur = self->buffer;
    self->buf.end = self->buffer + BUFFER_SIZE;
}

static int32_t buf_add_zero(struct jls_wr_s * self, uint32_t count) {
    struct buf_s * buf = &self->buf;
    if ((buf->cur + count) > buf->end) {
        JLS_LOGE("buffer to small");
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    for (uint32_t i = 0; i < count; ++i) {
        *buf->cur++ = 0;
    }
    return 0;
}

static int32_t buf_add_str(struct jls_wr_s * self, const char * cstr) {
    // Strings end with {0, 0x1f} = {null, unit separator}
    struct buf_s * buf = &self->buf;
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

static uint32_t buf_size(struct jls_wr_s * self) {
    return (self->buf.cur - self->buf.start);
}

static int32_t buf_wr_u8(struct jls_wr_s * self, uint8_t value) {
    struct buf_s * buf = &self->buf;
    if (buf->cur >= buf->end) {
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    *buf->cur++ = value;
    return 0;
}

static int32_t buf_wr_u16(struct jls_wr_s * self, uint16_t value) {
    struct buf_s * buf = &self->buf;
    if ((buf->cur + 1) >= buf->end) {
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    *buf->cur++ = (uint8_t) (value & 0xff);
    *buf->cur++ = (uint8_t) ((value >> 8) & 0xff);
    return 0;
}

static int32_t buf_wr_u32(struct jls_wr_s * self, uint32_t value) {
    struct buf_s * buf = &self->buf;
    if ((buf->cur + 3) >= buf->end) {
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    *buf->cur++ = (uint8_t) (value & 0xff);
    *buf->cur++ = (uint8_t) ((value >> 8) & 0xff);
    *buf->cur++ = (uint8_t) ((value >> 16) & 0xff);
    *buf->cur++ = (uint8_t) ((value >> 24) & 0xff);
    return 0;
}

static int32_t update_mra(struct jls_wr_s * self, struct chunk_s * mra, struct chunk_s * update) {
    if (mra->offset) {
        int64_t current_pos = jls_raw_chunk_tell(self->raw);
        mra->hdr.item_next = update->offset;
        ROE(jls_raw_chunk_seek(self->raw, mra->offset));
        ROE(jls_raw_wr_header(self->raw, &mra->hdr));
        ROE(jls_raw_chunk_seek(self->raw, current_pos));
    }
    *mra = *update;
    return 0;
}

int32_t jls_wr_source_def(struct jls_wr_s * self, const struct jls_source_def_s * source) {
    if (!self || !source) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    if (self->source_info[source->source_id].offset) {
        JLS_LOGE("Duplicate source: %d", (int) source->source_id);
        return JLS_ERROR_PARAMETER_INVALID;
    }

    // construct payload
    buf_reset(self);
    buf_add_zero(self, 64);  // reserve space for future use.
    ROE(buf_add_str(self, source->name));
    ROE(buf_add_str(self, source->vendor));
    ROE(buf_add_str(self, source->model));
    ROE(buf_add_str(self, source->version));
    ROE(buf_add_str(self, source->serial_number));
    uint32_t payload_length = buf_size(self);

    // construct header
    struct chunk_s * chunk = &self->source_info[source->source_id];
    chunk->hdr.item_next = 0;  // update later
    chunk->hdr.item_prev = self->source_mra.offset;
    chunk->hdr.tag = JLS_TAG_SOURCE_DEF;
    chunk->hdr.rsv0_u8 = 0;
    chunk->hdr.chunk_meta = source->source_id;
    chunk->hdr.payload_length = payload_length;
    chunk->hdr.payload_prev_length = self->payload_prev_length;
    chunk->offset = jls_raw_chunk_tell(self->raw);

    // write
    ROE(jls_raw_wr(self->raw, &chunk->hdr, self->buffer));
    self->payload_prev_length = payload_length;
    return update_mra(self, &self->source_mra, chunk);
}

int32_t jls_wr_utc_def(struct jls_wr_s * self, uint8_t utc_id, const char * name) {
    if (!self | !name) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    if (utc_id >= 64) {
        JLS_LOGE("utc_id too big: %d", (int) utc_id);
        return JLS_ERROR_PARAMETER_INVALID;
    }
    uint64_t utc_bitmap = 1LLU << utc_id;
    if (utc_bitmap & self->utc_bitmap) {
        JLS_LOGE("Duplicate utc: %d", (int) utc_id);
        return JLS_ERROR_ALREADY_EXISTS;
    }
    self->utc_bitmap |= utc_bitmap;

    // construct payload
    buf_reset(self);
    buf_add_zero(self, 64);  // reserve space for future use.
    ROE(buf_add_str(self, name));
    uint32_t payload_length = buf_size(self);

    // construct header
    struct chunk_s chunk;
    chunk.hdr.item_next = 0;  // update later
    chunk.hdr.item_prev = self->utc_mra.offset;
    chunk.hdr.tag = JLS_TAG_UTC_DEF;
    chunk.hdr.rsv0_u8 = 0;
    chunk.hdr.chunk_meta = 0;
    chunk.hdr.payload_length = payload_length;
    chunk.hdr.payload_prev_length = self->payload_prev_length;
    chunk.offset = jls_raw_chunk_tell(self->raw);

    // write
    ROE(jls_raw_wr(self->raw, &chunk.hdr, self->buffer));
    self->payload_prev_length = payload_length;
    return update_mra(self, &self->utc_mra, &chunk);
}

static int32_t track_wr_def(struct jls_wr_s * self, struct track_info_s * track_info) {
    // construct header
    struct chunk_s * chunk = &track_info->def;
    if (chunk->offset) {
        return 0;  // no need to update
    }
    chunk->hdr.item_next = 0;  // update later
    chunk->hdr.item_prev = self->source_mra.offset;
    chunk->hdr.tag = 0x20 | ((track_info->track_type & 0x03) << 3) | JLS_TRACK_CHUNK_DEF;
    chunk->hdr.rsv0_u8 = 0;
    chunk->hdr.chunk_meta = track_info->signal_id;
    chunk->hdr.payload_length = 0;
    chunk->hdr.payload_prev_length = self->payload_prev_length;
    chunk->offset = jls_raw_chunk_tell(self->raw);

    // write
    ROE(jls_raw_wr(self->raw, &chunk->hdr, NULL));
    self->payload_prev_length = 0;
    return update_mra(self, &self->track_def_mra, &chunk);
}

static int32_t track_wr_head(struct jls_wr_s * self, struct track_info_s * track_info) {
    // construct header
    struct chunk_s * chunk = &track_info->def;
    uint64_t offsets[JLS_SUMMARY_LEVEL_COUNT];
    for (unsigned int i = 0; i < JLS_SUMMARY_LEVEL_COUNT; ++i) {
        offsets[i] = track_info->index[i].offset;
    }

    if (!chunk->offset) {
        chunk->hdr.item_next = 0;  // update later
        chunk->hdr.item_prev = self->source_mra.offset;
        chunk->hdr.tag = 0x20 | ((track_info->track_type & 0x03) << 3) | JLS_TRACK_CHUNK_DEF;
        chunk->hdr.rsv0_u8 = 0;
        chunk->hdr.chunk_meta = track_info->signal_id;
        chunk->hdr.payload_length = sizeof(offsets);
        chunk->hdr.payload_prev_length = self->payload_prev_length;
        chunk->offset = jls_raw_chunk_tell(self->raw);
        ROE(jls_raw_wr(self->raw, &chunk->hdr, (uint8_t *) offsets));
    } else {
        int64_t pos = jls_raw_chunk_tell(self);
        ROE(jls_raw_chunk_seek(self->raw, chunk->offset));
        ROE(jls_raw_wr_payload(self, sizeof(offsets), (uint8_t *) offsets));
        self->payload_prev_length = sizeof(offsets);
        return update_mra(self, &self->track_head_mra, chunk);
    }
    return 0;
}

int32_t jls_wr_signal_def(struct jls_wr_s * self, const struct jls_signal_def_s * signal) {
    if (!self || !signal) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    if (signal->signal_id >= JLS_SIGNAL_COUNT) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    if (self->signal_info[signal->signal_id].def.offset) {
        JLS_LOGE("Duplicate signal: %d", (int) signal->signal_id);
        return JLS_ERROR_PARAMETER_INVALID;
    }
    if ((signal->signal_type != JLS_SIGNAL_TYPE_FSR) && (signal->signal_type != JLS_SIGNAL_TYPE_VSR)) {
        JLS_LOGE("Invalid signal type: %d", (int) signal->signal_type);
        return JLS_ERROR_PARAMETER_INVALID;
    }

    // construct payload
    buf_reset(self);
    ROE(buf_wr_u8(self, signal->source_id));
    ROE(buf_wr_u8(self, signal->signal_type));
    ROE(buf_wr_u16(self, 0));  // reserved
    ROE(buf_wr_u32(self, signal->data_type));
    ROE(buf_wr_u32(self, signal->sample_rate));
    ROE(buf_wr_u32(self, signal->samples_per_block));
    ROE(buf_wr_u32(self, signal->summary_downsample));
    ROE(buf_wr_u32(self, signal->utc_id));
    ROE(buf_add_zero(self, 3 + 64));  // reserve space for future use.
    ROE(buf_add_str(self, signal->name));
    ROE(buf_add_str(self, signal->si_units));
    uint32_t payload_length = buf_size(self);

    // construct header
    struct chunk_s * chunk = &self->signal_info[signal->signal_id].def;
    chunk->hdr.item_next = 0;  // update later
    chunk->hdr.item_prev = self->source_mra.offset;
    chunk->hdr.tag = JLS_TAG_SOURCE_DEF;
    chunk->hdr.rsv0_u8 = 0;
    chunk->hdr.chunk_meta = signal->signal_id;
    chunk->hdr.payload_length = payload_length;
    chunk->hdr.payload_prev_length = self->payload_prev_length;
    chunk->offset = jls_raw_chunk_tell(self->raw);

    // write
    ROE(jls_raw_wr(self->raw, &chunk->hdr, self->buffer));
    self->payload_prev_length = payload_length;
    ROE(update_mra(self, &self->signal_mra, chunk));

    if (signal->signal_type == JLS_SIGNAL_TYPE_FSR) {
        ROE(track_wr_def(self, &self->signal_info->tracks[JLS_TRACK_TYPE_FSR]));
        ROE(track_wr_head(self, &self->signal_info->tracks[JLS_TRACK_TYPE_FSR]));
        ROE(track_wr_def(self, &self->signal_info->tracks[JLS_TRACK_TYPE_ANNOTATION]));
        ROE(track_wr_head(self, &self->signal_info->tracks[JLS_TRACK_TYPE_ANNOTATION]));
        ROE(track_wr_def(self, &self->signal_info->tracks[JLS_TRACK_TYPE_UTC]));
        ROE(track_wr_head(self, &self->signal_info->tracks[JLS_TRACK_TYPE_UTC]));
    } else if (signal->signal_type == JLS_SIGNAL_TYPE_VSR) {
        ROE(track_wr_def(self, &self->signal_info->tracks[JLS_TRACK_TYPE_VSR]));
        ROE(track_wr_head(self, &self->signal_info->tracks[JLS_TRACK_TYPE_VSR]));
        ROE(track_wr_def(self, &self->signal_info->tracks[JLS_TRACK_TYPE_ANNOTATION]));
        ROE(track_wr_head(self, &self->signal_info->tracks[JLS_TRACK_TYPE_ANNOTATION]));
    }
    return 0;
}

int32_t jls_wr_user_data(struct jls_wr_s * self, uint16_t chunk_meta, const uint8_t * data, uint32_t data_size) {
    if (!self || !data) {
        return JLS_ERROR_PARAMETER_INVALID;
    }

    // construct header
    struct chunk_s chunk;
    chunk.hdr.item_next = 0;  // update later
    chunk.hdr.item_prev = self->user_data_mra.offset;
    chunk.hdr.tag = JLS_TAG_USER_DATA;
    chunk.hdr.rsv0_u8 = 0;
    chunk.hdr.chunk_meta = chunk_meta;
    chunk.hdr.payload_length = data_size;
    chunk.hdr.payload_prev_length = self->payload_prev_length;
    chunk.offset = jls_raw_chunk_tell(self->raw);

    // write
    ROE(jls_raw_wr(self->raw, &chunk.hdr, data));
    self->payload_prev_length = data_size;
    return update_mra(self, &self->signal_mra, &chunk);
}

// struct jls_track_index_s variable sized, format defined by track
// struct jls_track_data_s variable sized, format defined by track
// struct jls_track_summary_s variable sized, format defined by track
