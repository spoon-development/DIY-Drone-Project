# services/hmi

**nginx** serves a static page and reverse-proxies:

- `/stream/` → `video-stream:8080/cam.mjpg` (MJPEG)
- `/api/stats` → `stats:9101/metrics` (JSON)

Published on the host as **http://&lt;pi-ip&gt;:8080/** via `docker-compose.yml`.
