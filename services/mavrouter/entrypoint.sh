#!/bin/sh
set -eu

CONFIG_DIR="${MAVLINK_ROUTER_CONFIG_DIR:-/config}"
CONFIG="${CONFIG_DIR}/main.conf"
DESIRED="${CONFIG_DIR}/desired.json"
RELOAD_FLAG="${CONFIG_DIR}/reload.request"

mkdir -p "$CONFIG_DIR"

resolve_host_ip() {
  host="$1"
  case "$host" in
    [0-9]*.[0-9]*.[0-9]*.[0-9]*) echo "$host"; return 0 ;;
  esac
  getent hosts "$host" 2>/dev/null | awk '{print $1; exit}'
}

read_desired() {
  DEVICE="${FC_SERIAL_DEVICE:-/dev/ttyAMA0}"
  BAUD="${FC_SERIAL_BAUD:-921600}"
  TELEM_HOST="${FC_TELEMETRY_UDP_HOST:-telemetry}"
  TELEM_PORT="${FC_TELEMETRY_UDP_PORT:-14551}"

  if [ -f "$DESIRED" ]; then
    # Minimal JSON extraction without jq (device string, baud integer).
    if grep -q '"device"' "$DESIRED" 2>/dev/null; then
      val="$(sed -n 's/.*"device"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$DESIRED" | head -1)"
      [ -n "$val" ] && DEVICE="$val"
    fi
    if grep -q '"baud"' "$DESIRED" 2>/dev/null; then
      val="$(sed -n 's/.*"baud"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$DESIRED" | head -1)"
      [ -n "$val" ] && BAUD="$val"
    fi
  fi

  export FC_ACTIVE_DEVICE="$DEVICE"
  export FC_ACTIVE_BAUD="$BAUD"
  export FC_ACTIVE_TELEM_HOST="$TELEM_HOST"
  export FC_ACTIVE_TELEM_PORT="$TELEM_PORT"
}

write_config() {
  read_desired
  TELEM_IP="$(resolve_host_ip "$FC_ACTIVE_TELEM_HOST")"
  if [ -z "$TELEM_IP" ]; then
    echo "[mavrouter] cannot resolve ${FC_ACTIVE_TELEM_HOST} — is telemetry up on the docker network?"
    return 1
  fi
  cat >"$CONFIG" <<EOF
[General]
TcpServerPort = 0
ReportStats = true
MavlinkDialect = ardupilotmega

[UartEndpoint fc]
Device = ${FC_ACTIVE_DEVICE}
Baud = ${FC_ACTIVE_BAUD}
FlowControl = false

[UdpEndpoint telemetry]
Mode = normal
Address = ${TELEM_IP}
Port = ${FC_ACTIVE_TELEM_PORT}
EOF
  echo "[mavrouter] config device=${FC_ACTIVE_DEVICE} baud=${FC_ACTIVE_BAUD} -> udp ${TELEM_IP}:${FC_ACTIVE_TELEM_PORT} (${FC_ACTIVE_TELEM_HOST})"
}

while true; do
  rm -f "$RELOAD_FLAG"
  if ! write_config; then
    sleep 3
    continue
  fi
  if [ ! -e "$FC_ACTIVE_DEVICE" ]; then
    echo "[mavrouter] serial device $FC_ACTIVE_DEVICE not present — waiting (enable Pi UART or set FC_SERIAL_DEVICE in .env / HMI)"
    sleep 5
    continue
  fi
  mavlink-routerd -c "$CONFIG" &
  pid=$!
  while kill -0 "$pid" 2>/dev/null; do
    if [ -f "$RELOAD_FLAG" ]; then
      echo "[mavrouter] reload requested — restarting serial link"
      kill "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
      sleep 1
      break
    fi
    sleep 1
  done
  wait "$pid" 2>/dev/null || true
  sleep 2
done
