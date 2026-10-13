#ifndef ANONFB_CONFIG_H
#define ANONFB_CONFIG_H

#include <stddef.h>

typedef struct {
    char *admin_token;
    char *listen_address;
    unsigned int listen_port;
    char *db_path;
    char *static_dir;
    char *migrations_dir;
    size_t max_body_bytes;
    unsigned int submit_rate_max;
    unsigned int submit_rate_window;
    unsigned int admin_rate_max;
    unsigned int admin_rate_window;
    char *rate_limit_salt;
    char *log_level_name;
    char *log_ident;
    int log_to_syslog;
    unsigned int thread_pool_size;
    unsigned int connection_limit;
} config_t;

/* Returns 0 on success, -1 on invalid configuration. */
int config_load(config_t *config);
void config_free(config_t *config);

#endif /* ANONFB_CONFIG_H */
