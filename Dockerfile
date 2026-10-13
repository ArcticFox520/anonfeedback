# syntax=docker/dockerfile:1

FROM debian:bookworm-slim AS build

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        pkg-config \
        libmicrohttpd-dev \
        libsqlite3-dev \
        libcjson-dev \
        uuid-dev \
        libssl-dev \
        curl \
        python3 \
        sqlite3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

COPY CMakeLists.txt ./
COPY src ./src
COPY tests ./tests
COPY migrations ./migrations
COPY static ./static

RUN cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DWARNINGS_AS_ERRORS=ON \
    && cmake --build build -j"$(nproc)" \
    && ctest --test-dir build --output-on-failure

FROM debian:bookworm-slim AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        libmicrohttpd12 \
        libsqlite3-0 \
        libcjson1 \
        libuuid1 \
        libssl3 \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 10001 anonfb \
    && useradd --uid 10001 --gid 10001 --no-create-home --shell /usr/sbin/nologin anonfb

WORKDIR /app

COPY --from=build /src/build/anonfeedback /usr/local/bin/anonfeedback
COPY --from=build --chown=10001:10001 /src/static /app/static
COPY --from=build --chown=10001:10001 /src/migrations /app/migrations

RUN mkdir -p /data && chown 10001:10001 /data && chmod 0700 /data

USER 10001:10001

ENV LISTEN_ADDRESS=0.0.0.0 \
    LISTEN_PORT=8080 \
    DB_PATH=/data/feedback.db \
    STATIC_DIR=/app/static \
    MIGRATIONS_DIR=/app/migrations \
    LOG_LEVEL=info \
    LOG_TARGET=stderr

EXPOSE 8080
VOLUME ["/data"]

HEALTHCHECK --interval=30s --timeout=5s --start-period=5s --retries=3 \
    CMD ["/usr/local/bin/anonfeedback", "--healthcheck"]

ENTRYPOINT ["/usr/local/bin/anonfeedback"]
