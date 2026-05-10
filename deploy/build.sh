#!/usr/bin/env bash
# Frontend: build stack images locally from ./services (docker-compose.build.yml).
# Optional: push those images to GHCR after a successful build (docker login ghcr.io first).
#
# Usage (from repo root or from deploy/):
#   bash ./deploy/build.sh
#   bash ./deploy/build.sh stats hmi
#   bash ./deploy/build.sh --push              # build all, then push to GHCR
#   bash ./deploy/build.sh stats hmi --push   # build named services, then push
#
# Needs Docker Compose v2 ("docker compose") or v1 ("docker-compose" binary).
# On Ubuntu/WSL with only docker.io: sudo apt install -y docker-compose-v2
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

COMPOSE_FILES=(-f docker-compose.yml -f docker-compose.build.yml)
DO_PUSH=0
BUILD_ARGS=()

for arg in "$@"; do
  case "$arg" in
    --push) DO_PUSH=1 ;;
    -h|--help)
      sed -n '1,16p' "$0"
      exit 0
      ;;
    *) BUILD_ARGS+=("$arg") ;;
  esac
done

compose_build() {
  if docker compose version >/dev/null 2>&1; then
    docker compose "${COMPOSE_FILES[@]}" build "$@"
  elif command -v docker-compose >/dev/null 2>&1; then
    docker-compose "${COMPOSE_FILES[@]}" build "$@"
  else
    echo "Docker Compose not found." >&2
    echo "Install v2 plugin:  sudo apt update && sudo apt install -y docker-compose-v2" >&2
    echo "Then use:  docker compose ..." >&2
    exit 2
  fi
}

push_images_to_ghcr() {
  local stack registry tag stats hmi
  stack="${HERE}/config/stack.yml"
  [[ -f "$stack" ]] || { echo "[build] Missing $stack" >&2; exit 2; }

  registry="$(grep -E '^stack_image_registry:' "$stack" | head -1 | sed 's/^stack_image_registry:[[:space:]]*//;s/[[:space:]]*$//')"
  tag="$(grep -E '^stack_image_tag:' "$stack" | head -1 | sed 's/^stack_image_tag:[[:space:]]*//;s/[[:space:]]*$//')"
  [[ -n "$registry" && -n "$tag" ]] || {
    echo "[build] Could not parse stack_image_registry / stack_image_tag from $stack" >&2
    exit 2
  }
  stats="${registry}/dronebros-stats:${tag}"
  hmi="${registry}/dronebros-hmi:${tag}"

  for img in "$stats" "$hmi"; do
    if ! docker image inspect "$img" >/dev/null 2>&1; then
      echo "[build] Local image missing: $img (build the stack first)" >&2
      exit 2
    fi
  done

  echo "[build] pushing $stats"
  docker push "$stats"
  echo "[build] pushing $hmi"
  docker push "$hmi"
  echo "[build] push done."
}

compose_build "${BUILD_ARGS[@]}"

if [[ "$DO_PUSH" -eq 1 ]]; then
  push_images_to_ghcr
fi
