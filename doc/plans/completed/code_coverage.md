# C unit test code coverage

Status: complete (2026-10-12).  Stages 1 to 5 done; stage 6 not planned.
Stage 5 found recovery and repair defects, now in
doc/plans/recovery_io_faults.md.
Baseline measured on branch `fix/design_review` with the uncommitted
statistics tests.


## How to measure

See doc/coverage.md.


## Baseline (ctest, 17 test executables)

| Metric    | Covered     | Percent |
|-----------|-------------|---------|
| Lines     | 4262 / 4852 | 87%     |
| Branches  | 2415 / 3324 | 72%     |
| Functions | 308 / 314   | 98%     |

Lowest line coverage: datatype.c 72%, backend_posix.c 78%, copy.c 79%,
raw.c 80%.  Lowest branch coverage: ec.c 5% (name tables), copy.c 46%,
backend_posix.c 60%, raw.c 65%, repair.c 69%.  statistics.c and log.c
are at 100%.

After stages 1, 2, and 4 (18 test executables): lines 4326 / 4852 (89%),
branches 2515 / 3324 (75%), functions 312 / 314.  datatype.c and ec.c
are at 100%.

After stage 5 (19 test executables): lines 4416 / 4855 (91%), branches
2696 / 3322 (81%), functions 316 / 317.  copy.c rose from 79% to 91% of
lines and from 46% to 63% of branches.  The one function not covered,
`signal_tracks_free()`, needs an allocation failure (stage 6).


## Findings

Functions never called by any test:

* `jls_raw_chunk_tell_end()` (src/raw.c): no caller and no declaration
  in any header.  Dead code.
* `jls_version_str()` and `jls_version_u32()` (src/raw.c): public API,
  untested.
* `jls_dt_str()` (src/datatype.c): used by example/jls/info.c, untested.
* `construct_f64()` (src/core.c): rebuilds an omitted f64 data chunk.
  The writer omits only types of 8 bits or less, so only a crafted or
  legacy file reaches it.  `construct_f32()` is covered.
* `signal_tracks_free()` (src/writer.c): only reached when signal
  definition fails part way, such as an allocation failure.

Most other uncovered lines and branches are error returns: allocation
failures, backend I/O errors, and corrupt-file checks.  Write I/O
errors already have fault injection (`jls_bk_write_limit_set`, see
test/writer_test.c); allocation and read errors do not.


## Stages

1. Done.  The `JLS_COVERAGE` CMake option, doc/coverage.md, and the
   Linux CI `coverage` job that uploads the HTML report.
2. Done (test/raw_test.c, test/datatype_test.c, test/ec_test.c).  Test
   the untested public functions: `jls_version_str()`,
   `jls_version_u32()`, `jls_dt_str()`, and the `jls_tag_to_name()` and
   ec.c name tables.
3. Done.  Removed `jls_raw_chunk_tell_end()`.
4. Done (`test_fsr_omit_reconstruct_f64` in test/core_test.c).  Cover
   `construct_f64()` with a crafted omitted f64 chunk.
5. Done (test/read_fault_test.c).  `jls_bk_read_limit_set()` fails
   reads and seeks after an operation budget.  The test sweeps one
   failure through every read and seek of the reader, crash recovery,
   jls_repair(), and jls_copy(), and through the writes of the last
   three.  After the fault, the same reader, or a fault-free open,
   repair, or copy, must return the fault-free results.  Fixed defects
   it found: a double free when recovery failed to close the file, a
   reader that dropped a signal after a transient seek error, and an
   incomplete UTC time map after a transient read error.  Recovery and
   repair still leave inconsistent files after some faults: see
   doc/plans/recovery_io_faults.md.
6. Not planned.  Allocation failure coverage would need a test-only
   allocator hook, and its complexity outweighs the value of these paths
   (decided 2026-10-12).
