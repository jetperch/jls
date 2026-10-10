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


/**
 * @file
 *
 * @brief JLS writer internal functions.
 */

#ifndef JLS_WRITER_PRIV_H__
#define JLS_WRITER_PRIV_H__

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
struct jls_twr_s;


int32_t jls_twr_run(struct jls_twr_s * self);

/**
 * @brief Validate an annotation type.
 *
 * @param annotation_type The jls_annotation_type_e.
 * @return 0 or JLS_ERROR_PARAMETER_INVALID.
 */
int32_t jls_wr_annotation_type_validate(uint32_t annotation_type);

/**
 * @brief Validate a user_data or annotation payload.
 *
 * @param storage_type The jls_storage_type_e, which must be a defined value.
 * @param data The payload, which STRING and JSON require.
 * @param data_size The payload size in bytes, which BINARY requires.
 * @return 0 or JLS_ERROR_PARAMETER_INVALID.
 */
int32_t jls_wr_payload_validate(uint32_t storage_type, const uint8_t * data, uint32_t data_size);

/**
 * @brief Validate jls_wr_user_data() arguments.
 *
 * @param chunk_meta The chunk metadata, which must have bits 15:12 clear.
 * @param storage_type The jls_storage_type_e.
 * @param data The payload.
 * @param data_size The payload size in bytes.
 * @return 0 or JLS_ERROR_PARAMETER_INVALID.
 */
int32_t jls_wr_user_data_validate(uint16_t chunk_meta, uint32_t storage_type,
                                  const uint8_t * data, uint32_t data_size);

/**
 * @brief Validate jls_wr_annotation() arguments.
 *
 * @param annotation_type The jls_annotation_type_e.
 * @param storage_type The jls_storage_type_e, which must not be INVALID.
 * @param data The payload.
 * @param data_size The payload size in bytes.
 * @return 0 or JLS_ERROR_PARAMETER_INVALID.
 */
int32_t jls_wr_annotation_validate(uint32_t annotation_type, uint32_t storage_type,
                                   const uint8_t * data, uint32_t data_size);

/**
 * @brief Get the latched write error.
 *
 * @param self The writer instance.
 * @return 0 or the first write error.
 */
int32_t jls_wr_error(struct jls_wr_s * self);

/** @} */

#ifdef __cplusplus
}
#endif

#endif  /* JLS_WRITER_PRIV_H__ */
