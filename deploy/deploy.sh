#!/usr/bin/env bash
# Push the stack to hosts: reads deploy/inventory + deploy/config/stack.yml, runs Ansible.
#
# Uses deploy/.venv/bin/ansible-playbook if present (run deploy/setup-venv.sh once),
# otherwise ansible-playbook on your PATH.
#
# Usage (from repo root):
#   bash ./deploy/deploy.sh
#   bash ./deploy/deploy.sh --limit pi
#   bash ./deploy/deploy.sh --inventory ./deploy/inventory/hosts.yml --config ./deploy/config/stack.yml
#   bash ./deploy/deploy.sh --private-key ~/.ssh/id_ed25519 --check
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export ANSIBLE_CONFIG="${HERE}/ansible/ansible.cfg"

INV="${HERE}/inventory/hosts.yml"
CFG="${HERE}/config/stack.yml"
PLAYBOOK="${HERE}/ansible/playbooks/deploy.yml"
LIMIT=""
CHECK=()
AP_EXTRA=()

usage() {
  sed -n '1,18p' "$0"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --inventory)
      INV="${2:?}"
      shift 2
      ;;
    --config)
      CFG="${2:?}"
      shift 2
      ;;
    --limit)
      LIMIT="${2:?}"
      shift 2
      ;;
    --private-key|-i)
      AP_EXTRA+=(--private-key "${2:?}")
      shift 2
      ;;
    --check)
      CHECK=(--check)
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

[[ -f "$INV" ]] || {
  echo "Inventory not found: $INV" >&2
  exit 2
}
[[ -f "$CFG" ]] || {
  echo "Stack config not found: $CFG" >&2
  exit 2
}

ANSIBLE_PLAYBOOK=""
if [[ -x "${HERE}/.venv/bin/ansible-playbook" ]]; then
  ANSIBLE_PLAYBOOK="${HERE}/.venv/bin/ansible-playbook"
elif command -v ansible-playbook >/dev/null 2>&1; then
  ANSIBLE_PLAYBOOK="ansible-playbook"
else
  echo "ansible-playbook not found. Run once:  bash ./deploy/setup-venv.sh" >&2
  exit 2
fi

CMD=(
  "$ANSIBLE_PLAYBOOK"
  "${CHECK[@]}"
  "${AP_EXTRA[@]}"
  "$PLAYBOOK"
  -i "$INV"
  -e "@${CFG}"
)
[[ -n "$LIMIT" ]] && CMD+=(--limit "$LIMIT")

echo "[deploy] inventory=$INV"
echo "[deploy] config=$CFG"
echo "[deploy] ansible-playbook=$ANSIBLE_PLAYBOOK"
exec "${CMD[@]}"
