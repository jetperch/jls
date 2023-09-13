/*
 * Copyright 2021-2023 Jetperch LLC
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

#include "jls/track.h"
#include "jls/cdef.h"
#include "jls/log.h"
#include "jls/util.h"
#include <inttypes.h>
#include <string.h>


int32_t jls_track_wr_def(struct jls_core_track_s * track_info) {
    // construct track definition (no payload)
    struct jls_core_s * wr = track_info->parent->parent;
    struct jls_core_chunk_s chunk;
    memset(&chunk, 0, sizeof(chunk));
    chunk.hdr.item_next = 0;  // update later
    chunk.hdr.item_prev = wr->signal_head.offset;
    chunk.hdr.tag = jls_track_tag_pack(track_info->track_type, JLS_TRACK_CHUNK_DEF);
    chunk.hdr.rsv0_u8 = 0;
    chunk.hdr.chunk_meta = track_info->parent->signal_def.signal_id;
    chunk.hdr.payload_length = 0;
    chunk.offset = jls_raw_chunk_tell(wr->raw);

    // write
    ROE(jls_raw_wr(wr->raw, &chunk.hdr, NULL));
    return jls_core_update_item_head(wr, &wr->signal_head, &chunk);
}

int32_t jls_track_wr_head(struct jls_core_track_s * track_info) {
    // construct header
    struct jls_core_s * wr = track_info->parent->parent;
    struct jls_core_chunk_s * chunk = &track_info->head;
    if (!chunk->offset) {
        chunk->hdr.item_next = 0;  // update later
        chunk->hdr.item_prev = wr->signal_head.offset;
        chunk->hdr.tag = jls_track_tag_pack(track_info->track_type, JLS_TRACK_CHUNK_HEAD);
        chunk->hdr.rsv0_u8 = 0;
        chunk->hdr.chunk_meta = track_info->parent->signal_def.signal_id;
        chunk->hdr.payload_length = sizeof(track_info->head_offsets);
        chunk->offset = jls_raw_chunk_tell(wr->raw);
        JLS_LOGD1("jls_track_wr_head %d 0x%02x new %" PRIi64, (int) chunk->hdr.chunk_meta, chunk->hdr.tag, chunk->offset);
        ROE(jls_raw_wr(wr->raw, &chunk->hdr, (uint8_t *) track_info->head_offsets));
        track_info->head = *chunk;
        return jls_core_update_item_head(wr, &wr->signal_head, chunk);
    } else {
        JLS_LOGD1("jls_track_wr_head %d 0x%02x update %" PRIi64, (int) chunk->hdr.chunk_meta, chunk->hdr.tag, chunk->offset);
        int64_t pos = jls_raw_chunk_tell(wr->raw);
        ROE(jls_raw_chunk_seek(wr->raw, chunk->offset));
        ROE(jls_raw_wr_payload(wr->raw, sizeof(track_info->head_offsets), (uint8_t *) track_info->head_offsets));
        ROE(jls_raw_chunk_seek(wr->raw, pos));
    }
    return 0;
}

int32_t jls_track_update(struct jls_core_track_s * track, uint8_t level, int64_t pos) {
    if (!track->head_offsets[level]) {
        track->head_offsets[level] = pos;
        ROE(jls_track_wr_head(track));
    }
    return 0;
}

int32_t jls_track_repair_pointers(struct jls_core_track_s * track) {
    struct jls_core_signal_s * signal = track->parent;
    struct jls_core_s * core = signal->parent;
    struct jls_raw_s * raw = core->raw;
    int signal_id = (int) signal->signal_def.signal_id;

    JLS_LOGI("repair signal %d, track %d", signal_id, (int) track->track_type);
    struct jls_core_chunk_s index_chunk;
    struct jls_core_chunk_s summary_chunk;

    // find first non-empty level
    int64_t * offsets = track->head_offsets;
    int level = JLS_SUMMARY_LEVEL_COUNT - 1;
    for (; (level > 0); --level) {
        if (offsets[level]) {
            break;
        }
    }

    int64_t offset_next = 0;
    int64_t offset = 0;
    bool skip_this = false;
    bool skip_next = false;

    for (; level > 0; --level) {
        JLS_LOGI("repair signal %d, track %d", signal_id, (int) track->track_type, level);
        offset = offset_next ? offset_next : offsets[level];
        skip_next = offset_next;
        offset_next = 0;
        index_chunk.offset = 0;
        summary_chunk.offset = 0;
        int counter = 0;

        while (offset) {
            skip_this = skip_next;
            skip_next = false;
            JLS_LOGI("repair signal_id %d, level %d, offset %" PRIi64 " %s",
                     (int) signal_id, (int) level, offset, skip_this ? "skip" : "");
            // read index and summary chunks
            if (jls_raw_chunk_seek(raw, offset)) {
                break;
            }
            if (jls_core_rd_chunk(core)) {
                break;
            }
            track->index_head[level] = core->chunk_cur;
            offset = core->chunk_cur.hdr.item_next;
            int64_t offset_next_tmp = offset_next;

            if (JLS_TRACK_TYPE_FSR == track->track_type) {
                struct jls_fsr_index_s * r = (struct jls_fsr_index_s *) core->buf->start;
                if (r->header.entry_count > 0) {
                    offset_next_tmp = r->offsets[r->header.entry_count - 1];
                }
            } else {
                struct jls_index_s * r = (struct jls_index_s *) core->buf->start;
                if (r->header.entry_count > 0) {
                    offset_next_tmp = r->entries[r->header.entry_count - 1].offset;
                }
            }

            if (jls_core_rd_chunk(core)) {
                break;
            }
            track->summary_head[level] = core->chunk_cur;

            index_chunk = track->index_head[level];
            summary_chunk = track->summary_head[level];
            offset_next = offset_next_tmp;
            ++counter;
        }

        if (counter == 0) {
            offsets[level] = 0;
            track->index_head[level].offset = 0;
            track->summary_head[level].offset = 0;
        } else {
            index_chunk.hdr.item_next = 0;
            jls_core_update_chunk_header(core, &index_chunk);
            summary_chunk.hdr.item_next = 0;
            jls_core_update_chunk_header(core, &summary_chunk);
        }
    }

    // update level 0 (data)
    offset = offset_next ? offset_next : offsets[0];
    struct jls_core_chunk_s data_chunk = {.offset=0};
    while (offset) {
        if (jls_core_rd_chunk(core)) {
            if (data_chunk.offset) {
                data_chunk.hdr.item_next = 0;
                jls_core_update_chunk_header(core, &summary_chunk);
            }
            break;
        }
        data_chunk = core->chunk_cur;
        offset = core->chunk_cur.hdr.item_next;
    }

    ROE(jls_track_wr_head(track));
    return 0;
}
