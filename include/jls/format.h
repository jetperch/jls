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
 * @brief JLS file format.
 */

#ifndef JLS_FORMAT_H__
#define JLS_FORMAT_H__

#include "jls/cmacro.h"
#include <stdint.h>

/**
 * @ingroup jls
 * @defgroup jls_format File format
 *
 * @brief JLS file format.
 *
 * @{
 */

JLS_CPP_GUARD_START

#define JLS_FORMAT_VERSION_MAJOR  (1)
#define JLS_FORMAT_VERSION_MINOR  (0)
#define JLS_FORMAT_VERSION_PATCH  (0)
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
     * Each data entry consists of time in UTC and the data.
     */
    JLS_TRACK_TYPE_VSR = 1,

    /**
     * @brief Annotations contain infrequent, variable-typed data.
     *
     * @see jls_annotation_type_e for the annotation types.
     *
     * Each annotation data entry consists of time and associated
     * annotation data.
     * For FSR, time must be samples_id.
     * For VSR, time must be UTC.
     */
    JLS_TRACK_TYPE_ANNOTATION = 2,

    /**
     * @brief The UTC track associates sample_id with UTC.
     *
     * Each utc data entry consists of sample_id, UTC timestamp pairs.
     * This track is only used for FSR signals.
     */
    JLS_TRACK_TYPE_UTC = 3,

    /// The total number of track types
    JLS_TRACK_TYPE_COUNT = 4,
};

/**
 * @brief The data storage type.
 *
 * The data storage type applies to directly user-accessible data including
 * annotations and user_data.
 */
enum jls_storage_type_e {
    /// Invalid (unknown) storage type.
    JLS_STORAGE_TYPE_INVALID = 0,
    /// Raw binary data.
    JLS_STORAGE_TYPE_BINARY = 1,
    /// Null-terminated C-style string with UTF-8 encoding.
    JLS_STORAGE_TYPE_STRING = 2,
    /// JSON serialized data structure with NULL terminator and UTF-8 encoding.
    JLS_STORAGE_TYPE_JSON = 3,
};

/**
 * @brief The available annotation types.
 */
enum jls_annotation_type_e {
    /**
     * @brief Arbitrary user data.
     *
     * Application-dependent data with no standardized form or purpose.
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
     * - Number + a/b string, like "1a" and "1b", represent
     *   a marker pair (dual markers).
     * The name should be UTF-8 encoded.
     * The marker name can be followed by file separator ASCII 28 (0x1C)
     * and then followed by application-specific JSON-formatted metadata.
     * The jls_storage_type_e must be STRING.
     */
    JLS_ANNOTATION_TYPE_VERTICAL_MARKER = 2,

    /**
     * @brief A horizontal marker at a given y-axis value.
     *
     * Marker names can be arbitrary, but the convention is:
     * - Number strings, like "1", represent a single marker.
     * - Number string + a/b string, like "1a" and "1b", represent
     *   a marker pair (dual markers).
     * The jls_storage_type_e must be STRING.
     */
    JLS_ANNOTATION_TYPE_HORIZONTAL_MARKER = 3,

    /// consider sample_loss: start, stop
};

/**
 * @brief The chunks used to store the track information.
 */
enum jls_track_chunk_e {
    /**
     * @brief Track definition chunk.
     *
     * This chunk contains a zero-length (empty) payload.
     */
    JLS_TRACK_CHUNK_DEF = 0,

    /**
     * @brief Track offsets for the first chunk at each level.
     *
     * @see jls_track_head_s for all track types.
     *
     * This chunk provides fast seek access to the first chunk at
     * each summary level for this track.  The payload is jls_track_head_s
     * which contains the offset to the first INDEX chunk.
     */
    JLS_TRACK_CHUNK_HEAD = 1,

    /**
     * @brief The data chunk.
     *
     * @see jls_fsr_f32_data_s for FSR float32.
     * @see jls_annotation_s for ANNOTATION.
     * @see jls_utc_data_s for UTC.
     *
     * The payload varies by track type and data format.  All DATA
     * payloads start with jls_payload_header_s.
     */
    JLS_TRACK_CHUNK_DATA = 2,

    /**
     * @brief Provides the timestamp and offset for each contributing data chunk.
     *
     * @see jls_fsr_index_s for FSR track types.
     * @see jls_index_s for all other track types.
     *
     * This chunk MUST be immediately follow by a SUMMARY chunk.
     * All INDEX payloads start with jls_payload_header_s.
     */
    JLS_TRACK_CHUNK_INDEX = 3,

    /**
     * @brief The summary chunk.
     *
     * @see jls_fsr_f32_summary_s for FSR float32.
     * @see jls_annotation_summary_s for ANNOTATION.
     * @see jls_utc_summary_s for UTC.
     *
     * The payload format is defined by the track type.
     * All CHUNK payloads start with jls_payload_header_s.
     */
    JLS_TRACK_CHUNK_SUMMARY = 4,
};

#define JLS_TRACK_TAG_FLAG (0x20U)

/**
 * @brief Pack the chunk tag.
 *
 * @param track_type The jls_track_type_e
 * @param track_chunk The jls_track_chunk_e
 * @return The tag value.
 */
#define JLS_TRACK_TAG_PACK(track_type, track_chunk) \
    (JLS_TRACK_TAG_FLAG | (((track_type) & 0x03) << 3) | ((track_chunk) & 0x07))

#define JLS_TRACK_TAG_PACKER(track_type, track_chunk) \
    JLS_TRACK_TAG_PACK(JLS_TRACK_TYPE_##track_type, JLS_TRACK_CHUNK_##track_chunk)

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
    JLS_TAG_TRACK_FSR_DEF               = JLS_TRACK_TAG_PACKER(FSR, DEF),
    JLS_TAG_TRACK_FSR_HEAD              = JLS_TRACK_TAG_PACKER(FSR, HEAD),
    JLS_TAG_TRACK_FSR_DATA              = JLS_TRACK_TAG_PACKER(FSR, DATA),
    JLS_TAG_TRACK_FSR_INDEX             = JLS_TRACK_TAG_PACKER(FSR, INDEX),
    JLS_TAG_TRACK_FSR_SUMMARY           = JLS_TRACK_TAG_PACKER(FSR, SUMMARY),

    JLS_TAG_TRACK_VSR_DEF               = JLS_TRACK_TAG_PACKER(VSR, DEF),
    JLS_TAG_TRACK_VSR_HEAD              = JLS_TRACK_TAG_PACKER(VSR, HEAD),
    JLS_TAG_TRACK_VSR_DATA              = JLS_TRACK_TAG_PACKER(VSR, DATA),
    JLS_TAG_TRACK_VSR_INDEX             = JLS_TRACK_TAG_PACKER(VSR, INDEX),
    JLS_TAG_TRACK_VSR_SUMMARY           = JLS_TRACK_TAG_PACKER(VSR, SUMMARY),

    JLS_TAG_TRACK_ANNOTATION_DEF        = JLS_TRACK_TAG_PACKER(ANNOTATION, DEF),
    JLS_TAG_TRACK_ANNOTATION_HEAD       = JLS_TRACK_TAG_PACKER(ANNOTATION, HEAD),
    JLS_TAG_TRACK_ANNOTATION_DATA       = JLS_TRACK_TAG_PACKER(ANNOTATION, DATA),
    JLS_TAG_TRACK_ANNOTATION_INDEX      = JLS_TRACK_TAG_PACKER(ANNOTATION, INDEX),
    JLS_TAG_TRACK_ANNOTATION_SUMMARY    = JLS_TRACK_TAG_PACKER(ANNOTATION, SUMMARY),

    JLS_TAG_TRACK_UTC_DEF               = JLS_TRACK_TAG_PACKER(UTC, DEF),
    JLS_TAG_TRACK_UTC_HEAD              = JLS_TRACK_TAG_PACKER(UTC, HEAD),
    JLS_TAG_TRACK_UTC_DATA              = JLS_TRACK_TAG_PACKER(UTC, DATA),
    JLS_TAG_TRACK_UTC_INDEX             = JLS_TRACK_TAG_PACKER(UTC, INDEX),
    JLS_TAG_TRACK_UTC_SUMMARY           = JLS_TRACK_TAG_PACKER(UTC, SUMMARY),

    // other tags
    JLS_TAG_USER_DATA                   = 0x40, // own doubly-linked list
    JLS_TAG_END                         = 0xFF, // present if file closed properly
};

#define JLS_DATATYPE_BASETYPE_INT        (0x01)
#define JLS_DATATYPE_BASETYPE_UNSIGNED   (0x02)
#define JLS_DATATYPE_BASETYPE_UINT       (JLS_DATATYPE_BASETYPE_INT | JLS_DATATYPE_BASETYPE_UNSIGNED)
#define JLS_DATATYPE_BASETYPE_FLOAT      (0x04)

/**
 * @brief Construct a JLS datatype.
 *
 * @param basetype The datatype base type, one of [INT, UINT, FLOAT, BOOL]
 * @param size The size in bits.  Only the following options are supported:
 *      - INT: 4, 8, 16, 24, 32, 64
 *      - UINT: 1, 4, 8, 16, 24, 32, 64
 *      - FLOAT: 32, 64
 *      - BOOL = UINT 1
 * @param q The signed fixed-point location, only valid for INT and UINT.
 *      Set to 0 for normal, whole numbers.
 *      Set to 0 for FLOAT and BOOL.
 *      The fixed point is between bits q and q-1.  The value is scaled
 *      by 2 ** -q.
 */
#define JLS_DATATYPE_DEF(basetype, size, q)     \
    (((JLS_DATATYPE_BASETYPE_##basetype) & 0x0f) |        \
     (((uint32_t) ((size) & 0xff)) << 8) |   \
     (((uint32_t) ((q) & 0xff)) << 16))

static inline uint8_t jls_datatype_parse_basetype(uint32_t dt) {
    return (uint8_t) (dt & 0x0f);
}

static inline uint8_t jls_datatype_parse_size(uint32_t dt) {
    return (uint8_t) ((dt >> 8) & 0xff);
}

static inline uint8_t jls_datatype_parse_q(uint32_t dt) {
    return (uint8_t) ((dt >> 16) & 0xff);
}

#define JLS_DATATYPE_I4  JLS_DATATYPE_DEF(INT, 4, 0)
#define JLS_DATATYPE_I8  JLS_DATATYPE_DEF(INT, 8, 0)
#define JLS_DATATYPE_I16 JLS_DATATYPE_DEF(INT, 16, 0)
#define JLS_DATATYPE_I24 JLS_DATATYPE_DEF(INT, 24, 0)
#define JLS_DATATYPE_I32 JLS_DATATYPE_DEF(INT, 32, 0)
#define JLS_DATATYPE_I64 JLS_DATATYPE_DEF(INT, 64, 0)

#define JLS_DATATYPE_U1  JLS_DATATYPE_DEF(UINT, 1, 0)
#define JLS_DATATYPE_U4  JLS_DATATYPE_DEF(UINT, 4, 0)
#define JLS_DATATYPE_U8  JLS_DATATYPE_DEF(UINT, 8, 0)
#define JLS_DATATYPE_U16 JLS_DATATYPE_DEF(UINT, 16, 0)
#define JLS_DATATYPE_U24 JLS_DATATYPE_DEF(UINT, 24, 0)
#define JLS_DATATYPE_U32 JLS_DATATYPE_DEF(UINT, 32, 0)
#define JLS_DATATYPE_U64 JLS_DATATYPE_DEF(UINT, 64, 0)
#define JLS_DATATYPE_BOOL JLS_DATATYPE_U1

#define JLS_DATATYPE_F32 JLS_DATATYPE_DEF(FLOAT, 32, 0)
#define JLS_DATATYPE_F64 JLS_DATATYPE_DEF(FLOAT, 64, 0)

/**
 * @brief The source definition.
 */
struct jls_source_def_s {
    // store unique source_id in chunk_meta
    /**
     * @brief The source identifier.
     *
     * 0 reserved for global annotations, must be unique per instance
     */
    uint16_t source_id;
    // on disk: reserve 64 bytes as 0 for future use
    const char * name;          ///< The source name string.
    const char * vendor;        ///< The vendor name string.
    const char * model;         ///< The model string.
    const char * version;       ///< The version string.
    const char * serial_number; ///< The serial number string.
};

/**
 * @brief The signal definition.
 */
struct jls_signal_def_s {       // 0 reserved for VSR annotations
    // store unique signal_id in chunk_meta
    /**
     * @brief The signal identifier.
     *
     * 0 to JLS_SIGNAL_COUNT - 1, must be unique per instance
     */
    uint16_t signal_id;
    uint16_t source_id;                 ///< The source identifier, must match a source_def.
    uint8_t signal_type;                ///< The jls_signal_type_e signal type.
    uint16_t rsv16_0;                   //
    uint32_t data_type;                 ///< The JLS_DATATYPE_* data type for this signal.
    uint32_t sample_rate;               ///< TThe sample rate per second (Hz).  0 for VSR.
    uint32_t samples_per_data;          ///< The number of samples per data chunk.  (write suggestion)
    uint32_t sample_decimate_factor;    ///< The number of samples per summary level 1 entry.
    uint32_t entries_per_summary;       ///< The number of entries per summary chunk.  (write suggestion)
    uint32_t summary_decimate_factor;   ///< The number of summaries per summary, level >= 2.
    uint32_t annotation_decimate_factor;  ///< The annotation decimate factor for summaries.
    uint32_t utc_decimate_factor;       ///< The UTC decimate factor for summaries.
    int64_t sample_id_offset;           ///< The sample id offset for the first sample.  (FSR only)
    // on disk: reserve 64 bytes as 0 for future use

    /**
     * @brief The signal name.
     *
     * While C supports all possible names, language bindings
     * may simplify signal lookup.  The Python pyjls package
     * allows lookup by "source_name.signal_name" and "signal_id".
     *
     * For full compatibility:
     * 1. Avoid names convertable to integers
     * 2. Avoid names with '.' characters
     * 3. Use ASCII or valid UTF-8 encoding
     */
    const char * name;
    const char * units;                 ///< The units string, normally as SI with no scale prefix.
};

//  struct jls_track_def_s  // empty, only need chunk_meta for now

/**
 * @brief The track head payload for JLS_TRACK_CHUNK_HEAD.
 */
struct jls_track_head_s {
    uint64_t offset[16];  // 0 = data, 1 = first summary, ...
};

/// The summary storage order for each entry
enum jls_summary_fsr_e {
    JLS_SUMMARY_FSR_MEAN = 0,
    JLS_SUMMARY_FSR_STD = 1,
    JLS_SUMMARY_FSR_MIN = 2,
    JLS_SUMMARY_FSR_MAX = 3,
    JLS_SUMMARY_FSR_COUNT = 4,   // must be last
};

/**
 * @brief Union structure for parsing 32-bit versions.
 */
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
 * - Zero padding of 0-7 bytes, so that the entire chunk will end on a multiple
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

/**
 * @brief The payload header for DATA, INDEX, and SUMMARY chunks.
 */
struct jls_payload_header_s {
    int64_t timestamp;          ///< The sample_id for the first entry.
    uint32_t entry_count;       ///< The total number of entries.
    uint16_t entry_size_bits;   ///< The size of each entry, in bits.
    uint16_t rsv16;             ///< Reserved.
};

/// The FSR data chunk format.
struct jls_fsr_data_s {
    struct jls_payload_header_s header;  ///< The payload header.

    /**
     * @brief The summary data.
     *
     * Although data's type is float, the actual data type depends upon
     * the signal definition.
     */
    float data[];
};

/**
 * @brief The payload for JLS_TAG_TRACK_FSR_INDEX chunks.
 *
 * @see jls_index_s for all other INDEX chunk types.
 *
 * Since FSR has a fixed sample rate, the header contains enough information
 * to fully identify the timestamp for each offset.  Therefore, no additional
 * time information is required per entry.
 */
struct jls_fsr_index_s {
    struct jls_payload_header_s header;
    uint64_t offsets[];         ///< The chunk file offsets, spaced by fixed time intervals.
};

/**
 * @brief The FSR summary chunk format with f32 values.
 *
 * This summary format is used by all types except u32, u64, i32, i64, f64.
 */
struct jls_fsr_f32_summary_s {
    struct jls_payload_header_s header;  ///< The payload
    float data[][JLS_SUMMARY_FSR_COUNT]; ///< The summary data, each entry is 4 x f32: mean, std, min, max.
};

/**
 * @brief The FSR summary chunk format with f64 values.
 *
 * This summary format is used by u64, i64, f64.
 */
struct jls_fsr_f64_summary_s {
    struct jls_payload_header_s header;   ///< The payload
    double data[][JLS_SUMMARY_FSR_COUNT]; ///< The summary data, each entry is 4 x f64: mean, std, min, max.
};

/**
 * @brief The entry format for JLS_TRACK_CHUNK_INDEX payloads.
 * @see jls_index_s
 */
struct jls_index_entry_s {
    int64_t timestamp;      ///< The timestamp for this entry.  sample_id for FSR, UTC for VSR.
    uint64_t offset;        ///< The chunk file offset.
};

/**
 * @brief The payload for JLS_TRACK_CHUNK_INDEX chunks.
 *
 * @see jls_fsr_index_s for JLS_TAG_TRACK_FSR_INDEX.
 *
 * The INDEX payload maps timestamps to offsets to allow fast seek.
 * However, the JLS_TAG_TRACK_FSR_INDEX uses the jls_fsr_index_s since
 * the timestamp provides unnecessary, duplicative information for FSR tracks.
 */
struct jls_index_s {
    struct jls_payload_header_s header;
    struct jls_index_entry_s entries[];
};

/**
 * @brief Hold a single annotation record.
 *
 * This structure is used by both the API and JLS_TAG_TRACK_ANNOTATION_DATA.
 */
struct jls_annotation_s {
    int64_t timestamp;          ///< The timestamp for this annotation.  sample_id for FSR, UTC for VSR.
    uint64_t rsv64_1;           ///< Reserved, write to 0.
    uint8_t annotation_type;    ///< The jls_annotation_type_e.
    uint8_t storage_type;       ///< The jls_storage_type_e.
    uint8_t group_id;           ///< The optional group identifier.  If unused, write to 0.
    uint8_t rsv8_1;             ///< Reserved, write to 0.
    float y;                    ///< The y-axis value or NAN to automatically position.
    uint32_t data_size;         ///< The size of data in bytes.
    uint8_t data[];             ///< The annotation data.
};

/**
 * @brief The entry format for JLS_TAG_TRACK_ANNOTATION_SUMMARY.
 * @see jls_annotation_summary_s
 */
struct jls_annotation_summary_entry_s {
    int64_t timestamp;          ///< The timestamp (duplicates INDEX).
    uint8_t annotation_type;    ///< The jls_annotation_s.annotation_type
    uint8_t group_id;           ///< The jls_annotation_s.group_id
    uint8_t rsv8_1;             ///< Reserved, write to 0.
    uint8_t rsv8_2;             ///< Reserved, write to 0.
    float y;                    ///< The jls_annotation_s.y
};

/**
 * @brief The payload format for JLS_TAG_TRACK_ANNOTATION_SUMMARY chunks.
 */
struct jls_annotation_summary_s {
    struct jls_payload_header_s header;
    struct jls_annotation_summary_entry_s entries[];
};

/**
 * @brief The entry format for JLS_TAG_TRACK_UTC_DATA.
 *
 * @see jls_utc_summary_s
 *
 * This same format is reused for summary entries.
 * UTC.DATA only exists to provide recovery in the event that the
 * file is not properly closed.
 */
struct jls_utc_data_s {
    struct jls_payload_header_s header;
    int64_t timestamp;         ///< The timestamp in UTC.
};

/**
 * @brief The entry format for JLS_TAG_TRACK_UTC_SUMMARY.
 *
 * @see jls_utc_summary_s
 *
 * This same format is reused for summary entries.
 * UTC.DATA only exists to provide recovery in the event that the
 * file is not properly closed.
 */
struct jls_utc_summary_entry_s {
    int64_t sample_id;         ///< The timestamp in sample ids (duplicates INDEX).
    int64_t timestamp;         ///< The timestamp in UTC.
};

/**
 * @brief The payload format for JLS_TAG_TRACK_UTC_SUMMARY chunks.
 */
struct jls_utc_summary_s {
    struct jls_payload_header_s header;
    struct jls_utc_summary_entry_s entries[];
};

JLS_CPP_GUARD_END

/** @} */

#endif  /* JLS_FORMAT_H__ */
