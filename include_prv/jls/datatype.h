/*
 * Copyright 2022 Jetperch LLC
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
 * @brief JLS data type conversions.
 */

#ifndef JLS_PRIV_DATATYPE_H__
#define JLS_PRIV_DATATYPE_H__

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @ingroup jls
 * @defgroup jls_dt Data type conversions
 *
 * @brief JLS data type handling and conversions.
 *
 * @{
 */

/**
 * @brief Convert a buffer into doubles.
 * @param src The source buffer pointer
 * @param src_datatype The source buffer datatype, see JLS_DATATYPE_*.
 * @param[out] dst The output f64 buffer.
 * @param samples The number of samples to convert.  Both src and dst must
 *      be able to hold at least this many samples.  Packed types (u1, u4,
 *      i4) read ceil(samples * bits / 8) source bytes.
 * @return 0 or error code.  JLS_ERROR_PARAMETER_INVALID for an
 *      unsupported datatype, leaving dst unmodified.
 *
 * Fixed-point integer types (q != 0) scale each value by 2^-q.
 */
int32_t jls_dt_buffer_to_f64(const void * src, uint32_t src_datatype, double * dst, size_t samples);

/**
 * @brief The scale from raw values to values.
 * @param datatype The datatype, see JLS_DATATYPE_*.
 * @return 2^-q for fixed-point integer types (q != 0), otherwise 1.0.
 *
 * FSR summaries store raw values in every format version, so readers
 * apply this scale to summary entries.
 */
double jls_dt_scale(uint32_t datatype);


/** @} */

#ifdef __cplusplus
}
#endif

#endif  /* JLS_PRIV_DATATYPE_H__ */
