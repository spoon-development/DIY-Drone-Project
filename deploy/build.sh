#!/usr/bin/env bash
# Build stack images locally from ./services (uses docker-compose.build.yml).
#
# Usage (from repo root):
#   bash ./deploy/build.sh
#   bash ./deploy/build.sh stats hmi
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
exec docker compose -f docker-compose.yml -f docker-compose.build.yml build "$@"
