<!--
# Copyright 2014-2021 Jetperch LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
-->

# JLS

Welcome to the Joulescope® File Format project.  The goal of this project is to
provide performant data storage for huge, simultaneous, one-dimensional 
signals. This repository contains:

* The JLS file format specification
* The implementation in C
* Language bindings for Python


## Features

* Support for multiple, simultaneous data sources
* Support for multiple, simultaneous signal waveforms
  * Fixed sampling period
  * Handles missing samples gracefully (interpolate)
  * Multiple data types including:
    - Floating point: f32, f64
    - Unsigned integers in nibble (4 bit) increments. 
    - Signed integers in nibble (4 bit) increments.
    - Fixed-point, signed integers in nibble (4 bit) increments.
    - Boolean (digital) 1-bit signals.
* Support for timestamped time series data
  * Additional support for binary and text, such as for UTF-8 UART data.
* Fast read performance.
  * Signal Summaries
    * "Zoomed out" view with mean, min, max, standard deviation
    * Provides fast waveform load without any additional processing steps
  * Automatic load by summary level
  * Fast seek, next, previous access
* Wall-clock time (UTC) to sample id
  * Automatic with the host computer clock
  * Optional manual timestamping
* Annotations
  * Global (UTC time)
  * Attached to signals
  * Support for text and marker
* Reliability
  * File data still accessible in the case of improper program termination.
  * In case of file corruption, uncorrupted data is still accessible.
  * On write, avoid changing already written data.  The only exception is
    updating indices and the doubly-linked list next pointer.
* Compression options
  * lossless
  * lossy
  * lossy with downsampling below threshold

## Why?

The world is already full of file formats, and we would rather not create 
another one.  However, we could not identify a solution that met these
requirements.  [HDF5](https://www.hdfgroup.org/solutions/hdf5/) meets the
large storage requirements, but not the reliability and rapid load requirements.
The [Saleae binary export file format v2](https://support.saleae.com/faq/technical-faq/binary-export-format-logic-2)
is also not suitable since it buffers stores single, contiguous blocks.
[Sigrok v2](https://sigrok.org/wiki/File_format:Sigrok/v2) is similar.
The [Sigrok v3](https://sigrok.org/wiki/File_format:Sigrok/v3) format
(under development as of Feb 2021) is better in that it stores sequences of
"packets" containing data blocks, but it still will does not allow for
fast seek or summaries.

Timeseries databases, such as [InfluxDB](https://www.influxdata.com/), are 
powerful tools.  However, they are not well-designed for fast sample-rate
data.


## How?

At its lowest layer, JLS is an enhanced tag-length-value (TLV) format.
TLV files form the foundation of many reliable image and video formats, 
including PNG.  The enhanced header contains additional fields to speed 
navigation and improve reliability.  The JLS file format calls each TLV a
"chunk", and the enhanded tag-length component the chunk header.

The JLS file format uses chunks to store signals in a doubly-linked list.
Periodically, the format will also store a summary. 
As the file continues to grow, it will eventually grow to contain a 
summary of summaries, and even summaries of summaries of summaries of summaries.

Many applications, including the Joulescope UI, prioritize visualizing the 
waveform quickly upon open.  Waiting to scan through a 1 TB file is not a 
valid option.  The reader opens the file and scans the signal for the highest 
summary of summaries.  Each summary also contains indices back into the details
for rapid access.  


## Example file structure

```
sof
header
SOURCE_DEF(0)       // internal, reserved for global annotations
TS_DEF(0)           // internal, reserved for global annotations
SOURCE_DEF(1)       // input device 1
SIGNAL_DEF(1, 1)    // our signal, like "current"
UTC_DEF(0)          // local computer UTC time
USER_DATA           // just because
BLOCK_INDEX(0)
BLOCK_INDEX(1)
UTC_INDEX(1)
BLOCK_DATA(1)
BLOCK_DATA(1)
BLOCK_DATA(1)
BLOCK_DATA(1)
SUMMARY(1, lvl=1)
BLOCK_DATA(1)
BLOCK_DATA(1)
BLOCK_DATA(1)
BLOCK_DATA(1)
SUMMARY(1, lvl=1)
BLOCK_DATA(1)
BLOCK_DATA(1)
BLOCK_DATA(1)
BLOCK_DATA(1)
SUMMARY(1, lvl=1)
BLOCK_DATA(1)
BLOCK_DATA(1)
BLOCK_DATA(1)
BLOCK_DATA(1)
SUMMARY(1, lvl=1)
SUMMARY(1, lvl=2)
USER_DATA           // just because
eof
```

Note that BLOCK_INDEX(1) points to the first BLOCK_DATA, 
the first SUMMARY(1, lvl=1), and the SUMMARY(1, lvl=2). 
Each BLOCK_DATA is in a doubly-linked list with its next and previous
neighbors.  Each SUMMARY(1, lvl=1) is likewise in a separate doubly-linked
list, and the payload of each SUMMARY point to the summarized BLOCK_DATA
instances.  SUMMARY(1, lvl=2) points to each SUMMARY(1, lvl=1) instance.
As more data is added, the SUMMARY(1, lvl=2) will also get added to
the SUMMARY chunks at the same level.
