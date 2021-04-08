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

#include "jls/wf_f32.h"
#include "jls/cdef.h"
#include "jls/wr_prv.h"
#include "jls/ec.h"
#include "jls/log.h"
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>


#define SAMPLE_DECIMATE_FACTOR_MIN     (10)
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
    struct jls_fsr_f32_summary_s * summary;
};

struct jls_wf_f32_s {
    struct jls_wr_s * wr;
    struct jls_wf_f32_def_s def;
    uint32_t data_length;  // for data, in float32 values
    struct jls_fsr_f32_data_s * data;  // for level 0
    struct level_s * level[JLS_SUMMARY_LEVEL_COUNT];  // level 0 unused
    int64_t sample_id_offset;
};

static int32_t summaryN(struct jls_wf_f32_s * self, uint8_t level, int64_t pos);
static int32_t summary1(struct jls_wf_f32_s * self, int64_t pos);

static int32_t sample_buffer_alloc(struct jls_wf_f32_s * self) {
    size_t sample_buffer_sz = sizeof(struct jls_fsr_f32_data_s) + sizeof(float) * self->def.samples_per_data;
    self->data = malloc(sample_buffer_sz);
    JLS_LOGD1("%d sample_buffer alloc %p", self->def.signal_id, (void *) self->data);
    if (!self->data) {
        jls_wf_f32_close(self);
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    self->data->header.timestamp = 0;
    self->data->header.entry_count = 0;
    self->data->header.entry_size_bits = sizeof(float) * 8;
    self->data->header.rsv16 = 0;
    self->data_length = self->def.samples_per_data;
    return 0;
}

static void sample_buffer_free(struct jls_wf_f32_s * self) {
    if (self->data) {
        free(self->data);
    }
    self->data = NULL;
}

static int32_t summary_alloc(struct jls_wf_f32_s * self, uint8_t level) {
    uint32_t index_entries;

    if (level == 0) {
        return JLS_ERROR_PARAMETER_INVALID;
    } else if (level == 1) {
        uint32_t entries_per_data = self->def.samples_per_data / self->def.sample_decimate_factor;
        index_entries = self->def.entries_per_summary / entries_per_data;
    } else {
        index_entries = self->def.summary_decimate_factor;
    }

    size_t buffer_sz = sizeof(struct jls_fsr_f32_summary_s) + self->def.entries_per_summary * JLS_SUMMARY_FSR_COUNT * sizeof(float);
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

    b->summary = (struct jls_fsr_f32_summary_s *) buffer;
    b->summary->header.timestamp = 0;
    b->summary->header.entry_count = 0;
    b->summary->header.entry_size_bits = JLS_SUMMARY_FSR_COUNT * sizeof(b->summary->data[0]) * 8;
    b->summary->header.rsv16 = 0;

    self->level[level] = b;
    return 0;
}

static void summary_free(struct jls_wf_f32_s * self, uint8_t level) {
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

int32_t jls_wf_f32_align_def(struct jls_wf_f32_def_s * def) {
    uint32_t sample_decimate_factor = u32_max(def->sample_decimate_factor, SAMPLE_DECIMATE_FACTOR_MIN);
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

static int32_t wr_data(struct jls_wf_f32_s * self) {
    if (!self->data->header.entry_count) {
        return 0;
    }
    if (self->data->header.entry_count > self->data_length) {
        JLS_LOGE("internal memory error");
    }
    uint8_t * p_start = (uint8_t *) self->data;
    uint8_t * p_end = (uint8_t *) &self->data->data[self->data->header.entry_count];
    uint32_t payload_length = p_end - p_start;
    int64_t pos = jls_wr_tell_prv(self->wr);
    ROE(jls_wr_data_prv(self->wr, self->def.signal_id, JLS_TRACK_TYPE_FSR, p_start, payload_length));
    ROE(summary1(self, pos));
    self->data->header.timestamp += self->def.samples_per_data;
    self->data->header.entry_count = 0;
    return 0;
}

static int32_t wr_index(struct jls_wf_f32_s * self, uint8_t level) {
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
    uint32_t len = p_end - p_start;
    return jls_wr_index_prv(self->wr, self->def.signal_id, JLS_TRACK_TYPE_FSR, level, p_start, len);
}

static int32_t wr_summary(struct jls_wf_f32_s * self, uint8_t level) {
    struct level_s * dst = self->level[level];
    if (!dst->summary->header.entry_count) {
        return 0;
    }
    int64_t pos_next = jls_wr_tell_prv(self->wr);
    ROE(wr_index(self, level));

    uint8_t * p_start = (uint8_t *) dst->summary;
    uint8_t * p_end = (uint8_t *) &dst->summary->data[JLS_SUMMARY_FSR_COUNT * dst->summary->header.entry_count];
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

static int32_t summary_close(struct jls_wf_f32_s * self, uint8_t level) {
    struct level_s * dst = self->level[level];
    if (!dst) {
        return 0;
    }
    int32_t rc = wr_summary(self, level);
    summary_free(self, level);
    return rc;
}

int32_t jls_wf_f32_open(struct jls_wf_f32_s ** instance, struct jls_wr_s * wr, const struct jls_wf_f32_def_s * def) {
    struct jls_wf_f32_s * self = calloc(1, sizeof(struct jls_wf_f32_s));
    if (!self) {
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    self->wr = wr;
    self->def = *def;
    int32_t rc = jls_wf_f32_align_def(&self->def);
    if (rc) {
        jls_wf_f32_close(self);
        return rc;
    }

    *instance = self;
    return 0;
}

int32_t jls_wf_f32_close(struct jls_wf_f32_s * self) {
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

static int32_t summaryN(struct jls_wf_f32_s * self, uint8_t level, int64_t pos) {
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
    for (uint32_t idx = 0; idx < summaries_per; ++idx) {
        uint32_t sample_idx = idx * self->def.summary_decimate_factor;
        double v_mean = 0.0;
        float v_min = FLT_MAX;
        float v_max = -FLT_MAX;
        double v_var = 0.0;
        for (uint32_t sample = 0; sample < self->def.summary_decimate_factor; ++sample) {
            uint32_t offset = sample_idx * JLS_SUMMARY_FSR_COUNT;
            v_mean += src->summary->data[offset + JLS_SUMMARY_FSR_MEAN];
            if (src->summary->data[offset + JLS_SUMMARY_FSR_MIN] < v_min) {
                v_min = src->summary->data[offset + JLS_SUMMARY_FSR_MIN];
            }
            if (src->summary->data[offset + JLS_SUMMARY_FSR_MAX] > v_max) {
                v_max = src->summary->data[offset + JLS_SUMMARY_FSR_MAX];
            }
            ++sample_idx;
        }
        v_mean *= scale;
        sample_idx = idx * self->def.summary_decimate_factor;
        for (uint32_t sample = 0; sample < self->def.summary_decimate_factor; ++sample) {
            uint32_t offset = sample_idx * JLS_SUMMARY_FSR_COUNT;
            double v = src->summary->data[offset + JLS_SUMMARY_FSR_MEAN] - v_mean;
            double std = src->summary->data[offset + JLS_SUMMARY_FSR_STD];
            v_var += (std * std) + (v * v);
            ++sample_idx;
        }
        uint32_t dst_offset = dst->summary->header.entry_count * JLS_SUMMARY_FSR_COUNT;
        dst->summary->data[dst_offset + JLS_SUMMARY_FSR_MEAN] = (float) (v_mean);
        dst->summary->data[dst_offset + JLS_SUMMARY_FSR_MIN] = v_min;
        dst->summary->data[dst_offset + JLS_SUMMARY_FSR_MAX] = v_max;
        dst->summary->data[dst_offset + JLS_SUMMARY_FSR_STD] = (float) (sqrt(v_var * scale));
        ++dst->summary->header.entry_count;
    }

    if (dst->summary->header.entry_count >= dst->summary_entries) {
        ROE(wr_summary(self, level));
    }
    return 0;
}

static int32_t summary1(struct jls_wf_f32_s * self, int64_t pos) {
    const float * data = self->data->data;
    struct level_s * dst = self->level[1];

    if (!dst) {
        ROE(summary_alloc(self, 1));
        dst = self->level[1];
    }

    const double scale = 1.0 / self->def.sample_decimate_factor;
    // JLS_LOGI("1 add %" PRIi64 " @ %" PRIi64 " %p", pos, dst->index->offset, &dst->index->data[dst->index->offset]);
    dst->index->offsets[dst->index->header.entry_count++] = pos;

    uint32_t summaries_per = (uint32_t) (self->data->header.entry_count / self->def.sample_decimate_factor);
    for (uint32_t idx = 0; idx < summaries_per; ++idx) {
        uint32_t sample_idx = idx * self->def.sample_decimate_factor;
        double v_mean = 0.0;
        float v_min = FLT_MAX;
        float v_max = -FLT_MAX;
        double v_var = 0.0;
        for (uint32_t sample = 0; sample < self->def.sample_decimate_factor; ++sample) {
            float v = data[sample_idx];
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
        uint32_t dst_offset = dst->summary->header.entry_count * JLS_SUMMARY_FSR_COUNT;
        dst->summary->data[dst_offset + JLS_SUMMARY_FSR_MEAN] = (float) (v_mean);
        dst->summary->data[dst_offset + JLS_SUMMARY_FSR_MIN] = v_min;
        dst->summary->data[dst_offset + JLS_SUMMARY_FSR_MAX] = v_max;
        dst->summary->data[dst_offset + JLS_SUMMARY_FSR_STD] = (float) (sqrt(v_var * scale));
        ++dst->summary->header.entry_count;
    }

    if (dst->summary->header.entry_count >= dst->summary_entries) {
        ROE(wr_summary(self, 1));
    }
    return 0;
}

int32_t jls_wf_f32_data(struct jls_wf_f32_s * self, int64_t sample_id, const float * data, uint32_t data_length) {
    struct jls_fsr_f32_data_s * b = self->data;

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
        uint32_t length = (uint32_t) (self->data_length - b->header.entry_count);
        if (data_length < length) {
            length = data_length;
        }
        memcpy(&b->data[b->header.entry_count], data, length * sizeof(float));
        b->header.entry_count += length;
        data += length;
        data_length -= length;
        if (b->header.entry_count >= self->data_length) {
            ROE(wr_data(self));
        }
    }
    return 0;
}
