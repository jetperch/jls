# Regression review fixes (2026-10-12)

Status: IN PROGRESS.  Branch `fix/design_review` (e093a37) versus `main`
(6de5f41), reviewed after the design review in
`completed/design_review_2026_10.md` reported all findings fixed.

Method: four parallel line-by-line reviews (read path, write path,
raw/repair/copy/backend/tmap/statistics, pyjls), each with a `main`
worktree build and differential harnesses against both libraries; plus
ASan+UBSan ctest, the full `read_fault_test --full` and
`writer_test --full` sweeps, the pyjls suite, and a 20M-sample
cross-version benchmark.  Files written by `main` and the branch are
byte-identical; the old reader reads new files; u1/u4 reads are
bit-exact on the branch where `main` was wrong; recovery, copy, and
repair sweeps (about 4,900 cases) never recover less than `main`.

Severity: M = major, m = minor.


## Findings

| ID | Sev | Location | Defect |
|----|-----|----------|--------|
| M1 | M | writer.c wr_finalize | END written after a latched error hides unflushed data |
| M2 | M | wr_fsr.c, reader.c, core.c | fixed-point q: scaled summaries vs raw in old files |
| M3 | M | copy.c copy_chunk | rejected signal_def skipped, then every data chunk |
| M4 | M | pyjoulescope_ui exporter.py | Writer.close() raising skips !finalize |
| m1 | m | statistics.c combine | NaN mean from +/-inf treated as the invalid marker |
| m2 | m | reader.c fsr_statistics_samples | legacy 64-bit fallback buffers the increment |
| m3 | m | CHANGELOG.md | read-time signal definition caps not listed |
| m4 | m | binding.pyx time_map_length | raises for VSR signals; main returned 0 |
| m5 | m | threaded_writer.c | jls_wr_error() read unlocked; sync errors not latched |
| m6 | m | pyjoulescope_ui jls_v2.py | ChunkMeta.UI_META = 0x8001 uses reserved bits |
| m7 | m | test/jls_test.c, repair_test.c | shared temp filename fails `ctest -j` |
| m8 | m | reader.c, datatype.c | sub-byte sample statistics ~50% slower |

### M1: close after a latched error hides data

`wr_finalize()` frees the FSR and timestamp tracks without writing their
pending summaries, then writes END and the file header.  The reader
treats an END-terminated file as properly closed and skips recovery, so
data chunks after the last flushed level-1 summary are unreachable.
Disk-full repro: a crash recovers 1,220,608 samples per signal; close
after the error leaves 81,920 (f32) and 655,360 (u8).

Fix: when `self->error` is set, or any close step fails, skip
`jls_core_wr_end()`.  Still rewrite the file header so the file is
"not properly closed" and the reader's recovery rebuilds the indices and
summaries.  Test: latched error, close, reopen, `jls_rd_fsr_length()`
equals the crash-recovery length.  Correct the writer.h and
threaded_writer.h close documentation.

### M2: fixed-point q scaling is inconsistent across versions

The writer's `data_to_f64()` now calls `jls_dt_buffer_to_f64()`, which
applies 2^-q, so new summaries are stored scaled.  Files from 0.4 to
0.18 store raw summaries and `JLS_FORMAT_VERSION` is unchanged, so the
reader cannot tell them apart.  The reader scales samples but not
summary entries: one window mixes raw and scaled values.
`omitted_fill_byte()` receives the scaled mean, so omitted constant
chunks of q signals reconstruct the wrong raw byte.

Fix: store raw summaries (the writer summary path must not apply q) and
apply 2^-q when the reader converts summary entries.  Old and new files
are then identical.  `omitted_fill_byte()` then receives the raw mean.
Tests: statistics from summaries equal statistics from samples for a
q signal; a q file and a q=0 file with the same raw data differ only in
the data_type field; omitted q chunks reconstruct the raw value.

### M3: jls_copy drops a signal the destination writer rejects

`copy_chunk()` treats a `jls_wr_signal_def()` failure as a skippable
chunk; every data chunk then fails "fsr_data for undefined signal" and
copy returns 0.  The reader was relaxed for these files (G3) but copy's
destination writer was not.  Repro: a `main`-written u1 signal with
samples_per_data 6,553,600 reads fully but copies to nothing.

Fix: clamp samples_per_data and entries_per_summary to the write caps
before `jls_wr_signal_def()` (copy re-chunks data through
`jls_wr_fsr()`), and make a remaining signal_def failure fatal.  Test
in repair_offline_test.c with an oversized definition.

### M4: pyjoulescope_ui Exporter and Writer.close()

`Exporter._run` uses `with Writer(path) as jls:` and runs from
`range_tool.__run_outer` with no exception handling.  A latched write
error raises from `__exit__`, so the done callbacks and `!finalize` are
skipped and the partial file remains.  Fix in pyjoulescope_ui: catch
exceptions in `_run`, report through `self.error()`, remove the partial
file, and always finalize.

### m1: jls_statistics_combine invalid marker

The invalid branch keys on `isnan(mean)`, but a window holding +inf and
-inf has a NaN mean with valid min and max.  Key on
`isnan(min) && isnan(max)`, which `jls_statistics_invalid()` sets.

### m2: legacy 64-bit summary fallback cost

`fsr_statistics_samples()` allocates `increment * 8` bytes and reads
the whole window.  Combine per data chunk with
`jls_statistics_combine()` instead of buffering the increment.

### m3: document the read-time caps

List the caps from format.h in the CHANGELOG 0.19.0 entry.

### m4: time_map_length for VSR signals

Return 0 for non-FSR signals, as `main` did, instead of raising.

### m5: threaded writer latch

Read `jls_wr_error()` before `jls_bkt_process_unlock()`, and latch it
after the synchronous `jls_twr_source_def()` and `jls_twr_signal_def()`.

### m6: UI_META constant

Change `ChunkMeta.UI_META` to a value within bits 11:0.  It is defined
but never written.

### m7: shared test temp filename

`jls_test.c` and `repair_test.c` both use `jls_test_tmp.jls`.  Give
`repair_test.c` its own name.

### m8: sub-byte statistics performance

40M u1/u4 samples of sample-path statistics take 0.044 s on the branch
and 0.029 s on `main`.  Find the cause (datatype conversion or the NaN
skip) and recover the speed if the fix is local.


## Stages

1. M1, m5: writer close and threaded writer latch, with tests.
2. M2, m2, m4: reader and writer q handling, legacy fallback, pyjls.
3. M3, m1, m7: copy, statistics combine, test filenames.
4. M4, m6: pyjoulescope_ui.
5. m8: performance.
6. m3 and CHANGELOG; move this plan to completed.
