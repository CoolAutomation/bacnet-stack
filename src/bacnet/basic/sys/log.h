/**
 * @file
 * @brief Logging subsystem
 * @author Sergey Nazaryev <sergey@coolautomation.com>
 * @date 2025
 * @copyright SPDX-License-Identifier: MIT
 */
#ifndef BACNET_SYS_LOG_H
#define BACNET_SYS_LOG_H
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
/* BACnet Stack defines - first */
#include "bacnet/bacdef.h"
#ifndef PRINT_ENABLED
#define PRINT_ENABLED 0
#endif

#ifndef PRINT_WITH_TIMESTAMP
#define PRINT_WITH_TIMESTAMP 1 /*TODO=0*/
#endif

#ifndef PRINT_WITH_COLORS
#define PRINT_WITH_COLORS 1 /*TODO=0*/
#endif

#ifndef LOG_MODULE
#define LOG_MODULE "[N/A]"
#endif

#if PRINT_ENABLED
#include <errno.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

enum {
    BACNET_LOG_LEVEL_NONE = 0,
    BACNET_LOG_LEVEL_FATAL = 1,
    BACNET_LOG_LEVEL_ERR = 2,
    BACNET_LOG_LEVEL_WARN = 3,
    BACNET_LOG_LEVEL_INFO = 4,
    BACNET_LOG_LEVEL_DEBUG = 5,
    BACNET_LOG_LEVEL_TRACE = 6
};

typedef void (*blog_logFn)(const char *module, int level, const char *format, va_list ap);
extern blog_logFn cb_log_function;
#if PRINT_ENABLED
void blog_set_log_function(blog_logFn func);
void blog_set_level(int level);

static __inline__ void log_fatal(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    cb_log_function(LOG_MODULE, BACNET_LOG_LEVEL_FATAL, format, ap);
    va_end(ap);
}

static __inline__ void log_err(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    cb_log_function(LOG_MODULE, BACNET_LOG_LEVEL_ERR, format, ap);
    va_end(ap);
}

static __inline__ void log_warn(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    cb_log_function(LOG_MODULE, BACNET_LOG_LEVEL_WARN, format, ap);
    va_end(ap);
}

static __inline__ void log_info(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    cb_log_function(LOG_MODULE, BACNET_LOG_LEVEL_INFO, format, ap);
    va_end(ap);
}

static __inline__ void log_debug(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    cb_log_function(LOG_MODULE, BACNET_LOG_LEVEL_DEBUG, format, ap);
    va_end(ap);
}

static __inline__ void log_trace(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    cb_log_function(LOG_MODULE, BACNET_LOG_LEVEL_TRACE, format, ap);
    va_end(ap);
}
#else
static __inline__ void log_fatal(const char *format, ...)
{
    (void)format;
}
static __inline__ void log_err(const char *format, ...)
{
    (void)format;
}
static __inline__ void log_warn(const char *format, ...)
{
    (void)format;
}
static __inline__ void log_info(const char *format, ...)
{
    (void)format;
}
static __inline__ void log_debug(const char *format, ...)
{
    (void)format;
}
static __inline__ void log_trace(const char *format, ...)
{
    (void)format;
}
#endif

static __inline__ void log_perror(const char *s)
{
    log_err("%s: %s", s, strerror(errno));
}

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif
