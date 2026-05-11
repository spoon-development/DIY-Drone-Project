#!/usr/bin/env bash
# Run ON THE Raspberry Pi (SSH in as your deploy user), from any directory.
#
#   bash cleanup-legacy-on-pi.sh           # stop old stacks only (safe)
#   bash cleanup-legacy-on-pi.sh --delete  # also remove ~/dronebros, bundle tar, stray compose if it mentions dronebros
#
set -euo pipefail

DELETE=0
[[ "${1:-}" == "--delete" ]] && DELETE=1

down() {
  local dir="$1"
  [[ -f "$dir/docker-compose.yml" ]] || return 0
  echo "[cleanup] docker compose down in: $dir"
  (cd "$dir" && docker compose down --remove-orphans --volumes) || true
}

echo "[cleanup] Stopping legacy dronebros compose projects (if any)..."
down "${HOME}/dronebros"
if [[ -f "${HOME}/docker-compose.yml" ]]; then
  down "${HOME}"
fi
docker rm -f dronebros-stats dronebros-hmi 2>/dev/null || true

echo "[cleanup] Remaining containers (filtered):"
docker ps -a --format '{{.Names}}\t{{.Status}}' 2>/dev/null | grep -i drone || true

if [[ "$DELETE" -eq 1 ]]; then
  rm -f "${HOME}/dronebros-bundle.tar" 2>/dev/null || true
  rm -rf "${HOME}/dronebros" 2>/dev/null || true
  if [[ -f "${HOME}/docker-compose.yml" ]] && grep -qi dronebros "${HOME}/docker-compose.yml" 2>/dev/null; then
    rm -f "${HOME}/docker-compose.yml"
    echo "[cleanup] Removed ${HOME}/docker-compose.yml (referenced dronebros)."
  fi
  echo "[cleanup] Done (--delete): legacy files under \$HOME removed where listed."
else
  echo "[cleanup] Done (stop only). To also delete ~/dronebros, ~/dronebros-bundle.tar, and a stray ~/docker-compose.yml:"
  echo "           bash $0 --delete"
fi
