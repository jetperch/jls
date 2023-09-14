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

#include "jls/buffer.h"
#include "jls/cdef.h"
#include "jls/ec.h"
#include "jls/log.h"
#include <stdlib.h>
#include <string.h>


static inline int32_t wr_end(struct jls_buf_s * self) {
    if (self->cur > self->end) {
        self->end = self->cur;
    }
    return 0;
}

static int32_t strings_alloc(struct jls_buf_s * self) {
    struct jls_buf_strings_s * s = calloc(1, sizeof(struct jls_buf_strings_s));
    if (NULL == s) {
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    s->cur = s->buffer;
    s->next = NULL;
    if (NULL == self->strings_head) {
        self->strings_head = s;
    } else if (NULL != self->strings_tail) {
        self->strings_tail->next = s;
    }
    self->strings_tail = s;
    return 0;
}

struct jls_buf_s * jls_buf_alloc(void) {
    struct jls_buf_s * s = calloc(1, sizeof(struct jls_buf_s));
    if (NULL == s) {
        JLS_LOGE("jls_buf_alloc out of memory on jls_buf_s");
        return NULL;
    }
    s->start = calloc(1, JLS_BUF_DEFAULT_SIZE);
    if (NULL == s->start) {
        JLS_LOGE("jls_buf_alloc out of memory on buffer");
        free(s);
        return NULL;
    }
    s->cur = s->start;
    s->end = s->start;
    s->length = 0;
    s->alloc_size = JLS_BUF_DEFAULT_SIZE;
    s->strings_head = NULL;
    s->strings_tail = NULL;
    return s;
}

void jls_buf_free(struct jls_buf_s * self) {
    if (NULL == self) {
        return;
    }
    while (NULL != self->strings_head) {
        struct jls_buf_strings_s * next = self->strings_head->next;
        self->strings_head->next = NULL;
        free(self->strings_head);
        self->strings_head = next;
    }
    if (NULL != self->start) {
        free(self->start);
    }
    free(self);
}

int32_t jls_buf_realloc(struct jls_buf_s * self, size_t size) {
    if (size <= self->alloc_size) {
        return 0;
    }

    size_t alloc_size = self->alloc_size;
    while (alloc_size < size) {
        alloc_size *= alloc_size;
    }

    uint8_t * ptr = realloc(self->start, alloc_size);
    if (NULL == ptr) {
        JLS_LOGE("jls_buf_realloc out of memory");
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    self->start = ptr;
    self->alloc_size = alloc_size;
    return 0;
}

void jls_buf_reset(struct jls_buf_s * self) {
    self->cur = self->start;
    self->end = self->start;
    self->length = 0;
}

size_t jls_buf_length(struct jls_buf_s * self) {
    return self->length;
}

int32_t jls_buf_copy(struct jls_buf_s * self, const struct jls_buf_s * src) {
    ROE(jls_buf_realloc(self, src->length));
    memcpy(self->start, src->start, src->length);
    self->cur = 0;
    self->length = src->length;
    self->end = self->cur + self->length;
    return 0;
}

int32_t jls_buf_string_save(struct jls_buf_s * self, const char * cstr_in, char ** cstr_save) {
    if (NULL == self->strings_tail) {
        ROE(strings_alloc(self));
    }
    size_t sz = strlen(cstr_in) + 1;
    struct jls_buf_strings_s * s = self->strings_tail;
    char * buf_end = s->buffer + sizeof(s->buffer) - 1;
    if ((size_t) (buf_end - s->cur) < sz) {
        ROE(strings_alloc(self));
        s = self->strings_tail;
    }
    memcpy(s->cur, cstr_in, sz);
    if (NULL != cstr_save) {
        *cstr_save = s->cur;
    }
    s->cur += sz;
    return 0;
}

int32_t jls_buf_wr_zero(struct jls_buf_s * self, uint32_t count) {
    ROE(jls_buf_realloc(self, self->length + count));
    for (uint32_t i = 0; i < count; ++i) {
        *self->cur++ = 0;
    }
    self->length += count;
    return wr_end(self);
}

int32_t jls_buf_wr_str(struct jls_buf_s * self, const char * cstr) {
    // Strings end with {0, 0x1f} = {null, unit separator}
    size_t count = 2;
    size_t slen = 0;
    if (cstr) {
        slen = strlen(cstr);
        count += slen;
    }
    ROE(jls_buf_realloc(self, self->length + count));
    if (slen) {
        memcpy(self->cur, cstr, slen);
    }
    self->cur += slen;
    *self->cur++ = 0;
    *self->cur++ = 0x1f;
    self->length += count;
    return wr_end(self);
}

int32_t jls_buf_wr_bin(struct jls_buf_s * self, const void * data, uint32_t data_size) {
    ROE(jls_buf_realloc(self, self->length + data_size));
    memcpy(self->cur, data, data_size);
    self->cur += data_size;
    self->length += data_size;
    return wr_end(self);
}

int32_t jls_buf_wr_u8(struct jls_buf_s * self, uint8_t value) {
    ROE(jls_buf_realloc(self, self->length + sizeof(value)));
    *self->cur++ = value;
    self->length += sizeof(value);
    return wr_end(self);
}

int32_t jls_buf_wr_u16(struct jls_buf_s * self, uint16_t value) {
    ROE(jls_buf_realloc(self, self->length + sizeof(value)));
    *self->cur++ = (uint8_t) (value & 0xff);
    *self->cur++ = (uint8_t) ((value >> 8) & 0xff);
    self->length += sizeof(value);
    return wr_end(self);
}

int32_t jls_buf_wr_u32(struct jls_buf_s * self, uint32_t value) {
    ROE(jls_buf_realloc(self, self->length + sizeof(value)));
    *self->cur++ = (uint8_t) (value & 0xff);
    *self->cur++ = (uint8_t) ((value >> 8) & 0xff);
    *self->cur++ = (uint8_t) ((value >> 16) & 0xff);
    *self->cur++ = (uint8_t) ((value >> 24) & 0xff);
    self->length += sizeof(value);
    return wr_end(self);
}

int32_t jls_buf_wr_f32(struct jls_buf_s * self, float value) {
    uint8_t * p = (uint8_t *) &value;
    ROE(jls_buf_realloc(self, self->length + sizeof(value)));
    *self->cur++ = *p++;
    *self->cur++ = *p++;
    *self->cur++ = *p++;
    *self->cur++ = *p++;
    self->length += sizeof(value);
    return wr_end(self);
}

int32_t jls_buf_wr_i64(struct jls_buf_s * self, int64_t value) {
    ROE(jls_buf_realloc(self, self->length + sizeof(value)));
    *self->cur++ = (uint8_t) (value & 0xff);
    *self->cur++ = (uint8_t) ((value >> 8) & 0xff);
    *self->cur++ = (uint8_t) ((value >> 16) & 0xff);
    *self->cur++ = (uint8_t) ((value >> 24) & 0xff);
    *self->cur++ = (uint8_t) ((value >> 32) & 0xff);
    *self->cur++ = (uint8_t) ((value >> 40) & 0xff);
    *self->cur++ = (uint8_t) ((value >> 48) & 0xff);
    *self->cur++ = (uint8_t) ((value >> 56) & 0xff);
    self->length += sizeof(value);
    return wr_end(self);
}

int32_t jls_buf_rd_skip(struct jls_buf_s * self, size_t count) {
    if ((self->cur + count) > self->end) {
        return JLS_ERROR_EMPTY;
    }
    self->cur += count;
    return 0;
}

int32_t jls_buf_rd_u8(struct jls_buf_s * self, uint8_t * value) {
    if ((self->cur + sizeof(*value)) > self->end) {
        return JLS_ERROR_EMPTY;
    }
    *value = self->cur[0];
    self->cur += sizeof(*value);
    return 0;
}

int32_t jls_buf_rd_u16(struct jls_buf_s * self, uint16_t * value) {
    if ((self->cur + sizeof(*value)) > self->end) {
        return JLS_ERROR_EMPTY;
    }
    *value = ((uint16_t) self->cur[0])
             | (((uint16_t) self->cur[1]) << 8);
    self->cur += sizeof(*value);
    return 0;
}

int32_t jls_buf_rd_u32(struct jls_buf_s * self, uint32_t * value) {
    if ((self->cur + sizeof(*value)) > self->end) {
        return JLS_ERROR_EMPTY;
    }
    *value = ((uint32_t) self->cur[0])
             | (((uint32_t) self->cur[1]) << 8)
             | (((uint32_t) self->cur[2]) << 16)
             | (((uint32_t) self->cur[3]) << 24);
    self->cur += sizeof(*value);
    return 0;
}

int32_t jls_buf_rd_str(struct jls_buf_s * self, const char ** value) {
    struct jls_buf_strings_s * s;
    if (NULL == self->strings_tail) {
        ROE(strings_alloc(self));
    }
    s = self->strings_tail;
    char * str = s->cur;
    char * buf_end = s->buffer + sizeof(s->buffer) - 1;
    char ch;
    while (self->cur != self->end) {
        if (s->cur >= buf_end) {
            ROE(strings_alloc(self));
            // copy over partial.
            while (str <= buf_end) {
                *self->strings_tail->cur++ = *str++;
            }
            s = self->strings_tail;
            str = self->strings_tail->buffer;
        }

        ch = (char) *self->cur++;
        *s->cur++ = ch;
        // Strings end with {0, 0x1f} = {null, unit separator}
        if (ch == 0) {
            if (*self->cur == 0x1f) {
                self->cur++;
            }
            *value = str;
            return 0;
        }
    }

    *value = NULL;
    return JLS_ERROR_EMPTY;
}
