#!/usr/bin/env python3
"""HMI: static UI, proxy to stats, persisted JSON settings on /data (survives reboot)."""
from __future__ import annotations

import asyncio
import json
import os
import shlex
import subprocess
import urllib.error
import urllib.request
from pathlib import Path

from aiohttp import web

STATIC = Path(__file__).resolve().parent / "static"
DATA_DIR = Path(os.environ.get("DATA_DIR", "/data"))
SETTINGS_PATH = DATA_DIR / "hmi-settings.json"
MAX_BODY = int(os.environ.get("HMI_MAX_SETTINGS_BYTES", str(12 * 1024 * 1024)))
HOST = os.environ.get("HMI_BIND", "0.0.0.0")
PORT = int(os.environ.get("HMI_PORT", "80"))
STATS_FETCH_TIMEOUT = float(os.environ.get("STATS_FETCH_TIMEOUT", "35"))
HOST_NETPLAN_FILE = Path(os.environ.get("HMI_HOST_NETPLAN_FILE", "/etc/netplan/99-drone-hmi.yaml"))
NETPLAN_CMD = os.environ.get("HMI_NETPLAN_CMD", "/usr/sbin/netplan").strip() or "/usr/sbin/netplan"


def _stats_url_candidates() -> list[str]:
    raw = os.environ.get("STATS_METRICS_URLS", "").strip()
    if raw:
        urls = [u.strip() for u in raw.split(",") if u.strip()]
    else:
        one = os.environ.get("STATS_METRICS_URL", "http://stats:9101/metrics").strip()
        urls = [
            one,
            "http://stats:9101/metrics",
            "http://dronebros-stats:9101/metrics",
        ]
    seen: set[str] = set()
    out: list[str] = []
    for u in urls:
        if u not in seen:
            seen.add(u)
            out.append(u)
    return out


STATS_URL_CANDIDATES = _stats_url_candidates()


def _fetch_stats_sync(url: str) -> tuple[int, str, str]:
    """GET url; return (status, body text, content-type). Same stack as docker exec urllib tests."""
    req = urllib.request.Request(url, method="GET")
    with urllib.request.urlopen(req, timeout=STATS_FETCH_TIMEOUT) as resp:
        charset = resp.headers.get_content_charset() or "utf-8"
        raw = resp.read()
        text = raw.decode(charset, errors="replace")
        ct = resp.headers.get("Content-Type", "application/json")
        return resp.status, text, ct


def _display_hostname() -> str:
    return os.environ.get("DISPLAY_HOSTNAME", "").strip()


async def host_meta(_request: web.Request) -> web.Response:
    """Browser title / header: set at deploy via DISPLAY_HOSTNAME (Pi `hostname`)."""
    return web.json_response({"displayHostname": _display_hostname()})


def _read_settings() -> dict:
    if not SETTINGS_PATH.is_file():
        return {}
    try:
        raw = SETTINGS_PATH.read_text(encoding="utf-8")
        data = json.loads(raw)
        return data if isinstance(data, dict) else {}
    except (OSError, json.JSONDecodeError):
        return {}


def _write_settings(data: dict) -> None:
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    tmp = SETTINGS_PATH.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(data, indent=2), encoding="utf-8")
    tmp.replace(SETTINGS_PATH)


async def get_hmi_settings(_request: web.Request) -> web.Response:
    return web.json_response(_read_settings())


def _apply_netplan_on_host_sync(yaml_text: str) -> None:
    """Write netplan file on the host (bind-mount) and run netplan apply in host PID/mount/net ns."""
    HOST_NETPLAN_FILE.parent.mkdir(parents=True, exist_ok=True)
    tmp = HOST_NETPLAN_FILE.with_suffix(".yaml.tmp")
    tmp.write_text(yaml_text, encoding="utf-8")
    tmp.replace(HOST_NETPLAN_FILE)
    # PID 1 is the host init when the container is started with Docker Compose pid: "host".
    cmd = [
        "nsenter",
        "-t",
        "1",
        "-m",
        "-u",
        "-i",
        "-n",
        "-p",
        NETPLAN_CMD,
        "apply",
    ]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=120, check=False)
    if r.returncode != 0:
        err = (r.stderr or r.stdout or "").strip() or f"netplan exit {r.returncode}"
        raise RuntimeError(err)


def _schedule_host_reboot_sync() -> None:
    """Reboot the host after a short delay so the HTTP response can complete."""
    inner = "sleep 3; systemctl reboot 2>/dev/null || /sbin/reboot"
    cmd = [
        "nsenter",
        "-t",
        "1",
        "-m",
        "-u",
        "-i",
        "-n",
        "-p",
        "sh",
        "-c",
        f"nohup sh -c {shlex.quote(inner)} >/dev/null 2>&1 &",
    ]
    subprocess.Popen(
        cmd,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
    )


async def post_apply_network(_request: web.Request) -> web.Response:
    if os.environ.get("HMI_APPLY_NETPLAN", "1").strip().lower() in ("0", "false", "no", "off"):
        return web.json_response({"ok": False, "error": "Host netplan apply is disabled on this container."}, status=503)
    cur = _read_settings()
    yaml_text = (cur.get("netplanYaml") or "").strip()
    if not yaml_text:
        return web.json_response({"ok": False, "error": "No netplan text saved yet — fill the network form and save first."}, status=400)
    try:
        await asyncio.to_thread(_apply_netplan_on_host_sync, yaml_text)
    except FileNotFoundError as e:
        return web.json_response({"ok": False, "error": str(e)}, status=500)
    except OSError as e:
        return web.json_response({"ok": False, "error": str(e)}, status=500)
    except subprocess.TimeoutExpired:
        return web.json_response({"ok": False, "error": "netplan apply timed out"}, status=500)
    except RuntimeError as e:
        return web.json_response({"ok": False, "error": str(e)}, status=500)
    except Exception as e:  # noqa: BLE001
        return web.json_response({"ok": False, "error": str(e)}, status=500)

    reboot_scheduled = False
    if os.environ.get("HMI_REBOOT_AFTER_NETPLAN", "1").strip().lower() not in ("0", "false", "no", "off"):
        try:
            await asyncio.to_thread(_schedule_host_reboot_sync)
            reboot_scheduled = True
        except FileNotFoundError:
            pass

    return web.json_response({"ok": True, "rebootScheduled": reboot_scheduled})


async def post_hmi_settings(request: web.Request) -> web.Response:
    body = await request.read()
    if len(body) > MAX_BODY:
        return web.Response(status=413, text="Settings payload too large")
    try:
        incoming = json.loads(body.decode("utf-8"))
    except json.JSONDecodeError:
        return web.Response(status=400, text="Invalid JSON")
    if not isinstance(incoming, dict):
        return web.Response(status=400, text="Expected a JSON object")

    cur = _read_settings()
    if "background" in incoming:
        cur["background"] = incoming["background"]
    if "networkDraft" in incoming:
        cur["networkDraft"] = incoming["networkDraft"]
    if "netplanYaml" in incoming:
        cur["netplanYaml"] = incoming["netplanYaml"]
    _write_settings(cur)
    return web.json_response(cur)


async def proxy_stats(_request: web.Request) -> web.StreamResponse:
    """Proxy using stdlib urllib in a thread (matches behaviour of one-liner diagnostics on the Pi)."""
    last_err: BaseException | None = None
    for url in STATS_URL_CANDIDATES:
        for attempt in range(6):
            try:
                status, text, ct = await asyncio.to_thread(_fetch_stats_sync, url)
                if status != 200:
                    raise RuntimeError(f"stats returned HTTP {status}")
                main_type = (ct or "application/json").split(";")[0].strip()
                out = web.Response(status=status, text=text, content_type=main_type or "application/json")
                out.headers["Cache-Control"] = "no-store"
                return out
            except Exception as e:  # noqa: BLE001
                last_err = e
                print(f"[hmi] proxy_stats url={url!r} attempt={attempt}: {e!r}", flush=True)
                if attempt < 5:
                    await asyncio.sleep(0.25 * (attempt + 1))
    return web.json_response(
        {
            "error": str(last_err) if last_err else "unknown",
            "hint": "Tried: "
            + ", ".join(STATS_URL_CANDIDATES)
            + " — on the Pi: docker compose logs stats hmi && docker compose ps",
        },
        status=502,
    )


async def index(_request: web.Request) -> web.FileResponse:
    return web.FileResponse(STATIC / "index.html")


def create_app() -> web.Application:
    app = web.Application(client_max_size=MAX_BODY)
    app.router.add_get("/api/hmi-settings", get_hmi_settings)
    app.router.add_post("/api/hmi-settings", post_hmi_settings)
    app.router.add_post("/api/apply-network", post_apply_network)
    app.router.add_get("/api/host-meta", host_meta)
    app.router.add_get("/api/stats", proxy_stats)
    app.router.add_get("/", index)
    app.router.add_get("/index.html", index)
    app.router.add_static("/images/", STATIC / "images", show_index=False)
    return app


if __name__ == "__main__":
    web.run_app(create_app(), host=HOST, port=PORT, print=None)
