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
 * @brief JLS reader.
 */

#ifndef JLS_READER_H__
#define JLS_READER_H__

#include <stdint.h>
#include "jls/cmacro.h"
#include "jls/format.h"

/**
 * @ingroup jls
 * @defgroup jls_reader Reader
 *
 * @brief JLS reader.
 *
 * @{
 */

JLS_CPP_GUARD_START

/// The opaque JLS reader object.
struct jls_rd_s;

/**
 * @brief Open a JLS file to read contents.
 *
 * @param[out] instance The new JLS read instance.
 * @param path The JLS file path.
 * @return 0 or error code.
 *
 * Call jls_rd_close() when done.
 */
JLS_API int32_t jls_rd_open(struct jls_rd_s ** instance, const char * path);

/**
 * @brief Close a JLS file opened with jls_rd_open().
 * @param self The JLS read instance.
 */
JLS_API void jls_rd_close(struct jls_rd_s * self);

/**
 * @brief Get the array of sources in the file.
 *
 * @param self The reader instance.
 * @param[out] sources The array of sources.
 * @param[out] count The number of items in sources.
 * @return 0 or error code.
 */
JLS_API int32_t jls_rd_sources(struct jls_rd_s * self, struct jls_source_def_s ** sources, uint16_t * count);

/**
 * @brief Get the array of signals in the file.
 *
 * @param self The reader instance.
 * @param[out] signals The array of signals.
 * @param[out] count The number of items in signals.
 * @return 0 or error code.
 */
JLS_API int32_t jls_rd_signals(struct jls_rd_s * self, struct jls_signal_def_s ** signals, uint16_t * count);

/**
 * @brief Get the signal by signal_id.
 *
 * @param self The reader instance.
 * @param signal_id The signal id to get.
 * @param[out] signal The signal definition.
 * @return 0 or error code.
 */
JLS_API int32_t jls_rd_signal(struct jls_rd_s * self, uint16_t signal_id, struct jls_signal_def_s * signal);

/**
 * @brief Get the number of samples in an FSR signal.
 *
 * @param self The reader instance.
 * @param signal_id The signal id.
 * @param[out] samples The number of samples in the signal.
 * @return 0 or error code.
 */
JLS_API int32_t jls_rd_fsr_length(struct jls_rd_s * self, uint16_t signal_id, int64_t * samples);

/**
 * @brief Read fixed sample rate (FSR) data.
 *
 * @param self The reader instance.
 * @param signal_id The signal id.  The first
 *      recorded sample is always 0.  To get the first recorded sample_id,
 *      see jls_signal_def_s.sample_id_offset.
 * @param start_sample_id The starting sample id to read.
 * @param[out] data The samples read.
 * @param data_length The number of samples to read.
 *      data is at least this many samples.
 *      For data types less than 8 bits long, you need to provide an
 *      extra byte to allow for data shifting.  Therefore
 *      data is at least 1 + (data_length * entry_size_bits) / 8 bytes.
 * @return 0 or error code
 */
JLS_API int32_t jls_rd_fsr(struct jls_rd_s * self, uint16_t signal_id, int64_t start_sample_id,
                           void * data, int64_t data_length);

/**
 * @brief Read fixed sample rate (FSR) float32 data.
 *
 * @param self The reader instance.
 * @param signal_id The signal id.  The first
 *      recorded sample is always 0.  To get the first recorded sample_id,
 *      see jls_signal_def_s.sample_id_offset.
 * @param start_sample_id The starting sample id to read.
 * @param[out] data The samples read.
 * @param data_length The number of samples to read.  data is
 *      also at least this many entries (4 * data_length bytes).
 * @return 0 or error code
 */
JLS_API int32_t jls_rd_fsr_f32(struct jls_rd_s * self, uint16_t signal_id, int64_t start_sample_id,
                               float * data, int64_t data_length);

/**
 * @brief Read the statistics data for a fixed sampling rate signal.
 *
 * @param self The reader instance.
 * @param signal_id The FSR signal.
 * @param start_sample_id The starting sample id to read.  The first
 *      recorded sample is always 0.  To get the first recorded sample_id,
 *      see jls_signal_def_s.sample_id_offset.
 * @param increment The number of samples that form a single output summary.
 * @param[out] data The statistics information, in the shape of
 *      data[data_length][JLS_SUMMARY_FSR_COUNT].  The elements are
 *      mean, standard_deviation, min, max.
 *      Use JLS_SUMMARY_FSR_MEAN, JLS_SUMMARY_FSR_STD,
 *      JLS_SUMMARY_FSR_MIN, and JLS_SUMMARY_FSR_MAX to index the values.
 * @param data_length The number of statistics points to populate.  data
 *      is at least JLS_SUMMARY_FSR_COUNT * data_length elements, each
 *      of float64 type (8 bytes).
 *      This argument allows efficient computation over many consecutive
 *      windows, as is common for displaying waveforms.
 * @return 0 or error code.
 *
 * For data_length 1, the statistics are sample-accurate.  For
 * larger data_lengths, the external boundaries for start and end
 * are computed exactly.  The internal boundaries are approximated,
 * perfect for waveform display, but perhaps not suitable for other use
 * cases.  If you need sample accurate statistics over multiple
 * increments, call this function repeatedly with data_length 1.
 */
JLS_API int32_t jls_rd_fsr_statistics(struct jls_rd_s * self, uint16_t signal_id,
                                      int64_t start_sample_id, int64_t increment,
                                      double * data, int64_t data_length);

/**
 * @brief The function called for each annotation.
 *
 * @param user_data The arbitrary user data.
 * @param annotation The annotation which only remains valid for the duration
 *      of the call.
 * @return 0 to continue iteration or any other value to stop.
 * @see jls_rd_annotations
 */
typedef int32_t (*jls_rd_annotation_cbk_fn)(void * user_data, const struct jls_annotation_s * annotation);

/**
 * @brief Iterate over the annotations for a signal.
 *
 * @param self The reader instance.
 * @param signal_id The signal id.
 * @param timestamp The starting timestamp.  Skip all prior annotations.
 * @param cbk_fn The callback function that jls_rd_annotations() will
 *      call once for each matching annotation.  Return 0 to continue
 *      to the next annotation or a non-zero value to stop iteration.
 * @param cbk_user_data The arbitrary data provided to cbk_fn.
 * @return 0 or error code.
 */
JLS_API int32_t jls_rd_annotations(struct jls_rd_s * self, uint16_t signal_id, int64_t timestamp,
                                   jls_rd_annotation_cbk_fn cbk_fn, void * cbk_user_data);

/**
 * @brief The function called for each user data entry.
 *
 * @param user_data The arbitrary user data.
 * @param chunk_meta The chunk meta value.
 * @param storage_type The data storage type.
 * @param data The data.
 * @param data_size The size of data in bytes.
 * @return 0 to continue iteration or any other value to stop.
 * @see jls_rd_user_data
 */
typedef int32_t (*jls_rd_user_data_cbk_fn)(void * user_data,
        uint16_t chunk_meta, enum jls_storage_type_e storage_type,
        uint8_t * data, uint32_t data_size);

/**
 * @brief Iterate over user data entries.
 *
 * @param self The reader instance.
 * @param cbk_fn The callback function that jls_rd_user_data() will
 *      call once for each user data entry.  Return 0 to continue
 *      to the next user data entry or a non-zero value to stop iteration.
 * @param cbk_user_data The arbitrary data provided to cbk_fn.
 * @return 0 or error code.
 */
JLS_API int32_t jls_rd_user_data(struct jls_rd_s * self, jls_rd_user_data_cbk_fn cbk_fn, void * cbk_user_data);

/**
 * @brief The function called for each UTC entry.
 *
 * @param user_data The arbitrary user data.
 * @param utc The array of utc entries, which are sample_id / timestamp pairs.
 * @param size The number of utc entries for this callback.
 * @return 0 to continue iteration or any other value to stop.
 * @see jls_rd_annotations
 */
typedef int32_t (*jls_rd_utc_cbk_fn)(void * user_data,
        const struct jls_utc_summary_entry_s * utc, uint32_t size);

/**
 * @brief Iterate over the UTC timestamps for a FSR signal.
 *
 * @param self The reader instance.
 * @param signal_id The signal id.
 * @param sample_id The starting sample_id.  Skip all prior sample ids.
 * @param cbk_fn The callback function that jls_rd_utc() will
 *      call once for UTC entries.  Return 0 to continue
 *      to the next UTC entries or a non-zero value to stop iteration.
 * @param cbk_user_data The arbitrary data provided to cbk_fn.
 * @return 0 or error code.
 */
JLS_API int32_t jls_rd_utc(struct jls_rd_s * self, uint16_t signal_id, int64_t sample_id,
                           jls_rd_utc_cbk_fn cbk_fn, void * cbk_user_data);

/**
 * @brief Convert from sample_id to timestamp for FSR signals.
 *
 * @param self The reader instance.
 * @param signal_id The signal id.
 * @param sample_id The sample id to convert.
 * @param[out] timestamp The JLS timestamp for the sample_id.
 * @return 0 or error code.
 */
JLS_API int32_t jls_rd_sample_id_to_timestamp(struct jls_rd_s * self, uint16_t signal_id,
        int64_t sample_id, int64_t * timestamp);

/**
 * @brief Convert from timestamp to sample_id for FSR signals.
 *
 * @param self The reader instance.
 * @param signal_id The signal id.
 * @param timestamp The JLS timestamp to convert.
 * @param[out] sample_id The sample_id for timestamp.
 * @return 0 or error code.
 */
JLS_API int32_t jls_rd_timestamp_to_sample_id(struct jls_rd_s * self, uint16_t signal_id,
        int64_t timestamp, int64_t * sample_id);

JLS_CPP_GUARD_END

/** @} */

#endif  /* JLS_READER_H__ */
