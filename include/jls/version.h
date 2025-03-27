/*
 * Copyright 2014-2022 Jetperch LLC
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

#include "jls/cmacro.h"
#include <stdint.h>

/**
 * @ingroup jls
 * @defgroup jls_version Version
 *
 * @brief JLS version.
 *
 * @{
 */

JLS_CPP_GUARD_START

// Use version_update.py to update.
#define JLS_VERSION_MAJOR 0
#define JLS_VERSION_MINOR 12
#define JLS_VERSION_PATCH 1

/**
 * \brief Macro to encode version to uint32_t.
 *
 * \param major The major release number (0 to 255)
 * \param minor The minor release number (0 to 255)
 * \param patch The patch release number (0 to 65535)
 * \returns The 32-bit encoded version number.
 */
#define JLS_VERSION_ENCODE_U32(major, minor, patch) \
    ( (( ((uint32_t) (major)) &   0xff) << 24) | \
      (( ((uint32_t) (minor)) &   0xff) << 16) | \
      (( ((uint32_t) (patch)) & 0xffff) <<  0) )

/**
 * \brief Internal macro to convert argument to string.
 *
 * \param x The argument to convert to a string.
 * \return The string version of x.
 */
#define JLS_VERSION__STR(x) #x

/**
 * \brief Macro to create the version string separated by "." characters.
 *
 * \param major The major release number (0 to 255)
 * \param minor The minor release number (0 to 255)
 * \param patch The patch release number (0 to 65535)
 * \returns The firmware string.
 */
#define JLS_VERSION_ENCODE_STR(major, minor, patch) \
        JLS_VERSION__STR(major) "." JLS_VERSION__STR(minor) "." JLS_VERSION__STR(patch)

/// The JLS version as uint32_t
#define JLS_VERSION_U32 JLS_VERSION_ENCODE_U32(JLS_VERSION_MAJOR, JLS_VERSION_MINOR, JLS_VERSION_PATCH)

/// The JLS version as "major.minor.patch" string
#define JLS_VERSION_STR JLS_VERSION_ENCODE_STR(JLS_VERSION_MAJOR, JLS_VERSION_MINOR, JLS_VERSION_PATCH)

/**
 * @brief Get the JLS version string.
 * 
 * @return The JLS version string.
 */
JLS_API const char * jls_version_str(void);

/**
 * @brief Get the JLS version u32.
 * 
 * @return The JLS version u32 value.
 */
JLS_API uint32_t jls_version_u32(void);

JLS_CPP_GUARD_END

/** @} */

#endif /* JLS_VERSION_H_ */
