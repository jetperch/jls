# Recovery and repair after I/O faults

Status: complete (2026-10-12).  Found by the stage 5 fault sweeps in
doc/plans/completed/code_coverage.md.


## Problem

Crash recovery in jls_rd_open() and the offline jls_repair() rewrite the
file in place.  test/read_fault_test.c simulates two kinds of I/O fault
during recovery, repair, and jls_copy(), then checks the result with a
fault-free open:

* A read fault fails one read or seek, then I/O works again, like a
  transient error on a USB drive or network share.
* A write fault fails that write and every later write, like a crash or
  power loss part way through recovery.

These faults are simulated.  No customer file is known to have hit them.

Before this fix, with a 30000 sample test file, a read fault could leave
a file that no longer opened, or that opened with the end of a signal
missing (up to 29% of the samples) or some UTC entries missing.  The
samples it did return were correct.  jls_repair() could not restore
these files.  A crash during recovery could also leave a file that no
longer opened, but jls_repair() fully restored those.  A read fault
during jls_copy() could skip a chunk and still report success.


## Cause

Recovery, repair, and copy treated a failed read as damage or as the end
of the data.  They then truncated the tail, clamped chains, dropped
summary levels, marked live data chunks dead, or skipped chunks, and
wrote the result.  A few failed writes were only logged, and recovery
continued.


## Fix

Recovery, repair, and copy now stop with JLS_ERROR_IO on any I/O error,
without acting on the failed read.  After recovery or repair stops, the
file is as if the process had crashed at that point.  So that
JLS_ERROR_IO means only a backend failure:

* jls_raw_chunk_seek() returns JLS_ERROR_MESSAGE_INTEGRITY for a corrupt
  offset of 0 or less.
* jls_core_rd_chunk() returns JLS_ERROR_TRUNCATED for a payload past the
  file end.
* A chunk header read inside the file returns JLS_ERROR_IO, and one past
  the file end returns JLS_ERROR_EMPTY.
* Opening a file fails if a seek fails while finding the file end.

The changed sites: the end chunk search and orphan relink of the tail
truncation, the chain clamp walk, the user data chain, the track pointer
repair, the FSR summary rebuild including its summary writes, the
jls_repair() traversal and dead entry check, and the jls_copy() chunk
loop.

A crash during recovery can still leave a file that the next open
rejects, and jls_repair() restores it.  We chose not to make recovery
itself crash-consistent: the window is short, and jls_repair() covers
it.


## Tests

For every fault, the next open (or repair then open, or the copy) must
return exactly the fault-free data, or fail.  When an open fails,
jls_repair() must then restore exactly the fault-free data.  Data loss
is never allowed.

ctest samples the fault points (every 5th read or seek, every 37th write
byte) on a 12000 sample file: about 1 s in Release and 2.5 s with ASan,
with nearly the coverage of the full sweep.
`read_fault_test --full` tries every fault point: about 1 minute in
Release and 2 minutes with ASan.  Its results:

| Sweep          | Faults | Data lost | Needed jls_repair() |
|----------------|--------|-----------|---------------------|
| recovery read  | 377    | 0         | 0                   |
| recovery write | 4704   | 0         | 32                  |
| repair read    | 759    | 0         | 0                   |
| repair write   | 4776   | 0         | 0                   |

Disabling each fix, one at a time, makes the default sampled run fail,
except for two guards that this test file does not reach: the dead entry
check in jls_repair(), which reads only when a file has more damaged
regions than repair tracks, and the user data header refresh after
recovery clamps that chain.

The read fault must fail exactly one call.  With a fault that fails
every later read too, the next guarded read also fails, so disabling
any one guard went unnoticed.
