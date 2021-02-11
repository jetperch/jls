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
int32_t jls_wr_close(struct jls_wr_s * self);

int32_t jls_wr_source_def(struct jls_wr_s * self, const struct jls_source_def_s * source);
int32_t jls_wr_signal_def(struct jls_wr_s * self, const struct jls_signal_def_s * signal);

/**
 * @brief Add arbitrary user data.
 *
 * @param self The writer instance.
 * @param chunk_meta The arbitrary data.  Bits 15:12 are reserved, but
 *      bits 11:0 may be assigned by the application.
 * @param storage_type The storage type for data.
 * @param data The user data to store.
 * @param data_size The size of data for JLS_STORAGE_TYPE_BINARY.  Ignored
 *      for all other storage types.
 * @return 0 or error code.
 */
int32_t jls_wr_user_data(struct jls_wr_s * self, uint16_t chunk_meta,
        enum jls_storage_type_e storage_type, const uint8_t * data, uint32_t data_size);

int32_t jls_wr_fsr_f32(struct jls_wr_s * self, uint16_t signal_id,
        uint64_t sample_id, const float * data, uint32_t data_length);

/**
 * @brief Add an annotation to a FSR signal.
 *
 * @param self The writer instance.
 * @param signal_id The signal id.
 * @param sample_id The timestamp in sample_id units.
 * @param annotation_type The annotation type.
 * @param storage_type The storage type.
 * @param data The data for the annotation.
 * @param data_size The length of data for JLS_STORAGE_TYPE_BINARY storage_type.
 *      Set to 0 for all other storage types.
 * @return 0 or error code.
 */
int32_t jls_wr_fsr_annotation(struct jls_wr_s * self, uint16_t signal_id, uint64_t sample_id,
                              enum jls_annotation_type_e annotation_type,
                              enum jls_storage_type_e storage_type, const uint8_t * data, uint32_t data_size);

// todo jls_wr_fsr_utc test
//int32_t jls_wr_fsr_utc(struct jls_wr_s * self, uint16_t signal_id, uint64_t sample_id, int64_t utc);

// todo jls_wr_vsr_f32
//int32_t jls_wr_vsr_f32(struct jls_wr_s * self, uint16_t ts_id, int64_t timestamp, uint32_t data, uint32_t size);

/**
 * Add an annotation to a VSR signal.
 *
 * @param self The writer instance.
 * @param signal_id The signal id.  Use 0 for a global annotation.
 * @param timestamp The timestamp in UTC.
 * @param annotation_type The annotation type.
 * @param storage_type The storage type.
 * @param data The data for the annotation.
 * @param data_size The length of data for JLS_STORAGE_TYPE_BINARY storage_type.
 *      Set to 0 for all other storage types.
 * @return 0 or error code.
 */
int32_t jls_wr_vsr_annotation(struct jls_wr_s * self, uint16_t signal_id, int64_t timestamp,
                              enum jls_annotation_type_e annotation_type,
                              enum jls_storage_type_e storage_type, const uint8_t * data, uint32_t data_size);


/** @} */

#ifdef __cplusplus
}
#endif

#endif  /* JLS_WRITER_H__ */
