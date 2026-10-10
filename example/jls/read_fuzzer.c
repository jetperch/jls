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
#include "jls/crc32c.h"
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
#include <stdarg.h>
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
            "  --corrupt <mode>   Corruption mode, one of: truncate, zero_tail,\n"
            "                     zero_hole, torn_update, bitflip, write_prefix,\n"
            "                     resign, resign_header, resign_payload\n"
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
            "\n"
            "The resign modes mutate chunk header and payload fields with\n"
            "structure-aware values, then recompute the chunk CRCs so the\n"
            "hostile file reaches every parser.  resign_header mutates one\n"
            "header, resign_payload one payload, and resign applies 1-3 mixed\n"
            "mutations.  The file need not recover: a case passes when the\n"
            "reader, jls_copy and jls_repair return only JLS error codes\n"
            "without crashing or hanging.  Build with ASan/UBSan and set\n"
            "ASAN_OPTIONS=detect_leaks=0:halt_on_error=1:exitcode=99 so\n"
            "sanitizer reports count as crash failures.  jls_copy output is\n"
            "bounded by a write budget; exceeding it is reported as the\n"
            "copy_budget note (hostile sample_id gaps are NaN-filled).\n"
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
    FZ_FAIL_REPAIR,         // jls_repair failed or its result did not open
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
    "readonly", "repair_fail", "generate", "crash", "hang",
};

enum fz_mode_e {
    FZ_MODE_TRUNCATE,       // cut the file at a byte offset
    FZ_MODE_ZERO_TAIL,      // keep length, zero all bytes from offset
    FZ_MODE_ZERO_HOLE,      // zero an interior region (writeback reorder)
    FZ_MODE_TORN_UPDATE,    // corrupt an in-place-updated metadata chunk
                            // payload, then truncate the tail
    FZ_MODE_BITFLIP,        // flip random bits; graceful errors only
    FZ_MODE_WRITE_PREFIX,   // stop the writer after N bytes (exact crash)
    FZ_MODE_RESIGN,         // 1-3 mixed resign_header/resign_payload mutations
    FZ_MODE_RESIGN_HEADER,  // mutate one chunk header field, re-sign its CRC
    FZ_MODE_RESIGN_PAYLOAD, // mutate one chunk payload field, re-sign its CRC
};

static const char * FZ_MODE_NAMES[] = {
    "truncate", "zero_tail", "zero_hole", "torn_update", "bitflip",
    "write_prefix", "resign", "resign_header", "resign_payload",
};
#define FZ_MODE_COUNT (9)

enum fz_note_e {
    FZ_NOTE_COPY_BUDGET,    // jls_copy output exceeded its write budget
    FZ_NOTE_COUNT
};

static const char * FZ_NOTE_NAMES[] = {"copy_budget"};

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
    int64_t write_total;    // total backend bytes written to produce the file
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
    uint64_t notes[FZ_NOTE_COUNT];
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

// Generate a JLS file from cfg.  When crash is true, writer API errors
// (from an injected write-budget cutoff) are ignored and the file
// descriptor is closed abruptly to model a process crash mid-write.
static int32_t fz_generate_mode(struct fz_s * fz, const struct fz_cfg_s * cfg,
                                const char * path, bool crash) {
    struct jls_wr_s * wr = NULL;
    char buf[128];
    remove(path);

    // any write API may fail once the budget is exhausted; in crash mode
    // stop issuing writes but still close the fd, else propagate the error
    #define WR(x) do {                          \
        int32_t rc__ = (x);                     \
        if (rc__) {                             \
            if (crash) { goto crash_exit; }     \
            jls_wr_close(wr);                   \
            return rc__;                        \
        }                                       \
    } while (0)

    if (jls_wr_open(&wr, path)) {
        return crash ? 0 : JLS_ERROR_IO;
    }
    WR(jls_wr_source_def(wr, &FZ_SOURCE));
    WR(jls_wr_signal_def(wr, &FZ_SIGNAL_F32));
    if (cfg->secondary) {
        struct jls_signal_def_s sec = FZ_SIGNAL_F32;
        sec.signal_id = FZ_SIGNAL_ID_SEC;
        sec.data_type = cfg->sec_datatype;
        sec.name = "aux";
        sec.units = "";
        WR(jls_wr_signal_def(wr, &sec));
    }

    int64_t utc = JLS_TIME_YEAR;
    int64_t sec_pos = 0;
    int64_t w = 0;
    for (int64_t sample_id = 0; sample_id < cfg->samples; sample_id += FZ_WINDOW, ++w) {
        WR(jls_wr_fsr_f32(wr, FZ_SIGNAL_ID_F32, sample_id, fz->ref_f32 + sample_id, FZ_WINDOW));
        if (cfg->ts_en) {
            WR(jls_wr_utc(wr, FZ_SIGNAL_ID_F32, sample_id,
                          utc + JLS_COUNTER_TO_TIME(sample_id, FZ_SIGNAL_F32.sample_rate)));
            if ((w % 10) == 0) {
                snprintf(buf, sizeof(buf), "annotation %" PRIi64, w);
                WR(jls_wr_annotation(wr, FZ_SIGNAL_ID_F32, sample_id, 1.0f,
                                     JLS_ANNOTATION_TYPE_TEXT, 0, JLS_STORAGE_TYPE_STRING,
                                     (const uint8_t *) buf, 0));
            }
            if ((w % 25) == 0) {
                snprintf(buf, sizeof(buf), "{\"window\": %" PRIi64 "}", w);
                WR(jls_wr_user_data(wr, (uint16_t) (0x100 + (w / 25)), JLS_STORAGE_TYPE_JSON,
                                    (const uint8_t *) buf, 0));
            }
        }
        int64_t sec_limit = sample_id + FZ_WINDOW;
        if (sec_limit > cfg->sec_samples) {
            sec_limit = cfg->sec_samples;
        }
        while ((sec_pos + 1024) <= sec_limit) {
            const uint8_t * p = fz->ref_sec + ((sec_pos * cfg->sec_entry_bits) / 8);
            WR(jls_wr_fsr(wr, FZ_SIGNAL_ID_SEC, sec_pos, p, 1024));
            sec_pos += 1024;
        }
    }
    #undef WR

    if (crash) {
        goto crash_exit;
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

crash_exit: {
        // abrupt close: the writer state (heads, indices) is whatever the
        // partial writes left on disk, exactly as after a process crash
        struct jls_core_s * core = (struct jls_core_s *) wr;
        struct jls_bkf_s * bk = jls_raw_backend(core->raw);
        if (bk) {
            jls_bk_fclose(bk);
        }
    }
    return 0;
}

static int32_t fz_generate(struct fz_s * fz, const struct fz_cfg_s * cfg, const char * path) {
    return fz_generate_mode(fz, cfg, path, false);
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

    // Walk physically-present chunks by offset.  A read failure on the
    // final chunk (truncated/torn tail) is the normal end of a crashed
    // file, not an error; only an allocation failure aborts the scan.
    rc = 0;
    while (1) {
        struct jls_chunk_header_s hdr;
        int64_t offset = jls_raw_chunk_tell(raw);
        if (jls_raw_rd_header(raw, &hdr)) {
            break;  // no more complete chunks
        }
        struct fz_chunk_s c = {
                .offset = offset,
                .end_offset = offset + FZ_CHUNK_HDR_SZ + fz_psod(hdr.payload_length),
                .sample_end = 0,
                .tag = hdr.tag,
                .chunk_meta = hdr.chunk_meta,
                .payload_length = hdr.payload_length,
        };
        bool last = false;
        if ((hdr.tag == JLS_TAG_TRACK_FSR_DATA) && hdr.payload_length) {
            if (jls_raw_rd_payload(raw, 1 << 20, payload)) {
                break;  // incomplete final data chunk: exclude it
            }
            struct jls_payload_header_s * ph = (struct jls_payload_header_s *) payload;
            c.sample_end = ph->timestamp + ph->entry_count;
        } else if (jls_raw_chunk_next(raw)) {
            last = true;  // header valid but no room for a following chunk
        }
        rc = fz_scan_push(scan, &c);
        if (rc || last) {
            break;
        }
    }
    free(payload);
    jls_raw_close(raw);
    if (rc) {
        return rc;  // allocation failure only
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
    bool repaired;          // jls_repair ran: open must now succeed
    char desc[FZ_DETAIL_SZ]; // resign modes: the applied mutations
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
        } else if (cs->repaired && defs_ok) {
            FZ_FAIL(FZ_FAIL_REPAIR, "open rc=%" PRIi32 " %s after repair",
                    rc, jls_error_code_name(rc));
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
    jls_bk_write_limit_set(-1);
    int64_t writes_before = jls_bk_write_total();
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
    fz->scans[cfg->id].write_total = jls_bk_write_total() - writes_before;
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

/*
 * CRC-resigning mutations (resign, resign_header, resign_payload).
 *
 * Every other mode leaves chunk CRCs stale, so a mutated header or payload
 * is rejected by its CRC check before any parser sees it.  These modes
 * apply structure-aware mutations to chunks, then recompute the header
 * CRC (and the payload CRC footer for payload mutations), so the
 * hostile-but-CRC-valid file space reaches every parser.  A hostile file
 * need not recover data: see fz_hostile_exercise for the pass criteria.
 */

#define FZ_HDR_OFF_ITEM_NEXT        (0)
#define FZ_HDR_OFF_ITEM_PREV        (8)
#define FZ_HDR_OFF_TAG              (16)
#define FZ_HDR_OFF_RSV0             (17)
#define FZ_HDR_OFF_CHUNK_META       (18)
#define FZ_HDR_OFF_PAYLOAD_LENGTH   (20)
#define FZ_HDR_OFF_PAYLOAD_PREV     (24)
#define FZ_PH_SZ                    (16)  // struct jls_payload_header_s
#define FZ_PH_OFF_TIMESTAMP         (0)
#define FZ_PH_OFF_ENTRY_COUNT       (8)
#define FZ_PH_OFF_ENTRY_SIZE_BITS   (12)
#define FZ_PH_OFF_RSV16             (14)

static bool fz_mode_is_resign(enum fz_mode_e mode) {
    return (mode == FZ_MODE_RESIGN) || (mode == FZ_MODE_RESIGN_HEADER)
            || (mode == FZ_MODE_RESIGN_PAYLOAD);
}

// In-memory hostile file under construction.
struct fz_mut_s {
    uint8_t * buf;
    int64_t size;
    const struct fz_scan_s * scan;
    char * desc;            // human-readable mutation log, FZ_DETAIL_SZ
};

static void fz_desc_append(struct fz_mut_s * m, const char * fmt, ...) {
    size_t len = strlen(m->desc);
    if (len + 1 >= FZ_DETAIL_SZ) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vsnprintf(m->desc + len, FZ_DETAIL_SZ - len, fmt, args);
    va_end(args);
}

static uint64_t fz_rd_u(const uint8_t * p, uint32_t sz) {
    uint64_t v = 0;
    for (uint32_t i = 0; i < sz; ++i) {
        v |= ((uint64_t) p[i]) << (8 * i);
    }
    return v;
}

static void fz_wr_u(uint8_t * p, uint32_t sz, uint64_t v) {
    for (uint32_t i = 0; i < sz; ++i) {
        p[i] = (uint8_t) (v >> (8 * i));
    }
}

static uint32_t fz_pick_u32(uint32_t old) {
    static const uint32_t EDGES[] = {
        0, 1, 2, 3, 7, 8, 15, 16, 255, 256, 1000, 65535, 65536,
        1U << 20, (1U << 20) + 1, 1U << 24, 0x7FFFFFFFU, 0x80000000U,
        0xFFFFFFF0U, 0xFFFFFFFEU, 0xFFFFFFFFU,
    };
    const uint32_t n = (uint32_t) (sizeof(EDGES) / sizeof(EDGES[0]));
    switch (random_range_u32(0, 6)) {
        case 0: case 1: return EDGES[random_range_u32(0, n)];
        case 2: return old + random_range_u32(1, 17);
        case 3: return old - random_range_u32(1, 17);
        case 4: return (random_u32() & 1) ? (old * 2) : (old / 2);
        default: return random_u32();
    }
}

static int64_t fz_pick_i64(int64_t old) {
    static const int64_t EDGES[] = {
        0, 1, -1, 2, INT64_MIN, INT64_MIN + 1, INT64_MAX, INT64_MAX - 1,
        INT32_MAX, INT32_MIN, (int64_t) UINT32_MAX, 1LL << 32, 1LL << 62, -(1LL << 62),
    };
    const uint32_t n = (uint32_t) (sizeof(EDGES) / sizeof(EDGES[0]));
    switch (random_range_u32(0, 6)) {
        case 0: case 1: return EDGES[random_range_u32(0, n)];
        case 2: return (int64_t) ((uint64_t) old + random_range_u32(1, 2001));
        case 3: return (int64_t) ((uint64_t) old - random_range_u32(1, 2001));
        case 4: return (int64_t) (0U - (uint64_t) old);
        default: return (int64_t) random_u64();
    }
}

static uint16_t fz_pick_u16(uint16_t old) {
    static const uint16_t EDGES[] = {0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 24, 31, 32, 33,
                                     63, 64, 65, 128, 255, 256, 0x7FFF, 0x8000, 0xFFFF};
    const uint32_t n = (uint32_t) (sizeof(EDGES) / sizeof(EDGES[0]));
    switch (random_range_u32(0, 4)) {
        case 0: case 1: return EDGES[random_range_u32(0, n)];
        case 2: return (uint16_t) (old ^ (1U << random_range_u32(0, 16)));
        default: return (uint16_t) random_u32();
    }
}

// A file offset: self, neighbors, other chunks, misaligned, past EOF, ...
static uint64_t fz_pick_offset_value(struct fz_mut_s * m, int64_t self) {
    const struct fz_scan_s * scan = m->scan;
    const struct fz_chunk_s * other = &scan->chunks[random_range_u32(0, (uint32_t) scan->length)];
    switch (random_range_u32(0, 16)) {
        case 0: return 0;
        case 1: return (uint64_t) self;                         // self loop
        case 2: return (uint64_t) self + FZ_CHUNK_HDR_SZ;       // into own payload
        case 3: return (uint64_t) (self - FZ_CHUNK_HDR_SZ);     // backward, mid-chunk
        case 4: case 5: case 6: return (uint64_t) other->offset;  // any chunk, often backward
        case 7: return (uint64_t) other->offset + 4;            // misaligned
        case 8: return (uint64_t) other->offset + 8;            // inside a header
        case 9: return (uint64_t) m->size;                      // EOF
        case 10: return (uint64_t) m->size + 8;                 // past EOF
        case 11: return (uint64_t) m->size - 8;                 // tail
        case 12: return 1ULL << 40;
        case 13: return UINT64_MAX - 7;
        case 14: return 16;                                     // file header
        default: return (uint64_t) random_range_i64(0, m->size + 1) & ~7ULL;
    }
}

// Tag-class weights: bias toward parsers with the most structure.
static uint32_t fz_tag_weight(uint8_t tag) {
    switch (tag) {
        case JLS_TAG_SIGNAL_DEF: return 8;
        case JLS_TAG_SOURCE_DEF: return 3;
        case JLS_TAG_TRACK_FSR_DATA: return 4;
        case JLS_TAG_TRACK_FSR_INDEX: return 6;
        case JLS_TAG_TRACK_FSR_SUMMARY: return 6;
        case JLS_TAG_TRACK_ANNOTATION_DATA: return 5;
        case JLS_TAG_TRACK_ANNOTATION_INDEX: return 4;
        case JLS_TAG_TRACK_ANNOTATION_SUMMARY: return 4;
        case JLS_TAG_TRACK_UTC_DATA: return 4;
        case JLS_TAG_TRACK_UTC_INDEX: return 4;
        case JLS_TAG_TRACK_UTC_SUMMARY: return 4;
        case JLS_TAG_USER_DATA: return 4;
        case JLS_TAG_END: return 1;
        default: break;
    }
    if ((tag & JLS_TRACK_TAG_FLAG) && ((tag & 7) == JLS_TRACK_CHUNK_HEAD)) {
        return 5;
    }
    return 2;  // track defs and anything else
}

static bool fz_chunk_eligible(const struct fz_mut_s * m, const struct fz_chunk_s * c,
                              bool need_payload) {
    if ((c->offset + FZ_CHUNK_HDR_SZ) > m->size) {
        return false;
    }
    return !need_payload || (c->payload_length && (c->end_offset <= m->size));
}

// Pick a chunk: weighted tag class first, then uniform within the class,
// so the many FSR data chunks do not drown the rare definition chunks.
static const struct fz_chunk_s * fz_pick_chunk(struct fz_mut_s * m, bool need_payload) {
    const struct fz_scan_s * scan = m->scan;
    uint32_t counts[256] = {0};
    uint32_t total = 0;
    for (size_t i = 0; i < scan->length; ++i) {
        const struct fz_chunk_s * c = &scan->chunks[i];
        if (fz_chunk_eligible(m, c, need_payload)) {
            if (0 == counts[c->tag]++) {
                total += fz_tag_weight(c->tag);
            }
        }
    }
    if (0 == total) {
        return NULL;
    }
    uint32_t r = random_range_u32(0, total);
    uint32_t tag;
    for (tag = 0; tag < 255; ++tag) {
        if (counts[tag]) {
            uint32_t w = fz_tag_weight((uint8_t) tag);
            if (r < w) {
                break;
            }
            r -= w;
        }
    }
    uint32_t pick = random_range_u32(0, counts[tag]);
    for (size_t i = 0; i < scan->length; ++i) {
        const struct fz_chunk_s * c = &scan->chunks[i];
        if ((c->tag == tag) && fz_chunk_eligible(m, c, need_payload) && (0 == pick--)) {
            return c;
        }
    }
    return NULL;
}

static void fz_resign_header(struct fz_mut_s * m, int64_t offset) {
    struct jls_chunk_header_s hdr;
    memcpy(&hdr, m->buf + offset, sizeof(hdr));
    hdr.crc32 = jls_crc32c((const uint8_t *) &hdr, sizeof(hdr) - 4);
    memcpy(m->buf + offset, &hdr, sizeof(hdr));
}

static void fz_resign_payload(struct fz_mut_s * m, int64_t offset, uint32_t payload_length) {
    int64_t p = offset + FZ_CHUNK_HDR_SZ;
    int64_t crc_offset = p + fz_psod(payload_length) - 4;
    if (!payload_length || ((crc_offset + 4) > m->size)) {
        return;
    }
    fz_wr_u(m->buf + crc_offset, 4, jls_crc32c(m->buf + p, payload_length));
}

static void fz_mutate_header(struct fz_mut_s * m, const struct fz_chunk_s * c) {
    static const uint8_t TAGS[] = {
        JLS_TAG_INVALID, JLS_TAG_SOURCE_DEF, JLS_TAG_SIGNAL_DEF,
        JLS_TAG_TRACK_FSR_DEF, JLS_TAG_TRACK_FSR_HEAD, JLS_TAG_TRACK_FSR_DATA,
        JLS_TAG_TRACK_FSR_INDEX, JLS_TAG_TRACK_FSR_SUMMARY,
        JLS_TAG_TRACK_VSR_DEF, JLS_TAG_TRACK_VSR_HEAD, JLS_TAG_TRACK_VSR_DATA,
        JLS_TAG_TRACK_VSR_INDEX, JLS_TAG_TRACK_VSR_SUMMARY,
        JLS_TAG_TRACK_ANNOTATION_DEF, JLS_TAG_TRACK_ANNOTATION_HEAD,
        JLS_TAG_TRACK_ANNOTATION_DATA, JLS_TAG_TRACK_ANNOTATION_INDEX,
        JLS_TAG_TRACK_ANNOTATION_SUMMARY,
        JLS_TAG_TRACK_UTC_DEF, JLS_TAG_TRACK_UTC_HEAD, JLS_TAG_TRACK_UTC_DATA,
        JLS_TAG_TRACK_UTC_INDEX, JLS_TAG_TRACK_UTC_SUMMARY,
        JLS_TAG_USER_DATA, JLS_TAG_END,
    };
    static const uint16_t METAS[] = {0, 1, FZ_SIGNAL_ID_F32, FZ_SIGNAL_ID_SEC, 255, 256,
                                     0x0fff, 0x1000, 0xf000, 0xffff};
    uint8_t * h = m->buf + c->offset;
    switch (random_range_u32(0, 12)) {
        case 0: case 1: case 2: {  // header only: the chunk layout now lies
            uint32_t v = fz_pick_u32(c->payload_length);
            if (0 == random_range_u32(0, 4)) {
                v = (uint32_t) (m->size - c->offset);  // reaches EOF
            }
            fz_wr_u(h + FZ_HDR_OFF_PAYLOAD_LENGTH, 4, v);
            fz_desc_append(m, " hdr.payload_length=%" PRIu32, v);
            break;
        }
        case 3: {
            uint32_t v = fz_pick_u32((uint32_t) fz_rd_u(h + FZ_HDR_OFF_PAYLOAD_PREV, 4));
            fz_wr_u(h + FZ_HDR_OFF_PAYLOAD_PREV, 4, v);
            fz_desc_append(m, " hdr.payload_prev_length=%" PRIu32, v);
            break;
        }
        case 4: case 5: {
            uint64_t v = fz_pick_offset_value(m, c->offset);
            fz_wr_u(h + FZ_HDR_OFF_ITEM_NEXT, 8, v);
            fz_desc_append(m, " hdr.item_next=%" PRIu64, v);
            break;
        }
        case 6: case 7: {
            uint64_t v = fz_pick_offset_value(m, c->offset);
            fz_wr_u(h + FZ_HDR_OFF_ITEM_PREV, 8, v);
            fz_desc_append(m, " hdr.item_prev=%" PRIu64, v);
            break;
        }
        case 8: case 9: {
            uint16_t old = (uint16_t) fz_rd_u(h + FZ_HDR_OFF_CHUNK_META, 2);
            uint16_t v;
            switch (random_range_u32(0, 4)) {
                case 0: v = METAS[random_range_u32(0, sizeof(METAS) / sizeof(METAS[0]))]; break;
                case 1: v = (uint16_t) ((old & 0x0fff) | (random_range_u32(0, 16) << 12)); break;
                case 2: v = (uint16_t) ((old & 0xf000) | random_range_u32(0, 0x1000)); break;
                default: v = fz_pick_u16(old); break;
            }
            fz_wr_u(h + FZ_HDR_OFF_CHUNK_META, 2, v);
            fz_desc_append(m, " hdr.chunk_meta=0x%04x", (unsigned) v);
            break;
        }
        case 10: {
            uint8_t v = (random_u32() & 3) ? TAGS[random_range_u32(0, sizeof(TAGS))]
                                           : (uint8_t) random_u32();
            h[FZ_HDR_OFF_TAG] = v;
            fz_desc_append(m, " hdr.tag=0x%02x", (unsigned) v);
            break;
        }
        default:
            h[FZ_HDR_OFF_RSV0] = (uint8_t) random_u32();
            fz_desc_append(m, " hdr.rsv0=0x%02x", (unsigned) h[FZ_HDR_OFF_RSV0]);
            break;
    }
    fz_resign_header(m, c->offset);
}

// Mutate a u16/u32/i64 payload field at payload offset off, when in range.
static void fz_mut_field(struct fz_mut_s * m, uint8_t * p, uint32_t plen,
                         uint32_t off, uint32_t sz, const char * name) {
    if ((off + sz) > plen) {
        return;
    }
    uint64_t v;
    switch (sz) {
        case 2: v = fz_pick_u16((uint16_t) fz_rd_u(p + off, 2)); break;
        case 4: v = fz_pick_u32((uint32_t) fz_rd_u(p + off, 4)); break;
        default: v = (uint64_t) fz_pick_i64((int64_t) fz_rd_u(p + off, 8)); break;
    }
    fz_wr_u(p + off, sz, v);
    if (sz == 8) {
        fz_desc_append(m, " %s[@%" PRIu32 "]=%" PRIi64, name, off, (int64_t) v);
    } else {
        fz_desc_append(m, " %s[@%" PRIu32 "]=%" PRIu64, name, off, v);
    }
}

static void fz_mut_offset_field(struct fz_mut_s * m, const struct fz_chunk_s * c,
                                uint8_t * p, uint32_t off, const char * name) {
    if ((off + 8) > c->payload_length) {
        return;
    }
    uint64_t v = fz_pick_offset_value(m, c->offset);
    fz_wr_u(p + off, 8, v);
    fz_desc_append(m, " %s[@%" PRIu32 "]=%" PRIu64, name, off, v);
}

static void fz_mut_payload_header(struct fz_mut_s * m, uint8_t * p, uint32_t plen) {
    switch (random_range_u32(0, 7)) {
        case 0: case 1: fz_mut_field(m, p, plen, FZ_PH_OFF_TIMESTAMP, 8, "ph.timestamp"); break;
        case 2: case 3: fz_mut_field(m, p, plen, FZ_PH_OFF_ENTRY_COUNT, 4, "ph.entry_count"); break;
        case 4: case 5: fz_mut_field(m, p, plen, FZ_PH_OFF_ENTRY_SIZE_BITS, 2, "ph.entry_size_bits"); break;
        default: fz_mut_field(m, p, plen, FZ_PH_OFF_RSV16, 2, "ph.rsv16"); break;
    }
}

// Replace NUL terminators so strings run off the payload end.
static void fz_mut_unterminate(struct fz_mut_s * m, uint8_t * p, uint32_t plen) {
    uint32_t start = ((plen > 64) && (random_u32() & 1)) ? (plen - 64) : 0;
    for (uint32_t i = start; i < plen; ++i) {
        if (0 == p[i]) {
            p[i] = 'A';
        }
    }
    fz_desc_append(m, " unterminate[%" PRIu32 ":]", start);
}

static void fz_mut_bitflips(struct fz_mut_s * m, uint8_t * p, uint32_t plen) {
    uint32_t flips = random_range_u32(1, 9);
    for (uint32_t i = 0; i < flips; ++i) {
        p[random_range_u32(0, plen)] ^= (uint8_t) (1U << random_range_u32(0, 8));
    }
    fz_desc_append(m, " bitflip x%" PRIu32, flips);
}

static void fz_mut_signal_def(struct fz_mut_s * m, uint8_t * p, uint32_t plen) {
    // see jls_wr_signal_def: source_id u16 @0, signal_type u8 @2,
    // data_type @4, sample_rate @8, samples_per_data @12,
    // sample_decimate_factor @16, entries_per_summary @20,
    // summary_decimate_factor @24, annotation_decimate_factor @28,
    // utc_decimate_factor @32, reserved, then name and units strings
    static const uint32_t DT[] = {
        JLS_DATATYPE_U1, JLS_DATATYPE_U4, JLS_DATATYPE_U8, JLS_DATATYPE_U16,
        JLS_DATATYPE_U24, JLS_DATATYPE_U32, JLS_DATATYPE_U64,
        JLS_DATATYPE_I4, JLS_DATATYPE_I8, JLS_DATATYPE_I16, JLS_DATATYPE_I24,
        JLS_DATATYPE_I32, JLS_DATATYPE_I64, JLS_DATATYPE_F32, JLS_DATATYPE_F64,
        0, 0xFFFFFFFFU, 0x00000004U, 0x00000804U, 0x0000F00FU, 0x00FF1004U, 0x00201004U,
    };
    static const char * NAMES[] = {
        "def.sample_rate", "def.samples_per_data", "def.sample_decimate_factor",
        "def.entries_per_summary", "def.summary_decimate_factor",
        "def.annotation_decimate_factor", "def.utc_decimate_factor",
    };
    uint32_t f = random_range_u32(0, 13);
    switch (f) {
        case 0: fz_mut_field(m, p, plen, 0, 2, "def.source_id"); break;
        case 1: {
            uint8_t v = (random_u32() & 1) ? (uint8_t) random_range_u32(0, 3) : (uint8_t) random_u32();
            p[2] = v;
            fz_desc_append(m, " def.signal_type=%u", (unsigned) v);
            break;
        }
        case 2: case 3: {
            uint32_t v = (random_u32() & 3) ? DT[random_range_u32(0, sizeof(DT) / sizeof(DT[0]))]
                                            : fz_pick_u32((uint32_t) fz_rd_u(p + 4, 4));
            fz_wr_u(p + 4, 4, v);
            fz_desc_append(m, " def.data_type=0x%08" PRIx32, v);
            break;
        }
        case 11: fz_mut_unterminate(m, p, plen); break;
        case 12: {  // several factors at once, to get past single-field checks
            for (uint32_t k = 12; k <= 32; k += 4) {
                if (random_u32() & 1) {
                    uint32_t v = (random_u32() & 1) ? 1U : (1U << random_range_u32(0, 32));
                    fz_wr_u(p + k, 4, v);
                    fz_desc_append(m, " %s=%" PRIu32, NAMES[(k - 8) / 4], v);
                }
            }
            break;
        }
        default: {
            uint32_t k = f - 4;  // 0..6
            fz_mut_field(m, p, plen, 8 + 4 * k, 4, NAMES[k]);
            break;
        }
    }
}

static void fz_mut_annotation_data(struct fz_mut_s * m, uint8_t * p, uint32_t plen) {
    // annotation_type @16, storage_type @17, group_id @18, y @20, data_size @24
    switch (random_range_u32(0, 5)) {
        case 0: case 1: {
            uint8_t v = (random_u32() & 1) ? (uint8_t) random_range_u32(0, 6) : (uint8_t) random_u32();
            if (plen > 17) {
                p[17] = v;
            }
            fz_desc_append(m, " ann.storage_type=%u", (unsigned) v);
            break;
        }
        case 2: fz_mut_field(m, p, plen, 24, 4, "ann.data_size"); break;
        case 3: fz_mut_unterminate(m, p, plen); break;
        default: {
            uint8_t v = (uint8_t) random_u32();
            if (plen > 16) {
                p[16] = v;
            }
            fz_desc_append(m, " ann.annotation_type=%u", (unsigned) v);
            break;
        }
    }
}

static void fz_mutate_payload(struct fz_mut_s * m, const struct fz_chunk_s * c) {
    static const uint32_t F32[] = {0x7FC00000U, 0x7F800000U, 0xFF800000U, 0x7F7FFFFFU,
                                   0xFF7FFFFFU, 0x00000001U, 0x80000000U};
    uint8_t * p = m->buf + c->offset + FZ_CHUNK_HDR_SZ;
    uint32_t plen = c->payload_length;
    uint8_t tag = c->tag;
    uint8_t chunk_type = tag & 7;
    bool is_track = (0 != (tag & JLS_TRACK_TAG_FLAG));
    uint32_t entries = (plen > FZ_PH_SZ) ? (plen - FZ_PH_SZ) : 0;  // bytes after the payload header
    uint32_t r = random_range_u32(0, 16);

    if (r == 0) {
        fz_mut_bitflips(m, p, plen);
    } else if (r == 1) {
        // consistent shrink: a valid smaller chunk whose forward link
        // (by payload_length) lands inside its stale payload bytes
        uint32_t new_len = random_range_u32(0, plen);
        fz_wr_u(m->buf + c->offset + FZ_HDR_OFF_PAYLOAD_LENGTH, 4, new_len);
        fz_resign_header(m, c->offset);
        fz_desc_append(m, " shrink payload_length=%" PRIu32, new_len);
        if (new_len) {
            int64_t pad_start = c->offset + FZ_CHUNK_HDR_SZ + new_len;
            int64_t crc_offset = c->offset + FZ_CHUNK_HDR_SZ + fz_psod(new_len) - 4;
            memset(m->buf + pad_start, 0, (size_t) (crc_offset - pad_start));
        }
        plen = new_len;
    } else if (tag == JLS_TAG_SIGNAL_DEF) {
        fz_mut_signal_def(m, p, plen);
    } else if ((tag == JLS_TAG_SOURCE_DEF) || (tag == JLS_TAG_USER_DATA)) {
        if (r & 1) {
            fz_mut_unterminate(m, p, plen);
        } else {
            fz_mut_bitflips(m, p, plen);
        }
    } else if (is_track && (chunk_type == JLS_TRACK_CHUNK_HEAD)) {
        // struct jls_track_head_s: uint64_t offset[16]
        uint32_t level = (random_u32() & 1) ? random_range_u32(0, 3) : random_range_u32(0, 16);
        fz_mut_offset_field(m, c, p, level * 8, "head.offset");
    } else if (is_track && (chunk_type >= JLS_TRACK_CHUNK_DATA) && (r < 9)) {
        fz_mut_payload_header(m, p, plen);  // DATA, INDEX, SUMMARY
    } else if (tag == JLS_TAG_TRACK_FSR_INDEX) {
        if (entries >= 8) {
            fz_mut_offset_field(m, c, p, FZ_PH_SZ + 8 * random_range_u32(0, entries / 8),
                                "fsr_index.offsets");
        }
    } else if ((tag == JLS_TAG_TRACK_ANNOTATION_INDEX) || (tag == JLS_TAG_TRACK_UTC_INDEX)) {
        if (entries >= 16) {  // struct jls_index_entry_s
            uint32_t off = FZ_PH_SZ + 16 * random_range_u32(0, entries / 16);
            if (random_u32() & 1) {
                fz_mut_offset_field(m, c, p, off + 8, "index.offset");
            } else {
                fz_mut_field(m, p, plen, off, 8, "index.timestamp");
            }
        }
    } else if (tag == JLS_TAG_TRACK_ANNOTATION_DATA) {
        fz_mut_annotation_data(m, p, plen);
    } else if ((tag == JLS_TAG_TRACK_UTC_SUMMARY) || (tag == JLS_TAG_TRACK_ANNOTATION_SUMMARY)
               || (tag == JLS_TAG_TRACK_UTC_DATA)) {
        if (entries >= 8) {  // 16-byte entries with i64 fields; UTC data has one i64
            uint32_t off = FZ_PH_SZ + 8 * random_range_u32(0, entries / 8);
            fz_mut_field(m, p, plen, off, 8, "entry.i64");
        }
    } else if ((tag == JLS_TAG_TRACK_FSR_SUMMARY) && (entries >= 4)) {
        uint32_t off = FZ_PH_SZ + 4 * random_range_u32(0, entries / 4);
        fz_wr_u(p + off, 4, F32[random_range_u32(0, sizeof(F32) / sizeof(F32[0]))]);
        fz_desc_append(m, " summary.f32[@%" PRIu32 "]", off);
    } else {
        fz_mut_bitflips(m, p, plen);
    }
    fz_resign_payload(m, c->offset, plen);
}

// Build a resign case file from the pristine file.
static int32_t fz_make_resign(struct fz_s * fz, const struct fz_scan_s * scan,
                              const char * pristine_path, const char * case_path,
                              struct fz_case_s * cs) {
    struct fz_mut_s m = {.buf = NULL, .size = scan->fend, .scan = scan, .desc = cs->desc};
    FILE * f = fopen(pristine_path, "rb");
    if (!f) {
        return JLS_ERROR_IO;
    }
    m.buf = malloc((size_t) m.size);
    size_t sz = m.buf ? fread(m.buf, 1, (size_t) m.size, f) : 0;
    fclose(f);
    if (!m.buf || (sz != (size_t) m.size)) {
        free(m.buf);
        return JLS_ERROR_IO;
    }

    cs->desc[0] = 0;
    cs->offset = -1;
    uint32_t count = (fz->mode == FZ_MODE_RESIGN) ? random_range_u32(1, 4) : 1;
    for (uint32_t i = 0; i < count; ++i) {
        bool hdr = (fz->mode == FZ_MODE_RESIGN_HEADER)
                || ((fz->mode == FZ_MODE_RESIGN) && (random_u32() & 1));
        const struct fz_chunk_s * c = fz_pick_chunk(&m, !hdr);
        if (!c) {
            continue;
        }
        if (cs->offset < 0) {
            cs->offset = c->offset;
        }
        fz_desc_append(&m, "%s@%" PRIi64 " tag=0x%02x:", i ? " |" : "", c->offset, (unsigned) c->tag);
        if (hdr) {
            fz_mutate_header(&m, c);
        } else {
            fz_mutate_payload(&m, c);
        }
    }
    if (cs->offset < 0) {
        cs->offset = 0;
    }

    remove(case_path);
    int32_t rc = JLS_ERROR_IO;
    f = fopen(case_path, "wb");
    if (f) {
        if ((size_t) m.size == fwrite(m.buf, 1, (size_t) m.size, f)) {
            rc = 0;
        }
        fclose(f);
    }
    free(m.buf);
    return rc;
}

// Create the corrupted case file.  For most modes this corrupts a copy
// of the pristine file, and the pristine scan describes it.  For
// write_prefix the writer is stopped mid-run, producing a distinct byte
// layout; case_scan is then filled from the produced file and the
// caller must verify against it instead of the pristine scan.
static int32_t fz_make_case(struct fz_s * fz, const struct fz_cfg_s * cfg,
                            const struct fz_scan_s * scan, const char * pristine_path,
                            const char * case_path, struct fz_case_s * cs,
                            struct fz_scan_s * case_scan) {
    uint8_t buf[4096];
    memset(cs, 0, sizeof(*cs));
    cs->mode = fz->mode;
    cs->strict = (fz->mode != FZ_MODE_BITFLIP);
    cs->offset = scan->fend;
    cs->defs_ok = true;

    if (fz_mode_is_resign(fz->mode)) {
        cs->strict = false;
        cs->defs_ok = false;  // hostile file: no recovery expectations
        return fz_make_resign(fz, scan, pristine_path, case_path, cs);
    }

    if (fz->mode == FZ_MODE_WRITE_PREFIX) {
        int64_t budget = (fz->offset_forced >= 0)
                ? fz->offset_forced
                : random_range_i64(1, scan->write_total + 1);
        jls_bk_write_limit_set(budget);
        int32_t rc = fz_generate_mode(fz, cfg, case_path, true);
        jls_bk_write_limit_set(-1);
        if (rc) {
            return rc;
        }
        // the produced file's own layout is authoritative
        rc = fz_scan(case_path, case_scan);
        if (rc) {
            // too small to even scan (e.g. no file header): treat as
            // defs-damaged so a graceful open failure is acceptable
            case_scan->fend = 0;
            case_scan->defs_end = 0;
            case_scan->length = 0;
        }
        cs->offset = case_scan->fend;
        // signals are only expected once their SIGNAL_DEF chunk is fully
        // written; a shorter prefix may legitimately lack them
        bool has_def5 = false;
        bool has_def9 = false;
        for (size_t i = 0; i < case_scan->length; ++i) {
            if ((case_scan->chunks[i].tag == JLS_TAG_SIGNAL_DEF)
                    && (case_scan->chunks[i].end_offset <= case_scan->fend)) {  // complete payload
                if (case_scan->chunks[i].chunk_meta == FZ_SIGNAL_ID_F32) {
                    has_def5 = true;
                } else if (case_scan->chunks[i].chunk_meta == FZ_SIGNAL_ID_SEC) {
                    has_def9 = true;
                }
            }
        }
        cs->defs_ok = has_def5 && (!cfg->secondary || has_def9);
        return 0;
    }

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
            // Pick a chunk whose payload the writer rewrites in place.
            // Only HEAD payloads are updated in place; INDEX and SUMMARY
            // payloads are append-only, so tearing them models storage
            // corruption (bitflip/zero_hole class), not a crash.
            size_t candidates = 0;
            for (size_t i = 0; i < scan->length; ++i) {
                uint8_t t = scan->chunks[i].tag;
                if ((t & JLS_TRACK_TAG_FLAG) && scan->chunks[i].payload_length
                        && ((t & 7) == JLS_TRACK_CHUNK_HEAD)) {
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
                        && ((t & 7) == JLS_TRACK_CHUNK_HEAD)) {
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
            cs->defs_ok = (cs->offset >= scan->defs_end);
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

// Hostile-file callbacks touch every byte they are handed, so an
// out-of-bounds pointer or length becomes a sanitizer report.
struct fz_touch_s {
    uint64_t count;
    uint64_t sum;
};

static void fz_touch(struct fz_touch_s * t, const void * data, size_t size) {
    const uint8_t * p = (const uint8_t *) data;
    for (size_t i = 0; i < size; ++i) {
        t->sum += p[i];
    }
}

static int32_t fz_touch_ann_cbk(void * user_data, const struct jls_annotation_s * annotation) {
    struct fz_touch_s * t = (struct fz_touch_s *) user_data;
    ++t->count;
    fz_touch(t, annotation, sizeof(*annotation));
    fz_touch(t, annotation->data, annotation->data_size);
    return 0;
}

static int32_t fz_touch_utc_cbk(void * user_data, const struct jls_utc_summary_entry_s * utc, uint32_t size) {
    struct fz_touch_s * t = (struct fz_touch_s *) user_data;
    t->count += size;
    fz_touch(t, utc, sizeof(*utc) * (size_t) size);
    return 0;
}

static int32_t fz_touch_ud_cbk(void * user_data, uint16_t chunk_meta,
                               enum jls_storage_type_e storage_type,
                               uint8_t * data, uint32_t data_size) {
    struct fz_touch_s * t = (struct fz_touch_s *) user_data;
    (void) chunk_meta;
    (void) storage_type;
    ++t->count;
    fz_touch(t, data, data_size);
    return 0;
}

static void fz_touch_str(struct fz_touch_s * t, const char * s) {
    if (s) {
        fz_touch(t, s, strlen(s));
    }
}

// FSR read buffer size per the jls_rd_fsr contract (sub-byte types need
// one extra byte for shifting).  Sized exactly so ASan sees any overrun.
static size_t fz_fsr_buf_size(uint32_t entry_bits, int64_t length) {
    if (length <= 0) {
        return 1;
    }
    if (entry_bits < 8) {
        return 1 + (size_t) ((length * entry_bits) / 8);
    }
    return (size_t) ((length * entry_bits + 7) / 8);
}

#define FZ_HOSTILE_RD_MAX   (70000)  // samples per read; spans several data chunks
#define FZ_RC(name_, rc_) do {                                                  \
    int32_t rcv__ = (rc_);                                                      \
    if (!fz_error_code_valid(rcv__)) {                                          \
        FZ_FAIL(FZ_FAIL_ERROR_CODE, "%s: %s rc=%" PRIi32 " out of range",       \
                label, name_, rcv__);                                           \
    }                                                                           \
} while (0)

// Pick a start sample id around [0, n] including out-of-range extremes.
static int64_t fz_pick_start(int64_t n) {
    switch (random_range_u32(0, 10)) {
        case 0: return 0;
        case 1: return (n > 0) ? (n - 1) : 0;
        case 2: return n;
        case 3: return -1;
        case 4: return (random_u32() & 1) ? INT64_MAX : INT64_MIN;
        case 5: return (n > 10) ? (n - (int64_t) random_range_u32(1, 10)) : 0;
        default: return (n > 0) ? random_range_i64(0, n) : 0;
    }
}

static int64_t fz_pick_length(int64_t n, int64_t start) {
    int64_t avail = ((start >= 0) && (start <= n)) ? (n - start) : 0;
    switch (random_range_u32(0, 8)) {
        case 0: return 0;
        case 1: return 1;
        case 2: return (avail > 0) ? ((avail > FZ_HOSTILE_RD_MAX) ? FZ_HOSTILE_RD_MAX : avail) : 1;
        case 3: return (avail >= 0) && (avail < FZ_HOSTILE_RD_MAX) ? (avail + 1) : 2;  // one past end
        default: {
            int64_t max = (avail > 1) ? avail : 2;
            if (max > FZ_HOSTILE_RD_MAX) {
                max = FZ_HOSTILE_RD_MAX;
            }
            return random_range_i64(1, max + 1);
        }
    }
}

static uint32_t fz_hostile_fsr(struct jls_rd_s * rd, const struct jls_signal_def_s * def,
                               struct fz_touch_s * touch, const char * label, char * detail) {
    uint32_t fails = 0;
    uint16_t signal_id = def->signal_id;
    uint32_t entry_bits = jls_datatype_parse_size(def->data_type);
    int64_t n = 0;
    int32_t rc = jls_rd_fsr_length(rd, signal_id, &n);
    FZ_RC("fsr_length", rc);
    if (rc) {
        n = 0;
    }

    for (int k = 0; k < 4; ++k) {  // sample reads, native type
        int64_t start = fz_pick_start(n);
        int64_t len = fz_pick_length(n, start);
        size_t sz = fz_fsr_buf_size(entry_bits ? entry_bits : 64, len);
        uint8_t * buf = malloc(sz);
        if (!buf) {
            continue;
        }
        FZ_RC("fsr", jls_rd_fsr(rd, signal_id, start, buf, len));
        free(buf);
    }
    for (int k = 0; k < 2; ++k) {  // sample reads, f32 conversion
        int64_t start = fz_pick_start(n);
        int64_t len = fz_pick_length(n, start);
        float * buf = malloc(sizeof(float) * (size_t) ((len > 0) ? len : 1));
        if (!buf) {
            continue;
        }
        FZ_RC("fsr_f32", jls_rd_fsr_f32(rd, signal_id, start, buf, len));
        free(buf);
    }
    for (int k = 0; k < 5; ++k) {  // statistics
        int64_t sdf = def->sample_decimate_factor ? def->sample_decimate_factor : 1;
        const int64_t incrs[] = {1, 7, sdf, sdf * 3 + 1, (n > 1) ? n : 1};
        int64_t incr = incrs[k];
        if (incr > n) {
            incr = (random_u32() & 1) ? n : incr;
        }
        int64_t len = (random_u32() & 1) ? 1 : (int64_t) random_range_u32(1, 9);
        int64_t start = ((incr <= (n / len)) && (n > incr * len)) ? random_range_i64(0, n - incr * len + 1) : fz_pick_start(n);
        double * buf = malloc(sizeof(double) * JLS_SUMMARY_FSR_COUNT * (size_t) len);
        if (!buf) {
            continue;
        }
        FZ_RC("fsr_statistics", jls_rd_fsr_statistics(rd, signal_id, start, incr, buf, len));
        free(buf);
    }

    // time conversions and the time map
    const int64_t sample_ids[] = {0, n / 2, n, -1, INT64_MAX, INT64_MIN};
    const int64_t timestamps[] = {0, JLS_TIME_YEAR, -JLS_TIME_YEAR, INT64_MAX, INT64_MIN,
                                  (int64_t) random_u64()};
    for (size_t k = 0; k < 6; ++k) {
        int64_t v = 0;
        FZ_RC("sample_id_to_timestamp", jls_rd_sample_id_to_timestamp(rd, signal_id, sample_ids[k], &v));
        FZ_RC("timestamp_to_sample_id", jls_rd_timestamp_to_sample_id(rd, signal_id, timestamps[k], &v));
    }
    size_t tmap_len = jls_rd_tmap_length(rd, signal_id);
    const size_t tmap_idx[] = {0, tmap_len ? (tmap_len - 1) : 0, tmap_len, SIZE_MAX};
    for (size_t k = 0; k < 4; ++k) {
        struct jls_utc_summary_entry_s entry;
        FZ_RC("tmap_get", jls_rd_tmap_get(rd, signal_id, tmap_idx[k], &entry));
    }
    FZ_RC("utc", jls_rd_utc(rd, signal_id, 0, fz_touch_utc_cbk, touch));
    FZ_RC("utc_min", jls_rd_utc(rd, signal_id, INT64_MIN, fz_touch_utc_cbk, touch));
    return fails;
}

/**
 * @brief Exercise the full read API on a hostile (CRC-valid) file.
 *
 * Pass criteria: no crash, sanitizer report or hang (enforced by the
 * parent), and every call returns success or a JLS error code.
 */
static uint32_t fz_hostile_exercise(const char * path, const char * label, char * detail) {
    uint32_t fails = 0;
    struct jls_rd_s * rd = NULL;
    struct fz_touch_s touch = {0, 0};
    int32_t rc = jls_rd_open(&rd, path);
    FZ_RC("open", rc);
    if (rc) {
        return fails;
    }

    struct jls_source_def_s * sources = NULL;
    uint16_t source_count = 0;
    rc = jls_rd_sources(rd, &sources, &source_count);
    FZ_RC("sources", rc);
    for (uint16_t i = 0; !rc && (i < source_count); ++i) {
        fz_touch_str(&touch, sources[i].name);
        fz_touch_str(&touch, sources[i].vendor);
        fz_touch_str(&touch, sources[i].model);
        fz_touch_str(&touch, sources[i].version);
        fz_touch_str(&touch, sources[i].serial_number);
    }

    // copy the defs: reader calls may invalidate the signals array
    struct jls_signal_def_s * signals = NULL;
    uint16_t signal_count = 0;
    struct jls_signal_def_s defs[16];
    uint16_t def_count = 0;
    rc = jls_rd_signals(rd, &signals, &signal_count);
    FZ_RC("signals", rc);
    for (uint16_t i = 0; !rc && (i < signal_count); ++i) {
        fz_touch_str(&touch, signals[i].name);
        fz_touch_str(&touch, signals[i].units);
        if (def_count < 16) {
            defs[def_count++] = signals[i];
        }
    }

    for (uint16_t i = 0; i < def_count; ++i) {
        struct jls_signal_def_s def;
        FZ_RC("signal", jls_rd_signal(rd, defs[i].signal_id, &def));
        if (defs[i].signal_type == JLS_SIGNAL_TYPE_FSR) {
            fails |= fz_hostile_fsr(rd, &defs[i], &touch, label, (0 == fails) ? detail : (char[FZ_DETAIL_SZ]) {0});
        }
        FZ_RC("annotations", jls_rd_annotations(rd, defs[i].signal_id, INT64_MIN, fz_touch_ann_cbk, &touch));
        FZ_RC("annotations_mid", jls_rd_annotations(rd, defs[i].signal_id,
                                                    (int64_t) random_range_u32(0, 100000),
                                                    fz_touch_ann_cbk, &touch));
    }
    FZ_RC("user_data", jls_rd_user_data(rd, fz_touch_ud_cbk, &touch));
    jls_rd_close(rd);
    return fails;
}

static uint32_t fz_notes_;  // set in the verify child, see fz_result_s

static int64_t fz_file_size(const char * path) {
    FILE * f = fopen(path, "rb");
    int64_t sz = 0;
    if (f) {
        if (0 == fseek(f, 0, SEEK_END)) {
            sz = (int64_t) ftell(f);
        }
        fclose(f);
    }
    return sz;
}

// Hostile-file verification for the resign modes: exercise the reader on
// the case file (open may recover it in place), then jls_copy and
// jls_repair on the untouched copy, exercising each result that opens.
static uint32_t fz_hostile_verify(struct fz_s * fz, const char * case_path,
                                  const char * ro_path, char * detail) {
    uint32_t fails = 0;
    const char * label = "verify";
    char copy_path[600];
    snprintf(copy_path, sizeof(copy_path), "%s/case_copy.jls", fz->workdir);

    fails |= fz_hostile_exercise(case_path, "open", detail);

    remove(copy_path);
    // A forward sample_id jump in a hostile FSR data chunk makes the
    // writer fill up to JLS_WR_FSR_SKIP_MAX samples, so jls_copy output
    // can exceed the source by orders of magnitude.  Bound it with the
    // backend write budget and record a note rather than fill the disk.
    int64_t budget = 64 * fz_file_size(ro_path) + (16 << 20);
    int64_t written = jls_bk_write_total();
    jls_bk_write_limit_set(budget);
    int32_t rc = jls_copy(ro_path, copy_path, NULL, NULL, NULL, NULL);
    jls_bk_write_limit_set(-1);
    FZ_RC("jls_copy", rc);
    if ((jls_bk_write_total() - written) >= budget) {
        fz_notes_ |= (1U << FZ_NOTE_COPY_BUDGET);
        rc = JLS_ERROR_TOO_BIG;  // the truncated copy is not of interest
    }
    if (0 == rc) {
        fails |= fz_hostile_exercise(copy_path, "copy", (0 == fails) ? detail : (char[FZ_DETAIL_SZ]) {0});
    }
    remove(copy_path);

    rc = jls_repair(ro_path, NULL, NULL, NULL, NULL);
    FZ_RC("jls_repair", rc);
    if (0 == rc) {
        fails |= fz_hostile_exercise(ro_path, "repair", (0 == fails) ? detail : (char[FZ_DETAIL_SZ]) {0});
    }
    return fails;
}

// Run the oracle plus the read-only variant.  In an isolated child this
// runs with logging suppressed; a crash or hang is caught by the parent.
static uint32_t fz_case_verify(struct fz_s * fz, const struct fz_cfg_s * cfg,
                               const struct fz_scan_s * scan, const char * case_path,
                               const char * ro_path, const struct fz_case_s * cs,
                               uint64_t case_seed, char * detail) {
    uint32_t fails = 0;
    detail[0] = 0;
    if (fz_mode_is_resign(cs->mode)) {
        return fz_hostile_verify(fz, case_path, ro_path, detail);
    }
    // Modes that may produce damage beyond tail truncation.  write_prefix
    // belongs here: an in-place head update is two backend writes (payload,
    // then CRC footer), so a crash between them tears the head payload.
    const bool may_need_repair = (cs->mode == FZ_MODE_ZERO_HOLE)
            || (cs->mode == FZ_MODE_TORN_UPDATE)
            || (cs->mode == FZ_MODE_BITFLIP)
            || (cs->mode == FZ_MODE_WRITE_PREFIX);

    if (!may_need_repair) {
        // tail-truncation class: jls_rd_open must recover automatically
        fails = fz_oracle(fz, cfg, scan, case_path, cs, detail);
    } else {
        // open either succeeds or fails with MESSAGE_INTEGRITY, and
        // jls_repair must then restore the file
        struct jls_rd_s * rd = NULL;
        int32_t rc = jls_rd_open(&rd, case_path);
        if (0 == rc) {
            jls_rd_close(rd);
        } else if (!fz_error_code_valid(rc)) {
            FZ_FAIL(FZ_FAIL_ERROR_CODE, "open rc=%" PRIi32 " out of range", rc);
            return fails;
        } else if ((JLS_ERROR_MESSAGE_INTEGRITY != rc) && cs->defs_ok && cs->strict) {
            FZ_FAIL(FZ_FAIL_OPEN, "open rc=%" PRIi32 " %s with defs intact",
                    rc, jls_error_code_name(rc));
            return fails;
        }
        struct fz_case_s cs2 = *cs;
        rc = jls_repair(case_path, NULL, NULL, NULL, NULL);
        if (rc && cs->defs_ok) {
            FZ_FAIL(FZ_FAIL_REPAIR, "jls_repair rc=%" PRIi32 " %s",
                    rc, jls_error_code_name(rc));
            return fails;
        }
        if (0 == rc) {
            cs2.repaired = true;
            if ((cs->mode == FZ_MODE_ZERO_HOLE) || (cs->mode == FZ_MODE_BITFLIP)) {
                // dead data is summary-reconstructed and dead index/summary
                // spans may still fail reads: graceful-error checks only.
                // torn_update and write_prefix damage only head payloads and
                // the tail, so the full strict oracle must hold after repair.
                cs2.strict = false;
            }
        }
        fails = fz_oracle(fz, cfg, scan, case_path, &cs2, detail);
    }

    // read-only contract: a file needing truncation recovery must fail with
    // exactly JLS_ERROR_TRUNCATED; interior damage in a closed file may open
    // (reads detect it lazily)
#if !defined(_WIN32)
    bool needs_recovery = !cfg->closed
            || (((cs->mode == FZ_MODE_TRUNCATE)
                 || (cs->mode == FZ_MODE_ZERO_TAIL)
                 || (cs->mode == FZ_MODE_TORN_UPDATE)
                 || (cs->mode == FZ_MODE_WRITE_PREFIX))
                && (cs->offset < scan->fend));
    if (0 == (case_seed & 0xF)) {
        struct jls_rd_s * rd = NULL;
        chmod(ro_path, 0444);
        int32_t rc = jls_rd_open(&rd, ro_path);
        if (0 == rc) {
            jls_rd_close(rd);
            if (needs_recovery && cs->strict) {
                FZ_FAIL(FZ_FAIL_RDONLY, "read-only open of unrecovered file returned 0");
            }
        } else if (!fz_error_code_valid(rc)) {
            FZ_FAIL(FZ_FAIL_RDONLY, "read-only open rc=%" PRIi32 " out of range", rc);
        } else if (needs_recovery && cs->defs_ok && (JLS_ERROR_TRUNCATED != rc)) {
            FZ_FAIL(FZ_FAIL_RDONLY, "read-only open rc=%" PRIi32 " %s, expected TRUNCATED",
                    rc, jls_error_code_name(rc));
        }
        chmod(ro_path, 0644);
    }
#else
    (void) case_seed;
#endif

    // jls_repair must never make any case worse: with intact defs it must
    // succeed and yield an openable file; degenerate files must error cleanly
    int32_t repair_rc = jls_repair(ro_path, NULL, NULL, NULL, NULL);
    if (0 == repair_rc) {
        struct jls_rd_s * rd_repaired = NULL;
        repair_rc = jls_rd_open(&rd_repaired, ro_path);
        if (repair_rc) {
            FZ_FAIL(FZ_FAIL_REPAIR, "repaired copy open rc=%" PRIi32 " %s",
                    repair_rc, jls_error_code_name(repair_rc));
        } else {
            jls_rd_close(rd_repaired);
        }
    } else if (!fz_error_code_valid(repair_rc)) {
        FZ_FAIL(FZ_FAIL_ERROR_CODE, "repair rc=%" PRIi32 " out of range", repair_rc);
    } else if (cs->defs_ok) {
        FZ_FAIL(FZ_FAIL_REPAIR, "repair copy rc=%" PRIi32 " %s with defs intact",
                repair_rc, jls_error_code_name(repair_rc));
    }
    return fails;
}

struct fz_result_s {
    uint32_t fails;
    uint32_t notes;         // enum fz_note_e bitmask, not failures
    char detail[FZ_DETAIL_SZ];
};

static void fz_case_verify_r(struct fz_s * fz, const struct fz_cfg_s * cfg,
                             const struct fz_scan_s * scan, const char * case_path,
                             const char * ro_path, const struct fz_case_s * cs,
                             uint64_t case_seed, struct fz_result_s * result) {
    fz_notes_ = 0;
    result->fails = fz_case_verify(fz, cfg, scan, case_path, ro_path, cs,
                                   case_seed, result->detail);
    result->notes = fz_notes_;
}

// Run fz_case_verify in a child process so crashes and hangs are
// recorded instead of killing the campaign.
static void fz_case_run(struct fz_s * fz, const struct fz_cfg_s * cfg,
                        const struct fz_scan_s * scan, const char * case_path,
                        const char * ro_path, const struct fz_case_s * cs,
                        uint64_t case_seed, struct fz_result_s * result) {
    result->fails = 0;
    result->notes = 0;
    result->detail[0] = 0;
#if defined(_WIN32)
    fz_case_verify_r(fz, cfg, scan, case_path, ro_path, cs, case_seed, result);
#else
    if (!fz->isolate) {
        fz_case_verify_r(fz, cfg, scan, case_path, ro_path, cs, case_seed, result);
        return;
    }
    int fds[2];
    if (pipe(fds)) {
        fz_case_verify_r(fz, cfg, scan, case_path, ro_path, cs, case_seed, result);
        return;
    }
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        fz_case_verify_r(fz, cfg, scan, case_path, ro_path, cs, case_seed, result);
        return;
    }
    if (0 == pid) {  // child
        close(fds[0]);
        fz_log_quiet_count_ = 0;
        jls_log_register(fz_log_quiet);  // suppress log flood in bulk runs
        struct fz_result_s r;
        fz_case_verify_r(fz, cfg, scan, case_path, ro_path, cs, case_seed, &r);
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
    char orig_path[600];  // --keep: the case before verification modified it
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
    // the first-use oracle self-test draws random numbers; restore the
    // sequence so a --random repro matches a mid-campaign case
    uint64_t seed_restore = random_seed;
    int32_t ensure_rc = fz_ensure_pristine(fz, &cfg);
    random_seed = seed_restore;
    if (ensure_rc) {
        ++fz->fails[FZ_FAIL_GENERATE];
        ++fz->fail_count;
        return JLS_ERROR_UNSPECIFIED;  // generation/self-test failure is fatal
    }
    struct fz_scan_s * scan = &fz->scans[cfg_id];
    fz_path_pristine(fz, cfg_id, pristine_path, sizeof(pristine_path));

    snprintf(case_path, sizeof(case_path), "%s/case.jls", fz->workdir);
    snprintf(ro_path, sizeof(ro_path), "%s/case_ro.jls", fz->workdir);
    snprintf(orig_path, sizeof(orig_path), "%s/case_orig.jls", fz->workdir);
    struct fz_scan_s case_scan = {0};
    if (fz_make_case(fz, &cfg, scan, pristine_path, case_path, &cs, &case_scan)) {
        printf("case create failed\n");
        fz_scan_free(&case_scan);
        return JLS_ERROR_IO;
    }
    // write_prefix produces a distinct layout; verify against its own scan
    const struct fz_scan_s * vscan = (cs.mode == FZ_MODE_WRITE_PREFIX) ? &case_scan : scan;
    if (fz_copy_truncate(case_path, ro_path, INT64_MAX)
            || (fz->keep && fz_copy_truncate(case_path, orig_path, INT64_MAX))) {
        printf("case copy failed\n");
        fz_scan_free(&case_scan);
        return JLS_ERROR_IO;
    }

    if (fz->verbose) {
        printf("%20" PRIu64 " cfg=%2u %s @%9" PRIi64 "/%" PRIi64 " : ",
               case_seed, cfg_id, FZ_MODE_NAMES[cs.mode], cs.offset, vscan->fend);
        fflush(stdout);
    }
    fz_case_run(fz, &cfg, vscan, case_path, ro_path, &cs, case_seed, &result);
    uint32_t fails = result.fails;
    for (uint32_t bit = 0; bit < FZ_NOTE_COUNT; ++bit) {
        if (result.notes & (1U << bit)) {
            ++fz->notes[bit];
            if (fz->verbose || (fz->notes[bit] <= 3)) {  // a few repro seeds
                printf("%20" PRIu64 " cfg=%2u %s NOTE %s:%s\n", case_seed, cfg_id,
                       FZ_MODE_NAMES[cs.mode], FZ_NOTE_NAMES[bit], cs.desc);
            }
        }
    }

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
                   case_seed, cfg_id, FZ_MODE_NAMES[cs.mode], cs.offset, vscan->fend);
        }
        printf("FAIL [");
        for (uint32_t bit = 0; bit < FZ_FAIL_COUNT; ++bit) {
            if (fails & (1U << bit)) {
                printf(" %s", FZ_FAIL_NAMES[bit]);
            }
        }
        printf(" ] %s\n", result.detail);
        if (cs.desc[0]) {
            printf("    mutation:%s\n", cs.desc);
        }
        // the seed fully reproduces the case, but forced --cfg/--offset
        // change the random draw sequence so they must match this run
        printf("    repro: jls read_fuzzer --corrupt %s --random %" PRIu64,
               FZ_MODE_NAMES[fz->mode], case_seed);
        if (fz->cfg_forced >= 0) {
            printf(" --cfg %" PRIi32, fz->cfg_forced);
        }
        if (fz->offset_forced >= 0) {
            printf(" --offset %" PRIi64, fz->offset_forced);
        }
        printf(" --iterations 1 --workdir %s\n", fz->workdir);
        if (fz->keep) {
            char keep_path[700];
            snprintf(keep_path, sizeof(keep_path), "%s/fail_%" PRIu64 ".jls",
                     fz->workdir, case_seed);
            remove(keep_path);
            fz_copy_truncate(orig_path, keep_path, INT64_MAX);
            printf("    kept: %s\n", keep_path);
        }
    } else if (fz->verbose) {
        printf("PASS %s%s\n", result.detail, cs.desc);
    }
    fz_scan_free(&case_scan);
    remove(case_path);
    remove(ro_path);
    remove(orig_path);
    char copy_path[600];  // resign modes: jls_copy output of a crashed child
    snprintf(copy_path, sizeof(copy_path), "%s/case_copy.jls", fz->workdir);
    remove(copy_path);
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
    for (uint32_t bit = 0; bit < FZ_NOTE_COUNT; ++bit) {
        if (fz->notes[bit]) {
            printf("    note %-11s %" PRIu64 "\n", FZ_NOTE_NAMES[bit], fz->notes[bit]);
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
