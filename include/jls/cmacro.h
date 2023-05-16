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

/**
 * @file
 *
 * @brief Commonly used C macros for JLS.
 */

#ifndef JLS_CMACRO_INC_H__
#define JLS_CMACRO_INC_H__

/**
 * @ingroup jls
 * @defgroup jls_cmacro_inc C Macros
 *
 * @brief Commonly used C macros for JLS.
 *
 * @{
 */

/**
 * @def JLS_CPP_GUARD_START
 * @brief Make a C header file safe for a C++ compiler.
 *
 * This guard should be placed at near the top of the header file after
 * the \#if and imports.
 */

/**
 * @def JLS_CPP_GUARD_END
 * @brief Make a C header file safe for a C++ compiler.
 *
 * This guard should be placed at the bottom of the header file just before
 * the \#endif.
 */

#if defined(__cplusplus) && !defined(__CDT_PARSER__)
#define JLS_CPP_GUARD_START extern "C" {
#define JLS_CPP_GUARD_END };
#else
#define JLS_CPP_GUARD_START
#define JLS_CPP_GUARD_END
#endif

/**
 * @brief All functions that are available from the library are marked with
 *      JLS_API.  This platform-specific definition allows DLLs to be
 *      created properly on Windows.
 */
#if defined(WIN32) && defined(JLS_EXPORT)
#define JLS_API __declspec(dllexport)
#elif defined(WIN32) && defined(JLS_IMPORT)
#define JLS_API __declspec(dllimport)
#else
#define JLS_API
#endif


#define JLS_STRUCT_PACKED __attribute__((packed))


#ifdef __GNUC__
#define JLS_USED __attribute__((used))
#define JLS_FORMAT __attribute__((format))
#else
#define JLS_USED
#endif


/** @} */

#endif /* JLS_CMACRO_INC_H__ */

