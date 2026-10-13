#include "ratelimit.h"

#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include "log.h"
#include "util.h"

static void free_bucket(ratelimit_entry_t *entry)
{
    while (entry != NULL) {
        ratelimit_entry_t *next = entry->next;
        secure_zero(entry, sizeof(*entry));
        free(entry);
        entry = next;
    }
}

int ratelimit_init(ratelimit_t *limiter, unsigned int max_requests,
                   unsigned int window_seconds, const char *salt_secret)
{
    memset(limiter, 0, sizeof(*limiter));
    limiter->max_requests = max_requests;
    limiter->window_seconds = window_seconds;
    limiter->last_sweep = time(NULL);

    if (pthread_mutex_init(&limiter->lock, NULL) != 0) {
        log_error("rate limiter mutex init failed");
        return -1;
    }

    if (salt_secret != NULL && salt_secret[0] != '\0') {
        (void)SHA256((const unsigned char *)salt_secret, strlen(salt_secret), limiter->salt);
        return 0;
    }
    if (RAND_bytes(limiter->salt, (int)sizeof(limiter->salt)) != 1) {
        log_error("rate limit salt generation failed");
        (void)pthread_mutex_destroy(&limiter->lock);
        return -1;
    }
    return 0;
}

void ratelimit_destroy(ratelimit_t *limiter)
{
    size_t i;

    if (limiter == NULL) {
        return;
    }
    (void)pthread_mutex_lock(&limiter->lock);
    for (i = 0; i < RATELIMIT_BUCKETS; i++) {
        free_bucket(limiter->buckets[i]);
        limiter->buckets[i] = NULL;
    }
    limiter->entries = 0;
    (void)pthread_mutex_unlock(&limiter->lock);
    (void)pthread_mutex_destroy(&limiter->lock);
    secure_zero(limiter->salt, sizeof(limiter->salt));
}

void ratelimit_sweep(ratelimit_t *limiter, time_t now)
{
    size_t i;

    for (i = 0; i < RATELIMIT_BUCKETS; i++) {
        ratelimit_entry_t **link = &limiter->buckets[i];

        while (*link != NULL) {
            ratelimit_entry_t *entry = *link;

            if (now - entry->window_start >= (time_t)limiter->window_seconds) {
                *link = entry->next;
                limiter->entries--;
                secure_zero(entry, sizeof(*entry));
                free(entry);
            } else {
                link = &entry->next;
            }
        }
    }
    limiter->last_sweep = now;
}

/* Hashes the peer address into a salted, non-reversible bucket key. */
static int address_key(const ratelimit_t *limiter, const struct sockaddr *address,
                       unsigned char out[32])
{
    unsigned char raw[16];
    size_t raw_len;
    unsigned int digest_len = 0;

    if (address == NULL) {
        return -1;
    }
    if (address->sa_family == AF_INET) {
        const struct sockaddr_in *peer = (const struct sockaddr_in *)address;
        memcpy(raw, &peer->sin_addr, sizeof(peer->sin_addr));
        raw_len = sizeof(peer->sin_addr);
    } else if (address->sa_family == AF_INET6) {
        const struct sockaddr_in6 *peer = (const struct sockaddr_in6 *)address;
        memcpy(raw, &peer->sin6_addr, sizeof(peer->sin6_addr));
        raw_len = sizeof(peer->sin6_addr);
    } else {
        return -1;
    }

    if (HMAC(EVP_sha256(), limiter->salt, (int)sizeof(limiter->salt), raw, raw_len,
             out, &digest_len) == NULL ||
        digest_len != 32) {
        secure_zero(raw, sizeof(raw));
        return -1;
    }
    secure_zero(raw, sizeof(raw));
    return 0;
}

int ratelimit_allow(ratelimit_t *limiter, const struct sockaddr *address)
{
    unsigned char key[32];
    unsigned int index;
    time_t now;
    ratelimit_entry_t *entry;
    int allowed;

    if (address_key(limiter, address, key) != 0) {
        /* Unknown address family: fail open, never log the address. */
        return 1;
    }
    index = (unsigned int)key[0] % RATELIMIT_BUCKETS;
    now = time(NULL);

    (void)pthread_mutex_lock(&limiter->lock);
    if (now - limiter->last_sweep >= (time_t)limiter->window_seconds || limiter->entries > 8192) {
        ratelimit_sweep(limiter, now);
    }
    for (entry = limiter->buckets[index]; entry != NULL; entry = entry->next) {
        if (CRYPTO_memcmp(entry->key_hash, key, sizeof(key)) == 0) {
            break;
        }
    }
    if (entry == NULL) {
        entry = xcalloc(1, sizeof(*entry));
        memcpy(entry->key_hash, key, sizeof(key));
        entry->count = 0;
        entry->window_start = now;
        entry->next = limiter->buckets[index];
        limiter->buckets[index] = entry;
        limiter->entries++;
    }
    if (now - entry->window_start >= (time_t)limiter->window_seconds) {
        entry->count = 0;
        entry->window_start = now;
    }
    entry->count++;
    allowed = entry->count <= limiter->max_requests;
    (void)pthread_mutex_unlock(&limiter->lock);

    secure_zero(key, sizeof(key));
    return allowed;
}

size_t ratelimit_size(ratelimit_t *limiter)
{
    size_t size;

    (void)pthread_mutex_lock(&limiter->lock);
    size = limiter->entries;
    (void)pthread_mutex_unlock(&limiter->lock);
    return size;
}
