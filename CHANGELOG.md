
# CHANGELOG

This file contains the list of changes made to the JLS project.


## 0.4.0

2022 Mar 5 [in progress]

* Added support for additional data types (was only f32):
  * Signed Integer: i4, i8, i16, i24, i32, i64
  * Unsigned Integer: u1, u4, u8, u16, u24, u32, u64
  * Float: f64
  * Fixed point signed & unsigned integers.
* Renamed jls_rd_fsr_f32_statistics to jls_rd_fsr_statistics, which now
  always returns statistics as double (f64). 


## 0.3.4

2021 Oct 28

* Added all version fields to pyjls module.
* Fixed writer not correctly serializing null and empty strings #6.


## 0.3.3

2021 Jul 7

*   Added reader sample_id bounds checks to FSR functions.
*   Cached jls_rd_fsr_length results.


## 0.3.2

2021 Jul 7

*   Fixed incorrect statistics computation when using summaries.
*   Added example/jls_read.c.
*   Connected example/generate.py arguments to work correctly.
*   Fixed documentation for jls_rd_fsr_f32_statistics().
*   Added GitHub action.


## 0.3.1

2021 Apr 13

*   Fixed y annotation argument order.
*   Added horizontal marker annotation.
*   Fixed Python API to automatically convert to/from UTC python timestamps.
*   Fixed reader seek when contains multiple annotations at same timestamp.


## 0.3.0

2021 Apr 8

Yanked this release, use 0.3.1

*   NOT BACKWARDS COMPATIBLE with 0.2.6 and earlier.
*   Modified annotation to contain optional y-axis position.  API change.
*   Improved file format consistency and improved format.h.
*   Added UTC track writer and reader for FSR signals.
*   Added INDEX and SUMMARY writer for ANNOTATION and UTC tracks.
*   Added reader seek to timestamp for ANNOTATION and UTC using INDEX & SUMMARY.


## 0.2.6

2021 Mar 18

*   Fixed uninitialized variables in POSIX backend (thanks Valgrind!).
*   Fixed memory leaks (thanks Valgrind!).


## 0.2.5

2021 Mar 18

*   Added 32-bit ARMv7 support (Raspberry Pi OS).
*   Added 64-bit ARM support for Apple silicon (M1).


## 0.2.4

2021 Mar 16

*   Added support for aarch64 (Raspberry Pi 4).  Untested on mac M1.
*   Fixed POSIX time.


## 0.2.3

2021 Mar 10

*   Fixed sdist to include native files for pyjls cython build.
*   Added CREDITS.html.
*   Included credits, license & readme with pyjls source distribution.


## 0.2.2

2021 Mar 9

*   Fixed build using Clang and AppleClang.


## 0.2.1

2021 Mar 8

*   Fixed timeout on close when bursting data.
*   Added annotate_stress example.
*   Added flush.


## 0.2.0

2021 Mar 8

*   Added group_id parameter to annotation.
*   Fixed example/generate.py length argument.
*   Updated numpy dependency to 1.20.
*   Fixed pyjls console_script.
*   Changed logging interface and connected native to python logging.


## 0.1.0

2021 Mar 1

*   Initial public release.


## 0.0.1

2021 Feb 5

*   Initial documentation.
