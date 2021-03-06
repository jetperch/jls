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
 * @brief JLS raw backend.
 */

#ifndef JLS_PRIV_RAW_BACKEND_H__
#define JLS_PRIV_RAW_BACKEND_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @ingroup jls
 * @defgroup jls_raw_backend Raw backend
 *
 * @brief JLS raw OS-specific backend.
 *
 * @{
 */

struct jls_rawbk_s {
    int64_t fpos;                   // the current file position, to reduce ftell calls.
    int64_t fend;                   // the file end offset.
    int fd;
};

int32_t jls_rawbk_fopen(struct jls_rawbk_s * self, const char * filename, const char * mode);
int32_t jls_rawbk_fclose(struct jls_rawbk_s * self);
int32_t jls_rawbk_fwrite(struct jls_rawbk_s * self, const void * buffer, unsigned int count);
int32_t jls_rawbk_fread(struct jls_rawbk_s * self, void * const buffer, unsigned const buffer_size);
int32_t jls_rawbk_fseek(struct jls_rawbk_s * self, int64_t offset, int origin);
int64_t jls_rawbk_ftell(struct jls_rawbk_s * self);
int64_t jls_now();
struct jls_time_counter_s jls_time_counter();

/** @} */

#ifdef __cplusplus
}
#endif

#endif  /* JLS_PRIV_RAW_BACKEND_H__ */
