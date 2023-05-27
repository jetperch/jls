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

/*!
 * \file
 *
 * \brief Trivial logging support.
 */

#ifndef JLS_LOG_H_
#define JLS_LOG_H_

#include "jls/cmacro.h"
#include <stdint.h>

/**
 * @ingroup jls
 * @defgroup jls_log Console logging
 *
 * @brief Generic console logging with compile-time levels.
 *
 * To use this module, call jls_log_initialize() with the appropriate
 * handler for your application.
 *
 * @{
 */

JLS_CPP_GUARD_START

/**
 * @def JLS_LOG_GLOBAL_LEVEL
 *
 * @brief The global logging level.
 *
 * The maximum level to compile regardless of the individual module level.
 * This value should be defined in the project CMake (makefile).
 */
#ifndef JLS_LOG_GLOBAL_LEVEL
#define JLS_LOG_GLOBAL_LEVEL JLS_LOG_LEVEL_ALL
#endif

/**
 * @def JLS_LOG_LEVEL
 *
 * @brief The module logging level.
 *
 * Typical usage 1:  (not MISRA C compliant, but safe)
 *
 *      #define JLS_LOG_LEVEL JLS_LOG_LEVEL_WARNING
 *      #include "log.h"
 */
#ifndef JLS_LOG_LEVEL
#define JLS_LOG_LEVEL JLS_LOG_LEVEL_WARNING
#endif

/**
 * @def \_\_FILENAME\_\_
 *
 * @brief The filename to display for logging.
 *
 * When compiling C and C++ code, the __FILE__ define may contain a long path
 * that just confuses the log output.  The build tools, such as make and cmake,
 * can define __FILENAME__ to produce more meaningful results.
 *
 * A good Makefile usage includes:
 *
 */
#ifndef __FILENAME__
#define __FILENAME__ __FILE__
#endif

#if defined(__GNUC__) && !defined(_WIN32)
/* https://gcc.gnu.org/onlinedocs/gcc-4.7.2/gcc/Function-Attributes.html */
#define JLS_LOG_PRINTF_FORMAT __attribute__((format (printf, 1, 2)))
#else
#define JLS_LOG_PRINTF_FORMAT
#endif

/**
 * @brief The printf-style function call for each log message.
 *
 * @param format The print-style formatting string.
 *     The remaining parameters are arguments for the formatting string.
 * @return The number of characters printed.
 *
 * For PC-based applications, a common implementation is::
 *
 *     #include <stdarg.h>
 *     #include <stdio.h>
 *
 *     void jls_log_printf(const char * format, ...) {
 *         va_list arg;
 *         va_start(arg, format);
 *         vprintf(format, arg);
 *         va_end(arg);
 *     }
 *
 * If your application calls the LOG* macros from multiple threads, then
 * the jls_log_printf implementation must be thread-safe and reentrant.
 *
 * This function is exposed to allow for unit testing.
 */
JLS_API void jls_log_printf(const char * format, ...) JLS_LOG_PRINTF_FORMAT;

/**
 * @brief The callback for log messages.
 *
 * @param msg The log message.
 */
typedef void (*jls_log_cbk)(const char * msg);

/**
 * @brief Register a logging handler
 *
 * @param handler The log handler.  Pass NULL or call jls_log_unregister() to
 *      restore the default log handler.
 *
 * @return 0 or error code.
 *
 * The library initializes with a default null log handler so that logging
 * which occurs before jls_log_register will not cause a fault.  This function
 * may be safely called at any time, even without finalize.
 */
JLS_API void jls_log_register(jls_log_cbk handler);

/**
 * @brief Finalize the logging feature.
 *
 * This is equivalent to calling jls_log_initialize(0).
 */
JLS_API void jls_log_unregister(void);

/**
 * @def JLS_LOG_PRINTF
 * @brief The printf function including log formatting.
 *
 * @param level The level for this log message
 * @param format The formatting string
 * @param ... The arguments for the formatting string
 */
#ifndef JLS_LOG_PRINTF
#define JLS_LOG_PRINTF(level, format, ...) \
    jls_log_printf("%c %s:%d: " format "\n", jls_log_level_char[level], __FILENAME__, __LINE__, __VA_ARGS__);
#endif

/**
 * @brief The available logging levels.
 */
enum jls_log_level_e {
    /** Logging functionality is disabled. */
    JLS_LOG_LEVEL_OFF         = -1,
    /** A "panic" condition that may result in significant harm. */
    JLS_LOG_LEVEL_EMERGENCY   = 0,
    /** A condition requiring immediate action. */
    JLS_LOG_LEVEL_ALERT       = 1,
    /** A critical error which prevents further functions. */
    JLS_LOG_LEVEL_CRITICAL    = 2,
    /** An error which prevents the current operation from completing or
     *  will adversely effect future functionality. */
    JLS_LOG_LEVEL_ERROR       = 3,
    /** A warning which may adversely affect the current operation or future
     *  operations. */
    JLS_LOG_LEVEL_WARNING     = 4,
    /** A notification for interesting events. */
    JLS_LOG_LEVEL_NOTICE      = 5,
    /** An informative message. */
    JLS_LOG_LEVEL_INFO        = 6,
    /** Detailed messages for the software developer. */
    JLS_LOG_LEVEL_DEBUG1      = 7,
    /** Very detailed messages for the software developer. */
    JLS_LOG_LEVEL_DEBUG2      = 8,
    /** Insanely detailed messages for the software developer. */
    JLS_LOG_LEVEL_DEBUG3      = 9,
    /** All logging functionality is enabled. */
    JLS_LOG_LEVEL_ALL         = 10,
};

/** Detailed messages for the software developer. */
#define JLS_LOG_LEVEL_DEBUG JLS_LOG_LEVEL_DEBUG1

/**
 * @brief Map log level to a string name.
 */
extern char const * const jls_log_level_str[JLS_LOG_LEVEL_ALL + 1];

/**
 * @brief Map log level to a single character.
 */
extern char const jls_log_level_char[JLS_LOG_LEVEL_ALL + 1];

/**
 * @brief Convert a log level to a user-meaningful string description.
 *
 * @param level The log level.
 * @return The string description.
 */
JLS_API const char * jsdrv_log_level_to_str(int8_t level);

/**
 * @brief Convert a log level to a user-meaningful character.
 *
 * @param level The log level.
 * @return The character representing the log level.
 */
JLS_API char jsdrv_log_level_to_char(int8_t level);

/**
 * @brief Check the current level against the static logging configuration.
 *
 * @param level The level to query.
 * @return True if logging at level is permitted.
 */
#define JLS_LOG_CHECK_STATIC(level) ((level <= JLS_LOG_GLOBAL_LEVEL) && (level <= JLS_LOG_LEVEL) && (level >= 0))

/**
 * @brief Check a log level against a configured level.
 *
 * @param level The level to query.
 * @param cfg_level The configured logging level.
 * @return True if level is permitted given cfg_level.
 */
#define JLS_LOG_LEVEL_CHECK(level, cfg_level) (level <= cfg_level)

/*!
 * \brief Macro to log a printf-compatible formatted string.
 *
 * \param level The jls_log_level_e.
 * \param format The printf-compatible formatting string.
 * \param ... The arguments to the formatting string.
 */
#define JLS_LOG(level, format, ...) \
    do { \
        if (JLS_LOG_CHECK_STATIC(level)) { \
            JLS_LOG_PRINTF(level, format, __VA_ARGS__); \
        } \
    } while (0)


#ifdef _MSC_VER
/* Microsoft Visual Studio compiler support */
/** Log a emergency using printf-style arguments. */
#define JLS_LOG_EMERGENCY(format, ...)  JLS_LOG(JLS_LOG_LEVEL_EMERGENCY, format, __VA_ARGS__)
/** Log a alert using printf-style arguments. */
#define JLS_LOG_ALERT(format, ...)  JLS_LOG(JLS_LOG_LEVEL_ALERT, format, __VA_ARGS__)
/** Log a critical failure using printf-style arguments. */
#define JLS_LOG_CRITICAL(format, ...)  JLS_LOG(JLS_LOG_LEVEL_CRITICAL, format, __VA_ARGS__)
/** Log an error using printf-style arguments. */
#define JLS_LOG_ERROR(format, ...)     JLS_LOG(JLS_LOG_LEVEL_ERROR, format, __VA_ARGS__)
/** Log a warning using printf-style arguments. */
#define JLS_LOG_WARNING(format, ...)      JLS_LOG(JLS_LOG_LEVEL_WARNING, format, __VA_ARGS__)
/** Log a notice using printf-style arguments. */
#define JLS_LOG_NOTICE(format, ...)    JLS_LOG(JLS_LOG_LEVEL_NOTICE,   format, __VA_ARGS__)
/** Log an informative message using printf-style arguments. */
#define JLS_LOG_INFO(format, ...)      JLS_LOG(JLS_LOG_LEVEL_INFO,     format, __VA_ARGS__)
/** Log a detailed debug message using printf-style arguments. */
#define JLS_LOG_DEBUG1(format, ...)    JLS_LOG(JLS_LOG_LEVEL_DEBUG1,    format, __VA_ARGS__)
/** Log a very detailed debug message using printf-style arguments. */
#define JLS_LOG_DEBUG2(format, ...)    JLS_LOG(JLS_LOG_LEVEL_DEBUG2,  format, __VA_ARGS__)
/** Log an insanely detailed debug message using printf-style arguments. */
#define JLS_LOG_DEBUG3(format, ...)    JLS_LOG(JLS_LOG_LEVEL_DEBUG3,  format, __VA_ARGS__)

#else
/* GCC compiler support */
// zero length variadic arguments are not allowed for macros
// this hack ensures that LOG(message) and LOG(format, args...) are both supported.
// https://stackoverflow.com/questions/5588855/standard-alternative-to-gccs-va-args-trick
#define _JLS_LOG_SELECT(PREFIX, _11, _10, _9, _8, _7, _6, _5, _4, _3, _2, _1, SUFFIX, ...) PREFIX ## _ ## SUFFIX
#define _JLS_LOG_1(level, message) JLS_LOG(level, "%s", message)
#define _JLS_LOG_N(level, format, ...) JLS_LOG(level, format, __VA_ARGS__)
#define _JLS_LOG_DISPATCH(level, ...)  _JLS_LOG_SELECT(_JLS_LOG, __VA_ARGS__, N, N, N, N, N, N, N, N, N, N, 1, 0)(level, __VA_ARGS__)

/** Log a emergency using printf-style arguments. */
#define JLS_LOG_EMERGENCY(...)  _JLS_LOG_DISPATCH(JLS_LOG_LEVEL_EMERGENCY, __VA_ARGS__)
/** Log a alert using printf-style arguments. */
#define JLS_LOG_ALERT(...)  _JLS_LOG_DISPATCH(JLS_LOG_LEVEL_ALERT, __VA_ARGS__)
/** Log a critical failure using printf-style arguments. */
#define JLS_LOG_CRITICAL(...)  _JLS_LOG_DISPATCH(JLS_LOG_LEVEL_CRITICAL, __VA_ARGS__)
/** Log an error using printf-style arguments. */
#define JLS_LOG_ERROR(...)     _JLS_LOG_DISPATCH(JLS_LOG_LEVEL_ERROR, __VA_ARGS__)
/** Log a warning using printf-style arguments. */
#define JLS_LOG_WARNING(...)      _JLS_LOG_DISPATCH(JLS_LOG_LEVEL_WARNING, __VA_ARGS__)
/** Log a notice using printf-style arguments. */
#define JLS_LOG_NOTICE(...)    _JLS_LOG_DISPATCH(JLS_LOG_LEVEL_NOTICE,   __VA_ARGS__)
/** Log an informative message using printf-style arguments. */
#define JLS_LOG_INFO(...)      _JLS_LOG_DISPATCH(JLS_LOG_LEVEL_INFO,     __VA_ARGS__)
/** Log a detailed debug message using printf-style arguments. */
#define JLS_LOG_DEBUG1(...)    _JLS_LOG_DISPATCH(JLS_LOG_LEVEL_DEBUG1,    __VA_ARGS__)
/** Log a very detailed debug message using printf-style arguments. */
#define JLS_LOG_DEBUG2(...)    _JLS_LOG_DISPATCH(JLS_LOG_LEVEL_DEBUG2,  __VA_ARGS__)
/** Log an insanely detailed debug message using printf-style arguments. */
#define JLS_LOG_DEBUG3(...)    _JLS_LOG_DISPATCH(JLS_LOG_LEVEL_DEBUG3,  __VA_ARGS__)

#endif


/** Log an error using printf-style arguments.  Alias for JLS_LOG_ERROR. */
#define JLS_LOG_ERR JLS_LOG_ERROR
/** Log a warning using printf-style arguments.  Alias for JLS_LOG_WARNING. */
#define JLS_LOG_WARN JLS_LOG_WARNING
/** Log a detailed debug message using printf-style arguments.  Alias for JLS_LOG_DEBUG1. */
#define JLS_LOG_DEBUG JLS_LOG_DEBUG1
/** Log a detailed debug message using printf-style arguments.  Alias for JLS_LOG_DEBUG1. */
#define JLS_LOG_DBG JLS_LOG_DEBUG1

#define JLS_LOGE JLS_LOG_ERROR
#define JLS_LOGW JLS_LOG_WARNING
#define JLS_LOGN JLS_LOG_NOTICE
#define JLS_LOGI JLS_LOG_INFO
#define JLS_LOGD JLS_LOG_DEBUG1
#define JLS_LOGD1 JLS_LOG_DEBUG1
#define JLS_LOGD2 JLS_LOG_DEBUG2
#define JLS_LOGD3 JLS_LOG_DEBUG3

JLS_CPP_GUARD_END

/** @} */

#endif /* JLS_LOG_H_ */
