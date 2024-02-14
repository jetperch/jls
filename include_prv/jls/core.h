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

/**
 * @file
 *
 * @brief JLS core implementation shared by read, write, and repair.
 */

#ifndef JLS_CORE_H__
#define JLS_CORE_H__

#include "jls/cmacro.h"
#include "jls/format.h"
#include "jls/raw.h"
#include "jls/buffer.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/**
 * @ingroup jls
 * @defgroup jls_core JLS core.
 *
 * @brief JLS core implementation shared by read, write, and repair.
 *
 * @{
 */

JLS_CPP_GUARD_START

struct jls_core_signal_s;
struct jls_core_s;


struct jls_core_chunk_s {
    struct jls_chunk_header_s hdr;
    int64_t offset;
};

struct jls_core_f64_buf_s {
    double * start;
    double * end;
    size_t alloc_length;  // in double
    double buffer[];
};

struct jls_core_fsr_level_s {
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

struct jls_core_fsr_s {
    struct jls_core_signal_s * parent;
    int64_t signal_length;  // total, including skipped samples
    uint32_t data_length;  // for data, in samples
    struct jls_fsr_data_s * data;  // for level 0 sample data
    double * data_f64;             // for level 0 sample data summarization statistics computation
    int64_t sample_id_offset;
    uint8_t write_omit_data;      // omit level 0 sample data. >1=enabled, else disabled
    uint8_t shift_amount;
    uint8_t shift_buffer;
    uint64_t buffer_u64[4096];     // for shifting incoming sample data on skips & duplicates
    struct jls_core_fsr_level_s * level[JLS_SUMMARY_LEVEL_COUNT];  // level 0 unused

    struct jls_tmap_s * tmap;     // on read, map UTC to sample_id
};

struct jls_core_ts_s {
    struct jls_core_signal_s * parent;
    enum jls_track_type_e track_type;
    uint32_t decimate_factor;
    struct jls_index_s * index[JLS_SUMMARY_LEVEL_COUNT];                // level 0 not used
    struct jls_payload_header_s * summary[JLS_SUMMARY_LEVEL_COUNT];     // level 0 not used
};

struct jls_core_track_s {
    struct jls_core_signal_s * parent;
    uint8_t track_type;  // enum jls_track_type_e

    struct jls_core_chunk_s head;

    /**
     * @brief the offset for the first chunk at each summary level.
     *
     * For level 0, the offset for the first data chunk.
     * For all other levels, the offset of the first index chunk.
     *    The summary chunk must immediately follow each index chunk.
     */
    int64_t head_offsets[JLS_SUMMARY_LEVEL_COUNT];

    // holder for most recent to construct doubly linked list on write
    struct jls_core_chunk_s index_head[JLS_SUMMARY_LEVEL_COUNT];
    struct jls_core_chunk_s data_head;
    struct jls_core_chunk_s summary_head[JLS_SUMMARY_LEVEL_COUNT];
};

struct jls_core_signal_s {
    struct jls_core_s * parent;
    struct jls_core_chunk_s chunk_def;
    struct jls_signal_def_s signal_def;
    struct jls_core_track_s tracks[JLS_TRACK_TYPE_COUNT];   // array index is jls_track_type_e
    struct jls_core_fsr_s * track_fsr;
    struct jls_core_ts_s * track_anno;
    struct jls_core_ts_s * track_utc;  // for fsr only
};

struct jls_core_source_s {
    struct jls_core_chunk_s chunk_def;
    struct jls_source_def_s source_def;
};

struct jls_core_s {
    struct jls_raw_s * raw;
    struct jls_buf_s * buf;  // automatic target for chunk read

    struct jls_buf_s * rd_index;    // the index for the most recent FSR read operation
    struct jls_core_chunk_s rd_index_chunk;
    struct jls_buf_s * rd_summary;  // the summary for the most recent FSR read operation
    struct jls_core_chunk_s rd_summary_chunk;

    struct jls_core_source_s source_info[JLS_SOURCE_COUNT];
    struct jls_source_def_s source_def_api[JLS_SOURCE_COUNT];
    struct jls_core_chunk_s source_head;  // for most recently added source_def

    struct jls_core_signal_s signal_info[JLS_SIGNAL_COUNT];
    struct jls_signal_def_s signal_def_api[JLS_SIGNAL_COUNT];
    struct jls_core_chunk_s signal_head;  // for most recently added signal_def, track_def, track_head

    struct jls_core_chunk_s user_data_head;  // for most recently added user_data

    struct jls_core_chunk_s chunk_cur;           // most recent read chunk header, payload in buf
    struct jls_core_f64_buf_s * f64_sample_buf;  // for reading samples
    struct jls_core_f64_buf_s * f64_stats_buf;   // for reading statistics
};

int32_t jls_core_f64_buf_alloc(size_t length, struct jls_core_f64_buf_s ** buf);
void jls_core_f64_buf_free(struct jls_core_f64_buf_s * buf);

/**
 * @brief Validate the signal definition.
 *
 * @param def[in] The signal definition.
 * @return 0 or error code.
 */
int32_t jls_core_signal_def_validate(struct jls_signal_def_s const * def);

/**
 * @brief Align the signal definition parameters.
 *
 * @param def[inout] The signal definition.
 * @return 0 or error code.
 */
int32_t jls_core_signal_def_align(struct jls_signal_def_s * def);

/**
 * @brief Update the chunk header
 *
 * @param self The core instance.
 * @param chunk The chunk header.  The offset field must be valid.
 *      The header will be updated with the actual CRC32.
 * @return 0 or error code.
 */
int32_t jls_core_update_chunk_header(struct jls_core_s * self, struct jls_core_chunk_s * chunk);

/**
 * @brief Advance the item head for the doubly-linked list.
 *
 * @param self The core instance.
 * @param head The existing item head chunk.
 * @param next The next item head chunk.
 * @return 0 or error code.
 *
 * This function causes a file seek back to the head chunk in order to
 * perform the write.  However, it uses the provided head data to
 * skip performing a read.
 */
int32_t jls_core_update_item_head(struct jls_core_s * self,
        struct jls_core_chunk_s * head, struct jls_core_chunk_s * next);

int32_t jls_core_signal_validate(struct jls_core_s * self, uint16_t signal_id);
int32_t jls_core_signal_validate_typed(struct jls_core_s * self, uint16_t signal_id, enum jls_signal_type_e signal_type);
int32_t jls_core_validate_track_tag(struct jls_core_s * self, uint16_t signal_id, uint8_t tag);

static inline enum jls_track_chunk_e jls_core_tag_parse_track_chunk(uint8_t tag) {
    return (tag & 0x07);
}

static inline uint8_t jls_core_tag_parse_track_type(uint8_t tag) {
    return (tag >> 3) & 3;
}

int32_t jls_core_wr_data(struct jls_core_s * self, uint16_t signal_id, enum jls_track_type_e track_type,
                         const uint8_t * payload, uint32_t payload_length);

int32_t jls_core_wr_summary(struct jls_core_s * self, uint16_t signal_id, enum jls_track_type_e track_type, uint8_t level,
                            const uint8_t * payload, uint32_t payload_length);

int32_t jls_core_wr_index(struct jls_core_s * self, uint16_t signal_id, enum jls_track_type_e track_type, uint8_t level,
                          const uint8_t * payload, uint32_t payload_length);

int32_t jls_core_wr_end(struct jls_core_s * self);

int32_t jls_core_fsr_summary_level_alloc(struct jls_core_fsr_s * self, uint8_t level);
int32_t jls_core_fsr_summary1(struct jls_core_fsr_s * self, int64_t pos);
int32_t jls_core_fsr_summaryN(struct jls_core_fsr_s * self, uint8_t level, int64_t pos);

int32_t jls_fsr_open(struct jls_core_fsr_s ** instance, struct jls_core_signal_s * parent);
int32_t jls_fsr_close(struct jls_core_fsr_s * self);

int32_t jls_core_rd_chunk(struct jls_core_s * self);
int32_t jls_core_rd_chunk_end(struct jls_core_s * self);
int32_t jls_core_scan_sources(struct jls_core_s * self);
int32_t jls_core_scan_signals(struct jls_core_s * self);
int32_t jls_core_scan_fsr_sample_id(struct jls_core_s * self);
int32_t jls_core_scan_initial(struct jls_core_s * self);
int32_t jls_core_sources(struct jls_core_s * self, struct jls_source_def_s ** sources, uint16_t * count);
int32_t jls_core_signals(struct jls_core_s * self, struct jls_signal_def_s ** signals, uint16_t * count);
int32_t jls_core_signal(struct jls_core_s * self, uint16_t signal_id, struct jls_signal_def_s * signal);
int32_t jls_core_fsr_sample_buffer_alloc(struct jls_core_fsr_s * self);
void jls_core_fsr_sample_buffer_free(struct jls_core_fsr_s * self);
int32_t jls_core_fsr_seek(struct jls_core_s * self, uint16_t signal_id, uint8_t level, int64_t sample_id);
int32_t jls_core_fsr_length(struct jls_core_s * self, uint16_t signal_id, int64_t * samples);
int32_t jls_core_fsr(struct jls_core_s * self, uint16_t signal_id, int64_t start_sample_id,
                     void * data, int64_t data_length);
int32_t jls_core_fsr_f32(struct jls_core_s * self, uint16_t signal_id, int64_t start_sample_id,
                         float * data, int64_t data_length);
int32_t jls_core_fsr_statistics(struct jls_core_s * self, uint16_t signal_id,
                                int64_t start_sample_id, int64_t increment,
                                double * data, int64_t data_length);
int32_t jls_core_ts_seek(struct jls_core_s * self, uint16_t signal_id, uint8_t level,
                         enum jls_track_type_e track_type, int64_t timestamp);

int32_t jls_core_rd_fsr_level1(struct jls_core_s * self, uint16_t signal_id, int64_t start_sample_id);
int32_t jls_core_rd_fsr_data0(struct jls_core_s * self, uint16_t signal_id, int64_t start_sample_id);

int32_t jls_core_repair_fsr(struct jls_core_s * self, uint16_t signal_id);


JLS_CPP_GUARD_END

/** @} */

#endif  /* JLS_CORE_H__ */
