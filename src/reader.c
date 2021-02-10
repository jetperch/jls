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
#include <stdbool.h>

#define PAYLOAD_BUFFER_SIZE_DEFAULT (1 << 25)   // 32 MB
#define STRING_BUFFER_SIZE_DEFAULT (1 << 20)    // 1 MB
#define SIGNAL_MASK  (0x0fff)


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
    struct source_s * sources[JLS_SOURCE_COUNT];
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

struct signal_s {
    int64_t track_defs[4];
    int64_t track_heads[4];
};

struct jls_rd_s {
    struct jls_raw_s * raw;
    struct jls_source_def_s source_def[JLS_SOURCE_COUNT];
    struct jls_source_def_s source_def_api[JLS_SOURCE_COUNT];
    struct jls_signal_def_s signal_def[JLS_SIGNAL_COUNT];
    struct jls_signal_def_s signal_def_api[JLS_SOURCE_COUNT];

    struct signal_s signals[JLS_SIGNAL_COUNT];

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

#if 0

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
            if (*self->payload.cur == 0x1f) {
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
        uint16_t source_id = self->hdr.chunk_meta;
        if (source_id >= JLS_SOURCE_COUNT) {
            JLS_LOGW("source_id %d too big - skip", (int) source_id);
        } else {
            struct jls_source_def_s *src = &self->source_def[source_id];
            ROE(payload_skip(self, 64));
            ROE(payload_parse_str(self, (char **) &src->name));
            ROE(payload_parse_str(self, (char **) &src->vendor));
            ROE(payload_parse_str(self, (char **) &src->model));
            ROE(payload_parse_str(self, (char **) &src->version));
            ROE(payload_parse_str(self, (char **) &src->serial_number));
            src->source_id = source_id;  // indicate that this source is valid!
            JLS_LOGI("Found source %d : %s", (int) source_id, src->name);
        }
        if (!self->hdr.item_next) {
            break;
        }
        ROE(jls_raw_chunk_seek(self->raw, self->hdr.item_next));
    }
    return 0;
}

static int32_t signal_validate(struct jls_rd_s * self, uint16_t signal_id, struct jls_signal_def_s * s) {
    if (signal_id >= JLS_SIGNAL_COUNT) {
        JLS_LOGW("signal_id %d too big - skip", (int) signal_id);
        return JLS_ERROR_PARAMETER_INVALID;
    }
    if (self->source_def[s->source_id].source_id != s->source_id) {
        JLS_LOGW("signal %d: source_id %d not found", (int) signal_id, (int) s->source_id);
        return JLS_ERROR_PARAMETER_INVALID;
    }
    switch (s->signal_type) {
        case JLS_SIGNAL_TYPE_FSR: break;
        case JLS_SIGNAL_TYPE_VSR: break;
        default:
            JLS_LOGW("signal %d: invalid signal_type: %d", (int) signal_id, (int) s->signal_type);
            return JLS_ERROR_PARAMETER_INVALID;
    }

    // todo check data_type
    return 0;
}

static int32_t handle_signal_def(struct jls_rd_s * self) {
    uint16_t signal_id = self->hdr.chunk_meta;
    if (signal_id >= JLS_SIGNAL_COUNT) {
        JLS_LOGW("signal_id %d too big - skip", (int) signal_id);
        return JLS_ERROR_PARAMETER_INVALID;
    }
    struct jls_signal_def_s *s = &self->signal_def[signal_id];
    s->signal_id = signal_id;
    ROE(payload_parse_u16(self, &s->source_id));
    ROE(payload_parse_u8(self, &s->signal_type));
    ROE(payload_skip(self, 1));
    ROE(payload_parse_u32(self, &s->data_type));
    ROE(payload_parse_u32(self, &s->sample_rate));
    ROE(payload_parse_u32(self, &s->samples_per_block));
    ROE(payload_parse_u32(self, &s->summary_downsample));
    ROE(payload_parse_u32(self, &s->utc_rate_auto));
    ROE(payload_skip(self, 4 + 64));
    ROE(payload_parse_str(self, (char **) &s->name));
    ROE(payload_parse_str(self, (char **) &s->si_units));
    if (0 == signal_validate(self, signal_id, s)) {  // validate passed
        s->signal_id = signal_id;  // indicate that this signal is valid
        JLS_LOGI("Found signal %d : %s", (int) signal_id, s->name);
    }  // else skip
    return 0;
}

static bool is_signal_defined(struct jls_rd_s * self, uint16_t signal_id) {
    signal_id &= SIGNAL_MASK;  // mask off chunk_meta depth
    if (signal_id >= JLS_SIGNAL_COUNT) {
        JLS_LOGW("signal_id %d too big", (int) signal_id);
        return false;
    }
    if (self->signal_def[signal_id].signal_id != signal_id) {
        JLS_LOGW("signal_id %d not defined", (int) signal_id);
        return false;
    }
    return true;
}

static inline uint8_t tag_parse_track_type(uint8_t tag) {
    return (tag >> 3) & 3;
}

static int32_t validate_track_tag(struct jls_rd_s * self, uint16_t signal_id, uint8_t tag) {
    if (!is_signal_defined(self, signal_id)) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    struct jls_signal_def_s * signal_def = &self->signal_def[signal_id];
    uint8_t track_type = tag_parse_track_type(tag);
    switch (signal_def->signal_type) {
        case JLS_SIGNAL_TYPE_FSR:
            if ((track_type == JLS_TRACK_TYPE_FSR)
                || (track_type == JLS_TRACK_TYPE_ANNOTATION)
                || (track_type == JLS_TRACK_TYPE_UTC)) {
                // good
            } else {
                JLS_LOGW("unsupported track %d for FSR signal", (int) track_type);
                return JLS_ERROR_PARAMETER_INVALID;
            }
            break;
        case JLS_SIGNAL_TYPE_VSR:
            if ((track_type == JLS_TRACK_TYPE_VSR)
                || (track_type == JLS_TRACK_TYPE_ANNOTATION)) {
                // good
            } else {
                JLS_LOGW("unsupported track %d for VSR signal", (int) track_type);
                return JLS_ERROR_PARAMETER_INVALID;
            }
            break;
        default:
            // should have already been checked.
            JLS_LOGW("unsupported signal type: %d", (int) signal_def->signal_type);
            return JLS_ERROR_PARAMETER_INVALID;
    }
    return 0;
}

static int32_t handle_track_def(struct jls_rd_s * self, int64_t pos) {
    uint16_t signal_id = self->hdr.chunk_meta & SIGNAL_MASK;
    ROE(validate_track_tag(self, signal_id, self->hdr.tag));
    self->signals[signal_id].track_defs[tag_parse_track_type(self->hdr.tag)] = pos;
    return 0;
}

static int32_t handle_track_head(struct jls_rd_s * self, int64_t pos) {
    uint16_t signal_id = self->hdr.chunk_meta & SIGNAL_MASK;
    ROE(validate_track_tag(self, signal_id, self->hdr.tag));
    self->signals[signal_id].track_heads[tag_parse_track_type(self->hdr.tag)] = pos;
    return 0;
}

static int32_t scan_signals(struct jls_rd_s * self) {
    int64_t pos;
    JLS_LOGI("scan_signals");
    ROE(jls_raw_chunk_seek(self->raw, self->signal_head.offset));
    while (1) {
        pos = jls_raw_chunk_tell(self->raw);
        ROE(rd(self));
        if (self->hdr.tag == JLS_TAG_SIGNAL_DEF) {
            handle_signal_def(self);
        } else if ((self->hdr.tag & 7) == JLS_TRACK_CHUNK_DEF) {
            handle_track_def(self, pos);
        } else if ((self->hdr.tag & 7) == JLS_TRACK_CHUNK_HEAD) {
            handle_track_head(self, pos);
        } else {
            JLS_LOGW("unknown tag %d in signal list", (int) self->hdr.tag);
        }
        if (!self->hdr.item_next) {
            break;
        }
        ROE(jls_raw_chunk_seek(self->raw, self->hdr.item_next));
    }
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

int32_t jls_rd_sources(struct jls_rd_s * self, struct jls_source_def_s ** sources, uint16_t * count) {
    if (!self || !sources || !count) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    uint16_t c = 0;
    for (uint16_t i = 0; i < JLS_SOURCE_COUNT; ++i) {
        if (self->source_def[i].source_id == i) {
            // Note: source 0 is always defined, so calloc is ok
            self->source_def_api[c++] = self->source_def[i];
        }
    }
    *sources = self->source_def_api;
    *count = c;
    return 0;
}

int32_t jls_rd_signals(struct jls_rd_s * self, struct jls_signal_def_s ** signals, uint16_t * count) {
    if (!self || !signals || !count) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    uint16_t c = 0;
    for (uint16_t i = 0; i < JLS_SIGNAL_COUNT; ++i) {
        if (self->signal_def[i].signal_id == i) {
            // Note: signal 0 is always defined, so calloc is ok
            self->signal_def_api[c++] = self->signal_def[i];
        }
    }
    *signals = self->signal_def_api;
    *count = c;
    return 0;
}
