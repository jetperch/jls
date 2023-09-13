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

#include "jls/backend.h"
#include "jls/wr_prv.h"
#include "jls/ec.h"
#include "jls/log.h"
#include "jls/time.h"

#define _FILE_OFFSET_BITS
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <stdlib.h>
#include <errno.h>


// https://stackoverflow.com/questions/8883512/pthread-condition-variables-vs-win32-events-linux-vs-windows-ce
struct event_flag {
    pthread_mutex_t mutex;
    pthread_cond_t  condition;
    unsigned int    flag;
};

struct jls_bkt_s {
    pthread_mutex_t msg_mutex;
    pthread_mutex_t process_mutex;
    struct event_flag * msg_event;
    pthread_t thread;
};

// Simplified implementation, for backend purposed.
struct jls_twr_s {
    struct jls_bkt_s * bk;  // REQUIRED first entry
};

static struct event_flag* eventflag_create(void) {
    struct event_flag* ev;
    ev = (struct event_flag*) malloc(sizeof(struct event_flag));
    pthread_mutex_init(&ev->mutex, NULL);
    pthread_cond_init(&ev->condition, NULL);
    ev->flag = 0;
    return ev;
}

static void eventflag_destroy(struct event_flag * ev) {
    if (ev) {
        pthread_mutex_destroy(&ev->mutex);
        pthread_cond_destroy(&ev->condition);
        free(ev);
    }
}

static void eventflag_wait(struct event_flag* ev) {
    pthread_mutex_lock(&ev->mutex);
    while (!ev->flag) {
        pthread_cond_wait(&ev->condition, &ev->mutex);
    }
    ev->flag = 0;
    pthread_mutex_unlock(&ev->mutex);
}

static void eventflag_set(struct event_flag* ev) {
    pthread_mutex_lock(&ev->mutex);
    ev->flag = 1;
    pthread_cond_signal(&ev->condition);
    pthread_mutex_unlock(&ev->mutex);
}

// https://docs.microsoft.com/en-us/cpp/c-runtime-library/low-level-i-o?view=msvc-160
// The C standard library only gets in the way for JLS.
int32_t jls_bk_fopen(struct jls_bkf_s * self, const char * filename, const char * mode) {
    int oflag;
    int fmode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;

    switch (mode[0]) {
        case 'w':
            oflag = O_RDWR | O_CREAT | O_TRUNC;
            break;
        case 'r':
            oflag = O_RDONLY;
            break;
        case 'a':
            oflag = O_RDWR;
            break;
        default:
            return JLS_ERROR_PARAMETER_INVALID;
    }
    self->fd = open(filename, oflag, fmode);
    if (self->fd < 0) {
        JLS_LOGW("open failed with %d: filename=%s, mode=%s", errno, filename, mode);
        return JLS_ERROR_IO;
    }
    return 0;
}

int32_t jls_bk_fclose(struct jls_bkf_s * self) {
    if (self->fd != -1) {
        close(self->fd);
        self->fd = -1;
    }
    return 0;
}

int32_t jls_bk_fwrite(struct jls_bkf_s * self, const void * buffer, unsigned int count) {
    ssize_t sz = write(self->fd, buffer, count);
    if (sz < 0) {
        JLS_LOGE("write failed %d", errno);
        return JLS_ERROR_IO;
    }
    self->fpos += sz;
    if (self->fpos > self->fend) {
        self->fend = self->fpos;
    }
    if ((unsigned int) sz != count) {
        JLS_LOGE("write mismatch %zd != %u", sz, count);
        return JLS_ERROR_IO;
    }
    return 0;
}

int32_t jls_bk_fread(struct jls_bkf_s * self, void * const buffer, unsigned const buffer_size) {
    int sz = read(self->fd, buffer, buffer_size);
    if (sz < 0) {
        JLS_LOGE("read failed %d", errno);
        return JLS_ERROR_IO;
    }
    self->fpos += sz;
    if ((unsigned int) sz != buffer_size) {
        JLS_LOGE("write mismatch %d != %d", sz, buffer_size);
        return JLS_ERROR_IO;
    }
    return 0;
}

int32_t jls_bk_fseek(struct jls_bkf_s * self, int64_t offset, int origin) {
    int64_t pos = lseek(self->fd, offset, origin);
    if (pos < 0) {
        JLS_LOGE("seek fail %d", errno);
        return JLS_ERROR_IO;
    }
    if ((origin == SEEK_SET) && (pos != offset)) {
        JLS_LOGE("seek fail %d", errno);
        return JLS_ERROR_IO;
    }
    self->fpos = pos;
    return 0;
}

int64_t jls_bk_ftell(struct jls_bkf_s * self) {
    return lseek(self->fd, 0, SEEK_CUR);
}

int32_t jls_bk_fflush(struct jls_bkf_s * self) {
    return fsync(self->fd);
}

int32_t jls_bk_truncate(struct jls_bkf_s * self) {
    int rc = ftruncate(self->fd, self->fpos);
    if (rc) {
        JLS_LOGE("truncate fail %d", errno);
        return JLS_ERROR_IO;
    }
    if (self->fend > self->fpos) {
        self->fend = self->fpos;
    }
    return 0;
}

static void * task(void * user_data) {
    struct jls_twr_s * self = (struct jls_twr_s *) user_data;
    jls_twr_run(self);
    return NULL;
}

struct jls_bkt_s * jls_bkt_initialize(struct jls_twr_s * wr) {
    struct jls_bkt_s * self = calloc(1, sizeof(struct jls_bkt_s));
    if (!self) {
        return NULL;
    }

    if (pthread_mutex_init(&self->msg_mutex, NULL)) {
        JLS_LOGE("jls_bkt_initialize: msg_mutex failed");
        jls_bkt_finalize(self);
        return NULL;
    }
    if (pthread_mutex_init(&self->process_mutex, NULL)) {
        JLS_LOGE("jls_bkt_initialize: process_mutex failed");
        jls_bkt_finalize(self);
        return NULL;
    }

    self->msg_event = eventflag_create();
    if (!self->msg_event) {
        JLS_LOGE("jls_bkt_initialize: eventflag_create failed");
        jls_bkt_finalize(self);
        return NULL;
    }

    wr->bk = self;
    int rc = pthread_create(&self->thread, NULL, task, wr);
    if (rc) {
        JLS_LOGE("jls_bkt_initialize: pthread_create returned %d", rc);
        jls_bkt_finalize(self);
        wr->bk = NULL;
        return NULL;
    }
    return self;
}

void jls_bkt_finalize(struct jls_bkt_s * self) {
    if (self) {
        if (self->thread) {
            void * rv = NULL;
            int rc = pthread_join(self->thread, &rv);
            if (rc) {
                JLS_LOGE("jls_bkt_finalize join failed with %d", rc);
            }
        }
        if (self->msg_event) {
            eventflag_destroy(self->msg_event);
            self->msg_event = NULL;
        }
        pthread_mutex_destroy(&self->msg_mutex);
        pthread_mutex_destroy(&self->process_mutex);
        free(self);
    }
}

int jls_bkt_msg_lock(struct jls_bkt_s * self) {
    int rc = pthread_mutex_lock(&self->msg_mutex);
    if (rc) {
        JLS_LOGE("jls_bkt_msg_lock failed %d", (int) rc);
    }
    return rc;
}

int jls_bkt_msg_unlock(struct jls_bkt_s * self) {
    int rc = pthread_mutex_unlock(&self->msg_mutex);
    if (rc) {
        JLS_LOGE("jls_bkt_msg_unlock failed %d", (int) rc);
    }
    return rc;
}

int jls_bkt_process_lock(struct jls_bkt_s * self) {
    int rc = pthread_mutex_lock(&self->process_mutex);
    if (rc) {
        JLS_LOGE("jls_bkt_process_lock failed %d", (int) rc);
    }
    return rc;
}

int jls_bkt_process_unlock(struct jls_bkt_s * self) {
    int rc = pthread_mutex_unlock(&self->process_mutex);
    if (rc) {
        JLS_LOGE("jls_bkt_process_unlock failed %d", (int) rc);
    }
    return rc;
}

void jls_bkt_msg_wait(struct jls_bkt_s * self) {
    eventflag_wait(self->msg_event);
}

void jls_bkt_msg_signal(struct jls_bkt_s * self) {
    eventflag_set(self->msg_event);
}

void jls_bkt_sleep_ms(uint32_t duration_ms) {
    struct timespec ts;
    int rv;
    ts.tv_sec = duration_ms / 1000;
    ts.tv_nsec = ((long) (duration_ms % 1000)) * 1000000;
    do {
        rv = nanosleep(&ts, &ts);
    } while (rv && errno == EINTR);
}

int64_t jls_now(void) {
    int64_t t;
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts)) {
        JLS_LOGE("clock_gettime error");
    }
    t = ((int64_t) ts.tv_sec + JLS_TIME_EPOCH_UNIX_OFFSET_SECONDS) * JLS_TIME_SECOND;
    t += JLS_COUNTER_TO_TIME(ts.tv_nsec, 1000000000LL);
    return t;
}

struct jls_time_counter_s jls_time_counter(void) {
    struct jls_time_counter_s counter;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) {
        JLS_LOGE("clock_gettime error");
    }
    counter.value = ((int64_t) ts.tv_sec + JLS_TIME_EPOCH_UNIX_OFFSET_SECONDS) * JLS_TIME_SECOND;
    counter.value += JLS_COUNTER_TO_TIME(ts.tv_nsec, 1000000000LL);
    counter.frequency = JLS_TIME_SECOND;
    return counter;
}
