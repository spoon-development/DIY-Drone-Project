# Multi-arch (amd64 + arm64) for laptop pulls and Raspberry Pi
FROM debian:bookworm-slim

RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends python3 ca-certificates; \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY server.py /app/server.py

EXPOSE 9101
CMD ["python3", "-u", "/app/server.py"]
