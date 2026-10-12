/*
 * Copyright 2021-2025 Jetperch LLC
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

#include "jls/reader.h"
#include "jls/core.h"
#include "jls/backend.h"
#include "jls/raw.h"
#include "jls/track.h"
#include "jls/format.h"
#include "jls/datatype.h"
#include "jls/ec.h"
#include "jls/log.h"
#include "jls/cdef.h"
#include "jls/tmap.h"
#include "jls/buffer.h"
#include "jls/statistics.h"
#include "jls/util.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <float.h>
#include <stdint.h>

#define SIGNAL_MASK  (0x0fff)
#define DECIMATE_PER_DURATION (25)


struct jls_rd_s {
    struct jls_core_s core;
};


#define GOE(x)  do { \
    rc = (x);                           \
    if (rc) {                           \
        goto exit;                      \
    }                                   \
} while (0)


/**
 * @brief Recover a file truncated by a writer crash.
 *
 * @param core The core instance, opened read-only at a file whose final
 *      chunk is not JLS_TAG_END.
 * @param path The file path, reopened for write then read.
 * @param pos The offset of the last valid chunk.
 * @return 0 or error code.  JLS_ERROR_TRUNCATED when the file cannot be
 *      reopened for write (e.g. read-only).  Interior damage propagates
 *      as JLS_ERROR_MESSAGE_INTEGRITY; the caller maps all other
 *      failures to JLS_ERROR_TRUNCATED.
 *
 * Handles only the tail-truncation damage class in O(crash tail).
 * All other corruption requires the offline jls_repair() operation.
 */
static int32_t rd_open_recover(struct jls_core_s * core, const char * path, int64_t pos) {
    int32_t rc = jls_raw_close(core->raw);  // frees raw even on error
    core->raw = NULL;
    ROE(rc);
    rc = jls_raw_open(&core->raw, path, "a");
    if (rc && (rc != JLS_ERROR_TRUNCATED)) {
        return JLS_ERROR_TRUNCATED;  // read-only or locked: cannot repair in place
    }

    struct jls_core_chunk_s tail;
    ROE(jls_core_truncate_tail(core, pos, &tail));

    ROE(jls_raw_chunk_seek(core->raw, sizeof(struct jls_file_header_s)));
    ROE(jls_core_scan_initial(core));
    ROE(jls_core_scan_sources(core));
    ROE(jls_core_scan_signals(core));

    if ((0 == tail.hdr.item_prev) && (tail.hdr.tag & JLS_TRACK_TAG_FLAG)) {
        // first chunk of its chain: the lost update is the track head offset
        uint8_t chunk_type = jls_core_tag_parse_track_chunk(tail.hdr.tag);
        uint16_t sig = tail.hdr.chunk_meta & SIGNAL_MASK;
        uint8_t level = (uint8_t) (tail.hdr.chunk_meta >> 12);
        if ((sig < JLS_SIGNAL_COUNT)
                && ((JLS_TRACK_CHUNK_DATA == chunk_type) || (JLS_TRACK_CHUNK_INDEX == chunk_type))) {
            uint8_t track_type = jls_core_tag_parse_track_type(tail.hdr.tag);
            struct jls_core_track_s * t = &core->signal_info[sig].tracks[track_type];
            if ((NULL != t->parent) && (0 == t->head_offsets[level])) {
                JLS_LOGI("restore lost head offset: signal %d track %d level %d @ %" PRIi64,
                         (int) sig, (int) track_type, (int) level, tail.offset);
                t->head_offsets[level] = tail.offset;  // persisted by jls_track_repair_pointers
            }
        }
    }

    for (uint16_t signal_idx = 0; signal_idx < JLS_SIGNAL_COUNT; ++signal_idx) {
        struct jls_core_signal_s * signal_info = &core->signal_info[signal_idx];
        if (signal_info->signal_def.signal_id != signal_idx) {
            continue;
        }
        JLS_LOGI("repair signal %d", (int) signal_info->signal_def.signal_id);
        for (int track_idx = 0; track_idx < JLS_TRACK_TYPE_COUNT; ++track_idx) {
            if (!jls_core_track_applicable(signal_info->signal_def.signal_type, (uint8_t) track_idx)) {
                continue;
            }
            if (NULL == signal_info->tracks[track_idx].parent) {
                continue;  // truncated before the track head was written: no data exists
            }
            ROE(jls_track_repair_pointers(&signal_info->tracks[track_idx]));
        }
    }

    if (core->user_data_head.offset) {
        if (JLS_ERROR_NOT_FOUND ==
                jls_core_repair_chain(core, core->user_data_head.offset, JLS_TAG_USER_DATA, -1)) {
            JLS_LOGW("user data head unreadable");
        } else if ((0 == jls_raw_chunk_seek(core->raw, core->user_data_head.offset))
                   && (0 == jls_core_rd_chunk(core))) {
            core->user_data_head.hdr = core->chunk_cur.hdr;  // refresh: clamp may have rewritten it
        }
    }

    ROE(jls_core_scan_fsr_sample_id(core));

    for (uint16_t signal_idx = 0; signal_idx < JLS_SIGNAL_COUNT; ++signal_idx) {
        struct jls_core_signal_s * signal_info = &core->signal_info[signal_idx];
        if (signal_info->signal_def.signal_id != signal_idx) {
            continue;
        }
        if (signal_info->signal_def.signal_type == JLS_SIGNAL_TYPE_FSR) {
            ROE(jls_core_repair_fsr(core, signal_idx));
            // todo repair annotation
            // todo repair UTC
        } else {
            // todo repair VSR
            // todo repair annotation
        }
    }

    ROE(jls_raw_seek_end(core->raw));  // END must append, never overwrite
    ROE(jls_core_wr_end(core));
    rc = jls_raw_close(core->raw);  // frees raw even on error
    core->raw = NULL;
    ROE(rc);
    return jls_raw_open(&core->raw, path, "r");
}

int32_t jls_rd_open(struct jls_rd_s ** instance, const char * path) {
    int32_t rc = 0;
    if (!instance) {
        return JLS_ERROR_PARAMETER_INVALID;
    }

    struct jls_rd_s *self = calloc(1, sizeof(struct jls_rd_s));
    if (!self) {
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }

    struct jls_core_s * core = &self->core;
    core->buf = jls_buf_alloc();
    if (!core->buf) {
        GOE(JLS_ERROR_NOT_ENOUGH_MEMORY);
    }

    core->rd_index_chunk.offset = 0;
    core->rd_index = jls_buf_alloc();
    if (!core->rd_index) {
        GOE(JLS_ERROR_NOT_ENOUGH_MEMORY);
    }

    core->rd_summary_chunk.offset = 0;
    core->rd_summary = jls_buf_alloc();
    if (!core->rd_summary) {
        GOE(JLS_ERROR_NOT_ENOUGH_MEMORY);
    }

    rc = jls_raw_open(&core->raw, path, "r");
    if (rc && (rc != JLS_ERROR_TRUNCATED)) {
        goto exit;
    }

    if (jls_core_rd_chunk_end(core)) {
        GOE(JLS_ERROR_EMPTY);  // no valid chunk found
    }
    int64_t pos = jls_raw_chunk_tell(core->raw);

    if (self->core.chunk_cur.hdr.tag == JLS_TAG_END) {
        // properly closed: strict scans, any damage requires offline jls_repair
        GOE(jls_raw_chunk_seek(core->raw, sizeof(struct jls_file_header_s)));
        GOE(jls_core_scan_initial(core));
        GOE(jls_core_scan_sources(core));
        GOE(jls_core_scan_signals(core));
    } else {
        JLS_LOGW("not properly closed");  // indices & summaries may be incomplete
        core->recovery_active = true;
        rc = rd_open_recover(core, path, pos);
        core->recovery_active = false;
        if (rc && (JLS_ERROR_MESSAGE_INTEGRITY != rc) && (JLS_ERROR_NOT_ENOUGH_MEMORY != rc)) {
            rc = JLS_ERROR_TRUNCATED;  // truncation recovery failed
        }
        GOE(rc);
    }

    for (uint16_t i = 0; i < JLS_SIGNAL_COUNT; ++i) {
        struct jls_core_signal_s * signal_info = &core->signal_info[i];
        if ((signal_info->signal_def.signal_id == i) && (JLS_SIGNAL_TYPE_FSR == signal_info->signal_def.signal_type)) {
            GOE(jls_fsr_open(&signal_info->track_fsr, signal_info));
        }
    }

    GOE(jls_core_scan_fsr_sample_id(core));
    *instance = self;
    return 0;

exit:
    jls_rd_close(self);
    return rc;
}

void jls_rd_close(struct jls_rd_s * self) {
    if (self) {
        struct jls_core_s * core = &self->core;
        if (NULL != core->raw) {
            for (size_t i = 0; i < JLS_SIGNAL_COUNT; ++i) {
                struct jls_core_signal_s *signal_info = &core->signal_info[i];
                jls_fsr_close(signal_info->track_fsr);
            }
            jls_raw_close(core->raw);
        }
        core->raw = NULL;
        jls_buf_free(core->buf);
        jls_buf_free(core->rd_index);
        jls_buf_free(core->rd_summary);
        jls_core_f64_buf_free(core->f64_stats_buf);
        core->f64_stats_buf = NULL;
        jls_core_f64_buf_free(core->f64_sample_buf);
        core->f64_sample_buf = NULL;
        free(self);
    }
}

int32_t jls_rd_sources(struct jls_rd_s * self, struct jls_source_def_s ** sources, uint16_t * count) {
    if (!self) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    return jls_core_sources(&self->core, sources, count);
}

int32_t jls_rd_signals(struct jls_rd_s * self, struct jls_signal_def_s ** signals, uint16_t * count) {
    if (!self) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    return jls_core_signals(&self->core, signals, count);
}

int32_t jls_rd_signal(struct jls_rd_s * self, uint16_t signal_id, struct jls_signal_def_s * signal) {
    if (!self) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    return jls_core_signal(&self->core, signal_id, signal);
}

JLS_API int32_t jls_rd_fsr_length(struct jls_rd_s * self, uint16_t signal_id, int64_t * samples) {
    if (!self) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    return jls_core_fsr_length(&self->core, signal_id, samples);
}

int32_t jls_rd_fsr(struct jls_rd_s * self, uint16_t signal_id, int64_t start_sample_id,
                   void * data, int64_t data_length) {
    if (!self) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    return jls_core_fsr(&self->core, signal_id, start_sample_id, data, data_length);
}

JLS_API int32_t jls_rd_fsr_f32(struct jls_rd_s * self, uint16_t signal_id, int64_t start_sample_id,
                               float * data, int64_t data_length) {
    if (!self) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    return jls_core_fsr_f32(&self->core, signal_id, start_sample_id, data, data_length);
}

// File-provided offsets may be hostile: wrap instead of signed overflow (UB).
static inline int64_t i64_add_wrap(int64_t a, int64_t b) {
    return (int64_t) ((uint64_t) a + (uint64_t) b);
}

static inline int64_t i64_sub_wrap(int64_t a, int64_t b) {
    return (int64_t) ((uint64_t) a - (uint64_t) b);
}

// A NaN mean marks a summary entry without finite samples: treat as empty.
static inline void f32_to_stats(struct jls_statistics_s * stats, const float * data, int64_t count) {
    if (isnan(data[JLS_SUMMARY_FSR_MEAN])) {
        jls_statistics_reset(stats);
        return;
    }
    stats->k = count;
    stats->mean = data[JLS_SUMMARY_FSR_MEAN];
    stats->min = data[JLS_SUMMARY_FSR_MIN];
    stats->max = data[JLS_SUMMARY_FSR_MAX];
    // The writer stores population std (divide by count), see format.h.
    stats->s = ((double) data[JLS_SUMMARY_FSR_STD]) * data[JLS_SUMMARY_FSR_STD] * count;
}

static inline void stats_to_f64(double * data, struct jls_statistics_s * stats) {
    if (0 == stats->k) {  // no finite samples
        data[JLS_SUMMARY_FSR_MEAN] = NAN;
        data[JLS_SUMMARY_FSR_MIN] = NAN;
        data[JLS_SUMMARY_FSR_MAX] = NAN;
        data[JLS_SUMMARY_FSR_STD] = NAN;
        return;
    }
    data[JLS_SUMMARY_FSR_MEAN] = stats->mean;
    data[JLS_SUMMARY_FSR_MIN] = stats->min;
    data[JLS_SUMMARY_FSR_MAX] = stats->max;
    data[JLS_SUMMARY_FSR_STD] = sqrt(jls_statistics_var(stats));
}

// var_divisor: count for stored summaries (population std, see format.h),
// count - 1 for this reader's own results (sample std).
static inline void f64_to_stats(struct jls_statistics_s * stats, const double * data, int64_t count,
                                int64_t var_divisor) {
    if (isnan(data[JLS_SUMMARY_FSR_MEAN])) {
        jls_statistics_reset(stats);
        return;
    }
    stats->k = count;
    stats->mean = data[JLS_SUMMARY_FSR_MEAN];
    stats->min = data[JLS_SUMMARY_FSR_MIN];
    stats->max = data[JLS_SUMMARY_FSR_MAX];
    double std = data[JLS_SUMMARY_FSR_STD];
    stats->s = (var_divisor > 0) ? (std * std * (double) var_divisor) : 0.0;
}

static int32_t rd_stats_chunk(struct jls_core_s * self, uint16_t signal_id, uint8_t level, bool * is_f32) {
    uint16_t metadata = (signal_id & SIGNAL_MASK) | (((uint16_t) level) << 12);
    ROE(jls_core_rd_chunk_validate(self, JLS_TAG_TRACK_FSR_SUMMARY, metadata));
    struct jls_fsr_f32_summary_s * s = (struct jls_fsr_f32_summary_s *) self->buf->start;
    if (s->header.entry_size_bits == JLS_SUMMARY_FSR_COUNT * sizeof(float) * 8) {
        *is_f32 = true;  // 32-bit float summaries
    } else if (s->header.entry_size_bits == JLS_SUMMARY_FSR_COUNT * sizeof(double) * 8) {
        *is_f32 = false; // 64-bit float summaries
    } else {
        JLS_LOGW("invalid summary entry size: %d", (int) s->header.entry_size_bits);
        return JLS_ERROR_MESSAGE_INTEGRITY;
    }
    uint64_t sz_bytes = ((uint64_t) s->header.entry_count * s->header.entry_size_bits + 7) / 8;
    if (!*is_f32 && s->header.entry_count
            && ((sizeof(struct jls_payload_header_s) + sz_bytes / 2) == self->buf->length)) {
        // Writers before 0.19.0 stored only the first half of each 64-bit summary.
        JLS_LOGI("legacy truncated 64-bit summary at %" PRIi64, self->chunk_cur.offset);
        return JLS_ERROR_UNSUPPORTED_FILE;
    }
    if ((0 == s->header.entry_count)
            || ((sizeof(struct jls_payload_header_s) + sz_bytes) > self->buf->length)) {
        JLS_LOGW("invalid summary chunk at %" PRIi64 ": entry_count=%" PRIu32,
                 self->chunk_cur.offset, s->header.entry_count);
        return JLS_ERROR_MESSAGE_INTEGRITY;
    }
    return 0;
}

// Derive the entry from self->buf on each use: nested reads reload (and may grow) it.
static void summary_to_stats(struct jls_core_s * self, bool is_f32, int64_t idx, int64_t count,
                             struct jls_statistics_s * stats) {
    if (is_f32) {
        f32_to_stats(stats, ((struct jls_fsr_f32_summary_s *) self->buf->start)->data[idx], count);
    } else {
        f64_to_stats(stats, ((struct jls_fsr_f64_summary_s *) self->buf->start)->data[idx], count, count);
    }
}

static int32_t fsr_statistics_level(struct jls_core_s * self, uint16_t signal_id,
                                    int64_t start_sample_id, int64_t increment, uint8_t level,
                                    double * data, int64_t data_length);

static int32_t fsr_statistics(struct jls_core_s * self, uint16_t signal_id,
                              int64_t start_sample_id, int64_t increment, uint8_t level,
                              double * data, int64_t data_length) {
    // start_sample_id in JLS units with possible non-zero offset
    JLS_LOGD2("fsr_f32_statistics(signal_id=%d, start_id=%" PRIi64 ", incr=%" PRIi64 ", level=%d, len=%" PRIi64 ")",
              (int) signal_id, start_sample_id, increment, (int) level, data_length);
    bool is_f32 = true;
    struct jls_signal_def_s * signal_def = &self->signal_info[signal_id].signal_def;
    int64_t step_size = jls_core_fsr_step_size(signal_def, level);  // <= increment, see level selection
    double f64_tmp4[JLS_SUMMARY_FSR_COUNT];
    const int64_t sample_id_offset = signal_def->sample_id_offset;

    ROE(jls_core_fsr_seek(self, signal_id, level, start_sample_id)); // returns the index
    ROE(jls_raw_chunk_next(self->raw));  // statistics
    int64_t pos = jls_raw_chunk_tell(self->raw);
    ROE(rd_stats_chunk(self, signal_id, level, &is_f32));

    struct jls_payload_header_s * hdr = (struct jls_payload_header_s *) self->buf->start;
    int64_t chunk_sample_id = hdr->timestamp;
    int64_t src_end = hdr->entry_count;
    if ((start_sample_id < chunk_sample_id)
            || ((((uint64_t) start_sample_id - (uint64_t) chunk_sample_id) / (uint64_t) step_size)
                >= (uint64_t) src_end)) {  // corrupt summary timestamp or index
        JLS_LOGW("invalid summary chunk at %" PRIi64 ": timestamp %" PRIi64 " for %" PRIi64,
                 pos, chunk_sample_id, start_sample_id);
        return JLS_ERROR_MESSAGE_INTEGRITY;
    }
    int64_t entry_offset = ((start_sample_id - chunk_sample_id + step_size - 1) / step_size);
    int64_t entry_sample_id = entry_offset * step_size + chunk_sample_id;

    struct jls_statistics_s stats_accum;
    jls_statistics_reset(&stats_accum);
    struct jls_statistics_s stats_next;

    int64_t incr_remaining = increment;

    if (entry_sample_id != start_sample_id) {
        int64_t incr = entry_sample_id - start_sample_id;
        // invalidates stats, need to reload, providing API sample_id
        ROE(jls_core_fsr_statistics(self, signal_id, i64_sub_wrap(start_sample_id, sample_id_offset),
                                    incr, f64_tmp4, 1));
        ROE(jls_raw_chunk_seek(self->raw, pos));
        ROE(rd_stats_chunk(self, signal_id, level, &is_f32));
        f64_to_stats(&stats_accum, f64_tmp4, incr, incr - 1);
        incr_remaining -= incr;
        start_sample_id += incr;
    }
    int64_t src_offset = entry_offset;

    while (data_length) {
        if (src_offset >= src_end) {
            int64_t item_next = (int64_t) self->chunk_cur.hdr.item_next;
            if (item_next) {
                if (item_next <= self->chunk_cur.offset) {
                    // chains always point forward; reject corrupt backward/cyclic links
                    JLS_LOGW("invalid item_next at %" PRIi64, self->chunk_cur.offset);
                    return JLS_ERROR_MESSAGE_INTEGRITY;
                }
                ROE(jls_raw_chunk_seek(self->raw, item_next));
                ROE(rd_stats_chunk(self, signal_id, level, &is_f32));
                hdr = (struct jls_payload_header_s *) self->buf->start;
                if (hdr->timestamp != start_sample_id) {
                    // summaries are contiguous: anything else silently misaligns entries
                    JLS_LOGW("invalid summary chunk at %" PRIi64 ": timestamp %" PRIi64 " != %" PRIi64,
                             item_next, hdr->timestamp, start_sample_id);
                    return JLS_ERROR_MESSAGE_INTEGRITY;
                }
                src_offset = 0;
                src_end = hdr->entry_count;
            } else if ((incr_remaining <= step_size) && (data_length == 1)) {
                // not a problem, will fetch from lower statistics
            } else {
                // this level ends early (recovered file): finish from the level below
                int64_t window_start = start_sample_id - (increment - incr_remaining);
                JLS_LOGI("summary level %d ends at %" PRIi64 ", use level %d",
                         (int) level, start_sample_id, (int) level - 1);
                return fsr_statistics_level(self, signal_id, window_start, increment, level - 1,
                                            data, data_length);
            }
        }

        if (incr_remaining <= step_size) {
            if (data_length == 1) {
                ROE(jls_core_fsr_statistics(self, signal_id, i64_sub_wrap(start_sample_id, sample_id_offset),
                                            incr_remaining, f64_tmp4, 1));
                f64_to_stats(&stats_next, f64_tmp4, incr_remaining, incr_remaining - 1);
            } else {
                summary_to_stats(self, is_f32, src_offset, incr_remaining, &stats_next);
            }
            jls_statistics_combine(&stats_accum, &stats_accum, &stats_next);
            stats_to_f64(data, &stats_accum);
            data += JLS_SUMMARY_FSR_COUNT;
            --data_length;
            if (!data_length) {
                break;  // self->buf may now hold another chunk; src_offset may be past src_end
            }
            int64_t incr = step_size - incr_remaining;
            if (incr == 0) {
                jls_statistics_reset(&stats_accum);
            } else {
                summary_to_stats(self, is_f32, src_offset, incr, &stats_accum);
            }
            incr_remaining = increment - incr;
        } else {
            summary_to_stats(self, is_f32, src_offset, step_size, &stats_next);
            jls_statistics_combine(&stats_accum, &stats_accum, &stats_next);
            incr_remaining -= step_size;
        }
        start_sample_id += step_size;
        ++src_offset;
    }
    return 0;
}

// Load the data chunk containing sample_id as f64 into f64_sample_buf.
static int32_t rd_data_f64(struct jls_core_s * self, uint16_t signal_id, int64_t sample_id,
                           int64_t * idx, int64_t * count) {
    struct jls_signal_def_s * signal_def = &self->signal_info[signal_id].signal_def;
    uint8_t entry_size_bits = jls_datatype_parse_size(signal_def->data_type);
    ROE(jls_core_rd_fsr_data0(self, signal_id, sample_id));
    struct jls_fsr_data_s * s = (struct jls_fsr_data_s *) self->buf->start;
    uint64_t n = s->header.entry_count;
    // Convert entry_count, never samples_per_data: the payload holds only entry_count.
    // Check alloc_size, not length: reconstructed omitted chunks do not set length.
    if ((s->header.entry_size_bits != entry_size_bits)
            || ((sizeof(struct jls_fsr_data_s) + (n * entry_size_bits + 7) / 8) > self->buf->alloc_size)) {
        JLS_LOGW("invalid data chunk: signal_id=%d, sample_id=%" PRIi64, (int) signal_id, sample_id);
        return JLS_ERROR_MESSAGE_INTEGRITY;
    }
    int64_t chunk_sample_id = s->header.timestamp;
    if ((sample_id < chunk_sample_id) || (((uint64_t) sample_id - (uint64_t) chunk_sample_id) >= n)) {
        // corrupt index: the chunk does not contain sample_id, cannot progress
        JLS_LOGW("data chunk does not contain sample: signal_id=%d, sample_id=%" PRIi64,
                 (int) signal_id, sample_id);
        return JLS_ERROR_MESSAGE_INTEGRITY;
    }
    ROE(jls_core_f64_buf_alloc((size_t) n, &self->f64_sample_buf));
    ROE(jls_dt_buffer_to_f64(&s->data[0], signal_def->data_type, self->f64_sample_buf->start, (size_t) n));
    *idx = (int64_t) ((uint64_t) sample_id - (uint64_t) chunk_sample_id);
    *count = (int64_t) n;
    return 0;
}

// Sample-accurate statistics from the level 0 sample data.
static int32_t fsr_statistics_samples(struct jls_core_s * self, uint16_t signal_id,
                                      int64_t start_sample_id, int64_t increment,
                                      double * data, int64_t data_length) {
    JLS_LOGD2("f32(signal_id=%d, start_id=%" PRIi64 ", incr=%" PRIi64 ", level=0, len=%" PRIi64 ")",
              (int) signal_id, start_sample_id, increment, data_length);
    if ((uint64_t) increment > (SIZE_MAX / sizeof(double))) {
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    ROE(jls_core_f64_buf_alloc((size_t) increment, &self->f64_stats_buf));

    int64_t src_idx = 0;
    int64_t src_count = 0;
    int64_t window_count = 0;
    int64_t finite_count = 0;
    struct jls_statistics_s stats;

    while (data_length > 0) {
        if (src_idx >= src_count) {
            ROE(rd_data_f64(self, signal_id, start_sample_id, &src_idx, &src_count));
        }
        double v = self->f64_sample_buf->start[src_idx++];
        if (isfinite(v)) {  // match the writer summaries, which skip non-finite samples
            self->f64_stats_buf->start[finite_count++] = v;
        }
        if (++window_count >= increment) {
            jls_statistics_compute_f64(&stats, self->f64_stats_buf->start, (uint64_t) finite_count);
            stats_to_f64(data, &stats);
            data += JLS_SUMMARY_FSR_COUNT;
            window_count = 0;
            finite_count = 0;
            --data_length;
        }
        ++start_sample_id;
    }
    return 0;
}

static int32_t fsr_statistics_level(struct jls_core_s * self, uint16_t signal_id,
                                    int64_t start_sample_id, int64_t increment, uint8_t level,
                                    double * data, int64_t data_length) {
    if (level) {
        int32_t rc = fsr_statistics(self, signal_id, start_sample_id, increment, level, data, data_length);
        if (rc != JLS_ERROR_UNSUPPORTED_FILE) {
            return rc;
        }
        // legacy truncated 64-bit summaries: compute exactly from the samples
    }
    return fsr_statistics_samples(self, signal_id, start_sample_id, increment, data, data_length);
}

int32_t jls_core_fsr_statistics(struct jls_core_s * self, uint16_t signal_id,
                              int64_t start_sample_id, int64_t increment,
                              double * data, int64_t data_length) {
    // API zero-based start_sample_id
    ROE(jls_core_signal_validate_typed(self, signal_id, JLS_SIGNAL_TYPE_FSR));
    if (increment <= 0) {
        JLS_LOGW("invalid increment: %" PRIi64, increment);
        return JLS_ERROR_PARAMETER_INVALID;
    } else if (data_length < 0) {
        JLS_LOGW("invalid length: %" PRIi64, data_length);
        return JLS_ERROR_PARAMETER_INVALID;
    } else if (data_length == 0) {
        return 0;
    } else if (!data) {
        return JLS_ERROR_PARAMETER_INVALID;
    } else if (start_sample_id < 0) {
        JLS_LOGW("invalid start_sample_id: %" PRIi64, start_sample_id);
        return JLS_ERROR_PARAMETER_INVALID;
    }

    int64_t samples = 0;
    ROE(jls_core_fsr_length(self, signal_id, &samples));
    // divide, never multiply: increment * data_length may overflow
    if ((start_sample_id > samples) || (increment > ((samples - start_sample_id) / data_length))) {
        JLS_LOGW("invalid length: start=%" PRIi64 " increment=%" PRIi64 " length=%" PRIi64 " > %" PRIi64,
                 start_sample_id, increment, data_length, samples);
        return JLS_ERROR_PARAMETER_INVALID;
    }
    struct jls_signal_def_s * signal_def = &self->signal_info[signal_id].signal_def;
    const int64_t * head_offsets = self->signal_info[signal_id].tracks[JLS_TRACK_TYPE_FSR].head_offsets;
    const int64_t summary_decimate_factor = signal_def->summary_decimate_factor;

    // Select the highest present summary level whose step fits the increment.
    // Bound by level count and overflow: hostile factors must not hang or wrap.
    uint8_t level = 0;
    int64_t sample_multiple_next = signal_def->sample_decimate_factor;
    int64_t duration = increment * data_length;  // <= samples, checked above
    while (((level + 1) < JLS_SUMMARY_LEVEL_COUNT) && head_offsets[level + 1]
            && (sample_multiple_next > 0)
            && (increment >= sample_multiple_next)
            && ((duration / DECIMATE_PER_DURATION) >= sample_multiple_next)) {
        ++level;
        if ((summary_decimate_factor <= 1) || (sample_multiple_next > (INT64_MAX / summary_decimate_factor))) {
            break;
        }
        sample_multiple_next *= summary_decimate_factor;
    }
    start_sample_id = i64_add_wrap(start_sample_id, signal_def->sample_id_offset); // JLS file sample_id
    return fsr_statistics_level(self, signal_id, start_sample_id, increment, level, data, data_length);
}

JLS_API int32_t jls_rd_fsr_statistics(struct jls_rd_s * self, uint16_t signal_id,
                                      int64_t start_sample_id, int64_t increment,
                                      double * data, int64_t data_length) {
    if (!self) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    return jls_core_fsr_statistics(&self->core, signal_id, start_sample_id, increment, data, data_length);
}

int32_t jls_core_annotations(struct jls_core_s * self, uint16_t signal_id, int64_t timestamp,
                             jls_rd_annotation_cbk_fn cbk_fn, void * cbk_user_data) {
    struct jls_annotation_s * annotation;
    if (!cbk_fn) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    ROE(jls_core_signal_validate(self, signal_id));
    struct jls_signal_def_s * signal_def = &self->signal_info[signal_id].signal_def;
    const int64_t sample_id_offset = signal_def->sample_id_offset;
    timestamp = i64_add_wrap(timestamp, sample_id_offset);

    int32_t rv = jls_core_ts_seek(self, signal_id, 0, JLS_TRACK_TYPE_ANNOTATION, timestamp);
    if (rv == JLS_ERROR_NOT_FOUND) {
        return 0;  // no annotations, and that's just fine
    } else if (rv) {
        return rv;
    }

    // iterate
    int64_t pos = jls_raw_chunk_tell(self->raw);
    int64_t pos_prev = 0;
    while (pos) {
        if (pos <= pos_prev) {
            // chains always point forward; reject corrupt backward/cyclic links
            JLS_LOGW("invalid item_next at %" PRIi64, pos_prev);
            return JLS_ERROR_MESSAGE_INTEGRITY;
        }
        pos_prev = pos;
        ROE(jls_raw_chunk_seek(self->raw, pos));
        ROE(jls_core_rd_chunk(self));
        if (self->chunk_cur.hdr.tag != JLS_TAG_TRACK_ANNOTATION_DATA) {
            JLS_LOGW("invalid annotation chunk tag at %" PRIi64, pos);
            return JLS_ERROR_MESSAGE_INTEGRITY;
        }
        // save before the callback: nested reader calls overwrite chunk_cur and buf
        pos = (int64_t) self->chunk_cur.hdr.item_next;
        annotation = (struct jls_annotation_s *) self->buf->start;
        const uint32_t data_offset = (uint32_t) offsetof(struct jls_annotation_s, data);  // sizeof pads
        if ((self->chunk_cur.hdr.payload_length < data_offset)
                || (annotation->data_size > (self->chunk_cur.hdr.payload_length - data_offset))) {
            JLS_LOGW("annotation data_size exceeds payload at %" PRIi64, pos_prev);
            return JLS_ERROR_MESSAGE_INTEGRITY;
        }
        annotation->timestamp = i64_sub_wrap(annotation->timestamp, sample_id_offset);
        if (cbk_fn(cbk_user_data, annotation)) {
            return 0;
        }
    }
    return 0;
}

JLS_API int32_t jls_rd_annotations(struct jls_rd_s * self, uint16_t signal_id, int64_t timestamp,
                                   jls_rd_annotation_cbk_fn cbk_fn, void * cbk_user_data) {
    if (!self) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    return jls_core_annotations(&self->core, signal_id, timestamp, cbk_fn, cbk_user_data);
}

int32_t jls_core_user_data(struct jls_core_s * self, jls_rd_user_data_cbk_fn cbk_fn, void * cbk_user_data) {
    int32_t rv;
    if (!cbk_fn) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    int64_t pos = self->user_data_head.hdr.item_next;
    int64_t pos_prev = 0;
    uint16_t chunk_meta;
    while (pos) {
        if (pos <= pos_prev) {
            // chains always point forward; reject corrupt backward/cyclic links
            JLS_LOGW("invalid item_next at %" PRIi64, pos_prev);
            return JLS_ERROR_MESSAGE_INTEGRITY;
        }
        pos_prev = pos;
        ROE(jls_raw_chunk_seek(self->raw, pos));
        ROE(jls_core_rd_chunk(self));
        if (self->chunk_cur.hdr.tag != JLS_TAG_USER_DATA) {
            JLS_LOGW("invalid user data chunk tag at %" PRIi64, pos);
            return JLS_ERROR_MESSAGE_INTEGRITY;
        }
        uint8_t storage_type = (uint8_t) ((self->chunk_cur.hdr.chunk_meta >> 12) & 0x0f);
        switch (storage_type) {
            case JLS_STORAGE_TYPE_BINARY:  // intentional fall-through
            case JLS_STORAGE_TYPE_STRING:  // intentional fall-through
            case JLS_STORAGE_TYPE_JSON:
                break;
            default:
                JLS_LOGW("invalid user data storage type at %" PRIi64, pos);
                return JLS_ERROR_MESSAGE_INTEGRITY;
        }
        chunk_meta = self->chunk_cur.hdr.chunk_meta & 0x0fff;
        // save before the callback: nested reader calls overwrite chunk_cur and buf
        pos = (int64_t) self->chunk_cur.hdr.item_next;
        rv = cbk_fn(cbk_user_data, chunk_meta, storage_type,
                    self->buf->start, self->chunk_cur.hdr.payload_length);
        if (rv) {  // iteration done
            return 0;
        }
    }
    return 0;
}

JLS_API int32_t jls_rd_user_data(struct jls_rd_s * self, jls_rd_user_data_cbk_fn cbk_fn, void * cbk_user_data) {
    if (!self) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    return jls_core_user_data(&self->core, cbk_fn, cbk_user_data);
}

int32_t jls_core_utc(struct jls_core_s * self, uint16_t signal_id, int64_t sample_id,
                     jls_rd_utc_cbk_fn cbk_fn, void * cbk_user_data) {
    struct jls_utc_summary_s * utc;
    if (!cbk_fn) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    ROE(jls_core_signal_validate_typed(self, signal_id, JLS_SIGNAL_TYPE_FSR));
    struct jls_signal_def_s * signal_def = &self->signal_info[signal_id].signal_def;
    const int64_t sample_id_offset = signal_def->sample_id_offset;
    sample_id = i64_add_wrap(sample_id, sample_id_offset);
    int32_t rv = jls_core_ts_seek(self, signal_id, 1, JLS_TRACK_TYPE_UTC, sample_id);
    if (rv == JLS_ERROR_NOT_FOUND) {
        return 0;  // no utc entries, and that's just fine
    } else if (rv) {
        return rv;
    }

    // iterate
    struct jls_chunk_header_s hdr;
    hdr.item_next = jls_raw_chunk_tell(self->raw);
    int64_t pos_prev = 0;

    while (hdr.item_next) {
        int64_t pos = (int64_t) hdr.item_next;
        if (pos <= pos_prev) {
            // chains always point forward; reject corrupt backward/cyclic links
            JLS_LOGW("invalid item_next at %" PRIi64, pos_prev);
            return JLS_ERROR_MESSAGE_INTEGRITY;
        }
        pos_prev = pos;
        ROE(jls_raw_chunk_seek(self->raw, pos));
        ROE(jls_raw_rd_header(self->raw, &hdr));
        if (hdr.tag == JLS_TAG_TRACK_UTC_DATA) {
            ROE(jls_core_rd_chunk(self));
            if (self->chunk_cur.hdr.payload_length < sizeof(struct jls_utc_data_s)) {
                JLS_LOGW("invalid utc data payload length at %" PRIi64, pos);
                return JLS_ERROR_MESSAGE_INTEGRITY;  // else reports stale buffer bytes
            }
            struct jls_utc_data_s * utc_data = (struct jls_utc_data_s *) self->buf->start;
            struct jls_utc_summary_entry_s entry = {
                .sample_id = i64_sub_wrap(utc_data->header.timestamp, sample_id_offset),
                .timestamp = utc_data->timestamp,
            };
            if (cbk_fn(cbk_user_data, &entry, 1)) {
                return 0;
            }
            continue;
        } else if (hdr.tag == JLS_TAG_TRACK_UTC_INDEX) {
            ROE(jls_core_rd_chunk(self));  // index payload
            struct jls_index_s * index = (struct jls_index_s *) self->buf->start;
            // compare counts: pointer arithmetic past the buffer is undefined behavior
            if ((self->buf->length < sizeof(index->header))
                    || (index->header.entry_count
                        > ((self->buf->length - sizeof(index->header)) / sizeof(index->entries[0])))) {
                JLS_LOGW("invalid utc index payload length at %" PRIi64, pos);
                return JLS_ERROR_MESSAGE_INTEGRITY;
            }
            int64_t tail_offset = 0;
            if ((0 == hdr.item_next) && (index->header.entry_count > 0)) {
                // final index: entries not yet summarized follow the last summarized data chunk
                tail_offset = (int64_t) index->entries[index->header.entry_count - 1].offset;
            }
            ROE(jls_core_rd_chunk(self));  // summary
            if (self->chunk_cur.hdr.tag != JLS_TAG_TRACK_UTC_SUMMARY) {
                JLS_LOGW("missing utc summary after index at %" PRIi64, pos);
                return JLS_ERROR_MESSAGE_INTEGRITY;
            }
            utc = (struct jls_utc_summary_s *) self->buf->start;
            if ((self->buf->length < sizeof(utc->header))
                    || (utc->header.entry_count
                        > ((self->buf->length - sizeof(utc->header)) / sizeof(utc->entries[0])))) {
                JLS_LOGW("invalid utc summary payload length at %" PRIi64, self->chunk_cur.offset);
                return JLS_ERROR_MESSAGE_INTEGRITY;
            }
            uint32_t idx = 0;
            for (; (idx < utc->header.entry_count) && (sample_id > utc->entries[idx].sample_id); ++idx) {
                // iterate
            }
            uint32_t size = utc->header.entry_count - idx;
            for (uint32_t entry_idx = idx; entry_idx < utc->header.entry_count; ++entry_idx) {
                utc->entries[entry_idx].sample_id = i64_sub_wrap(utc->entries[entry_idx].sample_id, sample_id_offset);
            }
            if (size) {
                // after this callback, use only locals: nested calls reload buf
                if (cbk_fn(cbk_user_data, utc->entries + idx, size)) {
                    return 0;
                }
            }
            if (tail_offset) {
                // continue with the unsummarized tail data chunks (crash-recovered files)
                int32_t rc = jls_raw_chunk_seek(self->raw, tail_offset);
                if (0 == rc) {
                    rc = jls_raw_rd_header(self->raw, &hdr);
                }
                if (JLS_ERROR_IO == rc) {
                    return rc;  // transient: a partial result must not look complete
                } else if (rc || (hdr.tag != JLS_TAG_TRACK_UTC_DATA)) {
                    return 0;  // tail not locatable; the summarized entries were reported
                }
                pos_prev = tail_offset;
            }
        } else {
            JLS_LOGW("invalid utc chunk tag at %" PRIi64, pos);
            return JLS_ERROR_MESSAGE_INTEGRITY;
        }
    }
    return 0;
}

JLS_API int32_t jls_rd_utc(struct jls_rd_s * self, uint16_t signal_id, int64_t sample_id,
                           jls_rd_utc_cbk_fn cbk_fn, void * cbk_user_data) {
    if (!self) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    return jls_core_utc(&self->core, signal_id, sample_id, cbk_fn, cbk_user_data);
}

static int32_t utc_load(struct jls_core_s * self, uint16_t signal_id) {
    ROE(jls_core_signal_validate_typed(self, signal_id, JLS_SIGNAL_TYPE_FSR));
    struct jls_core_signal_s * signal = &self->signal_info[signal_id];
    if ((NULL != signal->track_fsr) && (NULL != signal->track_fsr->tmap)) {
        return 0;
    }

    if (NULL == signal->track_fsr) {
        return JLS_ERROR_UNAVAILABLE;  // jls_rd_open creates track_fsr for every FSR signal
    }

    struct jls_signal_def_s * signal_def = &signal->signal_def;
    struct jls_tmap_s * tmap = jls_tmap_alloc(signal_def->sample_rate);
    if (NULL == tmap) {
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    int64_t sample_rate = signal_def->sample_rate;
    int64_t sample_start = -3600 * sample_rate;  // within the last hour
    int32_t rc = jls_core_utc(self, signal_id, sample_start, jls_tmap_add_cbk, tmap);
    if (rc) {
        jls_tmap_free(tmap);  // never cache a partial map: later calls would succeed with it
        return rc;
    }
    if (0 == jls_tmap_length(tmap)) {
        // no UTC entries found, force sample_id 0 to utc 0.
        jls_tmap_add(tmap, 0, 0);
    }
    signal->track_fsr->tmap = tmap;
    return 0;
}

#define fsr_tmap() \
    if (!self) { return JLS_ERROR_PARAMETER_INVALID; } \
    ROE(utc_load(&self->core, signal_id)); \
    struct jls_tmap_s * fsr = self->core.signal_info[signal_id].track_fsr->tmap


size_t jls_rd_tmap_length(struct jls_rd_s * self, uint16_t signal_id) {
    if (!self || utc_load(&self->core, signal_id)) {
        return 0;
    }
    return jls_tmap_length(self->core.signal_info[signal_id].track_fsr->tmap);
}

int32_t jls_rd_sample_id_to_timestamp(struct jls_rd_s * self, uint16_t signal_id,
                                               int64_t sample_id, int64_t * timestamp) {
    fsr_tmap();
    if (!timestamp) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    return jls_tmap_sample_id_to_timestamp(fsr, sample_id, timestamp);
}

int32_t jls_rd_timestamp_to_sample_id(struct jls_rd_s * self, uint16_t signal_id,
                                              int64_t timestamp, int64_t * sample_id) {
    fsr_tmap();
    if (!sample_id) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    return jls_tmap_timestamp_to_sample_id(fsr, timestamp, sample_id);
}

int32_t jls_rd_tmap_get(struct jls_rd_s * self, uint16_t signal_id, size_t index, struct jls_utc_summary_entry_s * entry) {
    fsr_tmap();
    if (!entry) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    return jls_tmap_get(fsr, index, entry);
}
