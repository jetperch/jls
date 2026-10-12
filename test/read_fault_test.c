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

/*
 * Sweep a backend I/O failure through the reads and seeks (and, for the
 * paths that write, the writes) of the reader, crash recovery, repair,
 * and copy.  See doc/plans/completed/code_coverage.md, stage 5, and
 * doc/plans/completed/recovery_io_faults.md.
 *
 * By default, the recovery, repair, and copy sweeps sample the fault
 * points to keep the test fast with nearly the same coverage.  Run
 * "read_fault_test --full" to fail every read, seek, and write byte,
 * which takes about a minute, before a release or after changing
 * recovery, repair, or copy.
 *
 * A failure must return an error without crashing or leaking, and must
 * not poison later work: the same reader, or a later open, repair, or
 * copy, must return exactly the fault-free results.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include "jls/backend.h"
#include "jls/copy.h"
#include "jls/crc32c.h"
#include "jls/ec.h"
#include "jls/reader.h"
#include "jls/repair.h"
#include "jls/time.h"
#include "jls/writer.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static const char * SRC_PATH = "read_fault_test_src.jls";
static const char * TST_PATH = "read_fault_test_tmp.jls";
static const char * DST_PATH = "read_fault_test_dst.jls";

#define SAMPLE_COUNT (12000)
#define WINDOW_SIZE (937)
#define UTC_PERIOD (97)

static const struct jls_source_def_s SOURCE_3 = {
        .source_id = 3,
        .name = "source 3",
        .vendor = "vendor",
        .model = "model",
        .version = "1.0.0",
        .serial_number = "0001",
};

static const struct jls_signal_def_s SIGNAL_5 = {
        .signal_id = 5,
        .source_id = 3,
        .signal_type = JLS_SIGNAL_TYPE_FSR,
        .data_type = JLS_DATATYPE_F32,
        .sample_rate = 100000,
        .samples_per_data = 1000,
        .sample_decimate_factor = 100,
        .entries_per_summary = 200,
        .summary_decimate_factor = 10,
        .annotation_decimate_factor = 100,
        .utc_decimate_factor = 100,
        .name = "current",
        .units = "A",
};

static const struct jls_signal_def_s SIGNAL_6 = {
        .signal_id = 6,
        .source_id = 3,
        .signal_type = JLS_SIGNAL_TYPE_FSR,
        .data_type = JLS_DATATYPE_U1,
        .sample_rate = 100000,
        .samples_per_data = 1024,
        .sample_decimate_factor = 256,
        .entries_per_summary = 20,
        .summary_decimate_factor = 10,
        .annotation_decimate_factor = 100,
        .utc_decimate_factor = 100,
        .name = "gpi",
        .units = "",
};

// The fault point strides: sampled by default, 1 with --full.
static int64_t read_step_ = 5;          // reads and seeks
static int64_t write_step_ = 37;        // write bytes: recovery and repair
static int64_t copy_write_step_ = 97;   // write bytes: copy

enum fault_e {
    FAULT_READ,     // jls_bk_fread and jls_bk_fseek calls
    FAULT_WRITE,    // jls_bk_fwrite bytes
};

static void limit_set(enum fault_e fault, int64_t limit) {
    if (fault == FAULT_READ) {
        jls_bk_read_limit_set(limit);
    } else {
        jls_bk_write_limit_set(limit);
    }
}

static int64_t limit_total(enum fault_e fault) {
    return (fault == FAULT_READ) ? jls_bk_read_total() : jls_bk_write_total();
}

// Write the test file, closed or abandoned as in a writer crash.
static void gen(const char * path, int close) {
    struct jls_wr_s * wr = NULL;
    float * f32 = malloc(sizeof(float) * SAMPLE_COUNT);
    uint8_t * u1 = calloc(1, SAMPLE_COUNT / 8 + 1);
    assert_non_null(f32);
    assert_non_null(u1);
    for (int64_t i = 0; i < SAMPLE_COUNT; ++i) {
        f32[i] = (float) ((i * 7919) % 1000) * 0.001f;
        if ((i / 37) & 1) {
            u1[i >> 3] |= (uint8_t) (1 << (i & 7));
        }
    }
    assert_int_equal(0, jls_wr_open(&wr, path));
    assert_int_equal(0, jls_wr_source_def(wr, &SOURCE_3));
    assert_int_equal(0, jls_wr_signal_def(wr, &SIGNAL_5));
    assert_int_equal(0, jls_wr_signal_def(wr, &SIGNAL_6));
    assert_int_equal(0, jls_wr_user_data(wr, 0x21, JLS_STORAGE_TYPE_STRING, (const uint8_t *) "hello", 6));
    for (int64_t s = 0; s < SAMPLE_COUNT; s += WINDOW_SIZE) {
        uint32_t len = (uint32_t) (((SAMPLE_COUNT - s) > WINDOW_SIZE) ? WINDOW_SIZE : (SAMPLE_COUNT - s));
        assert_int_equal(0, jls_wr_fsr_f32(wr, 5, s, f32 + s, len));
        for (int64_t k = s; k < (s + len); ++k) {
            if (0 == (k % UTC_PERIOD)) {
                assert_int_equal(0, jls_wr_utc(wr, 5, k, JLS_TIME_YEAR + JLS_COUNTER_TO_TIME(k, 100000)));
            }
            if (0 == (k % 5000)) {
                assert_int_equal(0, jls_wr_annotation(wr, 5, k, 0.0f, JLS_ANNOTATION_TYPE_TEXT, 0,
                                                      JLS_STORAGE_TYPE_STRING, (const uint8_t *) "marker", 7));
            }
        }
    }
    assert_int_equal(0, jls_wr_fsr(wr, 6, 0, u1, SAMPLE_COUNT));
    assert_int_equal(0, jls_wr_user_data(wr, 0x22, JLS_STORAGE_TYPE_BINARY, (const uint8_t *) "\x01\x02", 2));
    if (close) {
        assert_int_equal(0, jls_wr_close(wr));
    } else {
        jls_bk_write_limit_set(0);  // abandon: close-time writes fail
        (void) jls_wr_close(wr);
        jls_bk_write_limit_set(-1);
    }
    free(f32);
    free(u1);
}

static void file_copy(const char * src, const char * dst) {
    FILE * fs = fopen(src, "rb");
    FILE * fd = fopen(dst, "wb");
    assert_non_null(fs);
    assert_non_null(fd);
    uint8_t buf[4096];
    size_t sz;
    while ((sz = fread(buf, 1, sizeof(buf), fs)) > 0) {
        assert_int_equal(sz, fwrite(buf, 1, sz, fd));
    }
    fclose(fs);
    fclose(fd);
}

// Everything the workload reads, reduced to lengths, counts, and CRCs.
struct result_s {
    int64_t length[2];
    uint32_t crc_f32;
    uint32_t crc_u1;
    uint32_t crc_stats;
    int64_t timestamp;
    int64_t sample_id;
    uint32_t crc_anno;
    uint32_t crc_utc;
    uint32_t crc_user;
};

static uint32_t crc_add(uint32_t crc, const void * data, size_t size) {
    return jls_crc32c(data, (uint32_t) size) ^ (crc * 31);
}

static int32_t on_anno(void * user_data, const struct jls_annotation_s * a) {
    uint32_t * crc = (uint32_t *) user_data;
    *crc = crc_add(*crc, &a->timestamp, sizeof(a->timestamp));
    *crc = crc_add(*crc, a->data, a->data_size);
    return 0;
}

static int32_t on_utc(void * user_data, const struct jls_utc_summary_entry_s * utc, uint32_t size) {
    uint32_t * crc = (uint32_t *) user_data;
    *crc = crc_add(*crc, utc, sizeof(*utc) * size);
    return 0;
}

static int32_t on_user(void * user_data, uint16_t chunk_meta, enum jls_storage_type_e storage_type,
                       uint8_t * data, uint32_t data_size) {
    uint32_t * crc = (uint32_t *) user_data;
    (void) storage_type;
    *crc = crc_add(*crc, &chunk_meta, sizeof(chunk_meta));
    *crc = crc_add(*crc, data, data_size);
    return 0;
}

static int workload_line_;  // the failing workload line, for diagnostics
#define RC(x) do { int32_t rc__ = (x); if (rc__) { workload_line_ = __LINE__; return rc__; } } while (0)

static int32_t workload(struct jls_rd_s * rd, struct result_s * r) {
    static float f32[SAMPLE_COUNT];
    static uint8_t u1[SAMPLE_COUNT / 8 + 1];
    double stats[32 * JLS_SUMMARY_FSR_COUNT];
    memset(r, 0, sizeof(*r));
    RC(jls_rd_fsr_length(rd, 5, &r->length[0]));
    RC(jls_rd_fsr_length(rd, 6, &r->length[1]));
    RC(jls_rd_fsr_f32(rd, 5, 0, f32, r->length[0]));
    r->crc_f32 = crc_add(0, f32, sizeof(float) * (size_t) r->length[0]);
    RC(jls_rd_fsr(rd, 6, 3, u1, r->length[1] - 3));  // unaligned
    r->crc_u1 = crc_add(0, u1, (size_t) (r->length[1] - 3) / 8);
    RC(jls_rd_fsr_statistics(rd, 5, 0, r->length[0], stats, 1));            // level 2
    r->crc_stats = crc_add(r->crc_stats, stats, sizeof(double) * JLS_SUMMARY_FSR_COUNT);
    // windows sized from the length: a recovered file is shorter
    RC(jls_rd_fsr_statistics(rd, 5, 150, (r->length[0] - 150) / 10, stats, 10));     // level 1
    r->crc_stats = crc_add(r->crc_stats, stats, sizeof(double) * JLS_SUMMARY_FSR_COUNT * 10);
    RC(jls_rd_fsr_statistics(rd, 5, 1234, 17, stats, 32));                          // samples
    r->crc_stats = crc_add(r->crc_stats, stats, sizeof(stats));
    RC(jls_rd_fsr_statistics(rd, 6, 0, r->length[1] / 10, stats, 10));              // u1 summaries
    r->crc_stats = crc_add(r->crc_stats, stats, sizeof(double) * JLS_SUMMARY_FSR_COUNT * 10);
    RC(jls_rd_annotations(rd, 5, 0, on_anno, &r->crc_anno));
    RC(jls_rd_utc(rd, 5, 0, on_utc, &r->crc_utc));
    RC(jls_rd_user_data(rd, on_user, &r->crc_user));
    RC(jls_rd_sample_id_to_timestamp(rd, 5, 20000, &r->timestamp));
    RC(jls_rd_timestamp_to_sample_id(rd, 5, r->timestamp + JLS_TIME_MILLISECOND, &r->sample_id));
    return 0;
}

static void open_workload(const char * path, struct result_s * r) {
    struct jls_rd_s * rd = NULL;
    assert_int_equal(0, jls_rd_open(&rd, path));
    int32_t rc = workload(rd, r);
    if (rc) {
        print_error("workload line %d: rc %d\n", workload_line_, rc);
        fail();
    }
    jls_rd_close(rd);
}

static void result_check(const struct result_s * expect, const struct result_s * actual, int64_t limit) {
    if (memcmp(expect, actual, sizeof(*expect))) {
        print_error("results differ after a fault at limit %" PRIi64 "\n", limit);
        fail();
    }
}

// The reader: fail each read or seek of open and the workload.
static void test_reader(void ** state) {
    (void) state;
    struct result_s ref;
    struct result_s r;
    struct jls_rd_s * rd = NULL;
    gen(SRC_PATH, 1);
    int64_t t0 = jls_bk_read_total();
    assert_int_equal(0, jls_rd_open(&rd, SRC_PATH));
    assert_int_equal(0, workload(rd, &ref));
    int64_t ops = jls_bk_read_total() - t0;
    jls_rd_close(rd);
    assert_int_equal(SAMPLE_COUNT, ref.length[0]);

    for (int64_t limit = 0; limit < ops; ++limit) {
        rd = NULL;
        jls_bk_read_limit_set(limit);
        int32_t rc = jls_rd_open(&rd, SRC_PATH);
        if (rc) {
            jls_bk_read_limit_set(-1);
            continue;
        }
        rc = workload(rd, &r);
        jls_bk_read_limit_set(-1);
        if (0 == rc) {
            print_error("limit %" PRIi64 " of %" PRIi64 ": workload did not fail\n", limit, ops);
            fail();
        }
        // the same reader recovers once I/O works again
        rc = workload(rd, &r);
        if (rc) {
            print_error("limit %" PRIi64 ": retry workload line %d: rc %d\n", limit, workload_line_, rc);
            fail();
        }
        result_check(&ref, &r, limit);
        jls_rd_close(rd);
    }
    remove(SRC_PATH);
}

/*
 * After a fault, the file must read exactly as without the fault: either
 * the next open (or repair then open) succeeds, or it fails and
 * jls_repair() restores the file.  Data loss is never allowed.  Recovery
 * stops on an I/O error, so the file is left as after a crash at that
 * point, and a crash during recovery can need jls_repair(), see
 * doc/plans/completed/recovery_io_faults.md.
 */
struct sweep_s {
    int64_t faults;
    int64_t repaired;       // needed jls_repair() to open
};

static int32_t open_and_read(const char * path, struct result_s * r) {
    struct jls_rd_s * rd = NULL;
    int32_t rc = jls_rd_open(&rd, path);
    if (0 == rc) {
        rc = workload(rd, r);
        jls_rd_close(rd);
    }
    return rc;
}

static void sweep_check(struct sweep_s * sw, int32_t rc, const char * path,
                        const struct result_s * ref, int64_t limit) {
    struct result_s r;
    ++sw->faults;
    if (0 == rc) {
        rc = open_and_read(path, &r);
    }
    if (rc) {
        ++sw->repaired;
        rc = jls_repair(path, NULL, NULL, NULL, NULL);
        if (0 == rc) {
            rc = open_and_read(path, &r);
        }
    }
    if (rc) {
        print_error("limit %" PRIi64 ": unreadable after repair, rc %d\n", limit, rc);
        fail();
    }
    result_check(ref, &r, limit);
}

static void sweep_report(const char * name, const struct sweep_s * sw) {
    print_message("%s: %" PRIi64 " faults, %" PRIi64 " needed jls_repair()\n",
                  name, sw->faults, sw->repaired);
}

// Crash recovery on open: fail each I/O, then a fault-free open must
// complete the recovery with the same results.
static void recovery_sweep(const char * name, enum fault_e fault, int64_t step) {
    struct result_s ref;
    struct sweep_s sw = {0, 0};
    struct jls_rd_s * rd = NULL;
    gen(SRC_PATH, 0);
    file_copy(SRC_PATH, TST_PATH);
    int64_t t0 = limit_total(fault);
    assert_int_equal(0, jls_rd_open(&rd, TST_PATH));
    int64_t ops = limit_total(fault) - t0;
    jls_rd_close(rd);
    open_workload(TST_PATH, &ref);
    assert_true(ref.length[0] > (SAMPLE_COUNT * 5 / 6));  // recovery keeps most data
    assert_true(ref.length[1] > (SAMPLE_COUNT * 5 / 6));

    for (int64_t limit = 0; limit < ops; limit += step) {
        file_copy(SRC_PATH, TST_PATH);
        limit_set(fault, limit);
        int32_t rc = jls_rd_open(&rd, TST_PATH);
        limit_set(fault, -1);
        if (0 == rc) {
            jls_rd_close(rd);
        }
        sweep_check(&sw, 0, TST_PATH, &ref, limit);
    }
    sweep_report(name, &sw);
    remove(TST_PATH);
    remove(SRC_PATH);
}

static void test_recovery_read(void ** state) {
    (void) state;
    recovery_sweep("recovery read", FAULT_READ, read_step_);
}

static void test_recovery_write(void ** state) {
    (void) state;
    recovery_sweep("recovery write", FAULT_WRITE, write_step_);
}

// Offline repair: fail each I/O, then a fault-free repair must produce
// the same results.
static void repair_sweep(const char * name, enum fault_e fault, int64_t step) {
    struct result_s ref;
    struct sweep_s sw = {0, 0};
    gen(SRC_PATH, 0);
    file_copy(SRC_PATH, TST_PATH);
    int64_t t0 = limit_total(fault);
    assert_int_equal(0, jls_repair(TST_PATH, NULL, NULL, NULL, NULL));
    int64_t ops = limit_total(fault) - t0;
    open_workload(TST_PATH, &ref);
    assert_true(ref.length[0] > (SAMPLE_COUNT * 5 / 6));  // recovery keeps most data
    assert_true(ref.length[1] > (SAMPLE_COUNT * 5 / 6));

    for (int64_t limit = 0; limit < ops; limit += step) {
        file_copy(SRC_PATH, TST_PATH);
        limit_set(fault, limit);
        (void) jls_repair(TST_PATH, NULL, NULL, NULL, NULL);
        limit_set(fault, -1);
        int32_t rc = jls_repair(TST_PATH, NULL, NULL, NULL, NULL);
        sweep_check(&sw, rc, TST_PATH, &ref, limit);
    }
    sweep_report(name, &sw);
    remove(TST_PATH);
    remove(SRC_PATH);
}

static void test_repair_read(void ** state) {
    (void) state;
    repair_sweep("repair read", FAULT_READ, read_step_);
}

static void test_repair_write(void ** state) {
    (void) state;
    repair_sweep("repair write", FAULT_WRITE, write_step_);
}

// Copy: fail each I/O.  A copy that reports success must be complete.
static void copy_sweep(enum fault_e fault, int64_t step) {
    struct result_s ref;
    struct result_s r;
    gen(SRC_PATH, 1);
    open_workload(SRC_PATH, &ref);
    int64_t t0 = limit_total(fault);
    assert_int_equal(0, jls_copy(SRC_PATH, DST_PATH, NULL, NULL, NULL, NULL));
    int64_t ops = limit_total(fault) - t0;
    open_workload(DST_PATH, &r);
    result_check(&ref, &r, -1);

    for (int64_t limit = 0; limit < ops; limit += step) {
        remove(DST_PATH);
        limit_set(fault, limit);
        int32_t rc = jls_copy(SRC_PATH, DST_PATH, NULL, NULL, NULL, NULL);
        limit_set(fault, -1);
        if (0 == rc) {
            open_workload(DST_PATH, &r);
            result_check(&ref, &r, limit);
        }
    }
    open_workload(SRC_PATH, &r);  // the source is untouched
    result_check(&ref, &r, -1);
    remove(DST_PATH);
    remove(SRC_PATH);
}

static void test_copy_read(void ** state) {
    (void) state;
    copy_sweep(FAULT_READ, read_step_);
}

static void test_copy_write(void ** state) {
    (void) state;
    copy_sweep(FAULT_WRITE, copy_write_step_);
}

int main(int argc, char * argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (0 == strcmp(argv[i], "--full")) {
            read_step_ = 1;
            write_step_ = 1;
            copy_write_step_ = 1;
        } else {
            printf("usage: read_fault_test [--full]\n"
                   "  --full  Fail every read, seek, and write byte (slow).\n");
            return 1;
        }
    }
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_reader),
            cmocka_unit_test(test_recovery_read),
            cmocka_unit_test(test_recovery_write),
            cmocka_unit_test(test_repair_read),
            cmocka_unit_test(test_repair_write),
            cmocka_unit_test(test_copy_read),
            cmocka_unit_test(test_copy_write),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
