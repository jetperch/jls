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
 * @brief JLS offline repair.
 */

#ifndef JLS_REPAIR_H__
#define JLS_REPAIR_H__

#include <stdint.h>
#include "jls/cmacro.h"
#include "jls/progress.h"

/**
 * @ingroup jls
 * @defgroup jls_repair Repair
 *
 * @brief JLS offline, in-place file repair.
 *
 * @{
 */

JLS_CPP_GUARD_START

/**
 * @brief Repair a damaged JLS file in place.
 *
 * @param path The target path, which must be writable.
 * @param msg_fn The function to call for messages, or NULL.
 * @param msg_user_data The arbitrary data provided to msg_fn.
 * @param progress_fn The function to call for progress, or NULL.
 * @param progress_user_data The arbitrary data for progress_fn.
 * @return 0 or error code.  JLS_ERROR_ABORTED when a callback
 *      requested stop.
 *
 * jls_rd_open() automatically recovers tail truncation, the common
 * damage from writer crashes and power loss, in O(crash tail).  It
 * fails with JLS_ERROR_MESSAGE_INTEGRITY on any other corruption:
 * torn track head payloads, interior bit errors or zeroed regions,
 * and damaged definition chains.  This operation repairs those cases
 * with a single full-file traversal: it validates every chunk,
 * relinks chains around dead chunks, rebuilds lost track heads,
 * converts dead sample data to omitted (summary-reconstructed)
 * regions, and finalizes the file.
 *
 * Data within dead chunks is not recoverable.  Sample reads over such
 * regions return summary-reconstructed values when the covering
 * summary survives, and fail otherwise.  For read-only media, use
 * jls_copy() to salvage into a new file instead.
 *
 * Chain repair covers sample (FSR) data only.  Annotation, UTC, and
 * VSR chunks weave mixed chains that readers walk inline, so damage
 * to those chunks is not relinked: reads of the affected track can
 * still fail with JLS_ERROR_MESSAGE_INTEGRITY after a successful
 * repair.  Use jls_copy() to salvage the readable prefix of such
 * tracks into a new file.
 */
JLS_API int32_t jls_repair(const char * path,
                           jls_msg_fn msg_fn, void * msg_user_data,
                           jls_progress_fn progress_fn, void * progress_user_data);

JLS_CPP_GUARD_END

/** @} */

#endif  /* JLS_REPAIR_H__ */
