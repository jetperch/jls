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
#define TS_DECIMATE_FACTOR_MIN          (10)
// Signal definition caps, see jls_signal_def_s in format.h.  They bound every
// chunk and buffer size derived from a signal definition well below the
// uint32 payload length and the threaded writer message limit.
#define SAMPLES_PER_DATA_MAX            (1UL << 22)   // 4 Mi samples
#define DATA_SIZE_BYTES_MAX             (1UL << 24)   // 16 MiB sample data per chunk
#define ENTRIES_PER_SUMMARY_MAX         (1UL << 19)   // 16 MiB of f64 summary entries
#define TS_DECIMATE_FACTOR_MAX          (1UL << 16)
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

static inline uint64_t u64_max(uint64_t a, uint64_t b) {
    return (a > b) ? a : b;
}

static uint64_t round_up_to_multiple(uint64_t x, uint64_t m) {
    return ((x + m - 1) / m) * m;
}

// x in [base, base + count) without signed overflow on hostile values.
static inline bool range_contains(int64_t base, int64_t count, int64_t x) {
    return (x >= base) && (count > 0) && (((uint64_t) x - (uint64_t) base) < (uint64_t) count);
}

// On-disk payload size (payload + pad + CRC), matching raw.c.
// todo replace with jls_raw_payload_size_on_disk() once raw.h exports it.
static int64_t raw_payload_size_on_disk(uint32_t payload_size) {
    if (!payload_size) {
        return 0;
    }
    return (((int64_t) payload_size + 4 + 7) / 8) * 8;
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
    if (length > ((SIZE_MAX - sizeof(struct jls_core_f64_buf_s)) / sizeof(double))) {
        return JLS_ERROR_NOT_ENOUGH_MEMORY;  // size would wrap
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

int32_t jls_core_signal_def_validate_fsr(struct jls_signal_def_s const * def) {
    // Structural checks that every reader division, loop, and buffer size
    // relies on.  jls_core_signal_def_align() always produces a passing def.
    uint64_t bits = jls_datatype_parse_size(def->data_type);
    uint64_t spd = def->samples_per_data;
    uint64_t sdf = def->sample_decimate_factor;
    uint64_t eps = def->entries_per_summary;
    uint64_t sumdf = def->summary_decimate_factor;
    if ((0 == bits) || (0 == spd) || (spd > SAMPLES_PER_DATA_MAX)
            || (((spd * bits) / 8) > DATA_SIZE_BYTES_MAX)) {
        JLS_LOGW("signal %d: invalid samples_per_data %" PRIu64, (int) def->signal_id, spd);
        return JLS_ERROR_PARAMETER_INVALID;
    }
    if ((0 == sdf) || (spd % sdf) || ((sdf * bits) % 8)) {
        JLS_LOGW("signal %d: invalid sample_decimate_factor %" PRIu64, (int) def->signal_id, sdf);
        return JLS_ERROR_PARAMETER_INVALID;
    }
    uint64_t entries_per_data = spd / sdf;
    if ((0 == eps) || (eps > ENTRIES_PER_SUMMARY_MAX) || (eps % entries_per_data)) {
        JLS_LOGW("signal %d: invalid entries_per_summary %" PRIu64, (int) def->signal_id, eps);
        return JLS_ERROR_PARAMETER_INVALID;
    }
    if ((sumdf < 2) || (eps % sumdf)) {
        JLS_LOGW("signal %d: invalid summary_decimate_factor %" PRIu64, (int) def->signal_id, sumdf);
        return JLS_ERROR_PARAMETER_INVALID;
    }
    return 0;
}

int64_t jls_core_fsr_step_size(struct jls_signal_def_s const * def, uint8_t level) {
    if (0 == level) {
        return 1;
    }
    // saturate: hostile head offsets can request levels the file never reached
    int64_t step = def->sample_decimate_factor;
    for (uint8_t lvl = 2; lvl <= level; ++lvl) {
        if (step > (INT64_MAX / (int64_t) def->summary_decimate_factor)) {
            return INT64_MAX;
        }
        step *= def->summary_decimate_factor;
    }
    return step;
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
        case 24: d = &SIGNAL_32_DEFAULTS; break;
        case 32: d = &SIGNAL_32_DEFAULTS; break;
        case 64: d = &SIGNAL_64_DEFAULTS; break;
        default: d = NULL; break;
    }

    if (d) {
        SIGNAL_DEF_DEFAULT(samples_per_data);
        SIGNAL_DEF_DEFAULT(sample_decimate_factor);
        SIGNAL_DEF_DEFAULT(entries_per_summary);
        SIGNAL_DEF_DEFAULT(summary_decimate_factor);
    }

    // common parameters, for all data types
    d = &SIGNAL_32_DEFAULTS;
    SIGNAL_DEF_DEFAULT(annotation_decimate_factor);
    SIGNAL_DEF_DEFAULT(utc_decimate_factor);
}

static int32_t ts_decimate_factor_align(uint16_t signal_id, const char * name, uint32_t * factor) {
    if (*factor < TS_DECIMATE_FACTOR_MIN) {
        JLS_LOGI("%s adjusted from %" PRIu32 " to %d", name, *factor, TS_DECIMATE_FACTOR_MIN);
        *factor = TS_DECIMATE_FACTOR_MIN;
    } else if (*factor > TS_DECIMATE_FACTOR_MAX) {
        JLS_LOGW("signal %d: %s %" PRIu32 " too big", (int) signal_id, name, *factor);
        return JLS_ERROR_PARAMETER_INVALID;
    }
    return 0;
}

int32_t jls_core_signal_def_align(struct jls_signal_def_s * def) {
    signal_def_defaults(def);
    uint8_t sample_size = jls_datatype_parse_size(def->data_type);
    if (0 == sample_size) {
        JLS_LOGW("signal %d: invalid data type 0x%08" PRIx32, (int) def->signal_id, def->data_type);
        return JLS_ERROR_PARAMETER_INVALID;
    }
    uint64_t samples_per_data_multiple = (SAMPLE_SIZE_BYTES_MAX * 8) / sample_size;

    // 64-bit math: user values near UINT32_MAX must not wrap to 0
    uint64_t sample_decimate_factor = u64_max(def->sample_decimate_factor, SAMPLE_DECIMATE_FACTOR_MIN);
    sample_decimate_factor = round_up_to_multiple(sample_decimate_factor, samples_per_data_multiple);

    uint64_t samples_per_data = u64_max(def->samples_per_data, SAMPLES_PER_DATA_MIN);
    uint64_t entries_per_summary = u64_max(def->entries_per_summary, ENTRIES_PER_SUMMARY_MIN);
    uint64_t summary_decimate_factor = u64_max(def->summary_decimate_factor, SUMMARY_DECIMATE_FACTOR_MIN);

    entries_per_summary = round_up_to_multiple(entries_per_summary, summary_decimate_factor);
    samples_per_data = round_up_to_multiple(samples_per_data, sample_decimate_factor);
    if ((samples_per_data > SAMPLES_PER_DATA_MAX) || (entries_per_summary > ENTRIES_PER_SUMMARY_MAX)) {
        JLS_LOGW("signal %d: samples_per_data %" PRIu64 " or entries_per_summary %" PRIu64 " too big",
                 (int) def->signal_id, samples_per_data, entries_per_summary);
        return JLS_ERROR_PARAMETER_INVALID;
    }
    uint64_t entries_per_data = samples_per_data / sample_decimate_factor;

    while (entries_per_summary % entries_per_data) {
        // reduce until fits.
        --entries_per_data;
    }

    samples_per_data = sample_decimate_factor * entries_per_data;

    if (sample_decimate_factor != def->sample_decimate_factor) {
        JLS_LOGI("sample_decimate_factor adjusted from %" PRIu32 " to %" PRIu64,
                def->sample_decimate_factor, sample_decimate_factor);
    }
    if (samples_per_data != def->samples_per_data) {
        JLS_LOGI("samples_per_data adjusted from %" PRIu32 " to %" PRIu64,
                def->samples_per_data, samples_per_data);
    }
    if (entries_per_summary != def->entries_per_summary) {
        JLS_LOGI("entries_per_summary adjusted from %" PRIu32 " to %" PRIu64,
                def->entries_per_summary, entries_per_summary);
    }
    ROE(ts_decimate_factor_align(def->signal_id, "annotation_decimate_factor", &def->annotation_decimate_factor));
    ROE(ts_decimate_factor_align(def->signal_id, "utc_decimate_factor", &def->utc_decimate_factor));

    def->sample_decimate_factor = (uint32_t) sample_decimate_factor;
    def->samples_per_data = (uint32_t) samples_per_data;
    def->entries_per_summary = (uint32_t) entries_per_summary;
    def->summary_decimate_factor = (uint32_t) summary_decimate_factor;
    return jls_core_signal_def_validate_fsr(def);
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
        return JLS_ERROR_NOT_FOUND;
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
    if (!jls_core_track_applicable(signal_def->signal_type, track_type)) {
        JLS_LOGW("unsupported track %d for signal type %d",
                 (int) track_type, (int) signal_def->signal_type);
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
    // at most one grow: the header gives the exact on-disk size
    for (int attempt = 0; attempt < 2; ++attempt) {
        self->chunk_cur.offset = jls_raw_chunk_tell(self->raw);
        size_t alloc_size = self->buf->alloc_size;
        uint32_t rd_max = (alloc_size > UINT32_MAX) ? UINT32_MAX : (uint32_t) alloc_size;
        int32_t rc = jls_raw_rd(self->raw, &self->chunk_cur.hdr, rd_max, self->buf->start);
        if (rc == JLS_ERROR_TOO_BIG) {
            if (attempt) {
                break;
            }
            int64_t sz = raw_payload_size_on_disk(self->chunk_cur.hdr.payload_length);
            int64_t fend = jls_raw_backend(self->raw)->fend;
            if ((self->chunk_cur.offset + (int64_t) sizeof(struct jls_chunk_header_s) + sz) > fend) {
                // the read would fail anyway: never allocate for a payload past EOF
                JLS_LOGW("chunk at %" PRIi64 " payload past end of file", self->chunk_cur.offset);
                return JLS_ERROR_IO;
            }
            if ((uint64_t) sz > SIZE_MAX) {
                return JLS_ERROR_NOT_ENOUGH_MEMORY;
            }
            ROE(jls_buf_realloc(self->buf, (size_t) sz));
        } else if (rc == 0) {
            self->buf->cur = self->buf->start;
            self->buf->length = self->chunk_cur.hdr.payload_length;
            self->buf->end = self->buf->start + self->buf->length;
            return 0;
        } else {
            return rc;
        }
    }
    JLS_LOGW("chunk at %" PRIi64 " too big after realloc", self->chunk_cur.offset);
    return JLS_ERROR_TOO_BIG;
}

int32_t jls_core_rd_chunk_validate(struct jls_core_s * self, uint8_t tag, uint16_t chunk_meta) {
    ROE(jls_core_rd_chunk(self));
    if (self->chunk_cur.hdr.tag != tag) {
        JLS_LOGW("chunk offset=%" PRIi64 " tag %d != expected %d",
                 self->chunk_cur.offset, (int) self->chunk_cur.hdr.tag, (int) tag);
        return JLS_ERROR_MESSAGE_INTEGRITY;
    }
    if (self->chunk_cur.hdr.chunk_meta != chunk_meta) {
        JLS_LOGW("chunk offset=%" PRIi64 " meta 0x%04x != expected 0x%04x",
                 self->chunk_cur.offset, (unsigned int) self->chunk_cur.hdr.chunk_meta,
                 (unsigned int) chunk_meta);
        return JLS_ERROR_MESSAGE_INTEGRITY;
    }
    return 0;
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
        // include i == 0: the chunk at the window start is a valid candidate
        // signed math: length may be smaller than a chunk header near file start
        for (int64_t i = (length - (int64_t) sizeof(struct jls_chunk_header_s)) / (int64_t) sizeof(uint64_t);
                i >= 0; --i) {
            h = (struct jls_chunk_header_s *) &data[i];
            uint32_t crc32 = jls_crc32c_hdr(h);
            if (crc32 == h->crc32) {
                int64_t pos_final = pos + i * sizeof(uint64_t);
                if (0 == pos_final) {
                    continue;  // file header, not a chunk
                }
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


// The chain class of a chunk: chunks link item_prev/item_next only within a class.
static uint32_t chunk_chain_class(const struct jls_chunk_header_s * hdr) {
    if (hdr->tag & JLS_TRACK_TAG_FLAG) {
        switch (jls_core_tag_parse_track_chunk(hdr->tag)) {
            case JLS_TRACK_CHUNK_DEF:  // intentional fall-through
            case JLS_TRACK_CHUNK_HEAD: return JLS_TAG_SIGNAL_DEF;
            default: return (((uint32_t) hdr->chunk_meta) << 8) | hdr->tag;  // per signal, track & level
        }
    }
    return hdr->tag;  // SOURCE_DEF, SIGNAL_DEF, USER_DATA
}

static bool chunk_same_chain(const struct jls_chunk_header_s * a, const struct jls_chunk_header_s * b) {
    return chunk_chain_class(a) == chunk_chain_class(b);
}

int32_t jls_core_truncate_tail(struct jls_core_s * self, int64_t pos, struct jls_core_chunk_s * tail) {
    // remove the partial write after the last valid chunk
    ROE(jls_raw_chunk_seek(self->raw, pos));
    ROE(jls_core_rd_chunk(self));
    ROE(jls_bk_truncate(jls_raw_backend(self->raw)));

    // rewrite the last valid chunk so subsequent appends link correctly
    ROE(jls_raw_chunk_seek(self->raw, pos));
    ROE(jls_raw_wr(self->raw, &self->chunk_cur.hdr, self->buf->cur));
    *tail = self->chunk_cur;
    tail->offset = pos;

    // relink an orphan final chunk: a crash between writing a chunk and
    // updating its predecessor's item_next leaves the chunk unreachable
    int64_t tail_prev = (int64_t) tail->hdr.item_prev;
    if (tail_prev && (tail_prev < pos)
            && (0 == jls_raw_chunk_seek(self->raw, tail_prev))
            && (0 == jls_core_rd_chunk(self))
            && chunk_same_chain(&self->chunk_cur.hdr, &tail->hdr)
            && (0 == self->chunk_cur.hdr.item_next)) {
        JLS_LOGI("relink orphan final chunk at %" PRIi64, pos);
        self->chunk_cur.hdr.item_next = (uint64_t) pos;
        ROE(jls_core_update_chunk_header(self, &self->chunk_cur));
    }
    return 0;
}

int32_t jls_core_scan_sources(struct jls_core_s * self) {
    JLS_LOGD1("jls_core_scan_sources");
    if (0 == self->source_head.offset) {
        // truncated before the first write, or defs region damaged
        JLS_LOGW("no source definitions found");
        return self->recovery_active ? 0 : JLS_ERROR_MESSAGE_INTEGRITY;
    }
    struct jls_core_chunk_s prev = {.offset = 0};
    ROE(jls_raw_chunk_seek(self->raw, self->source_head.offset));
    while (1) {
        int32_t rc = jls_core_rd_chunk(self);
        if (JLS_ERROR_EMPTY == rc) {
            if (self->recovery_active) {
                // chain reference into the truncated tail: clamp so the
                // repaired file passes the strict (clean) scans
                JLS_LOGW("source chain ends at truncation");
                if (prev.offset) {
                    prev.hdr.item_next = 0;
                    ROE(jls_core_update_chunk_header(self, &prev));
                }
                return 0;
            }
            return JLS_ERROR_MESSAGE_INTEGRITY;
        } else if (rc) {
            return rc;  // interior damage: offline jls_repair required
        }
        if (self->chunk_cur.hdr.tag != JLS_TAG_SOURCE_DEF) {
            JLS_LOGW("source chain: unexpected tag %d at %" PRIi64,
                     (int) self->chunk_cur.hdr.tag, self->chunk_cur.offset);
            return JLS_ERROR_MESSAGE_INTEGRITY;
        }
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
        prev = self->chunk_cur;
        if (!self->chunk_cur.hdr.item_next) {
            break;
        }
        if ((int64_t) self->chunk_cur.hdr.item_next <= self->chunk_cur.offset) {
            // chains always point forward
            JLS_LOGW("source chain: invalid item_next at %" PRIi64, self->chunk_cur.offset);
            return JLS_ERROR_MESSAGE_INTEGRITY;
        }
        ROE(jls_raw_chunk_seek(self->raw, self->chunk_cur.hdr.item_next));
    }
    return 0;
}

static int32_t handle_signal_def(struct jls_core_s * self) {
    uint16_t signal_id = self->chunk_cur.hdr.chunk_meta;
    if (signal_id >= JLS_SIGNAL_COUNT) {
        JLS_LOGW("signal_id %d too big - skip", (int) signal_id);
        return 0;  // semantically invalid but structurally sound: skip
    }
    struct jls_core_signal_s * signal_info = &self->signal_info[signal_id];
    if (signal_info->chunk_def.offset) {
        // keep the first: a later def must not retype a signal already in use
        JLS_LOGW("duplicate signal %d def at %" PRIi64 " - skip", (int) signal_id, self->chunk_cur.offset);
        return 0;
    }
    // parse into a temporary: commit only a fully validated definition
    struct jls_signal_def_s s;
    memset(&s, 0, sizeof(s));
    ROE(jls_buf_rd_u16(self->buf, &s.source_id));
    ROE(jls_buf_rd_u8(self->buf, &s.signal_type));
    ROE(jls_buf_rd_skip(self->buf, 1));
    ROE(jls_buf_rd_u32(self->buf, &s.data_type));
    ROE(jls_buf_rd_u32(self->buf, &s.sample_rate));
    ROE(jls_buf_rd_u32(self->buf, &s.samples_per_data));
    ROE(jls_buf_rd_u32(self->buf, &s.sample_decimate_factor));
    ROE(jls_buf_rd_u32(self->buf, &s.entries_per_summary));
    ROE(jls_buf_rd_u32(self->buf, &s.summary_decimate_factor));
    ROE(jls_buf_rd_u32(self->buf, &s.annotation_decimate_factor));
    ROE(jls_buf_rd_u32(self->buf, &s.utc_decimate_factor));
    ROE(jls_buf_rd_skip(self->buf, 92));
    ROE(jls_buf_rd_str(self->buf, (const char **) &s.name));
    ROE(jls_buf_rd_str(self->buf, (const char **) &s.units));
    s.signal_id = signal_id;
    if (jls_core_signal_def_validate(&s)
            || ((JLS_SIGNAL_TYPE_FSR == s.signal_type) && jls_core_signal_def_validate_fsr(&s))) {
        JLS_LOGW("Signal validation failed for %d : %s - skip", (int) signal_id, s.name);
        return 0;
    }
    signal_info->chunk_def = self->chunk_cur;
    signal_info->parent = self;
    signal_info->signal_def = s;  // signal_id match indicates that this signal is valid
    JLS_LOGD1("Found signal %d : %s", (int) signal_id, s.name);
    return 0;
}

static int32_t handle_track_def(struct jls_core_s * self, int64_t pos) {
    (void) pos;  // unused
    uint16_t signal_id = self->chunk_cur.hdr.chunk_meta & SIGNAL_MASK;
    if (jls_core_signal_validate(self, signal_id)) {
        JLS_LOGW("track def for undefined signal %d - skip", (int) signal_id);
        return 0;  // the signal def was skipped: skip its tracks too
    }
    ROE(jls_core_validate_track_tag(self, signal_id, self->chunk_cur.hdr.tag));
    return 0;
}

static int32_t handle_track_head(struct jls_core_s * self, int64_t pos) {
    (void) pos;  // unused
    uint16_t signal_id = self->chunk_cur.hdr.chunk_meta & SIGNAL_MASK;
    if (jls_core_signal_validate(self, signal_id)) {
        JLS_LOGW("track head for undefined signal %d - skip", (int) signal_id);
        return 0;  // the signal def was skipped: skip its tracks too
    }
    ROE(jls_core_validate_track_tag(self, signal_id, self->chunk_cur.hdr.tag));
    uint8_t track_type = jls_core_tag_parse_track_type(self->chunk_cur.hdr.tag);
    size_t expect_sz = JLS_SUMMARY_LEVEL_COUNT * sizeof(int64_t);

    if (self->buf->length != expect_sz) {
        // torn head payload: offline jls_repair can rebuild it
        JLS_LOGW("cannot parse signal %d head, sz=%zu, expect=%zu",
                 (int) signal_id, self->buf->length, expect_sz);
        return JLS_ERROR_MESSAGE_INTEGRITY;
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
    if (0 == self->signal_head.offset) {
        // truncated before the first write, or defs region damaged
        JLS_LOGW("no signal definitions found");
        return self->recovery_active ? 0 : JLS_ERROR_MESSAGE_INTEGRITY;
    }
    struct jls_core_chunk_s prev = {.offset = 0};
    ROE(jls_raw_chunk_seek(self->raw, self->signal_head.offset));
    while (1) {
        int32_t rc = jls_core_rd_chunk(self);
        if (JLS_ERROR_EMPTY == rc) {
            if (self->recovery_active) {
                // chain reference into the truncated tail: clamp so the
                // repaired file passes the strict (clean) scans
                JLS_LOGW("signal chain ends at truncation");
                if (prev.offset) {
                    prev.hdr.item_next = 0;
                    ROE(jls_core_update_chunk_header(self, &prev));
                }
                return 0;
            }
            return JLS_ERROR_MESSAGE_INTEGRITY;
        } else if (rc) {
            return rc;  // interior damage: offline jls_repair required
        }
        if (self->chunk_cur.hdr.tag == JLS_TAG_SIGNAL_DEF) {
            rc = handle_signal_def(self);
        } else if ((self->chunk_cur.hdr.tag & 7) == JLS_TRACK_CHUNK_DEF) {
            rc = handle_track_def(self, self->chunk_cur.offset);
        } else if ((self->chunk_cur.hdr.tag & 7) == JLS_TRACK_CHUNK_HEAD) {
            rc = handle_track_head(self, self->chunk_cur.offset);
        } else {
            JLS_LOGW("unknown tag %d in signal list", (int) self->chunk_cur.hdr.tag);
            rc = JLS_ERROR_MESSAGE_INTEGRITY;
        }
        if (rc) {
            return JLS_ERROR_MESSAGE_INTEGRITY;
        }
        prev = self->chunk_cur;
        if (!self->chunk_cur.hdr.item_next) {
            break;
        }
        if ((int64_t) self->chunk_cur.hdr.item_next <= self->chunk_cur.offset) {
            // chains always point forward
            JLS_LOGW("signal chain: invalid item_next at %" PRIi64, self->chunk_cur.offset);
            return JLS_ERROR_MESSAGE_INTEGRITY;
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
        if (jls_raw_chunk_seek(self->raw, offset)
                || jls_core_rd_chunk(self)
                || (self->chunk_cur.hdr.tag != JLS_TAG_TRACK_FSR_DATA)
                || (self->chunk_cur.hdr.chunk_meta != signal_id)
                || (self->buf->length < sizeof(struct jls_fsr_data_s))) {
            // interior damage: offline jls_repair required
            JLS_LOGW("jls_core_scan_fsr_sample_id signal %d: data head unreadable", (int) signal_id);
            return JLS_ERROR_MESSAGE_INTEGRITY;
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
        // compute the step size in samples between each index entry:
        // one data chunk at level 1, else one lower-level summary chunk.
        int64_t step_size = signal_def->samples_per_data;
        if (lvl > 1) {
            step_size = jls_core_fsr_step_size(signal_def, (uint8_t) (lvl - 1));
            if (step_size > (INT64_MAX / signal_def->entries_per_summary)) {
                step_size = INT64_MAX;
            } else {
                step_size *= signal_def->entries_per_summary;
            }
        }
        JLS_LOGD3("signal %d, level %d, offset=%" PRIi64 ", step_size=%" PRIi64,
                 (int) signal_id, lvl, offset, step_size);
        ROE(jls_raw_chunk_seek(self->raw, offset));
        ROE(jls_core_rd_chunk_validate(self, JLS_TAG_TRACK_FSR_INDEX,
                                       (uint16_t) ((signal_id & SIGNAL_MASK) | (lvl << 12))));

        struct jls_fsr_index_s * r = (struct jls_fsr_index_s *) self->buf->start;
        if ((self->buf->length < sizeof(r->header))
                || ((sizeof(r->header) + (uint64_t) r->header.entry_count * sizeof(r->offsets[0]))
                    > self->buf->length)) {
            JLS_LOGE("invalid payload length");
            return JLS_ERROR_MESSAGE_INTEGRITY;
        }
        int64_t chunk_timestamp = r->header.timestamp;
        int64_t chunk_entries = r->header.entry_count;
        JLS_LOGD3("timestamp=%" PRIi64 ", entries=%" PRIi64, chunk_timestamp, chunk_entries);

        int64_t idx = -1;
        if (sample_id >= chunk_timestamp) {
            idx = (int64_t) (((uint64_t) sample_id - (uint64_t) chunk_timestamp) / (uint64_t) step_size);
        }
        if ((idx < 0) || (idx >= chunk_entries)) {
            JLS_LOGE("invalid index signal %d, level %d, sample_id=%"
                     PRIi64 " offset=%" PRIi64 ": %" PRIi64 " >= %" PRIi64,
                     (int) signal_id, lvl, sample_id,
                     offset, idx, chunk_entries);
            return JLS_ERROR_MESSAGE_INTEGRITY;
        }
        offset = r->offsets[idx];
    }

    ROE(jls_raw_chunk_seek(self->raw, offset));
    return 0;
}

int32_t jls_core_fsr_length(struct jls_core_s * self, uint16_t signal_id, int64_t * samples) {
    ROE(jls_core_signal_validate_typed(self, signal_id, JLS_SIGNAL_TYPE_FSR));
    if (!samples) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    int64_t * signal_length = &self->signal_info[signal_id].track_fsr->signal_length;
    struct jls_signal_def_s * signal_def = &self->signal_info[signal_id].signal_def;
    if (*signal_length >= 0) {
        *samples = *signal_length;
        return 0;
    }

    // length is not store explicitly
    // traverse to last entry of each level to then reach last data block.
    int64_t offset = 0;
    int64_t length = 0;
    // unsigned math: hostile timestamps must not overflow, checked below
    uint64_t offset_u64 = (uint64_t) signal_def->sample_id_offset;
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
        ROE(jls_core_rd_chunk_validate(self, JLS_TAG_TRACK_FSR_INDEX,
                                       (uint16_t) ((signal_id & SIGNAL_MASK) | (lvl << 12))));

        r = (struct jls_fsr_index_s *) self->buf->start;
        if (self->buf->length < sizeof(r->header)) {
            JLS_LOGE("invalid payload length");
            return JLS_ERROR_MESSAGE_INTEGRITY;
        }
        if (r->header.entry_size_bits != (sizeof(r->offsets[0]) * 8)) {
            JLS_LOGE("invalid FSR index entry size: %d bits", (int) r->header.entry_size_bits);
            return JLS_ERROR_MESSAGE_INTEGRITY;
        }
        uint64_t sz = sizeof(r->header) + (uint64_t) r->header.entry_count * sizeof(r->offsets[0]);
        if (sz > self->buf->length) {
            JLS_LOGE("invalid payload length");
            return JLS_ERROR_MESSAGE_INTEGRITY;
        }
        if (r->header.entry_count > 0) {
            offset = r->offsets[r->header.entry_count - 1];
        }

        // only valid for level 1 index
        if (lvl == 1) {
            ROE(jls_core_rd_chunk_validate(self, JLS_TAG_TRACK_FSR_SUMMARY,
                                           (uint16_t) ((signal_id & SIGNAL_MASK) | (1 << 12)))); // summary
            struct jls_fsr_f32_summary_s * s = (struct jls_fsr_f32_summary_s *) self->buf->start;
            if (self->buf->length < sizeof(s->header)) {
                JLS_LOGE("invalid payload length");
                return JLS_ERROR_MESSAGE_INTEGRITY;
            }
            length = (int64_t) ((uint64_t) s->header.timestamp
                    + (uint64_t) s->header.entry_count * signal_def->sample_decimate_factor
                    - offset_u64);
        }
    }

    if (offset) {
        ROE(jls_raw_chunk_seek(self->raw, offset));
        ROE(jls_core_rd_chunk_validate(self, JLS_TAG_TRACK_FSR_DATA, signal_id));
        struct jls_fsr_data_s * d = (struct jls_fsr_data_s *) self->buf->start;
        if (self->buf->length < sizeof(d->header)) {
            JLS_LOGE("invalid payload length");
            return JLS_ERROR_MESSAGE_INTEGRITY;
        }
        length = (int64_t) ((uint64_t) d->header.timestamp + d->header.entry_count - offset_u64);
    }
    if (length < 0) {
        JLS_LOGW("signal %d: invalid length %" PRIi64, (int) signal_id, length);
        return JLS_ERROR_MESSAGE_INTEGRITY;
    }
    *signal_length = length;
    *samples = length;
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
        if ((i + 1) < count) {
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
        if ((i + 1) < count) {
            y[i + 1] = mean + g * sin(f2);
        }
    }
}

static int32_t clamp_round(double v, int32_t v_min, int32_t v_max) {
    if (isnan(v)) {
        return 0;
    }
    v = round(v);  // compare before the cast: out-of-range casts are UB
    if (v < v_min) {
        return v_min;
    } else if (v > v_max) {
        return v_max;
    }
    return (int32_t) v;
}

// The fill byte for an omitted chunk of constant samples with this mean.
static int32_t omitted_fill_byte(uint32_t data_type, double mean, uint8_t * byte) {
    int32_t v;
    switch (data_type & 0xffff) {
        case JLS_DATATYPE_U1:
            *byte = clamp_round(mean, 0, 1) ? 0xff : 0x00;
            break;
        case JLS_DATATYPE_U4:
            v = clamp_round(mean, 0, 15);
            *byte = (uint8_t) (v | (v << 4));
            break;
        case JLS_DATATYPE_I4:
            v = clamp_round(mean, -8, 7) & 0x0f;  // two's complement nibble
            *byte = (uint8_t) (v | (v << 4));
            break;
        case JLS_DATATYPE_U8:
            *byte = (uint8_t) clamp_round(mean, 0, 255);
            break;
        case JLS_DATATYPE_I8:
            *byte = (uint8_t) (clamp_round(mean, -128, 127) & 0xff);
            break;
        default:
            return JLS_ERROR_NOT_SUPPORTED;  // the writer only omits <= 8-bit types
    }
    return 0;
}

static int32_t reconstruct_omitted_chunk(struct jls_core_s * self, uint16_t signal_id, int64_t start_sample_id) {
    // jls_core_rd_fsr_level1() validated rd_index & rd_summary, and
    // jls_core_rd_fsr_data0() verified that the index covers start_sample_id.
    struct jls_signal_def_s * signal_def = &self->signal_info[signal_id].signal_def;
    uint8_t sample_size_bits = jls_datatype_parse_size(signal_def->data_type);
    const uint64_t samples_per_data = signal_def->samples_per_data;
    const uint64_t sz_samples = signal_def->sample_decimate_factor;
    const uint64_t entries_per_data = samples_per_data / sz_samples;

    struct jls_fsr_index_s * r = (struct jls_fsr_index_s *) self->rd_index->start;
    uint64_t t_index = ((uint64_t) start_sample_id - (uint64_t) r->header.timestamp) / samples_per_data;
    int64_t sample_id = (int64_t) ((uint64_t) r->header.timestamp + t_index * samples_per_data);

    // summary and index share the timestamp, so the entries align
    struct jls_fsr_f32_summary_s * s32 = (struct jls_fsr_f32_summary_s *) self->rd_summary->start;
    struct jls_fsr_f64_summary_s * s64 = (struct jls_fsr_f64_summary_s *) self->rd_summary->start;
    bool is_summary_64 = (s32->header.entry_size_bits == (4 * sizeof(double) * 8));
    uint64_t s_index = t_index * entries_per_data;
    if (s_index >= s32->header.entry_count) {
        JLS_LOGW("omitted chunk at %" PRIi64 " has no summary", sample_id);
        return JLS_ERROR_MESSAGE_INTEGRITY;
    }
    uint64_t s_count = s32->header.entry_count - s_index;
    if (s_count > entries_per_data) {
        s_count = entries_per_data;
    }
    uint64_t entry_bytes = s32->header.entry_size_bits / 8;
    if ((sizeof(s32->header) + (s_index + s_count) * entry_bytes) > self->rd_summary->length) {
        JLS_LOGW("omitted chunk at %" PRIi64 ": summary payload too short", sample_id);
        return JLS_ERROR_MESSAGE_INTEGRITY;
    }

    // signal_def validation caps this size far below SIZE_MAX
    size_t sz = (size_t) ((samples_per_data * sample_size_bits) / 8 + sizeof(struct jls_fsr_data_s));
    ROE(jls_buf_realloc(self->buf, sz));
    if (self->buf->alloc_size < sz) {
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }

    struct jls_fsr_data_s * data = (struct jls_fsr_data_s *) self->buf->start;
    data->header.entry_count = 0;
    data->header.timestamp = sample_id;
    data->header.entry_size_bits = sample_size_bits;
    data->header.rsv16 = 0;
    uint8_t * d = (uint8_t *) data->data;
    const uint64_t sz_bytes = (sz_samples * sample_size_bits) / 8;  // exact: validated
    const uint32_t data_type = signal_def->data_type & 0xffff;

    for (uint64_t k = 0; k < s_count; ++k, ++s_index) {
        double mu64;
        double std64;
        if (is_summary_64) {
            mu64 = s64->data[s_index][JLS_SUMMARY_FSR_MEAN];
            std64 = s64->data[s_index][JLS_SUMMARY_FSR_STD];
        } else {
            mu64 = s32->data[s_index][JLS_SUMMARY_FSR_MEAN];
            std64 = s32->data[s_index][JLS_SUMMARY_FSR_STD];
        }

        if (data_type == JLS_DATATYPE_F32) {
            construct_f32(sample_id + k * sz_samples, (float *) d, sz_samples, (float) mu64, (float) std64);
        } else if (data_type == JLS_DATATYPE_F64) {
            construct_f64(sample_id + k * sz_samples, (double *) d, sz_samples, mu64, std64);
        } else {
            uint8_t value;
            if (omitted_fill_byte(signal_def->data_type, mu64, &value)) {
                JLS_LOGW("omitted data not supported for data type 0x%08" PRIx32, signal_def->data_type);
                return JLS_ERROR_MESSAGE_INTEGRITY;
            }
            memset(d, value, sz_bytes);
        }
        d += sz_bytes;
        data->header.entry_count += (uint32_t) sz_samples;
    }
    self->buf->cur = self->buf->start;
    self->buf->length = sizeof(struct jls_fsr_data_s) + ((uint64_t) data->header.entry_count * sample_size_bits) / 8;
    self->buf->end = self->buf->start + self->buf->length;
    return 0;
}

// The minimum FSR summary payload length.  wr_fsr.c wr_summary() sizes the
// payload with the f32 entry layout, so files store only 16 bytes per entry
// even for 64-bit summaries.  Readers must still bound 64-bit entry
// access by the actual payload length.
static uint64_t summary_payload_min(uint32_t entry_count) {
    return sizeof(struct jls_payload_header_s) + (uint64_t) entry_count * JLS_SUMMARY_FSR_COUNT * sizeof(float);
}

// Validate the level-1 index and summary payloads before caching them.
static int32_t rd_fsr_level1_validate(struct jls_core_s * self, struct jls_signal_def_s * signal_def) {
    struct jls_fsr_index_s * idx = (struct jls_fsr_index_s *) self->rd_index->start;
    struct jls_fsr_f32_summary_s * s = (struct jls_fsr_f32_summary_s *) self->rd_summary->start;
    uint32_t entries_per_data = signal_def->samples_per_data / signal_def->sample_decimate_factor;
    uint32_t summary_bits = JLS_SUMMARY_FSR_COUNT * jls_core_fsr_summary_entry_size_bits(signal_def->data_type);
    if ((self->rd_index->length < sizeof(idx->header))
            || (idx->header.entry_size_bits != (sizeof(idx->offsets[0]) * 8))
            || (0 == idx->header.entry_count)
            || (idx->header.entry_count > (signal_def->entries_per_summary / entries_per_data))
            || ((sizeof(idx->header) + (uint64_t) idx->header.entry_count * sizeof(idx->offsets[0]))
                > self->rd_index->length)) {
        JLS_LOGW("invalid level 1 index chunk at %" PRIi64, self->rd_index_chunk.offset);
        return JLS_ERROR_MESSAGE_INTEGRITY;
    }
    if ((self->rd_summary->length < sizeof(s->header))
            || (s->header.entry_size_bits != summary_bits)
            || (s->header.entry_count > signal_def->entries_per_summary)
            || (summary_payload_min(s->header.entry_count) > self->rd_summary->length)
            || (s->header.timestamp != idx->header.timestamp)) {
        JLS_LOGW("invalid level 1 summary chunk at %" PRIi64, self->rd_summary_chunk.offset);
        return JLS_ERROR_MESSAGE_INTEGRITY;
    }
    return 0;
}

int32_t jls_core_rd_fsr_level1(struct jls_core_s * self, uint16_t signal_id, int64_t start_sample_id) {
    struct jls_signal_def_s * signal_def = &self->signal_info[signal_id].signal_def;
    uint16_t meta = (uint16_t) ((1 << 12) | (signal_id & SIGNAL_MASK));

    if (self->rd_index_chunk.hdr.chunk_meta != meta) {
        self->rd_index_chunk.offset = 0;
    } else if (self->rd_index_chunk.offset) {
        struct jls_fsr_index_s * idx = (struct jls_fsr_index_s *) self->rd_index->start;
        int64_t samples = (int64_t) idx->header.entry_count * signal_def->samples_per_data;
        if (range_contains(idx->header.timestamp, samples, start_sample_id)) {
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
    if (self->chunk_cur.hdr.tag == JLS_TAG_TRACK_FSR_DATA) {
        // no level 1 index: the seek found the first data chunk
        self->rd_index_chunk.offset = 0;
        self->rd_summary_chunk.offset = 0;
        if (self->chunk_cur.hdr.chunk_meta != signal_id) {
            JLS_LOGW("unexpected chunk meta: %d (expected %d)", (int) self->chunk_cur.hdr.chunk_meta, signal_id);
            return JLS_ERROR_MESSAGE_INTEGRITY;
        }
        return 0;
    }
    if ((self->chunk_cur.hdr.tag != JLS_TAG_TRACK_FSR_INDEX) || (self->chunk_cur.hdr.chunk_meta != meta)) {
        JLS_LOGW("chunk offset=%" PRIi64 " is not a level 1 index", self->chunk_cur.offset);
        return JLS_ERROR_MESSAGE_INTEGRITY;
    }
    ROE(jls_buf_copy(self->rd_index, self->buf));
    struct jls_core_chunk_s index_chunk = self->chunk_cur;

    ROE(jls_core_rd_chunk_validate(self, JLS_TAG_TRACK_FSR_SUMMARY, meta));  // summary
    ROE(jls_buf_copy(self->rd_summary, self->buf));
    self->rd_summary_chunk = self->chunk_cur;
    self->rd_index_chunk = index_chunk;
    int32_t rc = rd_fsr_level1_validate(self, signal_def);
    if (rc) {
        self->rd_index_chunk.offset = 0;  // never cache an invalid index
        self->rd_summary_chunk.offset = 0;
    }
    return rc;
}

int32_t jls_core_rd_fsr_data0(struct jls_core_s * self, uint16_t signal_id, int64_t start_sample_id) {
    int64_t offset = 0;
    struct jls_signal_def_s * signal_def = &self->signal_info[signal_id].signal_def;
    uint8_t entry_size_bits = jls_datatype_parse_size(signal_def->data_type);
    ROE(jls_core_rd_fsr_level1(self, signal_id, start_sample_id));
    if ((self->rd_index_chunk.offset == 0) && (self->chunk_cur.hdr.tag == JLS_TAG_TRACK_FSR_DATA)) {
        offset = self->chunk_cur.offset;
    } else {
        struct jls_fsr_index_s * idx = (struct jls_fsr_index_s *) self->rd_index->start;
        int64_t samples = (int64_t) idx->header.entry_count * signal_def->samples_per_data;
        if (!range_contains(idx->header.timestamp, samples, start_sample_id)) {  // corrupt index
            JLS_LOGW("invalid index chunk at %" PRIi64 " for sample_id %" PRIi64,
                     self->rd_index_chunk.offset, start_sample_id);
            self->rd_index_chunk.offset = 0;
            return JLS_ERROR_MESSAGE_INTEGRITY;
        }
        uint64_t idx_entry = ((uint64_t) start_sample_id - (uint64_t) idx->header.timestamp)
                / signal_def->samples_per_data;
        offset = idx->offsets[idx_entry];
    }

    if (0 == offset) {
        ROE(reconstruct_omitted_chunk(self, signal_id, start_sample_id));
    } else if (jls_raw_chunk_seek(self->raw, offset)) {
        return JLS_ERROR_NOT_FOUND;
    } else {
        int32_t rv = jls_core_rd_chunk(self);
        if (rv == JLS_ERROR_EMPTY) {
            return JLS_ERROR_NOT_FOUND;
        } else if (rv) {
            return rv;
        }
        if (self->chunk_cur.hdr.tag != JLS_TAG_TRACK_FSR_DATA) {
            JLS_LOGW("unexpected chunk tag: %d (expected %d)", (int) self->chunk_cur.hdr.tag, JLS_TAG_TRACK_FSR_DATA);
            return JLS_ERROR_MESSAGE_INTEGRITY;
        }
        if (self->chunk_cur.hdr.chunk_meta != signal_id) {
            JLS_LOGW("unexpected chunk meta: %d (expected %d)", (int) self->chunk_cur.hdr.chunk_meta, signal_id);
            return JLS_ERROR_MESSAGE_INTEGRITY;
        }
    }

    struct jls_fsr_data_s * r = (struct jls_fsr_data_s *) self->buf->start;
    if (self->buf->length < sizeof(r->header)) {
        JLS_LOGW("invalid data chunk at %" PRIi64, self->chunk_cur.offset);
        return JLS_ERROR_MESSAGE_INTEGRITY;
    }
    if (r->header.entry_size_bits != entry_size_bits) {
        JLS_LOGE("invalid data entry size: %d", (int) r->header.entry_size_bits);
        return JLS_ERROR_MESSAGE_INTEGRITY;
    }
    uint64_t sz_bytes = ((uint64_t) r->header.entry_count * entry_size_bits + 7) / 8;
    if ((r->header.entry_count > signal_def->samples_per_data)
            || ((sizeof(r->header) + sz_bytes) > self->buf->length)
            || !range_contains(r->header.timestamp, r->header.entry_count, start_sample_id)) {
        // callers index into the chunk at start_sample_id
        JLS_LOGW("invalid data chunk at %" PRIi64 " for sample_id %" PRIi64 ": timestamp=%" PRIi64
                 ", entry_count=%" PRIu32, self->chunk_cur.offset, start_sample_id,
                 r->header.timestamp, r->header.entry_count);
        return JLS_ERROR_MESSAGE_INTEGRITY;
    }
    return 0;
}

// Copy n_bits from src at bit src_bit to dst at bit dst_bit, LSB first.
// Writes only the bytes holding destination bits and clears the bits
// above the copied range in the final destination byte.
static void bits_copy(uint8_t * dst, uint64_t dst_bit, const uint8_t * src, uint64_t src_bit, uint64_t n_bits) {
    dst += dst_bit >> 3;
    dst_bit &= 7;
    src += src_bit >> 3;
    src_bit &= 7;
    while (n_bits) {
        uint32_t take = 8 - (uint32_t) dst_bit;
        if (take > n_bits) {
            take = (uint32_t) n_bits;
        }
        uint32_t v = ((uint32_t) src[0]) >> src_bit;
        if (take > (8 - src_bit)) {
            v |= ((uint32_t) src[1]) << (8 - src_bit);
        }
        v &= (1U << take) - 1;
        uint8_t keep = (uint8_t) ((1U << dst_bit) - 1);
        *dst = (uint8_t) ((*dst & keep) | (v << dst_bit));
        n_bits -= take;
        dst_bit += take;
        if (dst_bit >= 8) {
            dst_bit = 0;
            ++dst;
        }
        src_bit += take;
        src += src_bit >> 3;
        src_bit &= 7;
    }
}

int32_t jls_core_fsr(struct jls_core_s * self, uint16_t signal_id, int64_t start_sample_id,
                     void * data, int64_t data_length) {
    // start_sample_id is API zero-based
    uint8_t * data_u8 = (uint8_t *) data;
    ROE(jls_core_signal_validate_typed(self, signal_id, JLS_SIGNAL_TYPE_FSR));
    int64_t samples = 0;
    ROE(jls_core_fsr_length(self, signal_id, &samples));
    struct jls_signal_def_s * signal_def = &self->signal_info[signal_id].signal_def;
    if (data_length < 0) {
        JLS_LOGW("rd_fsr %d %s: length invalid %" PRIi64, (int) signal_id, signal_def->name, data_length);
        return JLS_ERROR_PARAMETER_INVALID;
    } else if (data_length == 0) {
        return 0;
    } else if (!data) {
        return JLS_ERROR_PARAMETER_INVALID;
    } else if (start_sample_id < 0) {
        JLS_LOGW("rd_fsr %d %s: start_sample_id invalid %" PRIi64 " length=%" PRIi64,
                 (int) signal_id, signal_def->name,
                 start_sample_id, samples);
        return JLS_ERROR_PARAMETER_INVALID;
    }

    const int64_t sample_id_offset = signal_def->sample_id_offset;
    uint8_t entry_size_bits = jls_datatype_parse_size(signal_def->data_type);

    // subtract, never add: start_sample_id + data_length may overflow
    if ((start_sample_id > samples) || (data_length > (samples - start_sample_id))) {
        JLS_LOGW("rd_fsr %d %s: start=%" PRIi64 " length=%" PRIi64 " > %" PRIi64,
                 (int) signal_id, signal_def->name,
                 start_sample_id, data_length, samples);
        return JLS_ERROR_PARAMETER_INVALID;
    }

    // file sample_id; a hostile offset wraps here, then fails the chunk checks
    start_sample_id = (int64_t) ((uint64_t) start_sample_id + (uint64_t) sample_id_offset);
    uint64_t dst_bit = 0;

    while (data_length > 0) {
        // validates that the chunk holds start_sample_id and its entries
        ROE(jls_core_rd_fsr_data0(self, signal_id, start_sample_id));
        struct jls_fsr_data_s * r = (struct jls_fsr_data_s *) self->buf->start;
        int64_t idx = start_sample_id - r->header.timestamp;
        int64_t sz_samples = (int64_t) r->header.entry_count - idx;
        if (sz_samples > data_length) {
            sz_samples = data_length;
        }
        const uint8_t * src = (const uint8_t *) r->data;
        if (entry_size_bits >= 8) {
            size_t sample_bytes = entry_size_bits / 8;
            memcpy(data_u8 + (dst_bit / 8), src + idx * sample_bytes, (size_t) sz_samples * sample_bytes);
        } else {
            bits_copy(data_u8, dst_bit, src, (uint64_t) idx * entry_size_bits,
                      (uint64_t) sz_samples * entry_size_bits);
        }
        dst_bit += (uint64_t) sz_samples * entry_size_bits;
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
        ROE(jls_core_rd_chunk_validate(self, jls_track_tag_pack(track_type, JLS_TRACK_CHUNK_INDEX),
                                       (uint16_t) ((signal_id & SIGNAL_MASK) | (lvl << 12))));

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

int32_t jls_core_repair_chain_walk(struct jls_core_s * self, struct jls_core_chain_walk_s * walk,
                                   int32_t max_hops) {
    while (!walk->done && (max_hops-- > 0)) {
        int64_t offset = walk->offset;
        if (0 == offset) {
            walk->done = true;
            break;
        }
        if (jls_raw_chunk_seek(self->raw, offset)
                || jls_core_rd_chunk(self)
                || (self->chunk_cur.hdr.tag != walk->tag)
                || ((walk->chunk_meta >= 0)
                    && (self->chunk_cur.hdr.chunk_meta != (uint16_t) walk->chunk_meta))) {
            walk->done = true;
            if (0 == walk->prev.offset) {
                return JLS_ERROR_NOT_FOUND;  // first chunk invalid; caller must clear its head
            }
            JLS_LOGI("repair chain tag %d: clamp at %" PRIi64 ", drop invalid link %" PRIi64,
                     (int) walk->tag, walk->prev.offset, offset);
            walk->prev.hdr.item_next = 0;
            return jls_core_update_chunk_header(self, &walk->prev);
        }
        walk->prev = self->chunk_cur;
        int64_t next = (int64_t) walk->prev.hdr.item_next;
        if (next && (next <= offset)) {
            walk->done = true;
            JLS_LOGI("repair chain tag %d: clamp backward link at %" PRIi64,
                     (int) walk->tag, walk->prev.offset);
            walk->prev.hdr.item_next = 0;
            return jls_core_update_chunk_header(self, &walk->prev);
        }
        walk->offset = next;
    }
    return 0;
}

int32_t jls_core_repair_chain(struct jls_core_s * self, int64_t offset, uint8_t tag, int32_t chunk_meta) {
    struct jls_core_chain_walk_s walk = {
        .offset = offset,
        .prev = {.offset = 0},
        .tag = tag,
        .chunk_meta = chunk_meta,
        .done = false,
    };
    while (!walk.done) {
        ROE(jls_core_repair_chain_walk(self, &walk, INT32_MAX));
    }
    return 0;
}

// Discard the rebuild without writing: closing normally would append
// summaries built from damaged data to the user's file.
static void repair_fsr_abort(struct jls_core_signal_s * signal_info) {
    struct jls_core_fsr_s * fsr = signal_info->track_fsr;
    if (NULL == fsr) {
        return;
    }
    jls_core_fsr_sample_buffer_free(fsr);  // no final data chunk
    for (size_t i = 1; i < JLS_SUMMARY_LEVEL_COUNT; ++i) {
        if (fsr->level[i]) {  // empty levels write nothing on close
            fsr->level[i]->index->header.entry_count = 0;
            fsr->level[i]->summary->header.entry_count = 0;
        }
    }
    jls_fsr_close(fsr);
    signal_info->track_fsr = NULL;
}

// Room for one more index entry plus entries summary entries at level.
// summary1/summaryN append without bounds checks, so damaged chains
// (short chunks, partial summaries) must stop before they overflow.
static bool repair_fsr_has_room(struct jls_core_fsr_s * fsr, int level, uint64_t entries) {
    struct jls_core_fsr_level_s * lvl = fsr->level[level];
    if (NULL == lvl) {
        return entries <= fsr->parent->signal_def.entries_per_summary;  // allocated on demand
    }
    return (lvl->index->header.entry_count < lvl->index_entries)
           && ((lvl->summary->header.entry_count + entries) <= lvl->summary_entries);
}

// Validate a loaded data, index or summary payload against its capacity.
// stored_bits is the minimum on-disk entry size, see summary_payload_min().
static bool repair_fsr_payload_valid(struct jls_core_s * self, uint16_t entry_size_bits, uint16_t stored_bits,
                                     uint32_t entries_max) {
    struct jls_payload_header_s * hdr = (struct jls_payload_header_s *) self->buf->start;
    return (self->buf->length >= sizeof(*hdr))
           && (hdr->entry_size_bits == entry_size_bits)
           && (hdr->entry_count <= entries_max)
           && ((sizeof(*hdr) + ((uint64_t) hdr->entry_count * stored_bits + 7) / 8) <= self->buf->length);
}

int32_t jls_core_repair_fsr(struct jls_core_s * self, uint16_t signal_id) {
    ROE(jls_core_signal_validate_typed(self, signal_id, JLS_SIGNAL_TYPE_FSR));
    struct jls_core_signal_s * signal_info = &self->signal_info[signal_id];
    signal_info->parent = self;
    ROE(jls_fsr_open(&signal_info->track_fsr, signal_info));
    struct jls_core_fsr_s * fsr = signal_info->track_fsr;
    struct jls_core_track_s * track = &signal_info->tracks[JLS_TRACK_TYPE_FSR];
    track->parent = signal_info;

    // find first non-empty level
    int64_t * offsets = track->head_offsets;
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

    struct jls_signal_def_s * signal_def = &signal_info->signal_def;
    const uint16_t index_bits = sizeof(int64_t) * 8;
    const uint16_t summary_bits = (uint16_t) (JLS_SUMMARY_FSR_COUNT
            * jls_core_fsr_summary_entry_size_bits(signal_def->data_type));
    const uint8_t sample_bits = jls_datatype_parse_size(signal_def->data_type);
    int64_t offset_index_next = 0;
    int64_t offset = offsets[level];
    struct jls_core_chunk_s index_head;
    bool skip_summary = false;

    while (level > 0) {
        JLS_LOGI("repair_fsr signal_id %d, level %d, offset %" PRIi64, (int) signal_id, (int) level, offset);
        // read into the current level's buffers; the level above accumulates the rebuilt summary
        struct jls_core_fsr_level_s * lvl = fsr->level[level];
        if (NULL == lvl) {
            if (jls_core_fsr_summary_level_alloc(fsr, (uint8_t) level)) {
                break;
            }
            lvl = fsr->level[level];
        }
        uint16_t meta = (uint16_t) ((signal_id & SIGNAL_MASK) | (level << 12));
        size_t index_sz = sizeof(struct jls_fsr_index_s) + lvl->index_entries * sizeof(int64_t);
        size_t summary_sz = sizeof(struct jls_fsr_f32_summary_s)
                + ((size_t) lvl->summary_entries * summary_bits) / 8;

        if (jls_core_rd_chunk_validate(self, JLS_TAG_TRACK_FSR_INDEX, meta)
                || (self->chunk_cur.hdr.payload_length > index_sz)) {  // read index
            break;
        }
        if (!repair_fsr_payload_valid(self, index_bits, index_bits, lvl->index_entries)) {
            JLS_LOGW("repair_fsr signal_id %d: invalid index at %" PRIi64, (int) signal_id, self->chunk_cur.offset);
            repair_fsr_abort(signal_info);
            return JLS_ERROR_MESSAGE_INTEGRITY;
        }
        index_head = self->chunk_cur;
        memcpy(lvl->index, self->buf->start, self->chunk_cur.hdr.payload_length);

        if (jls_core_rd_chunk_validate(self, JLS_TAG_TRACK_FSR_SUMMARY, meta)
                || (self->chunk_cur.hdr.payload_length > summary_sz)) {  // read summary
            break;
        }
        if (!repair_fsr_payload_valid(self, summary_bits, JLS_SUMMARY_FSR_COUNT * sizeof(float) * 8,
                                      lvl->summary_entries)) {
            JLS_LOGW("repair_fsr signal_id %d: invalid summary at %" PRIi64, (int) signal_id, self->chunk_cur.offset);
            repair_fsr_abort(signal_info);
            return JLS_ERROR_MESSAGE_INTEGRITY;
        }
        track->index_head[level] = index_head;
        offset_index_next = index_head.hdr.item_next;
        track->summary_head[level] = self->chunk_cur;
        memcpy(lvl->summary, self->buf->start, self->chunk_cur.hdr.payload_length);

        struct jls_fsr_index_s * r = lvl->index;
        jls_raw_seek_end(self->raw);
        uint64_t summary_entries = lvl->summary->header.entry_count / signal_def->summary_decimate_factor;
        if (!skip_summary && ((level + 1) < JLS_SUMMARY_LEVEL_COUNT)) {
            if (!repair_fsr_has_room(fsr, level + 1, summary_entries)) {
                JLS_LOGW("repair_fsr signal_id %d level %d: inconsistent summaries", (int) signal_id, level);
                break;
            } else if (jls_core_fsr_summaryN(fsr, (uint8_t) (level + 1), offset)) {
                JLS_LOGE("repair_fsr signal_id %d could not create summary - cannot repair this track", (int) signal_id);
            }
        }
        skip_summary = false;

        if ((offset_index_next > offset) && (0 == jls_raw_chunk_seek(self->raw, offset_index_next))) {
            offset = offset_index_next;
        } else {
            skip_summary = true;
            if (r->header.entry_count > 0) {
                offset = r->offsets[r->header.entry_count - 1];
                // this level's buffers now accumulate the rebuild from the lower-level tail
                lvl->index->header.entry_count = 0;
                lvl->summary->header.entry_count = 0;
                --level;
                if (0 != jls_raw_chunk_seek(self->raw, offset)) {
                    JLS_LOGE("Could not seek to lower-level index.  Cannot repair.");
                    break;
                }
            } else {
                JLS_LOGE("Empty index.  Cannot repair.");
                repair_fsr_abort(signal_info);
                return JLS_ERROR_NOT_SUPPORTED;
            }
        }
    }

    // update level 0 (data)
    if (jls_core_fsr_sample_buffer_alloc(fsr)) {
        signal_info->track_fsr = NULL;  // freed by the failed alloc
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    size_t data_sz = sizeof(struct jls_fsr_data_s)
            + ((size_t) signal_def->samples_per_data * sample_bits) / 8;
    while (offset) {
        if (jls_raw_chunk_seek(self->raw, offset)
                || jls_core_rd_chunk_validate(self, JLS_TAG_TRACK_FSR_DATA, (uint16_t) signal_id)
                || (self->chunk_cur.hdr.payload_length > data_sz)) {
            break;
        }
        struct jls_fsr_data_s * d = (struct jls_fsr_data_s *) self->buf->start;
        if ((self->buf->length < sizeof(d->header))
                || (0 == d->header.entry_count)
                || (d->header.entry_count > signal_def->samples_per_data)
                || !repair_fsr_payload_valid(self, sample_bits, sample_bits, signal_def->samples_per_data)) {
            JLS_LOGW("repair_fsr signal_id %d invalid data chunk at %" PRIi64, (int) signal_id, offset);
            break;
        }
        uint64_t summary_entries = d->header.entry_count / signal_def->sample_decimate_factor;
        if (!skip_summary && !repair_fsr_has_room(fsr, 1, summary_entries)) {
            JLS_LOGW("repair_fsr signal_id %d: inconsistent data chunk at %" PRIi64, (int) signal_id, offset);
            break;
        }
        memcpy(fsr->data, self->buf->start, self->buf->length);
        JLS_LOGI("repair_fsr signal_id %d, level %d, offset %" PRIi64 " sample_id %" PRIi64 " to %" PRIi64,
                 (int) signal_id, (int) level, offset,
                 fsr->data->header.timestamp,
                 fsr->data->header.timestamp + fsr->data->header.entry_count);
        fsr->data_length = fsr->data->header.entry_count;

        // summary1 may flush; all repair writes must append at the file end
        jls_raw_seek_end(self->raw);
        if (!skip_summary && jls_core_fsr_summary1(fsr, offset)) {
            JLS_LOGW("could not create summary - repair may not work");
        }
        skip_summary = false;
        if ((int64_t) self->chunk_cur.hdr.item_next <= offset) {
            break;  // chains always point forward; reject corrupt backward/cyclic links
        }
        offset = (int64_t) self->chunk_cur.hdr.item_next;
    }
    jls_core_fsr_sample_buffer_free(fsr);

    JLS_LOGI("repair_fsr signal_id %d finalizing", (int) signal_id);
    jls_raw_seek_end(self->raw);

    signal_info->track_fsr = NULL;
    ROE(jls_fsr_close(fsr));
    return 0;
}
