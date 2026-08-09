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
 * @brief JLS track handler.
 */

#ifndef JLS_TRACK_H__
#define JLS_TRACK_H__

#include "jls/cmacro.h"
#include "jls/format.h"
#include "jls/core.h"
#include <stdint.h>
#include <stddef.h>

/**
 * @ingroup jls
 * @defgroup jls_track JLS track.
 *
 * @brief JLS track implementation shared by read, write, and repair.
 *
 * @{
 */

JLS_CPP_GUARD_START

int32_t jls_track_wr_def(struct jls_core_track_s * track_info);
int32_t jls_track_wr_head(struct jls_core_track_s * track_info);
int32_t jls_track_update(struct jls_core_track_s * track, uint8_t level, int64_t pos);

/**
 * @brief Repair track head, index, summary, data pointer linkages.
 *
 * @param track The track instance.
 * @return 0 or error code.
 *
 * This function correctly rebuilds the track data structure when
 * the file is truncated.
 */
int32_t jls_track_repair_pointers(struct jls_core_track_s * track);

JLS_CPP_GUARD_END

/** @} */

#endif  /* JLS_TRACK_H__ */
