#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "db.h"
#include "http.h"
#include "log.h"
#include "ratelimit.h"
#include "util.h"

#ifndef ANONFB_VERSION
#define ANONFB_VERSION "0.0.0"
#endif

#define HEALTH_TIMEOUT_MS 5000

static int connect_with_timeout(const char *address, const char *port, int timeout_ms)
{
    struct addrinfo hints;
    struct addrinfo *candidates = NULL;
    struct addrinfo *candidate;
    int socket_fd = -1;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(address, port, &hints, &candidates) != 0) {
        return -1;
    }
    for (candidate = candidates; candidate != NULL; candidate = candidate->ai_next) {
        struct pollfd waiter;
        int flags;

        socket_fd = socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (socket_fd < 0) {
            continue;
        }
        flags = fcntl(socket_fd, F_GETFL, 0);
        (void)fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK);
        rc = connect(socket_fd, candidate->ai_addr, candidate->ai_addrlen);
        if (rc != 0 && errno != EINPROGRESS) {
            (void)close(socket_fd);
            socket_fd = -1;
            continue;
        }
        waiter.fd = socket_fd;
        waiter.events = POLLOUT;
        waiter.revents = 0;
        if (poll(&waiter, 1, timeout_ms) == 1) {
            int error = 0;
            socklen_t error_len = sizeof(error);

            if (getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &error, &error_len) == 0 && error == 0) {
                (void)fcntl(socket_fd, F_SETFL, flags);
                break;
            }
        }
        (void)close(socket_fd);
        socket_fd = -1;
    }
    freeaddrinfo(candidates);
    return socket_fd;
}

/* Container health probe; talks to the local endpoint only. */
static int run_healthcheck(void)
{
    const char *port = getenv("LISTEN_PORT");
    const char *address = getenv("LISTEN_ADDRESS");
    char request[128];
    char response[256];
    ssize_t received;
    int socket_fd;
    int healthy = 1;

    if (port == NULL || port[0] == '\0') {
        port = "8080";
    }
    if (address == NULL || address[0] == '\0' || strcmp(address, "0.0.0.0") == 0) {
        address = "127.0.0.1";
    } else if (strcmp(address, "::") == 0) {
        address = "::1";
    }

    socket_fd = connect_with_timeout(address, port, HEALTH_TIMEOUT_MS);
    if (socket_fd < 0) {
        return 1;
    }
    (void)snprintf(request, sizeof(request),
                   "GET /api/v1/health HTTP/1.0\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    if (send(socket_fd, request, strlen(request), 0) < 0) {
        healthy = 1;
    } else {
        received = recv(socket_fd, response, sizeof(response) - 1, 0);
        if (received <= 0) {
            healthy = 1;
        } else {
            response[received] = '\0';
            healthy = strstr(response, " 200 ") == NULL;
        }
    }
    (void)close(socket_fd);
    return healthy;
}

int main(int argc, char **argv)
{
    config_t config;
    app_t app;
    db_t db;
    sigset_t signals;
    int signal_number = 0;

    if (argc > 1 && strcmp(argv[1], "--healthcheck") == 0) {
        return run_healthcheck();
    }
    if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        (void)printf("anonfeedback %s\n", ANONFB_VERSION);
        return 0;
    }

    disable_core_dumps();
    (void)umask(0077);
    (void)signal(SIGPIPE, SIG_IGN);

    if (config_load(&config) != 0) {
        config_free(&config);
        return 1;
    }
    log_set_level(log_level_from_name(config.log_level_name));
    if (config.log_to_syslog) {
        (void)log_use_syslog(config.log_ident);
    }

    memset(&app, 0, sizeof(app));
    app.config = &config;
    if (db_open(&db, config.db_path, config.migrations_dir) != 0) {
        log_error("database initialization failed");
        config_free(&config);
        return 1;
    }
    app.db = &db;

    if (ratelimit_init(&app.submit_limiter, config.submit_rate_max, config.submit_rate_window,
                       config.rate_limit_salt) != 0 ||
        ratelimit_init(&app.admin_limiter, config.admin_rate_max, config.admin_rate_window,
                       config.rate_limit_salt) != 0) {
        db_close(&db);
        config_free(&config);
        return 1;
    }

    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    (void)pthread_sigmask(SIG_BLOCK, &signals, NULL);

    if (http_start(&app) != 0) {
        ratelimit_destroy(&app.submit_limiter);
        ratelimit_destroy(&app.admin_limiter);
        db_close(&db);
        config_free(&config);
        return 1;
    }

    while (sigwait(&signals, &signal_number) != 0) {
        /* Retry on interrupted wait. */
    }
    log_info("shutdown signal received");
    http_stop(&app);
    ratelimit_destroy(&app.submit_limiter);
    ratelimit_destroy(&app.admin_limiter);
    db_close(&db);
    config_free(&config);
    return 0;
}
