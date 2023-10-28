/*
 * Copyright 2023 Jetperch LLC
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
 * @brief JLS copy.
 */

#ifndef JLS_COPY_H__
#define JLS_COPY_H__

#include <stdint.h>
#include "jls/cmacro.h"
#include "jls/format.h"

/**
 * @ingroup jls
 * @defgroup jls_copy Copy
 *
 * @brief JLS copy.
 *
 * @{
 */

JLS_CPP_GUARD_START

/**
 * @brief The function called for messages.
 *
 * @param user_data The arbitrary user data.
 * @param msg The user-meaningful message.
 * @return 0 to continue copy or any other value to stop.
 */
typedef int32_t (*jls_copy_msg_fn)(void * user_data, const char * msg);

/**
 * @brief The function called for progress.
 *
 * @param user_data The arbitrary user data.
 * @param progress The normalized progress from 0.0 (starting) to 1.0 done.
 *      Multiply by 100 for percentage.
 * @return 0 to continue copy or any other value to stop.
 */
typedef int32_t (*jls_copy_progress_fn)(void * user_data, double progress);


/**
 * @brief Copy a JLS file.
 *
 * @param src The source path.
 * @param dst The destination path.
 * @param msg_fn The function to call for messages.
 * @param msg_user_data The arbitrary data provided to msg_fn.
 * @param progress_fn The function to call for progress indication.
 * @param progress_user_data The arbitrary data for progress_fn.
 * @return 0 or error code.
 *
 */
JLS_API int32_t jls_copy(const char * src, const char * dst,
                         jls_copy_msg_fn msg_fn, void * msg_user_data,
                         jls_copy_progress_fn progress_fn, void * progress_user_data);

JLS_CPP_GUARD_END

/** @} */

#endif  /* JLS_COPY_H__ */
