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
    switch (self->parent->signal_def.data_type & 0xffff) {
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

int32_t jls_core_fsr_sample_buffer_alloc(struct jls_core_fsr_s * self) {
    size_t sample_buffer_sz = sizeof(struct jls_payload_header_s) + (sample_size_bits(self) * self->parent->signal_def.samples_per_data) / 8;
    self->data = malloc(sample_buffer_sz);
    if (!self->data) {
        jls_fsr_close(self);
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    self->data_f64 = malloc(self->parent->signal_def.samples_per_data * sizeof(double));
    if (!self->data_f64) {
        jls_fsr_close(self);
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

    if (level == 0) {
        return JLS_ERROR_PARAMETER_INVALID;
    } else if (level == 1) {
        uint32_t entries_per_data = self->parent->signal_def.samples_per_data / self->parent->signal_def.sample_decimate_factor;
        index_entries = self->parent->signal_def.entries_per_summary / entries_per_data;
    } else {
        index_entries = self->parent->signal_def.summary_decimate_factor;
    }

    size_t dt_sz_bits = summary_entry_size(self);
    size_t buffer_sz = sizeof(struct jls_fsr_f32_summary_s)
            + (self->parent->signal_def.entries_per_summary * JLS_SUMMARY_FSR_COUNT * dt_sz_bits) / 8;
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
    }
    uint32_t data_length = (self->data->header.entry_count * sample_size_bits(self) + 7) / 8;
    uint32_t payload_length = sizeof(struct jls_fsr_data_s) + data_length;
    bool omit_data = (self->write_omit_data > 1);
    struct jls_core_track_s * track = &self->parent->tracks[JLS_TRACK_TYPE_FSR];

    if (sample_size_bits(self) <= 8) {
        // Automatically omit constant value data for data sizes 8 bits or less.
        uint8_t data_const = *((uint8_t *) self->data->data);
        if (sample_size_bits(self) == 1) {
            data_const = (data_const & 1) ? 0xff : 0x00;
        } else if (sample_size_bits(self) == 4) {
            data_const = (data_const & 0x0f);
            data_const |= (data_const << 4);
        }
        omit_data = is_mem_const(self->data->data, data_length, data_const);
    }

    // cannot omit first chunk, which stores the sample_id offset.
    omit_data &= (0 != track->data_head.offset);

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
    uint8_t * p_end = (uint8_t *) dst->summary->data[dst->summary->header.entry_count];
    uint32_t payload_len = (uint32_t) (p_end - p_start);
    ROE(jls_core_wr_summary(self->parent->parent, self->parent->signal_def.signal_id, JLS_TRACK_TYPE_FSR, level,
                            p_start, payload_len));
    ROE(jls_core_fsr_summaryN(self, level + 1, pos_next));

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

int32_t jls_fsr_close(struct jls_core_fsr_s * self) {
    int32_t rc;
    if (self) {
        if (self->data) {
            rc = wr_data(self);  // write remaining sample data
            if (rc) {
                JLS_LOGE("wr_data returned %" PRIi32, rc);
            }
            JLS_LOGD1("%d sample_buffer free %p", (int) self->parent->signal_def.signal_id, (void *) self->data);
            jls_core_fsr_sample_buffer_free(self);
        }

        for (size_t i = 1; i < JLS_SUMMARY_LEVEL_COUNT; ++i) {
            rc = summary_close(self, (uint8_t) i);
            if (rc) {
                JLS_LOGE("summary_close(%d) returned %" PRIi32, (int) i, rc);
            }
        }
        free(self);
    }
    return 0;
}

static void summary_entry_add(struct jls_core_fsr_s * self, uint8_t level,
        double v_mean, double v_min, double v_max, double v_var) {
    struct jls_core_fsr_level_s * dst = self->level[level];
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
                double v = src_data[offset + JLS_SUMMARY_FSR_MEAN] - v_mean;                    \
                double std = src_data[offset + JLS_SUMMARY_FSR_STD];                            \
                v_var += (std * std) + (v * v);                                                 \
                ++sample_idx;                                                                   \
            }                                                                                   \
            v_var /= count;                                                                 \
        }                                                                                    \
        summary_entry_add(self, level, v_mean, v_min, v_max, v_var);                        \
    }


int32_t jls_core_fsr_summaryN(struct jls_core_fsr_s * self, uint8_t level, int64_t pos) {
    if (level < 2) {
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
    if (0 == dst->index->header.entry_count) {
        dst->index->header.timestamp = src->index->header.timestamp;
        dst->summary->header.timestamp = src->summary->header.timestamp;
    }
    dst->index->offsets[dst->index->header.entry_count++] = pos;

    uint32_t summaries_per = (uint32_t) (src->summary->header.entry_count / self->parent->signal_def.summary_decimate_factor);
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

static void data_to_f64(struct jls_core_fsr_s * self) {
    void * src = &self->data->data[0];
    double * dst = self->data_f64;
    const uint32_t count = self->data->header.entry_count;
    jls_dt_buffer_to_f64(src, self->parent->signal_def.data_type, dst, count);
}

int32_t jls_core_fsr_summary1(struct jls_core_fsr_s * self, int64_t pos) {
    struct jls_core_fsr_level_s * dst = self->level[1];

    if (!dst) {
        ROE(jls_core_fsr_summary_level_alloc(self, 1));
        dst = self->level[1];
    }
    data_to_f64(self);

    double * data = self->data_f64;
    // JLS_LOGI("1 add %" PRIi64 " @ %" PRIi64 " %p", pos, dst->index->offset, &dst->index->data[dst->index->offset]);
    if (0 == dst->index->header.entry_count) {
        dst->index->header.timestamp = self->data->header.timestamp;
        dst->summary->header.timestamp = self->data->header.timestamp;
    }
    dst->index->offsets[dst->index->header.entry_count++] = pos;

    uint32_t summaries_per = (uint32_t) (self->data->header.entry_count / self->parent->signal_def.sample_decimate_factor);
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

    if (dst->summary->header.entry_count >= dst->summary_entries) {
        ROE(wr_summary(self, 1));
    }
    return 0;
}

static int32_t wr_data_inner(struct jls_core_fsr_s * self, const void * data, uint32_t data_length) {
    struct jls_fsr_data_s * b = self->data;
    uint8_t sample_size_bits = jls_datatype_parse_size(self->parent->signal_def.data_type);
    const uint8_t * src_u8 = (const uint8_t *) (data);
    uint8_t * dst_u8;
    uint8_t shift_this = (data_length * sample_size_bits) % 8;
    uint8_t shift_amount_next = (shift_this + self->shift_amount) % 8;

    while (data_length) {
        dst_u8 = (uint8_t *) &b->data[0];
        dst_u8 += (b->header.entry_count * sample_size_bits) / 8;
        uint32_t length = (uint32_t) (self->data_length - b->header.entry_count);
        if (data_length < length) {
            length = data_length;
        }
        if (self->shift_amount) {
            uint8_t mask = (1 << self->shift_amount) - 1;
            uint32_t bits = length * sample_size_bits + self->shift_amount;
            while (bits) {
                uint16_t v = (self->shift_buffer & mask) | (((uint16_t) (*src_u8++)) << self->shift_amount);
                if (bits >= 8) {
                    *dst_u8++ = (uint8_t) v;
                    bits -= 8;
                    self->shift_buffer = (uint8_t) (v >> 8);
                } else {
                    self->shift_buffer = (uint8_t) v;
                    break;
                }
            }
        } else {
            size_t byte_length = (length * sample_size_bits) / 8;
            if (byte_length) {
                memcpy(dst_u8, src_u8, byte_length);
            }
            self->shift_buffer = src_u8[byte_length];
            src_u8 += byte_length;
        }
        b->header.entry_count += length;
        data_length -= length;
        if (b->header.entry_count >= self->data_length) {
            ROE(wr_data(self));
        }
    }
    self->shift_amount = shift_amount_next;
    return 0;
}

int32_t jls_wr_fsr_data(struct jls_core_fsr_s * self, int64_t sample_id, const void * data, uint32_t data_length) {
    uint8_t sample_size_bits = jls_datatype_parse_size(self->parent->signal_def.data_type);

    if (0 == data_length) {
        return 0;
    }

    if (!self->data) {
        ROE(jls_core_fsr_sample_buffer_alloc(self));
        self->sample_id_offset = sample_id;  // can be nonzero
        self->data->header.timestamp = sample_id;
    }

    struct jls_fsr_data_s * b = self->data;
    int64_t sample_id_next = b->header.timestamp + b->header.entry_count;
    if (sample_id == sample_id_next) {
        // normal
    } else if (sample_id < sample_id_next) {
        JLS_LOGI("fsr dup: in=%" PRIi64 " expect=%" PRIi64,
                 sample_id, sample_id_next);
        if ((sample_id + data_length) <= sample_id_next) {
            return 0;
        }
        const uint8_t * data_u8 = (const uint8_t *) data;
        const uint8_t * data_end_u8 = data_u8 + (data_length * sample_size_bits + 7) / 8;
        uint32_t ffwd = (uint32_t) (sample_id_next - sample_id);
        data_length -= ffwd;
        if (sample_size_bits >= 8) {
            data = data_u8 + ffwd * (sample_size_bits / 8);
        } else {
            uint32_t shift = 0;
            uint32_t shift_samples = 0;
            if (sample_size_bits == 4) {
                shift = (ffwd & 1) ? 4 : 0;
                shift_samples = 1;
            } else if (sample_size_bits == 1) {
                shift = ffwd % sample_size_bits;
                shift_samples = shift;
            }
            if (shift == 0) {
                data = data_u8 + ffwd * (sample_size_bits / 8);
            } else {
                while (data_u8 < data_end_u8) {
                    size_t sz = data_end_u8 - data_u8;
                    if (sz > (sizeof(self->buffer_u64) - 8)) {
                        sz = sizeof(self->buffer_u64) - 8;
                    }
                    memcpy(self->buffer_u64, data_u8, sz);
                    self->buffer_u64[(sz / 8) + 1] = 0;
                    size_t sz_words = (sz + 7) / 8;
                    for (uint64_t idx = 0; idx < sz_words; ++idx) {
                        self->buffer_u64[idx] = (self->buffer_u64[idx] >> shift)
                                | (self->buffer_u64[idx + 1] << (64 - shift));
                    }
                    size_t entries = sz * (8 / sample_size_bits) - shift_samples;
                    ROE(wr_data_inner(self, self->buffer_u64, (uint32_t) entries));
                    data_u8 += sz - 1;
                }
                return 0;
            }
        }
    } else {
        JLS_LOGW("fsr %d skip: in=%" PRIi64 " expect=%" PRIi64 ", skipped=%" PRIi64,
                 self->parent->signal_def.signal_id,
                 sample_id, sample_id_next,
                 sample_id - sample_id_next);
        size_t skip = (size_t) (sample_id - sample_id_next);
        size_t buf_sz = 0;
        if (self->parent->signal_def.data_type == JLS_DATATYPE_F32) {
            float * f32 = (float *) self->buffer_u64;
            buf_sz = sizeof(self->buffer_u64);
            buf_sz /= sizeof(float);
            for (size_t idx = 0; idx < buf_sz; ++idx) {
                f32[idx] = NAN;
            }
        } else if (self->parent->signal_def.data_type == JLS_DATATYPE_F64) {
            double * f64 = (double *) self->buffer_u64;
            buf_sz = sizeof(self->buffer_u64) / sizeof(double);
            for (size_t idx = 0; idx < sizeof(self->buffer_u64) / sizeof(double); ++idx) {
                f64[idx] = NAN;
            }
        } else {
            buf_sz = (sizeof(self->buffer_u64) * sample_size_bits) / 8;
            memset(self->buffer_u64, 0, sizeof(self->buffer_u64));
        }
        while (skip) {
            if (skip < buf_sz) {
                buf_sz = skip;
            }
            ROE(wr_data_inner(self, self->buffer_u64, (uint32_t) buf_sz));
            skip -= buf_sz;
        }
    }

    return wr_data_inner(self, data, data_length);
}
