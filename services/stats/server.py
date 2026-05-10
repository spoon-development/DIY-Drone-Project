#!/usr/bin/env python3
"""Lightweight JSON metrics for the host Pi via mounted /proc, /sys, and root."""

from __future__ import annotations

import json
import os
import re
import socket
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any

HOST_PROC = os.environ.get("HOST_PROC", "/host/proc")
HOST_SYS = os.environ.get("HOST_SYS", "/host/sys")
HOST_ROOT = os.environ.get("HOST_ROOT", "/host_root")
LISTEN = ("0.0.0.0", int(os.environ.get("STATS_PORT", "9101")))


def _read(path: str) -> str | None:
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return None


def cpu_temp_c() -> float | None:
    raw = _read(f"{HOST_SYS}/class/thermal/thermal_zone0/temp")
    if raw is None:
        return None
    try:
        return round(int(raw.strip()) / 1000.0, 2)
    except ValueError:
        return None


def loadavg() -> dict[str, Any]:
    line = _read(f"{HOST_PROC}/loadavg")
    if not line:
        return {}
    parts = line.split()
    if len(parts) < 3:
        return {"raw": line.strip()}
    try:
        return {
            "1m": float(parts[0]),
            "5m": float(parts[1]),
            "15m": float(parts[2]),
        }
    except ValueError:
        return {"raw": line.strip()}


def memory_mb() -> dict[str, Any]:
    text = _read(f"{HOST_PROC}/meminfo")
    if not text:
        return {}

    def grab(label: str) -> int | None:
        m = re.search(rf"^{label}:\s+(\d+)\s+kB$", text, re.MULTILINE)
        if not m:
            return None
        return int(m.group(1)) // 1024

    total = grab("MemTotal")
    avail = grab("MemAvailable")
    free = grab("MemFree")
    out: dict[str, Any] = {}
    if total is not None:
        out["mem_total_mb"] = total
    if avail is not None:
        out["mem_available_mb"] = avail
    if free is not None:
        out["mem_free_mb"] = free
    if total is not None and avail is not None:
        out["mem_used_mb"] = max(total - avail, 0)
        out["mem_used_pct"] = round(100.0 * (total - avail) / total, 1)
    return out


def uptime() -> dict[str, Any]:
    line = _read(f"{HOST_PROC}/uptime")
    if not line:
        return {}
    parts = line.split()
    if len(parts) < 2:
        return {"raw": line.strip()}
    try:
        up = float(parts[0])
        idle = float(parts[1])
        return {"uptime_seconds": up, "idle_seconds": idle}
    except ValueError:
        return {"raw": line.strip()}


def disk_root() -> dict[str, Any]:
    path = HOST_ROOT
    if not os.path.isdir(path):
        return {"mounted": False}
    try:
        st = os.statvfs(path)
    except OSError as e:
        return {"mounted": True, "error": str(e)}
    total = st.f_frsize * st.f_blocks
    free = st.f_frsize * st.f_bavail
    used = total - free
    return {
        "mounted": True,
        "path": path,
        "total_mb": round(total / (1024 * 1024), 1),
        "used_mb": round(used / (1024 * 1024), 1),
        "avail_mb": round(free / (1024 * 1024), 1),
        "used_pct": round(100.0 * used / total, 1) if total else 0.0,
    }


def hostname() -> str | None:
    h = _read(f"{HOST_PROC}/sys/kernel/hostname")
    return h.strip() if h else None


def collect() -> dict[str, Any]:
    return {
        "hostname": hostname(),
        "cpu_temp_c": cpu_temp_c(),
        "loadavg": loadavg(),
        "memory": memory_mb(),
        "uptime": uptime(),
        "disk_root": disk_root(),
    }


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt: str, *args: Any) -> None:
        print(f"[stats] {self.address_string()} - {fmt % args}", flush=True)

    def do_GET(self) -> None:  # noqa: N802
        if self.path not in ("/", "/health", "/metrics"):
            self.send_error(404, "Not Found")
            return
        if self.path == "/health":
            body = b'{"status":"ok"}\n'
        else:
            payload = collect()
            body = json.dumps(payload, indent=2).encode("utf-8") + b"\n"
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)


def main() -> None:
    ThreadingHTTPServer.allow_reuse_address = True
    httpd = ThreadingHTTPServer(LISTEN, Handler)
    httpd.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    print(f"[stats] listening on http://{LISTEN[0]}:{LISTEN[1]}/metrics", flush=True)
    httpd.serve_forever()


if __name__ == "__main__":
    main()
