# Copyright 2021-2022 Jetperch LLC
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
           'SourceDef', 'SignalDef', 'SummaryFSR', 'jls_inject_log',
           'data_type_as_enum', 'data_type_as_str']


_log_c_name = 'pyjls.c'
_log_c = logging.getLogger(_log_c_name)
# _UTC_OFFSET = dateutil.parser.parse('2018-01-01T00:00:00Z').timestamp()
_UTC_OFFSET = 1514764800  # seconds
DEF _JLS_SIGNAL_COUNT = 256  # From jls/format.h


def _data_type_def(basetype, size, q):
    return (basetype & 0x0f) | ((size & 0xff) << 8) | ((q & 0xff) << 16)


class _DataTypeBase:
    INT       = 0x01
    UNSIGNED  = 0x02
    UINT      = 0x03
    FLOAT     = 0x04


class DataType:
    U1 = _data_type_def(_DataTypeBase.UINT, 1, 0)
    U4 = _data_type_def(_DataTypeBase.UINT, 4, 0)
    U8 = _data_type_def(_DataTypeBase.UINT, 8, 0)
    U16 = _data_type_def(_DataTypeBase.UINT, 16, 0)
    U32 = _data_type_def(_DataTypeBase.UINT, 32, 0)
    U64 = _data_type_def(_DataTypeBase.UINT, 64, 0)

    I4 = _data_type_def(_DataTypeBase.INT, 4, 0)
    I8 = _data_type_def(_DataTypeBase.INT, 8, 0)
    I16 = _data_type_def(_DataTypeBase.INT, 16, 0)
    I32 = _data_type_def(_DataTypeBase.INT, 32, 0)
    I64 = _data_type_def(_DataTypeBase.INT, 64, 0)

    F32 = _data_type_def(_DataTypeBase.FLOAT, 32, 0)
    F64 = _data_type_def(_DataTypeBase.FLOAT, 64, 0)


_data_type_map = {
    DataType.U1: np.uint8,      # packed
    DataType.U4: np.uint8,      # packed
    DataType.U8: np.uint8,
    DataType.U16: np.uint16,
    DataType.U32: np.uint32,
    DataType.U64: np.uint64,

    DataType.I4: np.uint8,      # packed
    DataType.I8: np.int8,
    DataType.I16: np.int16,
    DataType.I32: np.int32,
    DataType.I64: np.int64,

    DataType.F32: np.float32,
    DataType.F64: np.float64,
}


_data_type_as_enum = {
    'u1': DataType.U1,
    'u4': DataType.U4,
    'u8': DataType.U8,
    'u16': DataType.U16,
    'u32': DataType.U32,
    'u64': DataType.U64,

    'i4': DataType.I4,
    'i8': DataType.I8,
    'i16': DataType.I16,
    'i32': DataType.I32,
    'i64': DataType.I64,

    'f32': DataType.F32,
    'f64': DataType.F64,
}


_data_type_as_str = {}


def _populate_data_type():
    for key, value in list(_data_type_as_enum.items()):
        _data_type_as_enum[value] = value
        _data_type_as_str[value] = key
        _data_type_as_str[key] = key


_populate_data_type()


def data_type_as_enum(data_type):
    return _data_type_as_enum[data_type]


def data_type_as_str(data_type):
    return _data_type_as_str[data_type]


class AnnotationType:
    USER = c_jls.JLS_ANNOTATION_TYPE_USER
    TEXT = c_jls.JLS_ANNOTATION_TYPE_TEXT
    VMARKER = c_jls.JLS_ANNOTATION_TYPE_VERTICAL_MARKER
    HMARKER = c_jls.JLS_ANNOTATION_TYPE_HORIZONTAL_MARKER


_annotation_map = {
    'user': AnnotationType.USER,
    'usr': AnnotationType.USER,
    'text': AnnotationType.TEXT,
    'txt': AnnotationType.TEXT,
    'str': AnnotationType.TEXT,
    'string': AnnotationType.TEXT,
    'marker': AnnotationType.VMARKER,
    'vertical_marker': AnnotationType.VMARKER,
    'vmarker': AnnotationType.VMARKER,
    'horizontal_marker': AnnotationType.HMARKER,
    'hmarker': AnnotationType.HMARKER,
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


cdef void _log_cbk(const char * msg) nogil:
    with gil:
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
        s = ''
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


def utc_to_jls(utc):
    """Convert from python UTC timestamp to jls timestamp."""
    return int((utc - _UTC_OFFSET) * (2**30))


def jls_to_utc(timestamp):
    """Convert from jls timestamp to python UTC timestamp."""
    return (timestamp / (2**30)) + _UTC_OFFSET


def _handle_rc(name, rc):
    if rc == 0:
        return
    rc_name = c_jls.jls_error_code_name(rc).decode('utf-8')
    rc_descr = c_jls.jls_error_code_description(rc).decode('utf-8')
    raise RuntimeError(f'{name} {rc_name}[{rc}]: {rc_descr}')


cdef class Writer:
    cdef c_jls.jls_twr_s * _wr
    cdef c_jls.jls_signal_def_s _signals[_JLS_SIGNAL_COUNT]

    def __init__(self, path: str):
        cdef int32_t rc
        self._signals[0].signal_type = c_jls.JLS_SIGNAL_TYPE_VSR
        rc = c_jls.jls_twr_open(&self._wr, path.encode('utf-8'))
        _handle_rc('open', rc)

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
        _handle_rc('source_def', rc)

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
        cdef c_jls.jls_signal_def_s * s
        s = &self._signals[signal_id]
        if data_type is None:
            data_type = DataType.F32
        elif isinstance(data_type, str):
            data_type = _data_type_as_enum[data_type]
        s.signal_id = signal_id
        s.source_id = source_id
        s.signal_type = 0 if signal_type is None else int(signal_type)
        s.data_type = data_type
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
        rc = c_jls.jls_twr_signal_def(self._wr, s)
        _handle_rc('signal_def', rc)

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
        _handle_rc('user_data', rc)

    def fsr_f32(self, signal_id, sample_id, data):
        return self.fsr(signal_id, sample_id, data)

    def fsr(self, signal_id, sample_id, data):
        cdef int32_t rc
        cdef np.uint8_t [::1] u8
        cdef uint32_t data_type
        cdef uint32_t entry_size_bits
        cdef uint32_t length

        data_type = self._signals[signal_id].data_type
        entry_size_bits = (data_type >> 8) & 0xff
        np_type = _data_type_map[data_type & 0xffff]
        if np_type != data.dtype:
            raise ValueError(f'Data type mismatch {data.dtype} != {np_type}')
        data_u8 = data.view(dtype=np.uint8)
        u8 = data_u8
        length = len(data)
        if entry_size_bits == 4:
            length *= 2
        elif entry_size_bits == 1:
            length *= 8
        rc = c_jls.jls_twr_fsr(self._wr, signal_id, sample_id, &u8[0], length)
        _handle_rc('fsr', rc)

    def annotation(self, signal_id, timestamp, y, annotation_type, group_id, data):
        cdef int32_t rc
        if isinstance(annotation_type, str):
            annotation_type = _annotation_map[annotation_type.lower()]
        storage_type, payload, payload_length = _storage_pack(data)
        if y is None or not np.isfinite(y):
            y = NAN
        rc = c_jls.jls_twr_annotation(self._wr, signal_id, timestamp, y, annotation_type,
            group_id, storage_type, payload, payload_length)
        _handle_rc('annotation', rc)

    def utc(self, signal_id, sample_id, utc_i64):
        cdef int32_t rc
        rc = c_jls.jls_twr_utc(self._wr, signal_id, sample_id, utc_i64)
        _handle_rc('utc', rc)


cdef class AnnotationCallback:
    cdef uint8_t is_fsr
    cdef object cbk_fn

    def __init__(self, is_fsr, cbk_fn):
        self.is_fsr = is_fsr
        self.cbk_fn = cbk_fn


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
        _handle_rc('open', rc)

        rc = c_jls.jls_rd_sources(self._rd, &sources, &count)
        _handle_rc('rd_sources', rc)
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
        _handle_rc('rd_signals', rc)
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
            if signal_def.signal_type == c_jls.JLS_SIGNAL_TYPE_FSR:
                rc = c_jls.jls_rd_fsr_length(self._rd, signal_id, &samples)
                _handle_rc('rd_fsr_length', rc)
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
        cdef np.uint8_t [::1] u8
        cdef uint32_t data_type
        cdef uint32_t entry_size_bits
        cdef uint32_t u8_length
        cdef uint16_t signal_id_u16 = signal_id
        cdef int64_t start_sample_id_i64 = start_sample_id
        cdef int64_t length_i64 = length

        data_type = self._signals[signal_id].data_type
        entry_size_bits = (data_type >> 8) & 0xff
        np_type = _data_type_map[data_type & 0xffff]
        u8_length = length
        if entry_size_bits == 4:
            u8_length = (length + 1) // 2
        elif entry_size_bits == 1:
            u8_length = (length + 7) // 8
        else:
            u8_length *= entry_size_bits // 8

        data_u8 = np.empty(u8_length, dtype=np.uint8)
        data = data_u8.view(dtype=np_type)
        u8 = data_u8
        with nogil:
            rc = c_jls.jls_rd_fsr(self._rd, signal_id_u16, start_sample_id_i64, &u8[0], length_i64)
        _handle_rc('rd_fsr', rc)
        return data

    def fsr_statistics(self, signal_id, start_sample_id, increment, length):
        """Read FSR statistics.

        :param signal_id: The signal id.
        :param start_sample_id: The starting sample id to read.
        :param increment: The number of samples represented per return value.
        :param length: The number of return values to generate.
        :return The 2-D array[summary][stat] of np.float32 where the stat column is defined by SummaryFSR.
        """

        cdef int32_t rc
        cdef np.float64_t [:, :] c_data
        cdef uint16_t signal_id_u16 = signal_id
        cdef int64_t start_sample_id_i64 = start_sample_id
        cdef int64_t increment_i64 = increment
        cdef int64_t length_i64 = length

        data = np.empty((length, c_jls.JLS_SUMMARY_FSR_COUNT), dtype=np.float64)
        c_data = data
        with nogil:
            rc = c_jls.jls_rd_fsr_statistics(self._rd, signal_id_u16, start_sample_id_i64,
                                             increment_i64, &c_data[0, 0], length_i64)
        _handle_rc('rd_fsr_statistics', rc)
        return data

    def annotations(self, signal_id, timestamp, cbk_fn):
        """Read annotations from a signal.

        :param signal_id: The signal id.
        :param timestamp: The starting timestamp.  FSR uses sample_id.  VSR uses utc.
        :param cbk: The function(timestamp, y, annotation_type, group_id, data)
            to call for each annotation.  Return True to stop iteration over
            the annotations or False to continue iterating.
        """
        cdef int32_t rc
        is_fsr = self._signals[signal_id].signal_type == c_jls.JLS_SIGNAL_TYPE_FSR
        user_data = AnnotationCallback(is_fsr, cbk_fn)
        rc = c_jls.jls_rd_annotations(self._rd, signal_id, timestamp, _annotation_cbk_fn, <void *> user_data)
        _handle_rc('rd_annotations', rc)

    def user_data(self, cbk_fn):
        cdef int32_t rc
        rc = c_jls.jls_rd_user_data(self._rd, _user_data_cbk_fn, <void *> cbk_fn)
        _handle_rc('rd_user_data', rc)

    def utc(self, signal_id, sample_id, cbk_fn):
        """Read the sample_id / utc pairs from a FSR signal.

        :param signal_id: The signal id.
        :param sample_id: The starting sample_id.
        :param cbk: The function(entries)
            to call for each annotation.  Entries is an Nx2 numpy array of
            [sample_id, utc_timestamp].
            Return True to stop iteration over the annotations
            or False to continue iterating.
        """
        cdef int32_t rc
        rc = c_jls.jls_rd_utc(self._rd, signal_id, sample_id, _utc_cbk_fn, <void *> cbk_fn)
        _handle_rc('rd_utc', rc)


cdef int32_t _annotation_cbk_fn(void * user_data, const c_jls.jls_annotation_s * annotation):
    obj: AnnotationCallback = <object> user_data
    data = _storage_unpack(annotation[0].storage_type, annotation[0].data, annotation[0].data_size)
    y = annotation[0].y
    timestamp = annotation[0].timestamp
    if not obj.is_fsr:
        timestamp = annotation[0].timestamp
    if not isfinite(y):
        y = None
    try:
        rc = obj.cbk_fn(timestamp, y, annotation[0].annotation_type, annotation[0].group_id, data)
    except Exception:
        logging.getLogger(__name__).exception('in annotation callback')
        return 1
    return 1 if bool(rc) else 0


cdef int32_t _user_data_cbk_fn(void * user_data, uint16_t chunk_meta, c_jls.jls_storage_type_e storage_type,
        uint8_t * data, uint32_t data_size):
    cbk_fn = <object> user_data
    d = _storage_unpack(storage_type, data, data_size)
    rc = cbk_fn(chunk_meta, d)
    return 1 if bool(rc) else 0


cdef int32_t _utc_cbk_fn(void * user_data, const c_jls.jls_utc_summary_entry_s * utc, uint32_t size):
    cdef uint32_t idx
    cbk_fn = <object> user_data
    entries = np.empty((size, 2), dtype=np.int64)
    for idx in range(size):
        entries[idx, 0] = utc[idx].sample_id
        entries[idx, 1] = utc[idx].timestamp
    rc = cbk_fn(entries)
    return 1 if bool(rc) else 0
