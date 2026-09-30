# --- Build stage ---
FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git ca-certificates \
    libssl-dev libsqlite3-dev libcurl4-openssl-dev libpodofo-dev libasio-dev libjpeg-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# Unit tests run as part of the build: a failing test fails the image.
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j"$(nproc)" \
    && ctest --test-dir build --output-on-failure

# --- Runtime stage ---
FROM debian:bookworm-slim

# Set by CI: the release version, or canary-<sha> for the head of master.
ARG VERSION=dev
ENV LUGBULK_VERSION=$VERSION

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates curl libssl3 libsqlite3-0 libcurl4 libpodofo0.9.8 libjpeg62-turbo \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --home-dir /app --shell /usr/sbin/nologin lugbulk \
    && mkdir -p /data && chown lugbulk:lugbulk /data

WORKDIR /app
COPY --from=build /src/build/lugbulk_labels_web /app/lugbulk_labels_web
COPY sql/schema.sql /app/sql/schema.sql
COPY templates/ /app/templates/
COPY data/ /app/data/
COPY docker/entrypoint.sh /app/entrypoint.sh

# Mounted volume: sqlite db + image_cache/ live here, survive redeploys.
VOLUME ["/data"]
ENV LUGBULK_DATA_DIR=/data

EXPOSE 8080
HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
    CMD curl -fsS http://localhost:8080/healthz || exit 1
# Starts as root only to fix /data ownership, then drops to `lugbulk`.
ENTRYPOINT ["/app/entrypoint.sh"]
CMD ["/app/lugbulk_labels_web"]
