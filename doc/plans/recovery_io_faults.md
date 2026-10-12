# Recovery and repair after I/O faults

Status: proposed (2026-10-12).  Found by the stage 5 fault sweeps in
doc/plans/completed/code_coverage.md.


## Problem

Crash recovery in jls_rd_open() and the offline jls_repair() modify the
file in place.  An I/O error or a crash part way through can leave a file
that the next open or repair cannot read, or that reads with wrong data.
test/read_fault_test.c sweeps one failure through every read and seek,
and every 13th write byte, of a recovery or a repair.  It then runs a
fault-free open, or repair then open, and compares every read with the
fault-free reference:

| Sweep          | Faults | Next open fails | Wrong data |
|----------------|--------|-----------------|------------|
| recovery read  | 421    | 13              | 17         |
| recovery write | 394    | 3               | 0          |
| repair read    | 1287   | 26              | 4          |
| repair write   | 399    | 0               | 0          |

A write fault fails that write and every later write, as in a crash
(power loss, process kill) during recovery.  A read fault fails one read
or seek, as for a transient device error, and later I/O works again.

ASan and LeakSanitizer report nothing.  Until `RECOVERY_STRICT` is set,
the sweeps require only that and print the counts above.


## Findings

1. Recovery treats read errors as the end of valid data.
   jls_core_repair_fsr() and the track repair walks `break` on any
   failed chunk read, so a transient read error looks like a truncated
   chain.  Recovery then writes summaries and indices for the shorter
   chain.  Example: with the read at fault 299 failing, the level 2
   summary write fails, and the next open finds a level 1 index at
   offset 103264 where the level 0 chain expects a data chunk
   (`tag 35 != expected 34`).  Some write failures are only logged
   ("could not create summary - repair may not work"), and recovery
   continues.
2. A torn chunk written by a failed recovery is not recoverable.  With
   the writes stopping at byte 3042, a chunk header and payload are
   written without its CRC footer.  The next recovery picks that chunk
   as the last valid chunk and then fails its payload CRC with
   JLS_ERROR_MESSAGE_INTEGRITY, instead of truncating it as a crash
   tail.
3. jls_repair() has the same read error handling as finding 1.
4. jls_raw_close() still writes the file header after recovery failed.
   Check whether this ever marks a partly recovered file as closed.

Fixed while adding the sweeps (CHANGELOG 0.19.0): a double free when
recovery failed to close the file, a reader that dropped a signal after
a transient seek error, and an incomplete UTC time map after a
transient read error.


## Stages

Recovery must keep visiting each chunk at most once, since customer
files are large (doc/plans/completed/recovery_single_pass.md).

1. Propagate I/O errors.  In recovery and repair, return
   JLS_ERROR_IO immediately, writing nothing more, and keep `break`
   only for corruption (CRC, tag, or length errors).  Return, do not
   log, failed summary and index writes.  Tests: the read sweeps reach
   0 wrong data.
2. Crash-consistent recovery writes.  The next recovery must accept
   any prefix of a previous recovery's writes: detect a final chunk
   whose payload or CRC footer is incomplete as a crash tail, and
   rebuild partial summaries and indices.  Resolve finding 4.  Tests:
   the write sweeps at step 1 reach 0 failures.
3. Set `RECOVERY_STRICT` in test/read_fault_test.c and remove the
   report-only mode.  All four sweeps must pass under ASan.
