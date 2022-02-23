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

#include "jls/crc32c.h"
#include <arm_acle.h>
#include <arm_neon.h>
#include <assert.h>

// Used by Raspberry Pi 4 and new M1 Macs.
// See https://github.com/google/crc32c/blob/master/src/crc32c_arm64.cc
// Generic software implementation by Mark Adler: https://stackoverflow.com/a/17646775/888653

uint32_t jls_crc32c_hdr(const struct jls_chunk_header_s * hdr) {
    uint32_t crc32 = 0xFFFFFFFF;
    assert(0 == (0x7 & (intptr_t) hdr));
    const uint64_t * data64 = (const uint64_t *) hdr;
    const uint32_t * data32 = (const uint32_t *) hdr;
    crc32 = __crc32cd(crc32, *(data64 + 0));
    crc32 = __crc32cd(crc32, *(data64 + 1));
    crc32 = __crc32cd(crc32, *(data64 + 2));
    crc32 = __crc32cw(crc32, *(data32 + 6));
    return (crc32 ^ 0xFFFFFFFF);
}

uint32_t jls_crc32c(uint8_t const *data, uint32_t length) {
    uint32_t crc = 0xFFFFFFFF;
    for (; ((length > 0) && (0x7 & (intptr_t) data)); ++data, --length) {
        crc = __crc32cb(crc, *data);
    }
    for (; length >= 8; data += 8, length -= 8) {
        crc = __crc32cd(crc, *((const uint64_t *) data));
    }
    for (; length > 0; ++data, --length) {
        crc = __crc32cb(crc, *data);
    }
    return (crc ^ 0xFFFFFFFF);
}
