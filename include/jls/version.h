/*
 * Copyright 2014-2021 Jetperch LLC
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
 * @brief JLS library version.
 */

#ifndef JLS_VERSION_H_
#define JLS_VERSION_H_

/**
 * @ingroup jls
 * @defgroup jls_version Version
 *
 * @brief JLS Version.
 *
 * @{
 */


#define JLS_PLATFORM_ 1

#define EMBC_VERSION_MAJOR   0
#define EMBC_VERSION_MINOR   0
#define EMBC_VERSION_PATCH   1
#define EMBC_VERSION_U32     ((uint32_t) ( \
    ((0 & 0xff) << 24) | \
    ((0 & 0xff) << 16) | \
    (1 & 0xffff) ))
#define EMBC_VERSION_STR     "0.0.1"

/** @} */

#endif /* JLS_VERSION_H_ */

