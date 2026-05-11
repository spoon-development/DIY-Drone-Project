#!/usr/bin/env bash
# Local image build (compose). Does not push to GHCR — use build/push-images.sh after login.
#
# Usage (from repo root):
#   bash ./build/build.sh
#   bash ./build/build.sh stats hmi
#   bash ./build/build.sh --arm              # Pi: linux/arm64
#   bash ./build/build.sh --amd              # PC: linux/amd64
#   bash ./build/build.sh --arm stats hmi
#
# --arm / --amd  →  sets DOCKER_DEFAULT_PLATFORM for compose build (linux/arm64 | linux/amd64).
# Omit both for a native / default-platform build on this machine.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

COMPOSE_FILES=(-f docker-compose.yml -f docker-compose.build.yml)
BUILD_ARGS=()
PLATFORM_BUILD=""

usage() {
  sed -n '1,22p' "$0"
}

set_platform() {
  local want="$1"
  if [[ -n "$PLATFORM_BUILD" && "$PLATFORM_BUILD" != "$want" ]]; then
    echo "[build] use only one of --arm or --amd" >&2
    exit 2
  fi
  PLATFORM_BUILD="$want"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --arm)
      set_platform "linux/arm64"
      shift
      ;;
    --amd)
      set_platform "linux/amd64"
      shift
      ;;
    *)
      BUILD_ARGS+=("$1")
      shift
      ;;
  esac
done

compose_build() {
  # Many Compose installs reject "docker compose build --platform …" (unknown flag).
  # DOCKER_DEFAULT_PLATFORM is honored by docker build / buildx under compose.
  if docker compose version >/dev/null 2>&1; then
    if [[ -n "$PLATFORM_BUILD" ]]; then
      DOCKER_DEFAULT_PLATFORM="$PLATFORM_BUILD" docker compose "${COMPOSE_FILES[@]}" build "$@"
    else
      docker compose "${COMPOSE_FILES[@]}" build "$@"
    fi
  elif command -v docker-compose >/dev/null 2>&1; then
    if [[ -n "$PLATFORM_BUILD" ]]; then
      DOCKER_DEFAULT_PLATFORM="$PLATFORM_BUILD" docker-compose "${COMPOSE_FILES[@]}" build "$@"
    else
      docker-compose "${COMPOSE_FILES[@]}" build "$@"
    fi
  else
    echo "Docker Compose not found." >&2
    echo "Install v2 plugin:  sudo apt update && sudo apt install -y docker-compose-v2" >&2
    exit 2
  fi
}

compose_build "${BUILD_ARGS[@]}"
