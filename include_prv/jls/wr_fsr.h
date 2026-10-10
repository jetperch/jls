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
 * @brief JLS fixed-sampling rate signal writer.
 */

#ifndef JLS_WRITE_FSR_H__
#define JLS_WRITE_FSR_H__

#include <stdint.h>
#include "jls/format.h"
#include "jls/core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @ingroup jls
 * @defgroup jls_wr_fsr FSR signal writer.
 *
 * @brief JLS FSR signal writer.
 *
 * @{
 */


/**
 * @brief The maximum forward sample_id skip, in samples.
 *
 * The writer fills skipped samples synchronously (NaN for float types,
 * 0 otherwise).  Larger skips, such as from a corrupt sample_id, return
 * JLS_ERROR_TOO_BIG rather than fill the disk.
 */
#define JLS_WR_FSR_SKIP_MAX (1ULL << 32)

/**
 * @brief Validate a write without modifying any state.
 *
 * @param self The instance, which may be NULL.
 * @param sample_id The starting sample id for this data.
 * @param data_length The length of data in samples.
 * @return 0, JLS_ERROR_NOT_FOUND if self is NULL, or JLS_ERROR_TOO_BIG
 *      if the skip exceeds JLS_WR_FSR_SKIP_MAX.
 */
int32_t jls_wr_fsr_validate(struct jls_core_fsr_s * self, int64_t sample_id, uint32_t data_length);

/**
 * @brief Write the pending data and summaries, then free the instance.
 *
 * @param self The instance, which is always freed.
 * @return 0 or the first error.  Writing stops at the first error.
 */
int32_t jls_wr_fsr_close(struct jls_core_fsr_s * self);

/**
 * @brief Free the instance without writing pending data or summaries.
 *
 * @param self The instance.
 */
void jls_wr_fsr_free(struct jls_core_fsr_s * self);

/**
 * @brief Write fixed sample rate data.
 *
 * @param self The instance.
 * @param sample_id The starting sample id for this data.
 * @param data The packed data appropriate for the format.
 * @param data_length The length of data in samples (NOT BYTES).
 * @return 0 or error code.
 */
int32_t jls_wr_fsr_data(struct jls_core_fsr_s * self, int64_t sample_id, const void * data, uint32_t data_length);


/** @} */

#ifdef __cplusplus
}
#endif

#endif  /* JLS_WRITE_FSR_H__ */
