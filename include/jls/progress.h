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


/**
 * @file
 *
 * @brief JLS progress and message callbacks.
 */

#ifndef JLS_PROGRESS_H__
#define JLS_PROGRESS_H__

#include <stdint.h>
#include "jls/cmacro.h"

/**
 * @ingroup jls
 * @defgroup jls_progress Progress
 *
 * @brief JLS progress and message callbacks for long-running operations.
 *
 * @{
 */

JLS_CPP_GUARD_START

/**
 * @brief The function called for messages.
 *
 * @param user_data The arbitrary user data.
 * @param msg The user-meaningful message.
 * @return 0 to continue the operation or any other value to stop
 *      with JLS_ERROR_ABORTED.
 */
typedef int32_t (*jls_msg_fn)(void * user_data, const char * msg);

/**
 * @brief The function called for progress.
 *
 * @param user_data The arbitrary user data.
 * @param progress The normalized progress from 0.0 (starting) to 1.0 done.
 *      Multiply by 100 for percentage.
 * @return 0 to continue the operation or any other value to stop
 *      with JLS_ERROR_ABORTED.
 */
typedef int32_t (*jls_progress_fn)(void * user_data, double progress);

JLS_CPP_GUARD_END

/** @} */

#endif  /* JLS_PROGRESS_H__ */
