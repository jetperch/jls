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

struct jls_rd_annotation_s {
    int64_t timestamp;
    uint8_t annotation_type;
    uint8_t storage_type;
    uint32_t data_size;
    const uint8_t * data;
};


int32_t jls_rd_open(struct jls_rd_s ** instance, const char * path);
void jls_rd_close(struct jls_rd_s * self);

/**
 * @brief Get the array of sources in the file.
 *
 * @param self The reader instance.
 * @param sources[out] The array of sources.
 * @param count[out] The number of items in sources.
 * @return 0 or error code.
 */
int32_t jls_rd_sources(struct jls_rd_s * self, struct jls_source_def_s ** sources, uint16_t * count);

/**
 * @brief Get the array of signals in the file.
 *
 * @param self The reader instance.
 * @param signals[out] The array of signals.
 * @param count[out] The number of items in signals.
 * @return 0 or error code.
 */
int32_t jls_rd_signals(struct jls_rd_s * self, struct jls_signal_def_s ** signals, uint16_t * count);

int32_t jls_rd_fsr_length(struct jls_rd_s * self, uint16_t signal_id, int64_t * samples);
int32_t jls_rd_fsr_f32(struct jls_rd_s * self, uint16_t signal_id, int64_t start_sample_id,
                       float * data, size_t data_length);


int32_t jls_rd_annotations(struct jls_rd_s * self, uint16_t signal_id,
                          struct jls_rd_annotation_s ** annotations, uint32_t * count);
void jls_rd_annotations_free(struct jls_rd_s * self, struct jls_rd_annotation_s * annotations);

struct jls_rd_user_data_s {
    uint16_t chunk_meta;
    enum jls_storage_type_e storage_type;
    uint8_t * data;
    uint32_t data_size;
};

int32_t jls_rd_user_data_reset(struct jls_rd_s * self);
int32_t jls_rd_user_data_next(struct jls_rd_s * self, struct jls_rd_user_data_s * user_data);
int32_t jls_rd_user_data_prev(struct jls_rd_s * self, struct jls_rd_user_data_s * user_data);


/** @} */

#ifdef __cplusplus
}
#endif

#endif  /* JLS_READER_H__ */
