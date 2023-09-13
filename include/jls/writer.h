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
 * @brief JLS writer.
 */

#ifndef JLS_WRITER_H__
#define JLS_WRITER_H__

#include <stdint.h>
#include "jls/statistics.h"
#include "jls/format.h"

/**
 * @ingroup jls
 * @defgroup jls_writer Writer
 *
 * @brief JLS writer.
 *
 * @{
 */

JLS_CPP_GUARD_START

/// Opaque JLS writer object.
struct jls_wr_s;

/**
 * @brief Open a JLS file for writing.
 *
 * @param[out] instance The JLS writer instance.
 * @param path The JLS file path.
 * @return 0 or error code.
 *
 * Call jls_wr_close() when done.
 */
JLS_API int32_t jls_wr_open(struct jls_wr_s ** instance, const char * path);

/**
 * @brief Close a JLS file.
 *
 * @param self The JLS writer instance from jls_wr_open().
 * @return 0 or error code.
 */
JLS_API int32_t jls_wr_close(struct jls_wr_s * self);

/**
 * @brief Flush a JLS file to disk.
 *
 * @param self The JLS writer instance from jls_wr_open().
 * @return 0 or error code.
 */
JLS_API int32_t jls_wr_flush(struct jls_wr_s * self);

/**
 * @brief Define a new source.
 *
 * @param self The JLS writer instance.
 * @param source The source definition.
 * @return 0 or error code.
 *
 * This JLS file format supports multiple sources, which are usually different
 * instruments.  Each source can provide multiple signals.
 */
JLS_API int32_t jls_wr_source_def(struct jls_wr_s * self, const struct jls_source_def_s * source);

/**
 * @brief Define a new signal.
 *
 * @param self The JLS writer instance.
 * @param signal The signal definition.
 * @return 0 or error code.
 */
JLS_API int32_t jls_wr_signal_def(struct jls_wr_s * self, const struct jls_signal_def_s * signal);

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
JLS_API int32_t jls_wr_user_data(struct jls_wr_s * self, uint16_t chunk_meta,
        enum jls_storage_type_e storage_type, const uint8_t * data, uint32_t data_size);

/**
 * @brief Write fixed-rate sample data to a signal.
 *
 * @param self The JLS writer instance.
 * @param signal_id The signal id.
 * @param sample_id The sample id for data[0].
 * @param data The sample data array.  Data must be packed with no spacing
 *      between samples.  u1 stores 8 samples per byte, and u4 stores 2 samples
 *      per byte.
 * @param data_length The length of data in samples.
 * @return 0 or error code
 */
JLS_API int32_t jls_wr_fsr(struct jls_wr_s * self, uint16_t signal_id,
                           int64_t sample_id, const void * data, uint32_t data_length);

/**
 * @brief Write sample data to a float32 FSR signal.
 *
 * @param self The JLS writer instance.
 * @param signal_id The signal id.
 * @param sample_id The sample id for data[0].
 * @param data The sample data array.
 * @param data_length The length of data in floats (bytes / 4).
 * @return 0 or error code
 */
JLS_API int32_t jls_wr_fsr_f32(struct jls_wr_s * self, uint16_t signal_id,
        int64_t sample_id, const float * data, uint32_t data_length);

/**
 * @brief Omit level 0 data chunks from the signal's stream.
 *
 * @param self The JLS writer instance
 * @param signal_id The signal id.
 * @param enable 0 to disable (default), 1 to enable.
 * @return 0 or error code.
 *
 * Enable is delayed by one sample block to ensure any pending
 * data is writen.  Disable takes effect immediately.
 *
 * On read, the level 0 data is reconstructed using the summaries.
 */
JLS_API int32_t jls_wr_fsr_omit_data(struct jls_wr_s * self, uint16_t signal_id, uint32_t enable);

/**
 * @brief Add an annotation to a signal.
 *
 * @param self The writer instance.
 * @param signal_id The signal id.
 * @param timestamp The x-axis timestamp in sample_id for FSR and UTC for VSR.
 * @param y The y-axis value or NAN to automatically position.
 * @param annotation_type The annotation type.
 * @param group_id The optional group identifier.  If unused, set to 0.
 * @param storage_type The storage type.
 * @param data The data for the annotation.
 * @param data_size The length of data for JLS_STORAGE_TYPE_BINARY storage_type.
 *      Set to 0 for all other storage types.
 * @return 0 or error code.
 */
JLS_API int32_t jls_wr_annotation(struct jls_wr_s * self,
                                  uint16_t signal_id,
                                  int64_t timestamp,
                                  float y,
                                  enum jls_annotation_type_e annotation_type,
                                  uint8_t group_id,
                                  enum jls_storage_type_e storage_type,
                                  const uint8_t * data,
                                  uint32_t data_size);

/**
 * @brief Add a mapping from sample_id to UTC timestamp for an FSR signal.
 *
 * @param self The writer instance.
 * @param signal_id The signal id.
 * @param sample_id The sample_id for FSR.
 * @param utc The UTC timestamp.
 * @return 0 or error code.
 */
JLS_API int32_t jls_wr_utc(struct jls_wr_s * self, uint16_t signal_id, int64_t sample_id, int64_t utc);

// todo jls_wr_vsr_f32
// JLS_API int32_t jls_wr_vsr_f32(struct jls_wr_s * self, uint16_t ts_id, int64_t timestamp, uint32_t data, uint32_t size);

JLS_CPP_GUARD_END

/** @} */

#endif  /* JLS_WRITER_H__ */
