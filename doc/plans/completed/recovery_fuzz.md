# JLS Recovery Fuzz Test Plan (steps 1 & 2)

Status: DONE (2026-08-07)
Scope: build a crash/truncation fuzz tester, run campaigns, and identify
every case where `jls_rd_open` or subsequent reads fail on a damaged file.
Fixing defects is out of scope here; findings feed
[recovery_defects.md](recovery_defects.md), which now lists F1-F9 plus
observations O1-O2.

Outcome: all six corruption modes implemented in
`example/jls/read_fuzzer.c` (truncate, zero_tail, zero_hole,
torn_update, bitflip, write_prefix) with child-process crash/hang
isolation and a pristine-reference oracle.  A 3000-case truncate
campaign failed 58% of cases; nine distinct defects were root-caused
and logged.  One fuzz-blocking memory-safety bug (F7, writer OOB read)
was fixed in place so ASan campaigns could run.


## Background

The JLS format is designed for recovery: every chunk has a header CRC32C
and a payload CRC32C, headers are 8-byte aligned, and chunks form doubly
linked lists.  `jls_rd_open` performs automatic repair when the final
chunk is not `JLS_TAG_END` (src/reader.c:99-150):

1. Reopen the file in `"a"` (write) mode.
2. Find the last valid chunk via backward scan (`jls_core_rd_chunk_end`,
   src/core.c:461) and truncate after it.
3. Rewrite the last chunk to fix `payload_prev_length`.
4. Repair per-track pointers (`jls_track_repair_pointers`, src/track.c:77)
   and FSR summaries (`jls_core_repair_fsr`).
5. Write a new `JLS_TAG_END` chunk and reopen read-only.

Field reports say recovery does not always work; the most common trigger
is unexpected file truncation after a process crash.

Existing coverage:

- `test/repair_test.c`: 4 fixed truncation scenarios, single f32 FSR
  signal only.
- `example/jls/read_fuzzer.c`: fuzzes read *parameters* (ranges,
  increments) against a valid file with guard-byte overflow detection.
  It never damages the file.  This plan extends it.


## Fault model

A process crash leaves the file holding exactly the completed `write()`
calls.  Because the writer seeks backward to update head/index chunks in
place, a crash-time file is NOT always a clean prefix of the byte
stream.  An OS crash or power loss additionally allows lost/zeroed tail
pages and zero holes from out-of-order writeback.

Corruption modes to implement, in priority order:

| Mode         | Models                              | Method                                  |
|--------------|-------------------------------------|-----------------------------------------|
| truncate     | reported common failure             | cut file at arbitrary byte offset       |
| zero_tail    | ext4 delayed alloc after power loss | keep length, zero last N bytes          |
| zero_hole    | out-of-order writeback              | zero an interior aligned region         |
| torn_update  | crash during in-place head rewrite  | corrupt payload of a HEAD/index chunk   |
| bitflip      | storage corruption (graceful-error  | flip 1-8 random bits                    |
|              | check only, recovery not expected)  |                                         |
| write_prefix | exact process-crash state space     | stop writer after N backend writes      |

`truncate` applies to both closed files (END chunk present) and
unclosed files (writer skipped `jls_wr_close`, as in `repair_test.c`
`GEN_SKIP_CLOSE`), at offsets swept across chunk boundaries +/- small
deltas as well as uniformly random offsets.

`write_prefix` needs a small fault-injection hook in the backend (stop
or fail after N `jls_bk_fwrite` calls).  This is the only mode that
requires a `src/` change; it is staged last and needs approval before
implementation (see Stage 4).


## Architecture

Extend `example/jls/read_fuzzer.c` (already registered as the
`read_fuzzer` command in example/jls.c:73).  Keep everything in C per
project preference for diagnostic tools.

New behavior, selected by arguments (existing parameter-fuzz behavior
remains the default so current usage is unchanged):

```
jls read_fuzzer <path>                       # existing: read-parameter fuzz
jls read_fuzzer --corrupt <mode> [options]   # new: corruption campaign
    --random <seed>       PRNG seed (default 1); every case derives from it
    --iterations <n>      number of cases (default: run until failure/quit)
    --workdir <dir>       where generated/corrupted temp files live
    --keep                keep failing case files for repro
    --child <case-spec>   run one case in-process (crash isolation child)
```

Components:

1. **Generator**: deterministic seeded content so the oracle can verify
   values.  Matrix varied per case: data types (f32, u1, u4, i16, ...),
   1..3 FSR signals, VSR signal, annotations, UTC entries, user data,
   sample counts from tiny (< 1 data chunk) to multi-summary-level,
   closed vs unclosed.  Reuse the triangle generator approach from
   `repair_test.c`.  Cache generated pristine files per configuration
   and re-corrupt copies, so generation cost is amortized.
2. **Corruptor**: applies one mode per case at a seeded location.
3. **Runner/child**: the runner spawns `jls read_fuzzer --corrupt ...
   --child <case-spec>` as a subprocess per case so SIGSEGV, aborts, and
   hangs (timeout) are recorded instead of killing the campaign.  The
   case-spec (seed + generation config + mode + offset) fully reproduces
   the case standalone.
4. **Oracle** (in the child), all steps must hold:
   - `jls_rd_open` returns 0, or a documented error code for cases
     where recovery is legitimately impossible (e.g. header destroyed).
     Crash, hang, sanitizer report, or undocumented error = FAIL.
   - Signal enumeration succeeds; `jls_rd_fsr_length` returns
     `0 <= n <= original`, and n >= samples covered by the last intact
     data chunk (recovery must not discard more than the damage).
   - `jls_rd_fsr` over the full recovered range returns values equal to
     the pristine reference; guard bytes intact (reuse existing guard
     logic).
   - `jls_rd_fsr_statistics` at several increments matches statistics
     computed from the reference data (tolerance as in repair_test).
   - Annotations, UTC, and user data iterate without error and return a
     prefix of the original entries.
   - **Idempotence**: close, reopen twice more; same length and values,
     and second open must not need repair (END chunk present).
   - **Read-only**: `chmod` the corrupted copy read-only and attempt
     open; must fail gracefully (documented error), never crash.
5. **Reporting**: one line per case (seed, mode, offset, verdict); on
   failure print the full repro command and keep the file when
   `--keep`.

Builds for campaigns: Debug + `-fsanitize=address,undefined` local
build of the `jls` example (no CMake changes required if configured via
`CMAKE_C_FLAGS`; add a cmake option only if that proves awkward).


## Targeted seed cases (from code inspection)

Feed these as directed cases before random campaigns; each is a
suspected defect or gap:

1. `jls_track_repair_pointers` level-0 tail walk failure updates
   `summary_chunk` instead of the just-modified `data_chunk`
   (src/track.c:170-173) — dangling `item_next` on the last data chunk.
   Target: truncate mid data-chunk so the walk fails.
2. `jls_rd_open` returns `JLS_ERROR_EMPTY` without cleanup when no
   chunk is found (src/reader.c:94-95) — leak; also exercises
   truncation inside the first few chunks.
3. Truncation before the first `SIGNAL_DEF` / inside the initial chunk
   group (`jls_core_scan_initial` expects USER_DATA, SOURCE_DEF and
   SIGNAL_DEF; heads at offset 0 then fail `jls_raw_chunk_seek`).
4. Annotation / UTC / VSR chains are not repaired (`todo` in
   src/reader.c:139-144): truncate between the last annotation/UTC
   chunk and file end, then iterate annotations/UTC.
5. Recovery requires write access: read-only file and read-only
   directory cases.
6. Crash *during recovery*: truncate an unclosed file, then interrupt
   the repair itself (via write_prefix hook once available, or a second
   truncation of the mid-repair file) and open again — repair must be
   restartable.
7. Truncation exactly at: END-chunk header, mid END payload, chunk
   header boundary, header+1, header+31, payload CRC field, and inside
   the 8-byte pad, for both the last data chunk and the last
   summary/index chunk.
8. Concurrent open of an unclosed file by two readers (both attempt
   repair on the same path) — at minimum must not corrupt the file.
   Single directed case, documented result.


## Stages

Per project guidance, each stage touches at most 3 files.

- **Stage 1 — generator + truncate + oracle** (files:
  `example/jls/read_fuzzer.c`): generation matrix, truncate mode,
  oracle for open/length/data/stats, idempotence.  DONE.
- **Stage 2 — crash isolation + remaining corruptors** (files:
  `example/jls/read_fuzzer.c`): forked-child runner with timeout,
  zero_tail, zero_hole, torn_update, bitflip, read-only cases.  DONE.
- **Stage 3 — campaigns**: seeded Debug and ASan/UBSan runs, triage.
  DONE (results in recovery_defects.md).  NOTE: the fuzzer runs each
  case in a forked child by default (`--no-isolate` to disable), so a
  Debug campaign survives crashes/hangs.  Step 3 fixed F8/F9, so
  full sanitizer campaigns now run clean (see recovery_defects.md
  verification results).
- **Stage 4 — write_prefix hook** (files: `include/jls/backend.h`,
  `src/raw.c`, `src/backend_posix.c`, `src/backend_win.c`,
  `example/jls/read_fuzzer.c`): `jls_bk_write_limit_set` write-budget
  cutoff; write_prefix mode generates the exact partial file a crashed
  writer leaves and verifies against its own scan.  DONE.
- **Stage 5 — regression capture**: MOVED to step 3
  (recovery_defects.md).  Rationale: a regression test must fail before
  its fix and pass after, but committing failing tests violates the
  project rule that the unit suite stays green.  Each defect therefore
  gets its deterministic cmocka case co-located with its fix in step 3.
  Until then the one-command fuzzer repros in recovery_defects.md are
  the regression reference.  The one landed fix (F7) is already guarded
  by running the existing unit suite under the ASan build.

Exit criteria for steps 1 & 2 (met): all six modes implemented; a
3000-case truncate campaign and per-mode campaigns triaged; every
unique failure has a one-command repro recorded in recovery_defects.md.
Larger overnight campaigns (>=100k) remain worthwhile but are gated on
the F8/F9 memory-safety fixes for the ASan variant.
