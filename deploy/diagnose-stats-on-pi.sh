#!/usr/bin/env bash
# Diagnose HMI -> stats (HTTP 502 on /api/stats). Intended to run ON the Raspberry Pi
# where ~/dronebros/docker-compose.yml lives.
#
# Recommended — from your PC, repo root (nothing is copied to the Pi by default):
#   ssh spoon@192.168.0.52 'bash -s' < deploy/diagnose-stats-on-pi.sh
#
# If you already copied this file onto the Pi (e.g. to ~):
#   bash ~/diagnose-stats-on-pi.sh
#
# Non-default deploy directory on the Pi:
#   DRONEBROS_DIR=/opt/dronebros bash ./diagnose-stats-on-pi.sh
#
set -uo pipefail

DRONEBROS_DIR="${DRONEBROS_DIR:-$HOME/dronebros}"

echo "========== $(date) =========="
echo "hostname: $(hostname)"
echo "deploy dir: ${DRONEBROS_DIR}"

if [[ ! -d "$DRONEBROS_DIR" ]]; then
  echo "ERROR: directory not found: $DRONEBROS_DIR"
  echo "Create it with your normal deploy, or set DRONEBROS_DIR to where compose lives."
  echo "From your laptop (repo root), run the script on the Pi without copying files:"
  echo "  ssh spoon@<pi-ip> 'bash -s' < deploy/diagnose-stats-on-pi.sh"
  exit 1
fi

cd "$DRONEBROS_DIR" || exit 1

echo "---------- docker compose ps ----------"
docker compose ps -a || true

echo "---------- stats: health from inside stats container ----------"
docker exec dronebros-stats python3 -c "import urllib.request; print(urllib.request.urlopen('http://127.0.0.1:9101/health', timeout=5).read())" 2>&1 || echo "(failed: is dronebros-stats running?)"

echo "---------- stats: GET /metrics (first 200 chars) ----------"
docker exec dronebros-stats python3 -c "import urllib.request; r=urllib.request.urlopen('http://127.0.0.1:9101/metrics', timeout=15); b=r.read(500); print(b[:200])" 2>&1 || true

echo "---------- hmi: DNS for service name 'stats' ----------"
docker exec dronebros-hmi python -c "import socket; print(socket.gethostbyname('stats'))" 2>&1 || echo "(dns or exec failed)"

echo "---------- hmi -> http://stats:9101/health ----------"
docker exec dronebros-hmi python -c "import urllib.request; print(urllib.request.urlopen('http://stats:9101/health', timeout=10).read())" 2>&1 || echo "(FAILED — same failure mode as browser 502)"

echo "---------- hmi -> http://dronebros-stats:9101/health ----------"
docker exec dronebros-hmi python -c "import urllib.request; print(urllib.request.urlopen('http://dronebros-stats:9101/health', timeout=10).read())" 2>&1 || echo "(FAILED alternate host)"

echo "---------- hmi env (STATS*, DISPLAY*) ----------"
docker exec dronebros-hmi sh -c 'env | grep -E "^(STATS|DISPLAY)_" || true' 2>&1 || true

echo "---------- compose project network (names) ----------"
docker network ls --filter name=dronebros --format '{{.Name}}' || true

echo "---------- tail logs ----------"
docker compose logs --tail=50 stats 2>&1 || true
docker compose logs --tail=50 hmi 2>&1 || true

echo "---------- .env stack lines ----------"
if [[ -f .env ]]; then grep -E '^(IMAGE_|HMI_|DISPLAY_)' .env || true; else echo "(no .env)"; fi

echo "========== done =========="
