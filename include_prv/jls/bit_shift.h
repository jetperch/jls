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
 * @brief JLS shift.
 */

#ifndef JLS_PRIV_SHIFT_H__
#define JLS_PRIV_SHIFT_H__

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @ingroup jls
 * @defgroup jls_bit_shift Bit shift
 *
 * @brief JLS bit shifting utilities.
 *
 * @{
 */

/**
 * @brief Right shift all bits in an array.
 *
 * @param bits The number of bits to shift which is between 0 and 8, inclusive.
 * @param[inout] data The data array to shift, modified in place.
 * @param size The size of data in bytes.
 * @return 0 or JLS_ERROR_PARAMETER_INVALID
 */
int32_t jls_bit_shift_array_right(uint8_t bits, void * data, size_t size);


/** @} */

#ifdef __cplusplus
}
#endif

#endif  /* JLS_PRIV_SHIFT_H__ */
