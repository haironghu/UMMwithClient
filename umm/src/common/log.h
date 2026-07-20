#ifndef UMM_LOG_H
#define UMM_LOG_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Log levels
 * ======================================================================== */

#define UMM_LOG_DEBUG 0
#define UMM_LOG_INFO  1
#define UMM_LOG_WARN  2
#define UMM_LOG_ERROR 3
#define UMM_LOG_FATAL 4

/* String names for levels */
#define UMM_LOG_LEVEL_NAME_DEBUG "DEBUG"
#define UMM_LOG_LEVEL_NAME_INFO  "INFO"
#define UMM_LOG_LEVEL_NAME_WARN  "WARN"
#define UMM_LOG_LEVEL_NAME_ERROR "ERROR"
#define UMM_LOG_LEVEL_NAME_FATAL "FATAL"

/* ========================================================================
 * Global log-level control
 * ======================================================================== */

/**
 * Set the minimum log level. Messages below this level are suppressed.
 *
 * @param level  One of UMM_LOG_DEBUG .. UMM_LOG_FATAL.
 */
void umm_log_set_level(int level);

/**
 * Get the current minimum log level.
 * @return  Current level.
 */
int umm_log_get_level(void);

/* ========================================================================
 * Backing function (usually invoked via macros below)
 * ======================================================================== */

/**
 * Emit a single log record.
 *
 * @param level  Log level.
 * @param file   Source file name (__FILE__).
 * @param line   Line number (__LINE__).
 * @param fmt    printf-style format string.
 * @param ap     va_list of arguments.
 */
void umm_log(int level, const char *file, int line, const char *fmt, va_list ap);

/**
 * Variadic wrapper around umm_log().
 */
static inline void umm_logf(int level, const char *file, int line,
                            const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    umm_log(level, file, line, fmt, ap);
    va_end(ap);
}

/* ========================================================================
 * Convenience macros – capture file/line automatically
 * ======================================================================== */

#define umm_log_debug(fmt, ...) \
    do { umm_logf(UMM_LOG_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__); } while (0)

#define umm_log_info(fmt, ...) \
    do { umm_logf(UMM_LOG_INFO, __FILE__, __LINE__, fmt, ##__VA_ARGS__); } while (0)

#define umm_log_warn(fmt, ...) \
    do { umm_logf(UMM_LOG_WARN, __FILE__, __LINE__, fmt, ##__VA_ARGS__); } while (0)

#define umm_log_error(fmt, ...) \
    do { umm_logf(UMM_LOG_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__); } while (0)

#define umm_log_fatal(fmt, ...) \
    do { umm_logf(UMM_LOG_FATAL, __FILE__, __LINE__, fmt, ##__VA_ARGS__); } while (0)

#ifdef __cplusplus
}
#endif

#endif /* UMM_LOG_H */
