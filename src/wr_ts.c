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

#include "jls/wr_ts.h"
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

struct jls_wr_ts_s {
    struct jls_wr_s * wr;
    uint16_t signal_id;
    enum jls_track_type_e track_type;
    uint32_t decimate_factor;
    struct jls_index_s * index[JLS_SUMMARY_LEVEL_COUNT - 1];
    struct jls_payload_header_s * summary[JLS_SUMMARY_LEVEL_COUNT - 1];
};

static void ts_free(struct jls_wr_ts_s * self) {
    if (self) {
        for (int i = 0; i < JLS_SUMMARY_LEVEL_COUNT - 1; ++i) {
            if (self->index[i]) {
                free(self->index[i]);
                self->index[i] = 0;
            }
            if (self->summary[i]) {
                free(self->summary[i]);
                self->summary[i] = 0;
            }
        }
        free(self);
    }
}

static int32_t index_alloc(struct jls_wr_ts_s * self, uint8_t level) {
    if ((level < 1) || (level >= JLS_SUMMARY_LEVEL_COUNT)) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    if (self->index[level]) {
        return 0;
    }
    size_t sz = sizeof(struct jls_payload_header_s) + sizeof(struct jls_index_entry_s) * self->decimate_factor;
    struct jls_index_s * index = malloc(sz);
    if (!index) {
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    index->header.timestamp = 0;
    index->header.entry_count = 0;
    index->header.entry_size_bits = sizeof(struct jls_index_entry_s) * 8;
    index->header.rsv16 = 0;
    self->index[level - 1] = index;
    return 0;
}

static int32_t summary_alloc(struct jls_wr_ts_s * self, uint8_t level) {
    if ((level < 1) || (level >= JLS_SUMMARY_LEVEL_COUNT)) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    if (self->summary[level]) {
        return 0;
    }
    size_t entry_sz = 0;
    switch (self->track_type) {
        case JLS_TRACK_TYPE_VSR:
            entry_sz = sizeof(float) * 5;  // duration, mean, std, min, max
            break;
        case JLS_TRACK_TYPE_ANNOTATION:
            entry_sz = sizeof(struct jls_annotation_summary_entry_s);
            break;
        case JLS_TRACK_TYPE_UTC:
            entry_sz = sizeof(struct jls_utc_summary_entry_s);
            break;
        default:
            JLS_LOGE("unsupported track type %d", (int) self->track_type);
            break;
    }
    size_t sz = sizeof(struct jls_payload_header_s) + self->decimate_factor * entry_sz;
    sz = ((sz + 7) / 8) * 8;
    struct jls_payload_header_s * summary = malloc(sz);
    if (!summary) {
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    summary->timestamp = 0;
    summary->entry_count = 0;
    summary->entry_size_bits = (uint16_t) (8 * entry_sz);
    summary->rsv16 = 0;
    self->summary[level - 1] = summary;
    return 0;
}

int32_t jls_wr_ts_open(
        struct jls_wr_ts_s ** instance,
        struct jls_wr_s * wr,
        uint16_t signal_id,
        enum jls_track_type_e track_type,
        uint32_t decimate_factor) {
    struct jls_wr_ts_s * self = calloc(1, sizeof(struct jls_wr_ts_s));
    if (!self) {
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    self->wr = wr;
    self->signal_id = signal_id;
    self->track_type = track_type;
    self->decimate_factor = decimate_factor;
    *instance = self;
    return 0;
}

static void ts_close(struct jls_wf_f32_s * self, uint8_t level) {
    // todo
}

int32_t jls_wf_ts_close(struct jls_wf_f32_s * self) {
    if (self) {
        for (uint8_t level = 1; level < JLS_SUMMARY_LEVEL_COUNT; ++level) {
            ts_close(self, level);
        }
        free(self);
    }
    return 0;
}

int32_t jls_wr_ts_anno(struct jls_wr_ts_s * self, int64_t timestamp, int64_t offset,
                       enum jls_annotation_type_e annotation_type, uint8_t group_id, float y) {
    // todo add index entry
    // todo add summary entry
    return 0;
}

int32_t jls_wr_ts_utc(struct jls_wr_ts_s * self, int64_t sample_id, int64_t offset, int64_t utc) {
    // todo add index entry
    // todo add summary entry
    return 0;
}