#!/usr/bin/env python3
# Copyright 2021 Jetperch LLC
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

"""
Generate a JLS file.

This example script generates a JLS file.
"""

from pyjls import Reader, Writer, SourceDef, SignalDef, AnnotationType, DataType, SignalType
import argparse
import logging
import numpy as np
import os
import sys


def parser():
    p = argparse.ArgumentParser(description='JLS generator.')
    p.add_argument('filename',
                   help='The output JLS file path')
    p.add_argument('--name', help='The signal name')
    p.add_argument('--units', help='The signal value units (SI)')
    p.add_argument('--sample_rate', help='The sample rate in Hz.')
    p.add_argument('--length', type=int, default=1000000, help='The signal length in samples.')
    p.add_argument('--samples_per_data', help='The samples per data chunk.')
    p.add_argument('--sample_decimate_factor', help='The samples per summary entry.')
    p.add_argument('--entries_per_summary', help='The entries per summary chunk.')
    p.add_argument('--summary_decimate_factor', help='The summaries per summary entry.')
    return p


def run():
    args = parser().parse_args()

    source = SourceDef(
        source_id=1,
        name='performance',
        vendor='',
        model='',
        version='',
        serial_number='',
    )

    signal = SignalDef(
        signal_id=1,
        source_id=1,
        signal_type=SignalType.FSR,
        data_type=DataType.F32,
        sample_rate=1000000,
        samples_per_data=100000,
        sample_decimate_factor=1000,
        entries_per_summary=20000,
        summary_decimate_factor=100,
        annotation_decimate_factor=100,
        utc_decimate_factor=100,
        name='current',
        units='A',
    )

    dargs = vars(args)
    for key in signal.__dict__.keys():
        v = dargs.get(key)
        if v is not None:
            if key not in ['name', 'units']:
                v = int(v)
            setattr(signal, key, v)

    # generate waveform, 1 second chunk
    x = np.arange(signal.sample_rate)
    y = np.sin((2.0 * np.pi * 1000 / signal.sample_rate) * x)
    y = y.astype(dtype=np.float32)

    # Write to file
    y_len = len(y)
    sample_id = 0
    with Writer(args.filename) as wr:
        wr.source_def_from_struct(source)
        wr.signal_def_from_struct(signal)
        wr.user_data(0, 'string user data at start')
        length = args.length
        while length > 0:  # write data chunks
            iter_len = y_len if y_len < length else length
            wr.fsr_f32(1, sample_id, y[:iter_len])
            sample_id += iter_len
            length -= iter_len
        wr.user_data(42, b'binary data')
        wr.user_data(43, {'my': 'data', 'json': [1, 2, 3]})


if __name__ == "__main__":
    sys.exit(run())
