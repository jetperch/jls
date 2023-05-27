/*
 * Copyright 2014-2022 Jetperch LLC
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

static void cbk_default(const char * msg) {
    (void) msg;
}

static jls_log_cbk cbk_ = cbk_default;

void jls_log_printf(const char * fmt, ...) {
    char buffer[1024];
    va_list arg;
    va_start(arg, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, arg);
    va_end(arg);
    cbk_(buffer);
}

JLS_API void jls_log_register(jls_log_cbk handler) {
    if (NULL == handler) {
        cbk_ = cbk_default;
    } else {
        cbk_ = handler;
    }
}

JLS_API void jls_log_unregister(void) {
    jls_log_register(NULL);
}

JLS_API const char * jls_log_level_to_str(int8_t level) {
    if (level < 0) {
        return "OFF";
    }
    if (level >= JLS_LOG_LEVEL_ALL) {
        return jls_log_level_str[JLS_LOG_LEVEL_ALL];
    }
    return jls_log_level_str[level];
}

JLS_API char jls_log_level_to_char(int8_t level) {
    if (level < 0) {
        return '*';
    }
    if (level >= JLS_LOG_LEVEL_ALL) {
        return jls_log_level_char[JLS_LOG_LEVEL_ALL];
    }
    return jls_log_level_char[level];
}
