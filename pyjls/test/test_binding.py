# Copyright 2021-2025 Jetperch LLC
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

from pyjls.binding import Writer, Reader, SummaryFSR, DataType, jls_inject_log, copy, repair, TimeMap, \
    AnnotationType, SignalType
from pyjls.time64 import SECOND, YEAR
import pyjls
import gc
import io
import logging
from logging import StreamHandler
import os
import subprocess
import sys
import tempfile
import textwrap
import unittest
import numpy as np


_PYJLS_PARENT = os.path.dirname(os.path.dirname(os.path.abspath(pyjls.__file__)))


def _run_script(code, *args, timeout=30):
    """Run code in a new interpreter that imports this pyjls build."""
    env = dict(os.environ)
    env['PYTHONPATH'] = os.pathsep.join([_PYJLS_PARENT, env.get('PYTHONPATH', '')])
    return subprocess.run([sys.executable, '-c', textwrap.dedent(code), *args],
                          env=env, capture_output=True, text=True, timeout=timeout)


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

    def _write_fsr_100(self, w):
        w.source_def(source_id=1, name='n', vendor='v', model='m', version='1', serial_number='s')
        w.signal_def(3, source_id=1, sample_rate=1000, name='current', units='A')
        w.fsr(3, 0, np.zeros(100, dtype=np.float32))

    def test_close_twice_and_use_after_close(self):
        # Previously crashed the interpreter: close() freed the handle and kept it.
        w = Writer(self._path)
        self._write_fsr_100(w)
        w.close()
        w.close()
        with self.assertRaisesRegex(ValueError, 'I/O operation on closed Writer'):
            w.fsr(3, 100, np.zeros(100, dtype=np.float32))
        with Reader(self._path) as r:
            self.assertEqual(100, r.signals[3].length)
            r.close()  # inside the with block, which closes again
        with self.assertRaisesRegex(ValueError, 'I/O operation on closed Reader'):
            r.fsr(3, 0, 10)
        del w, r  # __dealloc__ after close must not close again

    def test_writer_dealloc_closes(self):
        w = Writer(self._path)
        self._write_fsr_100(w)
        del w
        gc.collect()
        with Reader(self._path) as r:
            self.assertEqual(100, r.signals[3].length)

    @unittest.skipUnless(os.path.isdir('/proc/self/fd'), 'requires /proc/self/fd')
    def test_reader_dealloc_closes(self):
        with Writer(self._path) as w:
            self._write_fsr_100(w)
        fd_count = len(os.listdir('/proc/self/fd'))
        r = Reader(self._path)
        self.assertGreater(len(os.listdir('/proc/self/fd')), fd_count)
        del r
        gc.collect()
        self.assertEqual(fd_count, len(os.listdir('/proc/self/fd')))

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
                timestamp = entry * SECOND + YEAR
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
                self.assertEqual(YEAR, r.sample_id_to_timestamp(signal_id, 0))
                self.assertEqual(0, r.timestamp_to_sample_id(signal_id, YEAR))
                t = r.sample_id_to_timestamp(signal_id, (0, 1000000))
                np.testing.assert_equal([YEAR, YEAR + SECOND], t)
                s = r.timestamp_to_sample_id(signal_id, t)
                np.testing.assert_equal([0, 1000000], s)

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

    def test_utc_timemap(self):
        signal_id = 3
        expected = self._utc_gen(signal_id)
        with Reader(self._path) as r:
            self.assertEqual(100, r.time_map_length(signal_id))
            tm = r.time_map_get(signal_id)
            np.testing.assert_equal(expected[:, 0], tm[:]['sample_id'])
            np.testing.assert_equal(expected[:, 1], tm[:]['timestamp'])
            self.assertEqual(100, len(tm))
            self.assertEqual(1, len(r.time_map_get(signal_id, 5)))
            for index in [(90, 95), (90, -5), (-10, -5), (-10, 95)]:
                d = r.time_map_get(signal_id, index)
                self.assertEqual(5, len(d))
                np.testing.assert_equal(tm[90:95], d)
            d = r.time_map_get(signal_id, (3, 2))
            self.assertEqual(0, len(d))

    def test_tmap(self):
        signal_id = 3
        expected = self._utc_gen(signal_id)
        with Reader(self._path) as r:
            tmap = TimeMap(r, signal_id)
            self.assertEqual(100, len(tmap))
            with tmap:
                self.assertEqual(100, len(tmap))
            tm = tmap.time_map_get()
            np.testing.assert_equal(expected[:, 0], tm[:]['sample_id'])
            np.testing.assert_equal(expected[:, 1], tm[:]['timestamp'])
            self.assertEqual(expected[5, 1], tmap.sample_id_to_timestamp(expected[5, 0]))
            np.testing.assert_equal(expected[:, 1], tmap.sample_id_to_timestamp(expected[:, 0]))
            self.assertEqual(expected[5, 0], tmap.timestamp_to_sample_id(expected[5, 1]))
            np.testing.assert_equal(expected[:, 0], tmap.timestamp_to_sample_id(expected[:, 1]))
            self.assertEqual(100, len(tm))
            self.assertEqual(1, len(tmap.time_map_get(5)))
            for index in [(90, 95), (90, -5), (-10, -5), (-10, 95)]:
                d = tmap.time_map_get(index)
                self.assertEqual(5, len(d))
                np.testing.assert_equal(tm[90:95], d)
            d = tmap.time_map_get((3, 2))
            self.assertEqual(0, len(d))

    def _fsr_vsr_gen(self):
        # FSR signal 3 with 100 samples, VSR signal 4
        with Writer(self._path) as w:
            self._write_fsr_100(w)
            w.signal_def(4, source_id=1, signal_type=SignalType.VSR, name='vsr', units='A')

    def _fsr_omit_size(self, enable):
        # Constant u8 data, so every chunk can be omitted when enabled.
        with Writer(self._path) as w:
            w.source_def(source_id=1, name='n')
            w.signal_def(3, source_id=1, sample_rate=100000, data_type='u8', samples_per_data=1000)
            if enable is not None:
                w.fsr_omit_data(3, enable)
            for sample_id in range(0, 200000, 1000):
                w.fsr(3, sample_id, np.full(1000, 7, dtype=np.uint8))
        with Reader(self._path) as r:
            d = r.fsr(3, 0, r.signals[3].length)
        self.assertEqual(200000, len(d))
        np.testing.assert_equal(7, d)
        return os.path.getsize(self._path)

    def test_fsr_omit_data(self):
        size_default = self._fsr_omit_size(None)
        self.assertEqual(size_default, self._fsr_omit_size(False))
        self.assertLess(self._fsr_omit_size(True), size_default // 10)

    def test_writer_signal_id_invalid(self):
        # signal_id values that fit uint16 are validated by the JLS library
        x = np.zeros(10, dtype=np.float32)
        with Writer(self._path) as w:
            w.source_def(source_id=1, name='n')
            for signal_id in [-1, 65536]:
                with self.assertRaisesRegex(ValueError, 'signal_id out of range'):
                    w.signal_def(signal_id, source_id=1)
                with self.assertRaises(OverflowError):  # compatible with earlier versions
                    w.signal_def(signal_id, source_id=1)
            with self.assertRaisesRegex(ValueError, 'PARAMETER_INVALID'):
                w.signal_def(256, source_id=1)
            with self.assertRaisesRegex(ValueError, 'invalid data_type'):
                w.signal_def(3, source_id=1, data_type='f16')
            w.signal_def(3, source_id=1, sample_rate=1000)
            with self.assertRaisesRegex(ValueError, 'ALREADY_EXISTS'):
                w.signal_def(3, source_id=1, sample_rate=1000)
            for signal_id, error in [(7, 'NOT_FOUND'), (300, 'PARAMETER_INVALID')]:
                with self.assertRaisesRegex(ValueError, error):
                    w.fsr(signal_id, 0, x)
                with self.assertRaisesRegex(ValueError, error):
                    w.fsr_omit_data(signal_id, True)
                with self.assertRaisesRegex(ValueError, error):
                    w.annotation(signal_id, 0, None, 'text', 0, 'x')
                with self.assertRaisesRegex(ValueError, error):
                    w.utc(signal_id, 0, 0)
            with self.assertRaisesRegex(ValueError, 'signal_id out of range'):
                w.fsr(70000, 0, x)
            # signal 0 is the predefined VSR signal
            with self.assertRaisesRegex(ValueError, 'NOT_SUPPORTED'):
                w.fsr(0, 0, x)
            with self.assertRaisesRegex(ValueError, 'NOT_SUPPORTED'):
                w.fsr_omit_data(0, True)
            with self.assertRaisesRegex(ValueError, 'NOT_SUPPORTED'):
                w.utc(0, 0, 0)
            w.annotation(0, 0, None, 'text', 0, 'vsr ok')
            w.fsr(3, 0, x)

    def test_writer_annotation_type_invalid(self):
        with Writer(self._path) as w:
            self._write_fsr_100(w)
            with self.assertRaisesRegex(ValueError, 'PARAMETER_INVALID'):
                w.annotation(3, 0, None, 99, 0, 'x')
            with self.assertRaisesRegex(ValueError, 'invalid annotation_type'):
                w.annotation(3, 0, None, 'bogus', 0, 'x')

    def test_writer_empty_data(self):
        with Writer(self._path) as w:
            self._write_fsr_100(w)
            w.fsr(3, 100, np.zeros(0, dtype=np.float32))
            w.annotation(3, 0, None, 'text', 0, None)
            w.annotation(3, 1, float('inf'), AnnotationType.USER, 0, b'')
        with Reader(self._path) as r:
            self.assertEqual(100, r.signals[3].length)
            r.annotations(3, 0, self._on_annotations)
        expected = [
            (0, None, AnnotationType.TEXT, 0, b''),
            (1, None, AnnotationType.USER, 0, b''),
        ]
        self.assertEqual(expected, self.annotations)

    def test_reader_signal_id_invalid(self):
        self._fsr_vsr_gen()
        with Reader(self._path) as r:
            fns = [
                lambda s: r.fsr(s, 0, 10),
                lambda s: r.fsr(s, 0, 0),
                lambda s: r.fsr_statistics(s, 0, 10, 1),
                lambda s: r.fsr_statistics(s, 0, 10, 0),
                lambda s: r.utc(s, 0, self._on_utc),
                lambda s: r.time_map_length(s),
                lambda s: r.time_map_get(s),
                lambda s: r.sample_id_to_timestamp(s, 0),
                lambda s: r.timestamp_to_sample_id(s, 0),
            ]
            for signal_id, error in [(7, 'NOT_FOUND'), (300, 'PARAMETER_INVALID')]:
                for fn in fns:
                    with self.assertRaisesRegex(ValueError, error):
                        fn(signal_id)
                with self.assertRaisesRegex(ValueError, error):
                    r.annotations(signal_id, 0, self._on_annotations)
            for fn in fns:
                with self.assertRaisesRegex(ValueError, 'NOT_SUPPORTED'):
                    fn(4)
            r.annotations(4, 0, self._on_annotations)
        self.assertEqual([], self.annotations)

    def test_reader_length_zero_and_negative(self):
        self._fsr_vsr_gen()
        with Reader(self._path) as r:
            d = r.fsr(3, 0, 0)
            self.assertEqual((0, ), d.shape)
            self.assertEqual(np.float32, d.dtype)
            self.assertEqual((0, SummaryFSR.COUNT), r.fsr_statistics(3, 0, 10, 0).shape)
            with self.assertRaisesRegex(ValueError, 'PARAMETER_INVALID'):
                r.fsr(3, 0, -1)
            with self.assertRaisesRegex(ValueError, 'PARAMETER_INVALID'):
                r.fsr_statistics(3, 0, 10, -1)

    def test_time_map_get_index_out_of_range(self):
        signal_id = 3
        self._utc_gen(signal_id)
        with Reader(self._path) as r:
            for index in [-101, (-200, 1)]:
                with self.assertRaisesRegex(ValueError, 'index out of range'):
                    r.time_map_get(signal_id, index)
            for index in [100, (0, 101), (0, 1 << 40)]:  # rejected by the JLS library
                with self.assertRaisesRegex(ValueError, 'PARAMETER_INVALID'):
                    r.time_map_get(signal_id, index)

    def _callback_cases(self):
        """Write a file with user data, annotations, and utc for signal 3.

        :return: The list of (name, fn(reader, cbk)) for each callback method.
        """
        with Writer(self._path) as w:
            w.source_def(source_id=1, name='n')
            w.signal_def(3, source_id=1, sample_rate=1000, utc_decimate_factor=10)
            w.fsr(3, 0, np.zeros(10, dtype=np.float32))
            for chunk_meta in range(3):
                w.user_data(chunk_meta, b'data')
            for idx in range(3):
                w.annotation(3, idx, None, 'text', 0, 'a')
            for entry in range(100):  # 10 callbacks of 10 entries
                w.utc(3, entry * 1000, YEAR + entry * SECOND)
        return [
            ('user_data', lambda r, cbk: r.user_data(cbk)),
            ('annotations', lambda r, cbk: r.annotations(3, 0, cbk)),
            ('utc', lambda r, cbk: r.utc(3, 0, cbk)),
        ]

    def test_callback_exception_propagates(self):
        class CbkError(Exception):
            pass

        for exc_type in [CbkError, KeyboardInterrupt]:
            for name, fn in self._callback_cases():
                with self.subTest(exc=exc_type.__name__, method=name):
                    calls = []

                    def cbk(*args):
                        calls.append(args)
                        raise exc_type('cbk')

                    with Reader(self._path) as r:
                        with self.assertRaises(exc_type):
                            fn(r, cbk)
                        self.assertEqual(1, len(calls))  # stopped the iteration
                        calls.clear()
                        fn(r, lambda *args: calls.append(args))  # still usable
                        self.assertGreater(len(calls), 1)

    def test_callback_close_raises(self):
        for name, fn in self._callback_cases():
            with self.subTest(method=name), Reader(self._path) as r:
                calls = []

                def cbk(*args):
                    calls.append(args)
                    r.close()  # freed the reader under the iteration

                with self.assertRaisesRegex(RuntimeError, 'in progress'):
                    fn(r, cbk)
                self.assertEqual(1, len(calls))
                self.assertEqual(10, len(r.fsr(3, 0, 10)))  # still open
            with self.assertRaisesRegex(ValueError, 'closed Reader'):
                r.fsr(3, 0, 10)

    def test_callback_nested_read(self):
        for name, fn in self._callback_cases():
            with self.subTest(method=name), Reader(self._path) as r:
                lengths = []
                fn(r, lambda *args: lengths.append(len(r.fsr(3, 0, 10))))
                self.assertGreater(len(lengths), 0)
                self.assertEqual({10}, set(lengths))

    def test_close_from_other_thread_while_busy(self):
        import threading
        self._callback_cases()
        entered = threading.Event()
        release = threading.Event()
        errors = []

        def cbk(*args):
            entered.set()
            release.wait(10)
            return True

        with Reader(self._path) as r:
            t = threading.Thread(target=r.user_data, args=(cbk, ))
            t.start()
            self.assertTrue(entered.wait(10))
            try:
                r.close()
            except RuntimeError as ex:
                errors.append(ex)
            release.set()
            t.join()
            self.assertEqual(1, len(errors))
            self.assertEqual(10, len(r.fsr(3, 0, 10)))  # still open

    def test_writer_close_from_other_thread(self):
        import threading
        x = np.zeros(1000, dtype=np.float32)
        with Writer(self._path) as w:
            self._write_fsr_100(w)

            def run():
                try:
                    for i in range(300):
                        w.fsr(3, 100 + i * 1000, x)
                except ValueError:
                    pass  # closed

            t = threading.Thread(target=run)
            t.start()
            while True:
                try:
                    w.close()
                    break
                except RuntimeError:
                    pass  # busy
            t.join()
        with Reader(self._path) as r:
            self.assertEqual(0, (r.signals[3].length - 100) % 1000)

    def test_user_data_chunk_meta_reserved_bits(self):
        # bits 15:12 are reserved: C rejects them instead of silently masking
        with Writer(self._path) as w:
            with self.assertRaises(ValueError):
                w.user_data(0x1000, b'ab')
            w.user_data(0x0fff, b'ab')
        with Reader(self._path) as r:
            metas = []
            r.user_data(lambda meta, data: metas.append(meta))
            self.assertEqual([0x0fff], metas)

    def test_writer_arg_out_of_range(self):
        x = np.zeros(10, dtype=np.float32)
        with Writer(self._path) as w:
            w.source_def(source_id=1, name='n')
            w.signal_def(3, source_id=1, sample_rate=1000)
            cases = [
                ('source_id', lambda: w.source_def(-1, name='x')),
                ('sample_rate', lambda: w.signal_def(4, 1, sample_rate=-1)),
                ('sample_rate', lambda: w.signal_def(4, 1, sample_rate=1 << 32)),
                ('data_type', lambda: w.signal_def(4, 1, data_type=-1)),
                ('signal_type', lambda: w.signal_def(4, 1, signal_type=256)),
                ('chunk_meta', lambda: w.user_data(-1, b'x')),
                ('sample_id', lambda: w.fsr(3, 1 << 63, x)),
                ('signal_id', lambda: w.fsr_omit_data(-1, True)),
                ('timestamp', lambda: w.annotation(3, 1 << 64, None, 'text', 0, 'x')),
                ('annotation_type', lambda: w.annotation(3, 0, None, -1, 0, 'x')),
                ('group_id', lambda: w.annotation(3, 0, None, 'text', 256, 'x')),
                ('utc', lambda: w.utc(3, 0, 1 << 63)),
            ]
            for name, fn in cases:
                with self.subTest(name=name):
                    with self.assertRaisesRegex(ValueError, f'{name} out of range'):
                        fn()
            with self.assertRaisesRegex(ValueError, 'flags out of range'):
                w.flags = -1
            w.fsr(3, 0, x)

    def test_reader_arg_out_of_range(self):
        self._utc_gen(3)
        with Reader(self._path) as r:
            cases = [
                ('signal_id', lambda: r.fsr(-1, 0, 10)),
                ('start_sample_id', lambda: r.fsr(3, 1 << 63, 10)),
                ('length', lambda: r.fsr_statistics(3, 0, 1, 1 << 63)),
                ('increment', lambda: r.fsr_statistics(3, 0, -1 << 64, 1)),
                ('signal_id', lambda: r.annotations(-1, 0, self._on_annotations)),
                ('timestamp', lambda: r.annotations(3, 1 << 63, self._on_annotations)),
                ('sample_id', lambda: r.utc(3, -(1 << 63) - 1, self._on_utc)),
                ('signal_id', lambda: r.time_map_length(65536)),
                ('sample_id', lambda: r.sample_id_to_timestamp(3, [1 << 63])),
                ('utc_timestamp', lambda: r.timestamp_to_sample_id(3, 1 << 63)),
            ]
            for name, fn in cases:
                with self.subTest(name=name):
                    with self.assertRaisesRegex(ValueError, f'{name} out of range'):
                        fn()

    def test_tmap_int64(self):
        self._utc_gen(3)
        with Reader(self._path) as r:
            t0 = r.sample_id_to_timestamp(3, 0)
            t_neg = t0 - SECOND  # before the first sample
            sample_id = r.timestamp_to_sample_id(3, t_neg)
            self.assertLess(sample_id, 0)
            result = r.timestamp_to_sample_id(3, [t_neg, t0])
            self.assertEqual(np.int64, result.dtype)
            self.assertEqual([sample_id, 0], result.tolist())
            result = r.sample_id_to_timestamp(3, [sample_id, 0])
            self.assertEqual(np.int64, result.dtype)
            self.assertEqual(t0, result[1])
            self.assertEqual(np.int64, r.time_map_get(3).dtype['sample_id'])
            # numpy scalars and 0-d arrays convert like scalars
            self.assertEqual(sample_id, r.timestamp_to_sample_id(3, np.array(t_neg)))
            self.assertEqual(t0, r.sample_id_to_timestamp(3, np.array(0)))
            self.assertEqual(t0, r.sample_id_to_timestamp(3, np.int64(0)))

    def test_fsr_statistics_dtype(self):
        with Writer(self._path) as w:
            self._write_fsr_100(w)
        with Reader(self._path) as r:
            self.assertEqual(np.float64, r.fsr_statistics(3, 0, 10, 2).dtype)

    def test_path_like(self):
        import pathlib
        path = pathlib.Path(self._path)
        with Writer(path) as w:
            self._write_fsr_100(w)
        with Reader(path) as r:
            self.assertEqual(100, r.signals[3].length)
        with Reader(os.fsencode(self._path)) as r:
            self.assertEqual(100, r.signals[3].length)
        dst = pathlib.Path(self._path + '.copy.jls')
        try:
            copy(path, dst)
            repair(dst)
            with Reader(dst) as r:
                self.assertEqual(100, r.signals[3].length)
        finally:
            if dst.is_file():
                dst.unlink()

    def test_init_twice_closes(self):
        path2 = self._path + '.2.jls'
        try:
            w = Writer(self._path)
            self._write_fsr_100(w)
            w.__init__(path2)
            with Reader(self._path) as r:  # finalized by __init__
                self.assertEqual(100, r.signals[3].length)
            self._write_fsr_100(w)
            w.close()
            r = Reader(self._path)
            r.__init__(path2)
            self.assertEqual(100, r.signals[3].length)
            r.close()
        finally:
            os.remove(path2)

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

    # Writes with sample_id gaps make the writer thread log a warning
    # while it holds the lock that signal_def() waits on.
    _SKIP_WARNINGS_WRITE = """
        import sys, numpy as np
        from pyjls import Writer, Reader
        w = Writer(sys.argv[1])
        w.source_def(1, name='s')
        w.signal_def(1, 1, sample_rate=1000, name='a')
        for i in range(200):
            w.fsr(1, i * 2000, np.zeros(1000, dtype=np.float32))
        w.signal_def(2, 1, sample_rate=1000, name='b')
    """

    def test_writer_log_from_writer_thread_does_not_deadlock(self):
        code = self._SKIP_WARNINGS_WRITE + """
        w.close()
        with Reader(sys.argv[1]) as r:
            print(r.signals[1].length, len(r.signals))
        """
        for _ in range(3):  # hung in 2 of 3 runs before the log queue
            p = _run_script(code, self._path)
            self.assertEqual(0, p.returncode, p.stderr)
            self.assertIn('399000 3', p.stdout)
            self.assertIn('skip: in=2000', p.stderr)  # writer thread log forwarded

    def test_writer_open_at_exit_closes(self):
        # Hung at interpreter exit before the log queue.
        p = _run_script(self._SKIP_WARNINGS_WRITE, self._path)
        self.assertEqual(0, p.returncode, p.stderr)
        with Reader(self._path) as r:
            self.assertEqual(399000, r.signals[1].length)
            self.assertEqual(3, len(r.signals))

    def test_writer_referenced_at_exit_closes(self):
        # A daemon thread keeps the Writer alive past finalization,
        # so only the atexit handler closes it.
        code = self._SKIP_WARNINGS_WRITE + """
        import threading, time
        threading.Thread(target=lambda x: time.sleep(100), args=(w, ), daemon=True).start()
        del w
        """
        p = _run_script(code, self._path)
        self.assertEqual(0, p.returncode, p.stderr)
        with Reader(self._path) as r:
            self.assertEqual(399000, r.signals[1].length)

    def test_writer_flush(self):
        with Writer(self._path) as w:
            self._write_fsr_100(w)
            w.flush()
            w.flags = Writer.FLAG_DROP_ON_OVERFLOW
            self.assertEqual(Writer.FLAG_DROP_ON_OVERFLOW, w.flags)
            w.flags = 0

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

    def _repair_case_create(self):
        """Write a file, then corrupt 64 bytes in the sample data region."""
        data = np.arange(110000, dtype=np.float32)
        with Writer(self._path) as w:
            w.source_def(source_id=1, name='name', vendor='vendor', model='model',
                         version='version', serial_number='serial_number')
            w.signal_def(3, source_id=1, sample_rate=1000000, name='current', units='A')
            w.fsr(3, 0, data)
        sz = os.path.getsize(self._path)
        with open(self._path, 'r+b') as f:
            f.seek(sz // 2)
            f.write(b'\x5a' * 64)
        return data

    def test_repair(self):
        msgs = []
        progresses = []
        data = self._repair_case_create()
        repair(self._path, msgs.append, progresses.append)
        self.assertEqual(1.0, progresses[-1])
        self.assertEqual(progresses, sorted(progresses))  # monotonic
        for msg in msgs:
            self.assertIsInstance(msg, str)
        with Reader(self._path) as r:
            self.assertEqual(len(data), r.signals[3].length)
            r.fsr(3, 0, len(data))  # reads complete without error

    def test_repair_cancel(self):
        self._repair_case_create()
        with self.assertRaises(RuntimeError):
            repair(self._path, None, lambda progress: True)
        repair(self._path)  # canceled repair reruns to completion
        with Reader(self._path) as r:
            self.assertEqual(3, r.signals[3].signal_id)

    def test_repair_msg_cancel(self):
        self._repair_case_create()
        with self.assertRaises(RuntimeError):
            repair(self._path, lambda msg: True)
        repair(self._path)  # canceled repair reruns to completion
        with Reader(self._path) as r:
            self.assertEqual(3, r.signals[3].signal_id)

    def test_repair_callback_exception_propagates(self):
        class CbkError(Exception):
            pass

        for exc_type in [CbkError, KeyboardInterrupt]:
            with self.subTest(exc=exc_type.__name__):
                self._repair_case_create()

                def raise_exc(*args):
                    raise exc_type('cbk')

                with self.assertRaises(exc_type):
                    repair(self._path, None, raise_exc)
                with self.assertRaises(exc_type):
                    repair(self._path, raise_exc)
                dst = self._path + '.copy.jls'
                with self.assertRaises(exc_type):
                    copy(self._path, dst, None, raise_exc)
                if os.path.isfile(dst):
                    os.remove(dst)
                repair(self._path)

    def test_repair_missing_file(self):
        with self.assertRaises(RuntimeError):
            repair(self._path + '.does_not_exist.jls')

    @unittest.skipUnless(sys.platform.startswith('linux'), 'uses RLIMIT_FSIZE')
    def test_writer_close_reports_write_error(self):
        # Writes past RLIMIT_FSIZE fail with EFBIG: close must report it.
        code = """
            import resource, signal, sys
            import numpy as np
            from pyjls import Writer
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (1 << 20, 1 << 20))
            w = Writer(sys.argv[1])
            w.source_def(1, name='s')
            w.signal_def(1, 1, data_type='f32', sample_rate=1000000)
            x = np.zeros(100000, dtype=np.float32)
            try:
                for i in range(100):
                    w.fsr(1, i * len(x), x)
            except RuntimeError:
                pass  # the latched error may surface on a later fsr()
            try:
                w.close()
            except RuntimeError as ex:
                print('close raised', ex)
                sys.exit(0)
            sys.exit(3)
        """
        with tempfile.TemporaryDirectory() as d:
            p = _run_script(code, os.path.join(d, 'full.jls'))
        self.assertEqual(0, p.returncode, p.stdout + p.stderr)
        self.assertIn('close raised', p.stdout)

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
