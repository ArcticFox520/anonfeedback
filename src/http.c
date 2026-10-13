#include "http.h"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "handlers.h"
#include "log.h"
#include "util.h"

#define STATIC_FILE_MAX_BYTES (4u * 1024u * 1024u)
#define REQUEST_READ_TIMEOUT_SECONDS 30
#define CONNECTION_MEMORY_SLACK (64u * 1024u)

static const char *const SECURITY_HEADERS[][2] = {
    {"X-Content-Type-Options", "nosniff"},
    {"Referrer-Policy", "no-referrer"},
    {"X-Frame-Options", "DENY"},
    {"Permissions-Policy", "geolocation=(), camera=(), microphone=(), payment=()"},
    {"Content-Security-Policy",
     "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; "
     "connect-src 'self'; form-action 'self'; frame-ancestors 'none'; base-uri 'none'"},
};

typedef struct {
    char *body;
    size_t body_len;
    size_t body_capacity;
    int too_large;
    struct sockaddr_storage peer;
    socklen_t peer_len;
    int peer_valid;
} connection_ctx_t;

/* Microhttpd messages may contain peer addresses, so they are dropped. */
static void mhd_log_sink(void *cls, const char *format, va_list args)
{
    (void)cls;
    (void)format;
    (void)args;
}

static void add_common_headers(struct MHD_Response *response)
{
    size_t i;

    for (i = 0; i < sizeof(SECURITY_HEADERS) / sizeof(SECURITY_HEADERS[0]); i++) {
        (void)MHD_add_response_header(response, SECURITY_HEADERS[i][0], SECURITY_HEADERS[i][1]);
    }
}

enum MHD_Result http_reply_json(request_t *req, unsigned int status, cJSON *body)
{
    char *text = cJSON_PrintUnformatted(body);
    struct MHD_Response *response;

    cJSON_Delete(body);
    if (text == NULL) {
        return http_reply_error(req, 500, "INTERNAL_ERROR", "服务器内部错误");
    }
    response = MHD_create_response_from_buffer(strlen(text), text, MHD_RESPMEM_MUST_FREE);
    if (response == NULL) {
        free(text);
        return MHD_NO;
    }
    (void)MHD_add_response_header(response, MHD_HTTP_HEADER_CONTENT_TYPE, "application/json");
    (void)MHD_add_response_header(response, MHD_HTTP_HEADER_CACHE_CONTROL, "no-store");
    add_common_headers(response);
    req->status = status;
    {
        enum MHD_Result result = MHD_queue_response(req->conn, status, response);
        MHD_destroy_response(response);
        return result;
    }
}

enum MHD_Result http_reply_error(request_t *req, unsigned int status, const char *code,
                                 const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *error = cJSON_CreateObject();

    if (root == NULL || error == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(error);
        return MHD_NO;
    }
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    cJSON_AddItemToObject(root, "error", error);
    return http_reply_json(req, status, root);
}

enum MHD_Result http_reply_empty(request_t *req, unsigned int status)
{
    struct MHD_Response *response = MHD_create_response_from_buffer(0, NULL, MHD_RESPMEM_PERSISTENT);
    enum MHD_Result result;

    if (response == NULL) {
        return MHD_NO;
    }
    (void)MHD_add_response_header(response, MHD_HTTP_HEADER_CACHE_CONTROL, "no-store");
    add_common_headers(response);
    req->status = status;
    result = MHD_queue_response(req->conn, status, response);
    MHD_destroy_response(response);
    return result;
}

int http_body_is_json(request_t *req)
{
    const char *header = MHD_lookup_connection_value(req->conn, MHD_HEADER_KIND,
                                                     MHD_HTTP_HEADER_CONTENT_TYPE);
    size_t media_len;

    if (header == NULL) {
        return 0;
    }
    media_len = strcspn(header, ";");
    while (media_len > 0 && (header[media_len - 1] == ' ' || header[media_len - 1] == '\t')) {
        media_len--;
    }
    return media_len == strlen("application/json") &&
           strncasecmp(header, "application/json", media_len) == 0;
}

const char *http_bearer_token(request_t *req)
{
    const char *header = MHD_lookup_connection_value(req->conn, MHD_HEADER_KIND, "Authorization");
    static const char scheme[] = "Bearer ";
    size_t i;
    size_t offset = sizeof(scheme) - 1;

    if (header == NULL) {
        return NULL;
    }
    for (i = 0; i < offset; i++) {
        char c = header[i];
        char expected = scheme[i];

        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (expected >= 'A' && expected <= 'Z') {
            expected = (char)(expected - 'A' + 'a');
        }
        if (c != expected) {
            return NULL;
        }
    }
    return header[offset] != '\0' ? header + offset : NULL;
}

static const char *mime_type_for(const char *path)
{
    const char *extension = strrchr(path, '.');

    if (extension == NULL) {
        return "application/octet-stream";
    }
    if (strcmp(extension, ".html") == 0) {
        return "text/html; charset=utf-8";
    }
    if (strcmp(extension, ".css") == 0) {
        return "text/css; charset=utf-8";
    }
    if (strcmp(extension, ".js") == 0) {
        return "application/javascript; charset=utf-8";
    }
    if (strcmp(extension, ".json") == 0) {
        return "application/json";
    }
    if (strcmp(extension, ".svg") == 0) {
        return "image/svg+xml";
    }
    if (strcmp(extension, ".ico") == 0) {
        return "image/x-icon";
    }
    if (strcmp(extension, ".png") == 0) {
        return "image/png";
    }
    return "application/octet-stream";
}

/* Only relative paths without traversal segments are accepted. */
static int relative_path_is_safe(const char *path)
{
    const char *segment = path;

    if (path[0] == '\0' || path[0] == '/') {
        return 0;
    }
    while (*path != '\0') {
        unsigned char c = (unsigned char)*path;

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-' || c == '/')) {
            return 0;
        }
        path++;
    }
    while (segment != NULL) {
        const char *separator = strchr(segment, '/');
        size_t len = separator != NULL ? (size_t)(separator - segment) : strlen(segment);

        if (len == 0 || (len == 1 && segment[0] == '.') ||
            (len == 2 && segment[0] == '.' && segment[1] == '.')) {
            return 0;
        }
        segment = separator != NULL ? separator + 1 : NULL;
    }
    return 1;
}

static enum MHD_Result reply_plain(request_t *req, unsigned int status, const char *message)
{
    struct MHD_Response *response =
        MHD_create_response_from_buffer(strlen(message), (void *)message, MHD_RESPMEM_PERSISTENT);
    enum MHD_Result result;

    if (response == NULL) {
        return MHD_NO;
    }
    (void)MHD_add_response_header(response, MHD_HTTP_HEADER_CONTENT_TYPE, "text/plain; charset=utf-8");
    (void)MHD_add_response_header(response, MHD_HTTP_HEADER_CACHE_CONTROL, "no-store");
    add_common_headers(response);
    req->status = status;
    result = MHD_queue_response(req->conn, status, response);
    MHD_destroy_response(response);
    return result;
}

static const char *static_target_for(const char *url)
{
    if (strcmp(url, "/") == 0 || strcmp(url, "/index.html") == 0) {
        return "index.html";
    }
    if (strcmp(url, "/admin") == 0 || strcmp(url, "/admin/") == 0 ||
        strcmp(url, "/admin.html") == 0) {
        return "admin.html";
    }
    if (strncmp(url, "/assets/", 8) == 0) {
        return url + 1;
    }
    if (strcmp(url, "/favicon.ico") == 0) {
        return "favicon.ico";
    }
    return NULL;
}

static enum MHD_Result serve_static(request_t *req)
{
    const char *target = static_target_for(req->path);
    char full_path[4096];
    struct stat info;
    int fd;
    char *buffer;
    ssize_t read_bytes;
    struct MHD_Response *response;
    enum MHD_Result result;

    if (target == NULL) {
        return reply_plain(req, 404, "not found\n");
    }
    if (!relative_path_is_safe(target)) {
        return reply_plain(req, 404, "not found\n");
    }
    if ((size_t)snprintf(full_path, sizeof(full_path), "%s/%s", req->app->config->static_dir,
                         target) >= sizeof(full_path)) {
        return reply_plain(req, 404, "not found\n");
    }
    fd = open(full_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return reply_plain(req, 404, "not found\n");
    }
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
        (unsigned long long)info.st_size > STATIC_FILE_MAX_BYTES) {
        (void)close(fd);
        return reply_plain(req, 404, "not found\n");
    }
    buffer = xmalloc((size_t)info.st_size);
    read_bytes = read(fd, buffer, (size_t)info.st_size);
    (void)close(fd);
    if (read_bytes != (ssize_t)info.st_size) {
        free(buffer);
        return reply_plain(req, 404, "not found\n");
    }
    response = MHD_create_response_from_buffer((size_t)info.st_size, buffer, MHD_RESPMEM_MUST_FREE);
    if (response == NULL) {
        free(buffer);
        return MHD_NO;
    }
    (void)MHD_add_response_header(response, MHD_HTTP_HEADER_CONTENT_TYPE, mime_type_for(full_path));
    (void)MHD_add_response_header(response, MHD_HTTP_HEADER_CACHE_CONTROL,
                                  strncmp(target, "assets/", 7) == 0 ? "public, max-age=600"
                                                                    : "no-store");
    add_common_headers(response);
    req->status = 200;
    result = MHD_queue_response(req->conn, MHD_HTTP_OK, response);
    MHD_destroy_response(response);
    return result;
}

static int path_equals(const char *path, const char *expected)
{
    return strcmp(path, expected) == 0;
}

static int path_has_prefix(const char *path, const char *prefix)
{
    return strncmp(path, prefix, strlen(prefix)) == 0;
}

static enum MHD_Result route_api(request_t *req)
{
    if (path_equals(req->path, "/api/v1/health")) {
        if (!path_equals(req->method, MHD_HTTP_METHOD_GET)) {
            return http_reply_error(req, 405, "METHOD_NOT_ALLOWED", "不支持的请求方法");
        }
        return handle_health(req);
    }
    if (path_equals(req->path, "/api/v1/meta")) {
        if (!path_equals(req->method, MHD_HTTP_METHOD_GET)) {
            return http_reply_error(req, 405, "METHOD_NOT_ALLOWED", "不支持的请求方法");
        }
        return handle_meta(req);
    }
    if (path_equals(req->path, "/api/v1/feedback")) {
        if (!path_equals(req->method, MHD_HTTP_METHOD_POST)) {
            return http_reply_error(req, 405, "METHOD_NOT_ALLOWED", "不支持的请求方法");
        }
        return handle_submit_feedback(req);
    }
    if (path_equals(req->path, "/api/v1/admin/feedback")) {
        if (!path_equals(req->method, MHD_HTTP_METHOD_GET)) {
            return http_reply_error(req, 405, "METHOD_NOT_ALLOWED", "不支持的请求方法");
        }
        return handle_admin_list(req);
    }
    if (path_has_prefix(req->path, "/api/v1/admin/feedback/")) {
        const char *id = req->path + strlen("/api/v1/admin/feedback/");

        if (strchr(id, '/') != NULL || !uuid_valid(id)) {
            return http_reply_error(req, 404, "NOT_FOUND", "意见不存在");
        }
        if (path_equals(req->method, MHD_HTTP_METHOD_GET)) {
            return handle_admin_get(req, id);
        }
        if (path_equals(req->method, MHD_HTTP_METHOD_PATCH)) {
            return handle_admin_update(req, id);
        }
        if (path_equals(req->method, MHD_HTTP_METHOD_DELETE)) {
            return handle_admin_delete(req, id);
        }
        return http_reply_error(req, 405, "METHOD_NOT_ALLOWED", "不支持的请求方法");
    }
    return http_reply_error(req, 404, "NOT_FOUND", "接口不存在");
}

/* Single gate in front of every administrative endpoint. */
static enum MHD_Result route_admin(request_t *req)
{
    const char *token;

    if (!ratelimit_allow(&req->app->admin_limiter, req->peer)) {
        return http_reply_error(req, 429, "RATE_LIMITED", "请求过于频繁，请稍后再试");
    }
    token = http_bearer_token(req);
    if (token == NULL || !constant_time_equals(token, req->app->config->admin_token)) {
        return http_reply_error(req, 401, "UNAUTHORIZED", "管理令牌无效");
    }
    return route_api(req);
}

static enum MHD_Result route_request(request_t *req)
{
    if (path_has_prefix(req->path, "/api/v1/admin/")) {
        return route_admin(req);
    }
    if (path_has_prefix(req->path, "/api/v1/")) {
        return route_api(req);
    }
    if (path_equals(req->method, MHD_HTTP_METHOD_GET) ||
        path_equals(req->method, MHD_HTTP_METHOD_HEAD)) {
        return serve_static(req);
    }
    return reply_plain(req, 405, "method not allowed\n");
}

static connection_ctx_t *connection_ctx_create(struct MHD_Connection *conn)
{
    connection_ctx_t *ctx = xcalloc(1, sizeof(*ctx));
    const union MHD_ConnectionInfo *info =
        MHD_get_connection_info(conn, MHD_CONNECTION_INFO_CLIENT_ADDRESS);

    if (info != NULL && info->client_addr != NULL) {
        socklen_t len = info->client_addr->sa_family == AF_INET6
                            ? (socklen_t)sizeof(struct sockaddr_in6)
                            : (socklen_t)sizeof(struct sockaddr_in);
        if (info->client_addr->sa_family == AF_INET || info->client_addr->sa_family == AF_INET6) {
            memcpy(&ctx->peer, info->client_addr, len);
            ctx->peer_len = len;
            ctx->peer_valid = 1;
        }
    }
    return ctx;
}

static void connection_ctx_free(void *cls)
{
    connection_ctx_t *ctx = cls;

    if (ctx == NULL) {
        return;
    }
    if (ctx->body != NULL) {
        secure_zero(ctx->body, ctx->body_capacity);
        free(ctx->body);
    }
    secure_zero(ctx, sizeof(*ctx));
    free(ctx);
}

static void request_completed(void *cls, struct MHD_Connection *conn, void **con_cls,
                              enum MHD_RequestTerminationCode code)
{
    (void)cls;
    (void)conn;
    (void)code;
    connection_ctx_free(*con_cls);
    *con_cls = NULL;
}

static int ctx_append_body(connection_ctx_t *ctx, const char *data, size_t len, size_t limit)
{
    if (ctx->body_len + len > limit) {
        ctx->too_large = 1;
        return -1;
    }
    if (ctx->body_len + len + 1 > ctx->body_capacity) {
        size_t capacity = ctx->body_capacity == 0 ? 1024 : ctx->body_capacity;

        while (capacity < ctx->body_len + len + 1) {
            capacity *= 2;
        }
        ctx->body = xrealloc(ctx->body, capacity);
        ctx->body_capacity = capacity;
    }
    memcpy(ctx->body + ctx->body_len, data, len);
    ctx->body_len += len;
    ctx->body[ctx->body_len] = '\0';
    return 0;
}

static enum MHD_Result access_handler(void *cls, struct MHD_Connection *conn, const char *url,
                                      const char *method, const char *version,
                                      const char *upload_data, size_t *upload_data_size,
                                      void **con_cls)
{
    app_t *app = cls;
    connection_ctx_t *ctx;
    request_t req;
    enum MHD_Result result;
    int body_method;

    (void)version;

    if (*con_cls == NULL) {
        *con_cls = connection_ctx_create(conn);
        /* First call only initializes per-request state. */
        return MHD_YES;
    }
    ctx = *con_cls;
    if (ctx == NULL) {
        return MHD_NO;
    }

    body_method = strcmp(method, MHD_HTTP_METHOD_POST) == 0 ||
                  strcmp(method, MHD_HTTP_METHOD_PATCH) == 0 ||
                  strcmp(method, MHD_HTTP_METHOD_PUT) == 0;

    if (body_method && *upload_data_size != 0) {
        (void)ctx_append_body(ctx, upload_data, *upload_data_size, app->config->max_body_bytes);
        *upload_data_size = 0;
        return MHD_YES;
    }

    memset(&req, 0, sizeof(req));
    req.app = app;
    req.conn = conn;
    req.method = method;
    req.path = url;
    req.body = ctx->body != NULL ? ctx->body : "";
    req.body_len = ctx->body_len;
    req.peer = ctx->peer_valid ? (const struct sockaddr *)&ctx->peer : NULL;

    if (ctx->too_large) {
        result = http_reply_error(&req, 413, "PAYLOAD_TOO_LARGE", "请求体过大");
    } else {
        result = route_request(&req);
    }

    /* Request logging keeps method, path and status; never headers or payloads. */
    log_info("%s %s -> %u", method, url, req.status);
    return result;
}

static int resolve_listen_address(app_t *app)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    char port[8];
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
    (void)snprintf(port, sizeof(port), "%u", app->config->listen_port);

    rc = getaddrinfo(strcmp(app->config->listen_address, "0.0.0.0") == 0
                         ? NULL
                         : app->config->listen_address,
                     port, &hints, &result);
    if (rc != 0 || result == NULL) {
        log_error("cannot resolve listen address");
        return -1;
    }
    if (result->ai_addrlen > sizeof(app->listen_address)) {
        freeaddrinfo(result);
        log_error("listen address is too large");
        return -1;
    }
    memcpy(&app->listen_address, result->ai_addr, result->ai_addrlen);
    app->listen_address_len = (socklen_t)result->ai_addrlen;
    freeaddrinfo(result);
    return 0;
}

int http_start(app_t *app)
{
    if (resolve_listen_address(app) != 0) {
        return -1;
    }
    app->daemon = MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD, (uint16_t)app->config->listen_port, NULL, NULL,
        &access_handler, app,
        MHD_OPTION_EXTERNAL_LOGGER, &mhd_log_sink, NULL,
        MHD_OPTION_SOCK_ADDR, (const struct sockaddr *)&app->listen_address,
        MHD_OPTION_THREAD_POOL_SIZE, app->config->thread_pool_size,
        MHD_OPTION_CONNECTION_LIMIT, app->config->connection_limit,
        MHD_OPTION_CONNECTION_TIMEOUT, REQUEST_READ_TIMEOUT_SECONDS,
        MHD_OPTION_CONNECTION_MEMORY_LIMIT, app->config->max_body_bytes + CONNECTION_MEMORY_SLACK,
        MHD_OPTION_NOTIFY_COMPLETED, &request_completed, NULL,
        MHD_OPTION_END);
    if (app->daemon == NULL) {
        log_error("cannot start http daemon");
        return -1;
    }
    log_info("listening on %s:%u", app->config->listen_address, app->config->listen_port);
    return 0;
}

void http_stop(app_t *app)
{
    if (app->daemon != NULL) {
        MHD_stop_daemon(app->daemon);
        app->daemon = NULL;
    }
}
