#include "util.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <uuid/uuid.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/sha.h>

static void die_oom(void)
{
    fputs("fatal: out of memory\n", stderr);
    abort();
}

void *xmalloc(size_t size)
{
    void *ptr = malloc(size == 0 ? 1 : size);
    if (ptr == NULL) {
        die_oom();
    }
    return ptr;
}

void *xcalloc(size_t count, size_t size)
{
    void *ptr = calloc(count == 0 ? 1 : count, size == 0 ? 1 : size);
    if (ptr == NULL) {
        die_oom();
    }
    return ptr;
}

void *xrealloc(void *ptr, size_t size)
{
    void *next = realloc(ptr, size == 0 ? 1 : size);
    if (next == NULL) {
        die_oom();
    }
    return next;
}

char *xstrdup(const char *src)
{
    size_t len;
    char *copy;

    if (src == NULL) {
        return NULL;
    }
    len = strlen(src);
    copy = xmalloc(len + 1);
    memcpy(copy, src, len + 1);
    return copy;
}

char *xstrndup(const char *src, size_t max_len)
{
    size_t len = 0;
    char *copy;

    if (src == NULL) {
        return NULL;
    }
    while (len < max_len && src[len] != '\0') {
        len++;
    }
    copy = xmalloc(len + 1);
    memcpy(copy, src, len);
    copy[len] = '\0';
    return copy;
}

void iso8601_utc(time_t when, char out[ISO8601_UTC_LEN])
{
    struct tm parts;

    if (gmtime_r(&when, &parts) == NULL) {
        memcpy(out, "1970-01-01T00:00:00Z", ISO8601_UTC_LEN);
        return;
    }
    (void)strftime(out, ISO8601_UTC_LEN, "%Y-%m-%dT%H:%M:%SZ", &parts);
}

void hour_bucket_utc(time_t when, char out[ISO8601_UTC_LEN])
{
    struct tm parts;

    if (gmtime_r(&when, &parts) == NULL) {
        memcpy(out, "1970-01-01T00:00:00Z", ISO8601_UTC_LEN);
        return;
    }
    (void)strftime(out, ISO8601_UTC_LEN, "%Y-%m-%dT%H:00:00Z", &parts);
}

int hour_bucket_valid(const char *value)
{
    static const size_t digit_positions[] = {0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18};
    size_t i;

    if (value == NULL || strlen(value) != ISO8601_UTC_LEN - 1) {
        return 0;
    }
    for (i = 0; i < sizeof(digit_positions) / sizeof(digit_positions[0]); i++) {
        char c = value[digit_positions[i]];
        if (c < '0' || c > '9') {
            return 0;
        }
    }
    if (value[4] != '-' || value[7] != '-' || value[10] != 'T' || value[13] != ':' ||
        value[16] != ':' || value[19] != 'Z') {
        return 0;
    }
    return value[14] == '0' && value[15] == '0' && value[17] == '0' && value[18] == '0';
}

void uuid_v4(char out[UUID_STR_LEN])
{
    uuid_t raw;
    char text[UUID_STR_LEN];

    uuid_generate_random(raw);
    uuid_unparse_lower(raw, text);
    memcpy(out, text, UUID_STR_LEN);
    secure_zero(raw, sizeof(raw));
}

int uuid_valid(const char *value)
{
    uuid_t raw;
    char lower[UUID_STR_LEN];

    if (value == NULL || strlen(value) != UUID_STR_LEN - 1) {
        return 0;
    }
    if (uuid_parse(value, raw) != 0) {
        return 0;
    }
    uuid_unparse_lower(raw, lower);
    return strcmp(lower, value) == 0;
}

static int ascii_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

/* Strips leading/trailing ASCII space and the UTF-8 ideographic space U+3000. */
char *str_trim_dup(const char *src)
{
    const char *start = src;
    const char *end;
    size_t len;

    if (src == NULL) {
        return NULL;
    }
    while (*start != '\0') {
        if (ascii_space((unsigned char)*start)) {
            start++;
        } else if ((unsigned char)start[0] == 0xE3 && (unsigned char)start[1] == 0x80 &&
                   (unsigned char)start[2] == 0x80) {
            start += 3;
        } else {
            break;
        }
    }
    end = start + strlen(start);
    while (end > start) {
        if (ascii_space((unsigned char)end[-1])) {
            end--;
        } else if (end - start >= 3 && (unsigned char)end[-3] == 0xE3 &&
                   (unsigned char)end[-2] == 0x80 && (unsigned char)end[-1] == 0x80) {
            end -= 3;
        } else {
            break;
        }
    }
    len = (size_t)(end - start);
    return xstrndup(start, len);
}

int utf8_valid(const char *data, size_t len)
{
    size_t i = 0;

    if (data == NULL) {
        return 0;
    }
    while (i < len) {
        unsigned char c = (unsigned char)data[i];
        size_t extra;
        uint32_t code;
        size_t k;

        if (c < 0x80) {
            i++;
            continue;
        }
        if ((c & 0xE0) == 0xC0) {
            extra = 1;
            code = (uint32_t)(c & 0x1F);
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            code = (uint32_t)(c & 0x0F);
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            code = (uint32_t)(c & 0x07);
        } else {
            return 0;
        }
        if (i + extra >= len) {
            return 0;
        }
        for (k = 1; k <= extra; k++) {
            unsigned char cc = (unsigned char)data[i + k];
            if ((cc & 0xC0) != 0x80) {
                return 0;
            }
            code = (code << 6) | (uint32_t)(cc & 0x3F);
        }
        if ((extra == 1 && code < 0x80) || (extra == 2 && code < 0x800) ||
            (extra == 3 && code < 0x10000)) {
            return 0;
        }
        if (code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
            return 0;
        }
        i += extra + 1;
    }
    return 1;
}

size_t utf8_count(const char *data, size_t len)
{
    size_t i = 0;
    size_t count = 0;

    if (data == NULL) {
        return 0;
    }
    while (i < len) {
        unsigned char c = (unsigned char)data[i];
        if (c < 0x80) {
            i++;
        } else if ((c & 0xE0) == 0xC0) {
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            i += 3;
        } else {
            i += 4;
        }
        count++;
    }
    return count;
}

int constant_time_equals(const char *a, const char *b)
{
    unsigned char da[SHA256_DIGEST_LENGTH];
    unsigned char db[SHA256_DIGEST_LENGTH];
    int equal;

    if (a == NULL || b == NULL) {
        return 0;
    }
    (void)SHA256((const unsigned char *)a, strlen(a), da);
    (void)SHA256((const unsigned char *)b, strlen(b), db);
    equal = CRYPTO_memcmp(da, db, sizeof(da)) == 0 ? 1 : 0;
    secure_zero(da, sizeof(da));
    secure_zero(db, sizeof(db));
    return equal;
}

void sha256_hex(const void *data, size_t len, char out[65])
{
    static const char hex[] = "0123456789abcdef";
    unsigned char digest[SHA256_DIGEST_LENGTH];
    size_t i;

    (void)SHA256((const unsigned char *)data, len, digest);
    for (i = 0; i < sizeof(digest); i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0F];
    }
    out[64] = '\0';
    secure_zero(digest, sizeof(digest));
}

const char *category_canonical(const char *value)
{
    static const char *const allowed[] = {"bug", "suggestion", "complaint", "other"};
    size_t i;

    if (value == NULL || value[0] == '\0') {
        return NULL;
    }
    for (i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (strcmp(value, allowed[i]) == 0) {
            return allowed[i];
        }
    }
    return NULL;
}

int category_valid(const char *value)
{
    if (value == NULL || value[0] == '\0') {
        return 1;
    }
    return category_canonical(value) != NULL;
}

int status_valid(const char *value)
{
    if (value == NULL) {
        return 0;
    }
    return strcmp(value, "new") == 0 || strcmp(value, "read") == 0 ||
           strcmp(value, "archived") == 0;
}

int parse_int_range(const char *text, long min, long max, long *out)
{
    char *end = NULL;
    long value;

    if (text == NULL || text[0] == '\0') {
        return 0;
    }
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return 0;
    }
    if (value < min || value > max) {
        return 0;
    }
    *out = value;
    return 1;
}

char *html_escape_dup(const char *src)
{
    size_t len;
    size_t i;
    size_t extra = 0;
    char *out;
    size_t pos = 0;

    if (src == NULL) {
        return NULL;
    }
    len = strlen(src);
    for (i = 0; i < len; i++) {
        switch (src[i]) {
        case '&':
            extra += 4;
            break;
        case '<':
        case '>':
            extra += 3;
            break;
        case '"':
        case '\'':
            extra += 5;
            break;
        default:
            break;
        }
    }
    out = xmalloc(len + extra + 1);
    for (i = 0; i < len; i++) {
        switch (src[i]) {
        case '&':
            memcpy(out + pos, "&amp;", 5);
            pos += 5;
            break;
        case '<':
            memcpy(out + pos, "&lt;", 4);
            pos += 4;
            break;
        case '>':
            memcpy(out + pos, "&gt;", 4);
            pos += 4;
            break;
        case '"':
            memcpy(out + pos, "&quot;", 6);
            pos += 6;
            break;
        case '\'':
            memcpy(out + pos, "&#39;", 5);
            pos += 5;
            break;
        default:
            out[pos++] = src[i];
            break;
        }
    }
    out[pos] = '\0';
    return out;
}

void secure_zero(void *data, size_t len)
{
    if (data != NULL && len > 0) {
        OPENSSL_cleanse(data, len);
    }
}

void disable_core_dumps(void)
{
    struct rlimit limit;

    limit.rlim_cur = 0;
    limit.rlim_max = 0;
    (void)setrlimit(RLIMIT_CORE, &limit);
}
