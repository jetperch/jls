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

struct jls_wr_s; // opaque, see "jls/writer.h"
struct jls_wr_fsr_s;  // opaque, defined by wr_fsr.c

/**
 * @brief Validate and align the signal definition.
 *
 * @param def[inout] The signal definition.
 * @return 0 or error code.
 */
int32_t jls_wr_fsr_validate(struct jls_signal_def_s * def);

int32_t jls_wr_fsr_open(struct jls_wr_fsr_s ** instance, struct jls_wr_s * wr, const struct jls_signal_def_s * def);
int32_t jls_wr_fsr_close(struct jls_wr_fsr_s * self);

/**
 * @brief Write fixed sample rate data.
 *
 * @param self The instance.
 * @param sample_id The starting sample id for this data.
 * @param data The packed data appropriate for the format.
 * @param data_length The length of data in samples (NOT BYTES).
 * @return 0 or error code.
 */
int32_t jls_wr_fsr_data(struct jls_wr_fsr_s * self, int64_t sample_id, const void * data, uint32_t data_length);


/** @} */

#ifdef __cplusplus
}
#endif

#endif  /* JLS_WRITE_FSR_H__ */
