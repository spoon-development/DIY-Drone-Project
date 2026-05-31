# Telemetry service

Subscribes to MAVLink from **mavlink-router** over UDP, decodes common messages, and exposes JSON for the HMI.

## Flow

```
SpeedyBee FC (UART, MAVLink2) → mavlink-router → UDP :14551 → telemetry → HMI /api/telemetry/*
```

Each HMI poll (`GET /api/telemetry`, ~1 s) triggers a refresh:

1. **Read** current `SR{N}_*` stream-rate params from the FC (`PARAM_REQUEST_READ`)
2. **Request** message intervals for attitude, battery, etc. (`SET_MESSAGE_INTERVAL`)
3. On first connect, optionally **write** desired `SR{N}_*` values once (`PARAM_SET`, `FC_SR_APPLY_ON_CONNECT=1`)

Commands are sent back to mavlink-router on the same UDP path (source port `:14551`) — no extra command port.

## HTTP API (internal port 9102)

| Route | Description |
|-------|-------------|
| `GET /health` | Liveness |
| `GET /api/telemetry` | Latest state; also queues SR param read + stream requests |
| `GET /api/config` | Serial device, baud, supported rates |
| `POST /api/config` | Update baud/device; triggers mavlink-router reload |

## Environment

| Variable | Default | Purpose |
|----------|---------|---------|
| `MAVLINK_UDP_PORT` | `14551` | UDP listen port (must match mavrouter config) |
| `FC_MAVLINK_SR_PORT` | `1` | ArduPilot `SR{N}_*` index for the Pi UART (often **1** = TELEM1) |
| `FC_SR_EXTRA1_HZ` | `10` | Target attitude rate (`SR{N}_EXTRA1`) when applying on connect |
| `FC_SR_EXTRA2_HZ` | `5` | VFR HUD rate |
| `FC_SR_EXTRA3_HZ` | `5` | AHRS2 / extras |
| `FC_SR_EXT_STAT_HZ` | `2` | SYS_STATUS rate |
| `FC_SR_APPLY_ON_CONNECT` | `1` | Write SR params once on first connect (`0` = read/request only) |
| `FC_SERIAL_DEVICE` | `/dev/ttyAMA0` | Default UART device |
| `FC_SERIAL_BAUD` | `921600` | Default baud |
| `TELEMETRY_CONFIG_DIR` | `/config` | Shared volume with mavlink-router |

## Pi serial

Enable UART in `/boot/firmware/config.txt` and wire TX/RX to the FC telem port. Default on Pi is **`/dev/ttyAMA0`**.

If attitude stays empty, confirm the FC MAVLink port matches **`fc_mavlink_sr_port`** in [`deploy/config/stack.yml`](../../deploy/config/stack.yml) (Mission Planner: which `SERIALx` is wired to the Pi → use that index for `SRx_*`).
