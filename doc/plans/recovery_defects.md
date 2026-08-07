# JLS Recovery Defect Remediation Plan (step 3)

Status: FRAMEWORK — populated as [recovery_fuzz.md](recovery_fuzz.md)
campaigns produce findings.  Do not start fixes until the finding is
recorded here with a repro and a regression test exists in
`test/repair_test.c`.


## Findings log

Format, one entry per unique defect:

```
### F<n>: <one-line summary>
- Discovered: <date>, fuzz mode <mode>, seed <seed>
- Repro: jls read_fuzzer --corrupt <mode> --child <case-spec>
- Class: <see classification>
- Symptom: <crash | error code | wrong data | ...>
- Root cause: <file:line analysis>
- Regression test: repair_test.c:<test name>
- Fix: <planned approach>  Status: OPEN | FIXED | WONTFIX
```

### F1: Recovery leaves dangling FSR data head; open then fails EMPTY
- Discovered: 2026-08-07, fuzz mode truncate, seed 1
- Repro: jls read_fuzzer --corrupt truncate --random 1 --cfg 12 --offset 5911 --iterations 1
- Class: 2 (unrecoverable open)
- Symptom: jls_rd_open returns JLS_ERROR_EMPTY on a file whose defs are
  intact but where truncation removed every complete data chunk.  The
  repair itself completes and rewrites the file, yet open still fails,
  and all later opens fail the same way.
- Root cause: `jls_track_repair_pointers` (src/track.c:108-162) level-0
  walk does not clear `track->head_offsets[0]` when the first data
  chunk is beyond the truncation point, then writes the stale head to
  the file.  `jls_core_scan_fsr_sample_id` (called from
  src/reader.c:129) seeks the dangling offset, gets EMPTY from
  `jls_core_rd_chunk`, and the GOE aborts the whole open.
- Regression test: TODO (stage 5)
- Fix: clear head_offsets[0] in the repair walk when the first data
  chunk is unreachable; make scan_fsr_sample_id treat an unreadable
  data head as "no data" instead of a fatal open error.
  Status: OPEN

### F2: jls_bk_fread error log says "write mismatch"
- Discovered: 2026-08-07 during F1 triage
- Class: cosmetic
- Root cause: copy-paste in src/backend_posix.c:146 (fread short-read
  path logs "write mismatch").
- Fix: change message to "read mismatch".  Status: OPEN

### F3: annotation/UTC/user-data iteration errors after recovery
- Discovered: 2026-08-07, fuzz mode truncate, campaign seed 12345
- Repro (examples):
  jls read_fuzzer --corrupt truncate --random 8706103446039621978 --cfg 29 --offset 732001 --iterations 1
  jls read_fuzzer --corrupt truncate --random 6475571562195355053 --cfg 30 --offset 17267 --iterations 1
  jls read_fuzzer --corrupt truncate --random 5719033040969464515 --cfg 25 --offset 79049 --iterations 1
- Class: 2 (iteration APIs return errors on recovered files)
- Symptom: `jls_rd_annotations` / `jls_rd_utc` / `jls_rd_user_data`
  return EMPTY (13), NOT_FOUND (16), or MESSAGE_INTEGRITY (9) instead
  of iterating the surviving entries.  By far the most frequent
  failure: ~60% of truncate cases with annotations enabled.
- Root cause (partial): annotation/UTC/VSR chains are never repaired
  (`todo` at src/reader.c:139-144), so item_next pointers dangle past
  EOF or into rewritten chunks; iteration follows them and errors out.
- write_prefix confirmation (cleanest repro, no byte corruption):
  the write_prefix campaign fails ONLY with utc_count, always at a
  round multiple of utc_decimate_factor (reader returns 100/200/300...
  when 113/246/399 UTC entries are actually present).  The reader
  surfaces only entries already folded into a UTC summary; the tail
  UTC data chunks written after the last summary are lost because the
  UTC index/summary chain is not rebuilt on repair.  Same mechanism
  applies to annotations.
- Regression test: TODO (stage 5)
- Fix: repair ts-track (UTC/annotation) chains like FSR — rebuild the
  index/summary from the surviving data chunks — and/or make iteration
  stop gracefully at a dangling/invalid chunk.  Status: OPEN

### F4: recovery discards intact samples (primary tail chunk; secondary
    signal near-total loss)
- Discovered: 2026-08-07, fuzz mode truncate, campaign seed 12345
- Repro (examples):
  jls read_fuzzer --corrupt truncate --random 8491820169393132966 --cfg 19 --offset 188487 --iterations 1
    (primary n=28080, intact 29120: one full data chunk lost)
  jls read_fuzzer --corrupt truncate --random 14242569707943863013 --cfg 10 --offset 111206 --iterations 1
    (secondary n=1, intact 20480: near-total loss)
- Class: 3 (silent excess data loss)
- Symptom: after successful repair, `jls_rd_fsr_length` reports fewer
  samples than are stored in fully intact data chunks below the
  truncation point.  Two magnitudes: exactly one trailing chunk
  (suspect src/track.c:170-173 updating `summary_chunk` where
  `data_chunk` was modified), and near-total collapse of the second
  FSR signal (n=1..40 with hundreds of thousands intact).
- Root cause (secondary collapse, verified on repro cfg=10 seed
  14242569707943863013): dangling pointer re-binding.  Truncation
  leaves stale item_next/index offsets pointing past EOF.  Repair
  processes signals in order and *appends* rebuilt index/summary/END
  chunks at exactly those offsets, so a later signal's stale pointers
  now reference valid but foreign chunks (e.g. signal 9's data chain
  ends at signal 5's rebuilt level-2 index).  Repair walks
  (`jls_core_repair_fsr` level-0 loop) and `jls_core_fsr_length` never
  validate chunk tag/chunk_meta, so the foreign chunk is consumed as
  signal-9 data: its header (timestamp=0, entry_count=1) yields
  length 1.  The same mechanism produces the F6 infinite loop.
- Regression test: TODO (stage 5)
- Fix direction: invalidate dangling pointers (zero item_next of each
  chain tail) BEFORE appending any repair chunks, and validate
  tag/meta on every chain/index hop in repair and read paths.
  Status: OPEN (root-caused)

### F5: fsr_length errors and non-idempotent reopen on some truncations
- Discovered: 2026-08-07, fuzz mode truncate, campaign seed 12345
- Repro:
  jls read_fuzzer --corrupt truncate --random 15123140413808120767 --cfg 7 --offset 434314 --iterations 1
  jls read_fuzzer --corrupt truncate --random 6005057263588564345 --cfg 20 --offset 5480089 --iterations 1
- Class: 2/5 (open succeeds but length query fails; reopen differs)
- Symptom: first open succeeds, `jls_rd_fsr_length` returns
  MESSAGE_INTEGRITY (9) or IO (5); reopen reports a different state.
- Regression test: TODO (stage 5)
- Fix: TBD after root cause.  Status: OPEN

### F6: infinite loop + unbounded logging reading a recovered file
- Discovered: 2026-08-07, fuzz mode truncate, campaign seed 12345
  (case following seed 16653658899299844132; wrote a 12 GB log and
  filled /tmp before being killed)
- Class: 1 (hang / resource exhaustion)
- Symptom: after a "successful" recovery, a sample read loops forever
  logging "unexpected chunk tag: 35 (expected 34)" /
  "unexpected chunk meta: 4101 (expected 9)".
- Root cause (partial): `jls_core_rd_fsr_data0` (src/core.c:1041-1046)
  only warns when the chunk an index points to is not a
  TRACK_FSR_DATA chunk for the expected signal, then proceeds; with a
  garbage/zero entry_count the caller's read loop makes no progress
  and retries the same chunk forever.  The repaired file's level-1
  index pointing at an INDEX chunk is itself a repair defect to
  root-cause.
- Regression test: TODO (stage 5)
- Fix: rd_fsr_data0 must return an error on tag/meta mismatch, and the
  read loops must fail on no-progress; separately fix the repair that
  produced the bad index.  Status: OPEN

### F7: writer out-of-bounds read on every byte-aligned FSR write
- Discovered: 2026-08-07, ASan build, during pristine file generation
  (not corruption-dependent; affects every producer)
- Class: 1 (memory safety)
- Symptom: AddressSanitizer heap-buffer-overflow READ of 1 byte in
  `wr_data_inner` (src/wr_fsr.c:484) via jls_wr_fsr_f32.
- Root cause: the byte-aligned path unconditionally saved
  `src_u8[byte_length]` into shift_buffer, reading one byte past the
  caller's buffer whenever the written span ends on a byte boundary.
  The saved byte is only meaningful for sub-byte data types with a
  trailing partial byte.
- Regression test: existing unit suite under ASan now covers it.
- Fix: read the trailing byte only when `(length * bits) & 7` is
  nonzero.  Status: FIXED (2026-08-07, fuzz-blocker; needed to run
  ASan campaigns)

### F8: heap-buffer-overflow in jls_core_repair_fsr (memcpy)
- Discovered: 2026-08-07, ASan campaigns (truncate, zero_tail,
  torn_update all abort here)
- Class: 1 (memory safety)
- Symptom: AddressSanitizer heap-buffer-overflow WRITE in
  `jls_core_repair_fsr` (src/core.c:1329),
  `memcpy(track_fsr->data, buf->start, buf->length)`.  The data buffer
  is sized for one data chunk (samples_per_data * dt), but the chunk
  read at the walked offset can be larger (a summary/foreign chunk
  reached via a dangling item_next, per F4/F6).
- Root cause: same dangling-pointer re-binding as F4/F6, plus no
  tag/meta/size validation before the memcpy.
- Regression test: TODO (stage 5)
- Fix: validate the chunk (tag == TRACK_FSR_DATA, chunk_meta ==
  signal, payload_length <= data buffer size) before copying, and
  bound the memcpy.  Shares the F4/F6 fix.  Status: OPEN

### F9: reader instance leaked on failed open
- Discovered: 2026-08-07, ASan LeakSanitizer (cfg=26 seed
  10223456587216945989 and any early-EMPTY open)
- Class: 1 (resource leak; C2 confirmed)
- Symptom: 21 allocations (~3.7 MB in that case) leaked when
  `jls_rd_open` returns without freeing `self`.
- Root cause: `jls_rd_open` early `return JLS_ERROR_EMPTY`
  (src/reader.c:94-95) and possibly other non-`goto exit` returns skip
  `jls_rd_close(self)`.
- Regression test: TODO (stage 5); run under LSan.
- Fix: route all failure returns in jls_rd_open through the exit
  cleanup.  Status: OPEN

### O2: open performs no repair for mid-file damage in closed files
- Discovered: 2026-08-07, torn_update + zero_hole smoke runs
- Class: 2 (architectural)
- Symptom: 7/8 torn_update cases fail open with MESSAGE_INTEGRITY.
  `jls_rd_open` runs scan_initial/scan_sources/scan_signals with
  fatal ROE *before* any repair, and repair only triggers when the
  final chunk is not JLS_TAG_END.  Any damaged def/head chunk in an
  otherwise closed file makes open fail with no repair attempt.
- Fix: TBD (step 3 design decision: tolerate/rebuild damaged heads,
  and consider repair for mid-file damage).  Status: OPEN

### O1: FSR statistics are approximate for entry-misaligned multi-entry
    reads (observation, needs Matt's ruling)
- Discovered: 2026-08-07 via oracle self-test on an undamaged file
- Detail: the writer adjusts signal def parameters (requested
  samples_per_data=1000 / sample_decimate_factor=100 became 1040/104).
  `jls_rd_fsr_statistics` with data_length > 1 and increment not a
  multiple of the (adjusted) sample_decimate_factor weights whole
  summary entries fractionally at output boundaries, giving mean
  errors ~ (sdf/increment) * range (measured 3e-4 on a +/-1 triangle,
  increment 10000).  The data_length == 1 path recurses to level 0 and
  is exact - inconsistent precision between the two paths.  The fuzz
  oracle now uses entry-aligned windows for exact checks and a
  proportional tolerance otherwise.  Question: is the approximation
  intended/documented, and should the multi-entry path recurse for
  boundary entries like the single-entry path does?

Candidate defects already suspected from code inspection — confirm via
fuzzer before promoting to findings:

- C1: `jls_track_repair_pointers` updates `summary_chunk` where
  `data_chunk` was modified (src/track.c:170-173).
- C2: `jls_rd_open` early return leaks reader instance
  (src/reader.c:94-95).
- C3: annotation/UTC/VSR chains never repaired (src/reader.c:139-144).
- C4: recovery impossible without write permission (design gap).
- C5: repair is not atomic; crash during repair may lose more data.


## Campaign results (2026-08-07)

Debug build, 3000 truncate cases (seed 31337), child-isolated:

| category         | count | finding |
|------------------|-------|---------|
| ann_error        | 906   | F3 |
| user_data_error  | 855   | F3 |
| utc_error        | 739   | F3 |
| excess_loss      | 529   | F4 |
| utc_count        | 510   | F3 |
| open_fail        | 113   | F1 |
| crash            | 34    | F8 (repair memcpy overflow) |
| data_mismatch    | 32    | F5 |
| not_idempotent   | 32    | F5 |
| hang             | 14    | F6 (repair index loop) |

1741/3000 cases failed (58%).  Child isolation recorded all 34 crashes
and 14 hangs without aborting the campaign.  Dominant category is the
unrepaired ts-track chains (F3: ann + utc + user_data ≈ 3010 hits
across cases).  ASan/UBSan builds abort on the first F8 overflow, so
memory-safety fixes (F7 done, F8/F9 open) must precede a full
sanitizer campaign.

write_prefix (400 cases): failures are exclusively utc_count — the
cleanest F3 repro, no byte corruption.  bitflip (400 cases): no strict
failures (graceful-error-only mode), memory-safety still enforced.


## Classification and fix policy

Ordered by severity; fix in this order:

1. **Memory safety / crash / hang** (segfault, sanitizer report,
   overflow past guard, infinite loop): always fix in read path.  A
   damaged file must never crash a reader, even for corruption we do
   not recover (bitflip mode).
2. **Unrecoverable open** (open fails on a file whose data chunks are
   intact): fix recovery so the intact prefix is readable.
3. **Silent excess data loss** (recovery succeeds but discards more
   samples than the damage requires): fix repair walk/truncation point.
4. **Wrong data** (recovered reads differ from pristine reference, or
   statistics inconsistent with samples): highest data-integrity
   priority after crashes.
5. **Non-idempotent repair** (second open changes length/values, or
   repair of a repaired file fails): fix ordering of repair writes;
   consider writing END chunk last as commit point.
6. **Design gaps** (read-only recovery, concurrent repair): propose
   design options to Matt before implementing; likely candidates are
   in-memory repair for read-only opens and advisory locking or
   O_EXCL sidecar during repair.

Policy boundaries:

- Recovery is only *required* for crash-consistent damage (truncate,
  zero_tail, zero_hole, torn_update, write_prefix).  For bitflip
  damage, graceful documented errors are sufficient.
- Never regress the format: fixes must not change on-disk layout;
  format changes require a separate proposal.


## Workflow per finding

1. Confirm repro on current head; record entry in the findings log.
2. Distill to a deterministic regression test in `test/repair_test.c`
   (or a new `recovery_test.c` if repair_test grows past ~600 lines);
   test must fail before the fix.
3. Root-cause with the repro under a debugger/sanitizer; write the
   analysis into the finding entry.
4. Fix within the existing recovery framework (reader.c / track.c /
   core.c); no code duplication; stay <= 3 files per fix.
5. Verify: regression test passes, full unit suite passes, then re-run
   the originating fuzz campaign seed plus a fresh 10k-case campaign in
   the affected mode.
6. Mark FIXED with commit reference.

Completion criteria for step 3: all findings FIXED or WONTFIX with
rationale, full fuzz campaign (all modes, ASan/UBSan) passes the exit
criteria in recovery_fuzz.md with zero unique failures, and CHANGELOG
updated.
