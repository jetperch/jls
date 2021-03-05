# Copyright 2021 Jetperch LLC
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

from pyjls.binding import Writer, Reader, SummaryFSR
import numpy as np
import os
import tempfile
import unittest
import numpy as np


class TestBinding(unittest.TestCase):

    def setUp(self):
        f = tempfile.NamedTemporaryFile(delete=False, suffix='.jls')
        f.close()
        self._path = f.name

    def tearDown(self) -> None:
        if os.path.isfile(self._path):
            os.remove(self._path)

    def test_basic(self):
        data = np.arange(110000, dtype=np.float32)
        with Writer(self._path) as w:
            w.source_def(source_id=1, name='name', vendor='vendor', model='model',
                         version='version', serial_number='serial_number')
            w.signal_def(3, source_id=1, sample_rate=1000000, name='current', units='A')
            w.user_data(1, b'user binary')
            w.user_data(2, 'user string')
            w.user_data(3, {'user': 'json'})
            w.fsr_f32(3, 0, data)
            w.annotation(3, 10, 'user', 20, b'annotation binary')
            w.annotation(3, 11, 'str', 21, 'annotation str')
            w.annotation(3, 12, 'marker', 22, '1')

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
            self.assertEqual(100000, s.samples_per_data)
            self.assertEqual(100, s.sample_decimate_factor)
            self.assertEqual(20000, s.entries_per_summary)
            self.assertEqual(100, s.summary_decimate_factor)
            self.assertEqual(100, s.annotation_decimate_factor)
            self.assertEqual(100, s.utc_decimate_factor)
            self.assertEqual('current', s.name)
            self.assertEqual('A', s.units)
            self.assertEqual(len(data), s.length)

            np.testing.assert_allclose(data, r.fsr(3, 0, len(data)))
            e_std = np.std(data, dtype=np.float64) * (len(data) / (len(data) - 1))
            stats = r.fsr_statistics(3, 0, len(data), 1)
            np.testing.assert_allclose(np.mean(data, dtype=np.float64), stats[0, SummaryFSR.MEAN])
            np.testing.assert_allclose(np.min(data), stats[0, SummaryFSR.MIN])
            np.testing.assert_allclose(np.max(data), stats[0, SummaryFSR.MAX])
            np.testing.assert_allclose(e_std, stats[0, SummaryFSR.STD], rtol=1e-4)

            annotations = []

            def annotations_fn(*args):
                annotations.append(args)

            r.annotations(3, 0, annotations_fn)
            self.assertEqual(3, len(annotations))
            self.assertEqual((10, 0, 20, b'annotation binary'), annotations[0])
            self.assertEqual((11, 1, 21, 'annotation str'), annotations[1])
            self.assertEqual((12, 2, 22, '1'), annotations[2])

            user_data = []
            def user_data_fn(*args):
                user_data.append(args)

            r.user_data(user_data_fn)
            self.assertEqual(3, len(user_data))
            self.assertEqual((1, b'user binary'), user_data[0])
            self.assertEqual((2, 'user string'), user_data[1])
            self.assertEqual((3, {'user': 'json'}), user_data[2])
