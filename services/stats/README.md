# services/stats

Read-only JSON metrics from the **host** Raspberry Pi by mounting `/proc`, `/sys`, and `/` (read-only at `/host_root`).

- `GET /metrics` — temperature, load average, memory, uptime, disk usage (root fs), hostname.
- `GET /health` — `{"status":"ok"}`.

The HMI proxies this at `/api/stats` so you only expose port `8080` on the host.
