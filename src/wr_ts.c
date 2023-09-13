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


enum commit_mode_e {
    COMMIT_MODE_NORMAL = 0,
    COMMIT_MODE_CLOSE
};

static void ts_free(struct jls_core_ts_s * self) {
    if (self) {
        for (int i = 0; i < JLS_SUMMARY_LEVEL_COUNT; ++i) {
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

static int32_t index_alloc(struct jls_core_ts_s * self, uint8_t level) {
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
    self->index[level] = index;
    return 0;
}

static int32_t summary_alloc(struct jls_core_ts_s * self, uint8_t level) {
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
    self->summary[level] = summary;
    return 0;
}

static int32_t alloc(struct jls_core_ts_s * self, uint8_t level) {
    ROE(index_alloc(self, level));
    ROE(summary_alloc(self, level));
    return 0;
}

int32_t jls_wr_ts_open(
        struct jls_core_ts_s ** instance,
        struct jls_core_signal_s * parent,
        enum jls_track_type_e track_type,
        uint32_t decimate_factor) {
    struct jls_core_ts_s * self = calloc(1, sizeof(struct jls_core_ts_s));
    if (!self) {
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    self->parent = parent;
    self->track_type = track_type;
    self->decimate_factor = decimate_factor;
    *instance = self;
    return 0;
}

static int32_t commit(struct jls_core_ts_s * self, int level, int mode) {
    if ((level < 1) || (level > JLS_SUMMARY_LEVEL_COUNT)) {
        JLS_LOGE("invalid level");
        return JLS_ERROR_PARAMETER_INVALID;
    }
    struct jls_index_s * index = self->index[level];
    struct jls_payload_header_s * summary_header = self->summary[level];

    if (!index || !summary_header || !index->header.entry_count) {
        return 0;
    } else if (mode == COMMIT_MODE_NORMAL) {
        ROE(alloc(self, level + 1));
    }

    // update headers
    index->header.timestamp = index->entries[0].timestamp;
    summary_header->timestamp = index->entries[0].timestamp;

    // write index
    uint8_t * p_end = (uint8_t *) &index->entries[index->header.entry_count];
    uint8_t * p_start = (uint8_t *) index;
    uint32_t len = (uint32_t) (p_end - p_start);
    uint64_t offset = jls_raw_chunk_tell(self->parent->parent->raw);
    ROE(jls_core_wr_index(self->parent->parent, self->parent->signal_def.signal_id,
                          self->track_type, level, p_start, len));

    // add to upper level and compute summary write
    struct jls_index_s * index_up = self->index[level + 1];
    struct jls_payload_header_s * summary_header_up = self->summary[level + 1];
    if (index_up) {
        struct jls_index_entry_s * index_up_entry = &index_up->entries[index_up->header.entry_count++];
        index_up_entry->timestamp = index->entries[0].timestamp;
        index_up_entry->offset = offset;
    }
    if (self->track_type == JLS_TRACK_TYPE_ANNOTATION) {
        struct jls_annotation_summary_s * summary = (struct jls_annotation_summary_s *) summary_header;
        p_end = (uint8_t *) &summary->entries[summary->header.entry_count];
        p_start = (uint8_t *) summary;
        if (mode != COMMIT_MODE_CLOSE) {
            struct jls_annotation_summary_s *summary_up = (struct jls_annotation_summary_s *) summary_header_up;
            summary_up->entries[summary_up->header.entry_count++] = summary->entries[0];
        }
    } else if (self->track_type == JLS_TRACK_TYPE_UTC) {
        struct jls_utc_summary_s * summary = (struct jls_utc_summary_s *) summary_header;
        p_end = (uint8_t *) &summary->entries[summary->header.entry_count];
        p_start = (uint8_t *) summary;
        if (mode != COMMIT_MODE_CLOSE) {
            struct jls_utc_summary_s *summary_up = (struct jls_utc_summary_s *) summary_header_up;
            summary_up->entries[summary_up->header.entry_count++] = summary->entries[0];
        }
    }

    // write summary.
    len = (uint32_t) (p_end - p_start);
    ROE(jls_core_wr_summary(self->parent->parent, self->parent->signal_def.signal_id,
                            self->track_type, level, p_start, len));

    // When up is full, commit it
    if (index_up && (index_up->header.entry_count >= self->decimate_factor)) {
        ROE(commit(self, level + 1, mode));
    }

    // Reset our entry count since all have been written.
    index->header.entry_count = 0;
    summary_header->entry_count = 0;
    return 0;
}

int32_t jls_wr_ts_close(struct jls_core_ts_s * self) {
    if (self) {
        for (uint8_t level = 1; level < JLS_SUMMARY_LEVEL_COUNT; ++level) {
            commit(self, level, COMMIT_MODE_CLOSE);
        }
        ts_free(self);
    }
    return 0;
}

int32_t jls_wr_ts_anno(struct jls_core_ts_s * self, int64_t timestamp, int64_t offset,
                       enum jls_annotation_type_e annotation_type, uint8_t group_id, float y) {
    if (self->track_type != JLS_TRACK_TYPE_ANNOTATION) {
        JLS_LOGE("track_type mismatch");
        return JLS_ERROR_PARAMETER_INVALID;
    }
    ROE(alloc(self, 1));
    struct jls_index_s * index = self->index[1];
    struct jls_annotation_summary_s * summary = (struct jls_annotation_summary_s *) self->summary[1];

    struct jls_index_entry_s * index_entry = &index->entries[index->header.entry_count++];
    index_entry->timestamp = timestamp;
    index_entry->offset = offset;

    struct jls_annotation_summary_entry_s * summary_entry = &summary->entries[summary->header.entry_count++];
    summary_entry->timestamp = timestamp;
    summary_entry->annotation_type = annotation_type;
    summary_entry->group_id = group_id;
    summary_entry->rsv8_1 = 0;
    summary_entry->rsv8_2 = 0;
    summary_entry->y = y;

    if (index->header.entry_count >= self->decimate_factor) {
        ROE(commit(self, 1, COMMIT_MODE_NORMAL));
    }

    return 0;
}

int32_t jls_wr_ts_utc(struct jls_core_ts_s * self, int64_t sample_id, int64_t offset, int64_t utc) {
    if (self->track_type != JLS_TRACK_TYPE_UTC) {
        JLS_LOGE("track_type mismatch");
        return JLS_ERROR_PARAMETER_INVALID;
    }
    ROE(alloc(self, 1));
    struct jls_index_s * index = self->index[1];
    struct jls_utc_summary_s * summary = (struct jls_utc_summary_s *) self->summary[1];

    struct jls_index_entry_s * index_entry = &index->entries[index->header.entry_count++];
    index_entry->timestamp = sample_id;
    index_entry->offset = offset;

    struct jls_utc_summary_entry_s * summary_entry = &summary->entries[summary->header.entry_count++];
    summary_entry->sample_id = sample_id;
    summary_entry->timestamp = utc;

    if (index->header.entry_count >= self->decimate_factor) {
        ROE(commit(self, 1, COMMIT_MODE_NORMAL));
    }

    return 0;
}
