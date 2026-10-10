/*
 * Copyright 2022 Jetperch LLC
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

#include "jls/bit_shift.h"
#include "jls/ec.h"
#include <string.h>

int32_t jls_bit_shift_array_right(uint8_t bits, void * data, size_t size) {
    if ((bits == 0) || (size == 0)) {
        return 0;
    }
    if (bits >= 8) {
        return JLS_ERROR_PARAMETER_INVALID;
    }

    uint8_t * u8 = (uint8_t *) data;
    if (size == 1) {
        u8[0] >>= bits;
        return 0;
    }
    uint8_t carry = u8[0] >> bits;
    for (size_t i = 1; i < size; ++i) {
        u8[i - 1] = (u8[i] << (8 - bits)) | carry;
        carry = u8[i] >> bits;
    }
    u8[size - 1] = carry;
    return 0;
}

void jls_bit_copy(uint8_t * dst, uint64_t dst_offset, const uint8_t * src, uint64_t src_offset, uint64_t bits) {
    if (!bits) {
        return;
    }
    dst += (size_t) (dst_offset >> 3);
    src += (size_t) (src_offset >> 3);
    uint32_t d_off = (uint32_t) (dst_offset & 7);
    uint32_t s_off = (uint32_t) (src_offset & 7);
    if (!d_off && !s_off) {
        size_t bytes = (size_t) (bits >> 3);
        memcpy(dst, src, bytes);
        uint32_t tail = (uint32_t) (bits & 7);
        if (tail) {
            dst[bytes] = src[bytes] & (uint8_t) ((1U << tail) - 1);
        }
        return;
    }
    while (bits) {
        if (!d_off && (bits >= 8)) {
            // Byte-aligned destination: build whole bytes from two source bytes.
            // Both source bytes hold copied bits, so this reads nothing extra.
            size_t bytes = (size_t) (bits >> 3);
            if (!s_off) {
                memcpy(dst, src, bytes);
            } else {
                uint32_t s_hi = 8 - s_off;
                for (size_t i = 0; i < bytes; ++i) {
                    dst[i] = (uint8_t) ((src[i] >> s_off) | (src[i + 1] << s_hi));
                }
            }
            dst += bytes;
            src += bytes;
            bits &= 7;
            if (!bits) {
                break;
            }
        }
        uint32_t take = 8 - d_off;
        if (take > bits) {
            take = (uint32_t) bits;
        }
        uint32_t v = ((uint32_t) src[0]) >> s_off;
        if ((s_off + take) > 8) {  // only touch src[1] when it holds copied bits
            v |= ((uint32_t) src[1]) << (8 - s_off);
        }
        v &= (1U << take) - 1;
        dst[0] = (uint8_t) ((dst[0] & ((1U << d_off) - 1)) | (v << d_off));
        d_off += take;
        if (d_off >= 8) {
            d_off = 0;
            ++dst;
        }
        s_off += take;
        src += s_off >> 3;
        s_off &= 7;
        bits -= take;
    }
}
