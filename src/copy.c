/*
 * Copyright 2023 Jetperch LLC
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


#include "jls/copy.h"
#include "jls/ec.h"
#include "jls/raw.h"
#include "jls/writer.h"
#include "jls/buffer.h"
#include "jls/cdef.h"
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>


#define PROGRESS_INTERVAL_BYTES (10000000LL)

#define MSG_ERROR(description, ec) do {                                                             \
    char msg_str[1024];                                                                             \
    if (NULL != msg_fn) {                                                                           \
        snprintf(msg_str, sizeof(msg_str), "%" PRIi64 ": ERROR %s | %d %s : %s (copy.c line %d)",   \
                 offset, description,                                                               \
                 ec, jls_error_code_name(ec), jls_error_code_description(ec),                       \
                 __LINE__);                                                                         \
        if (msg_fn(msg_user_data, msg_str)) {                                                       \
            cancel = 1;                                                                             \
        }                                                                                           \
    }                                                                                               \
} while (0)

#define VALIDATE(x, description) do {   \
    if (!(x)) {                         \
        *desc = (description);          \
        return JLS_ERROR_MESSAGE_INTEGRITY; \
    }                                   \
} while (0)

#define ROD(x, description) do {        \
    int32_t rc__ = (x);                 \
    if (rc__) {                         \
        *desc = (description);          \
        return rc__;                    \
    }                                   \
} while (0)


/**
 * @brief Copy one chunk read into buf.
 *
 * @param wr The destination writer.
 * @param hdr The source chunk header.
 * @param buf The source chunk payload of hdr->payload_length bytes.
 * @param data_types The data type for each signal written to wr, 0 if undefined.
 * @param[out] desc The failure description.
 * @return 0 or error code.
 *
 * The source is untrusted: validate every payload field before use.
 */
static int32_t copy_chunk(struct jls_wr_s * wr, const struct jls_chunk_header_s * hdr,
                          struct jls_buf_s * buf, uint32_t * data_types, const char ** desc) {
    uint32_t payload_length = hdr->payload_length;
    uint16_t signal_id = hdr->chunk_meta & 0x0fff;
    switch (hdr->tag) {
        case JLS_TAG_SOURCE_DEF: {
            struct jls_source_def_s source;
            source.source_id = hdr->chunk_meta;
            ROD(jls_buf_rd_skip(buf, 64), "source_def parse");
            ROD(jls_buf_rd_str(buf, (const char **) &source.name), "source_def parse");
            ROD(jls_buf_rd_str(buf, (const char **) &source.vendor), "source_def parse");
            ROD(jls_buf_rd_str(buf, (const char **) &source.model), "source_def parse");
            ROD(jls_buf_rd_str(buf, (const char **) &source.version), "source_def parse");
            ROD(jls_buf_rd_str(buf, (const char **) &source.serial_number), "source_def parse");
            if (source.source_id != 0) {
                ROD(jls_wr_source_def(wr, &source), "jls_wr_source_def");
            }
            return 0;
        }
        case JLS_TAG_SIGNAL_DEF: {
            struct jls_signal_def_s signal;
            signal.signal_id = hdr->chunk_meta;
            ROD(jls_buf_rd_u16(buf, &signal.source_id), "signal_def parse");
            ROD(jls_buf_rd_u8(buf, &signal.signal_type), "signal_def parse");
            ROD(jls_buf_rd_skip(buf, 1), "signal_def parse");
            ROD(jls_buf_rd_u32(buf, &signal.data_type), "signal_def parse");
            ROD(jls_buf_rd_u32(buf, &signal.sample_rate), "signal_def parse");
            ROD(jls_buf_rd_u32(buf, &signal.samples_per_data), "signal_def parse");
            ROD(jls_buf_rd_u32(buf, &signal.sample_decimate_factor), "signal_def parse");
            ROD(jls_buf_rd_u32(buf, &signal.entries_per_summary), "signal_def parse");
            ROD(jls_buf_rd_u32(buf, &signal.summary_decimate_factor), "signal_def parse");
            ROD(jls_buf_rd_u32(buf, &signal.annotation_decimate_factor), "signal_def parse");
            ROD(jls_buf_rd_u32(buf, &signal.utc_decimate_factor), "signal_def parse");
            ROD(jls_buf_rd_skip(buf, 92), "signal_def parse");
            ROD(jls_buf_rd_str(buf, (const char **) &signal.name), "signal_def parse");
            ROD(jls_buf_rd_str(buf, (const char **) &signal.units), "signal_def parse");
            if ((signal.signal_id != 0) && (signal.signal_id < JLS_SIGNAL_COUNT)) {
                ROD(jls_wr_signal_def(wr, &signal), "jls_wr_signal_def");
                if (JLS_SIGNAL_TYPE_FSR == signal.signal_type) {
                    data_types[signal.signal_id] = signal.data_type;
                }
            }
            return 0;
        }

        case JLS_TAG_TRACK_FSR_DATA: {
            struct jls_fsr_data_s * data = (struct jls_fsr_data_s *) buf->start;
            VALIDATE(signal_id < JLS_SIGNAL_COUNT, "fsr_data signal_id");
            uint32_t data_type = data_types[signal_id];
            VALIDATE(data_type, "fsr_data for undefined signal");
            VALIDATE(payload_length >= sizeof(*data), "fsr_data payload too short");
            uint8_t size_bits = jls_datatype_parse_size(data_type);
            VALIDATE(data->header.entry_size_bits == size_bits, "fsr_data entry_size_bits");
            uint64_t data_bytes = ((uint64_t) data->header.entry_count * size_bits + 7) / 8;
            VALIDATE((sizeof(*data) + data_bytes) <= payload_length, "fsr_data entry_count");
            // future: handle omitted data by looking at level 1 index & summary
            // future: decompress if needed
            ROD(jls_wr_fsr(wr, signal_id, data->header.timestamp,
                           data->data, data->header.entry_count), "jls_wr_fsr");
            return 0;
        }

        case JLS_TAG_TRACK_ANNOTATION_DATA: {
            struct jls_annotation_s * data = (struct jls_annotation_s *) buf->start;
            const uint32_t data_offset = (uint32_t) offsetof(struct jls_annotation_s, data);  // < sizeof
            VALIDATE(payload_length >= data_offset, "annotation payload too short");
            VALIDATE(data->data_size <= (payload_length - data_offset), "annotation data_size");
            if ((JLS_STORAGE_TYPE_STRING == data->storage_type)
                    || (JLS_STORAGE_TYPE_JSON == data->storage_type)) {
                VALIDATE(memchr(data->data, 0, data->data_size), "annotation string not terminated");
            }
            ROD(jls_wr_annotation(wr, signal_id, data->timestamp, data->y,
                                  data->annotation_type, data->group_id, data->storage_type,
                                  data->data, data->data_size), "jls_wr_annotation");
            return 0;
        }

        case JLS_TAG_TRACK_UTC_DATA: {
            struct jls_utc_data_s * data = (struct jls_utc_data_s *) buf->start;
            VALIDATE(payload_length >= sizeof(*data), "utc payload too short");
            ROD(jls_wr_utc(wr, signal_id, data->header.timestamp, data->timestamp), "jls_wr_utc");
            return 0;
        }

        case JLS_TAG_USER_DATA: {
            enum jls_storage_type_e storage_type = (hdr->chunk_meta >> 12) & 0x000f;
            if (storage_type == JLS_STORAGE_TYPE_INVALID) {
                return 0;
            }
            if ((JLS_STORAGE_TYPE_STRING == storage_type) || (JLS_STORAGE_TYPE_JSON == storage_type)) {
                VALIDATE(memchr(buf->start, 0, payload_length), "user_data string not terminated");
            }
            ROD(jls_wr_user_data(wr, hdr->chunk_meta & 0x0fff, storage_type,
                                 buf->start, payload_length), "jls_wr_user_data");
            return 0;
        }

        default:
            return 0;  // regenerated by the writer (defs, heads, index, summary, end) or unknown
    }
}

int32_t jls_copy(const char * src, const char * dst,
                 jls_copy_msg_fn msg_fn, void * msg_user_data,
                 jls_copy_progress_fn progress_fn, void * progress_user_data) {
    int32_t rc = 0;
    int32_t cancel = 0;
    int64_t offset = 0;
    int64_t offset_end = 0;
    int64_t offset_progress = 0;
    struct jls_raw_s * rd = NULL;
    struct jls_wr_s * wr = NULL;
    struct jls_chunk_header_s hdr;
    uint32_t data_types[JLS_SIGNAL_COUNT];  // per signal: 0 when undefined
    memset(data_types, 0, sizeof(data_types));
    struct jls_buf_s * buf = jls_buf_alloc();
    if (NULL == buf) {
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }

    rc = jls_raw_open(&rd, src, "r");
    if (rc && (rc != JLS_ERROR_TRUNCATED)) {
        goto exit;
    }
    rc = 0;  // salvage a truncated source
    offset = jls_raw_chunk_tell(rd);
    rc = jls_raw_seek_end(rd);
    if (rc) {
        goto exit;
    }
    offset_end = jls_raw_chunk_tell(rd);
    rc = jls_raw_chunk_seek(rd, offset);
    if (rc) {
        goto exit;
    }

    rc = jls_wr_open(&wr, dst);
    if (rc) {
        goto exit;
    }

    // Salvage: report and skip damaged chunks, stop only when the source
    // has no further chunks or the destination fails.
    while ((offset < offset_end) && !cancel) {
        rc = jls_raw_rd_header(rd, &hdr);
        if (rc) {
            MSG_ERROR("jls_raw_rd_header", rc);
            rc = jls_raw_chunk_seek(rd, offset + 1);
            if (0 == rc) {
                rc = jls_raw_chunk_scan(rd);
            }
            if (rc) {
                MSG_ERROR("jls_raw_chunk_scan", rc);
                rc = 0;  // no further valid chunks
                break;
            }
            offset = jls_raw_chunk_tell(rd);
            continue;  // read the resynced chunk's header
        }
        rc = jls_buf_realloc(buf, (size_t) jls_raw_payload_size_on_disk(hdr.payload_length));
        if (rc) {
            MSG_ERROR("jls_buf_realloc", rc);
            rc = JLS_ERROR_NOT_ENOUGH_MEMORY;
            break;
        }
        uint32_t payload_max = (buf->alloc_size > UINT32_MAX) ? UINT32_MAX : (uint32_t) buf->alloc_size;
        rc = jls_raw_rd_payload(rd, payload_max, buf->start);
        if (rc) {
            MSG_ERROR("jls_raw_rd_payload", rc);
            // the valid header bounds the payload within the file
            rc = jls_raw_chunk_seek(rd, offset);
            if (0 == rc) {
                rc = jls_raw_chunk_next(rd);
            }
            if (rc) {
                MSG_ERROR("jls_raw_chunk_next", rc);
                rc = JLS_ERROR_IO;
                break;
            }
            offset = jls_raw_chunk_tell(rd);
            continue;
        }
        buf->length = hdr.payload_length;
        buf->cur = buf->start;
        buf->end = buf->start + buf->length;

        const char * desc = "";
        rc = copy_chunk(wr, &hdr, buf, data_types, &desc);
        if (rc) {
            MSG_ERROR(desc, rc);
            if ((JLS_ERROR_NOT_ENOUGH_MEMORY == rc) || (JLS_ERROR_IO == rc)) {
                break;  // destination failure: further writes cannot succeed
            }
            rc = 0;  // skip this chunk
        }

        offset = jls_raw_chunk_tell(rd);
        if ((offset - offset_progress) >= PROGRESS_INTERVAL_BYTES) {
            offset_progress = offset;
            if ((NULL != progress_fn)
                    && progress_fn(progress_user_data, offset / (double) offset_end)) {
                cancel = 1;
            }
        }
    }
    if (cancel && (0 == rc)) {
        rc = JLS_ERROR_ABORTED;
    }

exit:
    if (NULL != wr) {
        int32_t rc_close = jls_wr_close(wr);
        if (0 == rc) {
            rc = rc_close;
        }
    }
    jls_raw_close(rd);  // read-only: nothing to flush
    jls_buf_free(buf);
    if ((0 == rc) && (NULL != progress_fn)) {
        progress_fn(progress_user_data, 1.0);
    }
    return rc;
}
