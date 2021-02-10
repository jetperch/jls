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
 * | 1C                | File separator.                                     |
 */
#define JLS_HEADER_IDENTIFICATION \
    {0x6a, 0x6c, 0x73, 0x66, 0x6d, 0x74, 0x0d, 0x0a, \
     0x20, 0x0a, 0x20, 0x1a, 0x20, 0x20, 0xb2, 0x1c}

/**
 * @brief The maximum allowed number of sources.
 */
#define JLS_SOURCE_COUNT (256)

/**
 * @brief The maximum allowed number of signals.
 */
#define JLS_SIGNAL_COUNT (256)

/**
 * @brief The number of summary levels.
 */
#define JLS_SUMMARY_LEVEL_COUNT (16)

/**
 * @brief The signal type definition.
 */
enum jls_signal_type_e {
    /// Fixed sampling rate
    JLS_SIGNAL_TYPE_FSR = 0,
    /// Variable sampling rate
    JLS_SIGNAL_TYPE_VSR = 1,
};

/**
 * @brief The available track types that store data over time.
 */
enum jls_track_type_e {
    /**
     * @brief Block tracks contain fixed sample-rate (FSR) data.
     *
     * The JLS_TAG_SIGNAL_DEF defines the sampling rate.
     */
    JLS_TRACK_TYPE_FSR = 0,

    /**
     * @brief Fixed-type, variable-sample-rate (VSR) time series data.
     *
     * Each entry consists of time in UTC and the data.
     */
    JLS_TRACK_TYPE_VSR = 1,

    /**
     * @brief Annotations contain infrequent, variable-typed data.
     *
     * @see jls_annotation_type_e for the annotation types.
     *
     * Each annotation entry consists of time and the data.
     * For FSR, time must be samples_id.
     * For VSR, time must be UTC.
     */
    JLS_TRACK_TYPE_ANNOTATION = 2,

    /**
     * @brief The UTC track associates sample_id with UTC.
     *
     * Each entry consists of sample_id, UTC pairs.  This track
     * is only used for FSR signals.
     */
    JLS_TRACK_TYPE_UTC = 3,
};

/**
 * @brief The data storage type.
 *
 * The data storage type applies to directly user-accessible data including
 * annotations and user_data.
 */
enum jls_storage_type_e {
    /// Raw binary data.
    JLS_STORAGE_TYPE_BINARY = 0,
    /// Null-terminated C-style string with UTF-8 encoding.
    JLS_STORAGE_TYPE_STRING = 1,
    /// JSON serialized data structure with NULL terminator and UTF-8 encoding.
    JLS_STORAGE_TYPE_JSON = 2,
};

/**
 * @brief The available annotation types.
 */
enum jls_annotation_type_e {
    /**
     * @brief Arbitrary user data.
     *
     * Application-dependent data with no standardized form or purpose.
     * This type supports any jls_storage_type_e.
     */
    JLS_ANNOTATION_TYPE_USER = 0,

    /**
     * @brief UTF-8 formatted text.
     *
     * Viewers should display this text at the appropriate location.
     * The jls_storage_type_e must be STRING.
     */
    JLS_ANNOTATION_TYPE_TEXT = 1,

    /**
     * @brief A vertical marker at a given time.
     *
     * Marker names can be arbitrary, but the convention is:
     * - Number strings, like "1", represent a single marker.
     * - Alpha + number string, like "A1" and "A2", represent
     *   a marker pair (dual markers).
     * The jls_storage_type_e must be STRING.
     */
    JLS_ANNOTATION_TYPE_MARKER = 2,
};

/**
 * @brief The chunks used to store the track information.
 */
enum jls_track_chunk_e {
    JLS_TRACK_CHUNK_DEF = 0,        // track definition
    JLS_TRACK_CHUNK_HEAD = 1,       // first INDEX chunk offset for each level (16 total)
    JLS_TRACK_CHUNK_INDEX = 2,      // The indices of all contributing summary chunks, followed immediately by summary
    JLS_TRACK_CHUNK_DATA = 3,       // sample data
    JLS_TRACK_CHUNK_SUMMARY = 4,    // reduction mean, min, max, std
};

/**
 * @brief The tag definitions.
 */
enum jls_tag_e {
    // CAUTION: update jls_tag_to_name on any changes
    JLS_TAG_INVALID                     = 0x00,

    // file definition tags
    JLS_TAG_SOURCE_DEF                  = 0x01,   // own doubly-linked list
    JLS_TAG_SIGNAL_DEF                  = 0x02,   // SIGNAL_DEF, TRACK_*_DEF, and TRACK_*_HEAD for doubly-linked list

    // track tags
    JLS_TAG_TRACK_FSR_DEF               = (0x20 | (JLS_TRACK_TYPE_FSR << 3) | JLS_TRACK_CHUNK_DEF),
    JLS_TAG_TRACK_FSR_HEAD              = (0x20 | (JLS_TRACK_TYPE_FSR << 3) | JLS_TRACK_CHUNK_HEAD),
    JLS_TAG_TRACK_FSR_INDEX             = (0x20 | (JLS_TRACK_TYPE_FSR << 3) | JLS_TRACK_CHUNK_INDEX),
    JLS_TAG_TRACK_FSR_DATA              = (0x20 | (JLS_TRACK_TYPE_FSR << 3) | JLS_TRACK_CHUNK_DATA),
    JLS_TAG_TRACK_FSR_SUMMARY           = (0x20 | (JLS_TRACK_TYPE_FSR << 3) | JLS_TRACK_CHUNK_SUMMARY),

    JLS_TAG_TRACK_VSR_DEF               = (0x20 | (JLS_TRACK_TYPE_VSR << 3) | JLS_TRACK_CHUNK_DEF),
    JLS_TAG_TRACK_VSR_HEAD              = (0x20 | (JLS_TRACK_TYPE_VSR << 3) | JLS_TRACK_CHUNK_HEAD),
    JLS_TAG_TRACK_VSR_INDEX             = (0x20 | (JLS_TRACK_TYPE_VSR << 3) | JLS_TRACK_CHUNK_INDEX),
    JLS_TAG_TRACK_VSR_DATA              = (0x20 | (JLS_TRACK_TYPE_VSR << 3) | JLS_TRACK_CHUNK_DATA),
    JLS_TAG_TRACK_VSR_SUMMARY           = (0x20 | (JLS_TRACK_TYPE_VSR << 3) | JLS_TRACK_CHUNK_SUMMARY),

    JLS_TAG_TRACK_ANNOTATION_DEF        = (0x20 | (JLS_TRACK_TYPE_ANNOTATION << 3) | JLS_TRACK_CHUNK_DEF),  // not used
    JLS_TAG_TRACK_ANNOTATION_HEAD       = (0x20 | (JLS_TRACK_TYPE_ANNOTATION << 3) | JLS_TRACK_CHUNK_HEAD),
    JLS_TAG_TRACK_ANNOTATION_INDEX      = (0x20 | (JLS_TRACK_TYPE_ANNOTATION << 3) | JLS_TRACK_CHUNK_INDEX),
    JLS_TAG_TRACK_ANNOTATION_DATA       = (0x20 | (JLS_TRACK_TYPE_ANNOTATION << 3) | JLS_TRACK_CHUNK_DATA),
    JLS_TAG_TRACK_ANNOTATION_SUMMARY    = (0x20 | (JLS_TRACK_TYPE_ANNOTATION << 3) | JLS_TRACK_CHUNK_SUMMARY),

    JLS_TAG_TRACK_UTC_DEF               = (0x20 | (JLS_TRACK_TYPE_UTC << 3) | JLS_TRACK_CHUNK_DEF),
    JLS_TAG_TRACK_UTC_HEAD              = (0x20 | (JLS_TRACK_TYPE_UTC << 3) | JLS_TRACK_CHUNK_HEAD),
    JLS_TAG_TRACK_UTC_INDEX             = (0x20 | (JLS_TRACK_TYPE_UTC << 3) | JLS_TRACK_CHUNK_INDEX),
    JLS_TAG_TRACK_UTC_DATA              = (0x20 | (JLS_TRACK_TYPE_UTC << 3) | JLS_TRACK_CHUNK_DATA),
    JLS_TAG_TRACK_UTC_SUMMARY           = (0x20 | (JLS_TRACK_TYPE_UTC << 3) | JLS_TRACK_CHUNK_SUMMARY),

    // other tags
    JLS_TAG_USER_DATA                   = 0x40, // own doubly-linked list
};

#define JLS_DATATYPE_BASETYPE_INT        (0x01)
#define JLS_DATATYPE_BASETYPE_UNSIGNED   (0x02)
#define JLS_DATATYPE_BASETYPE_UINT       (JLS_DATATYPE_INT | JLS_DATATYPE_UNSIGNED)
#define JLS_DATATYPE_BASETYPE_FLOAT      (0x04)

/**
 * @brief Construct a JLS datatype.
 *
 * @param basetype The datatype base type, one of [INT, UINT, FLOAT, BOOL]
 * @param size The size in bits.  Only the following options are supported:
 *      - INT: 4 * N where N is between 1 and 16, inclusive.
 *      - UINT: 1 (bool) or 4 * N where N is between 1 and 16, inclusive.
 *      - FLOAT: 32, 64
 * @param q The fixed-point location, only valid for INT and UINT.
 *      Set to 0 for normal, whole numbers.
 *      Set to 0 for FLOAT and BOOL.
 */
#define JLS_DATATYPE_DEF(basetype, size, q)     \
    (((JLS_DATATYPE_BASETYPE_##basetype) & 0x0f) |        \
     (((uint32_t) ((size) & 0xff)) << 8) |   \
     (((uint32_t) ((q) & 0xff)) << 16))

#define JLS_DATATYPE_I32 JLS_DATATYPE_DEF(INT, 32, 0)
#define JLS_DATATYPE_I64 JLS_DATATYPE_DEF(INT, 64, 0)
#define JLS_DATATYPE_U32 JLS_DATATYPE_DEF(UINT, 32, 0)
#define JLS_DATATYPE_U64 JLS_DATATYPE_DEF(UINT, 64, 0)
#define JLS_DATATYPE_F32 JLS_DATATYPE_DEF(FLOAT, 32, 0)
#define JLS_DATATYPE_F64 JLS_DATATYPE_DEF(FLOAT, 64, 0)
#define JLS_DATATYPE_BOOL JLS_DATATYPE_DEF(UINT, 1, 0)

struct jls_source_def_s {
    // store unique source_id in chunk_meta
    uint16_t source_id;          // 0 reserved for global annotations, must be unique
    // on disk: reserve 64 bytes as 0 for future use
    const char * name;
    const char * vendor;
    const char * model;
    const char * version;
    const char * serial_number;
};

struct jls_signal_def_s {       // 0 reserved for VSR annotations
    // store unique signal_id in chunk_meta
    uint16_t signal_id;
    uint16_t source_id;          // must match a source_def
    uint8_t signal_type;        // jls_signal_type_e
    uint16_t rsv16_0;
    uint32_t data_type;
    uint32_t sample_rate;       // 0 for VSR
    uint32_t samples_per_block;
    uint32_t summary_downsample;
    uint32_t utc_rate_auto;     // 0=off, else samples per UTC entry.
    // on disk: reserve 64 bytes as 0 for future use
    const char * name;
    const char * si_units;
};

//  struct jls_track_def_s  // empty, only need chunk_meta for now


struct jls_track_head_s {
    uint64_t offset[16];  // 0 = data, 1 = first summary, ...
};

// struct jls_track_index_s variable sized, format defined by track
// struct jls_track_data_s variable sized, format defined by track
// struct jls_track_summary_s variable sized, format defined by track

// struct jls_user_data_s contains raw data.  header field chuck_meta is user-defined.

union jls_version_u {
    uint32_t u32;
    struct {
        uint16_t patch;
        uint8_t minor;
        uint8_t major;
    } s;
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
     * @see JLS_FORMAT_VERSION_U32
     */
    union jls_version_u version;
    
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
     * The chunk header enables each chunk to participate in a
     * single, doubly-linked list.
     * This field indicates the location for the next item in the list.
     * The value is relative to start of the file.
     * 0 indicates end of list.
     *
     * This field allows simple, linear traversal of data, but the
     * next chunk is not known when the chunk is first created.
     * Therefore, this field requires that chunk headers are updated
     * when the software writes the next item to the file.
     */
    uint64_t item_next;

    /**
     * @brief The previous item.
     *
     * The chunk header enables each chunk to participate in a
     * single, doubly-linked list.
     * This field indicates the location for the previous item in the list.
     * The value is relative to start of the file.
     * 0 indicates start of list.
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
     * @brief The metadata associated with this chunk.
     *
     * Each tag is free to define the purpose of this field.
     * 
     * However, all data tags use this definition:
     * - chunk_meta[7:0] is the signal/time series identifier from 0 to 255.
     * - chunk_meta[11:8] is reserved.
     * - chunk_meta[15:12] contains the depth for this chunk from 0 to 15.
     *   - 0 = block (sample level)
     *   - 1 = First-level summary of block samples
     *   - 2 = Second-level summary of first-level summaries.
     *
     * User-data reserves chunk_meta[15:12] to store the storage_type and
     * another internal indications.  chunk_meta[11:0] may be assigned
     * by the specific application.
     */
    uint16_t chunk_meta;
    
    /**
     * @brief The length of the payload in bytes.  Can be 0.
     *
     * In addition to defining the payload size, this value is
     * also used for forward chunk traversal.
     */
    uint32_t payload_length;
    
    /**
     * @brief The length of the previous payload in bytes.
     *
     * Used for reverse chunk traversal.
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
