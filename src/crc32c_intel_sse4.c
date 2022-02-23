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

#include "jls/crc32c.h"
#include <nmmintrin.h>
#include <assert.h>
// https://software.intel.com/sites/landingpage/IntrinsicsGuide/#text=crc&expand=1288
// Could consider https://github.com/htot/crc32c/blob/master/crc32c/crc_iscsi_v_pcl.asm

uint32_t jls_crc32c_hdr(const struct jls_chunk_header_s * hdr) {
    uint32_t crc32;
#if defined(__x86_64__) || defined(_M_X64)
    uint64_t crc64;
    assert(0 == (0x7 & (intptr_t) hdr));
    const uint64_t * data = (const uint64_t *) hdr;
    crc64 = _mm_crc32_u64(0xFFFFFFFF, data[0]);
    crc64 = _mm_crc32_u64(crc64, data[1]);
    crc64 = _mm_crc32_u64(crc64, data[2]);
    crc32 = _mm_crc32_u32((uint32_t) crc64, (uint32_t) data[3]);
#else
    assert(0 == (0x3 & (intptr_t) hdr));
    const uint32_t * data = (const uint32_t *) hdr;
    crc32 = _mm_crc32_u32(0xFFFFFFFF, data[0]);
    crc32 = _mm_crc32_u32(crc32, data[1]);
    crc32 = _mm_crc32_u32(crc32, data[2]);
    crc32 = _mm_crc32_u32(crc32, data[3]);
    crc32 = _mm_crc32_u32(crc32, data[4]);
    crc32 = _mm_crc32_u32(crc32, data[5]);
    crc32 = _mm_crc32_u32(crc32, data[6]);
#endif
    return (crc32 ^ 0xFFFFFFFF);
}

uint32_t jls_crc32c(uint8_t const *data, uint32_t length) {
    uint32_t crc = 0xFFFFFFFF;
    for (; ((length > 0) && (0x7 & (intptr_t) data)); ++data, --length) {
        crc = _mm_crc32_u8(crc, *data);
    }
#if defined(__x86_64__) || defined(_M_X64)
    for (; length >= 8; data += 8, length -= 8) {
        crc = (uint32_t) _mm_crc32_u64(crc, *((const uint64_t *) data));
    }
#else
    for (; length >= 4; data += 4, length -= 4) {
        crc = _mm_crc32_u32(crc, *((const uint32_t *) data));
    }
#endif
    for (; length > 0; ++data, --length) {
        crc = _mm_crc32_u8(crc, *data);
    }
    return (crc ^ 0xFFFFFFFF);
}
