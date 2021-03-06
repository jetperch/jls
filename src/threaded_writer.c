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
#include "jls/cdef.h"
#include "jls/ec.h"
#include "jls/log.h"
#include "jls/msg_ring_buffer.h"
#include "jls/writer.h"
#include <stdlib.h>
#include <windows.h>


#define MRB_BUFFER_SIZE (100000000)
#define MSG_LOCK_TIMEOUT_MS (1000)
#define PROCESS_LOCK_TIMEOUT_MS (1000)

struct jls_twr_s {
    struct jls_wr_s * wr;
    HANDLE msg_mutex;
    HANDLE process_mutex;
    HANDLE msg_event;
    HANDLE thread;
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

void msg_lock(struct jls_twr_s * self) {
    DWORD rc = WaitForSingleObject(self->msg_mutex, MSG_LOCK_TIMEOUT_MS);
    if (WAIT_OBJECT_0 != rc) {
        JLS_LOGE("msg_lock failed");
    }
}

void msg_unlock(struct jls_twr_s * self) {
    if (!ReleaseMutex(self->msg_mutex)) {
        JLS_LOGE("msg_unlock failed");
    }
}

void process_lock(struct jls_twr_s * self) {
    DWORD rc = WaitForSingleObject(self->process_mutex, PROCESS_LOCK_TIMEOUT_MS);
    if (WAIT_OBJECT_0 != rc) {
        JLS_LOGE("msg_lock failed");
    }
}

void process_unlock(struct jls_twr_s * self) {
    if (!ReleaseMutex(self->process_mutex)) {
        JLS_LOGE("msg_unlock failed");
    }
}

static DWORD WINAPI task(LPVOID lpParam) {
    HANDLE handles[16];
    uint32_t msg_size;
    uint8_t * msg;
    struct msg_header_s hdr;
    uint8_t * payload;
    int32_t rc = 0;

    struct jls_twr_s * self = (struct jls_twr_s *) lpParam;
    handles[0] = self->msg_event;

    while (!self->quit) {
        WaitForMultipleObjects(1, handles, FALSE, 1);
        ResetEvent(self->msg_event);
        msg_lock(self);
        while (!self->quit) {
            msg = jls_mrb_peek(&self->mrb, &msg_size);
            msg_unlock(self);
            if (!msg) {
                break;
            }
            payload = msg + sizeof(hdr);
            uint32_t payload_sz = msg_size - sizeof(hdr);

            memcpy(&hdr, msg, sizeof(hdr));
            rc = 0;

            process_lock(self);
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
            process_unlock(self);

            if (rc) {
                JLS_LOGW("thread msg %d returned %d", (int) hdr.msg_type, (int) rc);
            }

            msg_lock(self);
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
        jls_wr_close(wr);
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    self->quit = 0;
    self->wr = wr;
    self->thread = NULL;
    self->msg_event = NULL;
    self->msg_mutex = NULL;
    self->process_mutex = NULL;
    jls_mrb_init(&self->mrb, self->mrb_buffer, MRB_BUFFER_SIZE);

    self->msg_mutex = CreateMutex(
            NULL,                   // default security attributes
            FALSE,                  // initially not owned
            NULL);                  // unnamed mutex
    if (!self->msg_mutex) {
        jls_twr_close(self);
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }

    self->process_mutex = CreateMutex(
            NULL,                   // default security attributes
            FALSE,                  // initially not owned
            NULL);                  // unnamed mutex
    if (!self->process_mutex) {
        jls_twr_close(self);
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }

    self->msg_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!self->msg_event) {
        jls_twr_close(self);
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }

    self->thread = CreateThread(
            NULL,                   // default security attributes
            0,                      // use default stack size
            task,                   // thread function name
            self,                   // argument to thread function
            0,                      // use default creation flags
            NULL);                  // returns the thread identifier
    if (!self->thread) {
        jls_twr_close(self);
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    if (!SetThreadPriority(self->thread, THREAD_PRIORITY_BELOW_NORMAL)) {
        JLS_LOGW("Could not reduce thread priority: %d", (int) GetLastError());
    }

    *instance = self;
    return 0;
}

int32_t msg_send(struct jls_twr_s * self, const struct msg_header_s * hdr, const uint8_t * payload, uint32_t payload_size) {
    int32_t rv = 0;
    uint32_t sz = sizeof(*hdr) + payload_size;
    msg_lock(self);
    uint8_t * msg = jls_mrb_alloc(&self->mrb, sz);
    if (!msg) {
        rv = JLS_ERROR_NOT_ENOUGH_MEMORY;
    } else {
        memcpy(msg, hdr, sizeof(*hdr));
        if (payload_size) {
            memcpy(msg + sizeof(*hdr), payload, payload_size);
        }
    }
    msg_unlock(self);
    SetEvent(self->msg_event);
    return rv;
}

int32_t jls_twr_close(struct jls_twr_s * self) {
    int rc = 0;
    if (self) {
        if (self->thread) {
            struct msg_header_s hdr = { .msg_type = MSG_CLOSE};
            msg_send(self, &hdr, NULL, 0);
            WaitForSingleObject(self->thread, 1000);
            //self->quit = 1;  // force, just in case.
            CloseHandle(self->thread);
            self->thread = NULL;
        }
        if (self->msg_event) {
            CloseHandle(self->msg_event);
            self->msg_event = NULL;
        }
        if (self->msg_mutex) {
            CloseHandle(self->msg_mutex);
            self->msg_mutex = NULL;
        }
        if (self->process_mutex) {
            CloseHandle(self->process_mutex);
            self->process_mutex = NULL;
        }
        rc = jls_wr_close(self->wr);
        self->wr = NULL;
        free(self);
    }
    return rc;
}

int32_t jls_twr_source_def(struct jls_twr_s * self, const struct jls_source_def_s * source) {
    process_lock(self);
    int32_t rv = jls_wr_source_def(self->wr, source);
    process_unlock(self);
    return rv;
}

int32_t jls_twr_signal_def(struct jls_twr_s * self, const struct jls_signal_def_s * signal) {
    process_lock(self);
    int32_t rv = jls_wr_signal_def(self->wr, signal);
    process_unlock(self);
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
