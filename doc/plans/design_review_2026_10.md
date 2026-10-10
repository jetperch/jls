# JLS Design Review (2026-10)

Status: IN PROGRESS.  Full review of the C library and pyjls after the
0.19.0 argument-validation pass (branch `fix/closed_handle`).  Fixes on
branch `fix/design_review`.

Method: five parallel line-by-line reviews (core read/index, reader and
statistics, writer and threaded writer, raw/backend/repair/copy, pyjls),
each confirming memory-safety findings with ASan/UBSan repros against
crafted files whose chunk CRCs were re-signed.  The existing
`read_fuzzer` campaign (all 6 modes, ~30k cases) was clean because it
never re-signs CRCs, so a mutated header always fails the CRC check;
the hostile-but-CRC-valid file space was untested.

Severity: C = critical (memory corruption), M = major, m = minor.
"valid" marks defects reachable from writer-produced files or plain API
calls, not just hostile files.


## Stage 1: buffer, raw, backend, copy, repair, tmap, log

| ID  | Sev | Location | Defect |
|-----|-----|----------|--------|
| R1  | C   | raw.c payload_size_on_disk | uint32 wrap; CRC reads ~4 GiB past buffer on open |
| R2  | C v | buffer.c jls_buf_realloc | squares size (1 MiB -> 1 TiB); cur/end not rebased (UAF) |
| R3  | M v | core.c jls_core_rd_chunk | grows to payload_length, not on-disk size: hang near 1 MiB |
| R4  | C v | buffer.c jls_buf_string_save | string > 1 MiB overflows the strings block |
| R5  | M   | buffer.c jls_buf_rd_str | string straddling a strings page truncates / stale buf_end |
| R6  | M   | raw.c jls_raw_chunk_scan | advances twice per block (skips half); tail < 32 B loops forever |
| R7  | M   | raw.c jls_raw_wr_payload | writes hdr->payload_length, ignores arg; repair stack over-read |
| R8  | M   | repair.c heads_rebuild, track.c | rewrites heads whose on-disk length mismatches |
| R9  | M   | copy.c | trusts entry_count/data_size; leaks buf/rd/wr; resync drops chunk |
| R10 | M   | backend_win.c finalize | frees bk while thread may run (1 s join timeout) |
| R11 | M   | backend_win.c truncate | `_chsize_s` failure (positive errno) never detected |
| R12 | m   | raw.c read_verify | UNSUPPORTED_FILE masked as TRUNCATED; uninit header read |
| R13 | m   | log.c | missing commas shift level names |
| R14 | m   | backend_posix.c | `_FILE_OFFSET_BITS` ineffective (32-bit large files) |
| R15 | m   | raw.c close/flush | jls_raw_close always 0; fflush returns -1 not JLS code |
| R16 | m   | raw.c append | last_payload_length uninit in 'a' mode |
| R17 | M v | tmap.c, reader.c | tmap never freed (leak per reader); realloc partial-fail UAF |
| R18 | m   | tmap.c interp_i64 | non-monotonic UTC: div by 0, NaN cast UB |
| R19 | m   | reader.c utc_load | failed load leaves partial tmap cached |


## Stage 2: signal definition validation (read and write)

| ID  | Sev | Location | Defect |
|-----|-----|----------|--------|
| D1  | C v | core.c signal_def_align | 32-bit round-up wraps: SIGFPE from jls_wr_signal_def |
| D2  | C   | core.c signal_def_validate | spd/sdf/eps/factors unchecked on read: div0, hang, OOB |
| D3  | C v | wr_fsr.c sample buffer | uint32 size product: f64 spd=2^26 allocates 16 B |
| D4  | C v | wr_ts.c, core.c defaults | 24-bit types get ts decimate 0; factor 1-2 overflows |
| D5  | M   | core.c handle_signal_def | fields committed before validation; duplicates overwrite |


## Stage 3: read path (core.c, reader.c, track.c, datatype.c)

| ID  | Sev | Location | Defect |
|-----|-----|----------|--------|
| C1  | C v | core.c jls_core_fsr u1/u4 | caller-buffer overflow, wrong data, spurious errors |
| C2  | C   | core.c rd_fsr_data0 | chunk timestamp not checked against start: OOB read |
| C3  | C   | core.c reconstruct_omitted | size wrap -> OOB write; s_index/entry_count unchecked |
| C4  | M v | core.c reconstruct_omitted | I4/I8 omitted chunks unreadable (MESSAGE_INTEGRITY) |
| C5  | M   | core.c rd_fsr_level1 | index/summary chunks not validated; copy rc ignored |
| C6  | C   | core.c repair_fsr + wr_fsr summary1/N | index overflow while rebuilding on open |
| C7  | C   | track.c track_repair_pointers | trusts index entry_count on open |
| C8  | M   | core.c repair_fsr error path | failed recovery appends junk summaries to file |
| C9  | C v | reader.c fsr_statistics | increment*length overflow bypasses bounds: heap write |
| C10 | C   | datatype.c, reader.c | odd I4/U4 count writes past end; converts spd not count |
| C11 | M v | reader.c, datatype.c | I24/U24 statistics from uninitialized memory |
| C12 | M v | reader.c annotations/user_data | nested reader call in callback truncates iteration |
| C13 | M   | reader.c fsr_statistics | summary pointers stale after nested buf growth |
| C14 | m   | core.c construct_f32/f64 | odd count writes y[count] |
| C15 | m   | core.c fsr bounds check | signed overflow in start + length |
| C16 | m   | core.c fsr_length | uint32 entry_count * sdf wraps |
| C17 | m   | datatype.c | fixed-point scaling never applied (`& 0xff) >> 16`) |
| C18 | m   | reader.c utc | UTC data payload length unchecked |
| C19 | m   | core.c truncate_tail, scan_fsr_sample_id | relink/offset not tag/meta checked |
| C20 | m   | reader.c | wrong error codes; NaN min/max; level-0 reload offset |


## Stage 4: write path (writer.c, wr_fsr.c, wr_ts.c, threaded_writer.c, mrb)

| ID  | Sev | Location | Defect |
|-----|-----|----------|--------|
| W1  | C   | wr_fsr.c, wr_ts.c appends | write failure then more writes overflows index/summary |
| W2  | M v | wr_fsr.c duplicate u4 | odd overlap loops forever writing to disk |
| W3  | C v | wr_fsr.c gap fill | buf_sz inverted for >8-bit types: heap leak into file |
| W4  | C v | msg_ring_buffer.c | size + 4 header not bounded: heap overflow |
| W5  | M   | wr_fsr.c buffer alloc fail | closes self while track points to it: UAF/double free |
| W6  | M   | writer.c signal_def | IO error leaves half-defined signal: NULL deref |
| W7  | M   | threaded_writer.c close | ignored CLOSE send failure: join hangs forever |
| W8  | M v | wr_fsr.c close | trailing partial u1/u4 byte written uninitialized |
| W9  | M v | wr_fsr.c shift | one-byte over-read of caller buffer |
| W10 | M v | wr_fsr.c duplicate u1/u4 | stores wrong samples |
| W11 | M v | threaded_writer.c | storage_type truncated to u8 after validation |
| W12 | M   | all writer layers | async and close errors dropped (latch design below) |
| W13 | m   | wr_fsr.c summaryN | std NaN after gap at level >= 2 |
| W14 | m   | wr_ts.c commit | `level > COUNT` should be `>=`; close count mismatch |
| W15 | m   | threaded_writer.c | STRING/JSON data_size 0 rejected (API contract) |
| W16 | m   | writer.c open | leak on error after raw open |
| W17 | m   | threaded_writer.c | unlocked flush_processed_id read (TSan) |
| W18 | m   | writer.c signal_def | NULL name/units dereferenced |

Error latching (W1, W12): the first write failure latches in the core;
later writes return it; close does best-effort END + header and returns
the first error.  The threaded writer latches the first async error,
drops subsequent data messages, and returns the error from the next
API call, flush, and close.  Appends stay bounds-checked regardless.


## Stage 5: pyjls

| ID  | Sev | Location | Defect |
|-----|-----|----------|--------|
| P1  | C v | binding.pyx source/signal_def | GIL vs process-lock deadlock on C log |
| P2  | M v | binding.pyx __dealloc__ | interpreter exit hangs with open Writer |
| P3  | M v | binding.pyx callbacks | close() inside callback: use-after-free |
| P4  | M v | binding.pyx callbacks | exceptions swallowed; KeyboardInterrupt ignored |
| P5  | m   | binding.pyx __init__ | re-init leaks open handle |
| P6  | m   | binding.pyx tmap | uint64 dtype for signed sample_id |
| P7  | m   | binding.pyx | OverflowError instead of ValueError |
| P8  | m   | binding.pyx flush, flags | return codes ignored |
| P9  | m   | binding.pyx | long C calls (open/copy/repair) hold the GIL |
| P10 | m   | binding.pyx | pathlib.Path rejected; doc dtype mismatch |


## Stage 6: fuzz coverage

Add a CRC-resigning corruption mode to `read_fuzzer` (mutate header and
payload fields, recompute both CRCs) so the hostile-file space reached
by these reviews is exercised continuously.
