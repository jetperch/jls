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
 * @brief JLS writer internal functions.
 */

#ifndef JLS_WRITER_PRIV_H__
#define JLS_WRITER_PRIV_H__

#include <stdint.h>
#include "jls/format.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @ingroup jls
 * @defgroup jls_writer Writer
 *
 * @brief JLS writer.
 *
 * @{
 */


// opaque object
struct jls_wr_s;
struct jls_twr_s;


int32_t jls_twr_run(struct jls_twr_s * self);

/** @} */

#ifdef __cplusplus
}
#endif

#endif  /* JLS_WRITER_PRIV_H__ */
