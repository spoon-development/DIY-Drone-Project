#!/usr/bin/env bash
# Local image build (compose). Does not push to GHCR — use build/push-images.sh after login.
#
# Skips docker compose build for a service when its source hash matches the last local build
# (stored under .build/). Override with --force or SKIP_UNCHANGED=0.
#
# Usage (from repo root):
#   bash ./build/build.sh
#   bash ./build/build.sh stats hmi video
#   bash ./build/build.sh --force
#   bash ./build/build.sh --only hmi
#   bash ./build/build.sh --arm              # Pi: linux/arm64
#   bash ./build/build.sh --amd              # PC: linux/amd64
#   bash ./build/build.sh --arm stats hmi video
#
# --arm / --amd  →  buildx cross-build for that platform (--load into local docker).
# Omit both for a native compose build on this machine.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

COMPOSE_FILES=(-f docker-compose.yml -f docker-compose.build.yml)
HASH_DIR="${ROOT}/.build"
SKIP_UNCHANGED="${SKIP_UNCHANGED:-1}"
FORCE=0
PLATFORM_BUILD=""
ONLY_ARG=""
declare -a SERVICES=()

usage() {
  sed -n '1,26p' "$0"
}

set_platform() {
  local want="$1"
  if [[ -n "$PLATFORM_BUILD" && "$PLATFORM_BUILD" != "$want" ]]; then
    echo "[build] use only one of --arm or --amd" >&2
    exit 2
  fi
  PLATFORM_BUILD="$want"
}

context_hash() {
  local svc="$1"
  local d="${ROOT}/services/${svc}"
  [[ -d "$d" ]] || {
    echo ""
    return 1
  }
  (cd "$d" && find . -type f ! -path '*/.*' 2>/dev/null | LC_ALL=C sort | while IFS= read -r rel; do
    [[ -f "$rel" ]] || continue
    sha256sum "$rel" 2>/dev/null || true
  done | sha256sum | awk '{print $1}')
}

hash_file() {
  local svc="$1"
  if [[ -n "$PLATFORM_BUILD" ]]; then
    local suffix="${PLATFORM_BUILD//\//-}"
    echo "${HASH_DIR}/built-${svc}-${suffix}.sha256"
  else
    echo "${HASH_DIR}/built-${svc}.sha256"
  fi
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --force|-f)
      FORCE=1
      shift
      ;;
    --no-skip-unchanged)
      SKIP_UNCHANGED=0
      shift
      ;;
    --only)
      ONLY_ARG="${2:?}"
      shift 2
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
      SERVICES+=("$1")
      shift
      ;;
  esac
done

if [[ -n "$ONLY_ARG" ]]; then
  SERVICES=()
  IFS=',' read -ra tmp <<<"$ONLY_ARG"
  for x in "${tmp[@]}"; do
    x="${x//[[:space:]]/}"
    [[ -n "$x" ]] && SERVICES+=("$x")
  done
fi

if [[ ${#SERVICES[@]} -eq 0 ]]; then
  SERVICES=(stats hmi video mavrouter telemetry)
fi

for svc in "${SERVICES[@]}"; do
  case "$svc" in
    stats|hmi|video|mavrouter|telemetry) ;;
    *)
      echo "[build] unknown service: $svc" >&2
      exit 2
      ;;
  esac
done

image_for_svc() {
  local reg tag
  reg="${IMAGE_REGISTRY:-ghcr.io/spoon-development}"
  tag="${IMAGE_TAG:-latest}"
  echo "${reg}/dronebros-${1}:${tag}"
}

platform_buildx() {
  if ! docker buildx version >/dev/null 2>&1; then
    echo "[build] docker buildx required for --arm/--amd (install Docker Buildx)" >&2
    exit 2
  fi
  local builder="diy-drone-local"
  if ! docker buildx inspect "$builder" >/dev/null 2>&1; then
    docker buildx create --name "$builder" --driver docker-container --bootstrap
  fi
  docker buildx use "$builder"
  local svc img
  for svc in "$@"; do
    img="$(image_for_svc "$svc")"
    echo "[build] buildx $img ($PLATFORM_BUILD)"
    docker buildx build --platform "$PLATFORM_BUILD" -t "$img" --load "${ROOT}/services/${svc}"
  done
}

compose_build() {
  if [[ -n "$PLATFORM_BUILD" ]]; then
    platform_buildx "$@"
    return
  fi
  if docker compose version >/dev/null 2>&1; then
    docker compose "${COMPOSE_FILES[@]}" build "$@"
  elif command -v docker-compose >/dev/null 2>&1; then
    docker-compose "${COMPOSE_FILES[@]}" build "$@"
  else
    echo "Docker Compose not found." >&2
    echo "Install v2 plugin:  sudo apt update && sudo apt install -y docker-compose-v2" >&2
    exit 2
  fi
}

mkdir -p "$HASH_DIR"

declare -a TO_BUILD=()
for svc in "${SERVICES[@]}"; do
  h="$(context_hash "$svc" || true)"
  if [[ "$SKIP_UNCHANGED" == "1" && "$FORCE" != "1" && -n "$h" ]]; then
    prev=""
    hf="$(hash_file "$svc")"
    [[ -f "$hf" ]] && prev="$(cat "$hf")"
    if [[ "$h" == "$prev" ]]; then
      echo "[build] skip $svc (unchanged since last build; use --force to rebuild)"
      continue
    fi
  fi
  TO_BUILD+=("$svc")
done

if [[ ${#TO_BUILD[@]} -eq 0 ]]; then
  echo "[build] nothing to build."
  exit 0
fi

compose_build "${TO_BUILD[@]}"

for svc in "${TO_BUILD[@]}"; do
  h="$(context_hash "$svc" || true)"
  [[ -n "$h" ]] && printf '%s' "$h" >"$(hash_file "$svc")"
done
