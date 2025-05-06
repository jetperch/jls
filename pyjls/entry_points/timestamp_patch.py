# Copyright 2025 Jetperch LLC
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


from pyjls import Reader, Writer, time64
import os
import sys


def parser_config(p):
    """Add or override FSR signal UTC time."""
    p.add_argument('--verbose', '-v',
                   action='store_true',
                   help='Display verbose information')
    p.add_argument('--timestamp',
                   help='new starting timestamp as ISO 8601: YYYY-MM-DDThh:mm:ss.ffffff')
    p.add_argument('infile',
                   help='JLS input file path')
    p.add_argument('outfile',
                   help='JLS output file path')
    return on_cmd


def on_cmd(args):
    with Reader(args.infile) as r:
        with Writer(args.outfile) as w:
            for source in r.sources.values():
                if source.source_id == 0:
                    continue
                w.source_def_from_struct(source)
            for signal in r.signals.values():
                if signal.signal_id == 0:
                    continue
                w.signal_def_from_struct(signal)

            if args.timestamp is None:
                timestamp = os.path.getctime(args.infile)
            timestamp = time64.as_time64(timestamp)

            # Attempt to get timestamp from user metadata
            user_data = {}

            def user_data_fn(chunk_meta_u16, data):
                w.user_data(chunk_meta_u16, data)
                try:
                    timestamp = data['timestamp']
                except:
                    pass

            r.user_data(user_data_fn)

            t_start = time64.as_time64(timestamp)
            for signal in r.signals.values():
                if signal.signal_id == 0:
                    continue
                d = r.fsr(signal.signal_id, 0, signal.length)
                w.fsr(signal.signal_id, 0, d)
                w.utc(signal.signal_id, 0, t_start)
    return 0
