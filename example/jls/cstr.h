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
 * @brief Safe C-style string utilities.
 */

#ifndef JLS_EXAMPLE_CSTR_H_
#define JLS_EXAMPLE_CSTR_H_

#include "jls/cmacro.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

JLS_CPP_GUARD_START

/**
 * @brief Convert a string to an unsigned 16-bit integer.
 *
 * @param src The input source string containing an integer.  Strings that
 *      start with "0x" are processed as case-insensitive hexadecimal.
 * @param value The output unsigned 32-bit integer value.
 * @return 0 on success or error code.  On error, the value will not be
 *      modified.  To allow default values on parsing errors, set value
 *      before calling this function.
 */
int jls_cstr_to_u16(const char * src, uint16_t * value);

/**
 * @brief Convert a string to an signed 16-bit integer.
 *
 * @param src The input source string containing an integer.
 * @param value The output integer value.
 * @return 0 on success or error code.  On error, the value will not be
 *      modified.  To allow default values on parsing errors, set value
 *      before calling this function.
 */
int jls_cstr_to_i16(const char * src, int16_t * value);

/**
 * @brief Convert a string to an unsigned 32-bit integer.
 *
 * @param src The input source string containing an integer.  Strings that
 *      start with "0x" are processed as case-insensitive hexadecimal.
 * @param value The output unsigned 32-bit integer value.
 * @return 0 on success or error code.  On error, the value will not be
 *      modified.  To allow default values on parsing errors, set value
 *      before calling this function.
 */
int jls_cstr_to_u32(const char * src, uint32_t * value);

/**
 * @brief Convert a string to an signed 32-bit integer.
 *
 * @param src The input source string containing an integer.
 * @param value The output integer value.
 * @return 0 on success or error code.  On error, the value will not be
 *      modified.  To allow default values on parsing errors, set value
 *      before calling this function.
 */
int jls_cstr_to_i32(const char * src, int32_t * value);

/**
 * @brief Convert a string to an unsigned 64-bit integer.
 *
 * @param src The input source string containing an integer.  Strings that
 *      start with "0x" are processed as case-insensitive hexadecimal.
 * @param value The output integer value.
 * @return 0 on success or error code.  On error, the value will not be
 *      modified.  To allow default values on parsing errors, set value
 *      before calling this function.
 */
int jls_cstr_to_u64(const char * src, uint64_t * value);

/**
 * @brief Convert a string to an signed 64-bit integer.
 *
 * @param src The input source string containing an integer.
 * @param value The output integer value.
 * @return 0 on success or error code.  On error, the value will not be
 *      modified.  To allow default values on parsing errors, set value
 *      before calling this function.
 */
int jls_cstr_to_i64(const char * src, int64_t * value);

/**
 * @brief Convert a string to a floating point number.
 *
 * @param src The input source string containing a floating point number.
 * @param value The output floating point value.
 * @return 0 on success or error code.  On error, the value will not be
 *      modified.  To allow default values on parsing errors, set value
 *      before calling this function.
 */
int jls_cstr_to_f32(const char * src, float * value);


JLS_CPP_GUARD_END

/** @} */

#endif /* JLS_EXAMPLE_CSTR_H_ */
