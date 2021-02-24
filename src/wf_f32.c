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

struct sample_buffer_s {  // for a single chunk
    uint32_t length;
    int64_t timestamp;
    int64_t offset;
    float data[];
};

struct summary_index_s {
    uint32_t length;
    int64_t timestamp;  // starting
    int64_t offset;    // number of valid entries
    int64_t data[];
};

struct summary_buffer_s {
    uint8_t level;
    uint32_t length;
    struct summary_index_s * index;
    int64_t timestamp;
    int64_t offset;
    float data[];    // mean, min, max, std
};

struct jls_wf_f32_s {
    struct jls_wr_s * wr;
    struct jls_wf_f32_def_s def;
    struct sample_buffer_s * sample_buffer;
    struct summary_buffer_s * summary[JLS_SUMMARY_LEVEL_COUNT - 1];
    int64_t sample_id_offset;
};

static int32_t summaryN(struct jls_wf_f32_s * self, uint8_t level, int64_t pos);
static int32_t summary1(struct jls_wf_f32_s * self, int64_t pos);

static int32_t sample_buffer_alloc(struct jls_wf_f32_s * self) {
    size_t sample_buffer_sz = sizeof(struct sample_buffer_s) + sizeof(float) * self->def.samples_per_data;
    self->sample_buffer = malloc(sample_buffer_sz);
    JLS_LOGD1("%d sample_buffer alloc %p", self->def.signal_id, self->sample_buffer);
    if (!self->sample_buffer) {
        jls_wf_f32_close(self);
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    self->sample_buffer->timestamp = 0;
    self->sample_buffer->offset = 0;
    self->sample_buffer->length = self->def.samples_per_data;
    return 0;
}

static void sample_buffer_free(struct jls_wf_f32_s * self) {
    if (self->sample_buffer) {
        free(self->sample_buffer);
    }
    self->sample_buffer = NULL;
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

    size_t buffer_sz = sizeof(struct summary_buffer_s) + 4 * self->def.entries_per_summary * sizeof(float);
    size_t index_sz = sizeof(struct summary_index_s) + index_entries * sizeof(int64_t);

    struct summary_buffer_s * b = malloc(buffer_sz);
    if (!b) {
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    b->level = level;
    b->length = JLS_SUMMARY_FSR_COUNT * self->def.entries_per_summary; // in floats
    b->offset = 0;
    b->index = calloc(1, index_sz);
    if (!b->index) {
        free(b);
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    b->timestamp = 0;
    b->index->length = index_entries;
    b->index->offset = 0;

    self->summary[level] = b;
    return 0;
}

static void summary_free(struct jls_wf_f32_s * self, uint8_t level) {
    if (self->summary[level]) {
        if (self->summary[level]->index) {
            free(self->summary[level]->index);
            self->summary[level]->index = NULL;
        }
        free(self->summary[level]);
        self->summary[level] = NULL;
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
    struct sample_buffer_s * b = self->sample_buffer;
    if (!b->offset) {
        return 0;
    }
    if (b->offset > b->length) {
        JLS_LOGE("internal memory error");
    }
    uint32_t payload_length = (uint32_t) (2 * sizeof(uint64_t) + b->offset * sizeof(float));
    int64_t pos = jls_wr_tell_prv(self->wr);
    ROE(jls_wr_data_prv(self->wr, self->def.signal_id, (uint8_t *) &b->timestamp, payload_length));
    ROE(summary1(self, pos));
    b->timestamp += self->def.samples_per_data;
    b->offset = 0;
    return 0;
}

static int32_t wr_index(struct jls_wf_f32_s * self, uint8_t level) {
    if (!self->summary[level]) {
        JLS_LOGW("No summary buffer, cannot write index");
        return 0;
    }
    struct summary_index_s * idx = self->summary[level]->index;
    if (!idx->offset) {
        return 0;
    }
    if (idx->offset > idx->length) {
        JLS_LOGE("internal memory error");
    }
    if (idx->offset < idx->length) {
        JLS_LOGI("wr_index fill level %d", (int) level);
        memset(&idx->data[idx->offset], 0, sizeof(int64_t) * (size_t) (idx->length - idx->offset));
    }
    uint32_t len = (uint32_t) ((2 + idx->offset) * sizeof(int64_t));
    return jls_wr_index_prv(self->wr, self->def.signal_id, level, (uint8_t *) &idx->timestamp, len);
}

static int32_t wr_summary(struct jls_wf_f32_s * self, uint8_t level) {
    struct summary_buffer_s * dst = self->summary[level];
    if (!dst->offset) {
        return 0;
    }
    if (dst->offset > dst->length) {
        JLS_LOGE("internal memory error");
    }
    int64_t pos_next = jls_wr_tell_prv(self->wr);
    ROE(wr_index(self, level));

    dst->offset /= JLS_SUMMARY_FSR_COUNT;  // 4 float32 values per entry
    uint32_t payload_len = (uint32_t) (2 * sizeof(int64_t) + dst->offset * 4 * sizeof(float));
    ROE(jls_wr_summary_prv(self->wr, self->def.signal_id, level, (uint8_t *) &dst->timestamp, payload_len));
    ROE(summaryN(self, level + 1, pos_next));

    // compute new timestamp for that level
    int64_t skip = self->def.entries_per_summary * self->def.sample_decimate_factor;
    if (level > 1) {
        skip *= self->def.summary_decimate_factor;
    }
    dst->index->timestamp += skip;
    dst->timestamp += skip;
    dst->offset = 0;
    dst->index->offset = 0;
    return 0;
}

static int32_t summary_close(struct jls_wf_f32_s * self, uint8_t level) {
    struct summary_buffer_s * dst = self->summary[level];
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
        if (self->sample_buffer) {
            wr_data(self);  // write remaining sample data
            JLS_LOGD1("%d sample_buffer free %p", (int) self->def.signal_id, self->sample_buffer);
            sample_buffer_free(self);
        }

        for (size_t i = 0; i < JLS_ARRAY_SIZE(self->summary); ++i) {
            summary_close(self, (uint8_t) i);
        }
    }
    return 0;
}

static int32_t summaryN(struct jls_wf_f32_s * self, uint8_t level, int64_t pos) {
    struct summary_buffer_s * src = self->summary[level - 1];
    struct summary_buffer_s * dst = self->summary[level];

    if (!dst) {
        ROE(summary_alloc(self, level));
        dst = self->summary[level];
    }

    const double scale = 1.0 / self->def.summary_decimate_factor;
    JLS_LOGD2("summaryN %d: %" PRIi64 " %" PRIi64, (int) level, dst->index->offset, pos);
    dst->index->data[dst->index->offset++] = pos;

    uint32_t summaries_per = (uint32_t) (src->offset / self->def.summary_decimate_factor);
    for (uint32_t idx = 0; idx < summaries_per; ++idx) {
        uint32_t sample_idx = idx * self->def.summary_decimate_factor;
        double v_mean = 0.0;
        float v_min = FLT_MAX;
        float v_max = -FLT_MAX;
        double v_var = 0.0;
        for (uint32_t sample = 0; sample < self->def.summary_decimate_factor; ++sample) {
            uint32_t offset = sample_idx * JLS_SUMMARY_FSR_COUNT;
            v_mean += src->data[offset + JLS_SUMMARY_FSR_MEAN];
            if (src->data[offset + JLS_SUMMARY_FSR_MIN] < v_min) {
                v_min = src->data[offset + JLS_SUMMARY_FSR_MIN];
            }
            if (src->data[offset + JLS_SUMMARY_FSR_MAX] > v_max) {
                v_max = src->data[offset + JLS_SUMMARY_FSR_MAX];
            }
            ++sample_idx;
        }
        v_mean *= scale;
        sample_idx = idx * self->def.summary_decimate_factor;
        for (uint32_t sample = 0; sample < self->def.summary_decimate_factor; ++sample) {
            uint32_t offset = sample_idx * JLS_SUMMARY_FSR_COUNT;
            double v = src->data[offset + JLS_SUMMARY_FSR_MEAN] - v_mean;
            double std = src->data[offset + JLS_SUMMARY_FSR_STD];
            v_var += (std * std) + (v * v);
            ++sample_idx;
        }
        dst->data[dst->offset + JLS_SUMMARY_FSR_MEAN] = (float) (v_mean);
        dst->data[dst->offset + JLS_SUMMARY_FSR_MIN] = v_min;
        dst->data[dst->offset + JLS_SUMMARY_FSR_MAX] = v_max;
        dst->data[dst->offset + JLS_SUMMARY_FSR_STD] = (float) (sqrt(v_var * scale));
        dst->offset += JLS_SUMMARY_FSR_COUNT;
    }

    if (dst->offset >= dst->length) {
        ROE(wr_summary(self, level));
    }
    return 0;
}

static int32_t summary1(struct jls_wf_f32_s * self, int64_t pos) {
    const float * data = self->sample_buffer->data;
    struct summary_buffer_s * dst = self->summary[1];

    if (!dst) {
        ROE(summary_alloc(self, 1));
        dst = self->summary[1];
    }

    const double scale = 1.0 / self->def.sample_decimate_factor;
    // JLS_LOGI("1 add %" PRIi64 " @ %" PRIi64 " %p", pos, dst->index->offset, &dst->index->data[dst->index->offset]);
    dst->index->data[dst->index->offset++] = pos;

    uint32_t summaries_per = (uint32_t) (self->sample_buffer->offset / self->def.sample_decimate_factor);
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
        dst->data[dst->offset + JLS_SUMMARY_FSR_MEAN] = (float) (v_mean);
        dst->data[dst->offset + JLS_SUMMARY_FSR_MIN] = v_min;
        dst->data[dst->offset + JLS_SUMMARY_FSR_MAX] = v_max;
        dst->data[dst->offset + JLS_SUMMARY_FSR_STD] = (float) (sqrt(v_var * scale));
        dst->offset += JLS_SUMMARY_FSR_COUNT;
    }

    if (dst->offset >= dst->length) {
        ROE(wr_summary(self, 1));
    }
    return 0;
}

int32_t jls_wf_f32_data(struct jls_wf_f32_s * self, int64_t sample_id, const float * data, uint32_t data_length) {
    struct sample_buffer_s * b = self->sample_buffer;

    if (!b) {
        ROE(sample_buffer_alloc(self));
        b = self->sample_buffer;
        self->sample_id_offset = sample_id;
    }
    sample_id -= self->sample_id_offset;

    // todo check for & handle sample_id skips
    if (!b->offset) {
        b->timestamp = sample_id;
    }

    while (data_length) {
        uint32_t length = (uint32_t) (b->length - b->offset);
        if (data_length < length) {
            length = data_length;
        }
        memcpy(b->data + b->offset, data, length * sizeof(float));
        b->offset += length;
        data += length;
        data_length -= length;
        if (b->offset >= b->length) {
            ROE(wr_data(self));
        }
    }
    return 0;
}
