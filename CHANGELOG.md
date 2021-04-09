
# CHANGELOG

This file contains the list of changes made to the JLS project.


## 0.3.0

2021 Apr 8

*   NOT BACKWARDS COMPATIBLE with 0.2.6 and earlier.
*   Modified annotation to contain optional y-axis position.  API change.
*   Improved file format consistency and improved format.h.
*   Added UTC track writer for FSR signals.
*   Added INDEX and SUMMARY writer for ANNOTATION and UTC tracks.


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
