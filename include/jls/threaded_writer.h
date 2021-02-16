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
 * @brief JLS threaded writer.
 */

#ifndef JLS_THREADED_WRITER_H__
#define JLS_THREADED_WRITER_H__

#include <stdint.h>
#include "jls/format.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @ingroup jls
 * @defgroup jls_threaded_writer Threaded writer
 *
 * @brief JLS threaded writer.
 *
 * This module wraps writer. 
 * 
 * @{
 */


// opaque object
struct jls_twr_s;


int32_t jls_twr_open(struct jls_twr_s ** instance, const char * path);
int32_t jls_twr_close(struct jls_twr_s * self);
int32_t jls_twr_source_def(struct jls_twr_s * self, const struct jls_source_def_s * source);
int32_t jls_twr_signal_def(struct jls_twr_s * self, const struct jls_signal_def_s * signal);
int32_t jls_twr_user_data(struct jls_twr_s * self, uint16_t chunk_meta,
        enum jls_storage_type_e storage_type, const uint8_t * data, uint32_t data_size);
int32_t jls_twr_fsr_f32(struct jls_twr_s * self, uint16_t signal_id,
        int64_t sample_id, const float * data, uint32_t data_length);
int32_t jls_twr_annotation(struct jls_twr_s * self, uint16_t signal_id, int64_t timestamp,
                          enum jls_annotation_type_e annotation_type,
                          enum jls_storage_type_e storage_type, const uint8_t * data, uint32_t data_size);

// todo jls_twr_fsr_utc
//int32_t jls_twr_fsr_utc(struct jls_twr_s * self, uint16_t signal_id, int64_t sample_id, int64_t utc);

// todo jls_twr_vsr_f32
//int32_t jls_twr_vsr_f32(struct jls_twr_s * self, uint16_t ts_id, int64_t timestamp, uint32_t data, uint32_t size);


/** @} */

#ifdef __cplusplus
}
#endif

#endif  /* JLS_THREADED_WRITER_H__ */