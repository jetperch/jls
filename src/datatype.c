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

#include "jls/datatype.h"
#include "jls/format.h"
#include "jls/ec.h"
#include "jls/log.h"
#include <math.h>


static inline int8_t uint4_to_int8(uint8_t k) {
    k = k & 0x0f;
    if (k & 0x08) {
        k |= 0xf0;
    }
    return (int8_t) k;
}

#define TO_DOUBLE(type_) { \
    const type_ * s = (const type_ *) src; \
    for (size_t i = 0; i < samples; ++i) { \
        dst[i] = (double) s[i]; \
    } \
    break; \
}

// Packed sub-byte types convert whole bytes, then the partial final byte:
// an odd count must neither write dst[samples] nor read past the source.
int32_t jls_dt_buffer_to_f64(const void * src, uint32_t src_datatype, double * dst, size_t samples) {
    const uint8_t * u8 = (const uint8_t *) src;
    size_t i = 0;  // sub-byte types: next sample
    switch (src_datatype & 0xffff) {
        case JLS_DATATYPE_I4:
            for (; (i + 2) <= samples; i += 2) {
                uint8_t k = u8[i >> 1];
                dst[i + 0] = (double) uint4_to_int8(k);
                dst[i + 1] = (double) uint4_to_int8(k >> 4);
            }
            if (i < samples) {
                dst[i] = (double) uint4_to_int8(u8[i >> 1]);
            }
            break;
        case JLS_DATATYPE_I8: TO_DOUBLE(int8_t);
        case JLS_DATATYPE_I16: TO_DOUBLE(int16_t);
        case JLS_DATATYPE_I24:
            for (size_t i = 0; i < samples; ++i, u8 += 3) {
                uint32_t k = ((uint32_t) u8[0]) | (((uint32_t) u8[1]) << 8) | (((uint32_t) u8[2]) << 16);
                dst[i] = (double) (int32_t) ((k & 0x00800000) ? (k | 0xff000000) : k);
            }
            break;
        case JLS_DATATYPE_I32: TO_DOUBLE(int32_t);
        case JLS_DATATYPE_I64: TO_DOUBLE(int64_t);
        case JLS_DATATYPE_U1:
            for (; (i + 8) <= samples; i += 8) {
                uint8_t k = u8[i >> 3];
                for (uint32_t b = 0; b < 8; ++b) {
                    dst[i + b] = (double) ((k >> b) & 1);
                }
            }
            for (; i < samples; ++i) {
                dst[i] = (double) ((u8[i >> 3] >> (i & 7)) & 1);
            }
            break;
        case JLS_DATATYPE_U4:
            for (; (i + 2) <= samples; i += 2) {
                uint8_t k = u8[i >> 1];
                dst[i + 0] = (double) (k & 0x0f);
                dst[i + 1] = (double) (k >> 4);
            }
            if (i < samples) {
                dst[i] = (double) (u8[i >> 1] & 0x0f);
            }
            break;
        case JLS_DATATYPE_U8: TO_DOUBLE(uint8_t);
        case JLS_DATATYPE_U16: TO_DOUBLE(uint16_t);
        case JLS_DATATYPE_U24:
            for (size_t i = 0; i < samples; ++i, u8 += 3) {
                dst[i] = (double) (((uint32_t) u8[0]) | (((uint32_t) u8[1]) << 8) | (((uint32_t) u8[2]) << 16));
            }
            break;
        case JLS_DATATYPE_U32: TO_DOUBLE(uint32_t);
        case JLS_DATATYPE_U64: TO_DOUBLE(uint64_t);

        case JLS_DATATYPE_F32: TO_DOUBLE(float);
        case JLS_DATATYPE_F64: TO_DOUBLE(double);
        default:
            JLS_LOGW("Invalid data type: 0x%08x", src_datatype);
            return JLS_ERROR_PARAMETER_INVALID;
    }

    double scale = jls_dt_scale(src_datatype);
    if (scale != 1.0) {
        for (size_t i = 0; i < samples; ++i) {
            dst[i] *= scale;
        }
    }
    return 0;
}

double jls_dt_scale(uint32_t datatype) {
    // fixed point: value = raw * 2^-q, see JLS_DATATYPE_DEF
    int8_t q = (int8_t) jls_datatype_parse_q(datatype);
    if ((jls_datatype_parse_basetype(datatype) & JLS_DATATYPE_BASETYPE_INT) && q) {
        return ldexp(1.0, -q);
    }
    return 1.0;
}

const char * jls_dt_str(uint32_t datatype) {
    switch (datatype & 0xffff) {
        case JLS_DATATYPE_I4:  return "i4";
        case JLS_DATATYPE_I8:  return "i8";
        case JLS_DATATYPE_I16: return "i16";
        case JLS_DATATYPE_I24: return "i24";
        case JLS_DATATYPE_I32: return "i32";
        case JLS_DATATYPE_I64: return "i64";

        case JLS_DATATYPE_U1:  return "u1";
        case JLS_DATATYPE_U4:  return "u4";
        case JLS_DATATYPE_U8:  return "u8";
        case JLS_DATATYPE_U16: return "u16";
        case JLS_DATATYPE_U24: return "u24";
        case JLS_DATATYPE_U32: return "u32";
        case JLS_DATATYPE_U64: return "u64";

        case JLS_DATATYPE_F32: return "f32";
        case JLS_DATATYPE_F64: return "f64";

        default: return "dt_unknown";
    }
}
