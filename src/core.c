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

#include "jls/core.h"
#include "jls/backend.h"
#include "jls/crc32c.h"
#include "jls/format.h"
#include "jls/bit_shift.h"
#include "jls/cdef.h"
#include "jls/ec.h"
#include "jls/log.h"
#include "jls/track.h"
#include "jls/util.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>


#define SAMPLE_SIZE_BYTES_MAX           (32)  // =256 bits, must be power of 2
#define SAMPLE_DECIMATE_FACTOR_MIN      (10)
#define SAMPLES_PER_DATA_MIN            (SAMPLE_DECIMATE_FACTOR_MIN)
#define ENTRIES_PER_SUMMARY_MIN         (SAMPLE_DECIMATE_FACTOR_MIN)
#define SUMMARY_DECIMATE_FACTOR_MIN     (SAMPLE_DECIMATE_FACTOR_MIN)
#define F64_BUF_LENGTH_MIN (1 << 16)
#define SIGNAL_MASK  (0x0fff)
#define TAU_F (6.283185307179586f)


static const struct jls_signal_def_s SIGNAL_64_DEFAULTS = {
        .samples_per_data = 8192,
        .sample_decimate_factor = 128,
        .entries_per_summary = 640,
        .summary_decimate_factor = 20,
};

static const struct jls_signal_def_s SIGNAL_32_DEFAULTS = {
        .samples_per_data = 8192,
        .sample_decimate_factor = 128,
        .entries_per_summary = 640,
        .summary_decimate_factor = 20,
        .annotation_decimate_factor = 100,
        .utc_decimate_factor = 100,
};

static const struct jls_signal_def_s SIGNAL_16_DEFAULTS = {
        .samples_per_data = 16384,
        .sample_decimate_factor = 256,
        .entries_per_summary = 1280,
        .summary_decimate_factor = 20,
};

static const struct jls_signal_def_s SIGNAL_8_DEFAULTS = {
        .samples_per_data = 32768,
        .sample_decimate_factor = 1024,
        .entries_per_summary = 640,
        .summary_decimate_factor = 20,
};

static const struct jls_signal_def_s SIGNAL_4_DEFAULTS = {
        .samples_per_data = 65536,
        .sample_decimate_factor = 1024,
        .entries_per_summary = 1280,
        .summary_decimate_factor = 20,
};

static const struct jls_signal_def_s SIGNAL_1_DEFAULTS = {
        .samples_per_data = 65536,
        .sample_decimate_factor = 1024,
        .entries_per_summary = 1280,
        .summary_decimate_factor = 20,
};

static inline uint32_t u32_max(uint32_t a, uint32_t b) {
    return (a > b) ? a : b;
}

static uint32_t round_up_to_multiple(uint32_t x, uint32_t m) {
    return ((x + m - 1) / m) * m;
}

int32_t jls_core_f64_buf_alloc(size_t length, struct jls_core_f64_buf_s ** buf) {
    if (*buf) {
        if ((*buf)->alloc_length >= (size_t) length) {
            return 0;
        } else {
            free(*buf);
            *buf = NULL;
        }
    }

    if (length < F64_BUF_LENGTH_MIN) {
        length = F64_BUF_LENGTH_MIN;
    }
    struct jls_core_f64_buf_s * b = malloc(sizeof(struct jls_core_f64_buf_s) + length * sizeof(double));
    if (!b) {
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    b->start = b->buffer;
    b->end = b->buffer + length;
    b->alloc_length = length;
    *buf = b;
    return 0;
}

void jls_core_f64_buf_free(struct jls_core_f64_buf_s * buf) {
    if (NULL != buf) {
        free(buf);
    }
}

int32_t jls_core_signal_def_validate(struct jls_signal_def_s const * def) {
    // externally verify signal_id
    // externally verify source_id

    if (def->signal_id >= JLS_SIGNAL_COUNT) {
        JLS_LOGW("signal_id %d too big - skip", (int) def->signal_id);
        return JLS_ERROR_PARAMETER_INVALID;
    }
    if (def->source_id >= JLS_SOURCE_COUNT) {
        JLS_LOGW("source_id %d too big - skip", (int) def->source_id);
        return JLS_ERROR_PARAMETER_INVALID;
    }

    if ((def->signal_type != JLS_SIGNAL_TYPE_FSR) && (def->signal_type != JLS_SIGNAL_TYPE_VSR)) {
        JLS_LOGE("Invalid signal type: %d", (int) def->signal_type);
        return JLS_ERROR_PARAMETER_INVALID;
    }

    switch (def->data_type & 0xffff) {
        case JLS_DATATYPE_I4: break;
        case JLS_DATATYPE_I8: break;
        case JLS_DATATYPE_I16: break;
        case JLS_DATATYPE_I24: break;
        case JLS_DATATYPE_I32: break;
        case JLS_DATATYPE_I64: break;

        case JLS_DATATYPE_U1: break;
        case JLS_DATATYPE_U4: break;
        case JLS_DATATYPE_U8: break;
        case JLS_DATATYPE_U16: break;
        case JLS_DATATYPE_U24: break;
        case JLS_DATATYPE_U32: break;
        case JLS_DATATYPE_U64: break;

        case JLS_DATATYPE_F32: break;
        case JLS_DATATYPE_F64: break;
        default:
            JLS_LOGW("Invalid data type: 0x%08x", def->data_type);
            return JLS_ERROR_PARAMETER_INVALID;
    }

    // Check fixed-point specification
    if (jls_datatype_parse_q(def->data_type)) {
        switch (jls_datatype_parse_basetype(def->data_type)) {
            case JLS_DATATYPE_BASETYPE_INT: break;
            case JLS_DATATYPE_BASETYPE_UINT: break;
            case JLS_DATATYPE_BASETYPE_FLOAT:
                JLS_LOGW("Floating point cannot support q");
                return JLS_ERROR_PARAMETER_INVALID;
            default:
                JLS_LOGW("Invalid data type: 0x%08x", def->data_type);
                return JLS_ERROR_PARAMETER_INVALID;
        }
    }
    return 0;
}

#define SIGNAL_DEF_DEFAULT(field)   \
    if (0 == def->field) {          \
        def->field = d->field;      \
    }

static void signal_def_defaults(struct jls_signal_def_s * def) {
    const struct jls_signal_def_s * d;
    uint8_t sample_size = jls_datatype_parse_size(def->data_type);
    switch (sample_size) {
        case 1:  d = &SIGNAL_1_DEFAULTS; break;
        case 4:  d = &SIGNAL_4_DEFAULTS; break;
        case 8:  d = &SIGNAL_8_DEFAULTS; break;
        case 16: d = &SIGNAL_16_DEFAULTS; break;
        case 32: d = &SIGNAL_32_DEFAULTS; break;
        case 64: d = &SIGNAL_64_DEFAULTS; break;
        default: return;
    }

    SIGNAL_DEF_DEFAULT(samples_per_data);
    SIGNAL_DEF_DEFAULT(sample_decimate_factor);
    SIGNAL_DEF_DEFAULT(entries_per_summary);
    SIGNAL_DEF_DEFAULT(summary_decimate_factor);

    // common parameters
    d = &SIGNAL_32_DEFAULTS;
    SIGNAL_DEF_DEFAULT(annotation_decimate_factor);
    SIGNAL_DEF_DEFAULT(utc_decimate_factor);
}

int32_t jls_core_signal_def_align(struct jls_signal_def_s * def) {
    signal_def_defaults(def);
    uint8_t sample_size = jls_datatype_parse_size(def->data_type);
    uint32_t samples_per_data_multiple = (SAMPLE_SIZE_BYTES_MAX * 8) / sample_size;

    uint32_t sample_decimate_factor = u32_max(def->sample_decimate_factor, SAMPLE_DECIMATE_FACTOR_MIN);
    sample_decimate_factor = round_up_to_multiple(sample_decimate_factor, samples_per_data_multiple);

    uint32_t samples_per_data = u32_max(def->samples_per_data, SAMPLES_PER_DATA_MIN);
    uint32_t entries_per_summary = u32_max(def->entries_per_summary, ENTRIES_PER_SUMMARY_MIN);
    uint32_t summary_decimate_factor = u32_max(def->summary_decimate_factor, SUMMARY_DECIMATE_FACTOR_MIN);

    entries_per_summary = round_up_to_multiple(entries_per_summary, summary_decimate_factor);
    samples_per_data = round_up_to_multiple(samples_per_data, sample_decimate_factor);
    uint32_t entries_per_data = samples_per_data / sample_decimate_factor;

    while (entries_per_summary != ((entries_per_summary / entries_per_data) * entries_per_data)) {
        // reduce until fits.
        --entries_per_data;
    }

    samples_per_data = sample_decimate_factor * entries_per_data;

    if (sample_decimate_factor != def->sample_decimate_factor) {
        JLS_LOGI("sample_decimate_factor adjusted from %" PRIu32 " to %" PRIu32,
                def->sample_decimate_factor, sample_decimate_factor);
    }
    if (samples_per_data != def->samples_per_data) {
        JLS_LOGI("samples_per_data adjusted from %" PRIu32 " to %" PRIu32,
                def->samples_per_data, samples_per_data);
    }
    if (entries_per_summary != def->entries_per_summary) {
        JLS_LOGI("entries_per_summary adjusted from %" PRIu32 " to %" PRIu32,
                def->entries_per_summary, entries_per_summary);
    }

    def->sample_decimate_factor = sample_decimate_factor;
    def->samples_per_data = samples_per_data;
    def->entries_per_summary = entries_per_summary;
    def->summary_decimate_factor = summary_decimate_factor;
    return 0;
}

int32_t jls_core_update_chunk_header(struct jls_core_s * self, struct jls_core_chunk_s * chunk) {
    if (chunk->offset) {
        int64_t current_pos = jls_raw_chunk_tell(self->raw);
        ROE(jls_raw_chunk_seek(self->raw, chunk->offset));
        ROE(jls_raw_wr_header(self->raw, &chunk->hdr));
        ROE(jls_raw_chunk_seek(self->raw, current_pos));
    }
    return 0;
}

int32_t jls_core_update_item_head(struct jls_core_s * self, struct jls_core_chunk_s * head, struct jls_core_chunk_s * next) {
    if (head->offset) {
        int64_t current_pos = jls_raw_chunk_tell(self->raw);
        head->hdr.item_next = next->offset;
        ROE(jls_raw_chunk_seek(self->raw, head->offset));
        ROE(jls_raw_wr_header(self->raw, &head->hdr));
        ROE(jls_raw_chunk_seek(self->raw, current_pos));
    }
    *head = *next;
    return 0;
}

int32_t jls_core_signal_validate(struct jls_core_s * self, uint16_t signal_id) {
    if (signal_id >= JLS_SIGNAL_COUNT) {
        JLS_LOGW("signal_id %d too big", (int) signal_id);
        return JLS_ERROR_PARAMETER_INVALID;
    }
    struct jls_core_signal_s * signal_info = &self->signal_info[signal_id];
    if (signal_info->signal_def.signal_id != signal_id) {
        JLS_LOGW("signal_id %d not defined", (int) signal_id);
        return false;
    }
    if (!signal_info->chunk_def.offset) {
        JLS_LOGW("attempted to annotated an undefined signal %d", (int) signal_id);
        return JLS_ERROR_NOT_FOUND;
    }
    return 0;
}

int32_t jls_core_signal_validate_typed(struct jls_core_s * self, uint16_t signal_id, enum jls_signal_type_e signal_type) {
    ROE(jls_core_signal_validate(self, signal_id));
    struct jls_core_signal_s * signal_info = &self->signal_info[signal_id];
    if (signal_info->signal_def.signal_type != signal_type) {
        JLS_LOGW("signal_id %d type invalid", (int) signal_id);
        return JLS_ERROR_NOT_SUPPORTED;
    }
    return 0;
}

int32_t jls_core_validate_track_tag(struct jls_core_s * self, uint16_t signal_id, uint8_t tag) {
    ROE(jls_core_signal_validate(self, signal_id));
    struct jls_signal_def_s * signal_def = &self->signal_info[signal_id].signal_def;
    uint8_t track_type = jls_core_tag_parse_track_type(tag);
    switch (signal_def->signal_type) {
        case JLS_SIGNAL_TYPE_FSR:
            if ((track_type == JLS_TRACK_TYPE_FSR)
                || (track_type == JLS_TRACK_TYPE_ANNOTATION)
                || (track_type == JLS_TRACK_TYPE_UTC)) {
                // good
            } else {
                JLS_LOGW("unsupported track %d for FSR signal", (int) track_type);
                return JLS_ERROR_PARAMETER_INVALID;
            }
            break;
        case JLS_SIGNAL_TYPE_VSR:
            if ((track_type == JLS_TRACK_TYPE_VSR)
                || (track_type == JLS_TRACK_TYPE_ANNOTATION)) {
                // good
            } else {
                JLS_LOGW("unsupported track %d for VSR signal", (int) track_type);
                return JLS_ERROR_PARAMETER_INVALID;
            }
            break;
        default:
            // should have already been checked.
            JLS_LOGW("unsupported signal type: %d", (int) signal_def->signal_type);
            return JLS_ERROR_PARAMETER_INVALID;
    }
    return 0;
}

int32_t jls_core_wr_data(struct jls_core_s * self, uint16_t signal_id, enum jls_track_type_e track_type,
                         const uint8_t * payload, uint32_t payload_length) {
    ROE(jls_core_signal_validate(self, signal_id));
    struct jls_core_signal_s * info = &self->signal_info[signal_id];
    struct jls_core_track_s * track = &info->tracks[track_type];
    struct jls_core_chunk_s chunk;

    chunk.hdr.item_next = 0;  // update later
    chunk.hdr.item_prev = track->data_head.offset;
    chunk.hdr.tag = jls_track_tag_pack(track_type, JLS_TRACK_CHUNK_DATA);
    chunk.hdr.rsv0_u8 = 0;
    chunk.hdr.chunk_meta = signal_id | (0 << 12);
    chunk.hdr.payload_length = payload_length;
    chunk.offset = jls_raw_chunk_tell(self->raw);

    if (JLS_LOG_CHECK_STATIC(JLS_LOG_LEVEL_DEBUG3)) {
        struct jls_payload_header_s * hdr = (struct jls_payload_header_s *) payload;
                JLS_LOGD3("wr_data(signal_id=%d, timestamp=%" PRIi64 ", entries=%" PRIu32 ") => offset=%" PRIi64,
                          (int) signal_id, hdr->timestamp, hdr->entry_count,
                          jls_raw_chunk_tell(self->raw));
    }

    ROE(jls_raw_wr(self->raw, &chunk.hdr, payload));
    ROE(jls_core_update_item_head(self, &track->data_head, &chunk));

    if (!track->head_offsets[0]) {
        track->head_offsets[0] = chunk.offset;
        ROE(jls_track_wr_head(track));
    }

    return 0;
}

int32_t jls_core_wr_summary(struct jls_core_s * self, uint16_t signal_id, enum jls_track_type_e track_type, uint8_t level,
                            const uint8_t * payload, uint32_t payload_length) {
    ROE(jls_core_signal_validate(self, signal_id));
    struct jls_core_signal_s * info = &self->signal_info[signal_id];
    struct jls_core_track_s * track = &info->tracks[track_type];
    struct jls_core_chunk_s chunk;

    chunk.hdr.item_next = 0;  // update later
    chunk.hdr.item_prev = track->summary_head[level].offset;
    chunk.hdr.tag = jls_track_tag_pack(track_type, JLS_TRACK_CHUNK_SUMMARY);
    chunk.hdr.rsv0_u8 = 0;
    chunk.hdr.chunk_meta = signal_id | (((uint16_t) level) << 12);
    chunk.hdr.payload_length = payload_length;
    chunk.offset = jls_raw_chunk_tell(self->raw);

    // write
    if (JLS_LOG_CHECK_STATIC(JLS_LOG_LEVEL_DEBUG3)) {
        struct jls_payload_header_s * hdr = (struct jls_payload_header_s *) payload;
                JLS_LOGD3("wr_summary(signal_id=%d, level=%d, timestamp=%" PRIi64 ", entries=%" PRIu32
                                  ") => offset=%" PRIi64,
                          (int) signal_id, (int) level,
                          hdr->timestamp, hdr->entry_count,
                          jls_raw_chunk_tell(self->raw));
    }
    ROE(jls_raw_wr(self->raw, &chunk.hdr, payload));
    ROE(jls_core_update_item_head(self, &track->summary_head[level], &chunk));
    return 0;
}

int32_t jls_core_wr_index(struct jls_core_s * self, uint16_t signal_id, enum jls_track_type_e track_type, uint8_t level,
                          const uint8_t * payload, uint32_t payload_length) {
    ROE(jls_core_signal_validate(self, signal_id));
    struct jls_core_signal_s * info = &self->signal_info[signal_id];
    struct jls_core_track_s * track = &info->tracks[track_type];
    struct jls_core_chunk_s chunk;

    chunk.hdr.item_next = 0;  // update later
    chunk.hdr.item_prev = track->index_head[level].offset;
    chunk.hdr.tag = jls_track_tag_pack(track_type, JLS_TRACK_CHUNK_INDEX);
    chunk.hdr.rsv0_u8 = 0;
    chunk.hdr.chunk_meta = signal_id | (((uint16_t) level) << 12);;
    chunk.hdr.payload_length = payload_length;
    chunk.offset = jls_raw_chunk_tell(self->raw);

    // write
    if (JLS_LOG_CHECK_STATIC(JLS_LOG_LEVEL_DEBUG3)) {
        struct jls_payload_header_s * hdr = (struct jls_payload_header_s *) payload;
                JLS_LOGD3("wr_index(signal_id=%d, level=%d, timestamp=%" PRIi64 ", entries=%" PRIu32
                                  ") => offset=%" PRIi64,
                          (int) signal_id, (int) level,
                          hdr->timestamp, hdr->entry_count,
                          jls_raw_chunk_tell(self->raw));
    }
    ROE(jls_raw_wr(self->raw, &chunk.hdr, payload));
    ROE(jls_core_update_item_head(self, &track->index_head[level], &chunk));
    ROE(jls_track_update(track, level, chunk.offset));

    return 0;
}

int32_t jls_core_wr_end(struct jls_core_s * self) {
    // construct header
    struct jls_core_chunk_s chunk;
    chunk.hdr.item_next = 0;
    chunk.hdr.item_prev = 0;
    chunk.hdr.tag = JLS_TAG_END;
    chunk.hdr.rsv0_u8 = 0;
    chunk.hdr.chunk_meta = 0;
    chunk.hdr.payload_length = 0;
    chunk.offset = jls_raw_chunk_tell(self->raw);

    // write
    ROE(jls_raw_wr(self->raw, &chunk.hdr, NULL));
    return 0;
}

int32_t jls_core_rd_chunk(struct jls_core_s * self) {
    while (1) {
        self->chunk_cur.offset = jls_raw_chunk_tell(self->raw);
        int32_t rc = jls_raw_rd(self->raw, &self->chunk_cur.hdr, (uint32_t) self->buf->alloc_size, self->buf->start);
        if (rc == JLS_ERROR_TOO_BIG) {
            ROE(jls_buf_realloc(self->buf, self->chunk_cur.hdr.payload_length));
        } else if (rc == 0) {
            self->buf->cur = self->buf->start;
            self->buf->length = self->chunk_cur.hdr.payload_length;
            self->buf->end = self->buf->start + self->buf->length;
            return 0;
        } else {
            return rc;
        }
    }
}

int32_t jls_core_rd_chunk_end(struct jls_core_s * self) {
    uint64_t data[128];
    struct jls_bkf_s * backend = jls_raw_backend(self->raw);
    int64_t end_pos = backend->fend & ~0x7ULL;
    int64_t length = end_pos;
    struct jls_chunk_header_s * h;
    while ((end_pos > 0) && (length > (int64_t) sizeof(*h))) {
        int64_t pos = end_pos - sizeof(data);
        if (pos < 0) {
            pos = 0;
        }
        if (jls_bk_fseek(backend, pos, SEEK_SET)) {
            return JLS_ERROR_IO;
        }
        length = end_pos - pos;
        if (jls_bk_fread(backend, (uint8_t *) data, (unsigned) length)) {
            return JLS_ERROR_EMPTY;
        }
        for (int64_t i = (length - sizeof(struct jls_chunk_header_s)) / sizeof(uint64_t); i > 0; --i) {
            h = (struct jls_chunk_header_s *) &data[i];
            uint32_t crc32 = jls_crc32c_hdr(h);
            if (crc32 == h->crc32) {
                int64_t pos_final = pos + i * sizeof(uint64_t);
                // likely chunk candidate, validate payload
                if (jls_raw_chunk_seek(self->raw, pos_final)) {
                    return JLS_ERROR_IO;
                }
                if (0 == jls_core_rd_chunk(self)) {
                    if (jls_raw_chunk_seek(self->raw, pos_final)) {
                        return JLS_ERROR_IO;
                    }
                    JLS_LOGI("End chunk at %" PRIi64 ", file end at %" PRIi64 ", offset %" PRIi64,
                             pos_final, backend->fend, backend->fend - pos_final);
                    return 0;
                }
            }
        }
        end_pos = pos + sizeof(struct jls_chunk_header_s) - sizeof(uint64_t);
    }
    return JLS_ERROR_NOT_FOUND;
}


int32_t jls_core_scan_sources(struct jls_core_s * self) {
    JLS_LOGD1("jls_core_scan_sources");
    ROE(jls_raw_chunk_seek(self->raw, self->source_head.offset));
    while (1) {
        ROE(jls_core_rd_chunk(self));
        uint16_t source_id = self->chunk_cur.hdr.chunk_meta;
        if (source_id >= JLS_SOURCE_COUNT) {
            JLS_LOGW("source_id %d too big - skip", (int) source_id);
        } else {
            struct jls_core_source_s * source_info = &self->source_info[source_id];
            source_info->chunk_def = self->chunk_cur;
            struct jls_source_def_s *src = &source_info->source_def;
            ROE(jls_buf_rd_skip(self->buf, 64));
            ROE(jls_buf_rd_str(self->buf, (const char **) &src->name));
            ROE(jls_buf_rd_str(self->buf, (const char **) &src->vendor));
            ROE(jls_buf_rd_str(self->buf, (const char **) &src->model));
            ROE(jls_buf_rd_str(self->buf, (const char **) &src->version));
            ROE(jls_buf_rd_str(self->buf, (const char **) &src->serial_number));
            src->source_id = source_id;  // indicate that this source is valid!
            JLS_LOGD1("Found source %d : %s", (int) source_id, src->name);
        }
        if (!self->chunk_cur.hdr.item_next) {
            break;
        }
        ROE(jls_raw_chunk_seek(self->raw, self->chunk_cur.hdr.item_next));
    }
    return 0;
}

static int32_t handle_signal_def(struct jls_core_s * self) {
    uint16_t signal_id = self->chunk_cur.hdr.chunk_meta;
    if (signal_id >= JLS_SIGNAL_COUNT) {
        JLS_LOGW("signal_id %d too big - skip", (int) signal_id);
        return JLS_ERROR_PARAMETER_INVALID;
    }
    struct jls_core_signal_s * signal_info = &self->signal_info[signal_id];
    signal_info->chunk_def = self->chunk_cur;
    signal_info->parent = self;
    struct jls_signal_def_s *s = &signal_info->signal_def;
    ROE(jls_buf_rd_u16(self->buf, &s->source_id));
    ROE(jls_buf_rd_u8(self->buf, &s->signal_type));
    ROE(jls_buf_rd_skip(self->buf, 1));
    ROE(jls_buf_rd_u32(self->buf, &s->data_type));
    ROE(jls_buf_rd_u32(self->buf, &s->sample_rate));
    ROE(jls_buf_rd_u32(self->buf, &s->samples_per_data));
    ROE(jls_buf_rd_u32(self->buf, &s->sample_decimate_factor));
    ROE(jls_buf_rd_u32(self->buf, &s->entries_per_summary));
    ROE(jls_buf_rd_u32(self->buf, &s->summary_decimate_factor));
    ROE(jls_buf_rd_u32(self->buf, &s->annotation_decimate_factor));
    ROE(jls_buf_rd_u32(self->buf, &s->utc_decimate_factor));
    ROE(jls_buf_rd_skip(self->buf, 92));
    ROE(jls_buf_rd_str(self->buf, (const char **) &s->name));
    ROE(jls_buf_rd_str(self->buf, (const char **) &s->units));
    if (0 == jls_core_signal_def_validate(s)) {  // validate passed
        s->signal_id = signal_id;  // indicate that this signal is valid
        JLS_LOGD1("Found signal %d : %s", (int) signal_id, s->name);
    } else {
        JLS_LOGW("Signal validation failed for %d : %s", signal_id, s->name);
    } // else skip
    return 0;
}

static int32_t handle_track_def(struct jls_core_s * self, int64_t pos) {
    (void) pos;  // unused
    uint16_t signal_id = self->chunk_cur.hdr.chunk_meta & SIGNAL_MASK;
    ROE(jls_core_validate_track_tag(self, signal_id, self->chunk_cur.hdr.tag));
    return 0;
}

static int32_t handle_track_head(struct jls_core_s * self, int64_t pos) {
    (void) pos;  // unused
    uint16_t signal_id = self->chunk_cur.hdr.chunk_meta & SIGNAL_MASK;
    ROE(jls_core_validate_track_tag(self, signal_id, self->chunk_cur.hdr.tag));
    uint8_t track_type = jls_core_tag_parse_track_type(self->chunk_cur.hdr.tag);
    size_t expect_sz = JLS_SUMMARY_LEVEL_COUNT * sizeof(int64_t);

    if (self->buf->length != expect_sz) {
        JLS_LOGW("cannot parse signal %d head, sz=%zu, expect=%zu",
                 (int) signal_id, self->buf->length, expect_sz);
        return JLS_ERROR_PARAMETER_INVALID;
    }
    struct jls_core_signal_s * signal = &self->signal_info[signal_id];
    struct jls_core_track_s * track = &signal->tracks[track_type];
    track->parent = signal;
    track->track_type = track_type;
    track->head = self->chunk_cur;
    memcpy(track->head_offsets, self->buf->start, expect_sz);
    return 0;
}

int32_t jls_core_scan_signals(struct jls_core_s * self) {
    JLS_LOGD1("jls_core_scan_signals");
    ROE(jls_raw_chunk_seek(self->raw, self->signal_head.offset));
    while (1) {
        ROE(jls_core_rd_chunk(self));
        if (self->chunk_cur.hdr.tag == JLS_TAG_SIGNAL_DEF) {
            handle_signal_def(self);
        } else if ((self->chunk_cur.hdr.tag & 7) == JLS_TRACK_CHUNK_DEF) {
            handle_track_def(self, self->chunk_cur.offset);
        } else if ((self->chunk_cur.hdr.tag & 7) == JLS_TRACK_CHUNK_HEAD) {
            handle_track_head(self, self->chunk_cur.offset);
        } else {
            JLS_LOGW("unknown tag %d in signal list", (int) self->chunk_cur.hdr.tag);
        }
        if (!self->chunk_cur.hdr.item_next) {
            break;
        }
        ROE(jls_raw_chunk_seek(self->raw, self->chunk_cur.hdr.item_next));
    }
    return 0;
}

int32_t jls_core_scan_fsr_sample_id(struct jls_core_s * self) {
    JLS_LOGD1("jls_core_scan_fsr_sample_id");
    for (uint32_t signal_id = 1; signal_id < JLS_SIGNAL_COUNT; ++signal_id) {
        struct jls_signal_def_s * signal_def = &self->signal_info[signal_id].signal_def;
        if ((signal_def->signal_id != signal_id) || (signal_def->signal_type != JLS_SIGNAL_TYPE_FSR)) {
            continue;
        }
        int64_t offset = self->signal_info[signal_id].tracks[JLS_TRACK_TYPE_FSR].head_offsets[0];
        if (offset == 0) {
            continue;  // no data
        }
        ROE(jls_raw_chunk_seek(self->raw, offset));
        ROE(jls_core_rd_chunk(self));
        if (self->chunk_cur.hdr.tag != JLS_TAG_TRACK_FSR_DATA) {
            JLS_LOGW("jls_core_scan_fsr_sample_id tag mismatch: %d", (int) self->chunk_cur.hdr.tag);
            continue;
        }

        struct jls_fsr_data_s * r = (struct jls_fsr_data_s *) self->buf->start;
        signal_def->sample_id_offset = r->header.timestamp;
    }
    return 0;
}

int32_t jls_core_scan_initial(struct jls_core_s * self) {
    int32_t rc = 0;
    uint8_t found = 0;

    for (int i = 0; found != 7; ++i) {
        if (i == 3) {
            JLS_LOGW("malformed JLS, continue searching");
        }
        int64_t pos = jls_raw_chunk_tell(self->raw);
        rc = jls_core_rd_chunk(self);
        if (rc == JLS_ERROR_EMPTY) {
            return 0;
        } else if (rc) {
            return rc;
        }

        JLS_LOGD1("scan tag %d : %s", self->chunk_cur.hdr.tag, jls_tag_to_name(self->chunk_cur.hdr.tag));
        switch (self->chunk_cur.hdr.tag) {
            case JLS_TAG_USER_DATA:
                found |= 1;
                if (!self->user_data_head.offset) {
                    self->user_data_head.offset = pos;
                    self->user_data_head.hdr = self->chunk_cur.hdr;
                }
                break;
            case JLS_TAG_SOURCE_DEF:
                found |= 2;
                if (!self->source_head.offset) {
                    self->source_head.offset = pos;
                    self->source_head.hdr = self->chunk_cur.hdr;
                }
                break;
            case JLS_TAG_SIGNAL_DEF:
                found |= 4;
                if (!self->signal_head.offset) {
                    self->signal_head.offset = pos;
                    self->signal_head.hdr = self->chunk_cur.hdr;
                }
                break;
            default:
                break;  // skip
        }
    }
    JLS_LOGD1("found initial tags");
    return 0;
}

int32_t jls_core_sources(struct jls_core_s * self, struct jls_source_def_s ** sources, uint16_t * count)  {
    if (!self || !sources || !count) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    uint16_t c = 0;
    for (uint16_t i = 0; i < JLS_SOURCE_COUNT; ++i) {
        if (self->source_info[i].source_def.source_id == i) {
            // Note: source 0 is always defined, so calloc is ok
            self->source_def_api[c++] = self->source_info[i].source_def;
        }
    }
    *sources = self->source_def_api;
    *count = c;
    return 0;
}

int32_t jls_core_signals(struct jls_core_s * self, struct jls_signal_def_s ** signals, uint16_t * count) {
    if (!self || !signals || !count) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    uint16_t c = 0;
    for (uint16_t i = 0; i < JLS_SIGNAL_COUNT; ++i) {
        if (self->signal_info[i].signal_def.signal_id == i) {
            // Note: signal 0 is always defined, so calloc is ok
            self->signal_def_api[c++] = self->signal_info[i].signal_def;
        }
    }
    *signals = self->signal_def_api;
    *count = c;
    return 0;
}

int32_t jls_core_signal(struct jls_core_s * self, uint16_t signal_id, struct jls_signal_def_s * signal) {
    ROE(jls_core_signal_validate(self, signal_id));
    if (signal) {
        *signal = self->signal_info[signal_id].signal_def;
    }
    return 0;
}

int32_t jls_core_fsr_seek(struct jls_core_s * self, uint16_t signal_id, uint8_t level, int64_t sample_id) {
    // timestamp in JLS units with possible non-zero offset
    ROE(jls_core_signal_validate(self, signal_id));
    struct jls_signal_def_s * signal_def = &self->signal_info[signal_id].signal_def;
    if (signal_def->signal_type != JLS_SIGNAL_TYPE_FSR) {
        JLS_LOGW("fsr_seek not support for signal type %d", (int) signal_def->signal_type);
        return JLS_ERROR_NOT_SUPPORTED;
    }
    int64_t offset = 0;
    int64_t * offsets = self->signal_info[signal_id].tracks[JLS_TRACK_TYPE_FSR].head_offsets;
    int initial_level = JLS_SUMMARY_LEVEL_COUNT - 1;
    for (; initial_level >= 0; --initial_level) {
        if (offsets[initial_level]) {
            offset = offsets[initial_level];
            break;
        }
    }
    if (!offset) {
        return JLS_ERROR_NOT_FOUND;
    }

    for (int lvl = initial_level; lvl > level; --lvl) {
        // compute the step size in samples between each index entry.
        int64_t step_size = signal_def->samples_per_data;  // each data chunk
        if (lvl > 1) {
            step_size *= signal_def->entries_per_summary /
                    (signal_def->samples_per_data / signal_def->sample_decimate_factor);
        }
        for (int k = 3; k <= lvl; ++k) {
            step_size *= signal_def->summary_decimate_factor;
        }
        JLS_LOGD3("signal %d, level %d, offset=%" PRIi64 ", step_size=%" PRIi64,
                 (int) signal_id, lvl, offset, step_size);
        ROE(jls_raw_chunk_seek(self->raw, offset));
        ROE(jls_core_rd_chunk(self));
        if (self->chunk_cur.hdr.tag != JLS_TAG_TRACK_FSR_INDEX) {
            JLS_LOGW("seek tag mismatch: %d", (int) self->chunk_cur.hdr.tag);
        }

        struct jls_fsr_index_s * r = (struct jls_fsr_index_s *) self->buf->start;
        int64_t chunk_timestamp = r->header.timestamp;
        int64_t chunk_entries = r->header.entry_count;
        JLS_LOGD3("timestamp=%" PRIi64 ", entries=%" PRIi64, chunk_timestamp, chunk_entries);
        uint8_t * p_end = (uint8_t *) &r->offsets[r->header.entry_count];

        if ((size_t) (p_end - self->buf->start) > self->buf->length) {
            JLS_LOGE("invalid payload length");
            return JLS_ERROR_PARAMETER_INVALID;
        }

        int64_t idx = (sample_id - chunk_timestamp) / step_size;
        if ((idx < 0) || (idx >= chunk_entries)) {
            JLS_LOGE("invalid index signal %d, level %d, sample_id=%"
                     PRIi64 " offset=%" PRIi64 ": %" PRIi64 " >= %" PRIi64,
                     (int) signal_id, lvl, sample_id,
                     offset, idx, chunk_entries);
            return JLS_ERROR_IO;
        }
        offset = r->offsets[idx];
    }

    ROE(jls_raw_chunk_seek(self->raw, offset));
    return 0;
}

int32_t jls_core_fsr_length(struct jls_core_s * self, uint16_t signal_id, int64_t * samples) {
    ROE(jls_core_signal_validate_typed(self, signal_id, JLS_SIGNAL_TYPE_FSR));
    int64_t * signal_length = &self->signal_info[signal_id].track_fsr->signal_length;
    struct jls_signal_def_s * signal_def = &self->signal_info[signal_id].signal_def;
    if (*signal_length >= 0) {
        *samples = *signal_length;
        return 0;
    }

    // length is not store explicitly
    // traverse to last entry of each level to then reach last data block.
    int64_t offset = 0;
    int64_t * offsets = self->signal_info[signal_id].tracks[JLS_TRACK_TYPE_FSR].head_offsets;
    int level = JLS_SUMMARY_LEVEL_COUNT - 1;
    for (; level >= 0; --level) {
        offset = offsets[level];
        if (offset && (0 == jls_raw_chunk_seek(self->raw, offset))) {
            break;
        } else {
            offset = 0;
            offsets[level] = 0;
        }
    }
    if (!offset) {
        *samples = 0;
        return 0;
    }
    struct jls_fsr_index_s * r = NULL;

    for (int lvl = level; lvl > 0; --lvl) {
        JLS_LOGD3("signal %d, level %d, index=%" PRIi64, (int) signal_id, (int) lvl, offset);
        ROE(jls_raw_chunk_seek(self->raw, offset));
        ROE(jls_core_rd_chunk(self));

        r = (struct jls_fsr_index_s *) self->buf->start;
        if (r->header.entry_size_bits != (sizeof(r->offsets[0]) * 8)) {
            JLS_LOGE("invalid FSR index entry size: %d bits", (int) r->header.entry_size_bits);
            return JLS_ERROR_PARAMETER_INVALID;
        }
        size_t sz = sizeof(r->header) + r->header.entry_count * sizeof(r->offsets[0]);
        if (sz > self->buf->length) {
            JLS_LOGE("invalid payload length");
            return JLS_ERROR_PARAMETER_INVALID;
        }
        if (r->header.entry_count > 0) {
            offset = r->offsets[r->header.entry_count - 1];
        }

        // only valid for level 1 index
        if (lvl == 1) {
            ROE(jls_core_rd_chunk(self)); // summary
            struct jls_fsr_f32_summary_s * s = (struct jls_fsr_f32_summary_s *) self->buf->start;
            *signal_length = s->header.timestamp +
                             s->header.entry_count * signal_def->sample_decimate_factor
                             - signal_def->sample_id_offset;
        }
    }

    if (offset) {
        ROE(jls_raw_chunk_seek(self->raw, offset));
        ROE(jls_core_rd_chunk(self));
        struct jls_fsr_data_s * d = (struct jls_fsr_data_s *) self->buf->start;
        *signal_length = d->header.timestamp + d->header.entry_count - signal_def->sample_id_offset;
    }
    *samples = *signal_length;
    return 0;
}

static void construct_f32(int64_t sample_id, float * y, int64_t count, float mean, float std) {
    for (int64_t i = 0; i < count; i += 2) {
        uint64_t ki = (int64_t) (sample_id + i);
        int64_t r1 = (ki ^ (ki >> 7)) * 2654435761ULL;                      // pseudo-randomize
        int64_t r2 = ((ki ^ (ki >> 13)) + 2147483647ULL) * 2654435761ULL;   // pseudo-randomize
        float f1 = (r1 & 0xffffffff) / (float) 0xffffffff;
        float f2 = (r2 & 0xffffffff) / (float) 0xffffffff;
        // https://en.wikipedia.org/wiki/Box%E2%80%93Muller_transform
        float g = std * sqrtf(-2.0f * logf(f1));
        f2 *= TAU_F;
        y[i + 0] = mean + g * cosf(f2);
        if ((i + 1) <= count) {
            y[i + 1] = mean + g * sinf(f2);
        }
    }
}

static void construct_f64(int64_t sample_id, double * y, int64_t count, double mean, double std) {
    for (int64_t i = 0; i < count; i += 2) {
        uint64_t ki = (int64_t) (sample_id + i);
        int64_t r1 = (ki ^ (ki >> 7)) * 2654435761ULL;                      // pseudo-randomize
        int64_t r2 = ((ki ^ (ki >> 13)) + 2147483647ULL) * 2654435761ULL;   // pseudo-randomize
        double f1 = (r1 & 0xffffffff) / (double) 0xffffffff;
        double f2 = (r2 & 0xffffffff) / (double) 0xffffffff;
        // https://en.wikipedia.org/wiki/Box%E2%80%93Muller_transform
        double g = std * sqrt(-2.0 * log(f1));
        f2 *= TAU_F;
        y[i + 0] = mean + g * cos(f2);
        if ((i + 1) <= count) {
            y[i + 1] = mean + g * sin(f2);
        }
    }
}

static int32_t reconstruct_omitted_chunk(struct jls_core_s * self, uint16_t signal_id, int64_t start_sample_id) {
    struct jls_signal_def_s * signal_def = &self->signal_info[signal_id].signal_def;
    uint8_t sample_size_bits = jls_datatype_parse_size(signal_def->data_type);

    struct jls_fsr_index_s * r = (struct jls_fsr_index_s *) self->rd_index->start;
    int64_t t_index = (start_sample_id - r->header.timestamp) / signal_def->samples_per_data;
    int64_t sample_id = (t_index * signal_def->samples_per_data) + r->header.timestamp;

    struct jls_fsr_f32_summary_s * s32 = (struct jls_fsr_f32_summary_s *) self->rd_summary->start;
    struct jls_fsr_f64_summary_s * s64 = (struct jls_fsr_f64_summary_s *) self->rd_summary->start;
    int64_t s_index = (sample_id - s32->header.timestamp) / signal_def->sample_decimate_factor;
    bool is_summary_64 = false;
    if (s32->header.entry_size_bits == (4 * sizeof(float) * 8)) {
        //
    } else if (s32->header.entry_size_bits == (4 * sizeof(double) * 8)) {
        is_summary_64 = true;
    } else {
        JLS_LOGE("unsupported summary element size");
        return JLS_ERROR_NOT_SUPPORTED;
    }


    size_t sz = (signal_def->samples_per_data * sample_size_bits) / 8 + sizeof(struct jls_fsr_data_s);
    ROE(jls_buf_realloc(self->buf, sz));

    struct jls_fsr_data_s * data = (struct jls_fsr_data_s *) self->buf->start;
    data->header.entry_count = 0;
    data->header.timestamp = sample_id;
    data->header.entry_size_bits = sample_size_bits;
    data->header.rsv16 = 0;
    uint8_t * d = (uint8_t *) data->data;

    const uint64_t sz_samples = signal_def->sample_decimate_factor;
    const uint64_t sz_bytes = (sz_samples * sample_size_bits) / 8;
    float mu32;
    float std32;
    double mu64;
    double std64;

    for (uint32_t k = 0; k < signal_def->samples_per_data / sz_samples; ++k) {
        if (s_index >= s32->header.entry_count) {
            break;
        }
        if (is_summary_64) {
            mu32 = (float) s64->data[s_index][JLS_SUMMARY_FSR_MEAN];
            std32 = (float) s64->data[s_index][JLS_SUMMARY_FSR_STD];
            mu64 = s64->data[s_index][JLS_SUMMARY_FSR_MEAN];
            std64 = s64->data[s_index][JLS_SUMMARY_FSR_STD];
        } else {
            mu32 = s32->data[s_index][JLS_SUMMARY_FSR_MEAN];
            std32 = s32->data[s_index][JLS_SUMMARY_FSR_STD];
            mu64 = s32->data[s_index][JLS_SUMMARY_FSR_MEAN];
            std64 = s32->data[s_index][JLS_SUMMARY_FSR_STD];
        }

        if (signal_def->data_type == JLS_DATATYPE_F32) {
            construct_f32(sample_id + k * sz_samples, (float *) d, sz_samples, mu32, std32);
        } else if (signal_def->data_type == JLS_DATATYPE_F64) {
            construct_f64(sample_id + k * sz_samples, (double *) d, sz_samples, mu64, std64);
        } else if (signal_def->data_type == JLS_DATATYPE_U8) {
            uint8_t value = (uint8_t) roundf(mu32);
            memset(d, value, sz_bytes);
        } else if (signal_def->data_type == JLS_DATATYPE_U4) {
            uint8_t value = ((uint8_t) roundf(mu32)) & 0x0F;
            value |= (value << 4);
            memset(d, value, sz_bytes);
        } else if (signal_def->data_type == JLS_DATATYPE_U1) {
            uint8_t value = ((uint8_t) roundf(mu32)) & 0x01;
            if (value) {
                value = 0xff;
            }
            memset(d, value, sz_bytes);
        } else {
            memset(d, 0, sz_bytes);  // for now, set to zero
            break;
        }
        d += sz_bytes;
        ++s_index;
        data->header.entry_count += (uint32_t) sz_samples;
    }
    return 0;
}

int32_t jls_core_rd_fsr_level1(struct jls_core_s * self, uint16_t signal_id, int64_t start_sample_id) {
    struct jls_signal_def_s * signal_def = &self->signal_info[signal_id].signal_def;

    if (self->rd_index_chunk.hdr.chunk_meta != ((1 << 12) | (signal_id & 0x00ff))) {
        self->rd_index_chunk.offset = 0;
    } else if (self->rd_index_chunk.offset) {
        struct jls_fsr_index_s * idx = (struct jls_fsr_index_s *) self->rd_index->start;
        int64_t sample_id_end = idx->header.timestamp + idx->header.entry_count * signal_def->samples_per_data;

        if ((start_sample_id >= idx->header.timestamp) && (start_sample_id < sample_id_end)) {
            // matches this index/summary entry, already in memory
            return 0;
        } else {
            self->rd_index_chunk.offset = 0;
        }
    }

    if (0 == self->rd_index_chunk.offset) {
        ROE(jls_core_fsr_seek(self, signal_id, 1, start_sample_id));
    }
    ROE(jls_core_rd_chunk(self));  // index
    jls_buf_copy(self->rd_index, self->buf);
    self->rd_index_chunk = self->chunk_cur;

    ROE(jls_core_rd_chunk(self));  // summary
    jls_buf_copy(self->rd_summary, self->buf);
    self->rd_summary_chunk = self->chunk_cur;
    return 0;
}

int32_t jls_core_rd_fsr_data0(struct jls_core_s * self, uint16_t signal_id, int64_t start_sample_id) {
    int64_t chunk_sample_id;
    struct jls_signal_def_s * signal_def = &self->signal_info[signal_id].signal_def;
    ROE(jls_core_rd_fsr_level1(self, signal_id, start_sample_id));
    struct jls_fsr_index_s * idx = (struct jls_fsr_index_s *) self->rd_index->start;
    int64_t idx_entry = (start_sample_id - idx->header.timestamp) / signal_def->samples_per_data;
    int64_t offset = idx->offsets[idx_entry];
    struct jls_fsr_data_s * r;

    if (0 == offset) {
        // omitted, assume full chunk
        chunk_sample_id = INT64_MAX - INT32_MAX;
    } else if (jls_raw_chunk_seek(self->raw, offset)) {
        return JLS_ERROR_NOT_FOUND;
    } else {
        int32_t rv = jls_core_rd_chunk(self);
        if (rv == JLS_ERROR_EMPTY) {
            return JLS_ERROR_NOT_FOUND;
        } else if (rv) {
            return rv;
        }
        r = (struct jls_fsr_data_s *) self->buf->start;
        chunk_sample_id = r->header.timestamp;

        if (self->chunk_cur.hdr.tag != JLS_TAG_TRACK_FSR_DATA) {
            JLS_LOGW("unexpected chunk tag: %d (expected %d)", (int) self->chunk_cur.hdr.tag, JLS_TAG_TRACK_FSR_DATA);
        }
        if (self->chunk_cur.hdr.chunk_meta != signal_id) {
            JLS_LOGW("unexpected chunk meta: %d (expected %d)", (int) self->chunk_cur.hdr.chunk_meta, signal_id);
        }
    }

    if (start_sample_id < chunk_sample_id) {  // omitted chunk
        ROE(reconstruct_omitted_chunk(self, signal_id, start_sample_id));
    }

    r = (struct jls_fsr_data_s *) self->buf->start;
    if (r->header.entry_size_bits != jls_datatype_parse_size(signal_def->data_type)) {
        JLS_LOGE("invalid data entry size: %d", (int) r->header.entry_size_bits);
        return JLS_ERROR_PARAMETER_INVALID;
    }
    return 0;
}

int32_t jls_core_fsr(struct jls_core_s * self, uint16_t signal_id, int64_t start_sample_id,
                     void * data, int64_t data_length) {
    // start_sample_id is API zero-based
    uint8_t * data_u8 = (uint8_t *) data;
    ROE(jls_core_signal_validate_typed(self, signal_id, JLS_SIGNAL_TYPE_FSR));
    int64_t samples = 0;
    ROE(jls_core_fsr_length(self, signal_id, &samples));
    struct jls_signal_def_s * signal_def = &self->signal_info[signal_id].signal_def;
    if (data_length <= 0) {
        return 0;
    } else if (start_sample_id < 0) {
        JLS_LOGW("rd_fsr %d %s: start_sample_id invalid %" PRIi64 " length=%" PRIi64,
                 (int) signal_id, signal_def->name,
                 start_sample_id, samples);
        return JLS_ERROR_PARAMETER_INVALID;
    }

    const int64_t sample_id_offset = signal_def->sample_id_offset;
    uint8_t entry_size_bits = jls_datatype_parse_size(signal_def->data_type);

    if ((start_sample_id + data_length) > samples) {
        JLS_LOGW("rd_fsr %d %s: start=%" PRIi64 " length=%" PRIi64 " > %" PRIi64 " by %" PRIi64,
                 (int) signal_id, signal_def->name,
                 start_sample_id, data_length, samples,
                 start_sample_id + data_length - samples);
        return JLS_ERROR_PARAMETER_INVALID;
    }

    //JLS_LOGD3("jls_rd_fsr_f32(%d, %" PRIi64 ")", (int) signal_id, start_sample_id);
    start_sample_id += sample_id_offset;  // file sample_id

    int64_t chunk_sample_id;
    int64_t chunk_sample_count;
    uint8_t * u8;
    uint8_t shift_bits = 0;
    uint8_t shift_carry = 0;

    while (data_length > 0) {
        ROE(jls_core_rd_fsr_data0(self, signal_id, start_sample_id));

        struct jls_fsr_data_s * r = (struct jls_fsr_data_s *) self->buf->start;
        chunk_sample_id = r->header.timestamp;
        chunk_sample_count = r->header.entry_count;
        u8 = (uint8_t *) &r->data[0];
        if (r->header.entry_size_bits != entry_size_bits) {
            JLS_LOGE("fsr entry size mismatch");
            return JLS_ERROR_UNSPECIFIED;
        }

        int64_t sz_samples = chunk_sample_count;
        if (start_sample_id > chunk_sample_id) {
            // should only happen on first chunk
            int64_t idx_start = start_sample_id - chunk_sample_id;
            sz_samples = chunk_sample_count - idx_start;
            u8 += ((idx_start * entry_size_bits) / 8);
            switch (entry_size_bits) {
                case 1: shift_bits = (uint8_t) (start_sample_id & 0x07); break;
                case 4: shift_bits = (uint8_t) ((start_sample_id & 0x01) * 4); break;
                default: break;
            }
            if (shift_bits) {
                shift_carry = (*u8++) >> shift_bits;
                uint8_t rem_bits = (uint8_t) ((start_sample_id + data_length - 1) & 0x07) + 1;
                if ((1 == entry_size_bits) && ((8 - shift_bits + rem_bits) > 8)) {
                    // write out carry on buffer wrap when carry + end bits exceed a byte
                    if (data_length > sz_samples) {
                        data_length += 8;
                    }
                } else if ((4 == entry_size_bits) && (sz_samples == 1)) {
                    data_length -= sz_samples;
                    start_sample_id += sz_samples;
                    continue;
                }
            }
        }

        if (sz_samples > data_length) {
            sz_samples = data_length;
        }

        size_t sz_bytes = (size_t) (sz_samples * entry_size_bits + 7) / 8;
        if (shift_bits) {
            for (size_t i = 0; i < sz_bytes; ++i) {
                data_u8[i] = (u8[i] << (8 - shift_bits)) | shift_carry;
                shift_carry = u8[i] >> shift_bits;
            }
            sz_bytes = (sz_samples * entry_size_bits) / 8;
        } else {
            memcpy(data_u8, u8, sz_bytes);
        }
        data_u8 += sz_bytes;
        data_length -= sz_samples;
        start_sample_id += sz_samples;
    }
    return 0;
}

int32_t jls_core_fsr_f32(struct jls_core_s * self, uint16_t signal_id, int64_t start_sample_id,
                         float * data, int64_t data_length) {
    ROE(jls_core_signal_validate_typed(self, signal_id, JLS_SIGNAL_TYPE_FSR));
    if (self->signal_info[signal_id].signal_def.data_type != JLS_DATATYPE_F32) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    return jls_core_fsr(self, signal_id, start_sample_id, data, data_length);
}

int32_t jls_core_ts_seek(struct jls_core_s * self, uint16_t signal_id, uint8_t level,
                         enum jls_track_type_e track_type, int64_t timestamp) {
    // timestamp in JLS units with possible non-zero offset
    ROE(jls_core_signal_validate(self, signal_id));
    switch (track_type) {
        case JLS_TRACK_TYPE_VSR: break;
        case JLS_TRACK_TYPE_ANNOTATION: break;
        case JLS_TRACK_TYPE_UTC: break;
        default:
            JLS_LOGW("jls_core_ts_seek: unsupported track type: %d", (int) track_type);
            return JLS_ERROR_PARAMETER_INVALID;
    }
    int64_t offset = 0;
    int64_t * offsets = self->signal_info[signal_id].tracks[track_type].head_offsets;

    int initial_level = JLS_SUMMARY_LEVEL_COUNT - 1;
    for (; initial_level >= 0; --initial_level) {
        if (offsets[initial_level]) {
            offset = offsets[initial_level];
            break;
        }
    }
    if (!offset) {
        return JLS_ERROR_NOT_FOUND;
    }

    for (int lvl = initial_level; lvl > level; --lvl) {
        JLS_LOGD3("signal %d, level %d, offset=%" PRIi64, (int) signal_id, (int) lvl, offset);
        ROE(jls_raw_chunk_seek(self->raw, offset));
        ROE(jls_core_rd_chunk(self));
        if (self->chunk_cur.hdr.tag != jls_track_tag_pack(track_type, JLS_TRACK_CHUNK_INDEX)) {
            JLS_LOGW("seek tag mismatch: %d", (int) self->chunk_cur.hdr.tag);
        }

        struct jls_index_s * r = (struct jls_index_s *) self->buf->start;
        uint8_t * p_end = (uint8_t *) &r->entries[r->header.entry_count];

        if ((size_t) (p_end - self->buf->start) > self->buf->length) {
            JLS_LOGE("invalid payload length");
            return JLS_ERROR_PARAMETER_INVALID;
        }
        if ((r->header.entry_count == 0) || (r->header.entry_count & 0x80000000)) {
            JLS_LOGE("invalid entry count");
            return JLS_ERROR_PARAMETER_INVALID;
        }

        int32_t idx = 0;
        for (; ; ++idx) {
            if (idx >= (int32_t) r->header.entry_count) {
                idx = ((int32_t) r->header.entry_count) - 1;
                break;
            } else if (r->entries[idx].timestamp> timestamp) {
                --idx;
                break;
            } else if (r->entries[idx].timestamp == timestamp) {
                break;
            }
        }
        if (idx < 0) {
            idx = 0;
        }
        offset = r->entries[idx].offset;
    }

    ROE(jls_raw_chunk_seek(self->raw, offset));
    return 0;
}

int32_t jls_core_repair_fsr(struct jls_core_s * self, uint16_t signal_id) {
    ROE(jls_core_signal_validate_typed(self, signal_id, JLS_SIGNAL_TYPE_FSR));
    struct jls_core_signal_s * signal_info = &self->signal_info[signal_id];
    signal_info->parent = self;
    ROE(jls_fsr_open(&signal_info->track_fsr, signal_info));
    struct jls_core_track_s * track = &signal_info->tracks[JLS_SIGNAL_TYPE_FSR];
    track->parent = signal_info;

    // find first non-empty level
    int64_t * offsets = signal_info->tracks[JLS_TRACK_TYPE_FSR].head_offsets;
    int level = JLS_SUMMARY_LEVEL_COUNT - 1;
    for (; (level > 0); --level) {
        if (offsets[level]) {
            if (0 == jls_raw_chunk_seek(self->raw, offsets[level])) {
                break;
            } else {
                offsets[level] = 0;
            }
        }
    }

    int64_t offset_index_next = 0;
    int64_t offset = offsets[level];
    struct jls_core_chunk_s index_head;

    jls_core_fsr_summary_level_alloc(signal_info->track_fsr, level);
    struct jls_core_fsr_level_s * lvl = signal_info->track_fsr->level[level];
    bool skip_summary = false;

    while (level > 0) {
        JLS_LOGI("repair_fsr signal_id %d, level %d, offset %" PRIi64, (int) signal_id, (int) level, offset);

        if (jls_core_rd_chunk(self)) {  // read index
            break;
        }
        index_head = self->chunk_cur;
        memcpy(lvl->index, self->buf->start, self->chunk_cur.hdr.payload_length);

        if (jls_core_rd_chunk(self)) {  // read summary
            break;
        }
        track->index_head[level] = index_head;
        offset_index_next = index_head.hdr.item_next;
        track->summary_head[level] = self->chunk_cur;
        memcpy(lvl->summary, self->buf->start, self->chunk_cur.hdr.payload_length);

        struct jls_fsr_index_s * r = lvl->index;
        if (r->header.entry_size_bits != (sizeof(r->offsets[0]) * 8)) {
            JLS_LOGE("invalid FSR index entry size: %d bits", (int) r->header.entry_size_bits);
            return JLS_ERROR_PARAMETER_INVALID;
        }
        size_t sz = sizeof(r->header) + r->header.entry_count * sizeof(r->offsets[0]);
        if (sz > self->buf->length) {
            JLS_LOGE("invalid payload length");
            return JLS_ERROR_PARAMETER_INVALID;
        }

        jls_raw_seek_end(self->raw);
        if (!skip_summary && jls_core_fsr_summaryN(signal_info->track_fsr, level + 1, offset)) {
            JLS_LOGE("repair_fsr signal_id %d could not create summary - cannot repair this track", (int) signal_id);
        }
        skip_summary = false;

        if ((offset_index_next > 0) && (0 == jls_raw_chunk_seek(self->raw, offset_index_next))) {
            offset = offset_index_next;
        } else {
            skip_summary = true;
            --level;
            if (r->header.entry_count > 0) {
                offset = r->offsets[r->header.entry_count - 1];
                lvl->index->header.entry_count = 0;
                lvl->summary->header.entry_count = 0;
                if (0 != jls_raw_chunk_seek(self->raw, offset)) {
                    JLS_LOGE("Could not seek to lower-level index.  Cannot repair.");
                    break;
                }
            } else {
                JLS_LOGE("Empty index.  Cannot repair.");
                return JLS_ERROR_NOT_SUPPORTED;
            }
        }
    }

    // update level 0 (data)
    jls_core_fsr_sample_buffer_alloc(signal_info->track_fsr);
    while (offset) {
        if (jls_raw_chunk_seek(self->raw, offset) || jls_core_rd_chunk(self)) {
            break;
        }
        memcpy(signal_info->track_fsr->data, self->buf->start, self->buf->length);
        JLS_LOGI("repair_fsr signal_id %d, level %d, offset %" PRIi64 " sample_id %" PRIi64 " to %" PRIi64 " data[0]=%f",
                 (int) signal_id, (int) level, offset,
                 signal_info->track_fsr->data->header.timestamp,
                 signal_info->track_fsr->data->header.timestamp + signal_info->track_fsr->data->header.entry_count,
                 signal_info->track_fsr->data->data[0]);
        signal_info->track_fsr->data_length = signal_info->track_fsr->data->header.entry_count;

        if (!skip_summary && jls_core_fsr_summary1(signal_info->track_fsr, offset)) {
            JLS_LOGW("could not create summary - repair may not work");
        }
        skip_summary = false;
        offset = self->chunk_cur.hdr.item_next;
    }
    jls_core_fsr_sample_buffer_free(signal_info->track_fsr);

    JLS_LOGI("repair_fsr signal_id %d finalizing", (int) signal_id);
    jls_raw_seek_end(self->raw);

    ROE(jls_fsr_close(signal_info->track_fsr));
    signal_info->track_fsr = NULL;
    return 0;
}
