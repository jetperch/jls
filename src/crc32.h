/*
 * Copyright 2014-2021 Jetperch LLC
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
 * @brief Cyclic Redundancy Codes (CRC)
 */

#ifndef JLS_CRC32_H__
#define JLS_CRC32_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @ingroup jls
 * @defgroup jls_crc CRC32
 *
 * @brief Cyclic Redundancy Codes (CRC).
 *
 * @{
 */

/**
 * @brief Compute the CRC-32
 *
 * @param crc The existing value for the crc which is used for continued block
 *      computations.  Pass 0 for the first block.
 * @param data The data for the CRC computation.
 * @param length The number of total_bytes in data.
 * @return The computed CRC-32.
 *
 * @see http://create.stephan-brumme.com/crc32/
 * @see https://pycrc.org
 */
uint32_t jls_crc32(uint32_t crc, uint8_t const *data, uint32_t length);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* JLS_CRC32_H__ */
