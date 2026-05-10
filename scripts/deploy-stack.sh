#!/usr/bin/env bash
# Deploy stats + HMI stack using Ansible (recommended): copies compose + .env seed, pull, up.
#
# Requires: ansible-playbook on PATH, SSH to the Pi, Docker + Compose v2 on the Pi.
#
# One-liner (from repo root):
#   ./scripts/deploy-stack.sh 192.168.1.50 pi
#
# Flags:
#   ./scripts/deploy-stack.sh -H pi.local -u pi --skip-pull
#   ./scripts/deploy-stack.sh 192.168.1.50 pi -i ~/.ssh/id_ed25519

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export ANSIBLE_CONFIG="${ROOT}/deploy/ansible/ansible.cfg"

HOST=""
USER=""
IDENTITY=""
SKIP_PULL="0"

if [[ $# -ge 2 && "${1:-}" != -* ]]; then
  HOST="$1"
  USER="$2"
  shift 2
fi

while [[ $# -gt 0 ]]; do
  case "$1" in
    -H|--host) HOST="${2:?}"; shift 2 ;;
    -u|--user) USER="${2:?}"; shift 2 ;;
    -i|--identity) IDENTITY="${2:?}"; shift 2 ;;
    --skip-pull) SKIP_PULL="1"; shift ;;
    -h|--help) head -n 22 "$0"; exit 0 ;;
    *) echo "Unknown: $1" >&2; exit 2 ;;
  esac
done

[[ -n "$HOST" && -n "$USER" ]] || { echo "Need host and user (e.g. ./scripts/deploy-stack.sh 192.168.1.50 pi)" >&2; exit 2; }

command -v ansible-playbook >/dev/null 2>&1 || {
  echo "ansible-playbook not found. Install Ansible or use: ./scripts/deploy-remote.sh ${HOST} ${USER}" >&2
  exit 127
}

PLAYBOOK="${ROOT}/deploy/ansible/playbooks/deploy_stack.yml"
INV="${HOST},"
SKIP_VAR="false"
[[ "$SKIP_PULL" == "1" ]] && SKIP_VAR="true"

ANSIBLE_SSH_ARGS=()
[[ -n "$IDENTITY" ]] && ANSIBLE_SSH_ARGS=(--private-key "$IDENTITY")

ansible-playbook "${ANSIBLE_SSH_ARGS[@]}" \
  "$PLAYBOOK" \
  -i "$INV" \
  -e "ansible_user=${USER}" \
  -e "skip_image_pull=${SKIP_VAR}"
