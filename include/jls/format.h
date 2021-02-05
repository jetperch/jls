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
 * @brief JLS file format.
 */

#ifndef JLS_FORMAT_H__
#define JLS_FORMAT_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @ingroup jls
 * @defgroup jls_format File format
 *
 * @brief JLS file format.
 *
 * @{
 */


#define JLS_FORMAT_VERSION_MAJOR  (0)
#define JLS_FORMAT_VERSION_MINOR  (1)
#define JLS_FORMAT_VERSION_PATCH  (1)
#define JLS_FORMAT_VERSION_U32     ((uint32_t) ( \
    ((JLS_FORMAT_VERSION_MAJOR & 0xff) << 24) | \
    ((JLS_FORMAT_VERSION_MINOR & 0xff) << 16) | \
    (JLS_FORMAT_VERSION_PATCH & 0xffff) ))

/**
 * @brief The file identification bytes at the start of the file.
 *
 * These bytes are not arbitrary.  We carefully selected them to provide:
 * - Identification: Help the application determine that
 *   this file is in the correct format with minimal uncertainty.
 * - Correct endianness:  Little endian has won, so this entire format is
 *   stored in little endian format.
 * - Proper binary processing:  The different line ending combinations
 *   ensure that the reader is not "fixing" the line endings, since this
 *   is a binary file format.
 * - Display: Include "substitute" and "file separator" so that text
 *   printers to not show the rest of the file.
 *
 * The following table describes maps each byte to its purpose:
 * | Value (hex)       | Purpose                                             |
 * | ----------------- | --------------------------------------------------- |
 * | 6A 6C 73 66 6d 74 | ASCII "jlsfmt", when viewed in text editor.         |
 * | 0D 0A             | DOS line ending to ensure binary correctness.       |
 * | 20                | ASCII space.                                        |
 * | 0A                | UNIX line ending to ensure binary correctness.      |
 * | 20                | ASCII space.                                        |
 * | 1A                | Substitute character (stops listing under Windows). |
 * | 20 20             | ASCII spaces.                                       |
 * | B2                | Ensure that system supports 8-bit data              |
 * | 1C                | File separater.                                     |
 */
#define JLS_HEADER_IDENTIFICATION \
    {0x6a, 0x6c, 0x73, 0x66, 0x6d, 0x74, 0x0d, 0x0a, \
     0x20, 0x0a, 0x20, 0x1a, 0x20, 0x20, 0xb2, 0x1c}


/**
 * @brief The tag definitions.
 */
enum jls_tag_e {
    JLS_TAG_SOURCE_DEF          = 0x00, // source_id, jls_source_def_s
    JLS_TAG_SIGNAL_DEF          = 0x01, // signal_id, source_id, jls_signal_def_s
    JLS_TAG_UTC_DEF             = 0x02, // utc_id, name
    JLS_TAG_USER_DATA           = 0x03,
    
    JLS_TAG_BLOCK_INDEX         = 0x04, // array of indices to each level
    JLS_TAG_BLOCK_DATA          = 0x05, // 64-bit sample_id, data
    JLS_TAG_BLOCK_SUMMARY       = 0x06, // array of indices, reduction mean, min, max, std
    
    JLS_TAG_ANNOTATION_INDEX    = 0x08,
    JLS_TAG_ANNOTATION_DATA     = 0x09,
    JLS_TAG_ANNOTATION_SUMMARY  = 0x0A,
    
    JLS_TAG_UTC_INDEX           = 0x0C,
    JLS_TAG_UTC_DATA            = 0x0D, // map sample_id to a timestamp for a utc_id.
    JLS_TAG_UTC_SUMMARY         = 0x0E,
    
    JLS_TAG_TS_DEF              = 0x13
    JLS_TAG_TS_INDEX            = 0x10, // map sample_id to a timestamp for a utc_id.
    JLS_TAG_TS_DATA             = 0x11, // utc, binary
    JLS_TAG_TS_SUMMARY          = 0x12, // array of utc, index
};


struct jls_source_def_s {
    const char * name;
    const char * vendor;
    const char * model;
    const char * version;
    const char * serial_number;
};


struct jls_signal_def_s {
    const char * name;
    const char * si_units;
    uint8_t data_type;
    uint8_t fixed_point_q;
    uint32_t sample_rate;
    uint32_t samples_per_block;
    uint32_t samples_per_summary;
};


/**
 * @brief The JLS file header structure.
 */
struct jls_file_header_s {
    /**
     * @brief The semi-unique file identification header.
     *
     * @see JLS_HEADER_IDENTIFICATION
     */
    uint8_t identification[16];
    
    /**
     * @brief The file length in bytes.
     *
     * This is the last field updated on file close.  If the file was not
     * closed gracefully, the value will be 0.
     */
    uint64_t length;
    
    
    /**
     * @brief The JLS file format version number.
     *
     * The format is major8.minor8.patch.
     * @see JLS_FORMAT_VERSION_U32
     */
    uint32_t version;  // major8.minor8.patch16
    
    /**
     * @brief The CRC32 from the start of the file through version.
     */
    uint32_t crc32;
};

/**
 * @brief The JLS chunk header structure.
 *
 * Every chunk starts with this header.  This header identifies the chunk's
 * payload, and enables the chunk to participate in one doubly-linked list.
 * 
 * If the length is zero, then the next chuck header immediately follows
 * this chunk header.  If the length is not zero, then the chunk consists of:
 * - A chunk header
 * - payload of length bytes
 * - Zero padding of 0-7 bytes, so that the entire chunk will end on a mulitple
 *   of 8 bytes.  This field ends on: 8 * k - 4
 * - crc32 over the payload.
 */
struct jls_chunk_header_s {
    /**
     * @brief The next item.
     *
     * Many tags, such as CHUNK, are connected as a doubly-linked list.
     * This field is the offset in bytes of the next item in the list
     * The value is relative to start of the file.
     */
    uint64_t item_next;

    /**
     * @brief The previous item.
     *
     * Many tags, such as CHUNK, are connected as a doubly-linked list.
     * This field is the offset in bytes of the previous item in the list
     * The value is relative to start of the file.
     */
    uint64_t item_prev;

    /**
     * @brief The tag.
     *
     * The jls_tag_e value that identifies the contents of this chunk.
     */
    uint8_t tag;
    
    /// Reserved for future use.  (compression?)
    uint8_t rsv0_u8;

    /**
     * @brief The tag info associated with this chunk.
     *
     *
     * Each tag is free to define the purpose of this field.
     * 
     * BLOCK, BLOCK_SUMMARY, and ANNOTATION define this as follows:
     * - tag_id[11:0] is the actual signal identifier from 0 to 4095.
     * - tag_id[15:12] contains the depth for this chunk from 0 to 15.
     *   - 0 = block
     *   - 1 = First-level summary of block
     *   - 2 = summary of first-level summaries.
     */
    uint16_t tag_id;
    
    /**
     * @brief The length of the payload in bytes.  Can be 0.
     */
    uint32_t payload_length;
    
    /**
     * @brief The length of the previous payload in bytes.
     */
    uint32_t payload_prev_length;
    
    /// The CRC32 over the header, excluding this field.
    uint32_t crc32;
};



/** @} */

#ifdef __cplusplus
}
#endif

#endif  /* JLS_FORMAT_H__ */
