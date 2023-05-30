/*
 * Copyright 2022-2023 Jetperch LLC
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

#include "jls.h"

#if defined(_WIN64)
#define PLATFORM "win64"
#elif defined(_WIN32)
#define PLATFORM "win32"
#elif defined(__CYGWIN__)
#define PLATFORM "win cygwin"
#elif defined(__APPLE__) && defined(__MACH__) // Apple OSX and iOS (Darwin)
    #define PLATFORM "osx"
#elif defined(__linux__)
    #define PLATFORM "linux"
#else
    #define PLATFORM "unknown"
#endif

#define ARG_CONSUME() --argc; ++argv
#define ARG_REQUIRE()  if (argc <= 0) {return usage();}

#define ROE(x) do {         \
    int rc__ = (x);         \
    if (rc__) {             \
        return rc__;        \
    }                       \
} while (0)

struct app_s {
    struct jls_rd_s * rd;
    struct jls_wr_s * wr;
    int32_t verbose;
    char * filename;
};

extern volatile int32_t quit_;

typedef int (*command_fn)(struct app_s * self, int argc, char * argv[]);

int on_help(struct app_s * self, int argc, char * argv[]);
int on_fsr_statistics(struct app_s * self, int argc, char * argv[]);
int on_info(struct app_s * self, int argc, char * argv[]);
int on_inspect(struct app_s * self, int argc, char * argv[]);
int on_version(struct app_s * self, int argc, char * argv[]);
