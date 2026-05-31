# mavlink-router bridges FC UART ↔ UDP for the telemetry service.
FROM debian:bookworm-slim AS build

RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
      ca-certificates \
      git \
      meson \
      ninja-build \
      pkg-config \
      g++ \
      python3; \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
RUN git clone --depth 1 --branch master https://github.com/mavlink-router/mavlink-router.git . \
  && git submodule update --init --recursive \
  && meson setup build -Dsystemdsystemunitdir=no \
  && ninja -C build \
  && ninja -C build install

FROM debian:bookworm-slim

RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends ca-certificates; \
    rm -rf /var/lib/apt/lists/*

COPY --from=build /usr/bin/mavlink-routerd /usr/local/bin/mavlink-routerd
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

ENV MAVLINK_ROUTER_CONFIG_DIR=/config
VOLUME ["/config"]

ENTRYPOINT ["/entrypoint.sh"]
