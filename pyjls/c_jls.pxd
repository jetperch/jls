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

from libc.stdint cimport uint8_t, uint16_t, uint32_t, uint64_t, int32_t, int64_t


cdef extern from "jls/format.h":

    enum jls_signal_type_e:
        JLS_SIGNAL_TYPE_FSR = 0
        JLS_SIGNAL_TYPE_VSR = 1

    enum jls_storage_type_e:
        JLS_STORAGE_TYPE_INVALID = 0
        JLS_STORAGE_TYPE_BINARY = 1
        JLS_STORAGE_TYPE_STRING = 2
        JLS_STORAGE_TYPE_JSON = 3

    enum jls_annotation_type_e:
        JLS_ANNOTATION_TYPE_USER = 0
        JLS_ANNOTATION_TYPE_TEXT = 1
        JLS_ANNOTATION_TYPE_VERTICAL_MARKER = 2
        JLS_ANNOTATION_TYPE_HORIZONTAL_MARKER = 3

    struct jls_source_def_s:
        uint16_t source_id
        const char * name
        const char * vendor
        const char * model
        const char * version
        const char * serial_number

    struct jls_signal_def_s:
        uint16_t signal_id
        uint16_t source_id
        uint8_t signal_type
        uint16_t rsv16_0
        uint32_t data_type
        uint32_t sample_rate
        uint32_t samples_per_data
        uint32_t sample_decimate_factor
        uint32_t entries_per_summary
        uint32_t summary_decimate_factor
        uint32_t annotation_decimate_factor
        uint32_t utc_decimate_factor
        const char * name
        const char * units

    struct jls_annotation_s:
        int64_t timestamp
        uint64_t rsv64_1
        uint8_t annotation_type
        uint8_t storage_type
        uint8_t group_id
        uint8_t rsv8_1
        float y
        uint32_t data_size
        uint8_t data[0]

    enum jls_summary_fsr_e:
        JLS_SUMMARY_FSR_MEAN = 0
        JLS_SUMMARY_FSR_STD = 1
        JLS_SUMMARY_FSR_MIN = 2
        JLS_SUMMARY_FSR_MAX = 3
        JLS_SUMMARY_FSR_COUNT = 4

    struct jls_utc_summary_entry_s:
        int64_t sample_id
        int64_t timestamp


cdef extern from "jls/log.h":
    void jls_log_printf(const char * format, ...)
    ctypedef void (*jls_log_cbk)(const char * msg)
    void jls_log_register(jls_log_cbk handler)
    void jls_log_unregister()


cdef extern from "jls/time.h":
    int64_t jls_now()


cdef extern from "jls/threaded_writer.h":
    struct jls_twr_s
    int32_t jls_twr_open(jls_twr_s ** instance, const char * path)
    int32_t jls_twr_close(jls_twr_s * self)
    int32_t jls_twr_flush(jls_twr_s * self)
    int32_t jls_twr_source_def(jls_twr_s * self, const jls_source_def_s * source)
    int32_t jls_twr_signal_def(jls_twr_s * self, const jls_signal_def_s * signal)
    int32_t jls_twr_user_data(jls_twr_s * self, uint16_t chunk_meta,
            jls_storage_type_e storage_type, const uint8_t * data, uint32_t data_size)
    int32_t jls_twr_fsr(jls_twr_s * self, uint16_t signal_id,
            int64_t sample_id, const void * data, uint32_t data_length)
    int32_t jls_twr_annotation(jls_twr_s * self, uint16_t signal_id,
            int64_t timestamp,
            float y,
            jls_annotation_type_e annotation_type,
            uint8_t group_id,
            jls_storage_type_e storage_type, 
            const uint8_t * data, uint32_t data_size)
    int32_t jls_twr_utc(jls_twr_s * self, uint16_t signal_id, 
                        int64_t sample_id, int64_t utc)


cdef extern from "jls/reader.h":
    struct jls_rd_s
    int32_t jls_rd_open(jls_rd_s ** instance, const char * path)
    void jls_rd_close(jls_rd_s * self)
    int32_t jls_rd_sources(jls_rd_s * self, jls_source_def_s ** sources, uint16_t * count)
    int32_t jls_rd_signals(jls_rd_s * self, jls_signal_def_s ** signals, uint16_t * count)
    int32_t jls_rd_signal(jls_rd_s * self, uint16_t signal_id, jls_signal_def_s * signal)
    int32_t jls_rd_fsr_length(jls_rd_s * self, uint16_t signal_id, int64_t * samples)
    int32_t jls_rd_fsr(jls_rd_s * self, uint16_t signal_id, int64_t start_sample_id, void * data, int64_t data_length)
    int32_t jls_rd_fsr_f32_statistics(jls_rd_s * self, uint16_t signal_id,
        int64_t start_sample_id, int64_t increment, float * data, int64_t data_length)
    ctypedef int32_t (*jls_rd_annotation_cbk_fn)(void * user_data, const jls_annotation_s * annotation)
    int32_t jls_rd_annotations(jls_rd_s * self, uint16_t signal_id,
        int64_t timestamp, jls_rd_annotation_cbk_fn cbk_fn, void * cbk_user_data)
    ctypedef int32_t (*jls_rd_user_data_cbk_fn)(void * user_data,
        uint16_t chunk_meta, jls_storage_type_e storage_type,
        uint8_t * data, uint32_t data_size)
    int32_t jls_rd_user_data(jls_rd_s * self, jls_rd_user_data_cbk_fn cbk_fn, void * cbk_user_data)
    ctypedef int32_t (*jls_rd_utc_cbk_fn)(void * user_data, const jls_utc_summary_entry_s * utc, uint32_t size)
    int32_t jls_rd_utc(jls_rd_s * self, uint16_t signal_id, int64_t sample_id,
                       jls_rd_utc_cbk_fn cbk_fn, void * cbk_user_data)