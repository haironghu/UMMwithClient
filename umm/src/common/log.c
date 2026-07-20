#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

/* ========================================================================
 * Globals
 * ======================================================================== */

static int g_log_level = UMM_LOG_DEBUG;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ========================================================================
 * Internal helpers
 * ======================================================================== */

static const char* level_name(int level)
{
    switch (level) {
    case UMM_LOG_DEBUG: return UMM_LOG_LEVEL_NAME_DEBUG;
    case UMM_LOG_INFO:  return UMM_LOG_LEVEL_NAME_INFO;
    case UMM_LOG_WARN:  return UMM_LOG_LEVEL_NAME_WARN;
    case UMM_LOG_ERROR: return UMM_LOG_LEVEL_NAME_ERROR;
    case UMM_LOG_FATAL: return UMM_LOG_LEVEL_NAME_FATAL;
    default:            return "???";
    }
}

/* ========================================================================
 * Public API
 * ======================================================================== */

void umm_log_set_level(int level)
{
    g_log_level = level;
}

int umm_log_get_level(void)
{
    return g_log_level;
}

void umm_log(int level, const char *file, int line, const char *fmt, va_list ap)
{
    if (level < g_log_level)
        return;

    /* Strip leading directory components for readability */
    const char *basename = strrchr(file, '/');
    if (!basename)
        basename = strrchr(file, '\\');
    if (basename)
        basename++;
    else
        basename = file;

    pthread_mutex_lock(&g_log_mutex);

    /* Emit: [LEVEL] file:line - message */
    fprintf(stderr, "[%s] %s:%d - ", level_name(level), basename, line);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    fflush(stderr);

    pthread_mutex_unlock(&g_log_mutex);

    if (level == UMM_LOG_FATAL)
        abort();
}
