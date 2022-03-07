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
    return 0;
}
