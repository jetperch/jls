/*
 * Copyright 2023-2026 Jetperch LLC
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
#include "jls/writer.h"
#include "jls/raw.h"
#include "jls/backend.h"
#include "jls/core.h"
#include "jls/format.h"
#include "jls/ec.h"
#include "jls/time.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "jls/log.h"

#if defined(_WIN32)
#include <direct.h>
#else
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif


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
            "usage: jls read_fuzzer [--<arg> <value>] [<path>]\n"
            "Perform read and file recovery fuzz testing on JLS files.\n"
            "\n"
            "Read parameter fuzz mode (default):\n"
            "  path          The path to the valid JLS file to read\n"
            "  --random      The 64-bit random number seed\n"
            "  --max-length  The maximum FSR read length in entries\n"
            "\n"
            "Corruption fuzz mode:\n"
            "  --corrupt <mode>   Corruption mode, one of:\n"
            "                     truncate, zero_tail, zero_hole, torn_update, bitflip\n"
            "  --random <seed>    The 64-bit random number seed\n"
            "  --iterations <n>   The number of cases, 0 for unlimited [1000]\n"
            "  --workdir <dir>    The working directory [read_fuzzer_work]\n"
            "  --cfg <id>         Force the file generation config id\n"
            "  --offset <n>       Force the corruption byte offset\n"
            "  --timeout <s>      Per-case timeout in seconds [120]\n"
            "  --no-isolate       Verify in-process (debug); default forks a\n"
            "                     child per case to catch crashes and hangs\n"
            "  --keep             Keep failing case files for repro\n"
            "  --verbose          Print each case, not just failures\n"
            "\n"
            "In corruption mode, each case generates (or reuses) a pristine\n"
            "file, applies a seeded corruption to a copy, then verifies\n"
            "recovery: open, signal defs, sample data vs reference,\n"
            "statistics, annotations, UTC, user data, and idempotent reopen.\n"
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


/****************************************************************************/
/* Corruption fuzz                                                          */
/****************************************************************************/

#define FZ_WINDOW               (937)
#define FZ_SIGNAL_ID_F32        (5)
#define FZ_SIGNAL_ID_SEC        (9)
#define FZ_CFG_COUNT            (48)
#define FZ_CHUNK_HDR_SZ         ((int64_t) sizeof(struct jls_chunk_header_s))
#define FZ_READ_BATCH           (65536)  // samples, keep multiple of 8 for u1
#define FZ_STATS_LEN_MAX        (64)
#define FZ_DETAIL_SZ            (256)

enum fz_fail_e {
    FZ_FAIL_OPEN = 0,       // open failed although data region was intact
    FZ_FAIL_ERROR_CODE,     // returned error code outside the enum range
    FZ_FAIL_SIGNAL,         // defined signal missing after recovery
    FZ_FAIL_LENGTH_OVER,    // recovered more samples than were written
    FZ_FAIL_EXCESS_LOSS,    // recovery discarded fully-written samples
    FZ_FAIL_DATA,           // sample data read error or value mismatch
    FZ_FAIL_STATS_ERROR,    // jls_rd_fsr_statistics returned an error
    FZ_FAIL_STATS_VALUE,    // statistics NaN or mismatch vs reference
    FZ_FAIL_ANN_ERROR,      // annotation iteration error
    FZ_FAIL_ANN_COUNT,      // annotation count out of expected bounds
    FZ_FAIL_UTC_ERROR,      // UTC iteration error
    FZ_FAIL_UTC_COUNT,      // UTC count out of expected bounds
    FZ_FAIL_UD_ERROR,       // user data iteration error
    FZ_FAIL_UD_COUNT,       // user data count out of expected bounds
    FZ_FAIL_REOPEN,         // second open of the repaired file failed
    FZ_FAIL_IDEMPOTENT,     // second open modified the file or length
    FZ_FAIL_RDONLY,         // read-only open misbehaved
    FZ_FAIL_GENERATE,       // could not generate/scan the pristine file
    FZ_FAIL_CRASH,          // the verify child process crashed
    FZ_FAIL_HANG,           // the verify child process timed out
    FZ_FAIL_COUNT
};

static const char * FZ_FAIL_NAMES[] = {
    "open_fail", "bad_error_code", "signal_missing", "length_over",
    "excess_loss", "data_mismatch", "stats_error", "stats_value",
    "ann_error", "ann_count", "utc_error", "utc_count",
    "user_data_error", "user_data_count", "reopen_fail", "not_idempotent",
    "readonly", "generate", "crash", "hang",
};

enum fz_mode_e {
    FZ_MODE_TRUNCATE,       // cut the file at a byte offset
    FZ_MODE_ZERO_TAIL,      // keep length, zero all bytes from offset
    FZ_MODE_ZERO_HOLE,      // zero an interior region (writeback reorder)
    FZ_MODE_TORN_UPDATE,    // corrupt an in-place-updated metadata chunk
                            // payload, then truncate the tail
    FZ_MODE_BITFLIP,        // flip random bits; graceful errors only
};

static const char * FZ_MODE_NAMES[] = {
    "truncate", "zero_tail", "zero_hole", "torn_update", "bitflip",
};
#define FZ_MODE_COUNT (5)

struct fz_chunk_s {
    int64_t offset;
    int64_t end_offset;     // offset of the next chunk (header + payload + pad + crc)
    int64_t sample_end;     // FSR data chunks only: timestamp + entry_count
    uint8_t tag;
    uint16_t chunk_meta;
    uint32_t payload_length;
};

struct fz_scan_s {
    struct fz_chunk_s * chunks;
    size_t length;
    size_t alloc;
    int64_t fend;
    int64_t defs_end;       // end of the initial defs region; damage below
                            // this offset makes open failure acceptable
};

struct fz_cfg_s {
    uint32_t id;
    int64_t samples;        // primary f32 signal sample count
    uint32_t secondary;     // 0=none, 1=u1, 2=i16, 3=u8
    uint32_t closed;        // 1=closed, 0=crash (no jls_wr_close)
    uint32_t ts_en;         // annotations + UTC + user data enabled
    uint32_t sec_datatype;
    uint32_t sec_entry_bits;
    int64_t sec_samples;
};

struct fz_s {
    enum fz_mode_e mode;
    char workdir[512];
    uint32_t keep;
    uint32_t verbose;
    uint32_t isolate;       // fork a child per case (POSIX only)
    uint32_t timeout;       // per-case timeout in seconds
    uint64_t iterations;
    int32_t cfg_forced;     // -1 for random
    int64_t offset_forced;  // -1 for random
    float * ref_f32;
    uint8_t * ref_sec;
    uint8_t * rd_buf;
    size_t rd_buf_sz;
    double * stats_buf;
    struct fz_scan_s scans[FZ_CFG_COUNT];
    uint8_t generated[FZ_CFG_COUNT];
    uint64_t fails[FZ_FAIL_COUNT];
    uint64_t case_count;
    uint64_t fail_count;
};

static const int64_t FZ_SIZES[] = {FZ_WINDOW * 8, FZ_WINDOW * 120, FZ_WINDOW * 1000};

static const struct jls_source_def_s FZ_SOURCE = {
        .source_id = 3,
        .name = "fuzz source",
        .vendor = "jetperch",
        .model = "read_fuzzer",
        .version = "1",
        .serial_number = "000001",
};

static const struct jls_signal_def_s FZ_SIGNAL_F32 = {
        .signal_id = FZ_SIGNAL_ID_F32,
        .source_id = 3,
        .signal_type = JLS_SIGNAL_TYPE_FSR,
        .data_type = JLS_DATATYPE_F32,
        .sample_rate = 100000,
        .samples_per_data = 1000,
        .sample_decimate_factor = 100,
        .entries_per_summary = 200,
        .summary_decimate_factor = 100,
        .annotation_decimate_factor = 100,
        .utc_decimate_factor = 100,
        .name = "current",
        .units = "A",
};

static void fz_cfg_init(struct fz_cfg_s * cfg, uint32_t id) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->id = id;
    cfg->samples = FZ_SIZES[id % 3];
    cfg->secondary = (id / 3) % 4;
    cfg->closed = (id / 12) % 2;
    cfg->ts_en = (id / 24) % 2;
    switch (cfg->secondary) {
        case 1: cfg->sec_datatype = JLS_DATATYPE_U1;  cfg->sec_entry_bits = 1;  break;
        case 2: cfg->sec_datatype = JLS_DATATYPE_I16; cfg->sec_entry_bits = 16; break;
        case 3: cfg->sec_datatype = JLS_DATATYPE_U8;  cfg->sec_entry_bits = 8;  break;
        default: break;
    }
    cfg->sec_samples = cfg->secondary ? ((cfg->samples / 1024) * 1024) : 0;
}

static void fz_ref_fill(struct fz_s * fz, const struct fz_cfg_s * cfg) {
    // triangle wave, matches test/repair_test.c gen_triangle(1000, ...)
    const uint32_t period = 1000;
    int64_t v_max = (period + 1) / 2;
    float f_offset = v_max / 2.0f;
    float gain = 2.0f / v_max;
    int64_t v = v_max / 2;
    int64_t incr = 1;
    for (int64_t i = 0; i < cfg->samples; ++i) {
        fz->ref_f32[i] = gain * (v - f_offset);
        if (v <= 0) {
            incr = 1;
        } else if (v >= v_max) {
            incr = -1;
        }
        v += incr;
    }
    if (cfg->secondary) {  // splitmix64, deterministic per config
        uint64_t s = 0x9E3779B97F4A7C15ULL ^ cfg->id;
        size_t sz = (size_t) ((cfg->sec_samples * cfg->sec_entry_bits) / 8);
        for (size_t i = 0; i < sz; ++i) {
            s += 0x9E3779B97F4A7C15ULL;
            uint64_t z = s;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
            fz->ref_sec[i] = (uint8_t) (z ^ (z >> 31));
        }
    }
}

static int32_t fz_generate(struct fz_s * fz, const struct fz_cfg_s * cfg, const char * path) {
    struct jls_wr_s * wr = NULL;
    char buf[128];
    remove(path);
    ROE(jls_wr_open(&wr, path));
    ROE(jls_wr_source_def(wr, &FZ_SOURCE));
    ROE(jls_wr_signal_def(wr, &FZ_SIGNAL_F32));
    if (cfg->secondary) {
        struct jls_signal_def_s sec = FZ_SIGNAL_F32;
        sec.signal_id = FZ_SIGNAL_ID_SEC;
        sec.data_type = cfg->sec_datatype;
        sec.name = "aux";
        sec.units = "";
        ROE(jls_wr_signal_def(wr, &sec));
    }

    int64_t utc = JLS_TIME_YEAR;
    int64_t sec_pos = 0;
    int64_t w = 0;
    for (int64_t sample_id = 0; sample_id < cfg->samples; sample_id += FZ_WINDOW, ++w) {
        ROE(jls_wr_fsr_f32(wr, FZ_SIGNAL_ID_F32, sample_id, fz->ref_f32 + sample_id, FZ_WINDOW));
        if (cfg->ts_en) {
            ROE(jls_wr_utc(wr, FZ_SIGNAL_ID_F32, sample_id,
                           utc + JLS_COUNTER_TO_TIME(sample_id, FZ_SIGNAL_F32.sample_rate)));
            if ((w % 10) == 0) {
                snprintf(buf, sizeof(buf), "annotation %" PRIi64, w);
                ROE(jls_wr_annotation(wr, FZ_SIGNAL_ID_F32, sample_id, 1.0f,
                                      JLS_ANNOTATION_TYPE_TEXT, 0, JLS_STORAGE_TYPE_STRING,
                                      (const uint8_t *) buf, 0));
            }
            if ((w % 25) == 0) {
                snprintf(buf, sizeof(buf), "{\"window\": %" PRIi64 "}", w);
                ROE(jls_wr_user_data(wr, (uint16_t) (0x100 + (w / 25)), JLS_STORAGE_TYPE_JSON,
                                     (const uint8_t *) buf, 0));
            }
        }
        int64_t sec_limit = sample_id + FZ_WINDOW;
        if (sec_limit > cfg->sec_samples) {
            sec_limit = cfg->sec_samples;
        }
        while ((sec_pos + 1024) <= sec_limit) {
            const uint8_t * p = fz->ref_sec + ((sec_pos * cfg->sec_entry_bits) / 8);
            ROE(jls_wr_fsr(wr, FZ_SIGNAL_ID_SEC, sec_pos, p, 1024));
            sec_pos += 1024;
        }
    }

    if (cfg->closed) {
        ROE(jls_wr_close(wr));
    } else {
        // simulate a crashed writer process: close the file descriptor
        // without flushing writer state.  The writer instance leaks
        // intentionally; jls_wr_close would write the missing chunks.
        struct jls_core_s * core = (struct jls_core_s *) wr;
        jls_bk_fclose(jls_raw_backend(core->raw));
    }
    return 0;
}

static int64_t fz_psod(uint32_t payload_length) {
    // payload size on disk: payload + pad-to-8 + crc32, see src/raw.c
    if (!payload_length) {
        return 0;
    }
    uint32_t pad = (payload_length + 4) & 7;
    if (pad) {
        pad = 8 - pad;
    }
    return (int64_t) payload_length + pad + 4;
}

static bool fz_tag_is_track_data(uint8_t tag) {
    return (0 != (tag & JLS_TRACK_TAG_FLAG)) && ((tag & 7) == JLS_TRACK_CHUNK_DATA);
}

static void fz_scan_free(struct fz_scan_s * scan) {
    if (scan->chunks) {
        free(scan->chunks);
        scan->chunks = NULL;
    }
    scan->length = 0;
    scan->alloc = 0;
}

static int32_t fz_scan_push(struct fz_scan_s * scan, const struct fz_chunk_s * c) {
    if (scan->length >= scan->alloc) {
        size_t alloc = scan->alloc ? (scan->alloc * 2) : 1024;
        struct fz_chunk_s * chunks = realloc(scan->chunks, alloc * sizeof(*chunks));
        if (!chunks) {
            return JLS_ERROR_NOT_ENOUGH_MEMORY;
        }
        scan->chunks = chunks;
        scan->alloc = alloc;
    }
    scan->chunks[scan->length++] = *c;
    return 0;
}

static int32_t fz_scan(const char * path, struct fz_scan_s * scan) {
    struct jls_raw_s * raw = NULL;
    fz_scan_free(scan);
    int32_t rc = jls_raw_open(&raw, path, "r");
    if (rc && (rc != JLS_ERROR_TRUNCATED)) {
        return rc;
    }
    scan->fend = jls_raw_backend(raw)->fend;
    uint8_t * payload = malloc(1 << 20);
    if (!payload) {
        jls_raw_close(raw);
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }

    while (1) {
        struct jls_chunk_header_s hdr;
        int64_t offset = jls_raw_chunk_tell(raw);
        rc = jls_raw_rd_header(raw, &hdr);
        if (rc) {
            rc = 0;  // end of walk
            break;
        }
        struct fz_chunk_s c = {
                .offset = offset,
                .end_offset = offset + FZ_CHUNK_HDR_SZ + fz_psod(hdr.payload_length),
                .sample_end = 0,
                .tag = hdr.tag,
                .chunk_meta = hdr.chunk_meta,
                .payload_length = hdr.payload_length,
        };
        if ((hdr.tag == JLS_TAG_TRACK_FSR_DATA) && hdr.payload_length) {
            rc = jls_raw_rd_payload(raw, 1 << 20, payload);
            if (rc) {
                break;
            }
            struct jls_payload_header_s * ph = (struct jls_payload_header_s *) payload;
            c.sample_end = ph->timestamp + ph->entry_count;
        } else if (jls_raw_chunk_next(raw)) {
            rc = fz_scan_push(scan, &c);
            break;
        }
        rc = fz_scan_push(scan, &c);
        if (rc) {
            break;
        }
    }
    free(payload);
    jls_raw_close(raw);
    if (rc) {
        return rc;
    }

    scan->defs_end = 0;
    for (size_t i = 0; i < scan->length; ++i) {
        struct fz_chunk_s * c = &scan->chunks[i];
        bool is_payload_chunk = fz_tag_is_track_data(c->tag)
                || ((c->tag == JLS_TAG_USER_DATA) && (c->payload_length > 0));
        if (is_payload_chunk) {
            break;
        }
        scan->defs_end = c->end_offset;
    }
    return 0;
}

// The maximum sample count fully stored at or below trunc_offset.
static int64_t fz_scan_sample_min(const struct fz_scan_s * scan, uint16_t signal_id,
                                  int64_t trunc_offset) {
    int64_t sample_end = 0;
    for (size_t i = 0; i < scan->length; ++i) {
        const struct fz_chunk_s * c = &scan->chunks[i];
        if ((c->tag == JLS_TAG_TRACK_FSR_DATA) && ((c->chunk_meta & 0x0fff) == signal_id)
                && (c->end_offset <= trunc_offset) && (c->sample_end > sample_end)) {
            sample_end = c->sample_end;
        }
    }
    return sample_end;
}

static void fz_scan_ts_counts(const struct fz_scan_s * scan, int64_t trunc_offset,
                              int64_t * counts_min, int64_t * counts_total) {
    // index 0: annotations, 1: UTC, 2: user data
    for (int i = 0; i < 3; ++i) {
        counts_min[i] = 0;
        counts_total[i] = 0;
    }
    for (size_t i = 0; i < scan->length; ++i) {
        const struct fz_chunk_s * c = &scan->chunks[i];
        if (0 == c->payload_length) {
            continue;
        }
        int idx;
        if (c->tag == JLS_TAG_TRACK_ANNOTATION_DATA) {
            idx = 0;
        } else if (c->tag == JLS_TAG_TRACK_UTC_DATA) {
            idx = 1;
        } else if (c->tag == JLS_TAG_USER_DATA) {
            idx = 2;
        } else {
            continue;
        }
        ++counts_total[idx];
        if (c->end_offset <= trunc_offset) {
            ++counts_min[idx];
        }
    }
}

static uint64_t fz_file_hash(const char * path) {
    FILE * f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    uint64_t h = 1469598103934665603ULL;  // FNV-1a
    uint8_t buf[65536];
    size_t sz;
    while ((sz = fread(buf, 1, sizeof(buf), f)) > 0) {
        for (size_t i = 0; i < sz; ++i) {
            h ^= buf[i];
            h *= 1099511628211ULL;
        }
    }
    fclose(f);
    return h;
}

static int32_t fz_copy_truncate(const char * src, const char * dst, int64_t length) {
    uint8_t buf[65536];
    FILE * fs = fopen(src, "rb");
    if (!fs) {
        return JLS_ERROR_IO;
    }
    remove(dst);
    FILE * fd = fopen(dst, "wb");
    if (!fd) {
        fclose(fs);
        return JLS_ERROR_IO;
    }
    int32_t rc = 0;
    int64_t remaining = length;
    while (remaining > 0) {
        size_t req = sizeof(buf);
        if ((int64_t) req > remaining) {
            req = (size_t) remaining;
        }
        size_t sz = fread(buf, 1, req, fs);
        if (0 == sz) {
            break;  // source shorter than requested length
        }
        if (sz != fwrite(buf, 1, sz, fd)) {
            rc = JLS_ERROR_IO;
            break;
        }
        remaining -= (int64_t) sz;
    }
    fclose(fs);
    fclose(fd);
    return rc;
}

static void fz_ref_stats(const float * ref, int64_t start, int64_t count,
                         double * mean, double * v_min, double * v_max) {
    double m = 0.0;
    double mn = ref[start];
    double mx = ref[start];
    for (int64_t i = start; i < (start + count); ++i) {
        double v = ref[i];
        m += v;
        if (v < mn) {
            mn = v;
        }
        if (v > mx) {
            mx = v;
        }
    }
    *mean = m / count;
    *v_min = mn;
    *v_max = mx;
}

struct fz_count_s {
    int64_t count;
};

static int32_t fz_ann_cbk(void * user_data, const struct jls_annotation_s * annotation) {
    (void) annotation;
    ((struct fz_count_s *) user_data)->count += 1;
    return 0;
}

static int32_t fz_utc_cbk(void * user_data, const struct jls_utc_summary_entry_s * utc, uint32_t size) {
    (void) utc;
    ((struct fz_count_s *) user_data)->count += size;
    return 0;
}

static int32_t fz_ud_cbk(void * user_data, uint16_t chunk_meta,
                         enum jls_storage_type_e storage_type,
                         uint8_t * data, uint32_t data_size) {
    (void) chunk_meta;
    (void) storage_type;
    (void) data;
    (void) data_size;
    ((struct fz_count_s *) user_data)->count += 1;
    return 0;
}

static bool fz_error_code_valid(int32_t rc) {
    return (rc >= 0) && (rc < JLS_ERROR_CODE_COUNT);
}

#define FZ_FAIL(bit_, ...) do {                                     \
    if (0 == fails) {                                               \
        snprintf(detail, FZ_DETAIL_SZ, __VA_ARGS__);                \
    }                                                               \
    fails |= (1U << (bit_));                                        \
} while (0)

// Verify sample data for one FSR signal against the reference.
// When strict is false, only memory-safety (guard) violations fail.
static uint32_t fz_check_data(struct fz_s * fz, struct jls_rd_s * rd, uint16_t signal_id,
                              uint32_t entry_bits, const uint8_t * ref, int64_t n,
                              bool strict, char * detail) {
    uint32_t fails = 0;
    const uint8_t guard_byte = 0xCC;
    const uint32_t guard_length = 32;
    for (int64_t start = 0; start < n; start += FZ_READ_BATCH) {
        int64_t len = n - start;
        if (len > FZ_READ_BATCH) {
            len = FZ_READ_BATCH;
        }
        size_t len_bytes = (size_t) ((len * entry_bits + 7) / 8);
        memset(fz->rd_buf + len_bytes, guard_byte, guard_length);
        int32_t rc = jls_rd_fsr(rd, signal_id, start, fz->rd_buf, len);
        if (!is_mem_const(fz->rd_buf + len_bytes, guard_length, guard_byte)) {
            FZ_FAIL(FZ_FAIL_DATA, "signal %d read @%" PRIi64 "+%" PRIi64 " guard overwrite",
                    (int) signal_id, start, len);
            return fails;
        }
        if (rc) {
            if (strict) {
                FZ_FAIL(FZ_FAIL_DATA, "signal %d read @%" PRIi64 "+%" PRIi64 " rc=%" PRIi32 " %s",
                        (int) signal_id, start, len, rc, jls_error_code_name(rc));
            }
            return fails;
        }
        size_t cmp_bytes = (size_t) ((len * entry_bits) / 8);  // full bytes only
        if (strict && cmp_bytes
                && memcmp(fz->rd_buf, ref + (size_t) ((start * entry_bits) / 8), cmp_bytes)) {
            FZ_FAIL(FZ_FAIL_DATA, "signal %d data mismatch in [%" PRIi64 ", %" PRIi64 ")",
                    (int) signal_id, start, start + len);
            return fails;
        }
    }
    return fails;
}

// Verify one statistics window against reference data.
//
// When the window boundaries do not align to summary entry boundaries
// (multiples of sample_decimate_factor) and len > 1, the reader
// approximates boundary entries by weighting the whole entry's summary,
// so allow a proportional tolerance for those windows.
static uint32_t fz_check_stats_window(struct fz_s * fz, struct jls_rd_s * rd,
                                      int64_t start, int64_t incr, int64_t len,
                                      uint32_t sdf, int64_t n, char * detail) {
    uint32_t fails = 0;
    bool approx = (incr >= sdf) && (len > 1)
            && (((start % sdf) != 0) || ((incr % sdf) != 0));
    int32_t rc = jls_rd_fsr_statistics(rd, FZ_SIGNAL_ID_F32, start, incr, fz->stats_buf, len);
    if (rc) {
        FZ_FAIL(FZ_FAIL_STATS_ERROR, "stats @%" PRIi64 " incr=%" PRIi64 " len=%" PRIi64
                " rc=%" PRIi32 " %s", start, incr, len, rc, jls_error_code_name(rc));
        return fails;
    }
    for (int64_t e = 0; e < len; ++e) {
        double mean_ref;
        double min_ref;
        double max_ref;
        int64_t e_start = start + e * incr;
        fz_ref_stats(fz->ref_f32, e_start, incr, &mean_ref, &min_ref, &max_ref);
        double * entry = fz->stats_buf + e * JLS_SUMMARY_FSR_COUNT;
        double mean = entry[JLS_SUMMARY_FSR_MEAN];
        double v_min = entry[JLS_SUMMARY_FSR_MIN];
        double v_max = entry[JLS_SUMMARY_FSR_MAX];
        if (isnan(mean) || isnan(v_min) || isnan(v_max)) {
            FZ_FAIL(FZ_FAIL_STATS_VALUE, "stats NaN @%" PRIi64 " incr=%" PRIi64 " entry=%" PRIi64,
                    start, incr, e);
            return fails;
        }
        bool ok;
        if (!approx) {
            double tol = 1e-5 * ((max_ref - min_ref) + fabs(mean_ref)) + 1e-9;
            ok = (fabs(mean - mean_ref) <= tol) && (v_min == min_ref) && (v_max == max_ref);
        } else {
            // widen by one entry on each side for min/max and mean bounds
            int64_t w_start = e_start - sdf;
            if (w_start < 0) {
                w_start = 0;
            }
            int64_t w_end = e_start + incr + sdf;
            if (w_end > n) {
                w_end = n;
            }
            double w_mean;
            double w_min;
            double w_max;
            fz_ref_stats(fz->ref_f32, w_start, w_end - w_start, &w_mean, &w_min, &w_max);
            double tol = (w_max - w_min) * ((2.0 * sdf) / incr) + 1e-6;
            ok = (fabs(mean - mean_ref) <= tol)
                    && (v_min >= (w_min - 1e-9)) && (v_min <= (min_ref + 1e-9))
                    && (v_max <= (w_max + 1e-9)) && (v_max >= (max_ref - 1e-9));
        }
        if (!ok) {
            FZ_FAIL(FZ_FAIL_STATS_VALUE,
                    "stats mismatch%s @%" PRIi64 " incr=%" PRIi64 " entry=%" PRIi64
                    " mean=%g/%g min=%g/%g max=%g/%g",
                    approx ? " (approx)" : "", start, incr, e,
                    mean, mean_ref, v_min, min_ref, v_max, max_ref);
            return fails;
        }
    }
    return fails;
}

// One corruption case applied to a pristine file copy.
struct fz_case_s {
    enum fz_mode_e mode;
    int64_t offset;         // destruction start; data at >= offset is damaged
    int64_t hole_len;       // zero_hole only
    int64_t torn_offset;    // torn_update only: corrupted chunk offset
    bool strict;            // full oracle; false = graceful-error checks only
    bool defs_ok;           // false: damage in defs region, open may fail
};

/**
 * @brief Verify a corrupted JLS file against the reference.
 *
 * @param fz The fuzzer instance with reference data already filled.
 * @param cfg The generation config for this file.
 * @param scan The pristine file scan.
 * @param path The corrupted file path.
 * @param cs The corruption case; data at offset >= cs->offset is damaged.
 * @param detail Output buffer of FZ_DETAIL_SZ for the first failure detail.
 * @return Bitmask of enum fz_fail_e failures, 0 on pass.
 */
static uint32_t fz_oracle(struct fz_s * fz, const struct fz_cfg_s * cfg,
                          const struct fz_scan_s * scan, const char * path,
                          const struct fz_case_s * cs, char * detail) {
    uint32_t fails = 0;
    struct jls_rd_s * rd = NULL;
    detail[0] = 0;

    const int64_t trunc_offset = cs->offset;
    const bool strict = cs->strict;
    bool defs_ok = cs->defs_ok;
    int32_t rc = jls_rd_open(&rd, path);
    if (rc) {
        if (!fz_error_code_valid(rc)) {
            FZ_FAIL(FZ_FAIL_ERROR_CODE, "open rc=%" PRIi32 " out of range", rc);
        } else if (defs_ok && strict) {
            FZ_FAIL(FZ_FAIL_OPEN, "open rc=%" PRIi32 " %s with defs intact",
                    rc, jls_error_code_name(rc));
        } else {
            snprintf(detail, FZ_DETAIL_SZ, "open rc=%" PRIi32 " %s, defs damaged (ok)",
                     rc, jls_error_code_name(rc));
        }
        return fails;
    }

    // signal enumeration
    struct jls_signal_def_s * signals = NULL;
    uint16_t signal_count = 0;
    bool has_f32 = false;
    bool has_sec = false;
    rc = jls_rd_signals(rd, &signals, &signal_count);
    if (rc) {
        FZ_FAIL(FZ_FAIL_SIGNAL, "jls_rd_signals rc=%" PRIi32, rc);
    } else {
        for (uint16_t i = 0; i < signal_count; ++i) {
            if (signals[i].signal_id == FZ_SIGNAL_ID_F32) {
                has_f32 = true;
            }
            if (signals[i].signal_id == FZ_SIGNAL_ID_SEC) {
                has_sec = true;
            }
        }
        if (defs_ok && strict && (!has_f32 || (cfg->secondary && !has_sec))) {
            FZ_FAIL(FZ_FAIL_SIGNAL, "signal missing: f32=%d sec=%d",
                    (int) has_f32, (int) has_sec);
        }
    }

    // primary signal: length, data, statistics
    int64_t n = 0;
    if (has_f32) {
        rc = jls_rd_fsr_length(rd, FZ_SIGNAL_ID_F32, &n);
        if (rc) {
            if (strict) {
                FZ_FAIL(FZ_FAIL_DATA, "fsr_length rc=%" PRIi32, rc);
            }
            n = 0;
        } else if (strict) {
            int64_t n_min = fz_scan_sample_min(scan, FZ_SIGNAL_ID_F32, trunc_offset);
            if (n > cfg->samples) {
                FZ_FAIL(FZ_FAIL_LENGTH_OVER, "n=%" PRIi64 " > written %" PRIi64, n, cfg->samples);
                n = 0;
            } else if (n < n_min) {
                FZ_FAIL(FZ_FAIL_EXCESS_LOSS, "n=%" PRIi64 " < intact %" PRIi64, n, n_min);
            }
        } else if (n > cfg->samples) {
            n = cfg->samples;  // lenient: bound reads to the reference size
        }
        if (n > 0) {
            fails |= fz_check_data(fz, rd, FZ_SIGNAL_ID_F32, 32, (uint8_t *) fz->ref_f32,
                                   n, strict, (0 == fails) ? detail : (char[FZ_DETAIL_SZ]) {0});
        }
        if (strict && (n >= 100)) {
            uint32_t f;
            char alt[FZ_DETAIL_SZ];
            // the writer adjusts the requested signal def; use the stored values
            struct jls_signal_def_s def;
            int64_t sdf = 100;
            if (0 == jls_rd_signal(rd, FZ_SIGNAL_ID_F32, &def)) {
                sdf = def.sample_decimate_factor;
            }
            f = fz_check_stats_window(fz, rd, 0, n, 1, (uint32_t) sdf, n,
                                      (0 == fails) ? detail : alt);
            fails |= f;
            const int64_t incrs[] = {3, sdf, 5 * sdf, 97 * sdf, sdf + 3};
            for (size_t k = 0; k < 5; ++k) {
                int64_t incr = incrs[k];
                if ((incr <= 0) || (incr > n)) {
                    continue;
                }
                int64_t len = n / incr;
                if (len > 8) {
                    len = 8;
                }
                int64_t start_max = n - incr * len;
                int64_t start = start_max ? random_range_i64(0, start_max + 1) : 0;
                if (0 == (incr % sdf)) {
                    start -= start % sdf;  // entry-aligned window: exact expectations
                }
                f = fz_check_stats_window(fz, rd, start, incr, len, (uint32_t) sdf, n,
                                          (0 == fails) ? detail : alt);
                fails |= f;
            }
        }
    }

    // secondary signal: length + data
    if (has_sec && cfg->secondary) {
        int64_t n2 = 0;
        rc = jls_rd_fsr_length(rd, FZ_SIGNAL_ID_SEC, &n2);
        if (rc) {
            if (strict) {
                FZ_FAIL(FZ_FAIL_DATA, "sec fsr_length rc=%" PRIi32, rc);
            }
        } else {
            if (strict) {
                int64_t n2_min = fz_scan_sample_min(scan, FZ_SIGNAL_ID_SEC, trunc_offset);
                if (n2 > cfg->sec_samples) {
                    FZ_FAIL(FZ_FAIL_LENGTH_OVER, "sec n=%" PRIi64 " > written %" PRIi64,
                            n2, cfg->sec_samples);
                    n2 = 0;
                } else if (n2 < n2_min) {
                    FZ_FAIL(FZ_FAIL_EXCESS_LOSS, "sec n=%" PRIi64 " < intact %" PRIi64,
                            n2, n2_min);
                }
            } else if (n2 > cfg->sec_samples) {
                n2 = cfg->sec_samples;
            }
            if (n2 > 0) {
                fails |= fz_check_data(fz, rd, FZ_SIGNAL_ID_SEC, cfg->sec_entry_bits, fz->ref_sec,
                                       n2, strict, (0 == fails) ? detail : (char[FZ_DETAIL_SZ]) {0});
            }
        }
    }

    // annotations, UTC, user data
    if (cfg->ts_en) {
        int64_t counts_min[3];
        int64_t counts_total[3];
        fz_scan_ts_counts(scan, trunc_offset, counts_min, counts_total);
        struct fz_count_s count;

        count.count = 0;
        rc = has_f32 ? jls_rd_annotations(rd, FZ_SIGNAL_ID_F32, 0, fz_ann_cbk, &count) : 0;
        if (rc && strict) {
            FZ_FAIL(FZ_FAIL_ANN_ERROR, "annotations rc=%" PRIi32 " %s", rc, jls_error_code_name(rc));
        } else if (strict && has_f32
                   && ((count.count < counts_min[0]) || (count.count > counts_total[0]))) {
            FZ_FAIL(FZ_FAIL_ANN_COUNT, "annotations %" PRIi64 " not in [%" PRIi64 ", %" PRIi64 "]",
                    count.count, counts_min[0], counts_total[0]);
        }

        count.count = 0;
        rc = has_f32 ? jls_rd_utc(rd, FZ_SIGNAL_ID_F32, 0, fz_utc_cbk, &count) : 0;
        if (rc && strict) {
            FZ_FAIL(FZ_FAIL_UTC_ERROR, "utc rc=%" PRIi32 " %s", rc, jls_error_code_name(rc));
        } else if (strict && has_f32
                   && ((count.count < counts_min[1]) || (count.count > counts_total[1]))) {
            FZ_FAIL(FZ_FAIL_UTC_COUNT, "utc %" PRIi64 " not in [%" PRIi64 ", %" PRIi64 "]",
                    count.count, counts_min[1], counts_total[1]);
        }

        count.count = 0;
        rc = jls_rd_user_data(rd, fz_ud_cbk, &count);
        if (rc && strict) {
            FZ_FAIL(FZ_FAIL_UD_ERROR, "user_data rc=%" PRIi32 " %s", rc, jls_error_code_name(rc));
        } else if (strict && ((count.count < counts_min[2]) || (count.count > counts_total[2]))) {
            FZ_FAIL(FZ_FAIL_UD_COUNT, "user_data %" PRIi64 " not in [%" PRIi64 ", %" PRIi64 "]",
                    count.count, counts_min[2], counts_total[2]);
        }
    }

    jls_rd_close(rd);
    rd = NULL;

    // idempotence: reopen must succeed, preserve length, and not modify the file
    uint64_t hash1 = fz_file_hash(path);
    rc = jls_rd_open(&rd, path);
    if (rc) {
        if (!fz_error_code_valid(rc)) {
            FZ_FAIL(FZ_FAIL_ERROR_CODE, "reopen rc=%" PRIi32 " out of range", rc);
        } else if (strict) {
            FZ_FAIL(FZ_FAIL_REOPEN, "reopen rc=%" PRIi32 " %s", rc, jls_error_code_name(rc));
        }
        return fails;
    }
    if (strict && has_f32) {
        int64_t n2 = 0;
        rc = jls_rd_fsr_length(rd, FZ_SIGNAL_ID_F32, &n2);
        if (rc || (n2 != n)) {
            FZ_FAIL(FZ_FAIL_IDEMPOTENT, "reopen length %" PRIi64 " != %" PRIi64 " rc=%" PRIi32,
                    n2, n, rc);
        }
    }
    jls_rd_close(rd);
    if (strict) {
        uint64_t hash2 = fz_file_hash(path);
        if (hash1 != hash2) {
            FZ_FAIL(FZ_FAIL_IDEMPOTENT, "reopen modified the file");
        }
    }
    return fails;
}

static void fz_path_pristine(struct fz_s * fz, uint32_t cfg_id, char * path, size_t path_sz) {
    snprintf(path, path_sz, "%s/pristine_%02u.jls", fz->workdir, cfg_id);
}

static int32_t fz_ensure_pristine(struct fz_s * fz, const struct fz_cfg_s * cfg) {
    char path[600];
    fz_path_pristine(fz, cfg->id, path, sizeof(path));
    if (fz->generated[cfg->id]) {
        return 0;
    }
    int32_t rc = fz_generate(fz, cfg, path);
    if (rc) {
        printf("generate cfg=%u failed rc=%" PRIi32 " %s\n", cfg->id, rc, jls_error_code_name(rc));
        return rc;
    }
    rc = fz_scan(path, &fz->scans[cfg->id]);
    if (rc) {
        printf("scan cfg=%u failed rc=%" PRIi32 " %s\n", cfg->id, rc, jls_error_code_name(rc));
        return rc;
    }
    fz->generated[cfg->id] = 1;

    if (cfg->closed) {
        // oracle self-test: an undamaged copy of a closed file must pass
        char case_path[600];
        char detail[FZ_DETAIL_SZ];
        snprintf(case_path, sizeof(case_path), "%s/selftest.jls", fz->workdir);
        struct fz_scan_s * scan = &fz->scans[cfg->id];
        ROE(fz_copy_truncate(path, case_path, scan->fend));
        struct fz_case_s cs = {
                .mode = FZ_MODE_TRUNCATE,
                .offset = scan->fend,
                .strict = true,
                .defs_ok = true,
        };
        uint32_t fails = fz_oracle(fz, cfg, scan, case_path, &cs, detail);
        remove(case_path);
        if (fails) {
            printf("ORACLE SELF-TEST FAILED cfg=%u fails=0x%08x : %s\n", cfg->id, fails, detail);
            printf("The oracle or generator is broken; fix before fuzzing.\n");
            return JLS_ERROR_UNSPECIFIED;
        }
    }
    return 0;
}

// Overwrite length bytes at offset in an existing file.
static int32_t fz_patch(const char * path, int64_t offset, const uint8_t * buf, size_t length) {
    FILE * f = fopen(path, "r+b");
    if (!f) {
        return JLS_ERROR_IO;
    }
    int32_t rc = 0;
    if (fseek(f, (long) offset, SEEK_SET) || (length != fwrite(buf, 1, length, f))) {
        rc = JLS_ERROR_IO;
    }
    fclose(f);
    return rc;
}

static int64_t fz_pick_offset(struct fz_s * fz, const struct fz_cfg_s * cfg,
                              const struct fz_scan_s * scan) {
    int64_t off_max = scan->fend - (cfg->closed ? 1 : 0);  // fend = no-op for closed files
    if (fz->offset_forced >= 0) {
        int64_t off = fz->offset_forced;
        return (off > off_max) ? off_max : off;
    }
    int64_t off;
    if (random_u32() & 1) {  // uniform
        off = random_range_i64(0, off_max + 1);
    } else {  // chunk-boundary relative
        static const int64_t DELTAS[] = {-33, -32, -8, -1, 0, 1, 7, 8, 31};
        const struct fz_chunk_s * c = &scan->chunks[random_range_u32(0, (uint32_t) scan->length)];
        off = c->end_offset + DELTAS[random_range_u32(0, 9)];
    }
    if (off < 0) {
        off = 0;
    }
    if (off > off_max) {
        off = off_max;
    }
    return off;
}

// Create the corrupted case file from the pristine file.
static int32_t fz_make_case(struct fz_s * fz, const struct fz_cfg_s * cfg,
                            const struct fz_scan_s * scan, const char * pristine_path,
                            const char * case_path, struct fz_case_s * cs) {
    uint8_t buf[4096];
    memset(cs, 0, sizeof(*cs));
    cs->mode = fz->mode;
    cs->strict = (fz->mode != FZ_MODE_BITFLIP);
    cs->offset = scan->fend;
    cs->defs_ok = true;

    switch (fz->mode) {
        case FZ_MODE_TRUNCATE:
            cs->offset = fz_pick_offset(fz, cfg, scan);
            cs->defs_ok = (cs->offset >= scan->defs_end);
            return fz_copy_truncate(pristine_path, case_path, cs->offset);

        case FZ_MODE_ZERO_TAIL: {
            cs->offset = fz_pick_offset(fz, cfg, scan);
            cs->defs_ok = (cs->offset >= scan->defs_end);
            ROE(fz_copy_truncate(pristine_path, case_path, scan->fend));
            memset(buf, 0, sizeof(buf));
            int64_t pos = cs->offset;
            while (pos < scan->fend) {
                size_t sz = sizeof(buf);
                if ((int64_t) sz > (scan->fend - pos)) {
                    sz = (size_t) (scan->fend - pos);
                }
                ROE(fz_patch(case_path, pos, buf, sz));
                pos += (int64_t) sz;
            }
            return 0;
        }

        case FZ_MODE_ZERO_HOLE: {
            cs->hole_len = 8 << random_range_u32(0, 10);  // 8 .. 4096 bytes
            if (cs->hole_len > (scan->fend - 32)) {
                cs->hole_len = scan->fend - 32;
            }
            cs->offset = (fz->offset_forced >= 0) ? fz->offset_forced
                    : random_range_i64(32, scan->fend - cs->hole_len + 1);
            cs->defs_ok = (cs->offset >= scan->defs_end);
            ROE(fz_copy_truncate(pristine_path, case_path, scan->fend));
            memset(buf, 0, sizeof(buf));
            int64_t pos = cs->offset;
            int64_t end = cs->offset + cs->hole_len;
            while (pos < end) {
                size_t sz = sizeof(buf);
                if ((int64_t) sz > (end - pos)) {
                    sz = (size_t) (end - pos);
                }
                ROE(fz_patch(case_path, pos, buf, sz));
                pos += (int64_t) sz;
            }
            return 0;
        }

        case FZ_MODE_TORN_UPDATE: {
            // pick an in-place-updated metadata chunk: HEAD, INDEX, or SUMMARY
            size_t candidates = 0;
            for (size_t i = 0; i < scan->length; ++i) {
                uint8_t t = scan->chunks[i].tag;
                if ((t & JLS_TRACK_TAG_FLAG) && scan->chunks[i].payload_length
                        && (((t & 7) == JLS_TRACK_CHUNK_HEAD)
                            || ((t & 7) == JLS_TRACK_CHUNK_INDEX)
                            || ((t & 7) == JLS_TRACK_CHUNK_SUMMARY))) {
                    ++candidates;
                }
            }
            if (0 == candidates) {  // no metadata chunks; degenerate to truncate
                cs->mode = FZ_MODE_TRUNCATE;
                cs->offset = fz_pick_offset(fz, cfg, scan);
                cs->defs_ok = (cs->offset >= scan->defs_end);
                return fz_copy_truncate(pristine_path, case_path, cs->offset);
            }
            size_t pick = (size_t) random_range_u32(0, (uint32_t) candidates);
            const struct fz_chunk_s * c = NULL;
            for (size_t i = 0; i < scan->length; ++i) {
                uint8_t t = scan->chunks[i].tag;
                if ((t & JLS_TRACK_TAG_FLAG) && scan->chunks[i].payload_length
                        && (((t & 7) == JLS_TRACK_CHUNK_HEAD)
                            || ((t & 7) == JLS_TRACK_CHUNK_INDEX)
                            || ((t & 7) == JLS_TRACK_CHUNK_SUMMARY))) {
                    if (0 == pick--) {
                        c = &scan->chunks[i];
                        break;
                    }
                }
            }
            cs->torn_offset = c->offset;
            // a torn in-place update implies a crash, so the tail after the
            // torn chunk may also be lost
            cs->offset = fz_pick_offset(fz, cfg, scan);
            if (cs->offset < c->end_offset) {
                cs->offset = c->end_offset;
            }
            ROE(fz_copy_truncate(pristine_path, case_path, cs->offset));
            size_t torn_len = c->payload_length / 2;
            if (torn_len > sizeof(buf)) {
                torn_len = sizeof(buf);
            }
            if (0 == torn_len) {
                torn_len = 1;
            }
            for (size_t i = 0; i < torn_len; ++i) {
                buf[i] = (uint8_t) random_u32();
            }
            return fz_patch(case_path, c->offset + FZ_CHUNK_HDR_SZ, buf, torn_len);
        }

        case FZ_MODE_BITFLIP: {
            ROE(fz_copy_truncate(pristine_path, case_path, scan->fend));
            uint32_t flips = random_range_u32(1, 9);
            cs->offset = scan->fend;  // no bound expectations
            cs->defs_ok = false;
            for (uint32_t i = 0; i < flips; ++i) {
                int64_t pos = random_range_i64(32, scan->fend);
                FILE * f = fopen(case_path, "r+b");
                if (!f) {
                    return JLS_ERROR_IO;
                }
                uint8_t b = 0;
                if (fseek(f, (long) pos, SEEK_SET) || (1 != fread(&b, 1, 1, f))) {
                    fclose(f);
                    return JLS_ERROR_IO;
                }
                b ^= (uint8_t) (1U << random_range_u32(0, 8));
                if (fseek(f, (long) pos, SEEK_SET) || (1 != fwrite(&b, 1, 1, f))) {
                    fclose(f);
                    return JLS_ERROR_IO;
                }
                fclose(f);
            }
            return 0;
        }

        default:
            return JLS_ERROR_PARAMETER_INVALID;
    }
}

static uint64_t fz_log_quiet_count_;

static void fz_log_quiet(const char * msg) {
    (void) msg;
    ++fz_log_quiet_count_;
}

// Run the oracle plus the read-only variant.  In an isolated child this
// runs with logging suppressed; a crash or hang is caught by the parent.
static uint32_t fz_case_verify(struct fz_s * fz, const struct fz_cfg_s * cfg,
                               const struct fz_scan_s * scan, const char * case_path,
                               const char * ro_path, const struct fz_case_s * cs,
                               uint64_t case_seed, char * detail) {
    uint32_t fails = fz_oracle(fz, cfg, scan, case_path, cs, detail);

    // read-only open of a damaged file must fail gracefully
#if !defined(_WIN32)
    bool damaged = (cs->offset < scan->fend) || !cfg->closed
            || (cs->mode != FZ_MODE_TRUNCATE);
    if (damaged && (0 == (case_seed & 0xF))) {
        struct jls_rd_s * rd = NULL;
        chmod(ro_path, 0444);
        int32_t rc = jls_rd_open(&rd, ro_path);
        if (0 == rc) {
            jls_rd_close(rd);
            if (cs->strict) {
                if (0 == fails) {
                    snprintf(detail, FZ_DETAIL_SZ, "read-only open of damaged file returned 0");
                }
                fails |= (1U << FZ_FAIL_RDONLY);
            }
        } else if (!fz_error_code_valid(rc)) {
            if (0 == fails) {
                snprintf(detail, FZ_DETAIL_SZ, "read-only open rc=%" PRIi32 " out of range", rc);
            }
            fails |= (1U << FZ_FAIL_RDONLY);
        }
        chmod(ro_path, 0644);
    }
#else
    (void) ro_path;
    (void) case_seed;
#endif
    return fails;
}

struct fz_result_s {
    uint32_t fails;
    char detail[FZ_DETAIL_SZ];
};

// Run fz_case_verify in a child process so crashes and hangs are
// recorded instead of killing the campaign.
static void fz_case_run(struct fz_s * fz, const struct fz_cfg_s * cfg,
                        const struct fz_scan_s * scan, const char * case_path,
                        const char * ro_path, const struct fz_case_s * cs,
                        uint64_t case_seed, struct fz_result_s * result) {
    result->fails = 0;
    result->detail[0] = 0;
#if defined(_WIN32)
    result->fails = fz_case_verify(fz, cfg, scan, case_path, ro_path, cs,
                                   case_seed, result->detail);
#else
    if (!fz->isolate) {
        result->fails = fz_case_verify(fz, cfg, scan, case_path, ro_path, cs,
                                       case_seed, result->detail);
        return;
    }
    int fds[2];
    if (pipe(fds)) {
        result->fails = fz_case_verify(fz, cfg, scan, case_path, ro_path, cs,
                                       case_seed, result->detail);
        return;
    }
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        result->fails = fz_case_verify(fz, cfg, scan, case_path, ro_path, cs,
                                       case_seed, result->detail);
        return;
    }
    if (0 == pid) {  // child
        close(fds[0]);
        fz_log_quiet_count_ = 0;
        jls_log_register(fz_log_quiet);  // suppress log flood in bulk runs
        struct fz_result_s r;
        r.fails = fz_case_verify(fz, cfg, scan, case_path, ro_path, cs,
                                 case_seed, r.detail);
        ssize_t wr_rc = write(fds[1], &r, sizeof(r));
        (void) wr_rc;
        _exit(0);
    }
    close(fds[1]);
    int status = 0;
    uint32_t waited_ms = 0;
    const uint32_t timeout_ms = fz->timeout * 1000;
    while (1) {
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            break;
        }
        if (waited_ms >= timeout_ms) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            result->fails = (1U << FZ_FAIL_HANG);
            snprintf(result->detail, sizeof(result->detail),
                     "timeout after %u s", fz->timeout);
            close(fds[0]);
            return;
        }
        usleep(10000);
        waited_ms += 10;
    }
    if (WIFSIGNALED(status)) {
        result->fails = (1U << FZ_FAIL_CRASH);
        snprintf(result->detail, sizeof(result->detail),
                 "child killed by signal %d", WTERMSIG(status));
        close(fds[0]);
        return;
    }
    ssize_t sz = read(fds[0], result, sizeof(*result));
    close(fds[0]);
    if (sz != (ssize_t) sizeof(*result)) {
        result->fails = (1U << FZ_FAIL_CRASH);
        snprintf(result->detail, sizeof(result->detail),
                 "child exited without result, status 0x%x", status);
    }
    result->detail[FZ_DETAIL_SZ - 1] = 0;
#endif
}

static int32_t fz_run_case(struct fz_s * fz, uint64_t case_seed) {
    char pristine_path[600];
    char case_path[600];
    char ro_path[600];
    struct fz_cfg_s cfg;
    struct fz_case_s cs;
    struct fz_result_s result;

    uint32_t cfg_id;
    if (fz->cfg_forced >= 0) {
        cfg_id = (uint32_t) fz->cfg_forced % FZ_CFG_COUNT;
    } else {
        cfg_id = random_range_u32(0, FZ_CFG_COUNT);
    }
    fz_cfg_init(&cfg, cfg_id);
    fz_ref_fill(fz, &cfg);
    if (fz_ensure_pristine(fz, &cfg)) {
        ++fz->fails[FZ_FAIL_GENERATE];
        ++fz->fail_count;
        return JLS_ERROR_UNSPECIFIED;  // generation/self-test failure is fatal
    }
    struct fz_scan_s * scan = &fz->scans[cfg_id];
    fz_path_pristine(fz, cfg_id, pristine_path, sizeof(pristine_path));

    snprintf(case_path, sizeof(case_path), "%s/case.jls", fz->workdir);
    snprintf(ro_path, sizeof(ro_path), "%s/case_ro.jls", fz->workdir);
    if (fz_make_case(fz, &cfg, scan, pristine_path, case_path, &cs)) {
        printf("case create failed\n");
        return JLS_ERROR_IO;
    }
    if (fz_copy_truncate(case_path, ro_path, INT64_MAX)) {
        printf("case copy failed\n");
        return JLS_ERROR_IO;
    }

    if (fz->verbose) {
        printf("%20" PRIu64 " cfg=%2u %s @%9" PRIi64 "/%" PRIi64 " : ",
               case_seed, cfg_id, FZ_MODE_NAMES[cs.mode], cs.offset, scan->fend);
        fflush(stdout);
    }
    fz_case_run(fz, &cfg, scan, case_path, ro_path, &cs, case_seed, &result);
    uint32_t fails = result.fails;

    ++fz->case_count;
    if (fails) {
        ++fz->fail_count;
        for (uint32_t bit = 0; bit < FZ_FAIL_COUNT; ++bit) {
            if (fails & (1U << bit)) {
                ++fz->fails[bit];
            }
        }
        if (!fz->verbose) {
            printf("%20" PRIu64 " cfg=%2u %s @%9" PRIi64 "/%" PRIi64 " : ",
                   case_seed, cfg_id, FZ_MODE_NAMES[cs.mode], cs.offset, scan->fend);
        }
        printf("FAIL [");
        for (uint32_t bit = 0; bit < FZ_FAIL_COUNT; ++bit) {
            if (fails & (1U << bit)) {
                printf(" %s", FZ_FAIL_NAMES[bit]);
            }
        }
        printf(" ] %s\n", result.detail);
        printf("    repro: jls read_fuzzer --corrupt %s --random %" PRIu64
               " --cfg %u --iterations 1 --workdir %s\n",
               FZ_MODE_NAMES[fz->mode], case_seed, cfg_id, fz->workdir);
        if (fz->keep) {
            char keep_path[700];
            snprintf(keep_path, sizeof(keep_path), "%s/fail_%" PRIu64 ".jls",
                     fz->workdir, case_seed);
            remove(keep_path);
            fz_copy_truncate(ro_path, keep_path, INT64_MAX);
            printf("    kept: %s\n", keep_path);
        }
    } else if (fz->verbose) {
        printf("PASS %s\n", result.detail);
    }
    remove(case_path);
    remove(ro_path);
    return 0;
}

static int fz_campaign(struct fz_s * fz) {
    int32_t rc = 0;

#if defined(_WIN32)
    _mkdir(fz->workdir);
#else
    mkdir(fz->workdir, 0755);
#endif

    fz->ref_f32 = malloc(sizeof(float) * (size_t) FZ_SIZES[2]);
    fz->ref_sec = malloc(2 * (size_t) FZ_SIZES[2]);
    fz->rd_buf_sz = (1 << 20) + 64;
    fz->rd_buf = malloc(fz->rd_buf_sz);
    fz->stats_buf = malloc(sizeof(double) * JLS_SUMMARY_FSR_COUNT * FZ_STATS_LEN_MAX);
    if (!fz->ref_f32 || !fz->ref_sec || !fz->rd_buf || !fz->stats_buf) {
        rc = JLS_ERROR_NOT_ENOUGH_MEMORY;
        goto cleanup;
    }

    uint64_t iteration = 0;
    while (!quit_) {
        if (fz->iterations && (iteration >= fz->iterations)) {
            break;
        }
        ++iteration;
        uint64_t case_seed = random_seed;
        rc = fz_run_case(fz, case_seed);
        if (rc) {
            break;
        }
        if (!fz->verbose && (0 == (iteration % 1000))) {
            printf("... %" PRIu64 " cases, %" PRIu64 " failures\n",
                   fz->case_count, fz->fail_count);
            fflush(stdout);
        }
    }

    printf("\ncorruption fuzz summary: mode=%s cases=%" PRIu64 " failures=%" PRIu64 "\n",
           FZ_MODE_NAMES[fz->mode], fz->case_count, fz->fail_count);
    for (uint32_t bit = 0; bit < FZ_FAIL_COUNT; ++bit) {
        if (fz->fails[bit]) {
            printf("    %-16s %" PRIu64 "\n", FZ_FAIL_NAMES[bit], fz->fails[bit]);
        }
    }

cleanup:
    free(fz->ref_f32);
    free(fz->ref_sec);
    free(fz->rd_buf);
    free(fz->stats_buf);
    for (uint32_t i = 0; i < FZ_CFG_COUNT; ++i) {
        fz_scan_free(&fz->scans[i]);
    }
    if (rc) {
        return rc;
    }
    return fz->fail_count ? 1 : 0;
}


/****************************************************************************/
/* Read parameter fuzz (original mode)                                      */
/****************************************************************************/

static int fuzz_read_params(const char * path, uint32_t max_length) {
    struct jls_rd_s * rd = NULL;
    uint8_t guard_byte = 0xCC;
    uint32_t guard_length = 32;
    uint8_t * guard;
    int32_t rc;

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
    if (!data) {
        jls_rd_close(rd);
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }

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
                printf("incomplete: ");
                for (int32_t i = - ((int32_t) guard_length); i < 0; ++i) {
                    printf(" %02x", guard[i]);
                }
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

    free(data);
    jls_rd_close(rd);
    return 0;
}

int on_read_fuzzer(struct app_s * self, int argc, char * argv[]) {
    char * path = NULL;
    char * corrupt_mode = NULL;
    char * workdir = NULL;
    int pos_arg = 0;
    uint32_t max_length = 5000;
    uint64_t iterations = 1000;
    uint32_t keep = 0;
    uint32_t verbose = 0;
    uint32_t cfg_forced = 0;
    uint64_t offset_forced = 0;
    uint32_t isolate = 1;
    uint32_t timeout = 120;
    bool has_cfg = false;
    bool has_offset = false;
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
        } else if (0 == strcmp("--corrupt", argv[0])) {
            ARG_CONSUME();
            ARG_REQUIRE();
            corrupt_mode = argv[0];
            ARG_CONSUME();
        } else if (0 == strcmp("--iterations", argv[0])) {
            ARG_CONSUME();
            ARG_REQUIRE();
            if (jls_cstr_to_u64(argv[0], &iterations)) {
                return usage();
            }
            ARG_CONSUME();
        } else if (0 == strcmp("--workdir", argv[0])) {
            ARG_CONSUME();
            ARG_REQUIRE();
            workdir = argv[0];
            ARG_CONSUME();
        } else if (0 == strcmp("--cfg", argv[0])) {
            ARG_CONSUME();
            ARG_REQUIRE();
            if (jls_cstr_to_u32(argv[0], &cfg_forced)) {
                return usage();
            }
            has_cfg = true;
            ARG_CONSUME();
        } else if (0 == strcmp("--offset", argv[0])) {
            ARG_CONSUME();
            ARG_REQUIRE();
            if (jls_cstr_to_u64(argv[0], &offset_forced)) {
                return usage();
            }
            has_offset = true;
            ARG_CONSUME();
        } else if (0 == strcmp("--timeout", argv[0])) {
            ARG_CONSUME();
            ARG_REQUIRE();
            if (jls_cstr_to_u32(argv[0], &timeout)) {
                return usage();
            }
            ARG_CONSUME();
        } else if (0 == strcmp("--no-isolate", argv[0])) {
            isolate = 0;
            ARG_CONSUME();
        } else if (0 == strcmp("--keep", argv[0])) {
            keep = 1;
            ARG_CONSUME();
        } else if (0 == strcmp("--verbose", argv[0])) {
            verbose = 1;
            ARG_CONSUME();
        } else {
            return usage();
        }
    }

    if (NULL != corrupt_mode) {
        int32_t mode = -1;
        for (int32_t i = 0; i < FZ_MODE_COUNT; ++i) {
            if (0 == strcmp(FZ_MODE_NAMES[i], corrupt_mode)) {
                mode = i;
                break;
            }
        }
        if (mode < 0) {
            printf("unsupported corruption mode: %s\n", corrupt_mode);
            return usage();
        }
        struct fz_s * fz = calloc(1, sizeof(struct fz_s));
        if (!fz) {
            return JLS_ERROR_NOT_ENOUGH_MEMORY;
        }
        fz->mode = (enum fz_mode_e) mode;
        snprintf(fz->workdir, sizeof(fz->workdir), "%s",
                 workdir ? workdir : "read_fuzzer_work");
        fz->keep = keep;
        fz->verbose = verbose;
        fz->isolate = isolate;
        fz->timeout = timeout ? timeout : 120;
        fz->iterations = iterations;
        fz->cfg_forced = has_cfg ? (int32_t) cfg_forced : -1;
        fz->offset_forced = has_offset ? (int64_t) offset_forced : -1;
        int rc = fz_campaign(fz);
        free(fz);
        return rc;
    }

    if (pos_arg != 1) {
        return usage();
    }
    return fuzz_read_params(path, max_length);
}
