/*
 * Copyright 2021 Jetperch LLC
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
 * @brief JLS raw access.
 */

#ifndef JLS_RAW_H__
#define JLS_RAW_H__

#include <stdio.h>
#include <stdint.h>
#include "jls/format.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @ingroup jls
 * @defgroup jls_raw JLS raw chunk-level access.
 *
 * @brief JLS raw access.
 *
 * @{
 */

/**
 * @brief The opaque JLS raw object.
 *
 * Use jls_raw_open() to create and jls_raw_close() to free.
 */
struct jls_raw_s;

/**
 * @brief Open a file and create a new JLS raw instance.
 *
 * @param instance[out] The new instance on success or NULL.
 * @param path The path to open.
 * @param mode The file open mode, which is one of "w", "r", "a".
 * @return 0 or error code.
 */
int32_t jls_raw_open(struct jls_raw_s ** instance, const char * path, const char * mode);

/**
 * @brief Close a file and free resources.
 *
 * @param self The JLS raw instance.
 * @return 0 or error code.
 */
int32_t jls_raw_close(struct jls_raw_s * self);

/**
 * @brief Write a chunk to the file at the current location.
 *
 * @param self The JLS raw instance.
 * @param tag The tag.
 * @param chuck_meta The chunk metadata.
 * @param payload_length The length of payload, in bytes.
 * @param payload The payload.
 * @return 0 or error code.
 */
int32_t jls_raw_wr(struct jls_raw_s * self, uint8_t tag, uint16_t chuck_meta, uint32_t payload_length, const uint8_t * payload);

/**
 * @brief Write a chunk header to the file at the current location.
 *
 * @param self The JLS raw instance.
 * @param tag The tag.
 * @param chuck_meta The chunk metadata.
 * @param payload_length The length of payload, in bytes.
 * @return 0 or error code.
 */
int32_t jls_raw_wr_header(struct jls_raw_s * self, uint8_t tag, uint16_t chuck_meta, uint32_t payload_length);

/**
 * @brief Write a chunk header to the file at the current location.
 *
 * @param self The JLS raw instance.
 * @param hdr The header structure to write.
 * @return 0 or error code.
 *
 * This function computes the CRC32 and updates the hdr instance.
 */
int32_t jls_raw_wr_header_struct(struct jls_raw_s * self, struct jls_chunk_header_s * hdr);

/**
 * @brief Write a chunk payload the the file at the current location and advance.
 *
 * @param self The JLS raw instance.
 * @param payload_length The length of payload, in bytes, which must match the chunk header.
 * @param payload The payload data.
 * @return 0 or error code.
 *
 * After writing the payload, advance to the next chunk location.
 */
int32_t jls_raw_wr_payload(struct jls_raw_s * self, uint32_t payload_length, const uint8_t * payload);

/**
 * @brief Read the current chunk and advance on success.
 *
 * @param self The JLS raw instance.
 * @param hdr[out] The chunk header.
 * @param payload_length_max The maximum length in bytes for payload.
 * @param payload The payload data.
 * @return 0 or error code.
 */
int32_t jls_raw_rd(struct jls_raw_s * self, struct jls_chunk_header_s * hdr, uint32_t payload_length_max, uint8_t * payload);

/**
 * @brief Read the current chunk header.
 *
 * @param self The JLS raw instance.
 * @param hdr[out] The chunk header.
 * @return 0 or error code.
 */
int32_t jls_raw_rd_header(struct jls_raw_s * self, struct jls_chunk_header_s * hdr);

/**
 * @brief Read the current chunk payload and advance on success.
 *
 * @param self The JLS raw instance.
 * @param payload_length_max The maximum length in bytes for payload.
 * @param payload The payload data.
 * @return 0 or error code.
 */
int32_t jls_raw_rd_payload(struct jls_raw_s * self, uint32_t payload_length_max, uint8_t * payload);

/**
 * @brief Seek to a chunk.
 *
 * @param self The JLS raw instance.
 * @param offset The chuck offset from a previous call to jls_raw_chunk_tell().
 * @param hdr[out] The chunk header at offset.
 * @return 0 or error code.
 */
int32_t jls_raw_chunk_seek(struct jls_raw_s * self, int64_t offset, struct jls_chunk_header_s * hdr);

/**
 * @brief Get the current chunk offset.
 *
 * @param self The JLS raw instance.
 * @return The chunk offset.
 * @see jls_raw_chunk_seek
 */
int64_t jls_raw_chunk_tell(struct jls_raw_s * self);

/**
 * @brief Navigate to the next chunk.
 *
 * @param self The JLS raw instance.
 * @param hdr[out] The next chunk header
 * @return 0, JLS_ERROR_EMPTY at end, or error code.
 */
int32_t jls_raw_chunk_next(struct jls_raw_s * self, struct jls_chunk_header_s * hdr);

/**
 * @brief Navigate to the previous chunk.
 *
 * @param self The JLS raw instance.
 * @param hdr[out] The previous chunk header
 * @return 0, JLS_ERROR_EMPTY at beginning, or error code.
 */
int32_t jls_raw_chunk_prev(struct jls_raw_s * self, struct jls_chunk_header_s * hdr);

/**
 * @brief Navigate to the next item in the doubly-linked list.
 *
 * @param self The JLS raw instance.
 * @param hdr[out] The chunk header for the next item.
 * @return 0, JLS_ERROR_EMPTY at end, or error code.
 */
int32_t jls_raw_item_next(struct jls_raw_s * self, struct jls_chunk_header_s * hdr);

/**
 * @brief Navigate to the previous item in the doubly-linked list.
 *
 * @param self The JLS raw instance.
 * @param hdr[out] The chunk header for the previous item.
 * @return 0, JLS_ERROR_EMPTY at beginning, or error code.
 */
int32_t jls_raw_item_prev(struct jls_raw_s * self, struct jls_chunk_header_s * hdr);


/** @} */

#ifdef __cplusplus
}
#endif

#endif  /* JLS_RAW_H__ */
