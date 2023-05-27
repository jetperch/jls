/*
 * Copyright 2014-2023 Jetperch LLC
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

/**
 * @file
 *
 * @brief Joulescope driver test utility
 */

#include "jls.h"
#include "jls/log.h"
#include "jls/jls_util_prv.h"
#include <inttypes.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <signal.h>


struct command_s {
    const char * command;
    command_fn fn;
    const char * description;
};

struct app_s app_;
volatile int32_t quit_;

// cross-platform handler for CTRL-C to exit program
static void signal_handler(int signal){
    if ((signal == SIGABRT) || (signal == SIGINT)) {
        quit_ = 1;
    }
}

static void on_log_recv(const char * msg) {
    char time_str[64];
    struct tm tm_utc;
    int64_t time_now = jls_now();
    time_t time_s = (time_t) (time_now / JLS_TIME_SECOND);
    int time_us = (int) ((time_now - time_s * JLS_TIME_SECOND) / JLS_TIME_MICROSECOND);
    time_s += JLS_TIME_EPOCH_UNIX_OFFSET_SECONDS;
#if _WIN32
    _gmtime64_s(&tm_utc, &time_s);  // windows only
#else
    gmtime_r(&time_s, &tm_utc);  // posix https://en.cppreference.com/w/c/chrono/gmtime
#endif
    strftime(time_str, sizeof(time_str), "%FT%T", &tm_utc);
    printf("%s.%06dZ %s\n", time_str, time_us, msg);
}

const struct command_s COMMANDS[] = {
//        {"dev",  on_dev,  "Developer tools"},
        {"info", on_info, "Display JLS file information"},
        {"version", on_version, "Display version and platform information"},
        {"help", on_help, "Display help"},
        {NULL, NULL, NULL}
};

static int usage(void) {
    const struct command_s * cmd = COMMANDS;
    printf("usage: jls <COMMAND> [...args]\n");
    printf("\nAvailable commands:\n");
    while (cmd->command) {
        printf("  %-12s %s\n", cmd->command, cmd->description);
        ++cmd;
    }
    return 1;
}

int on_help(struct app_s * self, int argc, char * argv[]) {
    (void) self;
    (void) argc;
    (void) argv;
    usage();
    return 0;
}

int on_version(struct app_s * self, int argc, char * argv[]) {
    (void) self;
    (void) argc;
    (void) argv;
    printf("%s\n", jls_version_str());
    return 0;
}


struct log_level_convert_s {
    const char * str;
    int8_t level;
};

int main(int argc, char * argv[]) {
    struct app_s * self = &app_;
    memset(self, 0, sizeof(*self));
    int32_t rc;

    if (argc < 2) {
        return usage();
    }
    ARG_CONSUME();

    signal(SIGABRT, signal_handler);
    signal(SIGINT, signal_handler);

    char * command_str = argv[0];
    ARG_CONSUME();

    rc = 9999;
    const struct command_s * cmd = COMMANDS;
    while (cmd->command) {
        if (strcmp(cmd->command, command_str) == 0) {
            rc = cmd->fn(self, argc, argv);
            break;
        }
        ++cmd;
    }
    if (rc == 9999) {
        rc = usage();
    }

    if (rc) {
        printf("### ERROR return code %d %s %s ###\n", rc,
               jls_error_code_name(rc),
               jls_error_code_description(rc));
    }
    return rc;
}
