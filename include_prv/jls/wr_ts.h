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
 * @brief JLS time series writer.
 */

#ifndef JLS_WRITE_TS_H__
#define JLS_WRITE_TS_H__

#include <stdint.h>
#include "jls/format.h"
#include "jls/core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @ingroup jls
 * @defgroup jls_wf_ts Timeseries writer.
 *
 * @brief JLS timeseries writer.
 *
 * @{
 */

/**
 * @brief Open a new timeseries for index writing.
 *
 * @param instance[out] The new instance
 * @param parent The parent instance.
 * @param track_type The track type (ANNOTATION, UTC) for this timeseries.
 * @param decimate_factor The decimation factor for each index level.
 * @return 0 or error code.
 */
int32_t jls_wr_ts_open(
        struct jls_core_ts_s ** instance,
        struct jls_core_signal_s * parent,
        enum jls_track_type_e track_type,
        uint32_t decimate_factor);

/**
 * @brief Close the timeseries and write summary information to disk.
 *
 * @param self The timeseries instance.
 * @return 0 or error code.
 */
int32_t jls_wr_ts_close(struct jls_core_ts_s * self);

/**
 * @brief Add a timeseries annotation entry.
 *
 * @param self The timeseries instance.
 * @param timestamp The timestamp (sample_id for FSR or utc for VSR).
 * @param offset The file offset for the referenced chunk.
 * @param annotation_type The annotation type.
 * @param group_id The group_id.
 * @param y The y-axis value.
 * @return 0 or error code.
 * @note The annotation data chunk must be written by the caller.
 */
int32_t jls_wr_ts_anno(struct jls_core_ts_s * self, int64_t timestamp, int64_t offset,
                       enum jls_annotation_type_e annotation_type, uint8_t group_id,
                       float y);

/**
 * @brief Add a UTC entry.
 *
 * @param self The timeseries instance.
 * @param sample_id The sample id.
 * @param offset The file offset for the referenced chunk.
 * @param utc The utc timestamp.
 * @return 0 or error code.
 * @note The utc data chunk must be written by the caller, if desired.
 *     Alternatively, provide offset=0 to omit level 0 data chunks.
 */
int32_t jls_wr_ts_utc(struct jls_core_ts_s * self, int64_t sample_id, int64_t offset, int64_t utc);


/** @} */

#ifdef __cplusplus
}
#endif

#endif  /* JLS_WRITE_TS_H__ */
