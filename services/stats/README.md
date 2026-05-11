# services/stats

C++ HTTP service on **9101**: `GET /metrics` (JSON), `GET /health`, `GET /` (same JSON as metrics).

Built with **CMake**; runtime image is **Debian bookworm-slim** with a static-ish dynamically linked binary and **curl** for the compose healthcheck.

Expects host paths mounted read-only (see root `docker-compose.yml`).
