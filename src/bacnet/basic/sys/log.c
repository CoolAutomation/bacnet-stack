/**
 * @file
 * @brief Debug print function
 * @author Sergey Nazaryev <sergey@coolautomation.com>
 * @date 2025
 * @copyright SPDX-License-Identifier: GPL-2.0-or-later WITH GCC-exception-2.0
 */
#include <stdint.h> /* for standard integer types uint8_t etc. */
#include <stdbool.h> /* for the standard bool type. */
#include <stdarg.h>
#if PRINT_ENABLED
#include <stdio.h> /* Standard I/O */
#include <stdlib.h> /* Standard Library */
#include <errno.h>
#endif
#include "bacnet/basic/sys/log.h"
#if PRINT_WITH_TIMESTAMP
#include "bacnet/datetime.h"
#endif

void default_log_function(const char *module, int level, const char *format, va_list ap);
blog_logFn cb_log_function = default_log_function;

#if PRINT_ENABLED
void blog_set_log_function(blog_logFn func)
{
    if (func)
        cb_log_function = func;
}

/* Makes sense only for default_log_function */
static int log_level = BACNET_LOG_LEVEL_TRACE;
void blog_set_level(int level)
{
    if (log_level < BACNET_LOG_LEVEL_NONE) {
        log_level = BACNET_LOG_LEVEL_NONE;
    } else if (log_level > BACNET_LOG_LEVEL_TRACE) {
        log_level = BACNET_LOG_LEVEL_TRACE;
    } else {
        log_level = level;
    }
}
#endif

void default_log_function(const char *module, int level, const char *format, va_list ap)
{
#if PRINT_ENABLED
    FILE *stream;

#ifdef PRINT_WITH_COLORS
    static const char* const level_colors[] = {
        "", "\x1b[35m", "\x1b[31m", "\x1b[33m", "\x1b[32m", "\x1b[36m", "\x1b[94m"
    };
#endif
    static const char* const level_strings[] = {
        "", "FATAL", "ERROR", " WARN", " INFO", "DEBUG", "TRACE"
    };
#ifdef PRINT_WITH_TIMESTAMP
    BACNET_DATE date;
    BACNET_TIME time;
#endif

    stream = level <= BACNET_LOG_LEVEL_ERR ? stderr : stdout;

#ifdef PRINT_WITH_TIMESTAMP
    datetime_local(&date, &time, NULL, NULL);
    fprintf(stream,
        "%02d:%02d:%02d.%03d ", time.hour,
        time.min, time.sec, time.hundredths * 10);
#endif /* DEBUG_WITH_TIMESTAMP */

#ifdef PRINT_WITH_COLORS
    fprintf(stream, "%s%s\x1b[0m: ", level_colors[level], level_strings[level]);
#else
    fprintf(stream, "%s: ", level_strings[level]);
#endif /* DEBUG_WITH_COLORS */

    /* TODO: check log_level */
    fprintf(stream, "%s: ", module);
    vfprintf(stream, format, ap);
    fprintf(stream, "\n");

    fflush(stream);
#else
    (void)module;
    (void)level;
    (void)format;
    (void)ap;
#endif
}
