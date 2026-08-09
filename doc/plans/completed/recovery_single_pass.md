# Recovery: Fast Automatic Truncation Path + Offline jls_repair

## Status

COMPLETE (2026 Aug 9).  The "Open follow-up" items below are optional
optimizations, deliberately deferred, not remaining scope.

## Requirements (Matt, 2026-08-08)

1. `jls_rd_open()` attempts tail-truncation recovery as needed
   (quick).  On read-only or failure, return JLS_ERROR_TRUNCATED.
2. On any other corruption that requires offline recovery, return
   JLS_ERROR_MESSAGE_INTEGRITY.  The user may try an offline
   jls_repair operation.
3. jls_repair accepts a progress callback (jls_progress_fn, the
   refactored jls_copy_progress_fn) and updates appropriately.
4. Recovery must traverse each chunk at most once; O(crash tail) for
   the common truncation case, never O(file) at open.

## Damage classes

The file is append-ordered.  In-place updates target either the
chain tails (item_next relinks, physically near the file end) or the
track HEAD payloads (defs region).  A head payload update is two
backend writes (payload, then CRC footer), so a crash can tear it.

- Class A, tail truncation (common: crash, power loss, arbitrary
  cut): recovered automatically by jls_rd_open in O(tail).
- Class B, torn/lost track HEAD payload: open fails with
  MESSAGE_INTEGRITY; jls_repair rebuilds the payload in place (or
  appends a replacement head linked into the signal chain).
- Class C, interior corruption (bit errors, zeroed regions): open
  succeeds when defs are intact; reads fail lazily with
  MESSAGE_INTEGRITY.  jls_repair relinks chains around dead chunks
  and zeroes their level-1 index entries so reads reconstruct the
  spans from summaries (fsr_omit mechanism).
- Class D, damaged defs region: open fails with MESSAGE_INTEGRITY.
  jls_repair relinks the defs chains around dead chunks; dead
  SOURCE_DEF/SIGNAL_DEF payloads are not reconstructed (v1).

## Implementation summary

- jls_rd_open reorder (src/reader.c): rd_chunk_end + truncation run
  BEFORE the defs scans, so post-truncate the discriminator is the
  file end: reads past `fend` are chain terminations (recovery mode),
  anything else is corruption.  Scans clamp dangling defs-chain links
  so the repaired file passes the strict (clean-file) scans on
  reopen.  All recovery failures map to TRUNCATED except
  MESSAGE_INTEGRITY and NOT_ENOUGH_MEMORY.
- Applicability gate: `jls_core_track_applicable()` (shared with
  jls_core_validate_track_tag) mirrors jls_wr_signal_def; parent ==
  NULL on an applicable track of a truncated file means "never
  written", which is lossless to skip.  jls_track_repair_head and its
  full-file scans are removed from the open path.
- Bounded level-0 clamp (src/track.c): the clamp starts from the last
  valid entry of the last valid level-1 index chunk.  For a signal
  whose tail is fully omitted (constant fsr_omit data), the backward
  item_prev hop over all-omitted indexes interleaves with a forward
  walk of the data chain from head_offsets[0], stopping at whichever
  finishes first: cost is min(trailing omitted index chunks, total
  data chunks), each chunk visited at most once, so neither a long
  omitted tail nor a long data prefix forces an O(file) walk
  (test_recovery_traversal_bound_omitted_tail).
- jls_repair (src/repair.c): one forward traversal validating every
  chunk header+payload, building per-chain tables (first, last-valid)
  keyed by (signal, track, level, kind) plus defs chains and heads.
  Fixes: relink around dead chunks (only when the stale link is
  nonzero, so prior clamps are not resurrected), clamp dangling chain
  ends, rebuild torn/stale/lost heads from the physically-scanned
  firsts, zero dead-data level-1 index entries, then finalize through
  the standard jls_rd_open recovery (tail summaries + END).
- TS (annotation/UTC) tracks weave ONE mixed chain
  (data -> index -> summary -> data) that readers walk inline, and a
  crash can leave an index whose summary was never written.  The
  chain model above is therefore FSR-only: repair leaves TS chains
  and intact TS heads untouched (torn TS heads are rebuilt with only
  the level-0 data offset) and the standard open recovery clamps the
  mixed chain, exactly as a plain recovery would.
- Verification: raw-layer read counters (jls_raw_rd_header_total /
  jls_raw_rd_payload_total) let recovery_test assert the open-time
  traversal bound directly (30x file size must not increase reads
  materially).  read_fuzzer criteria per mode:
  truncate/zero_tail: open == 0.  write_prefix/torn_update: open in
  {0, MESSAGE_INTEGRITY}; on MESSAGE_INTEGRITY, jls_repair == 0 then
  the full strict oracle must pass.  zero_hole/bitflip: repair == 0
  then graceful-error checks.  Every case also repairs a copy, which
  must yield an openable file.  Campaign 2026-08-09: 2000 cases per
  mode, zero failures.

## v1 limitations (documented, acceptable)

- Data inside dead chunks is lost; spans whose covering level-1
  summary also died still fail reads (open succeeds).
- Dead SOURCE_DEF/SIGNAL_DEF payloads are not reconstructed; the
  signal disappears.  Dead source_head/signal_head chunks at the
  file start leave the file unopenable (defs region damage).
- TS (annotation/UTC) chains are not relinked around dead chunks and
  their index entries are not zeroed (FSR only); annotation/UTC reads
  over interior damage may fail after repair.
- If a signal's first data chunk dies, repair repoints
  head_offsets[0] to the first surviving chunk; sample numbering
  shifts by the lost chunk (sample_id_offset changes).
- Full salvage of read-only or heavily damaged files remains
  `jls copy`.

## Open follow-up (optional)

- repair_fsr re-walks the top index level that repair_pointers just
  walked (small, geometrically decimated).  Hand the per-level tail
  state across to remove the retrace entirely.
- Byte-granular write fault injection in the fuzzer (current budget
  is whole-write granular) to model torn 32-byte header rewrites.
