#include "config.h"

#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "util.h"

#define ADMIN_TOKEN_MIN_LEN 16

static char *env_string(const char *name, const char *fallback)
{
    const char *value = getenv(name);

    if (value == NULL || value[0] == '\0') {
        return xstrdup(fallback);
    }
    return xstrdup(value);
}

static int env_uint(const char *name, unsigned int fallback, unsigned int min,
                    unsigned int max, unsigned int *out)
{
    const char *value = getenv(name);
    long parsed;

    if (value == NULL || value[0] == '\0') {
        *out = fallback;
        return 0;
    }
    if (!parse_int_range(value, (long)min, (long)max, &parsed)) {
        log_error("invalid %s: expected integer in [%u, %u]", name, min, max);
        return -1;
    }
    *out = (unsigned int)parsed;
    return 0;
}

static int admin_token_is_weak(const char *token)
{
    static const char *const denied[] = {
        "changeme", "change-me", "change_me", "password", "passwd", "secret",
        "admin", "administrator", "token", "test", "example", "your-token-here",
        "anonfeedback", "please-change-me"};
    size_t i;
    size_t len = strlen(token);

    for (i = 0; i < sizeof(denied) / sizeof(denied[0]); i++) {
        if (strcmp(token, denied[i]) == 0) {
            return 1;
        }
    }
    for (i = 1; i < len; i++) {
        if (token[i] != token[0]) {
            return 0;
        }
    }
    return 1;
}

static int load_admin_token(config_t *config)
{
    const char *token = getenv("ADMIN_TOKEN");
    size_t len;

    if (token == NULL || token[0] == '\0') {
        log_error("ADMIN_TOKEN is not set; refusing to start");
        return -1;
    }
    len = strlen(token);
    if (len < ADMIN_TOKEN_MIN_LEN) {
        log_error("ADMIN_TOKEN must be at least %d characters", ADMIN_TOKEN_MIN_LEN);
        return -1;
    }
    if (token[strcspn(token, " \t\r\n")] != '\0') {
        log_error("ADMIN_TOKEN must not contain whitespace");
        return -1;
    }
    if (admin_token_is_weak(token)) {
        log_error("ADMIN_TOKEN is too weak; generate one with: openssl rand -hex 32");
        return -1;
    }
    config->admin_token = xstrdup(token);
    return 0;
}

int config_load(config_t *config)
{
    memset(config, 0, sizeof(*config));

    if (load_admin_token(config) != 0) {
        return -1;
    }
    config->listen_address = env_string("LISTEN_ADDRESS", "0.0.0.0");
    config->db_path = env_string("DB_PATH", "./data/feedback.db");
    config->static_dir = env_string("STATIC_DIR", "./static");
    config->migrations_dir = env_string("MIGRATIONS_DIR", "./migrations");
    config->log_level_name = env_string("LOG_LEVEL", "info");
    config->log_ident = env_string("LOG_IDENT", "anonfeedback");
    config->rate_limit_salt = env_string("RATE_LIMIT_SALT", "");
    {
        const char *target = getenv("LOG_TARGET");

        config->log_to_syslog = (target != NULL && strcmp(target, "syslog") == 0);
    }

    {
        unsigned int max_body = 0;

        if (env_uint("LISTEN_PORT", 8080, 1, 65535, &config->listen_port) != 0 ||
            env_uint("MAX_BODY_BYTES", 65536, 1024, 4194304, &max_body) != 0) {
            return -1;
        }
        config->max_body_bytes = (size_t)max_body;
    }
    if (env_uint("SUBMIT_RATE_MAX", 10, 1, 100000, &config->submit_rate_max) != 0 ||
        env_uint("SUBMIT_RATE_WINDOW_SECONDS", 3600, 1, 86400, &config->submit_rate_window) != 0 ||
        env_uint("ADMIN_RATE_MAX", 600, 1, 1000000, &config->admin_rate_max) != 0 ||
        env_uint("ADMIN_RATE_WINDOW_SECONDS", 3600, 1, 86400, &config->admin_rate_window) != 0 ||
        env_uint("THREAD_POOL_SIZE", 4, 1, 64, &config->thread_pool_size) != 0 ||
        env_uint("CONNECTION_LIMIT", 128, 1, 4096, &config->connection_limit) != 0) {
        return -1;
    }
    return 0;
}

void config_free(config_t *config)
{
    if (config == NULL) {
        return;
    }
    free(config->admin_token);
    free(config->listen_address);
    free(config->db_path);
    free(config->static_dir);
    free(config->migrations_dir);
    free(config->rate_limit_salt);
    free(config->log_level_name);
    free(config->log_ident);
    memset(config, 0, sizeof(*config));
}
