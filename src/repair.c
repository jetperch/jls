/*
 * Copyright 2026 Jetperch LLC
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

#include "jls/repair.h"
#include "jls/backend.h"
#include "jls/buffer.h"
#include "jls/cdef.h"
#include "jls/core.h"
#include "jls/ec.h"
#include "jls/log.h"
#include "jls/raw.h"
#include "jls/reader.h"
#include "jls/util.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define PROGRESS_INTERVAL_BYTES (10000000LL)
#define DEAD_SPAN_MAX (4096)
#define HEAD_PAYLOAD_SIZE (JLS_SUMMARY_LEVEL_COUNT * sizeof(int64_t))

// Chain kinds per (signal, track, level).
#define KIND_MAIN (0)     // data chain at level 0, index chain at level >= 1
#define KIND_SUMMARY (1)  // summary chain at level >= 1
#define KIND_COUNT (2)

struct chain_s {
    int64_t first;                  // first valid chunk offset
    struct jls_core_chunk_s last;   // last valid chunk in file order
};

struct head_s {
    int64_t offset;
    bool payload_ok;
};

struct dead_span_s {
    int64_t start;
    int64_t end;
};

struct repair_s {
    struct jls_core_s * core;
    // [signal][track][level][kind]
    struct chain_s (* tracks)[JLS_TRACK_TYPE_COUNT][JLS_SUMMARY_LEVEL_COUNT][KIND_COUNT];
    struct chain_s source_chain;
    struct chain_s signal_chain;
    struct chain_s user_chain;
    struct head_s heads[JLS_SIGNAL_COUNT][JLS_TRACK_TYPE_COUNT];
    uint8_t signal_type[JLS_SIGNAL_COUNT];   // valid when signal_def_ok
    bool signal_def_ok[JLS_SIGNAL_COUNT];
    struct dead_span_s dead[DEAD_SPAN_MAX];  // ascending, disjoint
    uint32_t dead_count;
    bool dead_overflow;
    int64_t fend;                            // file end after tail truncation
    int64_t relink_count;
    int64_t clamp_count;
    int64_t head_rebuild_count;
    int64_t entry_zero_count;

    jls_msg_fn msg_fn;
    void * msg_user_data;
    jls_progress_fn progress_fn;
    void * progress_user_data;
};

static int32_t msg(struct repair_s * self, const char * fmt, ...) {
    char msg_str[1024];
    if (NULL == self->msg_fn) {
        return 0;
    }
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg_str, sizeof(msg_str), fmt, args);
    va_end(args);
    if (self->msg_fn(self->msg_user_data, msg_str)) {
        return JLS_ERROR_ABORTED;
    }
    return 0;
}

static int32_t progress(struct repair_s * self, double fract) {
    if ((NULL != self->progress_fn)
            && self->progress_fn(self->progress_user_data, fract)) {
        return JLS_ERROR_ABORTED;
    }
    return 0;
}

static void dead_span_add(struct repair_s * self, int64_t start, int64_t end) {
    if (end <= start) {
        return;
    }
    if (self->dead_count
            && (self->dead[self->dead_count - 1].end >= start)) {
        self->dead[self->dead_count - 1].end = end;  // coalesce adjacent damage
        return;
    }
    if (self->dead_count >= DEAD_SPAN_MAX) {
        self->dead_overflow = true;
        return;
    }
    self->dead[self->dead_count].start = start;
    self->dead[self->dead_count].end = end;
    ++self->dead_count;
}

static bool in_dead_span(struct repair_s * self, int64_t offset) {
    uint32_t lo = 0;
    uint32_t hi = self->dead_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (offset < self->dead[mid].start) {
            hi = mid;
        } else if (offset >= self->dead[mid].end) {
            lo = mid + 1;
        } else {
            return true;
        }
    }
    return false;
}

/**
 * @brief Account a valid chunk to its chain, repairing broken links.
 *
 * A chain's chunks appear in file order.  When the previous chunk's
 * item_next does not reference this chunk, the link is broken (its
 * target is dead or foreign): relink it.  A zero item_next is left
 * alone; it marks a deliberate chain end or clamp, and relinking it
 * could resurrect stale chunks orphaned by an earlier repair.
 */
static int32_t chain_update(struct repair_s * self, struct chain_s * chain) {
    struct jls_core_chunk_s * cur = &self->core->chunk_cur;
    if (0 == chain->first) {
        chain->first = cur->offset;
    } else if (chain->last.hdr.item_next
               && (chain->last.hdr.item_next != (uint64_t) cur->offset)) {
        ROE(msg(self, "%" PRIi64 ": relink to %" PRIi64 " (tag %d)",
                chain->last.offset, cur->offset, (int) cur->hdr.tag));
        chain->last.hdr.item_next = (uint64_t) cur->offset;
        ROE(jls_core_update_chunk_header(self->core, &chain->last));
        ++self->relink_count;
    }
    chain->last = *cur;
    return 0;
}

static int32_t chunk_record(struct repair_s * self) {
    struct jls_core_chunk_s * cur = &self->core->chunk_cur;
    uint8_t tag = cur->hdr.tag;
    switch (tag) {
        case JLS_TAG_SOURCE_DEF:
            return chain_update(self, &self->source_chain);
        case JLS_TAG_SIGNAL_DEF: {
            uint16_t sig = cur->hdr.chunk_meta;
            if ((sig < JLS_SIGNAL_COUNT) && (self->core->buf->length >= 3)) {
                self->signal_def_ok[sig] = true;
                self->signal_type[sig] = self->core->buf->start[2];
            }
            return chain_update(self, &self->signal_chain);
        }
        case JLS_TAG_USER_DATA:
            return chain_update(self, &self->user_chain);
        case JLS_TAG_END:
            return 0;
        default:
            break;
    }
    if (0 == (tag & JLS_TRACK_TAG_FLAG)) {
        return 0;  // unknown tag: leave unchained, chains relink around it
    }
    uint16_t sig = cur->hdr.chunk_meta & 0x0fff;
    uint8_t level = (uint8_t) (cur->hdr.chunk_meta >> 12);
    uint8_t track = jls_core_tag_parse_track_type(tag);
    if (sig >= JLS_SIGNAL_COUNT) {
        return 0;
    }
    // FSR tracks keep data, index, and summary in separate chains, so the
    // chain model (relink, clamp, head refresh) applies.  Annotation and
    // UTC tracks weave one mixed chain (data -> index -> summary -> data)
    // that readers walk inline: leave those chains untouched and let the
    // standard open recovery clamp them; record only the first data chunk
    // for torn-head rebuilds.
    bool is_fsr = (JLS_TRACK_TYPE_FSR == track);
    switch (jls_core_tag_parse_track_chunk(tag)) {
        case JLS_TRACK_CHUNK_DEF:
            return chain_update(self, &self->signal_chain);
        case JLS_TRACK_CHUNK_HEAD:
            self->heads[sig][track].offset = cur->offset;
            self->heads[sig][track].payload_ok =
                    (self->core->buf->length == HEAD_PAYLOAD_SIZE);
            return chain_update(self, &self->signal_chain);
        case JLS_TRACK_CHUNK_DATA:
            if (0 == level) {
                if (is_fsr) {
                    return chain_update(self, &self->tracks[sig][track][0][KIND_MAIN]);
                }
                if (0 == self->tracks[sig][track][0][KIND_MAIN].first) {
                    self->tracks[sig][track][0][KIND_MAIN].first = cur->offset;
                }
            }
            return 0;
        case JLS_TRACK_CHUNK_INDEX:
            if (level && is_fsr) {
                return chain_update(self, &self->tracks[sig][track][level][KIND_MAIN]);
            }
            return 0;
        case JLS_TRACK_CHUNK_SUMMARY:
            if (level && is_fsr) {
                return chain_update(self, &self->tracks[sig][track][level][KIND_SUMMARY]);
            }
            return 0;
        default:
            return 0;
    }
}

/// The single forward traversal: validate every chunk and build the tables.
static int32_t repair_traverse(struct repair_s * self) {
    struct jls_core_s * core = self->core;
    struct jls_raw_s * raw = core->raw;
    struct jls_chunk_header_s hdr;
    int64_t offset = sizeof(struct jls_file_header_s);
    int64_t progress_offset = 0;
    ROE(jls_raw_chunk_seek(raw, offset));

    while (offset < self->fend) {
        int32_t rc = jls_raw_rd_header(raw, &hdr);
        if (JLS_ERROR_EMPTY == rc) {
            dead_span_add(self, offset, self->fend);
            break;  // partial chunk at the end
        } else if (rc) {
            // damaged header: resync to the next valid chunk
            if (jls_raw_chunk_seek(raw, offset + 1) || jls_raw_chunk_scan(raw)) {
                dead_span_add(self, offset, self->fend);
                break;  // no further valid chunks
            }
            int64_t next = jls_raw_chunk_tell(raw);
            dead_span_add(self, offset, next);
            offset = next;
            continue;
        }
        rc = jls_core_rd_chunk(core);
        if (JLS_ERROR_NOT_ENOUGH_MEMORY == rc) {
            return rc;
        } else if (rc) {
            // damaged payload, valid header.  A torn track head is kept in
            // its chain; heads_rebuild() rewrites the payload in place.
            struct jls_core_chunk_s * cur = &core->chunk_cur;
            uint16_t sig = cur->hdr.chunk_meta & 0x0fff;
            if ((cur->hdr.tag & JLS_TRACK_TAG_FLAG)
                    && (JLS_TRACK_CHUNK_HEAD == jls_core_tag_parse_track_chunk(cur->hdr.tag))
                    && (sig < JLS_SIGNAL_COUNT)) {
                uint8_t track = jls_core_tag_parse_track_type(cur->hdr.tag);
                self->heads[sig][track].offset = cur->offset;
                self->heads[sig][track].payload_ok = false;
                ROE(chain_update(self, &self->signal_chain));
            } else if (jls_raw_chunk_next(raw)) {
                dead_span_add(self, offset, self->fend);
                break;
            } else {
                dead_span_add(self, offset, jls_raw_chunk_tell(raw));
            }
            if (jls_raw_chunk_seek(raw, offset) || jls_raw_chunk_next(raw)) {
                break;
            }
            offset = jls_raw_chunk_tell(raw);
            continue;
        }
        ROE(chunk_record(self));
        offset = jls_raw_chunk_tell(raw);
        if ((offset - progress_offset) >= PROGRESS_INTERVAL_BYTES) {
            progress_offset = offset;
            ROE(progress(self, offset / (double) self->fend));
        }
    }
    return 0;
}

/// Zero the item_next of every chain whose final link target is dead.
static int32_t chain_clamp(struct repair_s * self, struct chain_s * chain) {
    if (chain->last.offset && chain->last.hdr.item_next) {
        // a valid successor would have been relinked during the traversal
        ROE(msg(self, "%" PRIi64 ": clamp chain end", chain->last.offset));
        chain->last.hdr.item_next = 0;
        ROE(jls_core_update_chunk_header(self->core, &chain->last));
        ++self->clamp_count;
    }
    return 0;
}

static int32_t chains_clamp_all(struct repair_s * self) {
    ROE(chain_clamp(self, &self->source_chain));
    ROE(chain_clamp(self, &self->signal_chain));
    ROE(chain_clamp(self, &self->user_chain));
    for (uint32_t sig = 0; sig < JLS_SIGNAL_COUNT; ++sig) {
        for (uint32_t track = 0; track < JLS_TRACK_TYPE_COUNT; ++track) {
            for (uint32_t level = 0; level < JLS_SUMMARY_LEVEL_COUNT; ++level) {
                for (uint32_t kind = 0; kind < KIND_COUNT; ++kind) {
                    ROE(chain_clamp(self, &self->tracks[sig][track][level][kind]));
                }
            }
        }
    }
    return 0;
}

/// Rebuild torn head payloads and append heads lost entirely.
static int32_t heads_rebuild(struct repair_s * self) {
    struct jls_core_s * core = self->core;
    int64_t head_offsets[JLS_SUMMARY_LEVEL_COUNT];
    for (uint32_t sig = 0; sig < JLS_SIGNAL_COUNT; ++sig) {
        if (!self->signal_def_ok[sig]) {
            continue;
        }
        for (uint32_t track = 0; track < JLS_TRACK_TYPE_COUNT; ++track) {
            if (!jls_core_track_applicable(self->signal_type[sig], (uint8_t) track)) {
                continue;
            }
            struct head_s * head = &self->heads[sig][track];
            struct chain_s (* levels)[KIND_COUNT] = self->tracks[sig][track];
            for (uint32_t level = 0; level < JLS_SUMMARY_LEVEL_COUNT; ++level) {
                head_offsets[level] = levels[level][KIND_MAIN].first;
            }
            if (head->offset && head->payload_ok) {
                if (JLS_TRACK_TYPE_FSR != track) {
                    continue;  // mixed-chain track: the open recovery owns it
                }
                // intact payload: it may still reference dead chunks (e.g.
                // the first data chunk died); the physical scan is authoritative
                ROE(jls_raw_chunk_seek(core->raw, head->offset));
                ROE(jls_core_rd_chunk(core));
                if ((core->buf->length == HEAD_PAYLOAD_SIZE)
                        && (0 == memcmp(core->buf->start, head_offsets, HEAD_PAYLOAD_SIZE))) {
                    continue;  // consistent: no write
                }
                head->payload_ok = false;  // stale: rewrite below
            }
            if (head->offset) {
                // header survived, payload torn: rewrite the payload in place
                ROE(msg(self, "%" PRIi64 ": rebuild head payload, signal %d track %d",
                        head->offset, (int) sig, (int) track));
                ROE(jls_raw_chunk_seek(core->raw, head->offset));
                ROE(jls_raw_wr_payload(core->raw, (uint32_t) HEAD_PAYLOAD_SIZE,
                                       (uint8_t *) head_offsets));
            } else {
                // head chunk lost: append a new one, linked into the signal chain
                struct jls_chunk_header_s hdr;
                memset(&hdr, 0, sizeof(hdr));
                hdr.item_prev = (uint64_t) self->signal_chain.last.offset;
                hdr.tag = jls_track_tag_pack((uint8_t) track, JLS_TRACK_CHUNK_HEAD);
                hdr.chunk_meta = (uint16_t) sig;
                hdr.payload_length = (uint32_t) HEAD_PAYLOAD_SIZE;
                ROE(jls_raw_seek_end(core->raw));
                int64_t offset = jls_raw_chunk_tell(core->raw);
                ROE(msg(self, "%" PRIi64 ": append head, signal %d track %d",
                        offset, (int) sig, (int) track));
                ROE(jls_raw_wr(core->raw, &hdr, (uint8_t *) head_offsets));
                if (self->signal_chain.last.offset) {
                    self->signal_chain.last.hdr.item_next = (uint64_t) offset;
                    ROE(jls_core_update_chunk_header(core, &self->signal_chain.last));
                }
                self->signal_chain.last.offset = offset;
                self->signal_chain.last.hdr = hdr;
            }
            ++self->head_rebuild_count;
        }
    }
    return 0;
}

/// Test if a level-1 index entry references a dead or truncated data chunk.
static bool entry_is_dead(struct repair_s * self, int64_t offset,
                          uint8_t data_tag, uint16_t sig) {
    if ((offset <= 0) || (offset >= self->fend)) {
        return true;
    }
    if (!self->dead_overflow) {
        return in_dead_span(self, offset);
    }
    // too much damage to track spans: validate the target directly
    struct jls_chunk_header_s hdr;
    if (jls_raw_chunk_seek(self->core->raw, offset)
            || jls_raw_rd_header(self->core->raw, &hdr)
            || (hdr.tag != data_tag)
            || ((hdr.chunk_meta & 0x0fff) != sig)) {
        return true;
    }
    return false;
}

/**
 * @brief Convert dead FSR data chunks to omitted regions.
 *
 * Zero level-1 index entries that reference dead data chunks.  Reads
 * then reconstruct those regions from the covering summaries via the
 * fsr_omit mechanism instead of failing.
 */
static int32_t fsr_dead_data_to_omitted(struct repair_s * self) {
    struct jls_core_s * core = self->core;
    for (uint32_t sig = 0; sig < JLS_SIGNAL_COUNT; ++sig) {
        if (!self->signal_def_ok[sig]
                || (JLS_SIGNAL_TYPE_FSR != self->signal_type[sig])) {
            continue;
        }
        uint8_t data_tag = jls_track_tag_pack(JLS_TRACK_TYPE_FSR, JLS_TRACK_CHUNK_DATA);
        uint8_t index_tag = jls_track_tag_pack(JLS_TRACK_TYPE_FSR, JLS_TRACK_CHUNK_INDEX);
        uint16_t meta = (uint16_t) (sig | (1 << 12));
        int64_t offset = self->tracks[sig][JLS_TRACK_TYPE_FSR][1][KIND_MAIN].first;
        while (offset) {
            ROE(jls_raw_chunk_seek(core->raw, offset));
            ROE(jls_core_rd_chunk_validate(core, index_tag, meta));
            struct jls_fsr_index_s * r = (struct jls_fsr_index_s *) core->buf->start;
            if ((sizeof(r->header) + r->header.entry_count * sizeof(r->offsets[0]))
                    > core->buf->length) {
                return JLS_ERROR_MESSAGE_INTEGRITY;
            }
            bool dirty = false;
            for (uint32_t i = 0; i < r->header.entry_count; ++i) {
                if (r->offsets[i]
                        && entry_is_dead(self, (int64_t) r->offsets[i],
                                         data_tag, (uint16_t) sig)) {
                    r->offsets[i] = 0;  // omitted: reads reconstruct from summary
                    dirty = true;
                    ++self->entry_zero_count;
                }
            }
            int64_t next = (int64_t) core->chunk_cur.hdr.item_next;
            if (dirty) {
                ROE(msg(self, "%" PRIi64 ": signal %d dead data converted to omitted",
                        offset, (int) sig));
                ROE(jls_raw_chunk_seek(core->raw, offset));
                ROE(jls_raw_wr_payload(core->raw,
                                       core->chunk_cur.hdr.payload_length,
                                       core->buf->start));
            }
            offset = (next > offset) ? next : 0;  // chains always point forward
        }
    }
    return 0;
}

int32_t jls_repair(const char * path,
                   jls_msg_fn msg_fn, void * msg_user_data,
                   jls_progress_fn progress_fn, void * progress_user_data) {
    int32_t rc;
    struct repair_s * self = calloc(1, sizeof(struct repair_s));
    struct jls_core_s * core = calloc(1, sizeof(struct jls_core_s));
    if ((NULL == self) || (NULL == core)) {
        free(self);
        free(core);
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    self->core = core;
    self->msg_fn = msg_fn;
    self->msg_user_data = msg_user_data;
    self->progress_fn = progress_fn;
    self->progress_user_data = progress_user_data;
    self->tracks = calloc(JLS_SIGNAL_COUNT, sizeof(self->tracks[0]));
    core->buf = jls_buf_alloc();
    if ((NULL == self->tracks) || (NULL == core->buf)) {
        rc = JLS_ERROR_NOT_ENOUGH_MEMORY;
        goto cleanup;
    }

    rc = jls_raw_open(&core->raw, path, "a");
    if (rc && (rc != JLS_ERROR_TRUNCATED)) {
        goto cleanup;
    }
    rc = progress(self, 0.0);
    if (rc) {
        goto close;
    }

    // find the last valid chunk; truncate any torn tail
    if (jls_core_rd_chunk_end(core)) {
        rc = JLS_ERROR_EMPTY;  // no valid chunk found
        goto close;
    }
    if (core->chunk_cur.hdr.tag != JLS_TAG_END) {
        struct jls_core_chunk_s tail;
        int64_t pos = jls_raw_chunk_tell(core->raw);
        rc = jls_core_truncate_tail(core, pos, &tail);
        if (rc) {
            goto close;
        }
    }
    self->fend = jls_raw_backend(core->raw)->fend;

    // single forward traversal, then targeted fixes
    rc = repair_traverse(self);
    if (rc) {
        goto close;
    }
    rc = chains_clamp_all(self);
    if (rc) {
        goto close;
    }
    rc = heads_rebuild(self);
    if (rc) {
        goto close;
    }
    rc = fsr_dead_data_to_omitted(self);
    if (rc) {
        goto close;
    }
    rc = msg(self, "repair: %" PRIi64 " dead spans, %" PRIi64 " relinks, %" PRIi64
             " clamps, %" PRIi64 " heads rebuilt, %" PRIi64 " entries omitted",
             (int64_t) self->dead_count, self->relink_count, self->clamp_count,
             self->head_rebuild_count, self->entry_zero_count);
    if (rc) {
        goto close;
    }

close:
    if (core->raw) {
        jls_raw_close(core->raw);
        core->raw = NULL;
    }
    if (0 == rc) {
        // finalize: tail summaries and END via the standard open recovery
        struct jls_rd_s * rd = NULL;
        rc = jls_rd_open(&rd, path);
        if (0 == rc) {
            jls_rd_close(rd);
            if (progress(self, 1.0)) {
                rc = JLS_ERROR_ABORTED;
            }
        } else {
            // the repair already failed with rc: a cancel request here is moot
            (void) msg(self, "repair could not restore the file: %d %s",
                       rc, jls_error_code_name(rc));
        }
    }

cleanup:
    jls_buf_free(core->buf);
    free(self->tracks);
    free(core);
    free(self);
    return rc;
}
