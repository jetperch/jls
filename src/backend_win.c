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

#include "jls/backend.h"
#include "jls/ec.h"
#include "jls/log.h"
#include "jls/time.h"

#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <sys\stat.h>
#include <share.h>


// https://docs.microsoft.com/en-us/cpp/c-runtime-library/low-level-i-o?view=msvc-160
// The C standard library only gets in the way for JLS.
int32_t jls_bk_fopen(struct jls_bkf_s * self, const char * filename, const char * mode) {
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
    errno_t err = _sopen_s(&self->fd, filename, oflag, shflag, _S_IREAD | _S_IWRITE);
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
        JLS_LOGE("write mismatch %d != %d", sz, buffer_size);
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

int64_t jls_now() {
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

struct jls_time_counter_s jls_time_counter() {
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
