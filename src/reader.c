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
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <float.h>

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

    GOE(jls_core_scan_initial(core));
    GOE(jls_core_scan_sources(core));
    GOE(jls_core_scan_signals(core));

    if (jls_core_rd_chunk_end(core)) {
        return JLS_ERROR_EMPTY;  // no chunk found!
    }
    int64_t pos = jls_raw_chunk_tell(core->raw);

    if (self->core.chunk_cur.hdr.tag != JLS_TAG_END) {
        JLS_LOGW("not properly closed");  // indices & summaries may be incomplete
        GOE(jls_raw_close(core->raw));
        rc = jls_raw_open(&core->raw, path, "a");
        if (rc && (rc != JLS_ERROR_TRUNCATED)) {
            goto exit;
        }

        // find last full chunk and truncate remainder
        GOE(jls_raw_chunk_seek(core->raw, pos));
        GOE(jls_core_rd_chunk(core));
        GOE(jls_bk_truncate(jls_raw_backend(core->raw)));

        // rewrite last full chunk to update payload_prev_length
        GOE(jls_raw_chunk_seek(core->raw, pos));
        GOE(jls_raw_wr(core->raw, &core->chunk_cur.hdr, core->buf->cur));

        for (uint16_t signal_idx = 0; signal_idx < JLS_SIGNAL_COUNT; ++signal_idx) {
            struct jls_core_signal_s * signal_info = &core->signal_info[signal_idx];
            if (signal_info->signal_def.signal_id != signal_idx) {
                continue;
            }
            JLS_LOGI("repair signal %d", (int) signal_info->signal_def.signal_id);
            for (int track_idx = 0; track_idx < JLS_TRACK_TYPE_COUNT; ++track_idx) {
                if (NULL != signal_info->tracks[track_idx].parent) {
                    jls_track_repair_pointers(&signal_info->tracks[track_idx]);
                }
            }
        }

        GOE(jls_core_scan_fsr_sample_id(core));

        for (uint16_t signal_idx = 0; signal_idx < JLS_SIGNAL_COUNT; ++signal_idx) {
            struct jls_core_signal_s * signal_info = &core->signal_info[signal_idx];
            if (signal_info->signal_def.signal_id != signal_idx) {
                continue;
            }

            if (signal_info->signal_def.signal_type == JLS_SIGNAL_TYPE_FSR) {
                GOE(jls_core_repair_fsr(core, signal_idx));
                // todo repair annotation
                // todo repair UTC
            } else {
                // todo repair VSR
                // todo repair annotation
            }
        }

        GOE(jls_core_wr_end(core));
        GOE(jls_raw_close(core->raw));
        GOE(jls_raw_open(&core->raw, path, "r"));
    }

    for (uint16_t i = 0; i < JLS_SIGNAL_COUNT; ++i) {
        struct jls_core_signal_s * signal_info = &core->signal_info[i];
        if ((signal_info->signal_def.signal_id == i) && (JLS_SIGNAL_TYPE_FSR == signal_info->signal_def.signal_type)) {
            ROE(jls_fsr_open(&signal_info->track_fsr, signal_info));
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
    return jls_core_sources(&self->core, sources, count);
}

int32_t jls_rd_signals(struct jls_rd_s * self, struct jls_signal_def_s ** signals, uint16_t * count) {
    return jls_core_signals(&self->core, signals, count);
}

int32_t jls_rd_signal(struct jls_rd_s * self, uint16_t signal_id, struct jls_signal_def_s * signal) {
    return jls_core_signal(&self->core, signal_id, signal);
}

JLS_API int32_t jls_rd_fsr_length(struct jls_rd_s * self, uint16_t signal_id, int64_t * samples) {
    return jls_core_fsr_length(&self->core, signal_id, samples);
}

int32_t jls_rd_fsr(struct jls_rd_s * self, uint16_t signal_id, int64_t start_sample_id,
                   void * data, int64_t data_length) {
    return jls_core_fsr(&self->core, signal_id, start_sample_id, data, data_length);
}

JLS_API int32_t jls_rd_fsr_f32(struct jls_rd_s * self, uint16_t signal_id, int64_t start_sample_id,
                               float * data, int64_t data_length) {
    return jls_core_fsr_f32(&self->core, signal_id, start_sample_id, data, data_length);
}

static inline void f32_to_stats(struct jls_statistics_s * stats, const float * data, int64_t count) {
    stats->k = count;
    stats->mean = data[JLS_SUMMARY_FSR_MEAN];
    stats->min = data[JLS_SUMMARY_FSR_MIN];
    stats->max = data[JLS_SUMMARY_FSR_MAX];
    if (count > 1) {
        stats->s = ((double) data[JLS_SUMMARY_FSR_STD]) * data[JLS_SUMMARY_FSR_STD] * (count - 1);
    } else {
        stats->s = 0.0;
    }
}

static inline void stats_to_f64(double * data, struct jls_statistics_s * stats) {
    data[JLS_SUMMARY_FSR_MEAN] = stats->mean;
    data[JLS_SUMMARY_FSR_MIN] = stats->min;
    data[JLS_SUMMARY_FSR_MAX] = stats->max;
    data[JLS_SUMMARY_FSR_STD] = sqrt(jls_statistics_var(stats));
}

static inline void f64_to_stats(struct jls_statistics_s * stats, const double * data, int64_t count) {
    stats->k = count;
    stats->mean = data[JLS_SUMMARY_FSR_MEAN];
    stats->min = data[JLS_SUMMARY_FSR_MIN];
    stats->max = data[JLS_SUMMARY_FSR_MAX];
    if (count > 1) {
        stats->s = ((double) data[JLS_SUMMARY_FSR_STD]) * data[JLS_SUMMARY_FSR_STD] * (count - 1);
    } else {
        stats->s = 0.0;
    }
}

static int32_t rd_stats_chunk(struct jls_core_s * self, uint16_t signal_id, uint8_t level) {
    ROE(jls_core_rd_chunk(self));
    if (JLS_TAG_TRACK_FSR_SUMMARY != self->chunk_cur.hdr.tag) {
        JLS_LOGW("unexpected chunk tag %d at %" PRIi64, (int) self->chunk_cur.hdr.tag, self->chunk_cur.offset);
        return JLS_ERROR_IO;
    }
    uint16_t metadata = (signal_id & SIGNAL_MASK) | (((uint16_t) level) << 12);
    if (metadata != self->chunk_cur.hdr.chunk_meta) {
        JLS_LOGW("unexpected chunk meta 0x%04x", (unsigned int) self->chunk_cur.hdr.chunk_meta);
        return JLS_ERROR_IO;
    }

    /*
    // display stats chunk data
    int64_t *i64 = (int64_t *) self->payload.start;
    JLS_LOGI("stats chunk: sample_id=%" PRIi64 ", entries=%" PRIi64, i64[0], i64[1]);
    float * d = (float *) &i64[2];
    for (int64_t i = 0; i < i64[1] * 4; i += 4) {
        JLS_LOGI("stats: mean=%f min=%f max=%f std=%f",
            d[i + JLS_SUMMARY_FSR_MEAN],
            d[i + JLS_SUMMARY_FSR_MIN],
            d[i + JLS_SUMMARY_FSR_MAX],
            d[i + JLS_SUMMARY_FSR_STD]);
    }
    */
    return 0;
}

static int32_t fsr_statistics(struct jls_core_s * self, uint16_t signal_id,
                              int64_t start_sample_id, int64_t increment, uint8_t level,
                              double * data, int64_t data_length) {
    // start_sample_id in JLS units with possible non-zero offset
    JLS_LOGD2("fsr_f32_statistics(signal_id=%d, start_id=%" PRIi64 ", incr=%" PRIi64 ", level=%d, len=%" PRIi64 ")",
              (int) signal_id, start_sample_id, increment, (int) level, data_length);
    bool is_f32;
    struct jls_signal_def_s * signal_def = &self->signal_info[signal_id].signal_def;
    int64_t step_size = signal_def->sample_decimate_factor;
    for (uint8_t lvl = 2; lvl <= level; ++lvl) {
        step_size *= signal_def->summary_decimate_factor;
    }
    double f64_tmp4[JLS_SUMMARY_FSR_COUNT];
    const int64_t sample_id_offset = signal_def->sample_id_offset;

    ROE(jls_core_fsr_seek(self, signal_id, level, start_sample_id)); // returns the index
    ROE(jls_raw_chunk_next(self->raw));  // statistics
    int64_t pos = jls_raw_chunk_tell(self->raw);
    ROE(rd_stats_chunk(self, signal_id, level));

    struct jls_fsr_f32_summary_s * f32_summary = (struct jls_fsr_f32_summary_s *) self->buf->start;
    struct jls_fsr_f64_summary_s * f64_summary = (struct jls_fsr_f64_summary_s *) self->buf->start;
    int64_t chunk_sample_id = f32_summary->header.timestamp;
    if (f32_summary->header.entry_size_bits == JLS_SUMMARY_FSR_COUNT * sizeof(float) * 8) {
        is_f32 = true;  // 32-bit float summaries
    } else if (f32_summary->header.entry_size_bits == JLS_SUMMARY_FSR_COUNT * sizeof(double) * 8) {
        is_f32 = false; // 64-bit float summaries
    } else {
        JLS_LOGE("invalid summary entry size: %d", (int) f32_summary->header.entry_size_bits);
        return JLS_ERROR_PARAMETER_INVALID;
    }
    int64_t src_offset = 0;
    int64_t src_end = f32_summary->header.entry_count;
    int64_t entry_offset = ((start_sample_id - chunk_sample_id + step_size - 1) / step_size);
    int64_t entry_sample_id = entry_offset * step_size + chunk_sample_id;

    struct jls_statistics_s stats_accum;
    jls_statistics_reset(&stats_accum);
    struct jls_statistics_s stats_next;

    int64_t incr_remaining = increment;

    if (entry_sample_id != start_sample_id) {
        int64_t incr = entry_sample_id - start_sample_id;
        // invalidates stats, need to reload, providing API sample_id
        ROE(jls_core_fsr_statistics(self, signal_id, start_sample_id - sample_id_offset,
                                  incr, f64_tmp4, 1));
        ROE(jls_raw_chunk_seek(self->raw, pos));
        ROE(rd_stats_chunk(self, signal_id, level));
        f64_to_stats(&stats_accum, f64_tmp4, incr);
        incr_remaining -= incr;
        start_sample_id += incr;
    }
    src_offset += entry_offset;

    while (data_length) {
        if (src_offset >= src_end) {
            if (self->chunk_cur.hdr.item_next) {
                ROE(jls_raw_chunk_seek(self->raw, self->chunk_cur.hdr.item_next));
                ROE(rd_stats_chunk(self, signal_id, level));
                f32_summary = (struct jls_fsr_f32_summary_s *) self->buf->start;
                f64_summary = (struct jls_fsr_f64_summary_s *) self->buf->start;
                if (f32_summary->header.entry_size_bits == JLS_SUMMARY_FSR_COUNT * sizeof(float) * 8) {
                    is_f32 = true;  // 32-bit float summaries
                } else if (f32_summary->header.entry_size_bits == JLS_SUMMARY_FSR_COUNT * sizeof(double) * 8) {
                    is_f32 = false; // 64-bit float summaries
                } else {
                    JLS_LOGE("invalid summary entry size: %d", (int) f32_summary->header.entry_size_bits);
                    return JLS_ERROR_PARAMETER_INVALID;
                }
                src_offset = 0;
                src_end = f32_summary->header.entry_count;
            } else {
                if ((incr_remaining <= step_size) && (data_length == 1)) {
                    // not a problem, will fetch from lower statistics
                } else {
                    JLS_LOGW("cannot get final %" PRIi64 " samples", data_length);
                    for (int64_t idx = 0; idx < (JLS_SUMMARY_FSR_COUNT * data_length); ++idx) {
                        data[idx] = NAN;
                    }
                    return JLS_ERROR_PARAMETER_INVALID;
                }
            }
        }

        if (incr_remaining <= step_size) {
            if (data_length == 1) {
                ROE(jls_core_fsr_statistics(self, signal_id, start_sample_id - sample_id_offset,
                                            incr_remaining, f64_tmp4, 1));
                f64_to_stats(&stats_next, f64_tmp4, incr_remaining);
            } else if (is_f32) {
                f32_to_stats(&stats_next, f32_summary->data[src_offset], incr_remaining);
            } else {
                f64_to_stats(&stats_next, f64_summary->data[src_offset], incr_remaining);
            }
            jls_statistics_combine(&stats_accum, &stats_accum, &stats_next);
            stats_to_f64(data, &stats_accum);
            data += JLS_SUMMARY_FSR_COUNT;
            --data_length;
            int64_t incr = step_size - incr_remaining;
            if (incr < 0) {
                JLS_LOGE("internal error");
                incr = 0;
                jls_statistics_reset(&stats_accum);
            } else if (incr == 0) {
                jls_statistics_reset(&stats_accum);
            } else if (is_f32) {
                f32_to_stats(&stats_accum, f32_summary->data[src_offset], incr);
            } else {
                f64_to_stats(&stats_accum, f64_summary->data[src_offset], incr);
            }
            incr_remaining = increment - incr;
        } else {
            if (is_f32) {
                f32_to_stats(&stats_next, f32_summary->data[src_offset], step_size);
            } else {
                f64_to_stats(&stats_next, f64_summary->data[src_offset], step_size);
            }
            jls_statistics_combine(&stats_accum, &stats_accum, &stats_next);
            incr_remaining -= step_size;
        }
        start_sample_id += step_size;
        ++src_offset;
    }
    return 0;
}

int32_t jls_core_fsr_statistics(struct jls_core_s * self, uint16_t signal_id,
                              int64_t start_sample_id, int64_t increment,
                              double * data, int64_t data_length) {
    // API zero-based start_sample_id
    ROE(jls_core_signal_validate_typed(self, signal_id, JLS_SIGNAL_TYPE_FSR));
    if (increment <= 0) {
        JLS_LOGW("invalid increment: %" PRIi64, increment);
        return JLS_ERROR_PARAMETER_INVALID;
    } else if (data_length <= 0) {
        JLS_LOGW("invalid length: %" PRIi64, data_length);
        return 0;
    } else if (start_sample_id < 0) {
        JLS_LOGW("invalid start_sample_id: %" PRIi64, start_sample_id);
        return JLS_ERROR_PARAMETER_INVALID;
    }

    int64_t samples = 0;
    ROE(jls_core_fsr_length(self, signal_id, &samples));
    int64_t end_sample_id = start_sample_id + increment * data_length;
    if (end_sample_id > samples) {
        JLS_LOGW("invalid length: %" PRIi64 " > %" PRIi64, end_sample_id, samples);
        return JLS_ERROR_PARAMETER_INVALID;
    }
    struct jls_signal_def_s * signal_def = &self->signal_info[signal_id].signal_def;
    const int64_t sample_id_offset = signal_def->sample_id_offset;

    uint8_t level = 0;
    int64_t sample_multiple_next = signal_def->sample_decimate_factor;
    int64_t duration = increment * data_length;
    while ((increment >= sample_multiple_next)
            && (duration >= (DECIMATE_PER_DURATION * sample_multiple_next))) {
        ++level;
        sample_multiple_next *= signal_def->summary_decimate_factor;
    }
    start_sample_id += sample_id_offset; // JLS file sample_id

    if (level) {  // use summaries
        return fsr_statistics(self, signal_id, start_sample_id, increment, level, data, data_length);
    }  // else, use sample data
    JLS_LOGD2("f32(signal_id=%d, start_id=%" PRIi64 ", incr=%" PRIi64 ", level=0, len=%" PRIi64 ")",
              (int) signal_id, start_sample_id, increment, data_length);

    ROE(jls_core_f64_buf_alloc((size_t) increment, &self->f64_stats_buf));
    ROE(jls_core_f64_buf_alloc((size_t) signal_def->samples_per_data, &self->f64_sample_buf));
    int64_t buf_offset = 0;
    uint8_t entry_size_bits = jls_datatype_parse_size(signal_def->data_type);
    if (entry_size_bits > 32) {
        JLS_LOGE("entry_size > 64 (float64 stats) not yet supported");
        return JLS_ERROR_UNSUPPORTED_FILE;
    }

    ROE(jls_core_rd_fsr_data0(self, signal_id, start_sample_id));
    struct jls_fsr_data_s * s = (struct jls_fsr_data_s *) self->buf->start;
    int64_t chunk_sample_id = s->header.timestamp;
    if (s->header.entry_size_bits != entry_size_bits) {
        JLS_LOGE("invalid data entry size: %d", (int) s->header.entry_size_bits);
        return JLS_ERROR_PARAMETER_INVALID;
    }
    jls_dt_buffer_to_f64(&s->data[0], signal_def->data_type, self->f64_sample_buf->start, signal_def->samples_per_data);
    double * src = &self->f64_sample_buf->start[0];
    double * src_end = &self->f64_sample_buf->start[s->header.entry_count];
    if (start_sample_id > chunk_sample_id) {
        src += start_sample_id - chunk_sample_id;
    }
    double v_mean = 0.0;
    double v_min = DBL_MAX;
    double v_max = -DBL_MAX;
    double v_var = 0.0;
    double mean_scale = 1.0 / increment;
    double var_scale = 1.0;
    if (increment > 1) {
        var_scale = 1.0 / (increment - 1.0);
    }
    double v;

    while (data_length > 0) {
        if (src >= src_end) {
            ROE(jls_core_rd_fsr_data0(self, signal_id, start_sample_id));
            s = (struct jls_fsr_data_s *) self->buf->start;
            chunk_sample_id = s->header.timestamp;
            jls_dt_buffer_to_f64(&s->data[0], signal_def->data_type, self->f64_sample_buf->start, signal_def->samples_per_data);
            src = &self->f64_sample_buf->start[0];
            src_end = &self->f64_sample_buf->start[s->header.entry_count];
        }
        v = *src++;
        v_mean += v;
        if (v < v_min) {
            v_min = v;
        }
        if (v > v_max) {
            v_max = v;
        }
        self->f64_stats_buf->start[buf_offset++] = v;

        if (buf_offset >= increment) {
            v_mean *= mean_scale;
            v_var = 0.0;
            for (int64_t i = 0; i < increment; ++i) {
                double v_diff = self->f64_stats_buf->start[i] - v_mean;
                v_var += v_diff * v_diff;
            }
            v_var *= var_scale;

            data[JLS_SUMMARY_FSR_MEAN] = v_mean;
            data[JLS_SUMMARY_FSR_MIN] = v_min;
            data[JLS_SUMMARY_FSR_MAX] = v_max;
            data[JLS_SUMMARY_FSR_STD] = sqrt(v_var);
            data += JLS_SUMMARY_FSR_COUNT;

            buf_offset = 0;
            v_mean = 0.0;
            v_min = DBL_MAX;
            v_max = -DBL_MAX;
            --data_length;
        }
        ++start_sample_id;
    }
    return 0;
}

JLS_API int32_t jls_rd_fsr_statistics(struct jls_rd_s * self, uint16_t signal_id,
                                      int64_t start_sample_id, int64_t increment,
                                      double * data, int64_t data_length) {
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
    timestamp += sample_id_offset;

    int32_t rv = jls_core_ts_seek(self, signal_id, 0, JLS_TRACK_TYPE_ANNOTATION, timestamp);
    if (rv == JLS_ERROR_NOT_FOUND) {
        return 0;  // no annotations, and that's just fine
    } else if (rv) {
        return rv;
    }

    // iterate
    int64_t pos = jls_raw_chunk_tell(self->raw);
    while (pos) {
        ROE(jls_raw_chunk_seek(self->raw, pos));
        ROE(jls_core_rd_chunk(self));
        if (self->chunk_cur.hdr.tag != JLS_TAG_TRACK_ANNOTATION_DATA) {
            return JLS_ERROR_NOT_FOUND;
        }
        annotation = (struct jls_annotation_s *) self->buf->start;
        annotation->timestamp -= sample_id_offset;
        if (cbk_fn(cbk_user_data, annotation)) {
            return 0;
        }
        pos = self->chunk_cur.hdr.item_next;
    }
    return 0;
}

JLS_API int32_t jls_rd_annotations(struct jls_rd_s * self, uint16_t signal_id, int64_t timestamp,
                                   jls_rd_annotation_cbk_fn cbk_fn, void * cbk_user_data) {
    return jls_core_annotations(&self->core, signal_id, timestamp, cbk_fn, cbk_user_data);
}

int32_t jls_core_user_data(struct jls_core_s * self, jls_rd_user_data_cbk_fn cbk_fn, void * cbk_user_data) {
    int32_t rv;
    if (!cbk_fn) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    int64_t pos = self->user_data_head.hdr.item_next;
    uint16_t chunk_meta;
    while (pos) {
        ROE(jls_raw_chunk_seek(self->raw, pos));
        ROE(jls_core_rd_chunk(self));
        if (self->chunk_cur.hdr.tag != JLS_TAG_USER_DATA) {
            return JLS_ERROR_NOT_FOUND;
        }
        uint8_t storage_type = (uint8_t) ((self->chunk_cur.hdr.chunk_meta >> 12) & 0x0f);
        switch (storage_type) {
            case JLS_STORAGE_TYPE_BINARY:  // intentional fall-through
            case JLS_STORAGE_TYPE_STRING:  // intentional fall-through
            case JLS_STORAGE_TYPE_JSON:
                break;
            default:
                return JLS_ERROR_PARAMETER_INVALID;
        }
        chunk_meta = self->chunk_cur.hdr.chunk_meta & 0x0fff;
        rv = cbk_fn(cbk_user_data, chunk_meta, storage_type,
                    self->buf->start, self->chunk_cur.hdr.payload_length);
        if (rv) {  // iteration done
            return 0;
        }
        pos = self->chunk_cur.hdr.item_next;
    }
    return 0;
}

JLS_API int32_t jls_rd_user_data(struct jls_rd_s * self, jls_rd_user_data_cbk_fn cbk_fn, void * cbk_user_data) {
    return jls_core_user_data(&self->core, cbk_fn, cbk_user_data);
}

int32_t jls_core_utc(struct jls_core_s * self, uint16_t signal_id, int64_t sample_id,
                     jls_rd_utc_cbk_fn cbk_fn, void * cbk_user_data) {
    struct jls_utc_summary_s * utc;
    if (!cbk_fn) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    ROE(jls_core_signal_validate(self, signal_id));
    struct jls_signal_def_s * signal_def = &self->signal_info[signal_id].signal_def;
    const int64_t sample_id_offset = signal_def->sample_id_offset;
    sample_id += sample_id_offset;
    int32_t rv = jls_core_ts_seek(self, signal_id, 1, JLS_TRACK_TYPE_UTC, sample_id);
    if (rv == JLS_ERROR_NOT_FOUND) {
        return 0;  // no utc entries, and that's just fine
    } else if (rv) {
        return rv;
    }

    // iterate
    struct jls_chunk_header_s hdr;
    hdr.item_next = jls_raw_chunk_tell(self->raw);

    while (hdr.item_next) {
        ROE(jls_raw_chunk_seek(self->raw, hdr.item_next));
        ROE(jls_raw_rd_header(self->raw, &hdr));
        if (hdr.tag == JLS_TAG_TRACK_UTC_DATA) {
            ROE(jls_core_rd_chunk(self));
            struct jls_utc_data_s * utc_data = (struct jls_utc_data_s *) self->buf->start;
            struct jls_utc_summary_entry_s entry = {
                .sample_id = utc_data->header.timestamp - sample_id_offset,
                .timestamp = utc_data->timestamp,
            };
            if (cbk_fn(cbk_user_data, &entry, 1)) {
                return 0;
            }
            continue;
        } else if (hdr.tag == JLS_TAG_TRACK_UTC_INDEX) {
            ROE(jls_raw_chunk_next(self->raw));
            ROE(jls_core_rd_chunk(self));
            if (self->chunk_cur.hdr.tag != JLS_TAG_TRACK_UTC_SUMMARY) {
                return JLS_ERROR_NOT_FOUND;
            }
            utc = (struct jls_utc_summary_s *) self->buf->start;
            uint32_t idx = 0;
            for (; (idx < utc->header.entry_count) && (sample_id > utc->entries[idx].sample_id); ++idx) {
                // iterate
            }
            uint32_t size = utc->header.entry_count - idx;
            for (uint32_t entry_idx = idx; entry_idx < utc->header.entry_count; ++entry_idx) {
                utc->entries[entry_idx].sample_id -= sample_id_offset;
            }
            if (size) {
                if (cbk_fn(cbk_user_data, utc->entries + idx, size)) {
                    return 0;
                }
            }
        } else {
            return JLS_ERROR_NOT_FOUND;
        }
    }
    return 0;
}

JLS_API int32_t jls_rd_utc(struct jls_rd_s * self, uint16_t signal_id, int64_t sample_id,
                           jls_rd_utc_cbk_fn cbk_fn, void * cbk_user_data) {
    return jls_core_utc(&self->core, signal_id, sample_id, cbk_fn, cbk_user_data);
}

static int32_t utc_load(struct jls_core_s * self, uint16_t signal_id) {
    ROE(jls_core_signal_validate_typed(self, signal_id, JLS_SIGNAL_TYPE_FSR));
    struct jls_core_signal_s * signal = &self->signal_info[signal_id];
    if ((NULL != signal->track_fsr) && (NULL != signal->track_fsr->tmap)) {
        return 0;
    }

    struct jls_signal_def_s * signal_def = &signal->signal_def;
    signal->track_fsr->tmap = jls_tmap_alloc(signal_def->sample_rate);
    if (NULL == signal->track_fsr->tmap) {
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    int64_t sample_rate = signal_def->sample_rate;
    int64_t sample_start = -3600 * sample_rate;  // within the last hour
    return jls_core_utc(self, signal_id, sample_start, jls_tmap_add_cbk, signal->track_fsr->tmap);
}

JLS_API int32_t jls_rd_sample_id_to_timestamp(struct jls_rd_s * self, uint16_t signal_id,
                                               int64_t sample_id, int64_t * timestamp) {
    ROE(utc_load(&self->core, signal_id));
    struct jls_tmap_s * fsr = self->core.signal_info[signal_id].track_fsr->tmap;
    return jls_tmap_sample_id_to_timestamp(fsr, sample_id, timestamp);
}

JLS_API int32_t jls_rd_timestamp_to_sample_id(struct jls_rd_s * self, uint16_t signal_id,
                                              int64_t timestamp, int64_t * sample_id) {
    ROE(utc_load(&self->core, signal_id));
    struct jls_tmap_s * fsr = self->core.signal_info[signal_id].track_fsr->tmap;
    return jls_tmap_timestamp_to_sample_id(fsr, timestamp, sample_id);
}
