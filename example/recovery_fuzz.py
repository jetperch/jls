#!/usr/bin/env python3
# Copyright 2026 Jetperch LLC
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
Fuzz pyjls Reader recovery and reads on damaged files.

This example writes a file containing every record type with pyjls,
damages copies of it (truncate, zero_tail, or bitflip), and reads
everything back through the pyjls Reader.  Each case runs in a
subprocess, so crashes count as failures.  Reader errors
(RuntimeError, ValueError) are expected for damaged files and pass.

The C read_fuzzer (jls read_fuzzer --corrupt <mode>) covers the native
reader in more depth.  This script covers the Python binding.  To check
memory safety, build pyjls with AddressSanitizer and run with the ASan
runtime preloaded, for example::

    CFLAGS="-fsanitize=address" LDFLAGS="-fsanitize=address" \\
        python setup.py build_ext --inplace
    LD_PRELOAD=/path/to/libasan.so ASAN_OPTIONS=detect_leaks=0 \\
        python example/recovery_fuzz.py --cases 1000
"""

import argparse
import os
import random
import shutil
import subprocess
import sys
import tempfile
import numpy as np


MODES = ['truncate', 'zero_tail', 'bitflip']

_CHILD = r'''
import sys
from pyjls import Reader

def ignore(fn):
    try:
        fn()
    except (RuntimeError, ValueError):
        pass

try:
    r = Reader(sys.argv[1])
except (RuntimeError, ValueError):
    sys.exit(0)  # graceful refusal
with r:
    for signal_id, s in r.signals.items():
        ignore(lambda: r.annotations(signal_id, 0, lambda *a: False))
        if s.signal_type != 0:  # FSR only
            continue
        n = s.length
        if n:
            ignore(lambda: r.fsr(signal_id, 0, n))
            ignore(lambda: r.fsr_statistics(signal_id, 0, max(1, n // 7), min(n, 7)))
        ignore(lambda: r.utc(signal_id, 0, lambda e: False))
        ignore(lambda: r.time_map_get(signal_id))
        ignore(lambda: r.sample_id_to_timestamp(signal_id, [0, n // 2]))
    ignore(lambda: r.user_data(lambda *a: False))
'''


def parser():
    p = argparse.ArgumentParser(description='Fuzz pyjls Reader recovery on damaged files.')
    p.add_argument('--cases', type=int, default=300,
                   help='The number of damaged files to read')
    p.add_argument('--random', type=int, default=1,
                   help='The random number seed')
    p.add_argument('--mode', choices=MODES, action='append',
                   help='The corruption mode, which may repeat.  Default is all modes.')
    p.add_argument('--workdir',
                   help='The working directory.  Default is a temporary directory.')
    p.add_argument('--timeout', type=float, default=120,
                   help='The per-case timeout in seconds')
    return p


def generate(path):
    from pyjls import Writer
    with Writer(path) as w:
        w.source_def(source_id=1, name='src', vendor='v', model='m', version='1', serial_number='s')
        w.signal_def(3, 1, sample_rate=100000, name='i', units='A', utc_decimate_factor=10)
        w.signal_def(4, 1, sample_rate=100000, data_type='u1', name='d', units='')
        w.signal_def(5, 1, sample_rate=1000, data_type='u4', name='r', units='')
        w.signal_def(6, 1, signal_type=1, name='vsr', units='')
        x = np.sin(np.arange(200000, dtype=np.float32) * 0.001).astype(np.float32)
        for k in range(0, len(x), 10000):
            w.fsr(3, k, x[k:k + 10000])
            w.utc(3, k, k * 10000)
            w.annotation(3, k, None, 'text', 0, f'annotation {k}')
            w.annotation(6, k * 1000, 1.0, 'user', 1, b'\x01\x02')
            w.user_data(k % 4000, {'k': k})
        w.fsr(4, 0, np.arange(100000 // 8, dtype=np.uint8))
        w.fsr(5, 0, np.arange(5000, dtype=np.uint8))


def corrupt(data, mode, rng):
    b = bytearray(data)
    offset = rng.randrange(0, len(b))
    if mode == 'truncate':
        b = b[:offset]
    elif mode == 'zero_tail':
        b[offset:] = bytes(len(b) - offset)
    else:  # bitflip
        for _ in range(rng.randint(1, 8)):
            idx = rng.randrange(0, len(b))
            b[idx] ^= 1 << rng.randrange(8)
    return b, offset


def run():
    args = parser().parse_args()
    modes = args.mode or MODES
    rng = random.Random(args.random)
    workdir = args.workdir or tempfile.mkdtemp(prefix='jls_recovery_fuzz_')
    os.makedirs(workdir, exist_ok=True)
    pristine = os.path.join(workdir, 'pristine.jls')
    generate(pristine)
    with open(pristine, 'rb') as f:
        data = f.read()
    case_path = os.path.join(workdir, 'case.jls')
    failures = 0
    for case in range(args.cases):
        mode = modes[case % len(modes)]
        b, offset = corrupt(data, mode, rng)
        with open(case_path, 'wb') as f:
            f.write(b)
        try:
            p = subprocess.run([sys.executable, '-c', _CHILD, case_path],
                               capture_output=True, text=True, timeout=args.timeout)
            rc, stderr = p.returncode, p.stderr
        except subprocess.TimeoutExpired:
            rc, stderr = 'timeout', ''
        sanitizer = ('AddressSanitizer' in stderr) or ('runtime error' in stderr)
        if rc != 0 or sanitizer:
            failures += 1
            keep = os.path.join(workdir, f'fail_{case}_{mode}_{offset}.jls')
            shutil.copy(case_path, keep)
            last = [line for line in stderr.splitlines() if line.strip()][-3:]
            print(f'FAIL case={case} mode={mode} offset={offset} rc={rc} kept={keep}')
            for line in last:
                print(f'    {line}')
    print(f'recovery fuzz: file_size={len(data)} cases={args.cases} failures={failures} workdir={workdir}')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(run())
