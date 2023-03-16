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

#include "jls/wr_fsr.h"
#include "jls/cdef.h"
#include "jls/datatype.h"
#include "jls/wr_prv.h"
#include "jls/ec.h"
#include "jls/log.h"
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>

#define SAMPLE_SIZE_BYTES_MAX           (32)  // =256 bits, must be power of 2
#define SAMPLE_DECIMATE_FACTOR_MIN      (10)
#define SAMPLES_PER_DATA_MIN            (SAMPLE_DECIMATE_FACTOR_MIN)
#define ENTRIES_PER_SUMMARY_MIN         (SAMPLE_DECIMATE_FACTOR_MIN)
#define SUMMARY_DECIMATE_FACTOR_MIN     (SAMPLE_DECIMATE_FACTOR_MIN)

struct level_s {
    uint8_t level;
    uint8_t rsv8_1;
    uint8_t rsv8_2;
    uint8_t rsv8_3;
    uint32_t index_entries;
    uint32_t summary_entries;
    uint32_t rsv32_1;
    struct jls_fsr_index_s * index;
    struct jls_fsr_f32_summary_s * summary;  // either jls_fsr_f32_summary_s or jls_fsr_f64_summary_s
};

struct jls_wr_fsr_s {
    struct jls_wr_s * wr;
    struct jls_signal_def_s def;
    uint32_t data_length;  // for data, in samples
    struct jls_fsr_data_s * data;  // for level 0
    double * data_f64;
    struct level_s * level[JLS_SUMMARY_LEVEL_COUNT];  // level 0 unused
    int64_t sample_id_offset;
};

static int32_t summaryN(struct jls_wr_fsr_s * self, uint8_t level, int64_t pos);
static int32_t summary1(struct jls_wr_fsr_s * self, int64_t pos);

static inline uint8_t sample_size_bits(struct jls_wr_fsr_s * self) {
    return jls_datatype_parse_size(self->def.data_type);
}

static inline uint8_t summary_entry_size(struct jls_wr_fsr_s * self) {
    switch (self->def.data_type & 0xffff) {
        case JLS_DATATYPE_I32: // intentional fall-through
        case JLS_DATATYPE_I64: // intentional fall-through
        case JLS_DATATYPE_U32: // intentional fall-through
        case JLS_DATATYPE_U64: // intentional fall-through
        case JLS_DATATYPE_F64:
            return 64;
        default:
            return 32;
    }
}

static int32_t sample_buffer_alloc(struct jls_wr_fsr_s * self) {
    size_t sample_buffer_sz = sizeof(struct jls_payload_header_s) + (sample_size_bits(self) * self->def.samples_per_data) / 8;
    self->data = malloc(sample_buffer_sz);
    if (!self->data) {
        jls_wr_fsr_close(self);
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    self->data_f64 = malloc(self->def.samples_per_data * sizeof(double));
    if (!self->data_f64) {
        jls_wr_fsr_close(self);
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    JLS_LOGD1("%d sample_buffer alloc %p", self->def.signal_id, (void *) self->data);
    self->data->header.timestamp = 0;
    self->data->header.entry_count = 0;
    self->data->header.entry_size_bits = sample_size_bits(self);
    self->data->header.rsv16 = 0;
    self->data_length = self->def.samples_per_data;
    return 0;
}

static void sample_buffer_free(struct jls_wr_fsr_s * self) {
    if (self->data) {
        free(self->data);
        self->data = NULL;
    }
    if (self->data_f64) {
        free(self->data_f64);
        self->data_f64 = NULL;
    }
}

static int32_t summary_alloc(struct jls_wr_fsr_s * self, uint8_t level) {
    uint32_t index_entries;

    if (level == 0) {
        return JLS_ERROR_PARAMETER_INVALID;
    } else if (level == 1) {
        uint32_t entries_per_data = self->def.samples_per_data / self->def.sample_decimate_factor;
        index_entries = self->def.entries_per_summary / entries_per_data;
    } else {
        index_entries = self->def.summary_decimate_factor;
    }

    size_t dt_sz_bits = summary_entry_size(self);
    size_t buffer_sz = sizeof(struct jls_fsr_f32_summary_s)
            + (self->def.entries_per_summary * JLS_SUMMARY_FSR_COUNT * dt_sz_bits) / 8;
    size_t index_sz = sizeof(struct jls_fsr_index_s) + index_entries * sizeof(int64_t);
    index_sz = ((index_sz + 15) / 16) * 16;
    size_t sz = sizeof(struct level_s) + buffer_sz + index_sz;

    uint8_t * buffer = malloc(sz);
    if (!buffer) {
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    struct level_s * b = (struct level_s *) buffer;
    memset(b, 0, sizeof(struct level_s));
    b->level = level;
    b->index_entries = index_entries;
    b->summary_entries = self->def.entries_per_summary;
    buffer += sizeof(struct level_s);

    b->index = (struct jls_fsr_index_s *) buffer;
    b->index->header.timestamp = 0;
    b->index->header.entry_count = 0;
    b->index->header.entry_size_bits = sizeof(b->index->offsets[0]) * 8;
    b->index->header.rsv16 = 0;
    buffer += index_sz;

    b->summary = (struct jls_fsr_f32_summary_s *) buffer;  // actually jls_fsr_f32_summary_s or jls_fsr_f64_summary_s
    b->summary->header.timestamp = 0;
    b->summary->header.entry_count = 0;
    b->summary->header.entry_size_bits = (uint16_t) (JLS_SUMMARY_FSR_COUNT * dt_sz_bits);
    b->summary->header.rsv16 = 0;

    self->level[level] = b;
    return 0;
}

static void summary_free(struct jls_wr_fsr_s * self, uint8_t level) {
    if (self->level[level]) {
        free(self->level[level]);
        self->level[level] = NULL;
    }
}

static inline uint32_t u32_max(uint32_t a, uint32_t b) {
    return (a > b) ? a : b;
}

static uint32_t round_up_to_multiple(uint32_t x, uint32_t m) {
    return ((x + m - 1) / m) * m;
}

int32_t jls_wr_fsr_validate(struct jls_signal_def_s * def) {
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

static int32_t wr_data(struct jls_wr_fsr_s * self) {
    if (!self->data->header.entry_count) {
        return 0;
    }
    if (self->data->header.entry_count > self->data_length) {
        JLS_LOGE("internal memory error");
    }
    uint32_t payload_length = sizeof(struct jls_fsr_data_s)
            + (self->data->header.entry_count * sample_size_bits(self) + 7) / 8;
    uint8_t * p_start = (uint8_t *) self->data;
    int64_t pos = jls_wr_tell_prv(self->wr);
    ROE(jls_wr_data_prv(self->wr, self->def.signal_id, JLS_TRACK_TYPE_FSR, p_start, payload_length));
    ROE(summary1(self, pos));
    self->data->header.timestamp += self->def.samples_per_data;
    self->data->header.entry_count = 0;
    return 0;
}

static int32_t wr_index(struct jls_wr_fsr_s * self, uint8_t level) {
    if (!self->level[level]) {
        JLS_LOGW("No summary buffer, cannot write index");
        return 0;
    }
    struct jls_fsr_index_s * idx = self->level[level]->index;
    if (!idx->header.entry_count) {
        return 0;
    }
    if (idx->header.entry_count > self->level[level]->index_entries) {
        JLS_LOGE("internal memory error");
    }
    uint8_t * p_end = (uint8_t *) &idx->offsets[idx->header.entry_count];
    uint8_t * p_start = (uint8_t *) idx;
    uint32_t len = (uint32_t) (p_end - p_start);
    return jls_wr_index_prv(self->wr, self->def.signal_id, JLS_TRACK_TYPE_FSR, level, p_start, len);
}

static int32_t wr_summary(struct jls_wr_fsr_s * self, uint8_t level) {
    struct level_s * dst = self->level[level];
    if (!dst->summary->header.entry_count) {
        return 0;
    }
    int64_t pos_next = jls_wr_tell_prv(self->wr);
    ROE(wr_index(self, level));

    uint8_t * p_start = (uint8_t *) dst->summary;
    uint8_t * p_end = (uint8_t *) dst->summary->data[dst->summary->header.entry_count];
    uint32_t payload_len = (uint32_t) (p_end - p_start);
    ROE(jls_wr_summary_prv(self->wr, self->def.signal_id, JLS_TRACK_TYPE_FSR, level, p_start, payload_len));
    ROE(summaryN(self, level + 1, pos_next));

    // compute new timestamp for that level
    int64_t skip = dst->summary_entries * self->def.sample_decimate_factor;
    if (level > 1) {
        skip *= self->def.summary_decimate_factor;
    }
    dst->index->header.timestamp += skip;
    dst->index->header.entry_count = 0;
    dst->summary->header.timestamp += skip;
    dst->summary->header.entry_count = 0;
    return 0;
}

static int32_t summary_close(struct jls_wr_fsr_s * self, uint8_t level) {
    struct level_s * dst = self->level[level];
    if (!dst) {
        return 0;
    }
    int32_t rc = wr_summary(self, level);
    summary_free(self, level);
    return rc;
}

int32_t jls_wr_fsr_open(struct jls_wr_fsr_s ** instance, struct jls_wr_s * wr, const struct jls_signal_def_s * def) {
    struct jls_wr_fsr_s * self = calloc(1, sizeof(struct jls_wr_fsr_s));
    if (!self) {
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    self->wr = wr;
    self->def = *def;
    int32_t rc = jls_wr_fsr_validate(&self->def);
    if (rc) {
        jls_wr_fsr_close(self);
        return rc;
    }

    *instance = self;
    return 0;
}

int32_t jls_wr_fsr_close(struct jls_wr_fsr_s * self) {
    if (self) {
        if (self->data) {
            wr_data(self);  // write remaining sample data
            JLS_LOGD1("%d sample_buffer free %p", (int) self->def.signal_id, (void *) self->data);
            sample_buffer_free(self);
        }

        for (size_t i = 1; i < JLS_SUMMARY_LEVEL_COUNT; ++i) {
            summary_close(self, (uint8_t) i);
        }
        free(self);
    }
    return 0;
}

static void summary_entry_add(struct jls_wr_fsr_s * self, uint8_t level,
        double v_mean, double v_min, double v_max, double v_var) {
    struct level_s * dst = self->level[level];
    uint32_t dst_offset = dst->summary->header.entry_count * JLS_SUMMARY_FSR_COUNT;
    if (summary_entry_size(self) == 64) {
        double * data = (double *) dst->summary->data;
        data[dst_offset + JLS_SUMMARY_FSR_MEAN] = v_mean;
        data[dst_offset + JLS_SUMMARY_FSR_MIN] = v_min;
        data[dst_offset + JLS_SUMMARY_FSR_MAX] = v_max;
        data[dst_offset + JLS_SUMMARY_FSR_STD] = sqrt(v_var);
    } else {
        float * data = (float *) dst->summary->data;
        data[dst_offset + JLS_SUMMARY_FSR_MEAN] = (float) v_mean;
        data[dst_offset + JLS_SUMMARY_FSR_MIN] = (float) v_min;
        data[dst_offset + JLS_SUMMARY_FSR_MAX] = (float) v_max;
        data[dst_offset + JLS_SUMMARY_FSR_STD] = (float) sqrt(v_var);
    }
    ++dst->summary->header.entry_count;
}

#define SUMMARYN_BODY_TEMPLATE()                                                            \
    for (uint32_t idx = 0; idx < summaries_per; ++idx) {                                    \
        uint32_t sample_idx = idx * self->def.summary_decimate_factor;                      \
        double v_mean = 0.0;                                                                \
        double v_min = DBL_MAX;                                                             \
        double v_max = -DBL_MAX;                                                            \
        double v_var = 0.0;                                                                 \
        for (uint32_t sample = 0; sample < self->def.summary_decimate_factor; ++sample) {   \
            uint32_t offset = sample_idx * JLS_SUMMARY_FSR_COUNT;                           \
            v_mean += src_data[offset + JLS_SUMMARY_FSR_MEAN];                              \
            if (src_data[offset + JLS_SUMMARY_FSR_MIN] < v_min) {                           \
                v_min = src_data[offset + JLS_SUMMARY_FSR_MIN];                             \
            }                                                                               \
            if (src_data[offset + JLS_SUMMARY_FSR_MAX] > v_max) {                           \
                v_max = src_data[offset + JLS_SUMMARY_FSR_MAX];                             \
            }                                                                               \
            ++sample_idx;                                                                   \
        }                                                                                   \
        v_mean *= scale;                                                                    \
        sample_idx = idx * self->def.summary_decimate_factor;                               \
        for (uint32_t sample = 0; sample < self->def.summary_decimate_factor; ++sample) {   \
            uint32_t offset = sample_idx * JLS_SUMMARY_FSR_COUNT;                           \
            double v = src_data[offset + JLS_SUMMARY_FSR_MEAN] - v_mean;                    \
            double std = src_data[offset + JLS_SUMMARY_FSR_STD];                            \
            v_var += (std * std) + (v * v);                                                 \
            ++sample_idx;                                                                   \
        }                                                                                   \
        v_var *= scale;                                                                     \
        summary_entry_add(self, level, v_mean, v_min, v_max, v_var);                        \
    }


static int32_t summaryN(struct jls_wr_fsr_s * self, uint8_t level, int64_t pos) {
    if (level < 2) {
        JLS_LOGE("invalid summaryN level: %d", (int) level);
        return JLS_ERROR_PARAMETER_INVALID;
    }
    struct level_s * src = self->level[level - 1];
    struct level_s * dst = self->level[level];

    if (!dst) {
        ROE(summary_alloc(self, level));
        dst = self->level[level];
    }

    const double scale = 1.0 / self->def.summary_decimate_factor;
    JLS_LOGD2("summaryN %d: %" PRIu32 " %" PRIi64, (int) level, dst->index->header.entry_count, pos);
    dst->index->offsets[dst->index->header.entry_count++] = pos;

    uint32_t summaries_per = (uint32_t) (src->summary->header.entry_count / self->def.summary_decimate_factor);
    if (summary_entry_size(self) == 64) {
        double * src_data = ((struct jls_fsr_f64_summary_s *) src->summary)->data[0];
        SUMMARYN_BODY_TEMPLATE();
    } else {
        float * src_data = src->summary->data[0];
        SUMMARYN_BODY_TEMPLATE();
    }

    if (dst->summary->header.entry_count >= dst->summary_entries) {
        ROE(wr_summary(self, level));
    }
    return 0;
}

static void data_to_f64(struct jls_wr_fsr_s * self) {
    void * src = &self->data->data[0];
    double * dst = self->data_f64;
    const uint32_t count = self->data->header.entry_count;
    jls_dt_buffer_to_f64(src, self->def.data_type, dst, count);
}

static int32_t summary1(struct jls_wr_fsr_s * self, int64_t pos) {
    struct level_s * dst = self->level[1];

    if (!dst) {
        ROE(summary_alloc(self, 1));
        dst = self->level[1];
    }
    data_to_f64(self);

    double * data = self->data_f64;
    const double scale = 1.0 / self->def.sample_decimate_factor;
    const double var_scale = 1.0 / (self->def.sample_decimate_factor - 1);  // for sample variance
    // JLS_LOGI("1 add %" PRIi64 " @ %" PRIi64 " %p", pos, dst->index->offset, &dst->index->data[dst->index->offset]);
    dst->index->offsets[dst->index->header.entry_count++] = pos;

    uint32_t summaries_per = (uint32_t) (self->data->header.entry_count / self->def.sample_decimate_factor);
    for (uint32_t idx = 0; idx < summaries_per; ++idx) {
        uint32_t sample_idx = idx * self->def.sample_decimate_factor;
        double v_mean = 0.0;
        double v_min = DBL_MAX;
        double v_max = -DBL_MAX;
        double v_var = 0.0;
        for (uint32_t sample = 0; sample < self->def.sample_decimate_factor; ++sample) {
            double v = data[sample_idx];
            v_mean += v;
            if (v < v_min) {
                v_min = v;
            }
            if (v > v_max) {
                v_max = v;
            }
            ++sample_idx;
        }
        v_mean *= scale;
        sample_idx = idx * self->def.sample_decimate_factor;
        for (uint32_t sample = 0; sample < self->def.sample_decimate_factor; ++sample) {
            double v = data[sample_idx] - v_mean;
            v_var += v * v;
            ++sample_idx;
        }
        v_var *= var_scale;
        summary_entry_add(self, 1, v_mean, v_min, v_max, v_var);
    }

    if (dst->summary->header.entry_count >= dst->summary_entries) {
        ROE(wr_summary(self, 1));
    }
    return 0;
}

int32_t jls_wr_fsr_data(struct jls_wr_fsr_s * self, int64_t sample_id, const void * data, uint32_t data_length) {
    struct jls_fsr_data_s * b = self->data;
    const uint8_t * src_u8 = (const uint8_t *) (data);
    uint8_t * dst_u8;
    uint8_t sample_size_bits = jls_datatype_parse_size(self->def.data_type);

    if (0 == data_length) {
        return 0;
    }

    // only support byte-aligned writes for now - enforce
    switch (sample_size_bits) {
        case 1:
            if ((sample_id & 0x7) || (data_length & 0x7)) {
                return JLS_ERROR_PARAMETER_INVALID;
            }
            break;
        case 4:
            if ((sample_id & 1) || (data_length & 1)) {
                return JLS_ERROR_PARAMETER_INVALID;
            }
            break;
        default:
            break;
    }

    if (!b) {
        ROE(sample_buffer_alloc(self));
        b = self->data;
        self->sample_id_offset = sample_id;
    }
    sample_id -= self->sample_id_offset;

    // todo check for & handle sample_id skips
    if (!b->header.entry_count) {
        b->header.timestamp = sample_id;
    }

    while (data_length) {
        dst_u8 = (uint8_t *) &b->data[0];
        dst_u8 += (b->header.entry_count * sample_size_bits) / 8;
        uint32_t length = (uint32_t) (self->data_length - b->header.entry_count);
        if (data_length < length) {
            length = data_length;
        }
        size_t byte_length = (length * sample_size_bits) / 8;
        memcpy(dst_u8, src_u8, byte_length);
        b->header.entry_count += length;
        src_u8 += byte_length;
        data_length -= length;
        if (b->header.entry_count >= self->data_length) {
            ROE(wr_data(self));
        }
    }
    return 0;
}
