
# CHANGELOG

This file contains the list of changes made to the JLS project.


## 0.12.1

2025 Mar 27

* Added Linux ARM64 builds.


## 0.11.1

2024 Dec 5

* Added "group_id" documentation to pyjls.Writer.annotate.
* Fixed annotate entry point.


## 0.11.0

2024 Oct 29

* Migrated to numpy 2.
* Added Python 3.13 support.
* Dropped Python 3.9 support.


## 0.10.0

2024 Aug 21

* Improved Python binding to simplify finding sources and signals.
  * Added pyjls.source_lookup.
  * Improved pyjls.signal_lookup to support '<source_name>.<signal_name>' format.
* Added "--timestamp" option to export to add relative time to CSV output.


## 0.9.7

2024 Jul 25

* Fixed plot entry point to time align signals.


## 0.9.6

2024 Jun 28

* Fixed issue with stop time in extract entry point.
* Pinned numpy < 2 for build environment.


## 0.9.5

2024 May 29

* Fixed u4 data type reconstruction when omitted for compression  #13


## 0.9.4

2024 May 10

* Fixed unicode filename support on Windows  #12


## 0.9.3

2024 Mar 7

* Added "export" python subcommand.


## 0.9.2

2024 Feb 14

* Fixed index error in file repair operation.
* Fixed FSR summary reconstruction on truncation  #10


## 0.9.1

2023 Nov 13

* Bumped rev to fix publish to pypi.


## 0.9.0

2023 Nov 13

* Added jls_copy with "jls copy" command and pyjls "copy" entry point.
* Publish release assets including "jls.exe".


## 0.8.2

2023 Oct 25

* Added file truncation repair for never closed files.
* Added support for python 3.12.


## 0.8.1

2023 Sep 18

* Improved python binding Reader.fsr_statistics documentation #9.
* Fixed occasional missing last byte in jls_rd_fsr for 1-bit signals.


## 0.8.0

2023 Sep 16

* Added file truncation repair.
* Added jls_raw_truncate and jls_raw_backend.
* Refactored reader and writer into new core to enable repairer.
* Added jls_twr_flags_get/set.
* Added JLS_TWR_FLAG_DROP_ON_OVERFLOW which drops samples on overflow.
* Added jls_wr_fsr_omit_data and jls_twr_fsr_omit_data.
* Automatically omit constant 1, 4, & 8 bit entry FSR data chunks.
* Modified default signal_def settings for improved performance.
* Added jls_dt_str for pretty-printing the data type.
* Added u1 and u4 FSR data type support to performance tool.
* Fixed jls_rd_fsr out of bounds memory access.
* Added read_fuzzer tool to jls example.


## 0.7.3

2023 Jul 24

* Added "noexcept" to python callbacks.
  Cython 3.0 deprecates implicit noexcept.


## 0.7.2

2023 Jun 14

* Fixed build for Raspberry Pi.
* Added JLS_OPTIMIZE_CRC cmake option.


## 0.7.1

2023 Jun 7

* Added Reader.signal_lookup.
* Improved documentation.
* Added Read The Docs integration.
* Improved python "export" subcommand to specify "--signal" by name.
* Added python subcommand "plot".
* Improved build process, migrated to GitHub Actions.
* Bumped minimum python version from 3.8 to 3.9.


## 0.7.0

2023 May 31

* Fixed incorrect write timestamp stride in FSR index/summary entries.
  Any recording over 5.77 hours was incorrect. 
* Improved threaded writer.
  * Removed jls_wr_flush during close due to UI performance problems.
  * Release the GIL on some python Writer operations.
  * Reduced buffer size from 100,000,000 B to 64 MB. 
  * Save string null termination byte for annotations and user_data.
  * Increased thread priority on Windows.
  * Do not quit until all messages are processed.
* Added jls executable to examples.
* Improved reader logging and error handling.


## 0.6.3

2023 May 16

* Added support for building a shared library.
  Initialize build subdir with "cmake -DBUILD_SHARED_LIBS=ON .."


## 0.6.2

2023 Apr 28

* Improved UTC read processing.
* Added "--utc" option to info entry point.
* Fixed incorrect NaNs in summary on write.
* Added "export" pyjls entry point.


## 0.6.1

2023 Apr 27

* Added FSR support for missing and duplicate data.
* Added FSR support for unaligned u1 and u4 data.
* Improved log messages.


## 0.6.0

2023 Apr 26

* Fixed JLS to handle non-zero sample_id for first FSR data sample.
* Added pyjls.Reader.timestamp_to_sample_id and sample_id_to_timestamp.


## 0.5.3

2023 Apr 19

* Fixed build warnings for fn() declarations.


## 0.5.2

2023 Mar 30

* Reduced default log level to WARNING.


## 0.5.1

2023 Mar 16

* Added zero length check to jls_wr_fsr_data.


## 0.5.0

2023 Mar 9

* Added support for data_type strings (not just enum integers).
* Modified python reader bindings to release GIL.
* Migrated to time64 representation for all API calls.
  Use utc_to_jls and jls_to_utc to convert as needed to/from  
  python timestamps.  
* Added data_type_as_enum and data_type_as_str conversion functions.
* Improved exception messages.


## 0.4.3

2022 Nov 30

* Changed windows dependency from deprecated pypiwin32 to pywin32.
* Bumped numpy dependencies.
* Added build system requirements for pip.


## 0.4.2

2022 Mar 17

* Fixed pyjls build for Raspberry Pi OS 64-bit.


## 0.4.1

2022 Mar 7

* Fixed build for Linux and macOS.


## 0.4.0

2022 Mar 5

* Added support for additional data types (was only f32):
  * Signed Integer: i4, i8, i16, i24, i32, i64
  * Unsigned Integer: u1, u4, u8, u16, u24, u32, u64
  * Float: f64
  * Fixed point signed & unsigned integers.
* Renamed jls_rd_fsr_f32_statistics to jls_rd_fsr_statistics, which now
  always returns statistics as double (f64). 
* Improved documentation.


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
