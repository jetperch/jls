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
    p.add_argument('--name', default='current', help='The signal name')
    p.add_argument('--units', default='A', help='The signal value units (SI)')
    p.add_argument('--sample_rate', type=int, default=1000000, help='The sample rate in Hz.')
    p.add_argument('--length', type=int, default=1000000, help='The signal length in samples.')
    p.add_argument('--samples_per_data', type=int, default=100000, help='The samples per data chunk.')
    p.add_argument('--sample_decimate_factor', type=int, default=1000, help='The samples per summary entry.')
    p.add_argument('--entries_per_summary', type=int, default=20000, help='The entries per summary chunk.')
    p.add_argument('--summary_decimate_factor', type=int, default=100, help='The summaries per summary entry.')
    p.add_argument('--add',
                   action='append',
                   help='The waveform definition to add, which is one of:'
                        'sine,{amplitude},{frequency} '
                        'ramp'
                   )
    return p


def _waveform_factory(d):
    if d is None or not len(d):
        return None
    parts = d.split(',')
    name = parts[0].lower()
    if name == 'ramp':
        if len(parts) == 1:
            return lambda x: x
        elif len(parts) == 2:
            amplitude = float(parts[1])
            return lambda x: x * amplitude
        else:
            raise ValueError('Invalid ramp specification')
    elif name in ['sin', 'sine', 'sinusoid', 'cos', 'cosine', 'freq', 'frequency', 'tone']:
        if len(parts) != 3:
            raise RuntimeError(f'Must specify {name},amplitude,frequency')
        amplitude = float(parts[1])
        freq = float(parts[2])
        return lambda x: amplitude * np.sin((2.0 * np.pi * freq) * x)
    else:
        raise RuntimeError(f'Unknown waveform: {name}')


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
        sample_rate=args.sample_rate,
        samples_per_data=args.samples_per_data,
        sample_decimate_factor=args.sample_decimate_factor,
        entries_per_summary=args.entries_per_summary,
        summary_decimate_factor=args.summary_decimate_factor,
        annotation_decimate_factor=args.summary_decimate_factor,
        utc_decimate_factor=args.summary_decimate_factor,
        name=args.name,
        units=args.units,
    )

    dargs = vars(args)
    for key in signal.__dict__.keys():
        v = dargs.get(key)
        if v is not None:
            if key not in ['name', 'units']:
                v = int(v)
            setattr(signal, key, v)

    waveforms = []
    for waveform_def in args.add:
        fn = _waveform_factory(waveform_def)
        if fn is not None:
            waveforms.append(fn)

    # generate waveform, 1 second chunk at a time
    x = np.arange(signal.sample_rate, dtype=np.float64) * 1 / signal.sample_rate

    # Write to file
    y_len = len(x)
    sample_id = 0
    with Writer(args.filename) as wr:
        wr.source_def_from_struct(source)
        wr.signal_def_from_struct(signal)
        wr.user_data(0, 'string user data at start')
        length = args.length
        while length > 0:  # write data chunks
            y = np.zeros(y_len, dtype=np.float64)
            for waveform in waveforms:
                y += waveform(x)
            iter_len = y_len if y_len < length else length
            wr.fsr_f32(1, sample_id, y[:iter_len].astype(dtype=np.float32))
            sample_id += iter_len
            length -= iter_len
            x += 1.0  # increment
        wr.user_data(42, b'binary data')
        wr.user_data(43, {'my': 'data', 'json': [1, 2, 3]})


if __name__ == "__main__":
    sys.exit(run())
