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
Python binding for the native JLS implementation.
"""

# See https://cython.readthedocs.io/en/latest/index.html

# cython: boundscheck=True, wraparound=True, nonecheck=True, overflowcheck=True, cdivision=True

from libc.stdint cimport uint8_t, uint16_t, uint32_t, uint64_t, int32_t, int64_t
from libc.float cimport DBL_MAX
from libc.math cimport isfinite, NAN

from collections.abc import Mapping
import json
import logging
import numpy as np
import time
cimport numpy as np
from . cimport c_jls
from .structs import SourceDef, SignalDef


__all__ = ['DataType', 'AnnotationType', 'SignalType', 'Writer', 'Reader',
           'SourceDef', 'SignalDef', 'SummaryFSR', 'jls_inject_log']


_log_c_name = 'pyjls.c'
_log_c = logging.getLogger(_log_c_name)


def _data_type_def(basetype, size, q):
    return (basetype & 0x0f) | ((size & 0xff) << 8) | ((q & 0xff) << 16)


class _DataTypeBase:
    INT       = 0x01
    UNSIGNED  = 0x02
    UINT      = 0x03
    FLOAT     = 0x04


class DataType:
    F32 = _data_type_def(_DataTypeBase.FLOAT, 32, 0)


class AnnotationType:
    USER = c_jls.JLS_ANNOTATION_TYPE_USER
    TEXT = c_jls.JLS_ANNOTATION_TYPE_TEXT
    MARKER = c_jls.JLS_ANNOTATION_TYPE_MARKER


_annotation_map = {
    'user': AnnotationType.USER,
    'usr': AnnotationType.USER,
    'text': AnnotationType.TEXT,
    'txt': AnnotationType.TEXT,
    'str': AnnotationType.TEXT,
    'string': AnnotationType.TEXT,
    'marker': AnnotationType.MARKER,
}

_log_level_map = {
    '!': logging.CRITICAL,
    'A': logging.CRITICAL,
    'C': logging.CRITICAL,
    'E': logging.ERROR,
    'W': logging.WARNING,
    'N': logging.INFO,
    'I': logging.INFO,
    'D': logging.DEBUG,
    'D': logging.DEBUG,
    'D': logging.DEBUG,
}


class SignalType:
    FSR = c_jls.JLS_SIGNAL_TYPE_FSR
    VSR = c_jls.JLS_SIGNAL_TYPE_VSR


class SummaryFSR:
    MEAN = c_jls.JLS_SUMMARY_FSR_MEAN
    STD = c_jls.JLS_SUMMARY_FSR_STD
    MIN = c_jls.JLS_SUMMARY_FSR_MIN
    MAX = c_jls.JLS_SUMMARY_FSR_MAX
    COUNT = c_jls.JLS_SUMMARY_FSR_COUNT


cdef void _log_cbk(const char * msg):
    m = msg.decode('utf-8').strip()
    level, location, s = m.split(' ', 2)
    lvl = _log_level_map.get(level, logging.DEBUG)
    filename, line, _ = location.split(':')
    record = logging.LogRecord(_log_c_name, lvl, filename, int(line), s, None, None)
    _log_c.handle(record)


c_jls.jls_log_register(_log_cbk)


def jls_inject_log(level, filename, line, msg):
    cdef char * c_msg
    location = ':'.join([filename, str(line), ''])
    msg = ' '.join([level, location, msg]).encode('utf-8')
    c_msg = msg
    c_jls.jls_log_printf('%s\n'.encode('utf-8'), c_msg)


def _encode_str(s):
    if s is None:
        return s
    else:
        return s.encode('utf-8')


def _storage_pack(data):
    if isinstance(data, str):
        s = _encode_str(data)
        return c_jls.JLS_STORAGE_TYPE_STRING, s, len(s) + 1
    elif isinstance(data, bytes):
        return c_jls.JLS_STORAGE_TYPE_BINARY, data, len(data)
    else:
        s = _encode_str(json.dumps(data))
        return c_jls.JLS_STORAGE_TYPE_JSON, s, len(s) + 1


cdef _storage_unpack(uint8_t storage_type, const uint8_t * data, uint32_t data_size):
    cdef const char * str = <const char *> data
    if storage_type == c_jls.JLS_STORAGE_TYPE_STRING:
        return str[:data_size - 1].decode('utf-8')
    elif storage_type == c_jls.JLS_STORAGE_TYPE_BINARY:
        return data[:data_size]
    else:
        return json.loads(str[:data_size - 1].decode('utf-8'))


cdef class Writer:
    cdef c_jls.jls_twr_s * _wr

    def __init__(self, path: str):
        cdef int32_t rc
        rc = c_jls.jls_twr_open(&self._wr, path.encode('utf-8'))
        if rc:
            raise RuntimeError(f'open failed {rc}')

    def __enter__(self):
        return self

    def __exit__(self, type, value, traceback):
        self.close()

    def close(self):
        c_jls.jls_twr_close(self._wr)

    def flush(self):
        c_jls.jls_twr_flush(self._wr)

    def source_def(self, source_id, name=None, vendor=None, model=None, version=None, serial_number=None):
        cdef int32_t rc
        cdef c_jls.jls_source_def_s s
        name_b = _encode_str(name)
        vendor_b = _encode_str(vendor)
        model_b = _encode_str(model)
        version_b = _encode_str(version)
        serial_number_b = _encode_str(serial_number)
        s.source_id = source_id
        s.name = name_b
        s.vendor = vendor_b
        s.model = model_b
        s.version = version_b
        s.serial_number = serial_number_b
        rc = c_jls.jls_twr_source_def(self._wr, &s)
        if rc:
            raise RuntimeError(f'source_def failed {rc}')

    def source_def_from_struct(self, s: SourceDef):
        return self.source_def(s.source_id,
                               name=s.name,
                               vendor=s.vendor,
                               model=s.model,
                               version=s.version,
                               serial_number=s.serial_number)

    def signal_def(self, signal_id, source_id, signal_type=None, data_type=None, sample_rate=None,
                   samples_per_data=None, sample_decimate_factor=None, entries_per_summary=None,
                   summary_decimate_factor=None, annotation_decimate_factor=None, utc_decimate_factor=None,
                   name=None, units=None):
        cdef int32_t rc
        cdef c_jls.jls_signal_def_s s
        s.signal_id = signal_id
        s.source_id = source_id
        s.signal_type = 0 if signal_type is None else int(signal_type)
        s.data_type = DataType.F32 if data_type is None else data_type
        s.sample_rate = 0 if sample_rate is None else int(sample_rate)
        s.samples_per_data = 100000 if samples_per_data is None else int(samples_per_data)
        s.sample_decimate_factor = 100 if sample_decimate_factor is None else int(sample_decimate_factor)
        s.entries_per_summary = 20000 if entries_per_summary is None else int(entries_per_summary)
        s.summary_decimate_factor = 100 if summary_decimate_factor is None else int(summary_decimate_factor)
        s.annotation_decimate_factor = 100 if annotation_decimate_factor is None else int(annotation_decimate_factor)
        s.utc_decimate_factor = 100 if utc_decimate_factor is None else int(utc_decimate_factor)
        name_b = _encode_str(name)
        units_b = _encode_str(units)
        s.name = name_b
        s.units = units_b
        rc = c_jls.jls_twr_signal_def(self._wr, &s)
        if rc:
            raise RuntimeError(f'signal_def failed {rc}')

    def signal_def_from_struct(self, s: SignalDef):
        return self.signal_def(s.signal_id,
                               s.source_id,
                               signal_type=s.signal_type,
                               data_type=s.data_type,
                               sample_rate=s.sample_rate,
                               samples_per_data=s.samples_per_data,
                               sample_decimate_factor=s.sample_decimate_factor,
                               entries_per_summary=s.entries_per_summary,
                               summary_decimate_factor=s.summary_decimate_factor,
                               annotation_decimate_factor=s.annotation_decimate_factor,
                               utc_decimate_factor=s.utc_decimate_factor,
                               name=s.name,
                               units=s.units)

    def user_data(self, chunk_meta, data):
        cdef int32_t rc
        storage_type, payload, payload_length = _storage_pack(data)
        rc = c_jls.jls_twr_user_data(self._wr, chunk_meta, storage_type, payload, payload_length)
        if rc:
            raise RuntimeError(f'user_data failed {rc}')

    def fsr_f32(self, signal_id, sample_id, data):
        cdef int32_t rc
        cdef np.float32_t [::1] f32 = data
        rc = c_jls.jls_twr_fsr_f32(self._wr, signal_id, sample_id, &f32[0], len(data))
        if rc:
            raise RuntimeError(f'fsr_f32 failed {rc}')

    def annotation(self, signal_id, timestamp, annotation_type, group_id, y, data):
        cdef int32_t rc
        if isinstance(annotation_type, str):
            annotation_type = _annotation_map[annotation_type.lower()]
        storage_type, payload, payload_length = _storage_pack(data)
        if y is None or not np.isfinite(y):
            y = NAN
        rc = c_jls.jls_twr_annotation(self._wr, signal_id, timestamp, annotation_type,
            group_id, storage_type, y, payload, payload_length)
        if rc:
            raise RuntimeError(f'annotation failed {rc}')

    def utc(self, signal_id, sample_id, utc):
        cdef int32_t rc
        rc = c_jls.jls_twr_utc(self._wr, signal_id, sample_id, utc)
        if rc:
            raise RuntimeError(f'utc failed {rc}')


cdef class Reader:
    cdef c_jls.jls_rd_s * _rd
    cdef object _sources
    cdef object _signals

    def __init__(self, path: str):
        cdef int32_t rc
        cdef c_jls.jls_source_def_s * sources
        cdef c_jls.jls_signal_def_s * signals
        cdef uint16_t count
        cdef int64_t samples
        self._sources: Mapping[int, SourceDef] = {}
        self._signals: Mapping[int, SignalDef] = {}
        rc = c_jls.jls_rd_open(&self._rd, path.encode('utf-8'))
        if rc:
            raise RuntimeError(f'open failed {rc}')

        rc = c_jls.jls_rd_sources(self._rd, &sources, &count)
        if rc:
            raise RuntimeError(f'read sources failed {rc}')
        for i in range(count):
            source_def = SourceDef(
                source_id=sources[i].source_id,
                name=sources[i].name.decode('utf-8'),
                vendor=sources[i].vendor.decode('utf-8'),
                model=sources[i].model.decode('utf-8'),
                version=sources[i].version.decode('utf-8'),
                serial_number=sources[i].serial_number.decode('utf-8'))
            self._sources[sources[i].source_id] = source_def

        rc = c_jls.jls_rd_signals(self._rd, &signals, &count)
        if rc:
            raise RuntimeError(f'read signals failed {rc}')
        for i in range(count):
            signal_id = signals[i].signal_id
            signal_def = SignalDef(
                signal_id=signal_id,
                source_id=signals[i].source_id,
                signal_type=signals[i].signal_type,
                data_type=signals[i].data_type,
                sample_rate=signals[i].sample_rate,
                samples_per_data=signals[i].samples_per_data,
                sample_decimate_factor=signals[i].sample_decimate_factor,
                entries_per_summary=signals[i].entries_per_summary,
                summary_decimate_factor=signals[i].summary_decimate_factor,
                annotation_decimate_factor=signals[i].annotation_decimate_factor,
                utc_decimate_factor=signals[i].utc_decimate_factor,
                name=signals[i].name.decode('utf-8'),
                units=signals[i].units.decode('utf-8'))
            if signal_def.signal_type == 0:
                rc = c_jls.jls_rd_fsr_length(self._rd, signal_id, &samples)
                if rc:
                    raise RuntimeError(f'fsr signal length failed {rc}')
                signal_def.length = samples
            self._signals[signal_id] = signal_def

    def __enter__(self):
        return self

    def __exit__(self, type, value, traceback):
        self.close()

    def close(self):
        c_jls.jls_rd_close(self._rd)

    @property
    def sources(self) -> Mapping[int, SourceDef]:
        return self._sources

    @property
    def signals(self) -> Mapping[int, SignalDef]:
        return self._signals

    def fsr(self, signal_id, start_sample_id, length):
        cdef int32_t rc
        cdef np.float32_t [::1] c_data
        data = np.empty(length, dtype=np.float32)
        c_data = data
        rc = c_jls.jls_rd_fsr_f32(self._rd, signal_id, start_sample_id, &c_data[0], length)
        if rc:
            raise RuntimeError(f'fsr failed {rc}')
        return data

    def fsr_statistics(self, signal_id, start_sample_id, increment, length):
        """Read FSR statistics.

        :param signal_id: The signal id.
        :param start_sample_id: The starting sample id to read.
        :param increment: The number of samples represented per return value.
        :param length: The number of return values to generate.
        :return The 2-D array[summary][stat] where the stat column is defined by SummaryFSR.
        """

        cdef int32_t rc
        cdef np.float32_t [:, :] c_data
        data = np.empty((length, 4), dtype=np.float32)
        c_data = data
        rc = c_jls.jls_rd_fsr_f32_statistics(self._rd, signal_id, start_sample_id, increment, &c_data[0, 0], length)
        if rc:
            raise RuntimeError(f'fsr_statistics failed {rc}')
        return data

    def annotations(self, signal_id, timestamp, cbk_fn):
        """Read annotations from a signal.

        :param signal_id: The signal id.
        :param timestamp: The starting timestamp.
        :param cbk: The function(timestamp, annotation_type, group_id, y, data)
            to call for each annotation.  Return True to stop iteration over
            the annotations or False to continue iterating.
        """
        cdef int32_t rc
        rc = c_jls.jls_rd_annotations(self._rd, signal_id, timestamp, _annotation_cbk_fn, <void *> cbk_fn)
        if rc:
            raise RuntimeError(f'annotations failed {rc}')

    def user_data(self, cbk_fn):
        cdef int32_t rc
        rc = c_jls.jls_rd_user_data(self._rd, _user_data_cbk_fn, <void *> cbk_fn)
        if rc:
            raise RuntimeError(f'annotations failed {rc}')


cdef int32_t _annotation_cbk_fn(void * user_data, const c_jls.jls_annotation_s * annotation):
    cbk_fn = <object> user_data
    data = _storage_unpack(annotation[0].storage_type, annotation[0].data, annotation[0].data_size)
    y = annotation[0].y
    if not isfinite(y):
        y = None
    rc = cbk_fn(annotation[0].timestamp, annotation[0].annotation_type, annotation[0].group_id, y, data)
    return 1 if bool(rc) else 0


cdef int32_t _user_data_cbk_fn(void * user_data, uint16_t chunk_meta, c_jls.jls_storage_type_e storage_type,
        uint8_t * data, uint32_t data_size):
    cbk_fn = <object> user_data
    d = _storage_unpack(storage_type, data, data_size)
    rc = cbk_fn(chunk_meta, d)
    return 1 if bool(rc) else 0
