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
 * @brief JLS writer.
 */

#ifndef JLS_WRITER_H__
#define JLS_WRITER_H__

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
struct jls_wr_s;


int32_t jls_wr_open(struct jls_wr_s ** instance, const char * path);

int32_t jls_wr_source_def(struct jls_wr_s * self, struct jls_source_def_s const * source);
int32_t jls_wr_signal_def(struct jls_wr_s * self, const struct jls_signal_def_s * signal);
//int32_t jls_wr_utc_def(struct jls_wr_s * self, struct jls_utc_s * utc);
//int32_t jls_wr_ts_def(struct jls_wr_s * self, const struct jls_ts_def_s * ts);
int32_t jls_wr_user_data(struct jls_wr_s * self, uint16_t chuck_meta, uint32_t size, const uint8_t data[size]);

int32_t jls_wr_signal_f32(struct jls_wr_s * self, uint16_t signal_id, uint32_t size, const float data[size]);
int32_t jls_wr_signal_annotation_txt(struct jls_wr_s * self, uint16_t signal_id, uint64_t sample_id, const char * txt);
int32_t jls_wr_signal_annotation_marker(struct jls_wr_s * self, uint16_t signal_id, uint64_t sample_id, const char * marker_name);
//int32_t jls_wr_utc(struct jls_wr_s * self, uint16_t signal_id, uint8_t utc_id, int64_t utc, uint64_t sample_id);

int32_t jls_wr_ts_f32(struct jls_wr_s * self, uint16_t ts_id, int64_t timestamp, uint32_t size, const char txt[size]);
int32_t jls_wr_ts_annotation_txt(struct jls_wr_s * self, uint16_t signal_id, int64_t timestamp, const char * txt);



/** @} */

#ifdef __cplusplus
}
#endif

#endif  /* JLS_WRITER_H__ */
