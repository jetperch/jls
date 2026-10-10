/*
 * Copyright 2021-2022 Jetperch LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "jls/threaded_writer.h"
#include "jls/msg_ring_buffer.h"
#include "jls/wr_prv.h"
#include "jls/backend.h"
#include "jls/cdef.h"
#include "jls/ec.h"
#include "jls/log.h"
#include "jls/time.h"
#include "jls/writer.h"
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#define MRB_BUFFER_SIZE (64 * 1024 * 1024)
#define SIGNAL_TYPE_UNDEFINED (0xff)


struct jls_twr_s {
    struct jls_bkt_s * bk;  // REQUIRED first entry
    struct jls_wr_s * wr;
    uint32_t flags;  // jls_twr_flags_e bits
    // The msg lock protects the following fields and mrb.
    int32_t error;               // first async error, latched; see threaded_writer.h
    int32_t quit_req;            // the thread exits once the ring is empty
    uint64_t flush_send_id;
    uint64_t flush_processed_id;
    uint32_t data_type[JLS_SIGNAL_COUNT];   // jls_datatype_e
    uint8_t signal_type[JLS_SIGNAL_COUNT];  // jls_signal_type_e or SIGNAL_TYPE_UNDEFINED
    struct jls_mrb_s mrb;
    uint8_t mrb_buffer[];
};

struct msg_header_user_data_s {
    uint16_t chunk_meta;
    uint8_t storage_type;
};

struct msg_header_fsr_s {
    uint16_t signal_id;
    int64_t sample_id;
    uint32_t sample_count;
};

struct msg_header_fsr_omit_s {
    uint16_t signal_id;
    int32_t enable;
};

struct msg_header_annotation_s {
    uint16_t signal_id;
    int64_t timestamp;
    uint8_t annotation_type;
    uint8_t storage_type;
    uint8_t group_id;
    float y;
};

struct msg_header_utc_s {
    uint16_t signal_id;
    int64_t sample_id;
    int64_t utc;
};

struct msg_header_s {
    uint8_t msg_type;
    union {
        struct msg_header_user_data_s user_data;
        struct msg_header_fsr_s fsr;
        struct msg_header_fsr_omit_s fsr_omit;
        struct msg_header_annotation_s annotation;
        struct msg_header_utc_s utc;
    } h;
    uint64_t d;
};

enum message_e {
    MSG_CLOSE,          // no header data, no args
    MSG_FLUSH,          // no header data, no args
    MSG_USER_DATA,      // hdr.user_data, user_data
    MSG_FSR,            // hdr.fsr_f32, data
    MSG_FSR_OMIT,       // hdr.fsr_omit, no args
    MSG_ANNOTATION,     // hdr.annotation, data
    MSG_UTC,            // hdr.utc, data
    MSG_ITEM_COUNT,
};

const char * message_str[] = {
        "close",
        "flush",
        "user_data",
        "fsr",
        "fsr_omit",
        "annotation",
        "utc",
};

/*
 * Lock, retrying on backend timeout: proceeding without the lock would
 * corrupt the ring and latch state.
 */
static void msg_lock(struct jls_twr_s * self) {
    while (jls_bkt_msg_lock(self->bk)) {
        JLS_LOGW("msg lock timed out, retry");
    }
}

static void process_lock(struct jls_twr_s * self) {
    while (jls_bkt_process_lock(self->bk)) {
        JLS_LOGW("process lock timed out, retry");
    }
}

static int32_t error_get(struct jls_twr_s * self) {
    msg_lock(self);
    int32_t rc = self->error;
    jls_bkt_msg_unlock(self->bk);
    return rc;
}

static void error_latch(struct jls_twr_s * self, int32_t rc) {
    if (rc) {
        msg_lock(self);
        if (!self->error) {
            self->error = rc;
        }
        jls_bkt_msg_unlock(self->bk);
    }
}

int32_t jls_twr_run(struct jls_twr_s * self) {
    uint32_t msg_size = 0;
    uint8_t * msg = NULL;
    struct msg_header_s hdr;
    uint8_t * payload;
    int32_t rc = 0;
    int32_t error;
    bool quit = false;
    struct jls_time_counter_s counter_start = jls_time_counter();
    struct jls_time_counter_s counter_end;
    struct jls_time_counter_s counter_prev = counter_start;
    uint64_t duration_ms;

    JLS_LOGI("run start");
    if (NULL == self->bk) {
        JLS_LOGE("backend null, quit");  // should never happen
        return 0;
    }
    while (!quit) {
        jls_bkt_msg_wait(self->bk);
        while (1) {
            msg_lock(self);
            if (NULL != msg) {
                jls_mrb_pop(&self->mrb, &msg_size);
            }
            msg = jls_mrb_peek(&self->mrb, &msg_size);
            error = self->error;
            if (!msg && self->quit_req) {
                quit = true;  // drained after the close request
            }
            jls_bkt_msg_unlock(self->bk);
            if (!msg) {
                break;
            }
            counter_start = jls_time_counter();
            if (((counter_start.value - counter_prev.value) / counter_start.frequency) >= 1) {
                JLS_LOGD2("twr %" PRIu32 " msgs (%" PRIu32 " of %" PRIu32 " bytes)",
                          self->mrb.count, jls_mrb_used_bytes(&self->mrb), self->mrb.buf_size);
                counter_prev = counter_start;
            }
            payload = msg + sizeof(hdr);
            uint32_t payload_sz = msg_size - sizeof(hdr);

            memcpy(&hdr, msg, sizeof(hdr));
            rc = 0;

            process_lock(self);
            switch (hdr.msg_type) {
                case MSG_CLOSE:
                    quit = true;
                    break;
                case MSG_FLUSH:
                    rc = jls_wr_flush(self->wr);  // returns the writer's latched error
                    msg_lock(self);
                    if (hdr.d > self->flush_processed_id) {
                        self->flush_processed_id = hdr.d;
                    }
                    jls_bkt_msg_unlock(self->bk);
                    break;
                case MSG_USER_DATA:
                    if (!error) {  // after a latched error, drop data messages
                        rc = jls_wr_user_data(self->wr, hdr.h.user_data.chunk_meta, hdr.h.user_data.storage_type,
                                              payload, payload_sz);
                    }
                    break;
                case MSG_FSR:
                    if (!error) {
                        rc = jls_wr_fsr(self->wr, hdr.h.fsr.signal_id, hdr.h.fsr.sample_id, payload,
                                        hdr.h.fsr.sample_count);
                    }
                    break;
                case MSG_FSR_OMIT:
                    if (!error) {
                        rc = jls_wr_fsr_omit_data(self->wr, hdr.h.fsr_omit.signal_id, hdr.h.fsr_omit.enable);
                    }
                    break;
                case MSG_ANNOTATION:
                    if (!error) {
                        rc = jls_wr_annotation(self->wr, hdr.h.annotation.signal_id, hdr.h.annotation.timestamp,
                                               hdr.h.annotation.y,
                                               hdr.h.annotation.annotation_type,
                                               hdr.h.annotation.group_id,
                                               hdr.h.annotation.storage_type,
                                               (const uint8_t *) payload, payload_sz);
                    }
                    break;
                case MSG_UTC:
                    if (!error) {
                        rc = jls_wr_utc(self->wr, hdr.h.utc.signal_id, hdr.h.utc.sample_id, hdr.h.utc.utc);
                    }
                    break;
                default:
                    break;
            }
            jls_bkt_process_unlock(self->bk);
            counter_end = jls_time_counter();
            duration_ms = (1000 * (counter_end.value - counter_start.value)) / counter_end.frequency;
            if (duration_ms > 250) {
                JLS_LOGW("thread msg %d:%s took %" PRIu64 " ms",
                         (int) hdr.msg_type,
                         (hdr.msg_type < MSG_ITEM_COUNT) ? message_str[hdr.msg_type] : "unknown",
                         duration_ms);
            }

            if (rc) {
                JLS_LOGW("thread msg %d:%s returned %d:%s",
                         (int) hdr.msg_type,
                         (hdr.msg_type < MSG_ITEM_COUNT) ? message_str[hdr.msg_type] : "unknown",
                         (int) rc, jls_error_code_name(rc));
                // Latch only what the writer latched: write failures, not validation errors.
                error_latch(self, jls_wr_error(self->wr));
            }
        }
    }
    JLS_LOGI("run done");
    return 0;
}

int32_t jls_twr_open(struct jls_twr_s ** instance, const char * path) {
    struct jls_wr_s * wr;
    struct jls_twr_s * self;
    if (!instance) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    ROE(jls_wr_open(&wr, path));

    self = malloc(sizeof(struct jls_twr_s) + MRB_BUFFER_SIZE);
    if (NULL == self) {
        JLS_LOGE("jls_twr_open malloc failed");
        jls_wr_close(wr);
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    self->bk = NULL;
    self->wr = wr;
    self->flags = 0;
    self->error = 0;
    self->quit_req = 0;
    self->flush_send_id = 0;
    self->flush_processed_id = 0;
    memset(self->data_type, 0, sizeof(self->data_type));
    memset(self->signal_type, SIGNAL_TYPE_UNDEFINED, sizeof(self->signal_type));
    self->signal_type[0] = JLS_SIGNAL_TYPE_VSR;  // jls_wr_open defines signal 0

    jls_mrb_init(&self->mrb, self->mrb_buffer, MRB_BUFFER_SIZE);
    // The backend sets self->bk before starting the thread: do not assign it
    // again here, which would race with the thread's reads.
    if (!jls_bkt_initialize(self)) {
        JLS_LOGE("jls_bkt_initialize failed");
        jls_wr_close(wr);
        free(self);
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }

    *instance = self;
    return 0;
}

uint32_t jls_twr_flags_get(struct jls_twr_s * self) {
    return self ? self->flags : 0;
}

int32_t jls_twr_flags_set(struct jls_twr_s * self, uint32_t flags) {
    if (!self) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    self->flags = flags;
    return 0;
}

static int32_t msg_send_inner(struct jls_twr_s * self, const struct msg_header_s * hdr,
                              const uint8_t * payload, uint64_t payload_size) {
    uint64_t sz = sizeof(*hdr) + payload_size;  // 64-bit: never wraps
    if (sz > jls_mrb_msg_size_max(&self->mrb)) {
        return JLS_ERROR_TOO_BIG;
    }
    msg_lock(self);
    uint8_t *msg = jls_mrb_alloc(&self->mrb, (uint32_t) sz);
    if (msg) {
        memcpy(msg, hdr, sizeof(*hdr));
        if (payload_size) {
            memcpy(msg + sizeof(*hdr), payload, (size_t) payload_size);
        }
        jls_bkt_msg_unlock(self->bk);
        jls_bkt_msg_signal(self->bk);
        return 0;
    } else {
        jls_bkt_msg_unlock(self->bk);
        return JLS_ERROR_BUSY;
    }
}

static int32_t msg_send(struct jls_twr_s * self, const struct msg_header_s * hdr,
                        const uint8_t * payload, uint64_t payload_size) {
    int64_t t_start = jls_now();
    int64_t t_stop = t_start + JLS_TIME_MILLISECOND * (int64_t) JLS_BK_MSG_WRITE_TIMEOUT_MS;
    while (jls_now() <= t_stop) {
        int32_t rc = msg_send_inner(self, hdr, payload, payload_size);
        if (JLS_ERROR_BUSY != rc) {
            return rc;
        }
        jls_bkt_sleep_ms(5);
    }
    return JLS_ERROR_BUSY;
}

int32_t jls_twr_flush(struct jls_twr_s * self) {
    uint64_t flush_id;
    uint64_t processed_id;
    struct msg_header_s hdr = { .msg_type = MSG_FLUSH };
    if (!self) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    msg_lock(self);
    int32_t rc = self->error;
    flush_id = self->flush_send_id + 1;
    if (!rc) {
        self->flush_send_id = flush_id;
    }
    jls_bkt_msg_unlock(self->bk);
    if (rc) {
        return rc;
    }
    hdr.d = flush_id;
    ROE(msg_send(self, &hdr, NULL, 0));  // BUSY: the flush was not queued

    int64_t t_start = jls_now();
    int64_t t_stop = t_start + JLS_TIME_MILLISECOND * (int64_t) JLS_BK_FLUSH_TIMEOUT_MS;
    while (1) {
        msg_lock(self);
        processed_id = self->flush_processed_id;
        jls_bkt_msg_unlock(self->bk);
        if (processed_id >= flush_id) {
            break;
        }
        jls_bkt_sleep_ms(10);
        if (jls_now() >= t_stop) {
            JLS_LOGE("flush timed out");
            return JLS_ERROR_TIMED_OUT;
        }
    }
    return error_get(self);  // includes the flush result
}

int32_t jls_twr_close(struct jls_twr_s * self) {
    if (!self) {
        return 0;
    }
    JLS_LOGI("jls_twr_close start");
    // A flag, not a message: a full ring cannot block the close request.
    msg_lock(self);
    self->quit_req = 1;
    jls_bkt_msg_unlock(self->bk);
    jls_bkt_msg_signal(self->bk);
    jls_bkt_finalize(self->bk);
    self->bk = NULL;
    JLS_LOGI("jls_bkt_finalize done");
    int32_t rc = self->error;  // thread joined: no lock needed
    int32_t rc_close = jls_wr_close(self->wr);
    self->wr = NULL;
    free(self);
    JLS_LOGI("jls_wr_close done");
    return rc ? rc : rc_close;
}

int32_t jls_twr_source_def(struct jls_twr_s * self, const struct jls_source_def_s * source) {
    if (!self) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    ROE(error_get(self));
    process_lock(self);
    int32_t rv = jls_wr_source_def(self->wr, source);
    jls_bkt_process_unlock(self->bk);
    return rv;
}

int32_t jls_twr_signal_def(struct jls_twr_s * self, const struct jls_signal_def_s * signal) {
    if (!self) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    ROE(error_get(self));
    process_lock(self);
    int32_t rv = jls_wr_signal_def(self->wr, signal);
    if (0 == rv) {  // jls_wr_signal_def validated signal and signal_id
        msg_lock(self);
        self->data_type[signal->signal_id] = signal->data_type;
        self->signal_type[signal->signal_id] = signal->signal_type;
        jls_bkt_msg_unlock(self->bk);
    }
    jls_bkt_process_unlock(self->bk);
    return rv;
}

/*
 * Validate synchronously: the writer thread latches its errors.
 * Matches jls_core_signal_validate_typed() return codes.
 */
static int32_t signal_validate(struct jls_twr_s * self, uint16_t signal_id, bool fsr_only, uint32_t * data_type) {
    if (!self || (signal_id >= JLS_SIGNAL_COUNT)) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    msg_lock(self);
    int32_t rc = self->error;
    uint8_t signal_type = self->signal_type[signal_id];
    if (data_type) {
        *data_type = self->data_type[signal_id];
    }
    jls_bkt_msg_unlock(self->bk);
    if (rc) {
        return rc;
    }
    if (SIGNAL_TYPE_UNDEFINED == signal_type) {
        return JLS_ERROR_NOT_FOUND;
    }
    if (fsr_only && (JLS_SIGNAL_TYPE_FSR != signal_type)) {
        return JLS_ERROR_NOT_SUPPORTED;
    }
    return 0;
}

/*
 * The message copies data_size bytes, and the writer thread runs strlen()
 * on STRING and JSON payloads, so they must be NUL-terminated within it.
 * A STRING or JSON data_size of 0 means strlen(data) + 1, as for jls_wr_*.
 */
static int32_t payload_size(enum jls_storage_type_e storage_type, const uint8_t * data, uint32_t * data_size) {
    switch (storage_type) {
        case JLS_STORAGE_TYPE_STRING:  // intentional fall-through
        case JLS_STORAGE_TYPE_JSON:
            if (0 == *data_size) {
                size_t sz = strlen((const char *) data) + 1;
                if (sz > UINT32_MAX) {
                    return JLS_ERROR_TOO_BIG;
                }
                *data_size = (uint32_t) sz;
            } else if (NULL == memchr(data, 0, *data_size)) {
                return JLS_ERROR_PARAMETER_INVALID;
            }
            break;
        default:
            break;
    }
    return 0;
}

int32_t jls_twr_user_data(struct jls_twr_s * self, uint16_t chunk_meta,
                          enum jls_storage_type_e storage_type, const uint8_t * data, uint32_t data_size) {
    struct msg_header_s hdr = {
            .msg_type = MSG_USER_DATA,
            .h = {
                    .user_data = {
                            .chunk_meta = chunk_meta,
                            .storage_type = (uint8_t) storage_type,
                    }
            },
            .d = 0
    };
    if (!self) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    ROE(error_get(self));
    ROE(jls_wr_user_data_validate(chunk_meta, storage_type, data, data_size));  // also bounds storage_type
    ROE(payload_size(storage_type, data, &data_size));
    return msg_send(self, &hdr, data, data_size);
}

int32_t jls_twr_fsr(struct jls_twr_s * self, uint16_t signal_id,
                    int64_t sample_id, const void * data, uint32_t data_length) {
    struct msg_header_s hdr = {
            .msg_type = MSG_FSR,
            .h = {
                    .fsr = {
                            .signal_id = signal_id,
                            .sample_id = sample_id,
                            .sample_count = data_length,
                    }
            },
            .d = 0
    };
    uint32_t data_type = 0;
    ROE(signal_validate(self, signal_id, true, &data_type));
    if (data_length && !data) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    uint8_t entry_size_bits = jls_datatype_parse_size(data_type);
    uint64_t length = ((uint64_t) data_length * entry_size_bits + 7) / 8;
    int32_t rc;
    if (self->flags & JLS_TWR_FLAG_DROP_ON_OVERFLOW) {
        rc = msg_send_inner(self, &hdr, (const uint8_t *) data, length);
    } else {
        rc = msg_send(self, &hdr, (const uint8_t *) data, length);
    }
    if (rc) {
        JLS_LOGW("signal %" PRIu16 " drop %" PRIu32 " samples @ %" PRIi64,
                 signal_id, data_length, sample_id);
    }
    return rc;
}

int32_t jls_twr_fsr_f32(struct jls_twr_s * self, uint16_t signal_id,
                        int64_t sample_id, const float * data, uint32_t data_length) {
    uint32_t data_type = 0;
    ROE(signal_validate(self, signal_id, true, &data_type));
    if (JLS_DATATYPE_F32 != data_type) {  // data is float[data_length]
        return JLS_ERROR_PARAMETER_INVALID;
    }
    return jls_twr_fsr(self, signal_id, sample_id, data, data_length);
}

int32_t jls_twr_fsr_omit_data(struct jls_twr_s * self, uint16_t signal_id, uint32_t enable) {
    struct msg_header_s hdr = {
            .msg_type = MSG_FSR_OMIT,
            .h = {
                    .fsr_omit = {
                            .signal_id = signal_id,
                            .enable = enable,
                    }
            },
            .d = 0
    };
    ROE(signal_validate(self, signal_id, true, NULL));
    return msg_send(self, &hdr, NULL, 0);
}

int32_t jls_twr_annotation(struct jls_twr_s * self, uint16_t signal_id, int64_t timestamp,
                           float y,
                           enum jls_annotation_type_e annotation_type,
                           uint8_t group_id,
                           enum jls_storage_type_e storage_type,
                           const uint8_t * data, uint32_t data_size) {
    struct msg_header_s hdr = {
            .msg_type = MSG_ANNOTATION,
            .h = {
                    .annotation = {
                            .signal_id = signal_id,
                            .timestamp = timestamp,
                            .annotation_type = (uint8_t) annotation_type,
                            .storage_type = (uint8_t) storage_type,
                            .group_id = group_id,
                            .y = y
                    }
            },
            .d = 0
    };
    ROE(signal_validate(self, signal_id, false, NULL));
    ROE(jls_wr_annotation_validate(annotation_type, storage_type, data, data_size));  // also bounds the u8 fields
    ROE(payload_size(storage_type, data, &data_size));
    return msg_send(self, &hdr, data, data_size);
}

JLS_API int32_t jls_twr_utc(struct jls_twr_s * self, uint16_t signal_id, int64_t sample_id, int64_t utc) {
    struct msg_header_s hdr = {
            .msg_type = MSG_UTC,
            .h = {
                    .utc = {
                            .signal_id = signal_id,
                            .sample_id = sample_id,
                            .utc = utc
                    }
            },
            .d = 0
    };
    ROE(signal_validate(self, signal_id, true, NULL));
    return msg_send(self, &hdr, NULL, 0);
}
