# services/hmi

nginx + static assets. **Browser title and heading follow the host hostname** (from stats); fallback label “DIY drone stack”.

- Published port: **`HMI_HOST_PORT`** → container **80** (see root `docker-compose.yml`).
- **`/api/stats`** → proxies to `stats:9101/metrics` (not exposed on the host).
