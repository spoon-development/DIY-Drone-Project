# Multi-arch (amd64 + arm64) for laptop pulls and Raspberry Pi
FROM debian:bookworm-slim AS build

RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
      ca-certificates \
      cmake \
      g++ \
      git \
      libturbojpeg0-dev \
      make \
      pkg-config; \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt video_server.cpp ./
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  && cmake --build build -j "$(nproc)"

# Ubuntu 24.04 matches Pi OS; rpicam-vid is bind-mounted from the host in docker-compose.yml.
FROM ubuntu:24.04

WORKDIR /app

RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends ca-certificates curl libturbojpeg; \
    rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/dronebros-video /usr/local/bin/dronebros-video
COPY profiles ./profiles

ENV VIDEO_PROFILES_DIR=/app/profiles
ENV VIDEO_DEFAULT_PROFILE=uc60
ENV VIDEO_PORT=8765

EXPOSE 8765
HEALTHCHECK --interval=15s --timeout=5s --start-period=15s --retries=3 \
  CMD curl -fsS http://127.0.0.1:8765/health >/dev/null || exit 1

CMD ["/usr/local/bin/dronebros-video"]
