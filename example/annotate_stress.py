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
Generate a JLS annotation file.

This example script stress JLS annotation file creation.
"""

from pyjls import Reader, Writer, SourceDef, SignalDef, AnnotationType, DataType, SignalType
import argparse
import logging
import numpy as np
import os
import sys
import time


def parser():
    p = argparse.ArgumentParser(description='JLS generator.')
    p.add_argument('filename',
                   help='The output JLS file path')
    p.add_argument('--count',
                   type=int,
                   default=1000000,
                   help='The total number of annotations to write')
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

    # Write to file
    data = bytes(range(256))
    t_start = time.time()
    t_finish = time.time()
    count = 0

    try:
        with Writer(args.filename) as wr:
            wr.source_def_from_struct(source)
            wr.signal_def_from_struct(signal)
            wr.user_data(0, 'string user data at start')
            for count in range(args.count):
                wr.annotation(1, count, AnnotationType.USER, 0, data)
            t_finish = time.time()
    except Exception:
        logging.getLogger().exception('during write')

    t_end = time.time()
    print(f'{count + 1} annotations: duration={t_end - t_start:.3f} s, close={t_end - t_finish:.3f} s')


if __name__ == "__main__":
    sys.exit(run())
