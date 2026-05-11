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

WORKDIR /app
COPY CMakeLists.txt hmi_server.cpp ./
COPY static ./static
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  && cmake --build build -j "$(nproc)"

FROM debian:bookworm-slim

RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends ca-certificates util-linux; \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /app/build/dronebros-hmi /usr/local/bin/dronebros-hmi
COPY static ./static

ENV DATA_DIR=/data
ENV HMI_PORT=80
EXPOSE 80
CMD ["/usr/local/bin/dronebros-hmi"]
