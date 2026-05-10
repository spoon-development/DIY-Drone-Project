#!/usr/bin/env bash
# On a machine with Docker: save pre-pulled arm64 images to a tar for offline transfer.
# Example (after logging in to ghcr.io and pulling):
#   docker pull ghcr.io/spoon-development/dronebros-stats:latest
#   docker pull ghcr.io/spoon-development/dronebros-hmi:latest
#   ./scripts/export-images.sh ./dronebros-images.tar
set -euo pipefail
OUT="${1:?usage: export-images.sh /path/to/out.tar}"
REG="${IMAGE_REGISTRY:-ghcr.io/spoon-development}"
TAG="${IMAGE_TAG:-latest}"
docker save "${REG}/dronebros-stats:${TAG}" "${REG}/dronebros-hmi:${TAG}" -o "${OUT}"
echo "Wrote ${OUT} — copy to Pi, then: docker load -i $(basename "${OUT}")"
