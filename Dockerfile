# --- Build stage ---
FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git ca-certificates \
    libssl-dev libsqlite3-dev libcurl4-openssl-dev libpodofo-dev libasio-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j"$(nproc)"

# --- Runtime stage ---
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates libssl3 libsqlite3-0 libcurl4 libpodofo0.9.8 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /src/build/lugbulk_labels_web /app/lugbulk_labels_web
COPY sql/schema.sql /app/sql/schema.sql
COPY templates/ /app/templates/

# Mounted volume: sqlite db + image_cache/ live here, survive redeploys.
VOLUME ["/data"]
ENV LUGBULK_DATA_DIR=/data

EXPOSE 8080
CMD ["/app/lugbulk_labels_web"]
