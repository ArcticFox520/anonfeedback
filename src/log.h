#ifndef ANONFB_LOG_H
#define ANONFB_LOG_H

typedef enum {
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_WARN = 1,
    LOG_LEVEL_INFO = 2,
    LOG_LEVEL_DEBUG = 3
} log_level_t;

/* Never pass request bodies, opinions, client addresses or credentials here. */
void log_write(log_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

void log_set_level(log_level_t level);
log_level_t log_level_from_name(const char *name);
int log_use_syslog(const char *ident);

#define log_error(...) log_write(LOG_LEVEL_ERROR, __VA_ARGS__)
#define log_warn(...) log_write(LOG_LEVEL_WARN, __VA_ARGS__)
#define log_info(...) log_write(LOG_LEVEL_INFO, __VA_ARGS__)
#define log_debug(...) log_write(LOG_LEVEL_DEBUG, __VA_ARGS__)

#endif /* ANONFB_LOG_H */
