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
#include <stdio.h>


#define PROGRESS_INTERVAL_BYTES (10000000LL)

#define MSG_ERROR(description, ec) do {                                                             \
    char msg_str[1024];                                                                             \
    if (NULL != msg_fn) {                                                                           \
        snprintf(msg_str, sizeof(msg_str), "%" PRIi64 ": ERROR %s | %d %s : %s (copy.c line %d)",   \
                 offset, description,                                                               \
                 ec, jls_error_code_name(ec), jls_error_code_description(ec),                       \
                 __LINE__);                                                                         \
        msg_fn(msg_user_data, msg_str);                                                             \
    }                                                                                               \
} while (0)


int32_t jls_copy(const char * src, const char * dst,
                 jls_copy_msg_fn msg_fn, void * msg_user_data,
                 jls_copy_progress_fn progress_fn, void * progress_user_data) {
    int32_t rc = 0;
    int64_t offset = 0;
    int64_t offset_progress = 0;
    struct jls_raw_s * rd = NULL;
    struct jls_wr_s * wr = NULL;
    struct jls_buf_s * buf = jls_buf_alloc();
    if (NULL == buf) {
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }

    rc = jls_raw_open(&rd, src, "r");
    if (rc && (rc != JLS_ERROR_TRUNCATED)) {
        return rc;
    }
    offset = jls_raw_chunk_tell(rd);
    jls_raw_seek_end(rd);
    int64_t offset_end = jls_raw_chunk_tell(rd);
    jls_raw_chunk_seek(rd, offset);

    rc = jls_wr_open(&wr, dst);
    if (rc) {
        jls_raw_close(rd);
        return rc;
    }

    struct jls_chunk_header_s hdr;

    while (offset < offset_end) {
        rc = jls_raw_rd_header(rd, &hdr);
        if (rc) {
            MSG_ERROR("jls_raw_rd_header", rc);
            jls_raw_chunk_seek(rd, offset + 1);
            rc = jls_raw_chunk_scan(rd);
            if (rc) {
                MSG_ERROR("jls_raw_chunk_scan", rc);
                return rc;
            }
        }
        // printf("%" PRIi64 " %d %" PRIu32 "\n", offset, hdr.tag, hdr.payload_length);
        rc = jls_buf_realloc(buf, hdr.payload_length);
        if (rc) {
            MSG_ERROR("jls_buf_realloc", rc);
            return JLS_ERROR_NOT_ENOUGH_MEMORY;
        }
        rc = jls_raw_rd_payload(rd, (uint32_t) buf->alloc_size, buf->start);
        if (rc) {
            MSG_ERROR("jls_raw_rd_payload", rc);
            rc = jls_raw_chunk_next(rd);
            if (rc) {
                MSG_ERROR("jls_raw_chunk_next", rc);
                return JLS_ERROR_IO;
            }
            offset = jls_raw_chunk_tell(rd);
            continue;
        }
        buf->length = hdr.payload_length;
        buf->cur = buf->start;
        buf->end = buf->start + buf->length;

        switch (hdr.tag) {
            case JLS_TAG_INVALID: break;
            case JLS_TAG_SOURCE_DEF: {
                struct jls_source_def_s source;
                source.source_id = hdr.chunk_meta;
                ROE(jls_buf_rd_skip(buf, 64));
                ROE(jls_buf_rd_str(buf, (const char **) &source.name));
                ROE(jls_buf_rd_str(buf, (const char **) &source.vendor));
                ROE(jls_buf_rd_str(buf, (const char **) &source.model));
                ROE(jls_buf_rd_str(buf, (const char **) &source.version));
                ROE(jls_buf_rd_str(buf, (const char **) &source.serial_number));
                if (source.source_id != 0) {
                    ROE(jls_wr_source_def(wr, &source));
                }
                break;
            }
            case JLS_TAG_SIGNAL_DEF: {
                struct jls_signal_def_s signal;
                signal.signal_id = hdr.chunk_meta;
                ROE(jls_buf_rd_u16(buf, &signal.source_id));
                ROE(jls_buf_rd_u8(buf, &signal.signal_type));
                ROE(jls_buf_rd_skip(buf, 1));
                ROE(jls_buf_rd_u32(buf, &signal.data_type));
                ROE(jls_buf_rd_u32(buf, &signal.sample_rate));
                ROE(jls_buf_rd_u32(buf, &signal.samples_per_data));
                ROE(jls_buf_rd_u32(buf, &signal.sample_decimate_factor));
                ROE(jls_buf_rd_u32(buf, &signal.entries_per_summary));
                ROE(jls_buf_rd_u32(buf, &signal.summary_decimate_factor));
                ROE(jls_buf_rd_u32(buf, &signal.annotation_decimate_factor));
                ROE(jls_buf_rd_u32(buf, &signal.utc_decimate_factor));
                ROE(jls_buf_rd_skip(buf, 92));
                ROE(jls_buf_rd_str(buf, (const char **) &signal.name));
                ROE(jls_buf_rd_str(buf, (const char **) &signal.units));
                if (signal.signal_id != 0) {
                    ROE(jls_wr_signal_def(wr, &signal));
                }
                break;
            }

            case JLS_TAG_TRACK_FSR_DEF: break;
            case JLS_TAG_TRACK_FSR_HEAD: break;
            case JLS_TAG_TRACK_FSR_DATA: {
                uint16_t signal_id = hdr.chunk_meta & 0x0fff;
                struct jls_fsr_data_s * data = (struct jls_fsr_data_s *) buf->start;
                // future: handle omitted data by looking at level 1 index & summary
                // future: decompress if needed
                ROE(jls_wr_fsr(wr, signal_id, data->header.timestamp,
                               data->data, data->header.entry_count));
                break;
            }
            case JLS_TAG_TRACK_FSR_INDEX: break;
            case JLS_TAG_TRACK_FSR_SUMMARY: break;

            case JLS_TAG_TRACK_VSR_DEF: break;
            case JLS_TAG_TRACK_VSR_HEAD: break;
            case JLS_TAG_TRACK_VSR_DATA: break;
            case JLS_TAG_TRACK_VSR_INDEX: break;
            case JLS_TAG_TRACK_VSR_SUMMARY: break;

            case JLS_TAG_TRACK_ANNOTATION_DEF: break;
            case JLS_TAG_TRACK_ANNOTATION_HEAD: break;
            case JLS_TAG_TRACK_ANNOTATION_DATA: {
                uint16_t signal_id = hdr.chunk_meta & 0x0fff;
                struct jls_annotation_s * data = (struct jls_annotation_s *) buf->start;
                ROE(jls_wr_annotation(wr, signal_id, data->timestamp, data->y,
                            data->annotation_type, data->group_id, data->storage_type,
                            data->data, data->data_size));
                break;
            }
            case JLS_TAG_TRACK_ANNOTATION_INDEX: break;
            case JLS_TAG_TRACK_ANNOTATION_SUMMARY: break;

            case JLS_TAG_TRACK_UTC_DEF: break;
            case JLS_TAG_TRACK_UTC_HEAD: break;
            case JLS_TAG_TRACK_UTC_DATA: {
                uint16_t signal_id = hdr.chunk_meta & 0x0fff;
                struct jls_utc_data_s * data = (struct jls_utc_data_s *) buf->start;
                ROE(jls_wr_utc(wr, signal_id, data->header.timestamp, data->timestamp));
                break;
            }
            case JLS_TAG_TRACK_UTC_INDEX: break;
            case JLS_TAG_TRACK_UTC_SUMMARY: break;

            case JLS_TAG_USER_DATA: {
                enum jls_storage_type_e storage_type = (hdr.chunk_meta >> 12) & 0x000f;
                if (storage_type != JLS_STORAGE_TYPE_INVALID) {
                    ROE(jls_wr_user_data(wr, hdr.chunk_meta & 0x0fff, (hdr.chunk_meta >> 12) & 0x000f,
                                         buf->start, hdr.payload_length));
                }
                break;
            }
            case JLS_TAG_END: break;
            default: break;
        }
        offset = jls_raw_chunk_tell(rd);
        if ((offset - offset_progress) >= PROGRESS_INTERVAL_BYTES) {
            if (NULL != progress_fn) {
                progress_fn(progress_user_data, offset / (double) offset_end);
            }
            offset_progress = offset;
        }
    }
    if (NULL != progress_fn) {
        progress_fn(progress_user_data, 1.0);
    }
    jls_raw_close(rd);
    jls_wr_close(wr);
    return 0;
}
