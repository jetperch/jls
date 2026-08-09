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
#include "jls/ec.h"
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

/**
 * @brief Find where the level-0 chain clamp should start.
 *
 * @param core The core instance.
 * @param track The track to repair.
 * @param index1_offset The offset of the last valid level-1 index chunk.
 * @param start[out] The offset of the last valid data chunk referenced
 *      by the level-1 index chain, or 0 when no reference was found.
 * @return 0 or error code.
 *
 * The crash window starts at the last data chunk covered by a level-1
 * index, so clamping the data chain from there is O(tail) instead of
 * O(file).  Omitted (fsr_omit) entries are 0 and skipped; when a whole
 * index holds only omitted entries, hop item_prev to the previous
 * index chunk.
 */
static int32_t data_index_backstep(struct jls_core_s * core, struct jls_core_track_s * track,
                                   int64_t * index1_offset, int64_t * entry_offset) {
    uint16_t signal_id = track->parent->signal_def.signal_id;
    uint8_t index_tag = jls_track_tag_pack(track->track_type, JLS_TRACK_CHUNK_INDEX);
    uint16_t meta = (uint16_t) ((signal_id & 0x0fff) | (1 << 12));
    *entry_offset = 0;

    ROE(jls_raw_chunk_seek(core->raw, *index1_offset));
    ROE(jls_core_rd_chunk_validate(core, index_tag, meta));
    int64_t item_prev = (int64_t) core->chunk_cur.hdr.item_prev;
    uint32_t entry_count;
    if (JLS_TRACK_TYPE_FSR == track->track_type) {
        struct jls_fsr_index_s * r = (struct jls_fsr_index_s *) core->buf->start;
        entry_count = r->header.entry_count;
        if ((sizeof(r->header) + entry_count * sizeof(r->offsets[0])) > core->buf->length) {
            return JLS_ERROR_MESSAGE_INTEGRITY;
        }
        for (uint32_t i = entry_count; i > 0; --i) {
            if (r->offsets[i - 1]) {
                *entry_offset = (int64_t) r->offsets[i - 1];
                break;
            }
        }
    } else {
        struct jls_index_s * r = (struct jls_index_s *) core->buf->start;
        entry_count = r->header.entry_count;
        if ((sizeof(r->header) + entry_count * sizeof(r->entries[0])) > core->buf->length) {
            return JLS_ERROR_MESSAGE_INTEGRITY;
        }
        for (uint32_t i = entry_count; i > 0; --i) {
            if (r->entries[i - 1].offset) {
                *entry_offset = (int64_t) r->entries[i - 1].offset;
                break;
            }
        }
    }
    // all entries omitted in this index: hop to the previous index chunk
    *index1_offset = ((item_prev > 0) && (item_prev < *index1_offset)) ? item_prev : 0;
    return 0;
}

/**
 * @brief Clamp the level-0 data chain within the crash window.
 *
 * @param core The core instance.
 * @param track The track to repair.
 * @param index1_offset The offset of the last valid level-1 index chunk, or 0.
 * @param head_offset The data chain head offset (track head_offsets[0]);
 *      cleared to 0 when no data chunk survives.
 * @return 0 or error code.
 *
 * Interleave a backward search over level-1 index chunks (which finds
 * the last indexed data chunk, skipping omitted entries) with a
 * forward walk of the data chain from its head, stopping at whichever
 * finishes first.  Cost is min(trailing omitted index chunks, total
 * data chunks), each chunk visited at most once, so neither a long
 * omitted tail (constant fsr_omit signals) nor a long data prefix
 * forces an O(file) walk.
 */
static int32_t data_chain_clamp(struct jls_core_s * core, struct jls_core_track_s * track,
                                int64_t index1_offset, int64_t * head_offset) {
    uint16_t signal_id = track->parent->signal_def.signal_id;
    uint8_t data_tag = jls_track_tag_pack(track->track_type, JLS_TRACK_CHUNK_DATA);
    struct jls_core_chain_walk_s fwd = {
        .offset = *head_offset,
        .prev = {.offset = 0},
        .tag = data_tag,
        .chunk_meta = signal_id,
        .done = false,
    };

    while (index1_offset || (!fwd.done && fwd.offset)) {
        if (index1_offset) {
            int64_t entry_offset = 0;
            ROE(data_index_backstep(core, track, &index1_offset, &entry_offset));
            if (entry_offset) {
                // index entries reference completed pre-crash writes;
                // an invalid target is interior damage, not truncation
                ROE(jls_raw_chunk_seek(core->raw, entry_offset));
                ROE(jls_core_rd_chunk_validate(core, data_tag, signal_id));
                int32_t rc = jls_core_repair_chain(core, entry_offset, data_tag, signal_id);
                if (JLS_ERROR_NOT_FOUND == rc) {
                    rc = 0;  // start already validated
                }
                return rc;
            }
        }
        if (!fwd.done && fwd.offset) {
            int32_t rc = jls_core_repair_chain_walk(core, &fwd, 1);
            if (JLS_ERROR_NOT_FOUND == rc) {
                *head_offset = 0;  // dangling data head: no data chunk survives
                return 0;
            }
            ROE(rc);
            if (fwd.done) {
                return 0;
            }
        }
    }
    return 0;
}

int32_t jls_track_repair_pointers(struct jls_core_track_s * track) {
    struct jls_core_signal_s * signal = track->parent;
    struct jls_core_s * core = signal->parent;
    struct jls_raw_s * raw = core->raw;
    int signal_id = (int) signal->signal_def.signal_id;

    JLS_LOGI("repair signal %d, track %d", signal_id, (int) track->track_type);
    struct jls_core_chunk_s index_chunk = {.offset=0};
    struct jls_core_chunk_s index_chunk_next = {.offset=0};
    struct jls_core_chunk_s summary_chunk = {.offset=0};

    // find first non-empty level
    int64_t * offsets = track->head_offsets;
    int level = JLS_SUMMARY_LEVEL_COUNT - 1;
    for (; (level > 0); --level) {
        if (offsets[level]) {
            if (0 == jls_raw_chunk_seek(raw, offsets[level])) {
                break;
            } else {
                offsets[level] = 0;
                track->index_head[level].offset = 0;
                track->summary_head[level].offset = 0;
                track->head_offsets[level] = 0;
            }
        }
    }

    int64_t offset = offsets[level];
    int64_t offset_descend_next = 0;
    int64_t offset_descend = 0;
    int64_t index1_offset = 0;  // last valid level-1 index chunk

    while (level > 0) {
        JLS_LOGI("repair signal_id %d track %d, level %d, offset %" PRIi64,
                 (int) signal_id, (int) track->track_type, (int) level, offset);
        bool descend = false;
        uint16_t meta = (uint16_t) ((signal_id & 0x0fff) | (level << 12));
        uint8_t index_tag = jls_track_tag_pack(track->track_type, JLS_TRACK_CHUNK_INDEX);
        uint8_t summary_tag = jls_track_tag_pack(track->track_type, JLS_TRACK_CHUNK_SUMMARY);
        if (jls_raw_chunk_seek(raw, offset)
                || jls_core_rd_chunk_validate(core, index_tag, meta)) {  // index
            descend = true;
        } else {
            index_chunk_next = core->chunk_cur;
            offset_descend_next = 0;
            if (JLS_TRACK_TYPE_FSR == track->track_type) {
                struct jls_fsr_index_s * r = (struct jls_fsr_index_s *) core->buf->start;
                if (r->header.entry_count > 0) {
                    offset_descend_next = r->offsets[r->header.entry_count - 1];
                }
            } else {
                struct jls_index_s * r = (struct jls_index_s *) core->buf->start;
                if (r->header.entry_count > 0) {
                    offset_descend_next = r->entries[r->header.entry_count - 1].offset;
                }
            }
            if (jls_core_rd_chunk_validate(core, summary_tag, meta)) {
                descend = true;
            } else {
                index_chunk = index_chunk_next;
                summary_chunk = core->chunk_cur;
                offset = index_chunk.hdr.item_next;  // next index
                if (offset && (offset <= index_chunk.offset)) {
                    offset = 0;  // chains always point forward; drop corrupt backward link
                }
                offset_descend = offset_descend_next;
                track->index_head[level].offset = index_chunk.offset;
                track->summary_head[level].offset = summary_chunk.offset;
                if (1 == level) {
                    index1_offset = index_chunk.offset;
                }
            }
        }

        if (descend || (0 == offset)) {
            if (offset_descend && index_chunk.offset && summary_chunk.offset) {
                JLS_LOGI("descend signal_id %d track %d, level %d, offset %" PRIi64,
                         (int) signal_id, (int) track->track_type, (int) level, offset_descend);
                index_chunk.hdr.item_next = 0;
                summary_chunk.hdr.item_next = 0;
                jls_core_update_chunk_header(core, &index_chunk);
                jls_core_update_chunk_header(core, &summary_chunk);
                offset = offset_descend;
            } else {
                JLS_LOGI("restart signal_id %d track %d, level %d, offset %" PRIi64,
                         (int) signal_id, (int) track->track_type, (int) level, offsets[level - 1]);
                track->index_head[level].offset = 0;
                track->summary_head[level].offset = 0;
                track->head_offsets[level] = 0;
                offset = offsets[level - 1];
            }
            index_chunk.offset = 0;
            summary_chunk.offset = 0;
            offset_descend = 0;
            --level;
        }
    }

    // update level 0 (data): clamp the chain at the first invalid chunk.
    // Start in the crash window (found via the level-1 index), not at the
    // chain head: the walk must stay O(tail), never O(file).
    JLS_LOGI("repair signal_id %d track %d, level 0, offset %" PRIi64,
             (int) signal_id, (int) track->track_type, offsets[0]);
    ROE(data_chain_clamp(core, track, index1_offset, &offsets[0]));

    ROE(jls_track_wr_head(track));
    return 0;
}
