/*
 * Copyright 2021 Jetperch LLC
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
#include "jls/writer.h"
#include <stdlib.h>
#include <string.h>


#define MRB_BUFFER_SIZE (100000000)


struct jls_twr_s {
    struct jls_bkt_s * bk;
    struct jls_wr_s * wr;
    volatile int quit;
    struct jls_mrb_s mrb;
    uint8_t mrb_buffer[];
};

struct msg_header_user_data_s {
    uint16_t chunk_meta;
    uint8_t storage_type;
};

struct msg_header_fsr_f32_s {
    uint16_t signal_id;
    int64_t sample_id;
};

struct msg_header_annotation_s {
    uint16_t signal_id;
    int64_t timestamp;
    uint8_t annotation_type;
    uint8_t storage_type;
    uint8_t group_id;
};

struct msg_header_s {
    uint8_t msg_type;
    union {
        struct msg_header_user_data_s user_data;
        struct msg_header_fsr_f32_s fsr_f32;
        struct msg_header_annotation_s annotation;
    } h;
    uint64_t d;
};

enum message_e {
    MSG_CLOSE,          // no header data, no args
    MSG_USER_DATA,      // hdr.user_data, user_data
    MSG_FSR_F32,        // hdr.fsr_f32, data
    MSG_ANNOTATION,     // hdr.annotation, data
};

int32_t jls_twr_run(struct jls_twr_s * self) {
    uint32_t msg_size;
    uint8_t * msg;
    struct msg_header_s hdr;
    uint8_t * payload;
    int32_t rc = 0;

    while (!self->quit) {
        jls_bkt_msg_wait(self->bk);
        jls_bkt_msg_lock(self->bk);
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
                case MSG_USER_DATA:
                    rc = jls_wr_user_data(self->wr, hdr.h.user_data.chunk_meta, hdr.h.user_data.storage_type,
                                          payload, payload_sz);
                    break;
                case MSG_FSR_F32:
                    rc = jls_wr_fsr_f32(self->wr, hdr.h.fsr_f32.signal_id, hdr.h.fsr_f32.sample_id,
                                        (const float *) payload, payload_sz / 4);
                    break;
                case MSG_ANNOTATION:
                    rc = jls_wr_annotation(self->wr, hdr.h.annotation.signal_id, hdr.h.annotation.timestamp,
                                           hdr.h.annotation.annotation_type,
                                           hdr.h.annotation.group_id,
                                           hdr.h.annotation.storage_type,
                                           (const uint8_t *) payload, payload_sz);
                    break;
                default:
                    break;
            }
            jls_bkt_process_unlock(self->bk);

            if (rc) {
                JLS_LOGW("thread msg %d returned %d", (int) hdr.msg_type, (int) rc);
            }

            jls_bkt_msg_lock(self->bk);
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

int32_t jls_twr_close(struct jls_twr_s * self) {
    int rc = 0;
    if (self) {
        struct msg_header_s hdr = { .msg_type = MSG_CLOSE };
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

int32_t jls_twr_fsr_f32(struct jls_twr_s * self, uint16_t signal_id,
                        int64_t sample_id, const float * data, uint32_t data_length) {
    struct msg_header_s hdr = {
            .msg_type = MSG_FSR_F32,
            .h = {
                    .fsr_f32 = {
                            .signal_id = signal_id,
                            .sample_id = sample_id,
                    }
            },
            .d = 0
    };
    return msg_send(self, &hdr, (const uint8_t *) data, data_length * sizeof(float));
}

int32_t jls_twr_annotation(struct jls_twr_s * self, uint16_t signal_id, int64_t timestamp,
                           enum jls_annotation_type_e annotation_type,
                           uint8_t group_id,
                           enum jls_storage_type_e storage_type, const uint8_t * data, uint32_t data_size) {
    struct msg_header_s hdr = {
            .msg_type = MSG_ANNOTATION,
            .h = {
                    .annotation = {
                            .signal_id = signal_id,
                            .timestamp = timestamp,
                            .annotation_type = annotation_type,
                            .storage_type = storage_type,
                            .group_id = group_id,
                    }
            },
            .d = 0
    };
    return msg_send(self, &hdr, data, data_size);
}
