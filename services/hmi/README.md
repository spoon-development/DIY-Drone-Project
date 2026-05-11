# services/hmi

Python (**aiohttp**) serves static assets, proxies **`/api/stats`** to `stats:9101/metrics`, and persists **`/api/hmi-settings`** as JSON on **`/data`** (bind-mounted as `hmi-data` on the host so backgrounds and network drafts survive reboots).

- Published port: **`HMI_HOST_PORT`** → container **80** (see root `docker-compose.yml`).
- **`DATA_DIR`** (default `/data`) stores `hmi-settings.json`.
