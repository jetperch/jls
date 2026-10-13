/*
 * Copyright 2026 Jetperch LLC
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

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include "jls/msg_ring_buffer.h"
#include <stdlib.h>
#include <string.h>

// Exact-size heap buffers so ASan flags any access past buf_size.

static void test_empty(void **state) {
    (void) state;
    uint8_t * buf = malloc(64);
    struct jls_mrb_s m;
    uint32_t sz = 99;
    jls_mrb_init(&m, buf, 64);
    assert_int_equal(0, jls_mrb_used_bytes(&m));
    assert_null(jls_mrb_peek(&m, &sz));
    assert_int_equal(0, sz);
    assert_null(jls_mrb_pop(&m, &sz));
    free(buf);
}

static void test_size_limits(void **state) {
    (void) state;
    const uint32_t buf_size = 1024;
    uint8_t * buf = malloc(buf_size);
    struct jls_mrb_s m;
    uint32_t sz = 0;
    jls_mrb_init(&m, buf, buf_size);
    uint32_t size_max = jls_mrb_msg_size_max(&m);
    assert_int_equal(buf_size - 9, size_max);
    assert_null(jls_mrb_alloc(&m, buf_size));
    assert_null(jls_mrb_alloc(&m, buf_size - 2));
    assert_null(jls_mrb_alloc(&m, size_max + 1));
    assert_null(jls_mrb_alloc(&m, 0xffffffffU));

    for (int k = 0; k < 3; ++k) {  // repeat to also start from nonzero head
        uint8_t * p = jls_mrb_alloc(&m, size_max);
        assert_non_null(p);
        memset(p, 0x55, size_max);
        assert_null(jls_mrb_alloc(&m, 1));  // full
        assert_ptr_equal(p, jls_mrb_peek(&m, &sz));
        assert_int_equal(size_max, sz);
        assert_ptr_equal(p, jls_mrb_pop(&m, &sz));
        assert_int_equal(0, jls_mrb_used_bytes(&m));
        assert_non_null(jls_mrb_alloc(&m, 7));
        assert_non_null(jls_mrb_pop(&m, &sz));
    }
    free(buf);

    uint8_t tiny[8];
    jls_mrb_init(&m, tiny, sizeof(tiny));
    assert_int_equal(0, jls_mrb_msg_size_max(&m));
    assert_null(jls_mrb_alloc(&m, 0));
}

static void test_fifo_model(void **state) {
    (void) state;
    for (uint32_t buf_size = 16; buf_size < 160; buf_size += 7) {
        uint8_t * buf = malloc(buf_size);
        struct jls_mrb_s m;
        jls_mrb_init(&m, buf, buf_size);
        uint32_t fifo_sz[64];
        uint8_t fifo_val[64];
        uint32_t fifo_head = 0;
        uint32_t fifo_tail = 0;
        uint32_t seq = 0;
        srand(buf_size);
        for (int iter = 0; iter < 5000; ++iter) {
            if ((rand() & 1) && ((fifo_head - fifo_tail) < 64)) {
                uint32_t size = (uint32_t) rand() % (jls_mrb_msg_size_max(&m) + 1);
                uint8_t * p = jls_mrb_alloc(&m, size);
                if (p) {
                    memset(p, (uint8_t) seq, size);
                    fifo_sz[fifo_head & 63] = size;
                    fifo_val[fifo_head & 63] = (uint8_t) seq;
                    ++fifo_head;
                    ++seq;
                }
            } else {
                uint32_t sz = 0;
                uint8_t * p = jls_mrb_pop(&m, &sz);
                if (fifo_head == fifo_tail) {
                    assert_null(p);
                } else {
                    assert_non_null(p);
                    assert_int_equal(fifo_sz[fifo_tail & 63], sz);
                    for (uint32_t i = 0; i < sz; ++i) {
                        assert_int_equal(fifo_val[fifo_tail & 63], p[i]);
                    }
                    ++fifo_tail;
                }
            }
            assert_int_equal(fifo_head - fifo_tail, m.count);
            assert_true(jls_mrb_used_bytes(&m) < buf_size);
        }
        free(buf);
    }
}

int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_empty),
            cmocka_unit_test(test_size_limits),
            cmocka_unit_test(test_fifo_model),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
