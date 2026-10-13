#ifndef ANONFB_UTIL_H
#define ANONFB_UTIL_H

#include <stddef.h>
#include <time.h>

#define UUID_STR_LEN 37
#define ISO8601_UTC_LEN 21

void *xmalloc(size_t size);
void *xcalloc(size_t count, size_t size);
void *xrealloc(void *ptr, size_t size);
char *xstrdup(const char *src);
char *xstrndup(const char *src, size_t max_len);

/* Writes "YYYY-MM-DDTHH:MM:SSZ". */
void iso8601_utc(time_t when, char out[ISO8601_UTC_LEN]);
/* Writes the UTC hour bucket "YYYY-MM-DDTHH:00:00Z". */
void hour_bucket_utc(time_t when, char out[ISO8601_UTC_LEN]);
int hour_bucket_valid(const char *value);

void uuid_v4(char out[UUID_STR_LEN]);
int uuid_valid(const char *value);

char *str_trim_dup(const char *src);
int utf8_valid(const char *data, size_t len);
size_t utf8_count(const char *data, size_t len);

int constant_time_equals(const char *a, const char *b);

const char *category_canonical(const char *value);
int category_valid(const char *value);
int status_valid(const char *value);

int parse_int_range(const char *text, long min, long max, long *out);

char *html_escape_dup(const char *src);
void secure_zero(void *data, size_t len);
void disable_core_dumps(void);

#endif /* ANONFB_UTIL_H */
