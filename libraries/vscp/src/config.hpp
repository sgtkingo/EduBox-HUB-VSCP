/**
 * @file config.hpp
 * @brief Compile-time configuration for the VSCP library.
 *
 * Defaults intentionally preserve the configuration symbols used by the
 * upstream VSCP implementation. Applications may override any value with a
 * compiler build flag before this header is included.
 */

#pragma once

#ifndef VSCP_API_VERSION
#define VSCP_API_VERSION "1.5"
#endif

#ifndef MAX_PROTOCOL_REQUEST_SIZE
#define MAX_PROTOCOL_REQUEST_SIZE 1024
#endif

#ifndef PROTOCOL_INIT_TIMEOUT
#define PROTOCOL_INIT_TIMEOUT 500
#endif

#ifndef PROTOCOL_VERBOSE
// 0 = disabled, 1 = transport errors, 2 = errors and complete RX/TX frames.
#define PROTOCOL_VERBOSE 1
#endif

#if !defined(ARDUINO_H_ENV) && !defined(STDIO_H_ENV)
#if defined(ARDUINO)
#define ARDUINO_H_ENV
#else
#define STDIO_H_ENV
#endif
#endif

#if defined(ARDUINO_H_ENV) && defined(STDIO_H_ENV)
#error "Select only one VSCP environment: ARDUINO_H_ENV or STDIO_H_ENV"
#endif

#ifdef STDIO_H_ENV
#ifndef VSCP_ENABLE_IOSTREAM
#define VSCP_ENABLE_IOSTREAM 1
#endif

#ifndef VSCP_ENABLE_STDIO
#define VSCP_ENABLE_STDIO 1
#endif
#endif
