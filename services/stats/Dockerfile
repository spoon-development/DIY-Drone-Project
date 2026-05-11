# Multi-arch (amd64 + arm64) for laptop pulls and Raspberry Pi
FROM debian:bookworm-slim AS build

RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
      ca-certificates \
      cmake \
      g++ \
      git \
      make; \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt main.cpp collect.cpp collect.hpp ./
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  && cmake --build build -j "$(nproc)"

FROM debian:bookworm-slim

RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends ca-certificates curl; \
    rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/dronebros-stats /usr/local/bin/dronebros-stats

EXPOSE 9101
ENV STATS_PORT=9101
CMD ["/usr/local/bin/dronebros-stats"]
