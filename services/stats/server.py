#!/usr/bin/env python3
"""Lightweight JSON metrics for the host Pi via mounted /proc, /sys, and root."""

from __future__ import annotations

import glob
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
    """Prefer the host filesystem: bind-mounted /proc often reflects the container UTS name."""
    for path in (f"{HOST_ROOT}/etc/hostname", f"{HOST_PROC}/sys/kernel/hostname"):
        h = _read(path)
        if h:
            line = h.splitlines()[0].strip()
            if line:
                return line
    return None


def _proc_route_paths() -> list[str]:
    """Prefer host PID 1 netns (works when host /proc is bind-mounted)."""
    return [f"{HOST_PROC}/1/net/route", f"{HOST_PROC}/net/route"]


def _decode_route_ipv4(hex8: str) -> str | None:
    """Decode IPv4 from /proc/net/route gateway/mask columns (32-bit LE hex word)."""
    h = hex8.strip()
    if len(h) != 8:
        return None
    try:
        a = int(h, 16)
        return ".".join(str((a >> (8 * i)) & 0xFF) for i in range(4))
    except ValueError:
        return None


def _is_virtual_iface(name: str) -> bool:
    """Skip docker/bridge/tunnel ifaces when inferring the host LAN route."""
    if not name or name == "lo":
        return True
    nl = name.lower()
    if nl.startswith(("veth", "br-", "virbr", "docker", "lxc", "zt", "tun", "tap")):
        return True
    if nl in ("docker0", "cni0", "flannel.1", "cilium_host", "cilium_net"):
        return True
    return False


def _default_routes_from_proc() -> list[dict[str, Any]]:
    """All IPv4 default routes from the host routing table (may be several)."""
    out: list[dict[str, Any]] = []
    for path in _proc_route_paths():
        text = _read(path)
        if not text:
            continue
        lines = text.strip().splitlines()
        if len(lines) < 2:
            continue
        for line in lines[1:]:
            parts = line.split()
            if len(parts) < 8:
                continue
            iface, dest, gw_raw, mask = parts[0], parts[1], parts[2], parts[7]
            if dest != "00000000" or mask != "00000000":
                continue
            try:
                metric = int(parts[6])
            except (ValueError, IndexError):
                metric = 1_000_000
            g = _decode_route_ipv4(gw_raw)
            if g == "0.0.0.0":
                g = None
            out.append({"iface": iface, "gateway": g, "metric": metric, "source": path})
    return out


def default_ipv4_route() -> dict[str, Any]:
    """Pick lowest-metric default route, skipping obvious virtual ifaces when possible."""
    candidates = _default_routes_from_proc()
    if not candidates:
        return {}
    real = [c for c in candidates if not _is_virtual_iface(c["iface"])]
    pool = real if real else candidates
    pool.sort(key=lambda c: (c["metric"], c["iface"]))
    best = pool[0]
    return {
        "iface": best["iface"],
        "gateway": best["gateway"],
        "metric": best["metric"],
        "source": best["source"],
    }


def suggested_static_iface(route: dict[str, Any], ifaces: list[dict[str, Any]]) -> str:
    """Prefer wired iface that looks up so static IP matches Ethernet, not only Wi-Fi default route."""
    if not ifaces:
        cand = str(route.get("iface") or "").strip()
        return cand if cand and not _is_virtual_iface(cand) else "eth0"

    def sort_key(x: dict[str, Any]) -> tuple[int, int, int, str]:
        name = (x.get("name") or "").strip()
        if not name or name == "lo" or _is_virtual_iface(name):
            return (9, 9, 9, name)
        carrier_ok = x.get("carrier") is True
        up = (x.get("operstate") or "").lower() == "up"
        stale = (x.get("operstate") or "").lower() == "unknown" and x.get("carrier") is None
        link_score = 0 if (carrier_ok or up or stale) else 1
        if re.match(r"^(en|end|eth|usb)", name, re.I):
            medium = 0  # prefer Ethernet-like jacks for static LAN IP
        elif name.startswith("wl"):
            medium = 1
        else:
            medium = 2
        return (link_score, medium, name)

    phys = [x for x in ifaces if (x.get("name") or "").strip() not in ("", "lo")]
    phys.sort(key=sort_key)
    for x in phys:
        name = (x.get("name") or "").strip()
        if name == "lo" or _is_virtual_iface(name):
            continue
        return name
    cand = str(route.get("iface") or "").strip()
    if cand and not _is_virtual_iface(cand):
        return cand
    return "eth0"


def _list_sysfs_net_ifaces() -> list[dict[str, Any]]:
    base = f"{HOST_SYS}/class/net"
    out: list[dict[str, Any]] = []
    try:
        names = sorted(os.listdir(base))
    except OSError:
        return out
    for name in names:
        if name == "lo":
            continue
        pfx = f"{base}/{name}"
        mac = (_read(f"{pfx}/address") or "").strip()
        state = (_read(f"{pfx}/operstate") or "").strip()
        carrier = (_read(f"{pfx}/carrier") or "").strip()
        row: dict[str, Any] = {"name": name, "operstate": state or None}
        if mac and mac != "00:00:00:00:00:00":
            row["mac"] = mac
        if carrier in ("0", "1"):
            row["carrier"] = carrier == "1"
        out.append(row)
    return out


def _read_devicetree_model() -> str | None:
    raw = _read(f"{HOST_ROOT}/sys/firmware/devicetree/base/model")
    if not raw:
        return None
    return raw.replace("\x00", "").strip() or None


def _parse_cpuinfo_board() -> dict[str, Any]:
    text = _read(f"{HOST_PROC}/cpuinfo")
    if not text:
        return {}
    out: dict[str, Any] = {}
    for line in text.splitlines():
        if ":" not in line:
            continue
        k, v = line.split(":", 1)
        key = k.strip()
        val = v.strip()
        if key in ("Hardware", "Revision", "Model", "model name"):
            out[key.replace(" ", "_").lower()] = val
    return out


def compute_profile() -> dict[str, Any]:
    u = os.uname()
    board = _parse_cpuinfo_board()
    model = _read_devicetree_model()
    kind = "unknown"
    label_parts: list[str] = []
    if model:
        kind = "embedded"
        label_parts.append(model)
    elif board.get("hardware") or board.get("revision"):
        kind = "embedded"
        if board.get("hardware"):
            label_parts.append(board["hardware"])
        if board.get("revision"):
            label_parts.append(f"rev {board['revision']}")
    elif board.get("model_name"):
        kind = "general"
        label_parts.append(board["model_name"])
    label = " · ".join(label_parts) if label_parts else None
    return {
        "kind": kind,
        "label": label,
        "kernel": u.release,
        "machine": u.machine,
        "cpuinfo": board or None,
        "devicetree_model": model,
    }


def _grep_netplan_hints() -> list[dict[str, Any]]:
    paths = sorted(glob.glob(f"{HOST_ROOT}/etc/netplan/*.yaml"))
    paths += sorted(glob.glob(f"{HOST_ROOT}/etc/netplan/*.yml"))
    seen: set[str] = set()
    out: list[dict[str, Any]] = []
    for path in paths:
        if path in seen:
            continue
        seen.add(path)
        text = _read(path)
        if not text:
            continue
        hints: list[str] = []
        for m in re.finditer(
            r"(^\s*(?:addresses|gateway4|gateway6|routes|nameservers|dhcp4)\s*:.*$)",
            text,
            re.MULTILINE | re.IGNORECASE,
        ):
            hints.append(m.group(1).rstrip())
        disp = path
        if path.startswith(HOST_ROOT):
            disp = path[len(HOST_ROOT) :] or "/"
            if not disp.startswith("/"):
                disp = "/" + disp
        out.append({"path": disp, "hints": hints[:24]})
    return out


def _grep_dhcpcd_hints() -> list[str]:
    text = _read(f"{HOST_ROOT}/etc/dhcpcd.conf")
    if not text:
        return []
    hints: list[str] = []
    for line in text.splitlines():
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        if any(
            s.lower().startswith(p)
            for p in (
                "interface ",
                "static ip_",
                "static routers",
                "static domain_",
                "nogateway",
                "fallback",
            )
        ):
            hints.append(s)
    return hints[:40]


def network_summary() -> dict[str, Any]:
    ifaces = _list_sysfs_net_ifaces()
    all_defaults = _default_routes_from_proc()
    route = default_ipv4_route()
    netplan = _grep_netplan_hints()
    dhcpcd = _grep_dhcpcd_hints()
    primary = None
    for block in netplan:
        for h in block.get("hints", []):
            m = re.search(
                r"\b((?:\d{1,3}\.){3}\d{1,3})(?:/\d{1,2})?\b",
                h,
            )
            if m:
                primary = m.group(1)
                break
        if primary:
            break
    if not primary and dhcpcd:
        for line in dhcpcd:
            m = re.search(r"=\s*((?:\d{1,3}\.){3}\d{1,3})", line)
            if m:
                primary = m.group(1)
                break
    return {
        "interfaces": ifaces,
        "default_route": route or None,
        "default_routes": sorted(all_defaults, key=lambda c: (c["metric"], c["iface"]))[:12],
        "static_iface_hint": suggested_static_iface(route or {}, ifaces),
        "config_hints": {"netplan": netplan, "dhcpcd": dhcpcd},
        "primary_ipv4_hint": primary,
    }


def collect() -> dict[str, Any]:
    return {
        "hostname": hostname(),
        "cpu_temp_c": cpu_temp_c(),
        "loadavg": loadavg(),
        "memory": memory_mb(),
        "uptime": uptime(),
        "disk_root": disk_root(),
        "compute": compute_profile(),
        "network": network_summary(),
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
