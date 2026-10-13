#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#include "util.h"

static log_level_t current_level = LOG_LEVEL_INFO;
static int use_syslog = 0;

static const char *level_name(log_level_t level)
{
    switch (level) {
    case LOG_LEVEL_ERROR:
        return "error";
    case LOG_LEVEL_WARN:
        return "warn";
    case LOG_LEVEL_INFO:
        return "info";
    case LOG_LEVEL_DEBUG:
        return "debug";
    default:
        return "info";
    }
}

static int syslog_priority(log_level_t level)
{
    switch (level) {
    case LOG_LEVEL_ERROR:
        return LOG_ERR;
    case LOG_LEVEL_WARN:
        return LOG_WARNING;
    case LOG_LEVEL_INFO:
        return LOG_INFO;
    default:
        return LOG_DEBUG;
    }
}

log_level_t log_level_from_name(const char *name)
{
    if (name == NULL) {
        return LOG_LEVEL_INFO;
    }
    if (strcmp(name, "error") == 0) {
        return LOG_LEVEL_ERROR;
    }
    if (strcmp(name, "warn") == 0) {
        return LOG_LEVEL_WARN;
    }
    if (strcmp(name, "debug") == 0) {
        return LOG_LEVEL_DEBUG;
    }
    return LOG_LEVEL_INFO;
}

void log_set_level(log_level_t level)
{
    current_level = level;
}

int log_use_syslog(const char *ident)
{
    openlog(ident, LOG_PID | LOG_NDELAY, LOG_DAEMON);
    use_syslog = 1;
    return 0;
}

void log_write(log_level_t level, const char *fmt, ...)
{
    char message[1024];
    va_list args;

    if (level > current_level) {
        return;
    }
    va_start(args, fmt);
    (void)vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    if (use_syslog) {
        syslog(syslog_priority(level), "%s", message);
        return;
    }

    {
        char stamp[ISO8601_UTC_LEN];
        iso8601_utc(time(NULL), stamp);
        (void)fprintf(stderr, "%s %-5s %s\n", stamp, level_name(level), message);
        (void)fflush(stderr);
    }
}
