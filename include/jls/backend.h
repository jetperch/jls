/*
 * Copyright 2021-2022 Jetperch LLC
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
#include "jls/cmacro.h"

JLS_CPP_GUARD_START

/**
 * @ingroup jls
 * @defgroup jls_raw_backend Raw backend
 *
 * @brief JLS raw OS-specific backend.
 *
 * @{
 */

#define JLS_BK_MSG_WRITE_TIMEOUT_MS (5000)
#define JLS_BK_MSG_LOCK_TIMEOUT_MS (5000)
#define JLS_BK_PROCESS_LOCK_TIMEOUT_MS (2500)
#define JLS_BK_FLUSH_TIMEOUT_MS (20000)
#define JLS_BK_CLOSE_TIMEOUT_MS (1000)

/**
 * @brief The backend instance.
 */
struct jls_bkf_s {
    int64_t fpos;    ///< the current file position, to reduce ftell calls.
    int64_t fend;    ///< the file end offset.
    int fd;          ///< The file descriptor.
};

int32_t jls_bk_fopen(struct jls_bkf_s * self, const char * filename, const char * mode);
int32_t jls_bk_fclose(struct jls_bkf_s * self);
int32_t jls_bk_fwrite(struct jls_bkf_s * self, const void * buffer, unsigned int count);
int32_t jls_bk_fread(struct jls_bkf_s * self, void * const buffer, unsigned const buffer_size);
int32_t jls_bk_fseek(struct jls_bkf_s * self, int64_t offset, int origin);
int64_t jls_bk_ftell(struct jls_bkf_s * self);
int32_t jls_bk_fflush(struct jls_bkf_s * self);
int32_t jls_bk_truncate(struct jls_bkf_s * self);

// forward declaration for "threaded_writer.h"
struct jls_twr_s;
struct jls_bkt_s * jls_bkt_initialize(struct jls_twr_s * wr);
void jls_bkt_finalize(struct jls_bkt_s * self);
int jls_bkt_msg_lock(struct jls_bkt_s * self);          // 0 on success or error code
int jls_bkt_msg_unlock(struct jls_bkt_s * self);        // 0 on success or error code
int jls_bkt_process_lock(struct jls_bkt_s * self);      // 0 on success or error code
int jls_bkt_process_unlock(struct jls_bkt_s * self);    // 0 on success or error code
void jls_bkt_msg_wait(struct jls_bkt_s * self);
void jls_bkt_msg_signal(struct jls_bkt_s * self);
void jls_bkt_sleep_ms(uint32_t duration_ms);


JLS_API int64_t jls_now(void);
JLS_API struct jls_time_counter_s jls_time_counter(void);

/** @} */

JLS_CPP_GUARD_END

#endif  /* JLS_PRIV_RAW_BACKEND_H__ */
