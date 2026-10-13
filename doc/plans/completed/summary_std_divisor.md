# Summary std divisor mismatch

Status: COMPLETE (2026-10-11).  Found while writing `test_fsr_gap`
(test/jls_test.c).


## Problem

The writer stores each FSR summary entry's standard deviation with the
population divisor: `var = sum((x - mean)^2) / count` (src/wr_fsr.c,
level 1 and level N summaries).  It has done so since d96cfa2.

The reader converted a stored entry back to a sum of squares as if it
were a sample (Bessel) deviation: `s = std^2 * (count - 1)` in
`f32_to_stats` and `f64_to_stats` (src/reader.c), then reported
`s / (N - 1)`.  This dates from 8cf5694.

A statistics window built from several summary entries therefore
understated the within-entry variance by `(k - 1) / k`, where k is the
entry size in samples: 0.4% of variance (0.2% of std) for k = 256.
`test_fsr_statistics_u1` recorded the inconsistency: the same window
reported 0.433224 from samples and 0.433013 from summaries.


## Decision: the writer keeps population std

* Every existing file stores population std, and summary chunks record
  neither the divisor nor a format revision.  Changing the writer would
  make files ambiguous; fixing the reader is correct for all files.
* Population variance combines exactly across equal-size entries (the
  level N writer averages `std_i^2 + (mean_i - mean)^2`), and stays
  defined for single-sample entries.
* The reported std is an API choice, independent of storage: the reader
  reports the sample std (n - 1) from both samples and summaries.

format.h documents `JLS_SUMMARY_FSR_STD` as population std, and reader.h
documents that `jls_rd_fsr_statistics` reports the sample std.


## Fix

`f64_to_stats` takes a `var_divisor`: `count` for stored summary entries
(population), `count - 1` for the reader's own nested results (sample),
which fill the partial head and the final window.  `f32_to_stats` only
reads stored summaries and uses `count`.  The first attempt changed both
conversions to `count` and broke the final-window path, which converts
a sample std; `test_fsr_gap` caught it.


## Tests

* `test_fsr_gap` checks every window with no mixed NaN entry against the
  exact sample std at the sample, level 1, and level 2 paths, plus a
  window that starts mid entry.  A summary entry that mixes data and NaN
  fill uses its full size as n (its finite count is not stored), so
  single-entry mixed windows check a range.
* `test_fsr_statistics_u1` now expects 0.433224 from both paths.
* pyjls `test_fsr_statistics_std_from_summaries` checks f32 and f64
  summaries (f32, f64, u8, u1) against `np.std(ddof=1)` for level 1 and
  level 2 entries, multi-entry windows, and the nested head and final
  tail reads, and checks that summaries and samples agree.  The pyjls
  gap test checks the std across NaN fill.
* Negative controls: reverting any of the four divisors fails both
  `test_fsr_gap` and the pyjls test.
* The example_jls old-versus-new comparison (`tools/classify.py`)
  classifies the change as `stats_std_divisor`, bounded per window by
  the smallest summary piece the window uses; all 12 captures have no
  unexplained differences (2026-10-11).


## Remaining limitation

Summary entries do not record their finite sample count, so an entry
that is partly NaN contributes with its full count (documented in
reader.h).
