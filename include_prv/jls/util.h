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
 * @brief JLS utilities.
 */

#ifndef JLS_UTIL_H__
#define JLS_UTIL_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @ingroup jls
 * @defgroup jls_util Utilities
 *
 * @brief Common JLS utilities.
 *
 * @{
 */

/**
 * @brief Pack the chunk tag.
 *
 * @param track_type The jls_track_type_e
 * @param track_chunk The jls_track_chunk_e
 * @return The tag value.
 */
static inline uint8_t jls_track_tag_pack(uint8_t track_type, uint8_t track_chunk) {
    return JLS_TRACK_TAG_PACK(track_type, track_chunk);
}

/** @} */

#ifdef __cplusplus
}
#endif

#endif  /* JLS_UTIL_H__ */
