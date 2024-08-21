# Copyright 2021-2024 Jetperch LLC
#
# Licensed under the Apache License, Version 2.0 (the 'License');
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an 'AS IS' BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from pyjls.binding import SECOND, Writer, Reader, SummaryFSR, DataType, jls_inject_log, copy
import io
import logging
from logging import StreamHandler
import os
import tempfile
import unittest
import numpy as np


class TestBinding(unittest.TestCase):

    def setUp(self):
        f = tempfile.NamedTemporaryFile(delete=False, suffix='.jls')
        f.close()
        self._path = f.name
        self.user_data = []
        self.annotations = []
        self._utc = []

    def _on_user_data(self, *args):
        self.user_data.append(args)

    def _on_annotations(self, *args):
        self.annotations.append(args)

    def _on_utc(self, entries):
        self._utc.append(entries)
        return False

    @property
    def utc(self):
        return np.concatenate(self._utc)

    def tearDown(self) -> None:
        if os.path.isfile(self._path):
            os.remove(self._path)

    def test_source_with_none_strs(self):
        with Writer(self._path) as w:
            w.source_def(source_id=1, name='name', vendor='vendor', model='',
                         version=None, serial_number='serial_number')
            w.signal_def(3, source_id=1, sample_rate=1000000, name='current', units='A')
        with Reader(self._path) as r:
            self.assertEqual(2, len(r.sources))
            self.assertEqual([0, 1], sorted(r.sources.keys()))
            s = r.sources[1]
            self.assertEqual(1, s.source_id)
            self.assertEqual('name', s.name)
            self.assertEqual('', s.model)
            self.assertEqual('', s.version)

    def test_fsr_f32(self):
        data = np.arange(110000, dtype=np.float32)
        with Writer(self._path) as w:
            w.source_def(source_id=1, name='name', vendor='vendor', model='model',
                         version='version', serial_number='serial_number')
            w.signal_def(3, source_id=1, sample_rate=1000000, name='current', units='A')
            w.fsr(3, 0, data)

        with Reader(self._path) as r:
            self.assertEqual(2, len(r.sources))
            self.assertEqual([0, 1], sorted(r.sources.keys()))
            s = r.sources[1]
            self.assertEqual(1, s.source_id)
            self.assertEqual('name', s.name)
            self.assertEqual('vendor', s.vendor)
            self.assertEqual('model', s.model)
            self.assertEqual('version', s.version)
            self.assertEqual('serial_number', s.serial_number)

            self.assertEqual(2, len(r.signals))
            self.assertEqual([0, 3], sorted(r.signals.keys()))
            s = r.signals[3]
            self.assertEqual(3, s.signal_id)
            self.assertEqual(1, s.source_id)
            self.assertEqual(0, s.signal_type)
            self.assertEqual(1000000, s.sample_rate)
            self.assertEqual(8192, s.samples_per_data)
            self.assertEqual(128, s.sample_decimate_factor)
            self.assertEqual(640, s.entries_per_summary)
            self.assertEqual(20, s.summary_decimate_factor)
            self.assertEqual(100, s.annotation_decimate_factor)
            self.assertEqual(100, s.utc_decimate_factor)
            self.assertEqual('current', s.name)
            self.assertEqual('A', s.units)
            self.assertEqual(len(data), s.length)

            np.testing.assert_allclose(data, r.fsr(3, 0, len(data)))
            stats = r.fsr_statistics(3, 0, len(data), 1)
            np.testing.assert_allclose(np.mean(data, dtype=np.float64), stats[0, SummaryFSR.MEAN])
            np.testing.assert_allclose(np.min(data), stats[0, SummaryFSR.MIN])
            np.testing.assert_allclose(np.max(data), stats[0, SummaryFSR.MAX])
            np.testing.assert_allclose(np.std(data, ddof=1), stats[0, SummaryFSR.STD], rtol=1e-6)

    def test_fsr_u1(self):
        data = np.zeros(1024, dtype=np.uint8)
        data[1::2] = 1
        src = np.packbits(data, bitorder='little')

        with Writer(self._path) as w:
            w.source_def(source_id=1, name='name', vendor='vendor', model='model',
                         version='version', serial_number='serial_number')
            w.signal_def(3, source_id=1, data_type=DataType.U1, sample_rate=1000000, name='current', units='A')
            w.fsr(3, 0, src)

        with Reader(self._path) as r:
            self.assertEqual(2, len(r.sources))
            self.assertEqual([0, 1], sorted(r.sources.keys()))
            self.assertEqual(1, r.sources[1].source_id)
            self.assertEqual(2, len(r.signals))
            s = r.signals[3]
            self.assertEqual(DataType.U1, r.signals[3].data_type)
            self.assertEqual(len(data), s.length)

            dst = r.fsr(3, 0, s.length)
            dst_data = np.unpackbits(dst, count=s.length, bitorder='little')
            np.testing.assert_allclose(data, dst_data)

            stats = r.fsr_statistics(3, 0, s.length, 1)
            np.testing.assert_allclose(np.mean(data), stats[0, SummaryFSR.MEAN])
            np.testing.assert_allclose(np.min(data), stats[0, SummaryFSR.MIN])
            np.testing.assert_allclose(np.max(data), stats[0, SummaryFSR.MAX])
            np.testing.assert_allclose(np.std(data, ddof=1), stats[0, SummaryFSR.STD], rtol=1e-6)

    def test_user_data(self):
        data = [
            (1, b'user binary'),
            (2, 'user string'),
            (3, {'user': 'json'}),
        ]
        with Writer(self._path) as w:
            for d in data:
                w.user_data(*d)
        with Reader(self._path) as r:
            r.user_data(self._on_user_data)
        self.assertEqual(data, self.user_data)

    def _annotation_gen(self, signal_id):
        annotations = [
            (0, 2.0,  3, 23, '2'),
            (10, None, 0, 20, b'annotation binary'),
            (11, None, 1, 21, 'annotation str'),
            (12, 1.0,  2, 22, '1'),
        ]
        with Writer(self._path) as w:
            fs = 1000000
            w.source_def(source_id=1, name='name', vendor='vendor', model='model',
                         version='version', serial_number='serial_number')
            w.signal_def(signal_id=signal_id, source_id=1, sample_rate=fs, name='current', units='A')
            for a in annotations:
                w.annotation(signal_id, *a)
        return annotations

    def test_annotation(self):
        signal_id = 3
        expected = self._annotation_gen(signal_id)
        with Reader(self._path) as r:
            r.annotations(signal_id, 0, self._on_annotations)
        self.assertEqual(expected, self.annotations)

    def test_annotation_seek(self):
        signal_id = 3
        expected = self._annotation_gen(signal_id)
        with Reader(self._path) as r:
            r.annotations(signal_id, expected[2][0], self._on_annotations)
        self.assertEqual(expected[2:], self.annotations)

    def _utc_gen(self, signal_id, sample_id_offset=None):
        sample_id_offset = 0 if sample_id_offset is None else int(sample_id_offset)
        data = []
        with Writer(self._path) as w:
            fs = 1000000
            w.source_def(source_id=1, name='name', vendor='vendor', model='model',
                         version='version', serial_number='serial_number')
            w.signal_def(signal_id=signal_id, source_id=1, sample_rate=fs, name='current', units='A')
            w.fsr_f32(3, sample_id_offset, np.array([1, 2, 3, 4], dtype=np.float32))
            for entry in range(100):
                sample_id = entry * fs
                timestamp = entry + 60 * 60 * 24 * 365  # in seconds
                data.append([sample_id, timestamp])
                w.utc(signal_id, sample_id + sample_id_offset, timestamp)
        return np.array(data, dtype=np.int64)

    def test_utc(self):
        signal_id = 3
        path_base, path_ext = os.path.splitext(self._path)
        for idx in range(2):
            self.subTest(idx)
            self._path = f'{path_base}_{idx:04d}{path_ext}'
            sample_id_offset = idx * 1_000_000
            expected = self._utc_gen(signal_id, sample_id_offset=sample_id_offset)
            with Reader(self._path) as r:
                self.assertEqual(sample_id_offset, r.signals[signal_id].sample_id_offset)
                r.utc(signal_id, 0, self._on_utc)
                self.assertEqual(60 * 60 * 24 * 365, r.sample_id_to_timestamp(signal_id, 0))
                self.assertEqual(0, r.timestamp_to_sample_id(signal_id, 60 * 60 * 24 * 365))
            np.testing.assert_equal(expected, self.utc)
            os.remove(self._path)
            self._utc = []

    def test_utc_seek(self):
        signal_id = 3
        expected = self._utc_gen(signal_id)
        expected = expected[len(expected) // 2:, :]
        with Reader(self._path) as r:
            r.utc(signal_id, expected[0, 0], self._on_utc)
        np.testing.assert_equal(expected, self.utc)

    def test_log(self):
        log = logging.getLogger('pyjls.c')
        formatter = logging.Formatter("%(levelname)s:%(filename)s:%(lineno)d:%(name)s:%(message)s")
        stream = io.StringIO()
        stream_handler = StreamHandler(stream)
        stream_handler.setFormatter(formatter)
        log.addHandler(stream_handler)
        jls_inject_log('I', 'hello', 10, 'world')
        jls_inject_log('D', 'debug', 11, 'debug1')
        stream_handler.setLevel(logging.INFO)
        jls_inject_log('D', 'debug', 12, 'debug2')
        expect = ['INFO:hello:10:pyjls.c:world', 'DEBUG:debug:11:pyjls.c:debug1', '']
        self.assertEqual('\n'.join(expect), stream.getvalue())

    def test_copy(self):
        msgs = []
        progresses = []

        def on_msg(msg):
            msgs.append(msg)

        def on_progress(progress):
            progresses.append(progress)

        data = np.arange(110000, dtype=np.float32)
        with Writer(self._path) as w:
            w.source_def(source_id=1, name='name', vendor='vendor', model='model',
                         version='version', serial_number='serial_number')
            w.signal_def(3, source_id=1, sample_rate=1000000, name='current', units='A')
            w.fsr(3, 0, data)

        dst = tempfile.NamedTemporaryFile(delete=False, suffix='.jls')
        dst.close()
        try:
            copy(self._path, dst.name, on_msg, on_progress)
        finally:
            self.assertTrue(os.path.isfile(dst.name))
            os.remove(dst.name)
        self.assertEqual(1.0, progresses[-1])

    def test_signal_lookup(self):
        with Writer(self._path) as w:
            w.source_def(source_id=1, name='src1', vendor='vendor', model='model',
                         version='version', serial_number='serial_number')
            w.source_def(source_id=2, name='src2', vendor='vendor', model='model',
                         version='version', serial_number='serial_number')
            w.signal_def(3, source_id=1, sample_rate=1000000, name='current', units='A')
            w.signal_def(4, source_id=2, sample_rate=1000000, name='current', units='A')
            # be unfriendly/confusing and give string name as another signal_id
            w.signal_def(5, source_id=2, sample_rate=1000000, name='4', units='A')

        with Reader(self._path) as r:
            self.assertEqual(1, r.source_lookup(1).source_id)
            self.assertEqual(1, r.source_lookup('1').source_id)
            self.assertEqual(1, r.source_lookup('src1').source_id)

            signal = r.signal_lookup('current')
            self.assertEqual(3, signal.signal_id)
            self.assertEqual(3, r.signal_lookup(3).signal_id)
            self.assertEqual(3, r.signal_lookup('3').signal_id)
            self.assertEqual(3, r.signal_lookup('src1.current').signal_id)
            self.assertEqual(4, r.signal_lookup('src2.current').signal_id)
            self.assertEqual(4, r.signal_lookup(['src2', 'current']).signal_id)
            self.assertEqual(4, r.signal_lookup([2, 'current']).signal_id)
            self.assertEqual(4, r.signal_lookup([2, 4]).signal_id)
            self.assertEqual(5, r.signal_lookup('4').signal_id)
