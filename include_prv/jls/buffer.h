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

/**
 * @file
 *
 * @brief Buffer implementation for JLS files.
 */

#ifndef JLS_BUFFER_H__
#define JLS_BUFFER_H__

#include "jls/cmacro.h"
#include <stdint.h>
#include <stddef.h>

/**
 * @ingroup jls
 * @defgroup jls_buf JLS buffer.
 *
 * @brief JLS buffer.
 *
 * This module provides JLS payload construction and parsing.
 *
 * @{
 */

JLS_CPP_GUARD_START

#define JLS_BUF_DEFAULT_SIZE (1 << 20)   // 1 MiB
#define JLS_BUF_STRING_SIZE (1 << 20)    // 1 MiB

/**
 * @brief Efficient storage of constant strings.
 */
struct jls_buf_strings_s {
    struct jls_buf_strings_s * next;     ///< NULL or next string buffer
    char * cur;                          ///< Pointer to place start of next string
    char buffer[JLS_BUF_STRING_SIZE];    ///< Buffer for 0-terminated strings.
};

struct jls_buf_s {
    uint8_t * start;
    uint8_t * cur;
    uint8_t * end;      // current end
    size_t length;      // current length
    size_t alloc_size;  // end - start
    struct jls_buf_strings_s * strings_head;
    struct jls_buf_strings_s * strings_tail;
};

struct jls_buf_s * jls_buf_alloc(void);

void jls_buf_free(struct jls_buf_s * self);

int32_t jls_buf_realloc(struct jls_buf_s * self, size_t size);

void jls_buf_reset(struct jls_buf_s * self);

size_t jls_buf_length(struct jls_buf_s * self);

/**
 * @brief Copy the buffer binary contents.
 *
 * @param self The buffer instance to modify.
 * @param src The buffer instance to copy.
 * @return 0 or error code.
 *
 * This function does NOT modify the string buffers for self,
 * only the binary storage.
 */
int32_t jls_buf_copy(struct jls_buf_s * self, const struct jls_buf_s * src);

/**
 * @brief Persist a copy of the string.
 *
 * @param self The buffer instance.
 * @param cstr_in The string to save.
 * @param cstr_save The saved string, which remains valid until jls_buf_free(self).
 * @return 0 or error code.
 */
int32_t jls_buf_string_save(struct jls_buf_s * self, const char * cstr_in, char ** cstr_save);

int32_t jls_buf_wr_zero(struct jls_buf_s * self, uint32_t count);
int32_t jls_buf_wr_str(struct jls_buf_s * self, const char * cstr);
int32_t jls_buf_wr_bin(struct jls_buf_s * self, const void * data, uint32_t data_size);
int32_t jls_buf_wr_u8(struct jls_buf_s * self, uint8_t value);
int32_t jls_buf_wr_u16(struct jls_buf_s * self, uint16_t value);
int32_t jls_buf_wr_u32(struct jls_buf_s * self, uint32_t value);
int32_t jls_buf_wr_f32(struct jls_buf_s * self, float value);
int32_t jls_buf_wr_i64(struct jls_buf_s * self, int64_t value);


int32_t jls_buf_rd_skip(struct jls_buf_s * self, size_t count);
int32_t jls_buf_rd_u8(struct jls_buf_s * self, uint8_t * value);
int32_t jls_buf_rd_u16(struct jls_buf_s * self, uint16_t * value);
int32_t jls_buf_rd_u32(struct jls_buf_s * self, uint32_t * value);
int32_t jls_buf_rd_str(struct jls_buf_s * self, const char ** value);


JLS_CPP_GUARD_END

/** @} */

#endif  /* JLS_BUFFER_H__ */
