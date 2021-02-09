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


/**
 * @file
 *
 * @brief JLS reader.
 */

#ifndef JLS_READER_H__
#define JLS_READER_H__

#include <stdint.h>
#include "jls/format.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @ingroup jls
 * @defgroup jls_writer Writer
 *
 * @brief JLS writer.
 *
 * @{
 */


// opaque object
struct jls_rd_s;


int32_t jls_rd_open(struct jls_rd_s ** instance, const char * path);
void jls_rd_close(struct jls_rd_s * self);

int32_t jls_rd_source_iter(struct jls_writer_s * self, struct jls_source_def_s const * source);

#if 0
int32_t jls_rd_signal_def_foreach(struct jls_writer_s * self, struct jls_signal_def_s const * signal);
int32_t jls_rd_utc_def_foreach(struct jls_writer_s * self, struct jls_utc_def_s * utc);
int32_t jls_rd_ts_def_foreach(struct jls_writer_s * self, struct jls_ts_def_s * ts);
int32_t jls_rd_user_data_foreach(struct jls_writer_s * self, struct jls_user_data_def_s * user_data);


int32_t jls_rd_source_def(struct jls_writer_s * self, uint8_t source_id, struct jls_source_def_s const ** source);
int32_t jls_rd_signal_def(struct jls_writer_s * self, uint16_t signal_id, uint8_t source_id, const struct jls_signal_def_s ** signal);
//int32_t jls_wr_utc_def(struct jls_writer_s * self, uint8_t utc_id, const char * name);
//int32_t jls_wr_ts_def(struct jls_writer_s * self, uint16_t ts_id, const struct jls_ts_def_s * ts);
int32_t jls_rd_user_data(struct jls_writer_s * self, uint16_t tag_id, uint32_t size, const uint8_t * data);

int32_t jls_rd_signal_f32(struct jls_writer_s * self, uint16_t signal_id, uint32_t size, const float * data);
int32_t jls_rd_signal_annotation_txt(struct jls_writer_s * self, uint16_t signal_id, uint64_t sample_id, const char * txt);
int32_t jls_rd_signal_annotation_marker(struct jls_writer_s * self, uint16_t signal_id, uint64_t sample_id, const char * marker_name);
//int32_t jls_wr_utc(struct jls_writer_s * self, uint16_t signal_id, uint8_t utc_id, int64_t utc, uint64_t sample_id);

int32_t jls_wr_ts_f32(struct jls_writer_s * self, uint16_t ts_id, int64_t timestamp, uint32_t size, const char * txt);
int32_t jls_wr_ts_annotation_txt(struct jls_writer_s * self, uint16_t signal_id, int64_t timestamp, const char * txt);
#endif


/** @} */

#ifdef __cplusplus
}
#endif

#endif  /* JLS_READER_H__ */
