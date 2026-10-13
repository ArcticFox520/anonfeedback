#ifndef ANONFB_RATELIMIT_H
#define ANONFB_RATELIMIT_H

#include <pthread.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>

#define RATELIMIT_SALT_LEN 32
#define RATELIMIT_BUCKETS 256

typedef struct ratelimit_entry {
    unsigned char key_hash[32];
    unsigned int count;
    time_t window_start;
    struct ratelimit_entry *next;
} ratelimit_entry_t;

/*
 * In-memory only: client addresses are keyed by a salted HMAC and never stored,
 * logged or persisted in their original form.
 */
typedef struct {
    ratelimit_entry_t *buckets[RATELIMIT_BUCKETS];
    size_t entries;
    unsigned int max_requests;
    unsigned int window_seconds;
    unsigned char salt[RATELIMIT_SALT_LEN];
    pthread_mutex_t lock;
    time_t last_sweep;
} ratelimit_t;

/* A NULL or empty salt_secret makes the limiter generate a random salt. */
int ratelimit_init(ratelimit_t *limiter, unsigned int max_requests,
                   unsigned int window_seconds, const char *salt_secret);
void ratelimit_destroy(ratelimit_t *limiter);
/* Returns 1 when the request is allowed, 0 when the client is over quota. */
int ratelimit_allow(ratelimit_t *limiter, const struct sockaddr *address);
size_t ratelimit_size(ratelimit_t *limiter);
void ratelimit_sweep(ratelimit_t *limiter, time_t now);

#endif /* ANONFB_RATELIMIT_H */
