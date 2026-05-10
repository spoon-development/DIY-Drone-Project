# services/hmi

nginx + static assets. **Title in UI: Drone Bros Inc.**

- Published port: **`HMI_HOST_PORT`** → container **80** (see root `docker-compose.yml`).
- **`/api/stats`** → proxies to `stats:9101/metrics` (not exposed on the host).
