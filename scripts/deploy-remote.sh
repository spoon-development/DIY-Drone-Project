#!/usr/bin/env bash
# Minimal SSH deploy without Ansible: copies compose + seeds .env, then compose up.
# Usage:
#   ./scripts/deploy-remote.sh --host 192.168.1.50 --user pi
#   ./scripts/deploy-remote.sh -H pi.local -u pi --dir /opt/dronebros --skip-pull

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST=""
USER=""
IDENTITY=""
DIR="/opt/dronebros"
SKIP_PULL="0"

while [[ $# -gt 0 ]]; do
  case "$1" in
    -H|--host) HOST="${2:?}"; shift 2 ;;
    -u|--user) USER="${2:?}"; shift 2 ;;
    -i|--identity) IDENTITY="${2:?}"; shift 2 ;;
    --dir) DIR="${2:?}"; shift 2 ;;
    --skip-pull) SKIP_PULL="1"; shift ;;
    -h|--help) head -n 18 "$0"; exit 0 ;;
    *) echo "Unknown: $1" >&2; exit 2 ;;
  esac
done

[[ -n "$HOST" && -n "$USER" ]] || { echo "Need --host and --user"; exit 2; }

SSH_OPTS=(-o StrictHostKeyChecking=accept-new)
[[ -n "$IDENTITY" ]] && SSH_OPTS+=(-i "$IDENTITY")

ssh "${SSH_OPTS[@]}" "${USER}@${HOST}" "mkdir -p '${DIR}'"
scp "${SSH_OPTS[@]}" "${ROOT}/docker-compose.yml" "${USER}@${HOST}:${DIR}/docker-compose.yml"
scp "${SSH_OPTS[@]}" "${ROOT}/.env.example" "${USER}@${HOST}:${DIR}/.env.example"
ssh "${SSH_OPTS[@]}" "${USER}@${HOST}" "test -f '${DIR}/.env' || cp '${DIR}/.env.example' '${DIR}/.env'"

if [[ "$SKIP_PULL" == "1" ]]; then
  ssh "${SSH_OPTS[@]}" "${USER}@${HOST}" "cd '${DIR}' && docker compose up -d"
else
  ssh "${SSH_OPTS[@]}" "${USER}@${HOST}" "cd '${DIR}' && docker compose pull && docker compose up -d"
fi
