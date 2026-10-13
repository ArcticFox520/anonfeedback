#include "test.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ratelimit.h"

static void make_v4(struct sockaddr_in *address, const char *text)
{
    memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    CHECK(inet_pton(AF_INET, text, &address->sin_addr) == 1);
}

static void make_v6(struct sockaddr_in6 *address, const char *text)
{
    memset(address, 0, sizeof(*address));
    address->sin6_family = AF_INET6;
    CHECK(inet_pton(AF_INET6, text, &address->sin6_addr) == 1);
}

static void test_quota(void)
{
    ratelimit_t limiter;
    struct sockaddr_in first;
    struct sockaddr_in second;

    CHECK(ratelimit_init(&limiter, 3, 60, "fixed-test-salt") == 0);
    make_v4(&first, "203.0.113.10");
    make_v4(&second, "203.0.113.11");

    CHECK(ratelimit_allow(&limiter, (const struct sockaddr *)&first) == 1);
    CHECK(ratelimit_allow(&limiter, (const struct sockaddr *)&first) == 1);
    CHECK(ratelimit_allow(&limiter, (const struct sockaddr *)&first) == 1);
    CHECK(ratelimit_allow(&limiter, (const struct sockaddr *)&first) == 0);
    CHECK(ratelimit_allow(&limiter, (const struct sockaddr *)&first) == 0);
    CHECK(ratelimit_allow(&limiter, (const struct sockaddr *)&second) == 1);
    CHECK(ratelimit_size(&limiter) == 2);
    ratelimit_destroy(&limiter);
}

static void test_ipv6_and_unknown(void)
{
    ratelimit_t limiter;
    struct sockaddr_in6 peer;

    CHECK(ratelimit_init(&limiter, 1, 60, NULL) == 0);
    make_v6(&peer, "2001:db8::1");
    CHECK(ratelimit_allow(&limiter, (const struct sockaddr *)&peer) == 1);
    CHECK(ratelimit_allow(&limiter, (const struct sockaddr *)&peer) == 0);
    /* Unknown peers fail open instead of being tracked. */
    CHECK(ratelimit_allow(&limiter, NULL) == 1);
    CHECK(ratelimit_size(&limiter) == 1);
    ratelimit_destroy(&limiter);
}

static void test_no_raw_address_is_stored(void)
{
    ratelimit_t limiter;
    struct sockaddr_in peer;
    unsigned char raw[4];
    size_t i;
    size_t j;
    int leaked = 0;
    size_t entries = 0;

    CHECK(ratelimit_init(&limiter, 5, 60, "salt") == 0);
    make_v4(&peer, "198.51.100.77");
    memcpy(raw, &peer.sin_addr, sizeof(raw));
    for (i = 0; i < 5; i++) {
        CHECK(ratelimit_allow(&limiter, (const struct sockaddr *)&peer) == 1);
    }
    for (i = 0; i < RATELIMIT_BUCKETS; i++) {
        const ratelimit_entry_t *entry;

        for (entry = limiter.buckets[i]; entry != NULL; entry = entry->next) {
            entries++;
            for (j = 0; j + sizeof(raw) <= sizeof(entry->key_hash); j++) {
                if (memcmp(entry->key_hash + j, raw, sizeof(raw)) == 0) {
                    leaked = 1;
                }
            }
        }
    }
    CHECK(entries == 1);
    CHECK(leaked == 0);
    ratelimit_destroy(&limiter);
}

static void test_window_expiry(void)
{
    ratelimit_t limiter;
    struct sockaddr_in peer;

    CHECK(ratelimit_init(&limiter, 1, 1, "salt") == 0);
    make_v4(&peer, "192.0.2.5");
    CHECK(ratelimit_allow(&limiter, (const struct sockaddr *)&peer) == 1);
    CHECK(ratelimit_allow(&limiter, (const struct sockaddr *)&peer) == 0);
    (void)sleep(2);
    CHECK(ratelimit_allow(&limiter, (const struct sockaddr *)&peer) == 1);
    ratelimit_sweep(&limiter, time(NULL) + 10);
    CHECK(ratelimit_size(&limiter) == 0);
    ratelimit_destroy(&limiter);
}

static void test_salt_isolation(void)
{
    ratelimit_t first;
    ratelimit_t second;
    ratelimit_t random_salted;
    struct sockaddr_in peer;
    const ratelimit_entry_t *entry_a = NULL;
    const ratelimit_entry_t *entry_b = NULL;
    size_t i;

    CHECK(ratelimit_init(&first, 5, 60, "salt-a") == 0);
    CHECK(ratelimit_init(&second, 5, 60, "salt-b") == 0);
    CHECK(ratelimit_init(&random_salted, 5, 60, NULL) == 0);
    make_v4(&peer, "192.0.2.9");
    CHECK(ratelimit_allow(&first, (const struct sockaddr *)&peer) == 1);
    CHECK(ratelimit_allow(&second, (const struct sockaddr *)&peer) == 1);

    for (i = 0; i < RATELIMIT_BUCKETS; i++) {
        if (first.buckets[i] != NULL) {
            entry_a = first.buckets[i];
        }
        if (second.buckets[i] != NULL) {
            entry_b = second.buckets[i];
        }
    }
    CHECK(entry_a != NULL);
    CHECK(entry_b != NULL);
    if (entry_a != NULL && entry_b != NULL) {
        /* Same peer, different salt: the stored keys must differ. */
        CHECK(memcmp(entry_a->key_hash, entry_b->key_hash, 32) != 0);
    }
    {
        unsigned char zero[RATELIMIT_SALT_LEN] = {0};

        CHECK(memcmp(random_salted.salt, zero, sizeof(zero)) != 0);
    }
    ratelimit_destroy(&first);
    ratelimit_destroy(&second);
    ratelimit_destroy(&random_salted);
}

int main(void)
{
    test_quota();
    test_ipv6_and_unknown();
    test_no_raw_address_is_stored();
    test_window_expiry();
    test_salt_isolation();
    TEST_REPORT();
}
