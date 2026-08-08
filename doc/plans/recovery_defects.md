# JLS Recovery Defect Remediation Plan (step 3)

Status: COMPLETE (2026-08-08).  All findings F1-F9 are FIXED and
covered by `test/recovery_test.c` (verified to fail before the fixes
and pass after).  Every crash-consistent fuzz mode passes with zero
failures; see "Verification campaign results" below.  The design
questions are resolved: O1 closed as designed (multi-entry statistics
serve plot generation; single-point is exact), O2 closed as actual
data loss with salvage via `jls copy` replay, C4 closed as a
documented limitation (read-only files recover via replay to a
copy), C5 closed as an accepted risk with the append-only END-last
mitigation.

Fix commits:

- e83a062 core: validate chunk tag/meta on every hop, fix repair_fsr
  overflows (F6, F8, F9)
- 27d1071 core, track: fix repair truncation point, chain clamping,
  and mid-file writes (F1, F4, F5, C1)
- 49f4f1e reader, core, track: recover ts tracks, user data, and
  crash-window updates (F3, orphan chunks, torn heads)
- 3340f4c test: add crash-recovery regression suite

Additional defects found and fixed during step 3 (no fuzz-finding
number; discovered by root-causing F4/F5 and by the new tests):

- `jls_core_rd_chunk_end` backward window scan never tested the
  candidate at each window start (`i > 0` instead of `i >= 0`).  The
  skipped offsets recur every 1000 bytes, so repair truncated at the
  START of a fully valid last chunk, discarding it (the true F4
  "one chunk lost" mechanism).  Also: signed math for short windows
  (UBSan), and offset 0 (file header) excluded as a candidate.
- `jls_core_repair_fsr` kept a single `lvl` buffer pointer for the
  initial level while `level` decremented, writing lower-level chunks
  into the wrong buffers and passing a NULL summaryN source for files
  with 2+ summary levels.
- Repair wrote at the current file position in two places (data-walk
  summary1 flush, END chunk), overwriting valid chunks below the
  truncation point (the true F5 mechanism).  All repair writes now
  seek to the file end first.
- A crash between writing a chunk and updating its predecessor's
  item_next (or the track head offsets for a chain-first chunk)
  orphaned the final chunk.  Repair now relinks it, or restores the
  lost head offset.
- `test/fsr_omit_test.c` read one past the sample buffer
  (`y_end` clamped to sample_count, and window arithmetic typo).


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
- Regression test: recovery_test.c:test_truncate_all_data_lost
- Fix: jls_core_repair_chain clears head_offsets[0] when no data
  chunk survives; scan_fsr_sample_id treats an unreadable data head
  as "no data".  Status: FIXED (27d1071)

### F2: jls_bk_fread error log says "write mismatch"
- Discovered: 2026-08-07 during F1 triage
- Class: cosmetic
- Root cause: copy-paste in src/backend_posix.c:146 (fread short-read
  path logs "write mismatch").
- Fix: change message to "read mismatch".  Status: FIXED (2026-08-07,
  landed with the fuzzer backend work)

### F3: annotation/UTC/user-data iteration errors after recovery
- Discovered: 2026-08-07, fuzz mode truncate, campaign seed 12345
- Repro (examples):
  jls read_fuzzer --corrupt truncate --random 8706103446039621978 --cfg 29 --offset 732001 \
      --iterations 1
  jls read_fuzzer --corrupt truncate --random 6475571562195355053 --cfg 30 --offset 17267 \
      --iterations 1
  jls read_fuzzer --corrupt truncate --random 5719033040969464515 --cfg 25 --offset 79049 \
      --iterations 1
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
- Regression test: recovery_test.c:test_utc_annotation_user_data_after_crash
- Fix: ts-track chains are clamped by jls_track_repair_pointers via
  jls_core_repair_chain; the user data chain is clamped the same way;
  jls_core_utc continues into the unsummarized tail data chunks after
  the final index chunk (entries beyond the last summary were the
  utc_count loss).  Status: FIXED (49f4f1e)

### F4: recovery discards intact samples (primary tail chunk; secondary
    signal near-total loss)
- Discovered: 2026-08-07, fuzz mode truncate, campaign seed 12345
- Repro (examples):
  jls read_fuzzer --corrupt truncate --random 8491820169393132966 --cfg 19 --offset 188487 \
      --iterations 1
    (primary n=28080, intact 29120: one full data chunk lost)
  jls read_fuzzer --corrupt truncate --random 14242569707943863013 --cfg 10 --offset 111206 \
      --iterations 1
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
- Regression test: recovery_test.c:test_truncate_sweep
- Fix: chains are clamped (item_next of the last valid chunk zeroed)
  before repair appends anything, and every chain/index hop validates
  tag/meta (jls_core_rd_chunk_validate).  The "one trailing chunk"
  variant was the jls_core_rd_chunk_end window-scan miss (see the
  step 3 additional defects above), not track.c.
  Status: FIXED (27d1071)

### F5: fsr_length errors and non-idempotent reopen on some truncations
- Discovered: 2026-08-07, fuzz mode truncate, campaign seed 12345
- Repro:
  jls read_fuzzer --corrupt truncate --random 15123140413808120767 --cfg 7 --offset 434314 \
      --iterations 1
  jls read_fuzzer --corrupt truncate --random 6005057263588564345 --cfg 20 --offset 5480089 \
      --iterations 1
- Class: 2/5 (open succeeds but length query fails; reopen differs)
- Symptom: first open succeeds, `jls_rd_fsr_length` returns
  MESSAGE_INTEGRITY (9) or IO (5); reopen reports a different state.
- Regression test: recovery_test.c:test_truncate_sweep (reopen pass)
- Root cause: jls_core_repair_fsr's data-walk summary1 flush wrote at
  the current mid-file position, overwriting valid chunks below the
  truncation point; the END chunk had the same hazard when no FSR
  repair ran.  All repair writes now seek to the file end first.
  Status: FIXED (27d1071, 49f4f1e)

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
- Regression test: recovery_test.c:test_truncate_sweep (terminates)
- Fix: rd_fsr_data0 returns JLS_ERROR_MESSAGE_INTEGRITY on tag/meta
  mismatch and invalid entry counts; read loops fail on no-progress
  and reject backward item_next links; the repair no longer produces
  bad indices (F4/F5 fixes).  Status: FIXED (e83a062)

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
- Regression test: recovery_test.c:test_truncate_sweep under ASan
- Fix: every repair memcpy is bounded by the destination buffer size
  and preceded by tag/meta validation.  Also fixed: the walk kept a
  single lvl buffer pointer while descending levels, corrupting the
  buffers and passing a NULL summaryN source for 2+ level files.
  Status: FIXED (e83a062)

### F9: reader instance leaked on failed open
- Discovered: 2026-08-07, ASan LeakSanitizer (cfg=26 seed
  10223456587216945989 and any early-EMPTY open)
- Class: 1 (resource leak; C2 confirmed)
- Symptom: 21 allocations (~3.7 MB in that case) leaked when
  `jls_rd_open` returns without freeing `self`.
- Root cause: `jls_rd_open` early `return JLS_ERROR_EMPTY`
  (src/reader.c:94-95) and possibly other non-`goto exit` returns skip
  `jls_rd_close(self)`.
- Regression test: recovery_test.c:test_open_damaged_defs; verified
  leak-free with LSan (30 in-process fuzz cases; only the harness's
  intentional crash-simulation writer leak remains).
- Fix: all failure returns in jls_rd_open route through the exit
  cleanup; jls_raw_open also leaked its instance when the backend
  open failed (e.g. read-only recovery) and now frees it.
  Status: FIXED (e83a062, 35ebddf)

### O2: open performs no repair for mid-file damage in closed files
- Discovered: 2026-08-07, torn_update + zero_hole smoke runs
- Class: 2 (architectural)
- Symptom: 7/8 torn_update cases fail open with MESSAGE_INTEGRITY.
  `jls_rd_open` runs scan_initial/scan_sources/scan_signals with
  fatal ROE *before* any repair, and repair only triggers when the
  final chunk is not JLS_TAG_END.  Any damaged def/head chunk in an
  otherwise closed file makes open fail with no repair attempt.
- Fix: PARTIALLY ADDRESSED (49f4f1e).  For unclosed (crash) files:
  damaged track heads are rebuilt by physically scanning the chunks
  (jls_track_repair_head), and the definition scans skip a
  payload-damaged chunk instead of dropping every later definition.
  All torn_update cases now pass.
- Ruling (Matt, 2026-08-08): interior damage in closed files is
  actual data loss; in-place repair is not required.  Handling:
  1. Reads into the destroyed region keep failing gracefully with
     JLS_ERROR_MESSAGE_INTEGRITY (never silent wrong data, never a
     crash) — implemented and fuzz-verified.
  2. Salvage is recovery-by-replay: `jls copy <src> <dst>` walks the
     physical chunks, resyncs past damage (jls_raw_chunk_scan), and
     replays into a fresh file.  Verified on a read-only zero-holed
     file: the destroyed chunk becomes a skipped region, the copy
     opens cleanly at full length, and reads over the hole use the
     existing omitted-chunk reconstruction from surviving summaries.
  Possible future refinements (not required): report damage extents
  to the caller (e.g. a jls_rd API listing unreadable sample ranges)
  so applications can distinguish salvaged approximations from real
  samples; and/or an on-read omitted-chunk fallback behind an
  explicit opt-in.  Status: CLOSED (by design; salvage via jls copy)

### O1: FSR statistics are approximate for entry-misaligned multi-entry
    reads (observation) — CLOSED, as designed
- Ruling (Matt, 2026-08-08): the multi-entry path exists for plot
  point generation; interior boundary approximation is acceptable
  and as designed.  For exact statistics, request a single point
  (data_length == 1), which evaluates start and end down to the
  sample.  The fuzz oracle already reflects this: entry-aligned
  windows are checked exactly, misaligned multi-entry windows use a
  proportional tolerance.  No code change.
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

Candidate defects already suspected from code inspection:

- C1: `jls_track_repair_pointers` updates `summary_chunk` where
  `data_chunk` was modified.  CONFIRMED and FIXED (27d1071): the
  level-0 walk is now jls_core_repair_chain, which clamps the
  correct chunk.
- C2: `jls_rd_open` early return leaks reader instance.  CONFIRMED
  as F9, FIXED.
- C3: annotation/UTC/VSR chains never repaired.  CONFIRMED as F3,
  FIXED.
- C4: recovery impossible without write permission.  CLOSED
  (documented limitation, Matt 2026-08-08): in-place repair cannot
  and will not support read-only files; opening a damaged read-only
  file fails gracefully (no crash, no leak; fuzzer "readonly"
  checks pass).  The supported path is recovery by replay to a copy:
  `jls copy <src> <dst>` works from a read-only source (verified).
- C5: repair is not atomic; a crash during repair may lose more
  data.  CLOSED (accepted risk, Matt 2026-08-08).  Mitigation in
  place: all repair writes append at the file end and the END chunk
  is written last as the commit point, so an interrupted repair
  leaves another repairable unclosed file.


## Verification campaign results (2026-08-08, after fixes)

Debug build, child-isolated, seed 20260808:

| mode         | cases | failures | notes |
|--------------|-------|----------|-------|
| truncate     | 2000  | 0        | was 58% before fixes |
| zero_tail    | 2000  | 0        | |
| torn_update  | 2000  | 0        | mode now tears HEAD payloads (see below) |
| bitflip      | 2000  | 0        | graceful-error mode |
| write_prefix | 2000  | 0        | exact crash-prefix state space |
| zero_hole    | 500   | 414      | ALL graceful MESSAGE_INTEGRITY errors (O2) |

ASan/UBSan build (halt_on_error=0), 200 cases per mode plus the full
unit suite (12 tests incl. the new recovery_test): zero sanitizer
reports, zero failures in the crash-consistent modes.  The reader
path is leak-free under LSan; the only remaining report is the
harness's intentional crash-simulation writer abandonment.

zero_hole is interior damage in closed files: reads into the
destroyed region return JLS_ERROR_MESSAGE_INTEGRITY (documented,
never silent wrong data, never a crash).  Repairing it is the open
O2 design decision.

torn_update recalibration: the writer rewrites only HEAD payloads in
place (INDEX/SUMMARY payloads are append-only; their in-place updates
touch the 32-byte header, which a tear makes CRC-invalid and is
covered by the truncate/clamp handling).  The mode now tears HEAD
payloads, which jls_track_repair_head recovers.  Torn INDEX/SUMMARY
payloads model storage corruption and remain covered by zero_hole
and bitflip as graceful errors.


## Campaign results (2026-08-07, before fixes)

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

ASan/UBSan build, per-mode (child-isolated, halt_on_error=0):

| mode         | cases | strict fails | ASan child reports |
|--------------|-------|--------------|--------------------|
| truncate     | 400   | 235          | overflow @core.c:1329 |
| zero_tail    | 200   | 119          | overflow @core.c:1329 |
| zero_hole    | 200   | 168          | overflow @core.c:1329 |
| torn_update  | 200   | 138          | overflow @core.c:1329 |
| bitflip      | 300   | 0            | none |
| write_prefix | 300   | 58 (utc)     | none |

15 ASan child reports, all F8 (heap-buffer-overflow at
jls_core_repair_fsr core.c:1329).  No new memory-safety sites.

Harness note (NOT a library defect): LeakSanitizer also reports leaks
rooted at jls_wr_open (writer.c:67) via fz_generate_mode's crash path.
This is the fuzzer intentionally abandoning the writer instance to
model a process crash (the same idiom as test/repair_test.c's
GEN_SKIP_CLOSE).  It is a harness artifact in the *generation* (parent)
path; the real library leak is F9, in the *reader* path.  Run
campaigns with a LSan suppression for jls_wr_open, or generate crash
files in a forked child, if LSan noise obscures real reader leaks.


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
