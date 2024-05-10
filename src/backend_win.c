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
#include "jls/cdef.h"
#include "jls/wr_prv.h"
#include "jls/ec.h"
#include "jls/log.h"
#include "jls/time.h"

#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <sys\stat.h>
#include <share.h>


struct jls_bkt_s {
    HANDLE msg_mutex;
    HANDLE process_mutex;
    HANDLE msg_event;
    HANDLE thread;
};

// Simplified implementation, for backend purposed.
struct jls_twr_s {
    struct jls_bkt_s * bk;  // REQUIRED first entry
};

// https://docs.microsoft.com/en-us/cpp/c-runtime-library/low-level-i-o?view=msvc-160
// The C standard library only gets in the way for JLS.
int32_t jls_bk_fopen(struct jls_bkf_s * self, const char * filename, const char * mode) {
    // https://learn.microsoft.com/en-us/windows/win32/fileio/maximum-file-path-limitation?tabs=registry
    wchar_t filename_wide[32768];
    int oflag;
    int shflag;

    switch (mode[0]) {
        case 'w':
            oflag = _O_BINARY | _O_CREAT | _O_RDWR | _O_RANDOM | _O_TRUNC;
            shflag = _SH_DENYWR;
            break;
        case 'r':
            oflag = _O_BINARY | _O_RDONLY | _O_RANDOM;
            shflag = _SH_DENYNO;
            break;
        case 'a':
            oflag = _O_BINARY | _O_RDWR | _O_RANDOM;
            shflag = _SH_DENYWR;
            break;
        default:
            return JLS_ERROR_PARAMETER_INVALID;
    }
    if (!MultiByteToWideChar(CP_UTF8, 0, filename, -1, filename_wide, JLS_ARRAY_SIZE(filename_wide))) {
        return JLS_ERROR_IO;
    }
    errno_t err = _wsopen_s(&self->fd, filename_wide, oflag, shflag, _S_IREAD | _S_IWRITE);
    if (err != 0) {
        JLS_LOGW("open failed with %d: filename=%s, mode=%s", err, filename, mode);
        return JLS_ERROR_IO;
    }
    return 0;
}

int32_t jls_bk_fclose(struct jls_bkf_s * self) {
    if (self->fd != -1) {
        _close(self->fd);
        self->fd = -1;
    }
    return 0;
}

int32_t jls_bk_fwrite(struct jls_bkf_s * self, const void * buffer, unsigned int count) {
    int sz = _write(self->fd, buffer, count);
    if (sz < 0) {
        JLS_LOGE("write failed %d", errno);
        return JLS_ERROR_IO;
    }
    self->fpos += sz;
    if (self->fpos > self->fend) {
        self->fend = self->fpos;
    }
    if ((unsigned int) sz != count) {
        JLS_LOGE("write mismatch %d != %d", sz, count);
        return JLS_ERROR_IO;
    }
    return 0;
}

int32_t jls_bk_fread(struct jls_bkf_s * self, void * const buffer, unsigned const buffer_size) {
    int sz = _read(self->fd, buffer, buffer_size);
    if (sz < 0) {
        JLS_LOGE("read failed %d", errno);
        return JLS_ERROR_IO;
    }
    self->fpos += sz;
    if ((unsigned int) sz != buffer_size) {
        JLS_LOGE("read length mismatch: read %d, expected %d", sz, buffer_size);
        return JLS_ERROR_IO;
    }
    return 0;
}

int32_t jls_bk_fseek(struct jls_bkf_s * self, int64_t offset, int origin) {
    int64_t pos = _lseeki64(self->fd, offset, origin);
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
    return _telli64(self->fd);
}

int32_t jls_bk_fflush(struct jls_bkf_s * self) {
    return _commit(self->fd);
}

int32_t jls_bk_truncate(struct jls_bkf_s * self) {
    if (_chsize_s(self->fd, self->fpos) < 0) {
        JLS_LOGE("V failed %d", errno);
        return JLS_ERROR_IO;
    }
    if (self->fend > self->fpos) {
        self->fend = self->fpos;
    }
    return 0;
}

static DWORD WINAPI task(LPVOID lpParam) {
    struct jls_twr_s * self = (struct jls_twr_s *) lpParam;
    return jls_twr_run(self);
}


struct jls_bkt_s * jls_bkt_initialize(struct jls_twr_s * wr) {
    struct jls_bkt_s * self = calloc(1, sizeof(struct jls_bkt_s));
    if (!self) {
        return NULL;
    }
    self->msg_mutex = CreateMutex(
            NULL,                   // default security attributes
            FALSE,                  // initially not owned
            NULL);                  // unnamed mutex
    if (!self->msg_mutex) {
        jls_bkt_finalize(self);
        return NULL;
    }

    self->process_mutex = CreateMutex(
            NULL,                   // default security attributes
            FALSE,                  // initially not owned
            NULL);                  // unnamed mutex
    if (!self->process_mutex) {
        jls_bkt_finalize(self);
        return NULL;
    }

    self->msg_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!self->msg_event) {
        jls_bkt_finalize(self);
        return NULL;
    }

    wr->bk = self;
    self->thread = CreateThread(
            NULL,                   // default security attributes
            0,                      // use default stack size
            task,                   // thread function name
            wr,                     // argument to thread function
            0,                      // use default creation flags
            NULL);                  // returns the thread identifier
    if (!self->thread) {
        jls_bkt_finalize(self);
        wr->bk = NULL;
        return NULL;
    }
    if (!SetThreadPriority(self->thread, THREAD_PRIORITY_ABOVE_NORMAL)) {
        JLS_LOGW("Could not reduce thread priority: %d", (int) GetLastError());
    }
    return self;
}

void jls_bkt_finalize(struct jls_bkt_s * self) {
    if (self) {
        if (self->thread) {
            DWORD rc = WaitForSingleObject(self->thread, JLS_BK_CLOSE_TIMEOUT_MS);
            if (WAIT_OBJECT_0 != rc) {
                JLS_LOGE("thread close wait failed %d", (int) rc);
            }
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
        free(self);
    }
}

int jls_bkt_msg_lock(struct jls_bkt_s * self) {
    DWORD rc = WaitForSingleObject(self->msg_mutex, JLS_BK_MSG_LOCK_TIMEOUT_MS);
    if (WAIT_OBJECT_0 != rc) {
        JLS_LOGE("jls_bkt_msg_lock failed %d", (int) rc);
        return rc;
    }
    return 0;
}

int jls_bkt_msg_unlock(struct jls_bkt_s * self) {
    if (!ReleaseMutex(self->msg_mutex)) {
        JLS_LOGE("jls_bkt_msg_unlock failed");
        return 1;
    }
    return 0;
}

int jls_bkt_process_lock(struct jls_bkt_s * self) {
    DWORD rc = WaitForSingleObject(self->process_mutex, JLS_BK_PROCESS_LOCK_TIMEOUT_MS);
    if (WAIT_OBJECT_0 != rc) {
        JLS_LOGE("jls_bkt_msg_lock failed %d", (int) rc);
        return rc;
    }
    return 0;
}

int jls_bkt_process_unlock(struct jls_bkt_s * self) {
    if (!ReleaseMutex(self->process_mutex)) {
        JLS_LOGE("jls_bkt_msg_unlock failed");
        return 1;
    }
    return 0;
}

void jls_bkt_msg_wait(struct jls_bkt_s * self) {
    WaitForSingleObject(self->msg_event, 10);
    ResetEvent(self->msg_event);
}

void jls_bkt_msg_signal(struct jls_bkt_s * self) {
    SetEvent(self->msg_event);
}

void jls_bkt_sleep_ms(uint32_t duration_ms) {
    Sleep(duration_ms);
}


int64_t jls_now(void) {
    // Contains a 64-bit value representing the number of 100-nanosecond intervals since January 1, 1601 (UTC).
    // python
    // import dateutil.parser
    // dateutil.parser.parse('2018-01-01T00:00:00Z').timestamp() - dateutil.parser.parse('1601-01-01T00:00:00Z').timestamp()
    static const int64_t offset_s = 131592384000000000LL;  // 100 ns
    static const uint64_t frequency = 10000000; // 100 ns
    FILETIME filetime;
    GetSystemTimePreciseAsFileTime(&filetime);
    uint64_t t = ((uint64_t) filetime.dwLowDateTime) | (((uint64_t) filetime.dwHighDateTime) << 32);
    t -= offset_s;
    return JLS_COUNTER_TO_TIME(t, frequency);
}

struct jls_time_counter_s jls_time_counter(void) {
    struct jls_time_counter_s counter;
    static int first = 1;
    static uint64_t offset = 0;     // in 34Q30 time
    static LARGE_INTEGER perf_frequency = {.QuadPart = 0};

    // https://docs.microsoft.com/en-us/windows/win32/sysinfo/acquiring-high-resolution-time-stamps
    LARGE_INTEGER perf_counter;

    QueryPerformanceCounter(&perf_counter);

    if (first) {
        QueryPerformanceFrequency(&perf_frequency);
        offset = perf_counter.QuadPart;
        first = 0;
    }

    counter.value = perf_counter.QuadPart - offset;
    counter.frequency = perf_frequency.QuadPart;
    return counter;
}
