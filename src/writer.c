/*
 * Copyright 2021-2023 Jetperch LLC
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
#include "jls/buffer.h"
#include "jls/core.h"
#include "jls/track.h"
#include "jls/wr_fsr.h"
#include "jls/wr_ts.h"
#include "jls/cdef.h"
#include "jls/ec.h"
#include "jls/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

struct jls_wr_s {
    struct jls_core_s core;
};

const struct jls_source_def_s SOURCE_0 = {
        .source_id = 0,
        .name = "global_annotation_source",
        .vendor = "jls",
        .model = "-",
        .version = "1.0.0",
        .serial_number = "-"
};

const struct jls_signal_def_s SIGNAL_0 = {       // 0 reserved for VSR annotations
        .signal_id = 0,
        .source_id = 0,
        .signal_type = JLS_SIGNAL_TYPE_VSR,
        .data_type = JLS_DATATYPE_F32,
        .sample_rate = 0,
        .samples_per_data = 10,
        .sample_decimate_factor = 10,
        .entries_per_summary = 10,
        .summary_decimate_factor = 10,
        .annotation_decimate_factor = 100,
        .utc_decimate_factor = 100,
        .name = "global_annotation_signal",
        .units = "",
};

int32_t jls_wr_open(struct jls_wr_s ** instance, const char * path) {
    if (!instance) {
        return JLS_ERROR_PARAMETER_INVALID;
    }

    struct jls_wr_s * self = calloc(1, sizeof(struct jls_wr_s));
    if (!self) {
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    struct jls_core_s * core = &self->core;

    core->buf = jls_buf_alloc();
    if (!core->buf) {
        free(self);
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }

    for (uint16_t signal_id = 0; signal_id < JLS_SIGNAL_COUNT; ++signal_id) {
        struct jls_core_signal_s * signal = &core->signal_info[signal_id];
        signal->parent = &self->core;
        for (uint8_t track_type = 0; track_type < 4; ++track_type) {
            struct jls_core_track_s * t = &signal->tracks[track_type];
            t->parent = signal;
            t->track_type = track_type;
        }
    }

    int32_t rc = jls_raw_open(&core->raw, path, "w");
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
        struct jls_core_s * core = &self->core;
        for (size_t i = 0; i < JLS_SIGNAL_COUNT; ++i) {
            struct jls_core_signal_s * signal_info = &core->signal_info[i];
            jls_fsr_close(signal_info->track_fsr);
            jls_wr_ts_close(signal_info->track_anno);
            jls_wr_ts_close(signal_info->track_utc);
        }
        jls_core_wr_end(core);
        int32_t rc = jls_raw_close(core->raw);
        if (core->buf) {
            jls_buf_free(core->buf);
            core->buf = NULL;
        }
        free(self);
        return rc;
    }
    return 0;
}

int32_t jls_wr_flush(struct jls_wr_s * self) {
    return jls_raw_flush(self->core.raw);
}

static int32_t buf_wr_str(struct jls_buf_s * self, const char * src, char ** dst) {
    if (NULL == src) {
        if (NULL != dst) {
            *dst = NULL;
        }
    } else {
        jls_buf_string_save(self, src, dst);
    }
    return jls_buf_wr_str(self, src);
}

int32_t jls_wr_source_def(struct jls_wr_s * self, const struct jls_source_def_s * source) {
    if (!self || !source) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    struct jls_core_s * core = &self->core;
    struct jls_buf_s * buf = self->core.buf;
    if (source->source_id >= JLS_SOURCE_COUNT) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    struct jls_core_chunk_s * chunk = &core->source_info[source->source_id].chunk_def;
    if (chunk->offset) {
        JLS_LOGE("Duplicate source: %d", (int) source->source_id);
        return JLS_ERROR_ALREADY_EXISTS;
    }
    struct jls_source_def_s * sdef = &core->source_info[source->source_id].source_def;
    *sdef = *source;

    // construct payload
    jls_buf_reset(buf);
    jls_buf_wr_zero(buf, 64);  // reserve space for future use.
    ROE(buf_wr_str(buf, source->name, (char **) &sdef->name));
    ROE(buf_wr_str(buf, source->vendor, (char **) &sdef->vendor));
    ROE(buf_wr_str(buf, source->model, (char **) &sdef->model));
    ROE(buf_wr_str(buf, source->version, (char **) &sdef->version));
    ROE(buf_wr_str(buf, source->serial_number, (char **) &sdef->serial_number));
    uint32_t payload_length = (uint32_t) jls_buf_length(buf);

    // construct header
    chunk->hdr.item_next = 0;  // update later
    chunk->hdr.item_prev = core->source_head.offset;
    chunk->hdr.tag = JLS_TAG_SOURCE_DEF;
    chunk->hdr.rsv0_u8 = 0;
    chunk->hdr.chunk_meta = source->source_id;
    chunk->hdr.payload_length = payload_length;
    chunk->offset = jls_raw_chunk_tell(core->raw);

    // write
    ROE(jls_core_update_item_head(core, &core->source_head, chunk));
    ROE(jls_raw_wr(core->raw, &chunk->hdr, buf->start));
    return 0;
}

int32_t jls_wr_signal_def(struct jls_wr_s * self, const struct jls_signal_def_s * signal) {
    if (!self || !signal) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    struct jls_core_s * core = &self->core;
    struct jls_buf_s * buf = self->core.buf;
    uint16_t signal_id = signal->signal_id;
    if (signal_id >= JLS_SIGNAL_COUNT) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    if (signal->source_id >= JLS_SOURCE_COUNT) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    if (!core->source_info[signal->source_id].chunk_def.offset) {
        JLS_LOGW("source %d not found", signal->source_id);
        return JLS_ERROR_NOT_FOUND;
    }

    struct jls_core_signal_s * info = &core->signal_info[signal_id];
    if (info->chunk_def.offset) {
        JLS_LOGE("Duplicate signal: %d", (int) signal_id);
        return JLS_ERROR_ALREADY_EXISTS;
    }
    if ((signal->signal_type != JLS_SIGNAL_TYPE_FSR) && (signal->signal_type != JLS_SIGNAL_TYPE_VSR)) {
        JLS_LOGE("Invalid signal type: %d", (int) signal->signal_type);
        return JLS_ERROR_PARAMETER_INVALID;
    }

    // copy signal def
    info->signal_def = *signal;
    struct jls_signal_def_s * def = &info->signal_def;
    jls_buf_string_save(buf, signal->name, (char **) &def->name);
    jls_buf_string_save(buf, signal->units, (char **) &def->units);
    ROE(jls_core_signal_def_validate(def));
    ROE(jls_core_signal_def_align(def));

    switch (def->signal_type) {
        case JLS_SIGNAL_TYPE_FSR:
            if (!def->sample_rate) {
                JLS_LOGE("FSR requires sample rate");
                return JLS_ERROR_PARAMETER_INVALID;
            }
            break;
        case JLS_SIGNAL_TYPE_VSR:
            if (def->sample_rate) {
                JLS_LOGE("VSR but sample rate specified, ignoring");
                def->sample_rate = 0;
            }
            break;
        default:
            JLS_LOGE("Invalid signal type: %d", (int) def->signal_type);
            return JLS_ERROR_PARAMETER_INVALID;
    }

    // construct payload
    jls_buf_reset(buf);
    ROE(jls_buf_wr_u16(buf, def->source_id));
    ROE(jls_buf_wr_u8(buf, def->signal_type));
    ROE(jls_buf_wr_u8(buf, 0));  // reserved
    ROE(jls_buf_wr_u32(buf, def->data_type));
    ROE(jls_buf_wr_u32(buf, def->sample_rate));
    ROE(jls_buf_wr_u32(buf, def->samples_per_data));
    ROE(jls_buf_wr_u32(buf, def->sample_decimate_factor));
    ROE(jls_buf_wr_u32(buf, def->entries_per_summary));
    ROE(jls_buf_wr_u32(buf, def->summary_decimate_factor));
    ROE(jls_buf_wr_u32(buf, def->annotation_decimate_factor));
    ROE(jls_buf_wr_u32(buf, def->utc_decimate_factor));
    ROE(jls_buf_wr_zero(buf, 92));  // reserve space for future use.
    ROE(jls_buf_wr_str(buf, def->name));
    ROE(jls_buf_wr_str(buf, def->units));
    uint32_t payload_length = (uint32_t) jls_buf_length(buf);

    // construct header
    struct jls_core_chunk_s * chunk = &info->chunk_def;
    chunk->hdr.item_next = 0;  // update later
    chunk->hdr.item_prev = core->signal_head.offset;
    chunk->hdr.tag = JLS_TAG_SIGNAL_DEF;
    chunk->hdr.rsv0_u8 = 0;
    chunk->hdr.chunk_meta = signal_id;
    chunk->hdr.payload_length = payload_length;
    chunk->offset = jls_raw_chunk_tell(core->raw);

    // write
    ROE(jls_raw_wr(core->raw, &chunk->hdr, buf->start));
    ROE(jls_core_update_item_head(core, &core->signal_head, chunk));

    if (def->signal_type == JLS_SIGNAL_TYPE_FSR) {
        ROE(jls_track_wr_def(&info->tracks[JLS_TRACK_TYPE_FSR]));
        ROE(jls_track_wr_head(&info->tracks[JLS_TRACK_TYPE_FSR]));
        ROE(jls_track_wr_def(&info->tracks[JLS_TRACK_TYPE_ANNOTATION]));
        ROE(jls_track_wr_head(&info->tracks[JLS_TRACK_TYPE_ANNOTATION]));
        ROE(jls_track_wr_def(&info->tracks[JLS_TRACK_TYPE_UTC]));
        ROE(jls_track_wr_head(&info->tracks[JLS_TRACK_TYPE_UTC]));
        ROE(jls_fsr_open(&info->track_fsr, info));
        ROE(jls_wr_ts_open(&info->track_anno, info, JLS_TRACK_TYPE_ANNOTATION,
                           info->signal_def.annotation_decimate_factor));
        ROE(jls_wr_ts_open(&info->track_utc, info, JLS_TRACK_TYPE_UTC,
                           info->signal_def.utc_decimate_factor));
    } else if (def->signal_type == JLS_SIGNAL_TYPE_VSR) {
        ROE(jls_track_wr_def(&info->tracks[JLS_TRACK_TYPE_VSR]));
        ROE(jls_track_wr_head(&info->tracks[JLS_TRACK_TYPE_VSR]));
        ROE(jls_track_wr_def(&info->tracks[JLS_TRACK_TYPE_ANNOTATION]));
        ROE(jls_track_wr_head(&info->tracks[JLS_TRACK_TYPE_ANNOTATION]));
        ROE(jls_wr_ts_open(&info->track_anno, info, JLS_TRACK_TYPE_ANNOTATION,
                           info->signal_def.annotation_decimate_factor));
    }

    return 0;
}

int32_t jls_wr_user_data(struct jls_wr_s * self, uint16_t chunk_meta,
                         enum jls_storage_type_e storage_type, const uint8_t * data, uint32_t data_size) {
    if (!self) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    if (data_size && !data) {
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
            data_size = (uint32_t) strlen((const char *) data) + 1;
            break;
        default:
            return JLS_ERROR_PARAMETER_INVALID;
    }
    chunk_meta |= ((uint16_t) storage_type) << 12;

    // construct header
    struct jls_core_chunk_s chunk;
    chunk.hdr.item_next = 0;  // update later
    chunk.hdr.item_prev = self->core.user_data_head.offset;
    chunk.hdr.tag = JLS_TAG_USER_DATA;
    chunk.hdr.rsv0_u8 = 0;
    chunk.hdr.chunk_meta = chunk_meta;
    chunk.hdr.payload_length = data_size;
    chunk.offset = jls_raw_chunk_tell(self->core.raw);

    // write
    ROE(jls_raw_wr(self->core.raw, &chunk.hdr, data));
    return jls_core_update_item_head(&self->core, &self->core.user_data_head, &chunk);
}

int32_t jls_wr_fsr(struct jls_wr_s * self, uint16_t signal_id,
                           int64_t sample_id, const void * data, uint32_t data_length) {
    ROE(jls_core_signal_validate(&self->core, signal_id));
    struct jls_core_signal_s * info = &self->core.signal_info[signal_id];
    return jls_wr_fsr_data(info->track_fsr, sample_id, data, data_length);
}

int32_t jls_wr_fsr_f32(struct jls_wr_s * self, uint16_t signal_id,
                       int64_t sample_id, const float * data, uint32_t data_length) {
    ROE(jls_core_signal_validate(&self->core, signal_id));
    struct jls_core_signal_s * info = &self->core.signal_info[signal_id];
    if (info->signal_def.data_type != JLS_DATATYPE_F32) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    return jls_wr_fsr_data(info->track_fsr, sample_id, data, data_length);
}

int32_t jls_wr_fsr_omit_data(struct jls_wr_s * self, uint16_t signal_id, uint32_t enable) {
    ROE(jls_core_signal_validate(&self->core, signal_id));
    struct jls_core_signal_s * info = &self->core.signal_info[signal_id];
    if (enable) {
        info->track_fsr->write_omit_data |= 1;
    } else {
        info->track_fsr->write_omit_data = 0;
    }
    return 0;
}

int32_t jls_wr_annotation(struct jls_wr_s * self, uint16_t signal_id, int64_t timestamp,
                          float y,
                          enum jls_annotation_type_e annotation_type,
                          uint8_t group_id,
                          enum jls_storage_type_e storage_type,
                          const uint8_t * data, uint32_t data_size) {
    ROE(jls_core_signal_validate(&self->core, signal_id));
    struct jls_core_s * core = &self->core;
    struct jls_buf_s * buf = self->core.buf;
    struct jls_core_signal_s * signal_info = &core->signal_info[signal_id];
    struct jls_core_track_s * track = &signal_info->tracks[JLS_TRACK_TYPE_ANNOTATION];
    if ((annotation_type & 0xff) != annotation_type) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    if ((storage_type & 0xff) != storage_type) {
        return JLS_ERROR_PARAMETER_INVALID;
    }

    // construct payload
    jls_buf_reset(buf);
    ROE(jls_buf_wr_i64(buf, timestamp));
    ROE(jls_buf_wr_u32(buf, 1));  // number of entries
    ROE(jls_buf_wr_u16(buf, 0));  // unspecified entry length
    ROE(jls_buf_wr_u16(buf, 0));  // reserved
    ROE(jls_buf_wr_u8(buf, annotation_type));
    ROE(jls_buf_wr_u8(buf, storage_type));
    ROE(jls_buf_wr_u8(buf, group_id));
    ROE(jls_buf_wr_u8(buf, 0));    // reserved
    ROE(jls_buf_wr_f32(buf, y));
    switch (storage_type) {
        case JLS_STORAGE_TYPE_BINARY:
            ROE(jls_buf_wr_u32(buf, data_size));
            ROE(jls_buf_wr_bin(buf, data, data_size));
            break;
        case JLS_STORAGE_TYPE_STRING:
            ROE(jls_buf_wr_u32(buf, (uint32_t) (strlen((const char *) data) + 1)));
            ROE(jls_buf_wr_str(buf, (const char *) data));
            break;
        case JLS_STORAGE_TYPE_JSON:
            ROE(jls_buf_wr_u32(buf, (uint32_t) (strlen((const char *) data) + 1)));
            ROE(jls_buf_wr_str(buf, (const char *) data));
            break;
        default:
            return JLS_ERROR_PARAMETER_INVALID;
    }
    uint32_t payload_length = (uint32_t) jls_buf_length(buf);

    // construct header
    struct jls_core_chunk_s chunk;
    uint64_t offset = jls_raw_chunk_tell(core->raw);
    chunk.hdr.item_next = 0;  // update later
    chunk.hdr.item_prev = signal_info->tracks[JLS_TRACK_TYPE_ANNOTATION].data_head.offset;
    chunk.hdr.tag = JLS_TAG_TRACK_ANNOTATION_DATA;
    chunk.hdr.rsv0_u8 = 0;
    chunk.hdr.chunk_meta = signal_id;
    chunk.hdr.payload_length = payload_length;
    chunk.offset = offset;

    // write
    ROE(jls_raw_wr(core->raw, &chunk.hdr, buf->start));
    ROE(jls_core_update_item_head(core, &signal_info->tracks[JLS_TRACK_TYPE_ANNOTATION].data_head, &chunk));
    ROE(jls_track_update(track, 0, offset));
    ROE(jls_wr_ts_anno(signal_info->track_anno, timestamp, offset, annotation_type, group_id, y));
    return 0;
}

int32_t jls_wr_utc(struct jls_wr_s * self, uint16_t signal_id, int64_t sample_id, int64_t utc) {
    ROE(jls_core_signal_validate_typed(&self->core, signal_id, JLS_SIGNAL_TYPE_FSR));
    struct jls_core_signal_s * signal_info = &self->core.signal_info[signal_id];
    struct jls_core_track_s * track = &signal_info->tracks[JLS_TRACK_TYPE_UTC];

    // Construct payload
    struct jls_utc_data_s payload = {
        .header = {
                .timestamp=sample_id,
                .entry_count=1,
                .entry_size_bits=sizeof(utc) * 8,
                .rsv16=0,
        },
        .timestamp=utc
    };
    uint32_t payload_length = sizeof(payload);

    // construct header
    struct jls_core_chunk_s chunk;
    uint64_t offset = jls_raw_chunk_tell(self->core.raw);
    chunk.hdr.item_next = 0;  // update later
    chunk.hdr.item_prev = signal_info->tracks[JLS_TRACK_TYPE_UTC].data_head.offset;
    chunk.hdr.tag = JLS_TAG_TRACK_UTC_DATA;
    chunk.hdr.rsv0_u8 = 0;
    chunk.hdr.chunk_meta = signal_id;
    chunk.hdr.payload_length = payload_length;
    chunk.offset = offset;

    // write
    ROE(jls_raw_wr(self->core.raw, &chunk.hdr, (uint8_t *) &payload));
    ROE(jls_core_update_item_head(&self->core, &signal_info->tracks[JLS_TRACK_TYPE_UTC].data_head, &chunk));
    ROE(jls_track_update(track, 0, offset));

    ROE(jls_wr_ts_utc(signal_info->track_utc, sample_id, offset, utc));
    return 0;
}
