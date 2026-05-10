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
#   bash ./deploy/deploy.sh --skip-pull   # Pi already has images (offline / manual load)
#
# Bundle images on this machine, copy to Pi, load, then deploy (no registry pull on Pi):
#   bash ./deploy/deploy.sh --ship <pi-host> <ssh-user>
#   bash ./deploy/deploy.sh -i ~/.ssh/id_ed25519 --ship 192.168.0.52 pi
# With --check, skips docker pull/save/scp/load (Ansible dry-run only).
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
SKIP_PULL_EXTRA=()
SSH_IDENTITY=""
SHIP_HOST=""
SHIP_USER=""

usage() {
  sed -n '1,25p' "$0"
}

ship_bundle_to_pi() {
  local reg tag stats hmi tmp ssh_base scp_base dest
  reg="$(grep -E '^stack_image_registry:' "$CFG" | head -1 | sed 's/^stack_image_registry:[[:space:]]*//;s/[[:space:]]*$//')"
  tag="$(grep -E '^stack_image_tag:' "$CFG" | head -1 | sed 's/^stack_image_tag:[[:space:]]*//;s/[[:space:]]*$//')"
  [[ -n "$reg" && -n "$tag" ]] || {
    echo "[deploy] Could not parse stack_image_registry / stack_image_tag from $CFG" >&2
    exit 2
  }
  stats="${reg}/dronebros-stats:${tag}"
  hmi="${reg}/dronebros-hmi:${tag}"

  command -v docker >/dev/null 2>&1 || {
    echo "[deploy] --ship requires docker on this machine" >&2
    exit 2
  }

  ssh_base=(ssh -o StrictHostKeyChecking=accept-new)
  scp_base=(scp -o StrictHostKeyChecking=accept-new)
  [[ -n "$SSH_IDENTITY" ]] && ssh_base+=(-i "$SSH_IDENTITY") && scp_base+=(-i "$SSH_IDENTITY")
  dest="${SHIP_USER}@${SHIP_HOST}"

  echo "[deploy] ship: pulling $stats"
  docker pull "$stats"
  echo "[deploy] ship: pulling $hmi"
  docker pull "$hmi"

  tmp="$(mktemp -d "${TMPDIR:-/tmp}/diy-drone-ship.XXXXXX")"
  trap 'rm -rf "$tmp"' RETURN

  echo "[deploy] ship: saving tarballs"
  docker save -o "$tmp/dronebros-stats.tar" "$stats"
  docker save -o "$tmp/dronebros-hmi.tar" "$hmi"

  echo "[deploy] ship: copying to $dest"
  "${scp_base[@]}" "$tmp/dronebros-stats.tar" "$tmp/dronebros-hmi.tar" "${dest}:"

  echo "[deploy] ship: loading on Pi"
  "${ssh_base[@]}" "$dest" "docker load -i dronebros-stats.tar && docker load -i dronebros-hmi.tar && rm -f dronebros-stats.tar dronebros-hmi.tar"

  rm -rf "$tmp"
  trap - RETURN
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
      SSH_IDENTITY="${2:?}"
      shift 2
      ;;
    --check)
      CHECK=(--check)
      shift
      ;;
    --skip-pull)
      SKIP_PULL_EXTRA=(-e skip_image_pull=true)
      shift
      ;;
    --ship)
      SHIP_HOST="${2:?}"
      SHIP_USER="${3:?}"
      shift 3
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

if [[ -n "$SHIP_HOST" ]]; then
  SKIP_PULL_EXTRA=(-e skip_image_pull=true)
  if [[ ${#CHECK[@]} -eq 0 ]]; then
    ship_bundle_to_pi
  else
    echo "[deploy] --check: skipping --ship bundle (no pull/save/scp/load)"
  fi
fi

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
  "${SKIP_PULL_EXTRA[@]}"
)
[[ -n "$LIMIT" ]] && CMD+=(--limit "$LIMIT")

echo "[deploy] inventory=$INV"
echo "[deploy] config=$CFG"
echo "[deploy] ansible-playbook=$ANSIBLE_PLAYBOOK"
exec "${CMD[@]}"
