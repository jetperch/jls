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
 * @brief JLS raw access.
 */

#ifndef JLS_RAW_H__
#define JLS_RAW_H__

#include <stdio.h>
#include <stdint.h>
#include "jls/cmacro.h"
#include "jls/format.h"

/**
 * @ingroup jls
 * @defgroup jls_raw JLS raw chunk-level access.
 *
 * @brief JLS raw access.
 *
 * This module provides low-level write, read, and modify
 * access to the JLS file format.  The implementation maintains
 * the state of the "active chunk".
 *
 * @{
 */

JLS_CPP_GUARD_START

/**
 * @brief The opaque JLS raw object.
 *
 * Use jls_raw_open() to create and jls_raw_close() to free.
 */
struct jls_raw_s;

/**
 * @brief Open a file and create a new JLS raw instance.
 *
 * @param[out] instance The new instance on success or NULL.
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
 * @brief Get the JLS file format version.
 *
 * @param self The JLS raw instance.
 * @return The JLS file format version.
 * @see jls_version_u32() to get the JLS implementation version.
 */
union jls_version_u jls_raw_version(struct jls_raw_s * self);

/**
 * @brief Get the backend associated with this instance.
 *
 * @param self The JLS raw instance.
 * @return The backend or NULL.
 */
struct jls_bkf_s * jls_raw_backend(struct jls_raw_s * self);

/**
 * @brief Write a chunk to the file at the current location and advance on success.
 *
 * @param self The JLS raw instance.
 * @param hdr The header with all fields populated except CRC32.
 * @param payload The payload of size hdr->payload_length bytes.
 * @return 0 or error code.
 */
int32_t jls_raw_wr(struct jls_raw_s * self, struct jls_chunk_header_s * hdr, const uint8_t * payload);

/**
 * @brief Write a chunk header to the file at the current location.
 *
 * @param self The JLS raw instance.
 * @param hdr The header with all fields populated except CRC32.
 *      For chunks being appended to the file, this function will
 *      also automatically set payload_prev_length.
 *      This function modifies the structure in place with the actual
 *      values written in the file.
 * @return 0 or error code.
 */
int32_t jls_raw_wr_header(struct jls_raw_s * self, struct jls_chunk_header_s * hdr);

/**
 * @brief Write a chunk payload to the file at the current location.
 *
 * @param self The JLS raw instance.
 * @param payload_length The length of payload, in bytes, which must match the chunk header.
 * @param payload The payload of size hdr->payload_length bytes.
 * @return 0 or error code.
 */
int32_t jls_raw_wr_payload(struct jls_raw_s * self, uint32_t payload_length, const uint8_t * payload);

/**
 * @brief Read the current chunk and advance on success.
 *
 * @param self The JLS raw instance.
 * @param[out] hdr The chunk header.
 * @param payload_length_max The maximum length in bytes for payload.
 * @param payload The payload data.
 * @return 0 or error code.
 */
int32_t jls_raw_rd(struct jls_raw_s * self, struct jls_chunk_header_s * hdr, uint32_t payload_length_max, uint8_t * payload);

/**
 * @brief Read the current chunk header.
 *
 * @param self The JLS raw instance.
 * @param[out] h The chunk header.
 * @return 0 or error code.
 */
int32_t jls_raw_rd_header(struct jls_raw_s * self, struct jls_chunk_header_s * h);

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
 * @return 0 or error code.
 */
int32_t jls_raw_chunk_seek(struct jls_raw_s * self, int64_t offset);

/**
 * @brief Seek to the file end for writing.
 *
 * @param self The JLS raw instance.
 * @return 0 or error code.
 */
int32_t jls_raw_seek_end(struct jls_raw_s * self);

/**
 * @brief Get the current chunk offset.
 *
 * @param self The JLS raw instance.
 * @return The chunk offset.
 * @see jls_raw_chunk_seek
 */
int64_t jls_raw_chunk_tell(struct jls_raw_s * self);

/**
 * @brief Scan for the next possible valid chunk.
 *
 * @param self The JLS raw instance.
 * @return 0 or error code.
 *
 * After a successful scan, use jls_raw_chunk_tell() to
 * get the position of the found chunk.
 */
int32_t jls_raw_chunk_scan(struct jls_raw_s * self);

/**
 * @brief Flush all JLS changes to disk.
 *
 * @param self The JLS raw instance.
 * @return 0 or error code.
 */
int32_t jls_raw_flush(struct jls_raw_s * self);

/**
 * @brief Navigate to the next chunk.
 *
 * @param self The JLS raw instance.
 * @return 0, JLS_ERROR_EMPTY at end, or error code.
 *
 * Caution: if this function reaches the end, then
 * jls_raw_chunk_prev() will fail.
 */
int32_t jls_raw_chunk_next(struct jls_raw_s * self);

/**
 * @brief Navigate to the previous chunk.
 *
 * @param self The JLS raw instance.
 * @return 0,
 *      JLS_ERROR_EMPTY at beginning,
 *      JLS_ERROR_NOT_FOUND if jls_raw_chunk_next() reached end,
 *      or error code.
 */
int32_t jls_raw_chunk_prev(struct jls_raw_s * self);

/**
 * @brief Navigate to the next item in the doubly-linked list.
 *
 * @param self The JLS raw instance.
 * @return 0, JLS_ERROR_EMPTY at end, or error code.
 */
int32_t jls_raw_item_next(struct jls_raw_s * self);

/**
 * @brief Navigate to the previous item in the doubly-linked list.
 *
 * @param self The JLS raw instance.
 * @return 0, JLS_ERROR_EMPTY at beginning, or error code.
 */
int32_t jls_raw_item_prev(struct jls_raw_s * self);

/**
 * @brief Convert the JLS tag into a user-meaningful string.
 *
 * @param tag The tag value.
 * @return The string.
 */
const char * jls_tag_to_name(uint8_t tag);

/**
 * @brief Convert the data_type to a string.
 *
 * @param datatype The datatype.
 * @return The string representation of the datatype.
 */
const char * jls_dt_str(uint32_t datatype);

JLS_CPP_GUARD_END

/** @} */

#endif  /* JLS_RAW_H__ */
