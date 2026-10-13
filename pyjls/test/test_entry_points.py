# Copyright 2023-2025 Jetperch LLC
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

"""Test the pyjls command-line entry points."""

import argparse
import os
import tempfile
import unittest
import numpy as np
from pyjls import Writer, DataType, time64
from pyjls import Reader
from pyjls.entry_points import csv, extract, plot


_FS = 100_000
_T0 = 1_800_000_000 * time64.SECOND


class TestCsv(unittest.TestCase):

    def setUp(self):
        self._tempdir = tempfile.TemporaryDirectory()
        self._path = os.path.join(self._tempdir.name, 'test.jls')
        self._out = os.path.join(self._tempdir.name, 'out.csv')
        # two devices recorded together: the second starts 10 ms later and ends 20 ms later
        with Writer(self._path) as w:
            w.source_def(source_id=1, name='JS220-000043', vendor='v', model='JS220', version='1', serial_number='000043')
            w.source_def(source_id=2, name='JS110-000197', vendor='v', model='JS110', version='1', serial_number='000197')
            for signal_id, source_id, t_start, length in [(1, 1, _T0, _FS), (2, 2, _T0 + time64.SECOND // 100, _FS + _FS // 50)]:
                w.signal_def(signal_id=signal_id, source_id=source_id, data_type=DataType.F32, sample_rate=_FS,
                             name='current', units='A')
                w.utc(signal_id, 0, t_start)
                w.fsr_f32(signal_id, 0, np.full(length, float(signal_id), dtype=np.float32))
                w.utc(signal_id, length - 1, t_start + ((length - 1) * time64.SECOND) // _FS)

    def tearDown(self):
        self._tempdir.cleanup()

    def _run(self, count, signals=None):
        p = argparse.ArgumentParser()
        on_cmd = csv.parser_config(p)
        args = p.parse_args(['--count', str(count), self._path, self._out] + ([] if signals is None else ['--signals', signals]))
        return on_cmd(args), np.loadtxt(self._out, delimiter=',', comments='#')

    def test_different_extents(self):
        rc, data = self._run(1000)
        self.assertEqual(0, rc)
        self.assertEqual((1000, 3), data.shape)
        np.testing.assert_allclose(data[:, 1], 1.0)
        np.testing.assert_allclose(data[:, 2], 2.0)
        # the common window: 10 ms after the first signal's start to its end
        self.assertAlmostEqual(0.0, data[0, 0])
        self.assertAlmostEqual(1.0 - 0.01, data[-1, 0], delta=1e-3)

    def test_single_signal(self):
        rc, data = self._run(100, signals='1')
        self.assertEqual(0, rc)
        self.assertEqual((100, 2), data.shape)


class TestExtract(TestCsv):
    """Extract over the common window of signals with different extents."""

    def test_different_extents(self):
        out = os.path.join(self._tempdir.name, 'extract.jls')
        p = argparse.ArgumentParser()
        on_cmd = extract.parser_config(p)
        self.assertEqual(0, on_cmd(p.parse_args([self._path, out])))
        with Reader(out) as r:
            lengths = {s.signal_id: s.length for s in r.signals.values() if s.signal_id}
            self.assertEqual({1, 2}, set(lengths.keys()))
            # the common window is 10 ms shorter than the first signal
            self.assertAlmostEqual(_FS - _FS // 100, lengths[1], delta=2)
            self.assertAlmostEqual(_FS - _FS // 100, lengths[2], delta=2)
            for signal_id in (1, 2):
                np.testing.assert_allclose(r.fsr(signal_id, 0, lengths[signal_id]), float(signal_id))

    def test_single_signal(self):
        out = os.path.join(self._tempdir.name, 'extract.jls')
        p = argparse.ArgumentParser()
        on_cmd = extract.parser_config(p)
        self.assertEqual(0, on_cmd(p.parse_args(['--signals', '1', self._path, out])))
        with Reader(out) as r:
            self.assertEqual(_FS, r.signals[1].length)


class TestPlot(TestCsv):

    def _plot(self, *args):
        try:
            import matplotlib
        except ImportError:
            self.skipTest('matplotlib not installed')
        matplotlib.use('Agg')
        out = os.path.join(self._tempdir.name, 'plot.png')
        p = argparse.ArgumentParser()
        on_cmd = plot.parser_config(p)
        self.assertEqual(0, on_cmd(p.parse_args(list(args) + ['--out', out, self._path])))
        self.assertTrue(os.path.getsize(out) > 0)

    def test_different_extents(self):
        self._plot()

    def test_short_signal(self):
        self._plot('--sample_count', str(_FS * 10))  # fewer samples than points: every sample


if __name__ == '__main__':
    unittest.main()
