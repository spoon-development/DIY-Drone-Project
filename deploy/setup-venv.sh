#!/usr/bin/env bash
# Create deploy/.venv and install Ansible (avoids Ubuntu/WSL PEP 668 system pip issues).
#
# Usage (from repo root):
#   bash ./deploy/setup-venv.sh
#
# Debian/Ubuntu needs venv support (once):
#   sudo apt update && sudo apt install -y python3-venv
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"

if ! command -v python3 >/dev/null 2>&1; then
  echo "python3 not found. On Ubuntu/WSL: sudo apt install -y python3" >&2
  exit 2
fi

PY_MINOR="$(python3 -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')"

# Drop a half-created venv (common after a failed first run).
if [[ -d .venv ]] && [[ ! -x .venv/bin/pip ]]; then
  echo "[setup-venv] Removing incomplete .venv (no pip inside)." >&2
  rm -rf .venv
fi

if [[ ! -d .venv ]]; then
  if ! python3 -m venv .venv; then
    rm -rf .venv 2>/dev/null || true
    echo "" >&2
    echo "python3 -m venv failed (ensurepip / venv module missing)." >&2
    echo "On Debian or Ubuntu (including WSL), run once:" >&2
    echo "  sudo apt update && sudo apt install -y python3-venv" >&2
    echo "If apt still does not match your python3, try:" >&2
    echo "  sudo apt install -y python${PY_MINOR}-venv" >&2
    exit 2
  fi
fi

if [[ ! -x .venv/bin/pip ]]; then
  echo ".venv is missing pip. Remove it and install python3-venv:" >&2
  echo "  rm -rf \"$HERE/.venv\" && sudo apt install -y python3-venv" >&2
  exit 2
fi

./.venv/bin/pip install -U pip
./.venv/bin/pip install ansible-core

echo "Done. ansible-playbook: $HERE/.venv/bin/ansible-playbook"
"$HERE/.venv/bin/ansible-playbook" --version
