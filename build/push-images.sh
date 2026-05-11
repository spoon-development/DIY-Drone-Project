#!/usr/bin/env bash
# Push stats + HMI images to GHCR using buildx (after: docker login ghcr.io).
# Reads registry/tag from deploy/config/stack.yml — same names the Pi pulls.
#
# Usage (from repo root):
#   bash ./build/push-images.sh
#   bash ./build/push-images.sh --image multi
#   bash ./build/push-images.sh --image amd64
#   bash ./build/push-images.sh --image arm64
#
# --image multi (default)  → linux/amd64,linux/arm64 manifest
# --image amd64|arm64      → single-platform image push
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CFG="${ROOT}/deploy/config/stack.yml"
PLATFORM_PUSH="linux/amd64,linux/arm64"

usage() {
  sed -n '1,18p' "$0"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
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
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

[[ -f "$CFG" ]] || {
  echo "[push] Missing $CFG" >&2
  exit 2
}

registry="$(grep -E '^stack_image_registry:' "$CFG" | head -1 | sed 's/^stack_image_registry:[[:space:]]*//;s/[[:space:]]*$//')"
tag="$(grep -E '^stack_image_tag:' "$CFG" | head -1 | sed 's/^stack_image_tag:[[:space:]]*//;s/[[:space:]]*$//')"
[[ -n "$registry" && -n "$tag" ]] || {
  echo "[push] Could not parse stack_image_registry / stack_image_tag from $CFG" >&2
  exit 2
}

stats="${registry}/dronebros-stats:${tag}"
hmi="${registry}/dronebros-hmi:${tag}"

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

echo "[push] buildx push $stats ($PLATFORM_PUSH)"
docker buildx build --platform "$PLATFORM_PUSH" -t "$stats" --push "${ROOT}/services/stats"
echo "[push] buildx push $hmi ($PLATFORM_PUSH)"
docker buildx build --platform "$PLATFORM_PUSH" -t "$hmi" --push "${ROOT}/services/hmi"
echo "[push] done."
