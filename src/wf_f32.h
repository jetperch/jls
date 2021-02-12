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
 * @brief JLS float32 signal writer.
 */

#ifndef JLS_WRITE_F32_H__
#define JLS_WRITE_F32_H__

#include <stdint.h>
#include "jls/format.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @ingroup jls
 * @defgroup jls_wf_f32 FSR float32 signal writer.
 *
 * @brief JLS FSR float32 signal writer.
 *
 * @{
 */

struct jls_wr_s; // opaque
struct jls_wf_f32_s;

struct jls_wf_f32_def_s {
    uint16_t signal_id;
    uint32_t samples_per_data;
    uint32_t sample_decimate_factor;
    uint32_t entries_per_summary;
    uint32_t summary_decimate_factor;
};

int32_t jls_wf_f32_align_def(struct jls_wf_f32_def_s * def);
int32_t jls_wf_f32_open(struct jls_wf_f32_s ** instance, struct jls_wr_s * wr, const struct jls_wf_f32_def_s * def);
int32_t jls_wf_f32_close(struct jls_wf_f32_s * self);
int32_t jls_wf_f32_data(struct jls_wf_f32_s * self, int64_t sample_id, const float * data, uint32_t data_length);


/** @} */

#ifdef __cplusplus
}
#endif

#endif  /* JLS_WRITE_F32_H__ */
