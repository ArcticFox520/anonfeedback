#include "test.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "util.h"

static void test_trim(void)
{
    char *value = str_trim_dup("  hello world \t\r\n");
    char *blank = str_trim_dup(" \t\n ");
    char *wide = str_trim_dup("\xE3\x80\x80wide\xE3\x80\x80");
    char *inner = str_trim_dup("a b");

    CHECK_STR_EQ(value, "hello world");
    CHECK_STR_EQ(blank, "");
    CHECK_STR_EQ(wide, "wide");
    CHECK_STR_EQ(inner, "a b");
    CHECK_NULL(str_trim_dup(NULL));
    free(value);
    free(blank);
    free(wide);
    free(inner);
}

static void test_utf8(void)
{
    CHECK(utf8_valid("plain ascii", 11));
    CHECK(utf8_valid("\xE4\xB8\xAD\xE6\x96\x87", 6));
    CHECK(utf8_valid("\xF0\x9F\x98\x80", 4));
    CHECK(!utf8_valid("\xE4\xB8", 2));
    CHECK(!utf8_valid("\xC0\x80", 2));
    CHECK(!utf8_valid("\xED\xA0\x80", 3));
    CHECK(!utf8_valid("\xF5\x80\x80\x80", 4));
    CHECK(!utf8_valid("\xE4\x28\xA1", 3));
    CHECK(!utf8_valid(NULL, 3));

    CHECK(utf8_count("abc", 3) == 3);
    CHECK(utf8_count("\xE4\xB8\xAD\xE6\x96\x87", 6) == 2);
    CHECK(utf8_count("a\xE4\xB8\xAD\xF0\x9F\x98\x80", 8) == 3);
    CHECK(utf8_count("", 0) == 0);
}

static void test_time(void)
{
    char iso[ISO8601_UTC_LEN];
    char bucket[ISO8601_UTC_LEN];

    iso8601_utc(0, iso);
    CHECK_STR_EQ(iso, "1970-01-01T00:00:00Z");
    iso8601_utc(1776000000, iso);
    CHECK_STR_EQ(iso, "2026-04-12T13:20:00Z");

    hour_bucket_utc(1776000000, bucket);
    CHECK_STR_EQ(bucket, "2026-04-12T13:00:00Z");
    hour_bucket_utc(1776003599, bucket);
    CHECK_STR_EQ(bucket, "2026-04-12T14:00:00Z");
    hour_bucket_utc(1776003600, bucket);
    CHECK_STR_EQ(bucket, "2026-04-12T14:00:00Z");

    CHECK(hour_bucket_valid("2026-05-01T14:00:00Z"));
    CHECK(!hour_bucket_valid("2026-05-01T14:00:01Z"));
    CHECK(!hour_bucket_valid("2026-05-01T14:30:00Z"));
    CHECK(!hour_bucket_valid("2026-05-01 14:00:00Z"));
    CHECK(!hour_bucket_valid("2026-05-01T14:00:00"));
    CHECK(!hour_bucket_valid(""));
    CHECK(!hour_bucket_valid(NULL));
}

static void test_uuid(void)
{
    char first[UUID_STR_LEN];
    char second[UUID_STR_LEN];

    uuid_v4(first);
    uuid_v4(second);
    CHECK(uuid_valid(first));
    CHECK(uuid_valid(second));
    CHECK(strcmp(first, second) != 0);
    CHECK(first[14] == '4');
    CHECK(!uuid_valid("not-a-uuid"));
    CHECK(!uuid_valid("00000000-0000-4000-8000-00000000000"));
    CHECK(!uuid_valid("00000000000040008000000000000000"));
    CHECK(!uuid_valid(NULL));
}

static void test_constant_time(void)
{
    CHECK(constant_time_equals("token-value", "token-value"));
    CHECK(!constant_time_equals("token-value", "token-valuf"));
    CHECK(!constant_time_equals("token", "token-longer"));
    CHECK(!constant_time_equals("", "x"));
    CHECK(!constant_time_equals(NULL, "x"));
    CHECK(!constant_time_equals("x", NULL));
}

static void test_enums(void)
{
    CHECK_STR_EQ(category_canonical("bug"), "bug");
    CHECK_STR_EQ(category_canonical("suggestion"), "suggestion");
    CHECK_STR_EQ(category_canonical("complaint"), "complaint");
    CHECK_STR_EQ(category_canonical("other"), "other");
    CHECK_NULL(category_canonical("Bug"));
    CHECK_NULL(category_canonical(""));
    CHECK_NULL(category_canonical(NULL));
    CHECK(category_valid(NULL));
    CHECK(category_valid(""));
    CHECK(category_valid("bug"));
    CHECK(!category_valid("nope"));

    CHECK(status_valid("new"));
    CHECK(status_valid("read"));
    CHECK(status_valid("archived"));
    CHECK(!status_valid("deleted"));
    CHECK(!status_valid(NULL));
}

static void test_parse_int(void)
{
    long value = 0;

    CHECK(parse_int_range("42", 0, 100, &value) && value == 42);
    CHECK(parse_int_range("0", 0, 100, &value) && value == 0);
    CHECK(!parse_int_range("101", 0, 100, &value));
    CHECK(!parse_int_range("-1", 0, 100, &value));
    CHECK(!parse_int_range("4x", 0, 100, &value));
    CHECK(!parse_int_range("", 0, 100, &value));
    CHECK(!parse_int_range(NULL, 0, 100, &value));
}

static void test_html_escape(void)
{
    char *escaped = html_escape_dup("<script>alert(\"x\")&'</script>");

    CHECK_STR_EQ(escaped, "&lt;script&gt;alert(&quot;x&quot;)&amp;&#39;&lt;/script&gt;");
    free(escaped);
    escaped = html_escape_dup("plain");
    CHECK_STR_EQ(escaped, "plain");
    free(escaped);
    CHECK_NULL(html_escape_dup(NULL));
}

int main(void)
{
    test_trim();
    test_utf8();
    test_time();
    test_uuid();
    test_constant_time();
    test_enums();
    test_parse_int();
    test_html_escape();
    TEST_REPORT();
}
