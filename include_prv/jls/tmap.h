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
 * @brief JLS timestamp <-> sample_id mapping for FSR channels.
 */

#ifndef JLS_PRIV_RD_FSR_H__
#define JLS_PRIV_RD_FSR_H__

#include <stdint.h>
#include <stddef.h>
#include "jls/format.h"


#ifdef __cplusplus
extern "C" {
#endif

/**
 * @ingroup jls
 * @defgroup jls_tmap Time mapping for FSR sample_id & JLS timestamp conversions.
 *
 * @brief Convert between sample_id and JLS timestamps for the
 *      JLS reader for FSR signals with UTC channels.
 *
 * @{
 */


/// The opaque instance.
struct jls_tmap_s;

struct jls_tmap_s * jls_tmap_alloc(double sample_rate);
void jls_tmap_free(struct jls_tmap_s * self);

int32_t jls_tmap_add_cbk(void * user_data, const struct jls_utc_summary_entry_s * utc, uint32_t size);
int32_t jls_tmap_add(struct jls_tmap_s * self, int64_t sample_id, int64_t timestamp);
int32_t jls_tmap_sample_id_to_timestamp(struct jls_tmap_s * self, int64_t sample_id, int64_t * timestamp);
int32_t jls_tmap_timestamp_to_sample_id(struct jls_tmap_s * self, int64_t timestamp, int64_t * sample_id);


/** @} */

#ifdef __cplusplus
}
#endif

#endif  /* JLS_PRIV_RD_FSR_H__ */
