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
  * Fixed sample rate data
  * Handles missing samples gracefully (interpolate)
  * Multiple data types including:
    - Floating point: f32, f64
    - Unsigned integers in nibble (4 bit) increments. 
    - Signed integers in nibble (4 bit) increments.
    - Fixed-point, signed integers in nibble (4 bit) increments.
    - Boolean (digital) 1-bit signals.
* Support for variable sample rate (timestamped) data
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

Media containers are another option, especially the ISO base media file format
used by MPEG4 and many others:
  * [ISO/IEC 14496-14:2020 Specification](https://www.iso.org/standard/79110.html)
  * [Overview](https://mpeg.chiariglione.org/standards/mpeg-4/iso-base-media-file-format)

However, the standard does not included the ability to store the signal summaries
and our specific signal types.


## How?

At its lowest layer, JLS is an enhanced tag-length-value (TLV) format.
TLV files form the foundation of many reliable image and video formats, 
including MPEG4 and PNG.  The enhanced header contains additional fields
to speed navigation and improve reliability.  The JLS file format calls 
each TLV a "chunk", and the enhanced tag-length component the chunk header.

The JLS file format supports sources that produce data.  The file allows
the application to clearly define and label the source.  Each source
can have any number of associated signals.

Signals are 1-D sequences of values over time consisting of a single,
fixed data type.  Each signal can have multiple tracks that contain
data associated with that signal. The JLS file supports two signal types: 
fixed sample rate (FSR) and variable sample rate (VSR).  FSR signals
store their sample data in the FSR track using FSR_DATA and FSR_SUMMARY.
FSR time is in sample_id.  FSR signals also support:

* Sample time to UTC time mapping using the UTC track.
* Annotations with the ANNOTATION track. 

VSR signals store their sample data in the VSR track.  VSR signals
specify time in UTC (wall-clock time).  VSR signals also
support annotations with the ANNOTATION track.
The JLS file format supports VSR signals that only use the 
ANNOTATION track and not the VSR track.  Such signals are commonly 
used to store UART data where each line contains a UTC timestamp. 

Signals support DATA chunks and SUMMARY chunks.
The DATA chunks store the actual sample data.  The SUMMARY chunks
store the reduced statistics, where each statistic entry represents
multiple samples.  This file format stores the mean, min, max, 
and standard deviation.  The previous JLS file format stored variance,
but variance requires twice the bit size (squared) compared to 
standard deviation, at least for integer types.

Before each SUMMARY chunk, the JLS file will contain the INDEX chunk
which contains the starting time and offset for each chunk that 
contributed to the summary.  This SUMMARY chunk enables fast O(log n)
navigation of the file.. 

The JLS file format design supports SUMMARY of SUMMARY.  It supports
the DATA and up to 15 layers of SUMMARIES.  sample_id is given as a
64-bit integer, which allows each summary to include only 20 samples
and still support the full 64-bit integer space.  In practice, the
first level summary increases a single value to 4 values, so summary
steps are usually 50 or more.

Many applications, including the Joulescope UI, prioritize visualizing the 
waveform quickly upon open.  Waiting to scan through a 1 TB file is not a 
valid option.  The reader opens the file and scans the signal for the highest 
summary of summaries.  It can very quickly display this data, and then
start to retrieve more detailed information as requested.


## Example file structure

```
sof
header
SOURCE_DEF(0)       // internal, reserved for global annotations
TS_DEF(0, TS.0)     // internal, reserved for global annotations
HEAD(TS.0, TS)
HEAD(TS.0, ANNOTATION)
UTC_DEF(0)          // reserved for local computer UTC time
SOURCE_DEF(1)       // input device 1
SIGNAL_DEF(1, 1)    // our signal, like "current" or "voltage"
HEAD(SIG.1, BLOCK)
HEAD(SIG.1, UTC)
HEAD(SIG.1, ANNOTATION)
USER_DATA           // just because
BLOCK_DATA(SIG.1)
BLOCK_DATA(SIG.1)
BLOCK_DATA(SIG.1)
BLOCK_DATA(SIG.1)
INDEX(SIG.1, lvl=0)
BLOCK_SUMMARY(SIG.1, lvl=1)
BLOCK_DATA(SIG.1)
BLOCK_DATA(SIG.1)
BLOCK_DATA(SIG.1)
BLOCK_DATA(SIG.1)
INDEX(SIG.1, lvl=0)
BLOCK_SUMMARY(SIG.1, lvl=1)
BLOCK_DATA(SIG.1)
BLOCK_DATA(SIG.1)
BLOCK_DATA(SIG.1)
BLOCK_DATA(SIG.1)
INDEX(SIG.1, lvl=0)
BLOCK_SUMMARY(SIG.1, lvl=1)
BLOCK_DATA(SIG.1)
BLOCK_DATA(SIG.1)
BLOCK_DATA(SIG.1)
BLOCK_DATA(SIG.1)
INDEX(SIG.1, lvl=0)
BLOCK_SUMMARY(SIG.1, lvl=1)
INDEX(SIG.1, lvl=1)
BLOCK_SUMMARY(SIG.1, lvl=2)
USER_DATA           // just because
eof
```

Note that HEAD(SIG.1) points to the first INDEX(SIG.1, lvl=0) and
INDEX(SIG.1, lvl=1). 
Each BLOCK_DATA is in a doubly-linked list with its next and previous
neighbors.  Each INDEX(SIG.1, lvl=0) is likewise in a separate doubly-linked
list, and the payload of each SUMMARY point to the summarized BLOCK_DATA
instances.  INDEX(SIG.1, lvl=1) points to each INDEX(SIG.1, lvl=0) instance.
As more data is added, the INDEX(SIG.1, lvl=1) will also get added to
the INDEX chunks at the same level.
