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
 * @brief C definition.
 */

#ifndef JLS_CDEF_H__
#define JLS_CDEF_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @ingroup jls
 * @defgroup jls_cdef Common C definitions
 *
 * @brief Common C definitions.
 *
 * @{
 */

#define ROE(x)  do {                        \
    int32_t rc__ = (x);                     \
    if (rc__) {                             \
        return rc__;                        \
    }                                       \
} while (0)

#define RLE(x)  do {                        \
    int32_t rc__ = (x);                     \
    if (rc__) {                             \
        JLS_LOGE("error %d: " #x, rc__);    \
        return rc__;                        \
    }                                       \
} while (0)

/**
 * @brief Perform a compile-time check
 *
 * @param COND The condition which should normally be true.
 * @param MSG The error message which must be a valid C identifier  This
 *      message will be cryptically displayed by the compiler on error.
 */
#define JLS_STATIC_ASSERT(COND, MSG) typedef char static_assertion_##MSG[(COND)?1:-1]

/**
 * @brief Compute the number of elements in an array.
 *
 * @param x The array (NOT a pointer).
 * @return The number of elements in the array.
 */
#define JLS_ARRAY_SIZE(x) ( sizeof(x) / sizeof((x)[0]) )



/** @} */

#ifdef __cplusplus
}
#endif

#endif  /* JLS_CDEF_H__ */
