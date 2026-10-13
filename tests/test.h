#ifndef ANONFB_TEST_H
#define ANONFB_TEST_H

#include <stdio.h>
#include <string.h>

static int checks_run = 0;
static int checks_failed = 0;

#define CHECK(condition)                                                                     \
    do {                                                                                     \
        checks_run++;                                                                        \
        if (!(condition)) {                                                                  \
            checks_failed++;                                                                 \
            (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);        \
        }                                                                                    \
    } while (0)

#define CHECK_STR_EQ(actual, expected)                                                       \
    do {                                                                                     \
        const char *check_actual = (actual);                                                  \
        const char *check_expected = (expected);                                              \
        checks_run++;                                                                        \
        if (check_actual == NULL || strcmp(check_actual, check_expected) != 0) {              \
            checks_failed++;                                                                 \
            (void)fprintf(stderr, "FAIL %s:%d: %s == \"%s\" (got \"%s\")\n", __FILE__,        \
                          __LINE__, #actual, check_expected,                                  \
                          check_actual != NULL ? check_actual : "(null)");                    \
        }                                                                                    \
    } while (0)

#define CHECK_NULL(value)                                                                    \
    do {                                                                                     \
        checks_run++;                                                                        \
        if ((value) != NULL) {                                                               \
            checks_failed++;                                                                 \
            (void)fprintf(stderr, "FAIL %s:%d: %s is not NULL\n", __FILE__, __LINE__, #value); \
        }                                                                                    \
    } while (0)

#define TEST_REPORT()                                                                        \
    do {                                                                                     \
        (void)printf("%s: %d checks, %d failed\n", __FILE__, checks_run, checks_failed);       \
        return checks_failed == 0 ? 0 : 1;                                                    \
    } while (0)

#endif /* ANONFB_TEST_H */
