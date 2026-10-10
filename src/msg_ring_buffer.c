/*
 * Copyright 2020-2022 Jetperch LLC
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

#include "jls/msg_ring_buffer.h"
#include "jls/log.h"
#include <string.h>

/*
 * The message storage format is:
 *    sz[7:0], sz[15:8], sz[23:16], sz[31:24], msg[0...N]
 * The message size, sz, must be less than 0x80000000.
 * Any message with the bit[31] set is considered a control header,
 * which indicates wrap_around.
 *
 * Invariants: head and tail never exceed buf_size - 4, so a wrap marker
 * always fits at head.  head == tail means empty, so a message never
 * fills the last free byte before tail.
 */

#define HDR_SZ (4U)
#define WRAP_MARKER (0xffffffffU)

void jls_mrb_init(struct jls_mrb_s * self, uint8_t * buffer, uint32_t buffer_size) {
    self->buf = buffer;
    self->buf_size = buffer_size;
    jls_mrb_clear(self);
}

void jls_mrb_clear(struct jls_mrb_s * self) {
    self->head = 0;
    self->tail = 0;
    self->count = 0;
    memset(self->buf, 0, self->buf_size);
}

uint32_t jls_mrb_used_bytes(struct jls_mrb_s * self) {
    if (self->head >= self->tail) {
        return self->head - self->tail;
    } else {
        return (self->head + self->buf_size) - self->tail;
    }
}

uint32_t jls_mrb_msg_size_max(struct jls_mrb_s * self) {
    // header + message + room for the next wrap marker + 1 byte gap
    uint32_t overhead = HDR_SZ + HDR_SZ + 1;
    return (self->buf_size > overhead) ? (self->buf_size - overhead) : 0;
}

static inline uint8_t * add_sz(uint8_t * p, uint32_t sz) {
    p[0] = sz & 0xff;
    p[1] = (sz >> 8) & 0xff;
    p[2] = (sz >> 16) & 0xff;
    p[3] = (sz >> 24) & 0xff;
    return (p + 4);
}

uint8_t * jls_mrb_alloc(struct jls_mrb_s * self, uint32_t size) {
    uint32_t head = self->head;
    uint32_t tail = self->tail;
    uint64_t total = (uint64_t) HDR_SZ + size;  // 64-bit: never wraps
    uint64_t head_max = (uint64_t) self->buf_size - HDR_SZ;  // keep room for a wrap marker

    if ((self->buf_size <= (2 * HDR_SZ)) || (size > jls_mrb_msg_size_max(self))) {
        JLS_LOGE("jls_mrb_alloc too big");
        return NULL;
    }

    if (head == tail) {
        // Empty: restart at 0 for the most contiguous space.
        head = 0;
        self->head = 0;
        self->tail = 0;
        tail = 0;
    }

    uint32_t pos;
    if (head >= tail) {
        if ((head + total) <= head_max) {
            pos = head;  // fits as is, no wrap
        } else if (total < tail) {
            add_sz(self->buf + head, WRAP_MARKER);  // fits after wrap
            pos = 0;
        } else {
            return NULL;  // does not fit
        }
    } else if ((head + total) < tail) {
        pos = head;  // fits as is
    } else {
        return NULL;  // does not fit.
    }
    uint8_t * p = add_sz(self->buf + pos, size);
    self->head = (uint32_t) (pos + total);
    ++self->count;
    return p;
}

static inline uint32_t get_sz(uint8_t const * p) {
    return ((uint32_t) p[0])
            | (((uint32_t) p[1]) << 8)
            | (((uint32_t) p[2]) << 16)
            | (((uint32_t) p[3]) << 24);
}

uint8_t * jls_mrb_peek(struct jls_mrb_s * self, uint32_t * size) {
    uint32_t head = self->head;
    uint32_t sz;
    *size = 0;

    if (self->tail == head) {
        return NULL;
    }
    sz = get_sz(self->buf + self->tail);
    if (sz >= 0x80000000) {
        // rollover
        if (head > self->tail) {
            JLS_LOGE("buffer overflow"); // should never be possible
            jls_mrb_clear(self);
            return NULL;
        }
        self->tail = 0;
        if (self->tail == head) {
            return NULL;
        }
        sz = get_sz(self->buf);
    }
    if (((uint64_t) self->tail + HDR_SZ + sz) > self->buf_size) {
        JLS_LOGE("buffer corrupt");  // should never be possible
        jls_mrb_clear(self);
        return NULL;
    }
    *size = sz;
    return (self->buf + self->tail + HDR_SZ);
}

uint8_t * jls_mrb_pop(struct jls_mrb_s * self, uint32_t * size) {
    uint8_t *p = jls_mrb_peek(self, size);
    if (p) {
        self->tail += HDR_SZ + *size;  // never past buf_size - 4, see alloc
        if (self->count) {
            --self->count;
        }
    }
    return p;
}
