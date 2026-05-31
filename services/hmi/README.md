# services/hmi

C++ (**cpp-httplib**) serves static assets, proxies **`/api/stats`** to the stats service `…/metrics`, proxies **`/api/video/profiles`** and **`/api/video/stream`** to the video service when it is running (see **`VIDEO_SERVICE_URL`**, default `http://video:8765`; video is optional via Compose profile **`video`** — see root `docker-compose.yml` and **`COMPOSE_PROFILES`**), and persists **`/api/hmi-settings`** as JSON on **`/data`** (bind-mounted as `hmi-data` on the host so backgrounds and network drafts survive reboots).

- Published port: **`HMI_HOST_PORT`** → container **80** (see root `docker-compose.yml`).
- **`DATA_DIR`** (default `/data`) stores `hmi-settings.json` (includes optional **`videoCameraProfile`** for the Video tab).
- **`util-linux`** in the image provides **`nsenter`** for host netplan apply when the stack uses `pid: host` (see compose).
