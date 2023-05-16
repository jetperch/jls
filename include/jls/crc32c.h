/*
 * Copyright 2014-2022 Jetperch LLC
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

#ifndef JLS_CRC32C_H__
#define JLS_CRC32C_H__

#include "jls/cmacro.h"
#include <stdint.h>

JLS_CPP_GUARD_START

/**
 * @ingroup jls
 * @defgroup jls_crc32c CRC32C
 *
 * @brief Cyclic Redundancy Codes (CRC).
 *
 * @{
 */

// Forward declaration for format.h
struct jls_chunk_header_s;

/**
 * @brief Compute the CRC-32C over a chunk header.
 *
 * @param hdr The chunk header for the CRC computation which MUST
 *      be aligned on a 64-bit (8 byte) boundary.
 * @return The computed CRC-32C.
 */
uint32_t jls_crc32c_hdr(const struct jls_chunk_header_s * hdr);


/**
 * @brief Compute the CRC-32C
 *
 * @param data The data for the CRC computation.
 * @param length The number of total_bytes in data.
 * @return The computed CRC-32C.
 */
JLS_API uint32_t jls_crc32c(uint8_t const *data, uint32_t length);

JLS_CPP_GUARD_END

/** @} */

#endif /* JLS_CRC32C_H__ */
