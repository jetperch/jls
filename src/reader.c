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

#include "jls/reader.h"
#include "jls/raw.h"
#include "jls/format.h"
#include "jls/ec.h"
#include "jls/log.h"
#include "crc32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAYLOAD_BUFFER_SIZE_DEFAULT (1 << 25)   // 32 MB
#define STRING_BUFFER_SIZE_DEFAULT (1 << 20)    // 1 MB


#define ROE(x)  do {                        \
    int32_t rc__ = (x);                     \
    if (rc__) {                             \
        return rc__;                        \
    }                                       \
} while (0)

#define RLE(x)  do {                        \
    int32_t rc__ = (x);                     \
    if (rc__) {                             \
        JLS_LOGE("error %d: " #x, rc__);    \
        return rc__;                        \
    }                                       \
} while (0)

#if 0
struct source_s {
    struct jls_source_def_s def;
};

struct signal_s {
    struct jls_signal_def_s def;
};

struct jls_rd_s {
    struct source_s * sources[256];
    struct signal_s * signals[JLS_SIGNAL_COUNT];
    struct jls_chunk_header_s hdr;  // the header for the current chunk
    uint8_t * chunk_buffer;         // storage for the
    size_t chunk_buffer_sz;
    FILE * f;
};
#endif


struct chunk_s {
    struct jls_chunk_header_s hdr;
    int64_t offset;
};

struct payload_s {
    uint8_t * start;
    uint8_t * cur;
    uint8_t * end;
    size_t alloc_size;
};

struct strings_s {
    char * start;
    char * cur;
    char * end;
    size_t alloc_size;
};

struct jls_rd_s {
    struct jls_raw_s * raw;
    struct jls_source_def_s sources[256];

    struct jls_chunk_header_s hdr;
    struct payload_s payload;
    struct strings_s strings;

    struct chunk_s source_head;  // source_def
    struct chunk_s signal_head;  // signal_det, track_*_def, track_*_head
    struct chunk_s user_data_head;  // user_data, ignore first
};

static int32_t payload_skip(struct jls_rd_s * self, size_t count) {
    if ((self->payload.cur + count) > self->payload.end) {
        return JLS_ERROR_EMPTY;
    }
    self->payload.cur += count;
    return 0;
}

#if 0

static int32_t payload_parse_u8(struct jls_rd_s * self, uint8_t * value) {
    if ((self->payload.cur + 1) > self->payload.end) {
        return JLS_ERROR_EMPTY;
    }
    *value = self->payload.cur[0];
    self->payload.cur += 1;
    return 0;
}

static int32_t payload_parse_u16(struct jls_rd_s * self, uint16_t * value) {
    if ((self->payload.cur + 2) > self->payload.end) {
        return JLS_ERROR_EMPTY;
    }
    *value = ((uint16_t) self->payload.cur[0])
            | (((uint16_t) self->payload.cur[1]) << 8);
    self->payload.cur += 2;
    return 0;
}

static int32_t payload_parse_u32(struct jls_rd_s * self, uint32_t * value) {
    if ((self->payload.cur + 4) > self->payload.end) {
        return JLS_ERROR_EMPTY;
    }
    *value = ((uint32_t) self->payload.cur[0])
             | (((uint32_t) self->payload.cur[1]) << 8)
             | (((uint32_t) self->payload.cur[2]) << 16)
             | (((uint32_t) self->payload.cur[3]) << 24);
    self->payload.cur += 4;
    return 0;
}

static int32_t payload_parse_u64(struct jls_rd_s * self, uint64_t * value) {
    if ((self->payload.cur + 8) > self->payload.end) {
        return JLS_ERROR_EMPTY;
    }
    *value = ((uint64_t) self->payload.cur[0])
             | (((uint64_t) self->payload.cur[1]) << 8)
             | (((uint64_t) self->payload.cur[2]) << 16)
             | (((uint64_t) self->payload.cur[3]) << 24)
             | (((uint64_t) self->payload.cur[4]) << 32)
             | (((uint64_t) self->payload.cur[5]) << 40)
             | (((uint64_t) self->payload.cur[6]) << 48)
             | (((uint64_t) self->payload.cur[7]) << 56);
    self->payload.cur += 8;
    return 0;
}

#endif

static int32_t payload_parse_str(struct jls_rd_s * self, char ** value) {
    char * str = self->strings.cur;
    char ch;
    while (self->payload.cur != self->payload.end) {
        if (self->strings.cur >= self->strings.end) {
            size_t offset = self->strings.cur - self->strings.start;
            size_t sz = self->strings.alloc_size * 2;
            char * ptr = realloc(self->strings.start, sz);
            if (!ptr) {
                return JLS_ERROR_NOT_ENOUGH_MEMORY;
            }
            str = (str - self->strings.start) + ptr;
            self->strings.start = ptr;
            self->strings.cur = ptr + offset;
            self->strings.end = ptr + sz;
            self->strings.alloc_size = sz;
        }

        ch = (char) *self->payload.cur++;
        *self->strings.cur++ = ch;
        if (ch == 0) {
            if (self->payload.cur[1] == 0x1f) {
                self->payload.cur++;
            }
            *value = str;
            return 0;
        }
    }

    *value = NULL;
    return JLS_ERROR_EMPTY;
}

static int32_t rd(struct jls_rd_s * self) {
    while (1) {
        int32_t rc = jls_raw_rd(self->raw, &self->hdr, self->payload.alloc_size, self->payload.start);
        if (rc == JLS_ERROR_TOO_BIG) {
            size_t sz_new = self->payload.alloc_size;
            while (sz_new < self->hdr.payload_length) {
                sz_new *= 2;
            }
            uint8_t *ptr = realloc(self->payload.start, sz_new);
            if (!ptr) {
                return JLS_ERROR_NOT_ENOUGH_MEMORY;
            }
            self->payload.start = ptr;
            self->payload.alloc_size = sz_new;
        } else if (rc == 0) {
            self->payload.end = self->payload.start + self->hdr.payload_length;
            self->payload.cur = self->payload.start;
            return 0;
        } else {
            return rc;
        }
    }
}

static int32_t scan_sources(struct jls_rd_s * self) {
    JLS_LOGI("scan_sources");
    ROE(jls_raw_chunk_seek(self->raw, self->source_head.offset));
    while (1) {
        ROE(rd(self));
        uint8_t source_id = (uint8_t) (self->hdr.chunk_meta & 0x00ff);
        struct jls_source_def_s * src = &self->sources[source_id];
        ROE(payload_skip(self, 64));
        ROE(payload_parse_str(self, (char **) &src->name));
        ROE(payload_parse_str(self, (char **)&src->vendor));
        ROE(payload_parse_str(self, (char **)&src->model));
        ROE(payload_parse_str(self, (char **)&src->version));
        ROE(payload_parse_str(self, (char **)&src->serial_number));
        JLS_LOGI("Found source %d : %s", (int) source_id, src->name);
        if (!self->hdr.item_next) {
            break;
        }
        ROE(jls_raw_chunk_seek(self->raw, self->hdr.item_next));
    }
    return 0;
}

static int32_t scan_signals(struct jls_rd_s * self) {
    (void) self;
    JLS_LOGI("find signals");
    return 0;
}

static int32_t scan(struct jls_rd_s * self) {
    int32_t rc = 0;
    uint8_t found = 0;

    for (int i = 0; found != 7; ++i) {
        if (i == 3) {
            JLS_LOGW("malformed JLS, continue searching");
        }
        int64_t pos = jls_raw_chunk_tell(self->raw);
        rc = rd(self);
        if (rc == JLS_ERROR_EMPTY) {
            return 0;
        } else if (rc) {
            return rc;
        }

        JLS_LOGI("tag %d : %s", self->hdr.tag, jls_tag_to_name(self->hdr.tag));

        switch (self->hdr.tag) {
            case JLS_TAG_USER_DATA:
                found |= 1;
                if (!self->user_data_head.offset) {
                    self->user_data_head.offset = pos;
                    self->user_data_head.hdr = self->hdr;
                }
                break;
            case JLS_TAG_SOURCE_DEF:
                found |= 2;
                if (!self->source_head.offset) {
                    self->source_head.offset = pos;
                    self->source_head.hdr = self->hdr;
                }
                break;
            case JLS_TAG_SIGNAL_DEF:
                found |= 4;
                if (!self->signal_head.offset) {
                    self->signal_head.offset = pos;
                    self->signal_head.hdr = self->hdr;
                }
                break;
            default:
                break;  // skip
        }
    }
    JLS_LOGI("found initial tags");
    ROE(scan_sources(self));
    ROE(scan_signals(self));
    return 0;
}

int32_t jls_rd_open(struct jls_rd_s ** instance, const char * path) {
    if (!instance) {
        return JLS_ERROR_PARAMETER_INVALID;
    }

    struct jls_rd_s *self = calloc(1, sizeof(struct jls_rd_s));
    if (!self) {
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }

    self->payload.start = malloc(PAYLOAD_BUFFER_SIZE_DEFAULT);
    self->strings.start = malloc(STRING_BUFFER_SIZE_DEFAULT);
    if (!self->payload.start || !self->strings.start) {
        jls_rd_close(self);
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    self->payload.alloc_size = PAYLOAD_BUFFER_SIZE_DEFAULT;
    self->payload.cur = self->payload.start;
    self->payload.end = self->payload.start;
    self->strings.alloc_size = STRING_BUFFER_SIZE_DEFAULT;
    self->strings.cur = self->strings.start;
    self->strings.end = self->strings.start + STRING_BUFFER_SIZE_DEFAULT;

    int32_t rc = jls_raw_open(&self->raw, path, "r");
    if (rc) {
        jls_rd_close(self);
        return rc;
    }

    ROE(scan(self));
    *instance = self;
    return rc;
}

void jls_rd_close(struct jls_rd_s * self) {
    if (self) {
        jls_raw_close(self->raw);
        if (self->strings.start) {
            free(self->strings.start);
            self->strings.start = NULL;
        }
        if (self->payload.start) {
            free(self->payload.start);
            self->payload.start = NULL;
        }
        self->raw = NULL;
        free(self);
    }
}
