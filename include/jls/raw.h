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


/**
 * @file
 *
 * @brief JLS raw access.
 */

#ifndef JLS_RAW_H__
#define JLS_RAW_H__

#include <stdio.h>
#include <stdint.h>
#include "jls/format.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @ingroup jls
 * @defgroup jls_raw JLS raw chunk-level access.
 *
 * @brief JLS raw access.
 *
 * @{
 */

struct jls_raw_s {
    struct jls_chunk_header_s hdr;  // the header for the current chunk
    int64_t offset;                 // the offset for the current chunk
    int64_t fpos;                   // the current file position (minimize fseek)
    uint32_t payload_length_prev;
    FILE * f;
    uint8_t write_en;
};

int32_t jls_raw_open(struct jls_raw_s ** instance, const char * path, const char * mode);
int32_t jls_raw_close(struct jls_raw_s * self);

int32_t jls_raw_wr(struct jls_raw_s * self, uint8_t tag, uint16_t chuck_meta, uint32_t payload_length, const uint8_t * payload);
int32_t jls_raw_wr_header(struct jls_raw_s * self, uint8_t tag, uint16_t chuck_meta, uint32_t payload_length);
int32_t jls_raw_wr_payload(struct jls_raw_s * self, uint32_t payload_length, const uint8_t * payload);

int32_t jls_raw_rd(struct jls_raw_s * self, struct jls_chunk_header_s * hdr, uint32_t payload_length_max, uint8_t * payload);
int32_t jls_raw_rd_header(struct jls_raw_s * self, struct jls_chunk_header_s * hdr);
int32_t jls_raw_rd_payload(struct jls_raw_s * self, uint32_t payload_length_max, uint8_t * payload);

int32_t jls_raw_chunk_seek(struct jls_raw_s * self, int64_t offset, struct jls_chunk_header_s * hdr);
int64_t jls_raw_chunk_tell(struct jls_raw_s * self);
int32_t jls_raw_chunk_next(struct jls_raw_s * self, struct jls_chunk_header_s * hdr);
int32_t jls_raw_chunk_prev(struct jls_raw_s * self, struct jls_chunk_header_s * hdr);
int32_t jls_raw_item_next(struct jls_raw_s * self, struct jls_chunk_header_s * hdr);
int32_t jls_raw_item_prev(struct jls_raw_s * self, struct jls_chunk_header_s * hdr);


/** @} */

#ifdef __cplusplus
}
#endif

#endif  /* JLS_RAW_H__ */
