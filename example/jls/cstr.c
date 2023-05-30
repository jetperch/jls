/*
 * Copyright 2015-2021 Jetperch LLC
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

#include "cstr.h"


#define FLOAT_EXP_MAX (38)


static int _isspace(char c) {
    if ((c == ' ') || ((c >= 9) && (c <= 13))) {
        return 1;
    }
    return 0;
}

int jls_cstr_to_u16(const char * src, uint16_t * value) {
    uint64_t u64;
    int rc = jls_cstr_to_u64(src, &u64);
    if (rc) {
        return rc;
    }
    if (u64 > UINT16_MAX) {
        return 1; // overflow
    }
    *value = (uint16_t) u64;
    return 0;
}

int jls_cstr_to_i16(const char * src, int16_t * value) {
    int64_t i64;
    int rc = jls_cstr_to_i64(src, &i64);
    if (rc) {
        return rc;
    }
    if ((i64 > INT16_MAX) || (i64 < INT16_MIN)) {
        return 1; // overflow
    }
    *value = (int16_t) i64;
    return 0;
}

int jls_cstr_to_u32(const char * src, uint32_t * value) {
    uint64_t u64;
    int rc = jls_cstr_to_u64(src, &u64);
    if (rc) {
        return rc;
    }
    if (u64 > UINT32_MAX) {
        return 1; // overflow
    }
    *value = (uint32_t) u64;
    return 0;
}

int jls_cstr_to_i32(const char * src, int32_t * value) {
    int64_t i64;
    int rc = jls_cstr_to_i64(src, &i64);
    if (rc) {
        return rc;
    }
    if ((i64 > INT32_MAX) || (i64 < INT32_MIN)) {
        return 1; // overflow
    }
    *value = (int32_t) i64;
    return 0;
}

int jls_cstr_to_u64(const char * src, uint64_t * value) {
    uint64_t v = 0;

    if ((NULL == src) || (NULL == value)) {
        return 1;
    }

    while (*src && _isspace((uint8_t) *src)) {
        ++src;
    }
    if (!*src) { // empty string.
        return 1;
    }
    if ((src[0] == '0') && (src[1] == 'x')) {
        // hex string
        uint64_t nibble;
        src += 2;
        while (*src) {
            char c = *src;
            if ((c >= '0') && (c <= '9')) {
                nibble = c - '0';
            } else if ((c >= 'a') && (c <= 'f')) {
                nibble = c - 'a' + 10;
            } else if ((c >= 'A') && (c <= 'F')) {
                nibble = c - 'A' + 10;
            } else if (c == '_') {
                ++src;
                continue;
            } else {
                break;
            }
            if (v & 0xFF00000000000000LLU) {
                return 1;  // overflow
            }
            v = (v << 4) + nibble;
            ++src;
        }
    } else {
        while ((*src >= '0') && (*src <= '9')) {
            if (((v >> 32) * 10) & 0xFFFFFFFF00000000LLU) {
                return 1;  // overflow
            }
            v = v * 10 + (*src - '0');
            ++src;
        }
    }
    while (*src) {
        if (!_isspace((uint8_t) *src++)) { // did not parse full string
            return 1;
        }
    }
    *value = v;
    return 0;
}

int jls_cstr_to_i64(const char * src, int64_t * value) {
    int neg = 0;
    uint64_t v;

    if ((NULL == src) || (NULL == value)) {
        return 1;
    }

    while (*src && _isspace((uint8_t) *src)) {
        ++src;
    }

    if (*src == '-') {
        neg = 1;
        ++src;
    } else if (*src == '+') {
        ++src;
    }

    int rc = jls_cstr_to_u64(src, &v);
    if (rc) {
        return rc;
    }
    if (v > INT64_MAX) {
        return 1;
    }
    *value = (int64_t) v;
    if (neg) {
        *value = -*value;
    }
    return 0;
}
