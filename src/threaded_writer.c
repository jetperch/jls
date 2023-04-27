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


#define MRB_BUFFER_SIZE (100000000)


struct jls_twr_s {
    struct jls_bkt_s * bk;  // REQUIRED first entry
    struct jls_wr_s * wr;
    volatile int quit;
    volatile uint64_t flush_send_id;
    volatile uint64_t flush_processed_id;
    uint8_t fsr_entry_size_bits[JLS_SIGNAL_COUNT];
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
    MSG_ANNOTATION,     // hdr.annotation, data
    MSG_UTC,            // hdr.utc, data
    MSG_ITEM_COUNT,
};

const char * message_str[] = {
        "close",
        "flush",
        "user_data",
        "fsr",
        "annotation",
        "utc",
};

int32_t jls_twr_run(struct jls_twr_s * self) {
    uint32_t msg_size;
    uint8_t * msg;
    struct msg_header_s hdr;
    uint8_t * payload;
    int32_t rc = 0;

    if (!self->bk) {
        JLS_LOGE("backend null on entry");
    }

    while (!self->quit) {
        if (!self->bk) {
            JLS_LOGE("backend null");
            jls_bkt_sleep_ms(500);
            continue;
        }
        jls_bkt_msg_wait(self->bk);
        if (jls_bkt_msg_lock(self->bk)) {
            continue;
        }
        while (!self->quit) {
            msg = jls_mrb_peek(&self->mrb, &msg_size);
            jls_bkt_msg_unlock(self->bk);
            if (!msg) {
                break;
            }
            payload = msg + sizeof(hdr);
            uint32_t payload_sz = msg_size - sizeof(hdr);

            memcpy(&hdr, msg, sizeof(hdr));
            rc = 0;

            jls_bkt_process_lock(self->bk);
            switch (hdr.msg_type) {
                case MSG_CLOSE:
                    self->quit = 1;
                    break;
                case MSG_FLUSH:
                    jls_wr_flush(self->wr);
                    self->flush_processed_id = hdr.d > self->flush_processed_id ? hdr.d : self->flush_processed_id;
                    break;
                case MSG_USER_DATA:
                    rc = jls_wr_user_data(self->wr, hdr.h.user_data.chunk_meta, hdr.h.user_data.storage_type,
                                          payload, payload_sz);
                    break;
                case MSG_FSR:
                    rc = jls_wr_fsr(self->wr, hdr.h.fsr.signal_id, hdr.h.fsr.sample_id, payload, hdr.h.fsr.sample_count);
                    break;
                case MSG_ANNOTATION:
                    rc = jls_wr_annotation(self->wr, hdr.h.annotation.signal_id, hdr.h.annotation.timestamp,
                                           hdr.h.annotation.y,
                                           hdr.h.annotation.annotation_type,
                                           hdr.h.annotation.group_id,
                                           hdr.h.annotation.storage_type,
                                           (const uint8_t *) payload, payload_sz);
                    break;
                case MSG_UTC:
                    rc = jls_wr_utc(self->wr, hdr.h.utc.signal_id, hdr.h.utc.sample_id, hdr.h.utc.utc);
                default:
                    break;
            }
            jls_bkt_process_unlock(self->bk);

            if (rc) {
                JLS_LOGW("thread msg %d:%s returned %d:%s",
                         (int) hdr.msg_type,
                         (hdr.msg_type < MSG_ITEM_COUNT) ? message_str[hdr.msg_type] : "unknown",
                         (int) rc, jls_error_code_name(rc));
            }

            while (!self->quit && jls_bkt_msg_lock(self->bk)) {
                // retry
            }
            jls_mrb_pop(&self->mrb, &msg_size);
            // stay locked
        }
    }
    return 0;
}

int32_t jls_twr_open(struct jls_twr_s ** instance, const char * path) {
    struct jls_wr_s * wr;
    struct jls_twr_s * self;
    ROE(jls_wr_open(&wr, path));

    self = malloc(sizeof(struct jls_twr_s) + MRB_BUFFER_SIZE);
    if (!self) {
        JLS_LOGE("jls_twr_open malloc failed");
        jls_wr_close(wr);
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    self->quit = 0;
    self->wr = wr;
    self->flush_send_id = 0;
    self->flush_processed_id = 0;

    jls_mrb_init(&self->mrb, self->mrb_buffer, MRB_BUFFER_SIZE);
    self->bk = jls_bkt_initialize(self);
    if (!self->bk) {
        JLS_LOGE("jls_bkt_initialize failed");
        jls_wr_close(wr);
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }

    *instance = self;
    return 0;
}

int32_t msg_send(struct jls_twr_s * self, const struct msg_header_s * hdr, const uint8_t * payload, uint32_t payload_size) {
    uint32_t sz = sizeof(*hdr) + payload_size;
    for (uint32_t count = 0; count < (JLS_BK_MSG_WRITE_TIMEOUT_MS / 5); ++count) {
        jls_bkt_msg_lock(self->bk);
        uint8_t *msg = jls_mrb_alloc(&self->mrb, sz);
        if (msg) {
            memcpy(msg, hdr, sizeof(*hdr));
            if (payload_size) {
                memcpy(msg + sizeof(*hdr), payload, payload_size);
            }
            jls_bkt_msg_unlock(self->bk);
            jls_bkt_msg_signal(self->bk);
            return 0;
        }
        jls_bkt_msg_unlock(self->bk);
        jls_bkt_sleep_ms(5);
    }
    return JLS_ERROR_BUSY;
}

int32_t jls_twr_flush(struct jls_twr_s * self) {
    uint64_t flush_id;
    struct msg_header_s hdr = { .msg_type = MSG_FLUSH };
    jls_bkt_msg_lock(self->bk);
    flush_id = self->flush_send_id + 1;
    self->flush_send_id = flush_id;
    jls_bkt_msg_unlock(self->bk);
    hdr.d = flush_id;
    msg_send(self, &hdr, NULL, 0);

    int64_t t_start = jls_now();
    int64_t t_stop = t_start + JLS_TIME_MILLISECOND * (int64_t) JLS_BK_FLUSH_TIMEOUT_MS;
    while (self->flush_processed_id < flush_id) {
        jls_bkt_sleep_ms(10);
        if (jls_now() >= t_stop) {
            JLS_LOGE("flush timed out");
            return JLS_ERROR_TIMED_OUT;
        }
    }
    return 0;
}

int32_t jls_twr_close(struct jls_twr_s * self) {
    int rc = 0;
    if (self) {
        struct msg_header_s hdr = { .msg_type = MSG_CLOSE };
        jls_twr_flush(self);
        msg_send(self, &hdr, NULL, 0);
        jls_bkt_finalize(self->bk);
        rc = jls_wr_close(self->wr);
        self->wr = NULL;
        free(self);
    }
    return rc;
}

int32_t jls_twr_source_def(struct jls_twr_s * self, const struct jls_source_def_s * source) {
    jls_bkt_process_lock(self->bk);
    int32_t rv = jls_wr_source_def(self->wr, source);
    jls_bkt_process_unlock(self->bk);
    return rv;
}

int32_t jls_twr_signal_def(struct jls_twr_s * self, const struct jls_signal_def_s * signal) {
    jls_bkt_process_lock(self->bk);
    self->fsr_entry_size_bits[signal->signal_id] = jls_datatype_parse_size(signal->data_type);
    int32_t rv = jls_wr_signal_def(self->wr, signal);
    jls_bkt_process_unlock(self->bk);
    return rv;
}

int32_t jls_twr_user_data(struct jls_twr_s * self, uint16_t chunk_meta,
                          enum jls_storage_type_e storage_type, const uint8_t * data, uint32_t data_size) {
    struct msg_header_s hdr = {
            .msg_type = MSG_USER_DATA,
            .h = {
                    .user_data = {
                            .chunk_meta = chunk_meta,
                            .storage_type = storage_type,
                    }
            },
            .d = 0
    };
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
    uint32_t length = (data_length * self->fsr_entry_size_bits[signal_id] + 7) / 8;
    return msg_send(self, &hdr, (const uint8_t *) data, length);
}

int32_t jls_twr_fsr_f32(struct jls_twr_s * self, uint16_t signal_id,
                        int64_t sample_id, const float * data, uint32_t data_length) {
    return jls_twr_fsr(self, signal_id, sample_id, data, data_length);
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
                            .annotation_type = annotation_type,
                            .storage_type = storage_type,
                            .group_id = group_id,
                            .y = y
                    }
            },
            .d = 0
    };
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
    return msg_send(self, &hdr, NULL, 0);
}
