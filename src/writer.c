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
#include "wr_prv.h"
#include "wf_f32.h"
#include "cdef.h"
#include "jls/ec.h"
#include "jls/log.h"
#include "crc32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>


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
    int64_t head_offsets[JLS_SUMMARY_LEVEL_COUNT];  // offset of first chunk at each summary level

    // holder for most recent to construct doubly linked list
    struct chunk_s index[JLS_SUMMARY_LEVEL_COUNT];
    struct chunk_s data;
    struct chunk_s summary[JLS_SUMMARY_LEVEL_COUNT];
};

struct signal_info_s {
    struct chunk_s chunk_def;
    struct jls_signal_def_s signal_def;
    char name[1024];
    struct track_info_s tracks[4];   // index jls_track_type_e
    struct jls_wf_f32_s * signal_writer;

    uint8_t data_tag;
    uint8_t summary_tag;
    uint8_t index_tag;
    uint8_t track_type;
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

    struct chunk_s source_info[JLS_SOURCE_COUNT];
    struct chunk_s source_mra;  // most recently added

    struct signal_info_s signal_info[JLS_SIGNAL_COUNT];
    struct chunk_s signal_mra;
    struct chunk_s user_data_mra;
    uint32_t payload_prev_length;
};

struct jls_source_def_s SOURCE_0 = {
        .source_id = 0,
        .name = "s", // "global_annotation_source",
        .vendor = "None",
        .model = "None",
        .version = "1.0.0",
        .serial_number = "None"
};

struct jls_signal_def_s SIGNAL_0 = {       // 0 reserved for VSR annotations
        .signal_id = 0,
        .source_id = 0,
        .signal_type = JLS_SIGNAL_TYPE_VSR,
        .data_type = JLS_DATATYPE_F32,
        .sample_rate = 0,
        .samples_per_data = 10,
        .samples_decimate_factor = 10,
        .entries_per_summary = 10,
        .summary_decimate_factor = 10,
        .utc_rate_auto = 0,  // disabled
        .name = "global_annotation_signal",
        .si_units = "",
};

int32_t jls_wr_open(struct jls_wr_s ** instance, const char * path) {
    if (!instance) {
        return JLS_ERROR_PARAMETER_INVALID;
    }

    struct jls_wr_s * self = calloc(1, sizeof(struct jls_wr_s));
    if (!self) {
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }

    for (uint16_t signal_id = 0; signal_id < JLS_SIGNAL_COUNT; ++signal_id) {
        for (uint8_t track_type = 0; track_type < 4; ++track_type) {
            struct track_info_s * t = &self->signal_info[signal_id].tracks[track_type];
            t->signal_id = signal_id;
            t->track_type = track_type;
        }
    }

    int32_t rc = jls_raw_open(&self->raw, path, "w");
    if (rc) {
        free(self);
        return rc;
    }

    ROE(jls_wr_user_data(self, 0, JLS_STORAGE_TYPE_INVALID, NULL, 0));
    ROE(jls_wr_source_def(self, &SOURCE_0));
    ROE(jls_wr_signal_def(self, &SIGNAL_0));

    *instance = self;
    return 0;
}

int32_t jls_wr_close(struct jls_wr_s * self) {
    if (self) {
        for (size_t i = 0; i < JLS_SIGNAL_COUNT; ++i) {
            if (self->signal_info[i].signal_writer) {
                jls_wf_f32_close(self->signal_info[i].signal_writer);
            }
        }
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
            *buf->cur++ = 0;
            *buf->cur++ = 0x1f;
            return 0;
        }
    }
    JLS_LOGE("buffer to small");
    return JLS_ERROR_NOT_ENOUGH_MEMORY;
}

static int32_t buf_add_bin(struct jls_wr_s * self, const uint8_t * data, uint32_t data_size) {
    struct buf_s * buf = &self->buf;
    if ((buf->cur + data_size) > buf->end) {
        JLS_LOGE("buffer to small");
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    memcpy(buf->cur, data, data_size);
    buf->cur += data_size;
    return 0;
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

static int32_t buf_wr_i64(struct jls_wr_s * self, int64_t value) {
    struct buf_s * buf = &self->buf;
    if ((buf->cur + 3) >= buf->end) {
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    *buf->cur++ = (uint8_t) (value & 0xff);
    *buf->cur++ = (uint8_t) ((value >> 8) & 0xff);
    *buf->cur++ = (uint8_t) ((value >> 16) & 0xff);
    *buf->cur++ = (uint8_t) ((value >> 24) & 0xff);
    *buf->cur++ = (uint8_t) ((value >> 24) & 0xff);
    *buf->cur++ = (uint8_t) ((value >> 32) & 0xff);
    *buf->cur++ = (uint8_t) ((value >> 40) & 0xff);
    *buf->cur++ = (uint8_t) ((value >> 48) & 0xff);
    *buf->cur++ = (uint8_t) ((value >> 56) & 0xff);
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
    if (source->source_id >= JLS_SOURCE_COUNT) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    if (self->source_info[source->source_id].offset) {
        JLS_LOGE("Duplicate source: %d", (int) source->source_id);
        return JLS_ERROR_ALREADY_EXISTS;
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

static int32_t track_wr_def(struct jls_wr_s * self, struct track_info_s * track_info) {
    // construct header
    struct chunk_s * chunk = &track_info->def;
    if (chunk->offset) {
        return 0;  // no need to update
    }
    chunk->hdr.item_next = 0;  // update later
    chunk->hdr.item_prev = self->signal_mra.offset;
    chunk->hdr.tag = 0x20 | ((track_info->track_type & 0x03) << 3) | JLS_TRACK_CHUNK_DEF;
    chunk->hdr.rsv0_u8 = 0;
    chunk->hdr.chunk_meta = track_info->signal_id;
    chunk->hdr.payload_length = 0;
    chunk->hdr.payload_prev_length = self->payload_prev_length;
    chunk->offset = jls_raw_chunk_tell(self->raw);

    // write
    ROE(jls_raw_wr(self->raw, &chunk->hdr, NULL));
    self->payload_prev_length = 0;
    return update_mra(self, &self->signal_mra, chunk);
}

static int32_t track_wr_head(struct jls_wr_s * self, struct track_info_s * track_info) {
    // construct header
    struct chunk_s * chunk = &track_info->head;
    if (!chunk->offset) {
        chunk->hdr.item_next = 0;  // update later
        chunk->hdr.item_prev = self->signal_mra.offset;
        chunk->hdr.tag = 0x20 | ((track_info->track_type & 0x03) << 3) | JLS_TRACK_CHUNK_HEAD;
        chunk->hdr.rsv0_u8 = 0;
        chunk->hdr.chunk_meta = track_info->signal_id;
        chunk->hdr.payload_length = sizeof(track_info->head_offsets);
        chunk->hdr.payload_prev_length = self->payload_prev_length;
        chunk->offset = jls_raw_chunk_tell(self->raw);
        ROE(jls_raw_wr(self->raw, &chunk->hdr, (uint8_t *) track_info->head_offsets));
    } else {
        int64_t pos = jls_raw_chunk_tell(self->raw);
        ROE(jls_raw_chunk_seek(self->raw, chunk->offset));
        ROE(jls_raw_wr_payload(self->raw, sizeof(track_info->head_offsets), (uint8_t *) track_info->head_offsets));
        ROE(jls_raw_chunk_seek(self->raw, pos));
        self->payload_prev_length = sizeof(track_info->head_offsets);
        return update_mra(self, &self->signal_mra, chunk);
    }
    return 0;
}

int32_t jls_wr_signal_def(struct jls_wr_s * self, const struct jls_signal_def_s * signal) {
    if (!self || !signal) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    uint16_t signal_id = signal->signal_id;
    if (signal_id >= JLS_SIGNAL_COUNT) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    if (signal->source_id >= JLS_SOURCE_COUNT) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    if (!self->source_info[signal->source_id].offset) {
        JLS_LOGW("source %d not found", signal->source_id);
        return JLS_ERROR_NOT_FOUND;
    }

    struct signal_info_s * info = &self->signal_info[signal_id];
    if (info->chunk_def.offset) {
        JLS_LOGE("Duplicate signal: %d", (int) signal_id);
        return JLS_ERROR_ALREADY_EXISTS;
    }
    if ((signal->signal_type != JLS_SIGNAL_TYPE_FSR) && (signal->signal_type != JLS_SIGNAL_TYPE_VSR)) {
        JLS_LOGE("Invalid signal type: %d", (int) signal->signal_type);
        return JLS_ERROR_PARAMETER_INVALID;
    }

    info->signal_def = *signal;
    snprintf(info->name, sizeof(info->name), "%s", signal->name);
    info->signal_def.name = info->name;

    switch (signal->signal_type) {
        case JLS_SIGNAL_TYPE_FSR:
            if (!signal->sample_rate) {
                JLS_LOGE("FSR requires sample rate");
                return JLS_ERROR_PARAMETER_INVALID;
            }
            info->data_tag = JLS_TAG_TRACK_FSR_DATA;
            info->summary_tag = JLS_TAG_TRACK_FSR_SUMMARY;
            info->index_tag = JLS_TAG_TRACK_FSR_INDEX;
            info->track_type = JLS_TAG_TRACK_FSR_DATA;
            break;
        case JLS_SIGNAL_TYPE_VSR:
            if (signal->sample_rate) {
                JLS_LOGE("VSR but sample rate specified, ignoring");
                info->signal_def.sample_rate = 0;
            }
            info->data_tag = JLS_TAG_TRACK_VSR_DATA;
            info->summary_tag = JLS_TAG_TRACK_VSR_SUMMARY;
            info->index_tag = JLS_TAG_TRACK_VSR_INDEX;
            info->track_type = JLS_TAG_TRACK_VSR_DATA;
            break;
        default:
            JLS_LOGE("Invalid signal type: %d", (int) signal->signal_type);
            return JLS_ERROR_PARAMETER_INVALID;
    }

    if (signal->data_type != JLS_DATATYPE_F32) {
        JLS_LOGW("Only f32 datatype currently supported");
        // todo: support other data types.
        return JLS_ERROR_NOT_SUPPORTED;
    }

    // construct payload
    buf_reset(self);
    ROE(buf_wr_u16(self, signal->source_id));
    ROE(buf_wr_u8(self, signal->signal_type));
    ROE(buf_wr_u8(self, 0));  // reserved
    ROE(buf_wr_u32(self, signal->data_type));
    ROE(buf_wr_u32(self, info->signal_def.sample_rate));
    ROE(buf_wr_u32(self, info->signal_def.samples_per_data));
    ROE(buf_wr_u32(self, info->signal_def.samples_decimate_factor));
    ROE(buf_wr_u32(self, info->signal_def.entries_per_summary));
    ROE(buf_wr_u32(self, info->signal_def.summary_decimate_factor));
    ROE(buf_wr_u32(self, signal->utc_rate_auto));
    ROE(buf_add_zero(self, 64));  // reserve space for future use.
    ROE(buf_add_str(self, signal->name));
    ROE(buf_add_str(self, signal->si_units));
    uint32_t payload_length = buf_size(self);

    // construct header
    struct chunk_s * chunk = &info->chunk_def;
    chunk->hdr.item_next = 0;  // update later
    chunk->hdr.item_prev = self->signal_mra.offset;
    chunk->hdr.tag = JLS_TAG_SIGNAL_DEF;
    chunk->hdr.rsv0_u8 = 0;
    chunk->hdr.chunk_meta = signal_id;
    chunk->hdr.payload_length = payload_length;
    chunk->hdr.payload_prev_length = self->payload_prev_length;
    chunk->offset = jls_raw_chunk_tell(self->raw);

    // write
    ROE(jls_raw_wr(self->raw, &chunk->hdr, self->buffer));
    self->payload_prev_length = payload_length;
    ROE(update_mra(self, &self->signal_mra, chunk));

    if (signal->signal_type == JLS_SIGNAL_TYPE_FSR) {
        ROE(track_wr_def(self, &info->tracks[JLS_TRACK_TYPE_FSR]));
        ROE(track_wr_head(self, &info->tracks[JLS_TRACK_TYPE_FSR]));
        ROE(track_wr_def(self, &info->tracks[JLS_TRACK_TYPE_ANNOTATION]));
        ROE(track_wr_head(self, &info->tracks[JLS_TRACK_TYPE_ANNOTATION]));
        ROE(track_wr_def(self, &info->tracks[JLS_TRACK_TYPE_UTC]));
        ROE(track_wr_head(self, &info->tracks[JLS_TRACK_TYPE_UTC]));
    } else if (signal->signal_type == JLS_SIGNAL_TYPE_VSR) {
        ROE(track_wr_def(self, &info->tracks[JLS_TRACK_TYPE_VSR]));
        ROE(track_wr_head(self, &info->tracks[JLS_TRACK_TYPE_VSR]));
        ROE(track_wr_def(self, &info->tracks[JLS_TRACK_TYPE_ANNOTATION]));
        ROE(track_wr_head(self, &info->tracks[JLS_TRACK_TYPE_ANNOTATION]));
    }

    // Open the signal
    struct jls_wf_f32_def_s def = {
            .signal_id = signal->signal_id,
            .samples_per_data = signal->samples_per_data,
            .sample_decimate_factor = signal->samples_decimate_factor,
            .entries_per_summary = signal->entries_per_summary,
            .summary_decimate_factor = signal->summary_decimate_factor,
    };
    ROE(jls_wf_f32_open(&info->signal_writer, self, &def));

    return 0;
}

int32_t jls_wr_user_data(struct jls_wr_s * self, uint16_t chunk_meta,
                         enum jls_storage_type_e storage_type, const uint8_t * data, uint32_t data_size) {
    if (!self) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    if (data_size & !data) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    if (chunk_meta & 0xf000) {
        JLS_LOGW("chunk_meta[15:12] nonzero.  Will be modified.");
        chunk_meta &= 0x0fff;
    }

    switch (storage_type) {
        case JLS_STORAGE_TYPE_INVALID:
            data_size = 0; // allowed, but should only be used for the initial chunk.
            break;
        case JLS_STORAGE_TYPE_BINARY:
            break;
        case JLS_STORAGE_TYPE_STRING:  // intentional fall-through
        case JLS_STORAGE_TYPE_JSON:
            data_size = strlen((const char *) data) + 1;
            break;
        default:
            return JLS_ERROR_PARAMETER_INVALID;
    }
    chunk_meta |= ((uint16_t) storage_type) << 12;

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
    return update_mra(self, &self->user_data_mra, &chunk);
}

// struct jls_track_index_s variable sized, format defined by track
// struct jls_track_data_s variable sized, format defined by track
// struct jls_track_summary_s variable sized, format defined by track

static int32_t signal_validate(struct jls_wr_s * self, uint16_t signal_id) {
    if (signal_id >= JLS_SIGNAL_COUNT) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    struct signal_info_s * signal_info = &self->signal_info[signal_id];
    if (!signal_info->chunk_def.offset) {
        JLS_LOGW("attempted to annotated an undefined signal %d", (int) signal_id);
        return JLS_ERROR_NOT_FOUND;
    }
    return 0;
}

static int32_t signal_validate_typed(struct jls_wr_s * self, uint16_t signal_id, enum jls_signal_type_e signal_type) {
    ROE(signal_validate(self, signal_id));
    struct signal_info_s * signal_info = &self->signal_info[signal_id];
    if (signal_info->signal_def.signal_type != signal_type) {
        return JLS_ERROR_NOT_SUPPORTED;
    }
    return 0;
}

int64_t jls_wr_tell_prv(struct jls_wr_s * self) {
    return jls_raw_chunk_tell(self->raw);
}

int32_t jls_wr_data_prv(struct jls_wr_s * self, uint16_t signal_id, uint8_t level,
                        const uint8_t * payload, uint32_t payload_length) {
    ROE(signal_validate(self, signal_id));
    struct signal_info_s * info = &self->signal_info[signal_id];
    struct track_info_s * track = &info->tracks[info->track_type];
    struct chunk_s chunk;

    if (!level) {  // data chunk
        chunk.hdr.item_next = 0;  // update later
        chunk.hdr.item_prev = track->data.offset;
        chunk.hdr.tag = info->data_tag;
        chunk.hdr.rsv0_u8 = 0;
        chunk.hdr.chunk_meta = signal_id | (0 << 12);
        chunk.hdr.payload_length = payload_length;
        chunk.hdr.payload_prev_length = self->payload_prev_length;
        chunk.offset = jls_raw_chunk_tell(self->raw);

        // write
        ROE(jls_raw_wr(self->raw, &chunk.hdr, payload));
        self->payload_prev_length = chunk.hdr.payload_length;
        ROE(update_mra(self, &track->data, &chunk));
    } else {  // summary chunk
        chunk.hdr.item_next = 0;  // update later
        chunk.hdr.item_prev = track->summary[level].offset;
        chunk.hdr.tag = info->summary_tag;
        chunk.hdr.rsv0_u8 = 0;
        chunk.hdr.chunk_meta = signal_id | (((uint16_t) level) << 12);
        chunk.hdr.payload_length = payload_length;
        chunk.hdr.payload_prev_length = self->payload_prev_length;
        chunk.offset = jls_raw_chunk_tell(self->raw);

        // write
        ROE(jls_raw_wr(self->raw, &chunk.hdr, payload));
        self->payload_prev_length = chunk.hdr.payload_length;
        ROE(update_mra(self, &track->summary[level], &chunk));
    }
    return 0;
}

int32_t jls_wr_index_prv(struct jls_wr_s * self, uint16_t signal_id, uint8_t level,
                         const uint8_t * payload, uint32_t payload_length) {
    ROE(signal_validate(self, signal_id));
    struct signal_info_s * info = &self->signal_info[signal_id];
    struct track_info_s * track = &info->tracks[info->track_type];
    struct chunk_s chunk;

    chunk.hdr.item_next = 0;  // update later
    chunk.hdr.item_prev = track->index[level].offset;
    chunk.hdr.tag = info->index_tag;
    chunk.hdr.rsv0_u8 = 0;
    chunk.hdr.chunk_meta = signal_id | (((uint16_t) level) << 12);;
    chunk.hdr.payload_length = payload_length;
    chunk.hdr.payload_prev_length = self->payload_prev_length;
    chunk.offset = jls_raw_chunk_tell(self->raw);

    // write
    ROE(jls_raw_wr(self->raw, &chunk.hdr, payload));
    self->payload_prev_length = chunk.hdr.payload_length;
    ROE(update_mra(self, &track->index[level], &chunk));
    return 0;
}

int32_t jls_wr_fsr_f32(struct jls_wr_s * self, uint16_t signal_id,
                       int64_t sample_id, const float * data, uint32_t data_length) {
    ROE(signal_validate(self, signal_id));
    struct signal_info_s * info = &self->signal_info[signal_id];
    return jls_wf_f32_data(info->signal_writer, sample_id, data, data_length);
}

int32_t jls_wr_annotation(struct jls_wr_s * self, uint16_t signal_id, int64_t timestamp,
                          enum jls_annotation_type_e annotation_type,
                          enum jls_storage_type_e storage_type, const uint8_t * data, uint32_t data_size) {
    ROE(signal_validate(self, signal_id));
    struct signal_info_s * signal_info = &self->signal_info[signal_id];
    if ((annotation_type & 0xff) != annotation_type) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    if ((storage_type & 0xff) != storage_type) {
        return JLS_ERROR_PARAMETER_INVALID;
    }

    // construct payload
    buf_reset(self);
    ROE(buf_wr_i64(self, timestamp));
    ROE(buf_wr_u8(self, annotation_type));
    ROE(buf_wr_u8(self, storage_type));
    ROE(buf_add_zero(self, 6));
    switch (storage_type) {
        case JLS_STORAGE_TYPE_BINARY:
            ROE(buf_add_bin(self, data, data_size));
            break;
        case JLS_STORAGE_TYPE_STRING:
            ROE(buf_add_str(self, (const char *) data));
            break;
        case JLS_STORAGE_TYPE_JSON:
            ROE(buf_add_str(self, (const char *) data));
            break;
        default:
            return JLS_ERROR_PARAMETER_INVALID;
    }
    uint32_t payload_length = buf_size(self);

    // construct header
    struct chunk_s chunk;
    chunk.hdr.item_next = 0;  // update later
    chunk.hdr.item_prev = signal_info->tracks[JLS_TRACK_TYPE_ANNOTATION].data.offset;
    chunk.hdr.tag = JLS_TAG_TRACK_ANNOTATION_DATA;
    chunk.hdr.rsv0_u8 = 0;
    chunk.hdr.chunk_meta = signal_id;
    chunk.hdr.payload_length = payload_length;
    chunk.hdr.payload_prev_length = self->payload_prev_length;
    chunk.offset = jls_raw_chunk_tell(self->raw);

    // write
    ROE(jls_raw_wr(self->raw, &chunk.hdr, data));
    self->payload_prev_length = payload_length;
    return update_mra(self, &signal_info->tracks[JLS_TRACK_TYPE_ANNOTATION].data, &chunk);
}

int32_t jls_wr_fsr_utc(struct jls_wr_s * self, uint16_t signal_id, int64_t sample_id, int64_t utc) {
    ROE(signal_validate_typed(self, signal_id,  JLS_SIGNAL_TYPE_FSR));
    struct signal_info_s * signal_info = &self->signal_info[signal_id];

    // future feature: buffer multiple samples into same record?
    // todo implement utc_rate_auto

    // Construct payload
    int64_t payload[2] = {sample_id, utc};
    uint32_t payload_length = sizeof(payload);

    // construct header
    struct chunk_s chunk;
    chunk.hdr.item_next = 0;  // update later
    chunk.hdr.item_prev = signal_info->tracks[JLS_TRACK_TYPE_UTC].data.offset;
    chunk.hdr.tag = JLS_TAG_TRACK_UTC_DATA;
    chunk.hdr.rsv0_u8 = 0;
    chunk.hdr.chunk_meta = signal_id;
    chunk.hdr.payload_length = payload_length;
    chunk.hdr.payload_prev_length = self->payload_prev_length;
    chunk.offset = jls_raw_chunk_tell(self->raw);

    // write
    ROE(jls_raw_wr(self->raw, &chunk.hdr, (uint8_t *) payload));
    self->payload_prev_length = payload_length;
    return update_mra(self, &signal_info->tracks[JLS_TRACK_TYPE_UTC].data, &chunk);
}
