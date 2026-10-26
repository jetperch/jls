(jls_recovery)=

# File Recovery

JLS is designed to survive the most common failures.  When the writer
crashes, the computer loses power, or the recording is otherwise not
closed correctly, the file is left truncated.  The next time you open
it, JLS repairs the truncated tail automatically and quickly, regardless
of file size.  You do not need to do anything for this case.

Some damage is not automatically recoverable.  Examples include bit
errors or zeroed regions inside the file, often caused by failing
storage, interrupted copies, or sync tools.  For these files, opening
fails, typically with `MESSAGE_INTEGRITY`.  The Joulescope UI may refuse
to open the file or fail partway through the data.

JLS provides two tools for these cases:

| Tool     | Modifies source | Output        | Use when                                     |
|----------|-----------------|---------------|----------------------------------------------|
| `copy`   | No              | New JLS file  | Recommended.  Also for read-only media.      |
| `repair` | Yes, in place   | Same file     | You have a backup and lack space for a copy. |

Both tools are available in Python (pyjls) and as the C-based `jls`
command line tool.


## Before you start

* **Make a backup.**  Copy the damaged file somewhere safe before trying
  any recovery.  Opening a truncated file and `repair` both write to the
  file.
* **Check free space.**  `copy` writes a new file about the same size
  as the source.


## Copy to a new file (recommended)

`copy` reads the damaged source file and replays every readable chunk
into a new destination file.  It reports damaged chunks and skips them.
The source file is never modified, so you can safely run it on a
read-only file or medium.

### Python

[Install Python](https://www.python.org/downloads/) for your system,
and then run:

```
python -m pip install -U pyjls
python -m pyjls copy src.jls dst.jls
```

Replace `src.jls` and `dst.jls` with your actual source and destination
JLS file paths.  Then open `dst.jls` in the Joulescope UI.

Add `--no-progress` to hide the progress bar.

### C command line tool

The `jls` executable performs the same operation without Python:

```
jls copy src.jls dst.jls
```

For Windows, download `jls.exe` from the assets of the latest
[GitHub release](https://github.com/jetperch/jls/releases).  On other
platforms, build it from source with CMake; the executable is
`example/jls` in the build directory.

### What to expect

`copy` prints a message for each damaged region it skips, such as:

```
4002328: ERROR jls_raw_rd_payload | 9 MESSAGE_INTEGRITY : The message integrity check failed
fsr 1 skip: in=1100000 expect=1000000, skipped=100000
```

The first number is the file offset of the damaged chunk.  The `skip`
message shows that 100000 samples were lost from signal 1.  Samples in
damaged chunks cannot be recovered, so the destination file contains a
gap at that point.  All signals, annotations, UTC time entries, and user
data that remain readable are copied.

The destination is a newly written JLS file, so its internal chunk
layout may differ from the original.  The sample data and metadata
are the same.


## Repair in place

`repair` fixes the damaged file itself, without writing a second copy.
It scans the entire file once, relinks around damaged chunks, rebuilds
lost index information, and finalizes the file.  Where the damaged
sample data is still covered by summary (reduction) data, reads over that
region return values reconstructed from the summary.

Repair only rebuilds sample (FSR) data.  If annotation or UTC time data
is damaged, reads of that data may still fail after a successful repair.
In that case, use `copy` on the repaired file or on your backup.

### Python

```
python -m pip install -U pyjls
python -m pyjls repair damaged.jls
```

### C command line tool

```
jls repair damaged.jls
```

### What to expect

On success, repair prints a summary such as:

```
repair: 1 dead spans, 1 relinks, 0 clamps, 0 heads rebuilt, 1 entries omitted
```


## Programmatic use

Applications can offer recovery to their users.  Opening a damaged file
fails with `JLS_ERROR_MESSAGE_INTEGRITY` (C) or `RuntimeError` with
`MESSAGE_INTEGRITY` in the message (Python).  Opening a truncated file
on read-only media fails with `JLS_ERROR_TRUNCATED`.  In either case,
call copy or repair:

| Operation | C API            | Python API      |
|-----------|------------------|-----------------|
| Copy      | `jls_copy()`     | `pyjls.copy()`  |
| Repair    | `jls_repair()`   | `pyjls.repair()`|

Both accept optional message and progress callbacks, and both support
cancellation from either callback.  See the [C API](c_api.rst) and the
[Python API](py_api.rst) for details.


## Getting help

If neither tool recovers your data, please post on the
[Joulescope forum](https://forum.joulescope.com/).  Include the tool
output and, if possible, a link to the damaged file.
