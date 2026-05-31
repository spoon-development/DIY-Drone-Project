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
COPY CMakeLists.txt telemetry_server.cpp ./
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  && cmake --build build -j "$(nproc)"

FROM debian:bookworm-slim

RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends ca-certificates curl; \
    rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/dronebros-telemetry /usr/local/bin/dronebros-telemetry

EXPOSE 9102
ENV TELEMETRY_PORT=9102
ENV MAVLINK_UDP_PORT=14551
ENV TELEMETRY_CONFIG_DIR=/config
VOLUME ["/config"]
CMD ["/usr/local/bin/dronebros-telemetry"]
