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

/*
 * Dynamically detect the CRC implementation.
 *
 * We would prefer to use the build process to include the correct
 * implementation.  However, the variations with building
 * python wheels make this very challenging, especially considering
 * macOS universal2 builds.  Since the native compilation must know
 * compiler flags, we make the decision here.
 */

#if defined(JLS_OPTIMIZE_CRC_DISABLE)
#include "crc32c_sw.c"

#elif defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__)
#if defined(_M_X64) || defined(__x86_64__)
#include "crc32c_intel_sse4.c"
#else
#include "crc32c_sw.c"
#endif

#elif defined(__APPLE__) && defined(__MACH__)
#if defined(_M_ARM64) || defined(__aarch64__) || defined(__arm64__)
#include "crc32c_arm_neon.c"
#elif defined(_M_X64) || defined(__x86_64__)
#include "crc32c_intel_sse4.c"
#else
#include "crc32c_sw.c"
#endif

#elif defined(__linux__) && __linux__
#if defined(_M_X64) || defined(__x86_64__)
#include "crc32c_intel_sse4.c"
#else
// Raspberry Pi 4 does not support CRC optimization.
// Other Linux platforms may, but disable for now.
#include "crc32c_sw.c"
#endif

#else
#include "crc32c_sw.c"

#endif
