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
int32_t jls_wr_utc_def(struct jls_wr_s * self, uint8_t utc_id, const char * name);
int32_t jls_wr_signal_def(struct jls_wr_s * self, const struct jls_signal_def_s * signal);
int32_t jls_wr_user_data(struct jls_wr_s * self, uint16_t chunk_meta, const uint8_t * data, uint32_t data_size);

int32_t jls_wr_fsr_f32(struct jls_wr_s * self, uint16_t signal_id, const float * data, uint32_t data_size);
int32_t jls_wr_fsr_annotation_txt(struct jls_wr_s * self, uint16_t signal_id, uint64_t sample_id, const char * txt);
int32_t jls_wr_fsr_annotation_marker(struct jls_wr_s * self, uint16_t signal_id, uint64_t sample_id, const char * marker_name);
int32_t jls_wr_fsr_utc(struct jls_wr_s * self, uint16_t signal_id, uint8_t utc_id, uint64_t sample_id, int64_t utc);

int32_t jls_wr_vsr_f32(struct jls_wr_s * self, uint16_t ts_id, int64_t timestamp, uint32_t data, uint32_t size);
int32_t jls_wr_vsr_annotation_txt(struct jls_wr_s * self, uint16_t signal_id, int64_t timestamp, const char * txt);
int32_t jls_wr_vsr_annotation_marker(struct jls_wr_s * self, uint16_t signal_id, int64_t timestamp, const char * marker_name);


/**
 * @brief Write a global text annotation.
 *
 * @param self The writer instance.
 * @param timestamp The UTC timestamp in jls/time.h format.
 * @param txt The text annotation.
 * @return 0 or error code.
 */
static inline int32_t jls_wr_annotation_txt(struct jls_wr_s * self, int64_t timestamp, const char * txt) {
    return jls_wr_vsr_annotation_txt(self, 0, timestamp, txt);
}

/**
 * @brief Write a global marker annotation.
 *
 * @param self The writer instance.
 * @param timestamp The UTC timestamp in jls/time.h format.
 * @param marker_name The marker name.
 * @return 0 or error code.
 */
static inline int32_t jls_wr_annotation_marker(struct jls_wr_s * self, int64_t timestamp, const char * marker_name) {
    return jls_wr_vsr_annotation_marker(self, 0, timestamp, marker_name);
}


/** @} */

#ifdef __cplusplus
}
#endif

#endif  /* JLS_WRITER_H__ */
