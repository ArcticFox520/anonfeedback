#ifndef ANONFB_HTTP_H
#define ANONFB_HTTP_H

#include <microhttpd.h>
#include <stddef.h>
#include <sys/socket.h>

#include <cjson/cJSON.h>

#include "config.h"
#include "db.h"
#include "ratelimit.h"

typedef struct {
    config_t *config;
    db_t *db;
    ratelimit_t submit_limiter;
    ratelimit_t admin_limiter;
    struct MHD_Daemon *daemon;
    struct sockaddr_storage listen_address;
    socklen_t listen_address_len;
} app_t;

typedef struct {
    app_t *app;
    struct MHD_Connection *conn;
    const char *method;
    const char *path;
    const char *body;
    size_t body_len;
    const struct sockaddr *peer;
    /* Address is used for the in-memory rate limiter only. */
    unsigned int status;
} request_t;

/* Starts the daemon, returns 0 on success. */
int http_start(app_t *app);
void http_stop(app_t *app);

enum MHD_Result http_reply_json(request_t *req, unsigned int status, cJSON *body);
enum MHD_Result http_reply_error(request_t *req, unsigned int status, const char *code,
                                 const char *message);
enum MHD_Result http_reply_empty(request_t *req, unsigned int status);
int http_body_is_json(request_t *req);
/* Returns the bearer token when the Authorization header is well formed. */
const char *http_bearer_token(request_t *req);

#endif /* ANONFB_HTTP_H */
