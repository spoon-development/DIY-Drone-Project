#!/usr/bin/env bash
# Push stats + HMI + video images to GHCR using buildx (after: docker login ghcr.io).
# Reads registry/tag from deploy/config/stack.yml — same names the Pi pulls.
#
# Skips build/push when the service directory hash matches the last successful push
# (stored under .build/). Override with --force or SKIP_UNCHANGED=0.
#
# Usage (from repo root):
#   bash ./build/push-images.sh
#   bash ./build/push-images.sh --force
#   bash ./build/push-images.sh --only stats,hmi
#   bash ./build/push-images.sh video              # push only dronebros-video (tag from stack.yml)
#   bash ./build/push-images.sh --image arm64
#   bash ./build/push-images.sh --image multi --only video
#
# Tag and registry come from deploy/config/stack.yml unless you override for this run only:
#   IMAGE_TAG=mytag bash ./build/push-images.sh video
#   IMAGE_REGISTRY=ghcr.io/org bash ./build/push-images.sh
#
# --image multi (default)  → linux/amd64,linux/arm64 manifest
# --image amd64|arm64      → single-platform image push
# --force                  → push all listed services regardless of hash
# --no-skip-unchanged      → same as SKIP_UNCHANGED=0
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CFG="${ROOT}/deploy/config/stack.yml"
PLATFORM_PUSH="linux/amd64,linux/arm64"
HASH_DIR="${ROOT}/.build"
SKIP_UNCHANGED="${SKIP_UNCHANGED:-1}"
FORCE=0
ONLY_ARG=""
declare -a SERVICES=()

usage() {
  sed -n '1,35p' "$0"
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
    --image)
      shift
      case "${1:-}" in
        amd64) PLATFORM_PUSH="linux/amd64" ;;
        arm64) PLATFORM_PUSH="linux/arm64" ;;
        multi|all) PLATFORM_PUSH="linux/amd64,linux/arm64" ;;
        *)
          echo "[push] --image must be amd64, arm64, or multi" >&2
          exit 2
          ;;
      esac
      shift
      ;;
    -*)
      echo "[push] unknown option: $1" >&2
      echo "[push] Push only the video image:  bash ./build/push-images.sh video" >&2
      echo "[push] Or:                          bash ./build/push-images.sh --only video" >&2
      echo "[push] Tag is from deploy/config/stack.yml (stack_image_tag), not a CLI flag." >&2
      echo "[push] Override tag for this run:    IMAGE_TAG=latest bash ./build/push-images.sh video" >&2
      echo "[push] Run from repo root (or use): bash ./build/push-images.sh --help" >&2
      exit 2
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

[[ -f "$CFG" ]] || {
  echo "[push] Missing $CFG" >&2
  exit 2
}

registry="$(grep -E '^stack_image_registry:' "$CFG" | head -1 | sed 's/^stack_image_registry:[[:space:]]*//;s/[[:space:]]*$//')"
tag="$(grep -E '^stack_image_tag:' "$CFG" | head -1 | sed 's/^stack_image_tag:[[:space:]]*//;s/[[:space:]]*$//')"
[[ -n "${IMAGE_REGISTRY:-}" ]] && registry="$IMAGE_REGISTRY" && echo "[push] IMAGE_REGISTRY override: $registry" >&2
[[ -n "${IMAGE_TAG:-}" ]] && tag="$IMAGE_TAG" && echo "[push] IMAGE_TAG override: $tag" >&2
[[ -n "$registry" && -n "$tag" ]] || {
  echo "[push] Could not parse stack_image_registry / stack_image_tag from $CFG" >&2
  exit 2
}

if ! docker buildx version >/dev/null 2>&1; then
  echo "[push] docker buildx not found; install buildx to push images." >&2
  echo "[push] See: https://docs.docker.com/build/install-buildx/" >&2
  exit 2
fi

builder="diy-drone-multiarch"
if ! docker buildx inspect "$builder" >/dev/null 2>&1; then
  docker buildx create --name "$builder" --driver docker-container --bootstrap
fi
docker buildx use "$builder"

mkdir -p "$HASH_DIR"

image_for() {
  case "$1" in
    stats) echo "${registry}/dronebros-stats:${tag}" ;;
    hmi) echo "${registry}/dronebros-hmi:${tag}" ;;
    video) echo "${registry}/dronebros-video:${tag}" ;;
    mavrouter) echo "${registry}/dronebros-mavrouter:${tag}" ;;
    telemetry) echo "${registry}/dronebros-telemetry:${tag}" ;;
    *)
      echo "[push] unknown service: $1 (expected stats, hmi, video, mavrouter, or telemetry)" >&2
      exit 2
      ;;
  esac
}

context_for() {
  case "$1" in
    stats|hmi|video|mavrouter|telemetry) echo "${ROOT}/services/$1" ;;
    *)
      echo "[push] unknown service: $1" >&2
      exit 2
      ;;
  esac
}

for svc in "${SERVICES[@]}"; do
  case "$svc" in
    stats|hmi|video|mavrouter|telemetry) ;;
    *)
      echo "[push] unknown service in list: $svc" >&2
      exit 2
      ;;
  esac
done

for svc in "${SERVICES[@]}"; do
  img="$(image_for "$svc")"
  ctx="$(context_for "$svc")"
  h="$(context_hash "$svc" || true)"
  if [[ "$SKIP_UNCHANGED" == "1" && "$FORCE" != "1" && -n "$h" ]]; then
    prev=""
    [[ -f "$HASH_DIR/pushed-${svc}.sha256" ]] && prev="$(cat "$HASH_DIR/pushed-${svc}.sha256")"
    if [[ "$h" == "$prev" ]]; then
      echo "[push] skip $svc (context unchanged since last push; use --force to rebuild)"
      continue
    fi
  fi
  echo "[push] buildx push $img ($PLATFORM_PUSH)"
  docker buildx build --platform "$PLATFORM_PUSH" -t "$img" --push "$ctx"
  h="$(context_hash "$svc" || true)"
  [[ -n "$h" ]] && printf '%s' "$h" >"$HASH_DIR/pushed-${svc}.sha256"
done

echo "[push] done."
