/*
 * Copyright 2014-2017 Jetperch LLC
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

#include "jls/log.h"
#include <stdio.h>
#include <stdarg.h>

char const * const jls_log_level_str[JLS_LOG_LEVEL_ALL + 1] = {
        "EMERGENCY",
        "ALERT",
        "CRITICAL",
        "ERROR",
        "WARN",
        "NOTICE"
        "INFO",
        "DEBUG",
        "DEBUG2"
        "DEBUG3",
        "ALL"
};

char const jls_log_level_char[JLS_LOG_LEVEL_ALL + 1] = {
        '!', 'A', 'C', 'E', 'W', 'N', 'I', 'D', 'D', 'D', '.'
};

void jls_log_printf_default(const char * fmt, ...) {
    // todo remove
    va_list arg;
    va_start(arg, fmt);
    vprintf(fmt, arg);
    va_end(arg);
    fflush(stdout);
}

volatile jls_log_printf jls_log_printf_ = jls_log_printf_default;

int jls_log_initialize(jls_log_printf handler) {
    if (NULL == handler) {
        jls_log_printf_ = jls_log_printf_default;
    } else {
        jls_log_printf_ = handler;
    }
    return 0;
}

void jls_log_finalize() {
    jls_log_initialize(0);
}
