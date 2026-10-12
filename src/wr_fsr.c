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
#include "jls/core.h"
#include "jls/cdef.h"
#include "jls/datatype.h"
#include "jls/wr_prv.h"
#include "jls/bit_shift.h"
#include "jls/tmap.h"
#include "jls/ec.h"
#include "jls/log.h"
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>


static inline uint8_t sample_size_bits(struct jls_core_fsr_s * self) {
    return jls_datatype_parse_size(self->parent->signal_def.data_type);
}

static inline uint8_t summary_entry_size(struct jls_core_fsr_s * self) {
    return jls_core_fsr_summary_entry_size_bits(self->parent->signal_def.data_type);
}

int32_t jls_core_fsr_sample_buffer_alloc(struct jls_core_fsr_s * self) {
    // 64-bit size math: a 32-bit product wraps for large samples_per_data.
    uint64_t spd = self->parent->signal_def.samples_per_data;
    uint64_t data_sz = (spd * sample_size_bits(self) + 7) / 8;
    if ((spd * sizeof(double)) > (SIZE_MAX / 2)) {
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    // Free only the buffers on failure: the signal still references self.
    self->data = malloc(sizeof(struct jls_payload_header_s) + (size_t) data_sz);
    self->data_f64 = malloc((size_t) (spd * sizeof(double)));
    if (!self->data || !self->data_f64) {
        jls_core_fsr_sample_buffer_free(self);
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    JLS_LOGD1("%d sample_buffer alloc %p", self->parent->signal_def.signal_id, (void *) self->data);
    self->data->header.timestamp = 0;
    self->data->header.entry_count = 0;
    self->data->header.entry_size_bits = sample_size_bits(self);
    self->data->header.rsv16 = 0;
    self->data_length = self->parent->signal_def.samples_per_data;
    return 0;
}

void jls_core_fsr_sample_buffer_free(struct jls_core_fsr_s * self) {
    if (self->data) {
        free(self->data);
        self->data = NULL;
    }
    if (self->data_f64) {
        free(self->data_f64);
        self->data_f64 = NULL;
    }
}

int32_t jls_core_fsr_summary_level_alloc(struct jls_core_fsr_s * self, uint8_t level) {
    uint32_t index_entries;

    const struct jls_signal_def_s * def = &self->parent->signal_def;
    if ((level == 0) || (level >= JLS_SUMMARY_LEVEL_COUNT) || !def->sample_decimate_factor) {
        return JLS_ERROR_PARAMETER_INVALID;
    } else if (level == 1) {
        uint32_t entries_per_data = def->samples_per_data / def->sample_decimate_factor;
        index_entries = entries_per_data ? (def->entries_per_summary / entries_per_data) : 0;
    } else {
        index_entries = def->summary_decimate_factor;
    }
    if (!index_entries) {
        return JLS_ERROR_PARAMETER_INVALID;  // appends could never fit
    }

    size_t dt_sz_bits = summary_entry_size(self);
    size_t buffer_sz = sizeof(struct jls_fsr_f32_summary_s)
            + ((size_t) def->entries_per_summary * JLS_SUMMARY_FSR_COUNT * dt_sz_bits) / 8;
    buffer_sz = ((buffer_sz + 15) / 16) * 16;

    size_t index_sz = sizeof(struct jls_fsr_index_s) + index_entries * sizeof(int64_t);
    index_sz = ((index_sz + 15) / 16) * 16;

    size_t sz = sizeof(struct jls_core_fsr_level_s) + buffer_sz + index_sz;
    uint8_t * buffer = malloc(sz);
    if (!buffer) {
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    struct jls_core_fsr_level_s * b = (struct jls_core_fsr_level_s *) buffer;
    memset(b, 0, sizeof(struct jls_core_fsr_level_s));
    b->level = level;
    b->index_entries = index_entries;
    b->summary_entries = self->parent->signal_def.entries_per_summary;
    buffer += sizeof(struct jls_core_fsr_level_s);

    b->index = (struct jls_fsr_index_s *) buffer;
    b->index->header.timestamp = self->sample_id_offset;
    b->index->header.entry_count = 0;
    b->index->header.entry_size_bits = sizeof(b->index->offsets[0]) * 8;
    b->index->header.rsv16 = 0;
    buffer += index_sz;

    b->summary = (struct jls_fsr_f32_summary_s *) buffer;  // actually jls_fsr_f32_summary_s or jls_fsr_f64_summary_s
    b->summary->header.timestamp = self->sample_id_offset;
    b->summary->header.entry_count = 0;
    b->summary->header.entry_size_bits = (uint16_t) (JLS_SUMMARY_FSR_COUNT * dt_sz_bits);
    b->summary->header.rsv16 = 0;

    self->level[level] = b;
    return 0;
}

static void summary_free(struct jls_core_fsr_s * self, uint8_t level) {
    if (self->level[level]) {
        free(self->level[level]);
        self->level[level] = NULL;
    }
}

static bool is_mem_const(void * mem, size_t mem_size, uint8_t c) {
    uint8_t * m = (uint8_t *) mem;
    uint8_t * m_end = m + mem_size;
    while (m < m_end) {
        if (*m++ != c) {
            return false;
        }
    }
    return true;
}

static int32_t wr_data(struct jls_core_fsr_s * self) {
    if (!self->data->header.entry_count) {
        return 0;
    }
    if (self->data->header.entry_count > self->data_length) {
        JLS_LOGE("internal memory error");
        return JLS_ERROR_UNSPECIFIED;
    }
    uint64_t data_length_u64 = ((uint64_t) self->data->header.entry_count * sample_size_bits(self) + 7) / 8;
    if ((data_length_u64 + sizeof(struct jls_fsr_data_s)) > UINT32_MAX) {
        return JLS_ERROR_TOO_BIG;
    }
    uint32_t data_length = (uint32_t) data_length_u64;
    uint32_t payload_length = (uint32_t) sizeof(struct jls_fsr_data_s) + data_length;
    struct jls_core_track_s * track = &self->parent->tracks[JLS_TRACK_TYPE_FSR];
    bool omit_data = (sample_size_bits(self) <= 8) & (self->write_omit_data > 1) & (0 != track->data_head.offset);

    if (omit_data) {
        // Automatically omit constant value data for data sizes 8 bits or less.
        uint8_t data_const = *((uint8_t *) self->data->data);
        if (sample_size_bits(self) == 1) {
            data_const = (data_const & 1) ? 0xff : 0x00;
        } else if (sample_size_bits(self) == 4) {
            data_const = (data_const & 0x0f);
            data_const |= (data_const << 4);
        }
        omit_data &= is_mem_const(self->data->data, data_length, data_const);
    }

    uint8_t * p_start = (uint8_t *) self->data;
    int64_t pos = jls_raw_chunk_tell(self->parent->parent->raw);
    if (omit_data) {
        pos = 0;
    } else {
        ROE(jls_core_wr_data(self->parent->parent, self->parent->signal_def.signal_id,
                             JLS_TRACK_TYPE_FSR, p_start, payload_length));
    }
    ROE(jls_core_fsr_summary1(self, pos));
    self->data->header.timestamp += self->parent->signal_def.samples_per_data;
    self->data->header.entry_count = 0;
    self->write_omit_data = (self->write_omit_data << 1) | (self->write_omit_data & 1);
    return 0;
}

static int32_t wr_index(struct jls_core_fsr_s * self, uint8_t level) {
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
        return JLS_ERROR_UNSPECIFIED;
    }
    uint8_t * p_end = (uint8_t *) &idx->offsets[idx->header.entry_count];
    uint8_t * p_start = (uint8_t *) idx;
    uint32_t len = (uint32_t) (p_end - p_start);
    return jls_core_wr_index(self->parent->parent, self->parent->signal_def.signal_id, JLS_TRACK_TYPE_FSR, level,
                             p_start, len);
}

static int32_t wr_summary(struct jls_core_fsr_s * self, uint8_t level) {
    struct jls_core_fsr_level_s * dst = self->level[level];
    if (!dst->summary->header.entry_count) {
        return 0;
    }
    int64_t pos_next = jls_raw_chunk_tell(self->parent->parent->raw);
    ROE(wr_index(self, level));

    uint8_t * p_start = (uint8_t *) dst->summary;
    // Size by the actual entry size: 64-bit summaries are twice the f32 layout.
    uint32_t payload_len = (uint32_t) (sizeof(dst->summary->header)
            + ((uint64_t) dst->summary->header.entry_count * JLS_SUMMARY_FSR_COUNT * summary_entry_size(self)) / 8);
    ROE(jls_core_wr_summary(self->parent->parent, self->parent->signal_def.signal_id, JLS_TRACK_TYPE_FSR, level,
                            p_start, payload_len));
    if ((level + 1) < JLS_SUMMARY_LEVEL_COUNT) {  // the top level is a sink
        ROE(jls_core_fsr_summaryN(self, level + 1, pos_next));
    }

    dst->index->header.entry_count = 0;
    dst->summary->header.entry_count = 0;
    return 0;
}

static int32_t summary_close(struct jls_core_fsr_s * self, uint8_t level) {
    struct jls_core_fsr_level_s * dst = self->level[level];
    if (!dst) {
        return 0;
    }
    int32_t rc = wr_summary(self, level);
    summary_free(self, level);
    return rc;
}

int32_t jls_fsr_open(struct jls_core_fsr_s ** instance, struct jls_core_signal_s * parent) {
    struct jls_core_fsr_s * self = calloc(1, sizeof(struct jls_core_fsr_s));
    if (!self) {
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    self->parent = parent;
    self->signal_length = -1;

    *instance = self;
    return 0;
}

void jls_wr_fsr_free(struct jls_core_fsr_s * self) {
    if (self) {
        jls_core_fsr_sample_buffer_free(self);
        for (size_t i = 1; i < JLS_SUMMARY_LEVEL_COUNT; ++i) {
            summary_free(self, (uint8_t) i);
        }
        jls_tmap_free(self->tmap);  // allocated by the reader
        self->tmap = NULL;
        free(self);
    }
}

int32_t jls_wr_fsr_close(struct jls_core_fsr_s * self) {
    int32_t rc = 0;
    if (!self) {
        return 0;
    }
    if (self->data) {
        self->write_omit_data = 0;  // disable omit for last data chunk
        rc = wr_data(self);  // write remaining sample data
        if (rc) {
            JLS_LOGE("wr_data returned %" PRIi32, rc);
        }
    }
    // Stop at the first error: later writes would only add inconsistent summaries.
    for (size_t i = 1; (0 == rc) && (i < JLS_SUMMARY_LEVEL_COUNT); ++i) {
        rc = summary_close(self, (uint8_t) i);
        if (rc) {
            JLS_LOGE("summary_close(%d) returned %" PRIi32, (int) i, rc);
        }
    }
    jls_wr_fsr_free(self);
    return rc;
}

int32_t jls_fsr_close(struct jls_core_fsr_s * self) {
    // Always 0: core.c callers clear their reference only on success.
    (void) jls_wr_fsr_close(self);
    return 0;
}

static void summary_entry_add(struct jls_core_fsr_s * self, uint8_t level,
        double v_mean, double v_min, double v_max, double v_var) {
    struct jls_core_fsr_level_s * dst = self->level[level];  // capacity checked by caller
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
        uint32_t sample_idx = idx * self->parent->signal_def.summary_decimate_factor;                      \
        uint32_t count = 0;                                                                 \
        double v;                                                                           \
        double v_mean = 0.0;                                                                \
        double v_min = DBL_MAX;                                                             \
        double v_max = -DBL_MAX;                                                            \
        double v_var = 0.0;                                                                 \
        for (uint32_t sample = 0; sample < self->parent->signal_def.summary_decimate_factor; ++sample) {   \
            uint32_t offset = sample_idx * JLS_SUMMARY_FSR_COUNT;                           \
            v = src_data[offset + JLS_SUMMARY_FSR_MEAN];                                    \
            if (isfinite(v)) {                                                              \
                ++count;                                                                    \
                v_mean += v;                                                                \
                if (src_data[offset + JLS_SUMMARY_FSR_MIN] < v_min) {                       \
                    v_min = src_data[offset + JLS_SUMMARY_FSR_MIN];                         \
                }                                                                           \
                if (src_data[offset + JLS_SUMMARY_FSR_MAX] > v_max) {                       \
                    v_max = src_data[offset + JLS_SUMMARY_FSR_MAX];                         \
                }                                                                           \
            }                                                                               \
            ++sample_idx;                                                                   \
        }                                                                                   \
        if (count == 0) {                                                                   \
            v_mean = NAN;                                                                   \
            v_var = NAN;                                                                    \
            v_min = NAN;                                                                    \
            v_max = NAN;                                                                    \
        } else {                                                                            \
            v_mean /= count;                                                                    \
            sample_idx = idx * self->parent->signal_def.summary_decimate_factor;                               \
            for (uint32_t sample = 0; sample < self->parent->signal_def.summary_decimate_factor; ++sample) {   \
                uint32_t offset = sample_idx * JLS_SUMMARY_FSR_COUNT;                           \
                double v = src_data[offset + JLS_SUMMARY_FSR_MEAN];                             \
                if (isfinite(v)) {  /* skip gaps, as the mean loop does */                      \
                    double std = src_data[offset + JLS_SUMMARY_FSR_STD];                        \
                    v -= v_mean;                                                                \
                    v_var += (std * std) + (v * v);                                             \
                }                                                                               \
                ++sample_idx;                                                                   \
            }                                                                                   \
            v_var /= count;                                                                 \
        }                                                                                    \
        summary_entry_add(self, level, v_mean, v_min, v_max, v_var);                        \
    }


int32_t jls_core_fsr_summaryN(struct jls_core_fsr_s * self, uint8_t level, int64_t pos) {
    if ((level < 2) || (level >= JLS_SUMMARY_LEVEL_COUNT)) {
        JLS_LOGE("invalid jls_core_fsr_summaryN level: %d", (int) level);
        return JLS_ERROR_PARAMETER_INVALID;
    }
    struct jls_core_fsr_level_s * src = self->level[level - 1];
    struct jls_core_fsr_level_s * dst = self->level[level];

    if (!dst) {
        ROE(jls_core_fsr_summary_level_alloc(self, level));
        dst = self->level[level];
    }

    JLS_LOGD2("jls_core_fsr_summaryN %d: entries=%" PRIu32 ", offset=%" PRIi64 ", sample_id=%" PRIi64,
              (int) level, dst->index->header.entry_count, pos, dst->index->header.timestamp);
    // Bound every append: repair feeds file-provided counts, and a failed flush leaves the buffers full.
    uint32_t src_count = src->summary->header.entry_count;
    if (src_count > src->summary_entries) {
        src_count = src->summary_entries;
    }
    uint32_t summaries_per = src_count / self->parent->signal_def.summary_decimate_factor;
    if ((dst->index->header.entry_count >= dst->index_entries)
            || (summaries_per > (dst->summary_entries - dst->summary->header.entry_count))) {
        JLS_LOGE("jls_core_fsr_summaryN %d full", (int) level);
        return JLS_ERROR_FULL;
    }
    if (0 == dst->index->header.entry_count) {
        dst->index->header.timestamp = src->index->header.timestamp;
        dst->summary->header.timestamp = src->summary->header.timestamp;
    }
    dst->index->offsets[dst->index->header.entry_count++] = pos;

    if (summary_entry_size(self) == 64) {
        double * src_data = ((struct jls_fsr_f64_summary_s *) src->summary)->data[0];
        SUMMARYN_BODY_TEMPLATE();
    } else {
        float * src_data = src->summary->data[0];
        SUMMARYN_BODY_TEMPLATE();
    }

    if ((dst->summary->header.entry_count >= dst->summary_entries)
            || (dst->index->header.entry_count >= dst->index_entries)) {
        ROE(wr_summary(self, level));
    }
    return 0;
}

static void data_to_f64(struct jls_core_fsr_s * self) {
    void * src = &self->data->data[0];
    double * dst = self->data_f64;
    const uint32_t count = self->data->header.entry_count;
    // Strip q: summaries store raw values in every format version, see jls_dt_scale().
    if (jls_dt_buffer_to_f64(src, self->parent->signal_def.data_type & 0xffff, dst, count)) {
        // Unsupported conversion: NaN summaries, never uninitialized memory.
        for (uint32_t idx = 0; idx < count; ++idx) {
            dst[idx] = NAN;
        }
    }
}

int32_t jls_core_fsr_summary1(struct jls_core_fsr_s * self, int64_t pos) {
    struct jls_core_fsr_level_s * dst = self->level[1];

    if (!dst) {
        ROE(jls_core_fsr_summary_level_alloc(self, 1));
        dst = self->level[1];
    }
    if (self->data->header.entry_count > self->parent->signal_def.samples_per_data) {
        return JLS_ERROR_PARAMETER_INVALID;  // larger than the sample buffer
    }
    uint32_t summaries_per = (uint32_t) (self->data->header.entry_count / self->parent->signal_def.sample_decimate_factor);
    // Bound every append: repair feeds file-provided counts, and a failed flush leaves the buffers full.
    if ((dst->index->header.entry_count >= dst->index_entries)
            || (summaries_per > (dst->summary_entries - dst->summary->header.entry_count))) {
        JLS_LOGE("jls_core_fsr_summary1 full");
        return JLS_ERROR_FULL;
    }
    data_to_f64(self);

    double * data = self->data_f64;
    if (0 == dst->index->header.entry_count) {
        dst->index->header.timestamp = self->data->header.timestamp;
        dst->summary->header.timestamp = self->data->header.timestamp;
    }
    dst->index->offsets[dst->index->header.entry_count++] = pos;

    for (uint32_t idx = 0; idx < summaries_per; ++idx) {
        uint32_t sample_idx = idx * self->parent->signal_def.sample_decimate_factor;
        uint32_t count = 0;
        double v_mean = 0.0;
        double v_min = DBL_MAX;
        double v_max = -DBL_MAX;
        double v_var = 0.0;
        for (uint32_t sample = 0; sample < self->parent->signal_def.sample_decimate_factor; ++sample) {
            double v = data[sample_idx];
            if (isfinite(v)) {
                ++count;
                v_mean += v;
                if (v < v_min) {
                    v_min = v;
                }
                if (v > v_max) {
                    v_max = v;
                }
            }
            ++sample_idx;
        }
        if (count == 0) {
            v_mean = NAN;
            v_min = NAN;
            v_max = NAN;
            v_var = NAN;
        } else {
            v_mean /= count;
            sample_idx = idx * self->parent->signal_def.sample_decimate_factor;
            for (uint32_t sample = 0; sample < self->parent->signal_def.sample_decimate_factor; ++sample) {
                double v = data[sample_idx];
                if (isfinite(v)) {
                    v -= v_mean;
                    v_var += v * v;
                }
                ++sample_idx;
            }
            if (count == 1) {
                v_var = 0.0;
            } else {
                v_var /= count;
            }
        }
        summary_entry_add(self, 1, v_mean, v_min, v_max, v_var);
    }

    if ((dst->summary->header.entry_count >= dst->summary_entries)
            || (dst->index->header.entry_count >= dst->index_entries)) {
        ROE(wr_summary(self, 1));
    }
    return 0;
}

/*
 * Append data_length samples starting at bit src_offset of data.
 * Sub-byte types keep their partial trailing byte in self->data (upper
 * bits zero), so close writes it and appends never read past the
 * caller's buffer.
 */
static int32_t wr_data_inner(struct jls_core_fsr_s * self, const uint8_t * data, uint64_t src_offset,
                             uint64_t data_length) {
    struct jls_fsr_data_s * b = self->data;
    uint8_t bits = sample_size_bits(self);
    while (data_length) {
        uint64_t length = self->data_length - b->header.entry_count;
        if (data_length < length) {
            length = data_length;
        }
        jls_bit_copy((uint8_t *) b->data, (uint64_t) b->header.entry_count * bits,
                     data, src_offset, length * bits);
        b->header.entry_count += (uint32_t) length;
        src_offset += length * bits;
        data_length -= length;
        if (b->header.entry_count >= self->data_length) {
            ROE(wr_data(self));
        }
    }
    return 0;
}

/// Fill skipped samples: NaN for float types, 0 otherwise.
static int32_t wr_fill(struct jls_core_fsr_s * self, uint64_t count) {
    struct jls_fsr_data_s * b = self->data;
    uint8_t bits = sample_size_bits(self);
    uint32_t data_type = self->parent->signal_def.data_type;
    while (count) {
        uint64_t length = self->data_length - b->header.entry_count;
        if (count < length) {
            length = count;
        }
        if (JLS_DATATYPE_F32 == data_type) {
            float * f32 = ((float *) b->data) + b->header.entry_count;
            for (uint64_t idx = 0; idx < length; ++idx) {
                f32[idx] = NAN;
            }
        } else if (JLS_DATATYPE_F64 == data_type) {
            double * f64 = ((double *) b->data) + b->header.entry_count;
            for (uint64_t idx = 0; idx < length; ++idx) {
                f64[idx] = NAN;
            }
        } else {
            // A partial trailing byte already has zero upper bits.
            uint64_t start = ((uint64_t) b->header.entry_count * bits + 7) / 8;
            uint64_t end = (((uint64_t) b->header.entry_count + length) * bits + 7) / 8;
            memset(((uint8_t *) b->data) + start, 0, (size_t) (end - start));
        }
        b->header.entry_count += (uint32_t) length;
        count -= length;
        if (b->header.entry_count >= self->data_length) {
            ROE(wr_data(self));
        }
    }
    return 0;
}

int32_t jls_wr_fsr_validate(struct jls_core_fsr_s * self, int64_t sample_id, uint32_t data_length) {
    if (!self) {
        return JLS_ERROR_NOT_FOUND;  // signal definition did not complete
    }
    (void) sample_id;
    (void) data_length;
    return 0;
}

int32_t jls_wr_fsr_data(struct jls_core_fsr_s * self, int64_t sample_id, const void * data, uint32_t data_length) {
    ROE(jls_wr_fsr_validate(self, sample_id, data_length));
    if (0 == data_length) {
        return 0;
    }

    if (!self->data) {
        ROE(jls_core_fsr_sample_buffer_alloc(self));
        self->sample_id_offset = sample_id;  // can be nonzero
        self->data->header.timestamp = sample_id;
    }

    struct jls_fsr_data_s * b = self->data;
    uint8_t bits = sample_size_bits(self);
    int64_t sample_id_next = b->header.timestamp + b->header.entry_count;
    uint64_t ffwd = 0;
    if (sample_id == sample_id_next) {
        // normal
    } else if (sample_id < sample_id_next) {
        JLS_LOGI("fsr dup: in=%" PRIi64 " expect=%" PRIi64,
                 sample_id, sample_id_next);
        ffwd = (uint64_t) sample_id_next - (uint64_t) sample_id;
        if (ffwd >= data_length) {
            return 0;
        }
    } else {
        uint64_t skip = (uint64_t) sample_id - (uint64_t) sample_id_next;
        JLS_LOGW("fsr %d skip: in=%" PRIi64 " expect=%" PRIi64 ", skipped=%" PRIu64,
                 self->parent->signal_def.signal_id,
                 sample_id, sample_id_next, skip);
        ROE(wr_fill(self, skip));
    }
    // Skip duplicates by bit offset: sub-byte types can start mid-byte.
    return wr_data_inner(self, (const uint8_t *) data, ffwd * bits, data_length - ffwd);
}
