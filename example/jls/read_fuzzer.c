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

#include "jls.h"
#include "cstr.h"
#include "jls_util_prv.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>


#define PAYLOAD_MAX_SIZE (32U * 1024U * 1024U)

static uint64_t random_seed = 1;
static const uint64_t random_multiplier = (2654435761ULL | (2654435761ULL << 32));


static uint64_t random_u64(void) {
    random_seed *= random_multiplier;
    random_seed >>= 1;
    random_seed *= random_multiplier;
    return random_seed;
}

static uint32_t random_u32(void) {
    random_seed *= random_multiplier;
    random_seed >>= 1;
    return (uint32_t) random_seed;
}

static uint32_t random_range_u32(uint32_t min_inc, uint32_t max_exc) {
    return (random_u32() % (max_exc - min_inc)) + min_inc;
}

static int64_t random_range_i64(int64_t min_inc, int64_t max_exc) {
    uint64_t y = (random_u64() % (uint64_t) (max_exc - min_inc));
    return min_inc + (int64_t) y;
}

static int usage(void) {
    printf(
            "usage: jls read_fuzzer [--<arg> <value>] <path>\n"
            "Perform read fuzz testing on a JLS file.\n"
            "\n"
            "Required positional arguments:"
            "  path        The path to the JLS file to read\n"
            "\n"
            "Optional arguments:\n"
            "  random      The 64-bit random number seed\n"
            "  max_length  The maximum FSR read length in entries\n"
    );
    return 1;
}

static bool is_mem_const(void * mem, size_t mem_size, uint8_t c) {
    uint8_t * m = (uint8_t *) mem;
    uint8_t * m_end = m + mem_size;
    while (m < m_end) {
        if (*m++ != c) {
            return false;
        }
    }
    return true;
}

int on_read_fuzzer(struct app_s * self, int argc, char * argv[]) {
    struct jls_rd_s * rd = NULL;
    char * path = NULL;
    uint8_t guard_byte = 0xCC;
    uint32_t guard_length = 32;
    uint8_t * guard;
    int pos_arg = 0;
    uint32_t max_length = 5000;
    (void) self;

    while (argc) {
        if (argv[0][0] != '-') {
            if (pos_arg == 0) {
                path = argv[0];
            } else {
                return usage();
            }
            ARG_CONSUME();
            ++pos_arg;
        } else if (0 == strcmp("--random", argv[0])) {
            ARG_CONSUME();
            ARG_REQUIRE();
            if (jls_cstr_to_u64(argv[0], &random_seed)) {
                return usage();
            }
            ARG_CONSUME();
        } else if (0 == strcmp("--max-length", argv[0])) {
            ARG_CONSUME();
            ARG_REQUIRE();
            if (jls_cstr_to_u32(argv[0], &max_length)) {
                return usage();
            }
            ARG_CONSUME();
        }
    }
    if (pos_arg != 1) {
        return usage();
    }

    ROE(jls_rd_open(&rd, path));

    struct jls_signal_def_s * signals;
    uint16_t signal_count = 0;
    ROE(jls_rd_signals(rd, &signals, &signal_count));
    if ((signal_count >= 1) && (signals[0].signal_id == 0)) {
        ++signals;
        --signal_count;
    }
    if (0 == signal_count) {
        printf("Signals: none found, cannot fuzz test\n");
        jls_rd_close(rd);
        return 0;
    }

    size_t data_size = 10000000;
    uint8_t * data = malloc(data_size);
    int32_t rc;

    while (!quit_) {
        printf("%21" PRIu64 ": ", random_seed);
        int64_t samples = 0;
        uint32_t signal_id = random_range_u32(0, signal_count);
        struct jls_signal_def_s * s = &signals[signal_id];
        rc = jls_rd_fsr_length(rd, s->signal_id, &samples);
        if (rc) {
            printf("\njls_rd_fsr_length returned %" PRIi32 "\n", rc);
            break;
        }
        uint8_t test_type = random_range_u32(0, 2);
        int64_t s_start = random_range_i64(0, samples - 1);
        int64_t s_end = random_range_i64(s_start + 1, samples);
        int64_t s_length = s_end - s_start;

        if (test_type == 0) {  // FSR samples
            if (s_length > max_length) {
                s_length = max_length;
            }
            s_length = random_range_i64(1, s_length + 1);
            printf("SAMPLES %d, %" PRIi64 ", %" PRIi64 "\n", s->signal_id, s_start, s_length);
            size_t length_bytes = (s_length * jls_datatype_parse_size(s->data_type) + 7) / 8;
            guard = data + length_bytes;
            memset(guard, guard_byte, guard_length);
            *(guard - 1) = guard_byte;
            rc = jls_rd_fsr(rd, s->signal_id, s_start, data, s_length);
            if (rc) {
                printf("jls_rd_fsr returned %" PRIi32 "\n", rc);
                break;
            }
            if (*(guard - 1) == guard_byte) {
                printf("DOH");
                break;
            }
        } else {  // FSR summary
            int64_t increment = random_range_i64(1, s_length + 1);
            s_length = s_length / increment;
            if (s_length > max_length) {
                s_length = max_length;
            }
            printf("STATS %d, %" PRIi64 ", %" PRIi64 ", %" PRIi64 "\n",
                   s->signal_id, s_start, increment, s_length);
            guard = data + s_length * 32;
            memset(guard, guard_byte, guard_length);
            rc = jls_rd_fsr_statistics(rd, s->signal_id, s_start, increment,
                (double *) data, s_length);
            if (rc) {
                printf("jls_rd_fsr_statistics returned %" PRIi32 "\n", rc);
                break;
            }
        }
        if (!is_mem_const(guard, guard_length, guard_byte)) {
            printf("guard failed: ");
            for (uint32_t i = 0; i < guard_length; ++i) {
                printf(" %02x", guard[i]);
            }
            printf("\n");
            break;
        }
    }

    jls_rd_close(rd);
    return 0;
}
