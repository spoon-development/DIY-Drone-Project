#!/usr/bin/env bash
# Push the stack to hosts: reads deploy/inventory + deploy/config/stack.yml, runs Ansible.
#
# Default flow (no Pi registry access required):
#   1) docker pull --platform … both images on *this* machine (see stack_bundle_platform in stack.yml; needs internet + Docker; arm64 pull on amd64 may need QEMU/binfmt)
#   2) docker save → tarballs (on WSL, install skopeo: apt install skopeo — deploy falls back to skopeo copy if docker save errors)
#   3) scp tarballs to each target host from inventory (SSH)
#   4) ssh docker load on each Pi
#   5) ansible: compose file + .env on Pi, compose up (skip_image_pull true — no ghcr on Pi)
#
# Uses deploy/.venv/bin/ansible-playbook if you created deploy/.venv yourself, otherwise ansible-playbook on PATH
# (install: sudo apt install ansible-core   or   pipx install ansible-core).
#
# Usage (from repo root):
#   bash ./deploy/deploy.sh
#   bash ./deploy/deploy.sh --limit pi
#   bash ./deploy/deploy.sh --inventory ./deploy/inventory/hosts.yml --config ./deploy/config/stack.yml
#   bash ./deploy/deploy.sh --private-key ~/.ssh/id_ed25519
#   bash ./deploy/deploy.sh --skip-bundle   # Pi already has correct images; still skips registry pull on Pi
#   bash ./deploy/deploy.sh --pull-on-pi    # Pi pulls from registry (Pi must reach ghcr.io); no local bundle
#   bash ./deploy/deploy.sh --ship <host-or-ip> <ssh-user>   # bundle only to this SSH target (use with --limit if needed)
#   bash ./deploy/deploy.sh --check         # Ansible dry-run; skips bundle and docker steps
#   bash ./deploy/deploy.sh --ask-become-pass   # force sudo password prompt (same as -K)
#   bash ./deploy/deploy.sh --no-ask-become-pass
#   bash ./deploy/deploy.sh --ask-pass      # SSH password if you do not use ssh keys (-k)
#
# Default deploy dir is ~/dronebros (no sudo). This script adds -b --ask-become-pass only if inventory
# sets dronebros_deploy_dir under /opt or /srv (or another non-home path that needs root to create).
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
BECOME_FLAGS=()
EXPLICIT_ASK_BECOME=0
EXPLICIT_NO_ASK_BECOME=0
SSH_ASK_PASS=()
SSH_IDENTITY=""
# Default: bundle images from this machine to every Ansible target (Pi never pulls GHCR).
BUNDLE_IMAGES=1
PULL_ON_PI=0
SHIP_HOST=""
SHIP_USER=""

usage() {
  sed -n '1,45p' "$0"
}

_ansible_inv() {
  if [[ -x "${HERE}/.venv/bin/ansible-inventory" ]]; then
    echo "${HERE}/.venv/bin/ansible-inventory"
  elif command -v ansible-inventory >/dev/null 2>&1; then
    command -v ansible-inventory
  else
    return 1
  fi
}

# Return 0 if ansible-playbook should get -b --ask-become-pass (inventory uses /opt, /srv, etc.).
# OS/CPU for docker pull when bundling (must match the Pi). Override: env DEPLOY_BUNDLE_PLATFORM=linux/arm/v7
_bundle_platform_from_config() {
  local p cfg="${1:-$CFG}"
  p="$(grep -E '^stack_bundle_platform:' "$cfg" 2>/dev/null | head -1 | sed 's/^stack_bundle_platform:[[:space:]]*//;s/[[:space:]]*$//')"
  p="${p//\"/}"
  p="${p//\'/}"
  [[ -n "$p" ]] || p="${DEPLOY_BUNDLE_PLATFORM:-linux/arm64}"
  echo "$p"
}

# skopeo --override-* selects one variant from the registry manifest (avoids broken docker save on some WSL setups).
_bundle_skopeo_flags() {
  case "${1:-linux/arm64}" in
    linux/arm/v7|linux/armhf) echo --override-os linux --override-arch arm --override-variant v7 ;;
    linux/amd64) echo --override-os linux --override-arch amd64 ;;
    *) echo --override-os linux --override-arch arm64 ;;
  esac
}

# Save by image ID first; on failure use skopeo (docker-archive) if installed; then re-pull and retry once.
_bundle_save_images_retry() {
  local stats_ref="$1" hmi_ref="$2" plat="$3" stats_tar="$4" hmi_tar="$5"
  local attempt sid hid
  local -a skf

  for attempt in 1 2; do
    sid="$(docker image inspect --format '{{.Id}}' "$stats_ref")" || {
      echo "[deploy] docker image inspect failed for $stats_ref" >&2
      return 1
    }
    hid="$(docker image inspect --format '{{.Id}}' "$hmi_ref")" || {
      echo "[deploy] docker image inspect failed for $hmi_ref" >&2
      return 1
    }
    echo "[deploy] bundle: docker save → $(basename "$stats_tar") $(basename "$hmi_tar") (ids ${sid:0:12}… ${hid:0:12}…)"
    if docker save -o "$stats_tar" "$sid" && docker save -o "$hmi_tar" "$hid"; then
      return 0
    fi

    if command -v skopeo >/dev/null 2>&1; then
      echo "[deploy] bundle: docker save failed (common on WSL + multi-arch); trying skopeo copy…" >&2
      rm -f "$stats_tar" "$hmi_tar"
      read -r -a skf <<<"$(_bundle_skopeo_flags "$plat")"
      if skopeo copy "${skf[@]}" "docker://${stats_ref}" "docker-archive:${stats_tar}:${stats_ref}" &&
        skopeo copy "${skf[@]}" "docker://${hmi_ref}" "docker-archive:${hmi_tar}:${hmi_ref}"; then
        return 0
      fi
      echo "[deploy] bundle: skopeo copy also failed." >&2
    else
      echo "[deploy] bundle: skopeo not installed — it usually fixes this error on WSL." >&2
      echo "  Install:  sudo apt update && sudo apt install -y skopeo" >&2
    fi

    if [[ "$attempt" -ge 2 ]]; then
      echo "[deploy] bundle: giving up. Optional: docker system prune -af  then re-run (clears broken local blobs)." >&2
      return 1
    fi
    echo "[deploy] bundle: removing local tags and re-pulling for $plat…" >&2
    docker rmi -f "$stats_ref" "$hmi_ref" 2>/dev/null || true
    docker pull --platform "$plat" "$stats_ref"
    docker pull --platform "$plat" "$hmi_ref"
  done
  return 1
}

_deploy_inventory_suggests_sudo() {
  local inv="$1" line val found=0 any_system=0 any_home=0
  while IFS= read -r line; do
    [[ "$line" =~ dronebros_deploy_dir[[:space:]]*: ]] || continue
    val="${line#*:}"
    val="${val//[[:space:]]/}"
    val="${val//\"/}"
    val="${val//\'/}"
    [[ -z "$val" ]] && continue
    found=1
    case "$val" in
      /opt/*|/opt|/srv/*|/srv|/usr/local/*|/var/opt/*) any_system=1 ;;
      /home/*) any_home=1 ;;
    esac
  done < <(grep -E 'dronebros_deploy_dir[[:space:]]*:' "$inv" 2>/dev/null | grep -Ev '^[[:space:]]*#' || true)

  [[ "$any_system" -eq 1 ]] && return 0
  [[ "$found" -eq 0 ]] && return 1
  [[ "$any_home" -eq 1 && "$any_system" -eq 0 ]] && return 1
  return 0
}

# Pull/save locally, then scp + docker load on dest (SSH_USER@SSH_HOST).
_bundle_push_one_target() {
  local reg tag stats hmi plat tmp ssh_base scp_base dest
  local ssh_host="$1" ssh_user="$2"

  reg="$(grep -E '^stack_image_registry:' "$CFG" | head -1 | sed 's/^stack_image_registry:[[:space:]]*//;s/[[:space:]]*$//')"
  tag="$(grep -E '^stack_image_tag:' "$CFG" | head -1 | sed 's/^stack_image_tag:[[:space:]]*//;s/[[:space:]]*$//')"
  [[ -n "$reg" && -n "$tag" ]] || {
    echo "[deploy] Could not parse stack_image_registry / stack_image_tag from $CFG" >&2
    exit 2
  }
  stats="${reg}/dronebros-stats:${tag}"
  hmi="${reg}/dronebros-hmi:${tag}"
  plat="$(_bundle_platform_from_config)"

  command -v docker >/dev/null 2>&1 || {
    echo "[deploy] Docker is required on this machine to pull and save images." >&2
    exit 2
  }

  ssh_base=(ssh -o StrictHostKeyChecking=accept-new)
  scp_base=(scp -o StrictHostKeyChecking=accept-new)
  [[ -n "$SSH_IDENTITY" ]] && ssh_base+=(-i "$SSH_IDENTITY") && scp_base+=(-i "$SSH_IDENTITY")
  dest="${ssh_user}@${ssh_host}"

  echo "[deploy] bundle: pulling (this machine) $stats for $plat"
  docker pull --platform "$plat" "$stats"
  echo "[deploy] bundle: pulling (this machine) $hmi for $plat"
  docker pull --platform "$plat" "$hmi"

  tmp="$(mktemp -d "${TMPDIR:-/tmp}/diy-drone-bundle.XXXXXX")"
  trap 'rm -rf "$tmp"' RETURN

  _bundle_save_images_retry "$stats" "$hmi" "$plat" "$tmp/dronebros-stats.tar" "$tmp/dronebros-hmi.tar"

  echo "[deploy] bundle: copying to $dest"
  "${scp_base[@]}" "$tmp/dronebros-stats.tar" "$tmp/dronebros-hmi.tar" "${dest}:"

  echo "[deploy] bundle: loading on $dest"
  "${ssh_base[@]}" "$dest" "docker load -i dronebros-stats.tar && docker load -i dronebros-hmi.tar && rm -f dronebros-stats.tar dronebros-hmi.tar"

  rm -rf "$tmp"
  trap - RETURN
}

# One pull/save on this machine, then push the same tarballs to every inventory host (avoids duplicate pulls).
_bundle_push_inventory_hosts() {
  local reg tag stats hmi plat tmp ssh_base scp_base ansible_inv hosts inv_name

  reg="$(grep -E '^stack_image_registry:' "$CFG" | head -1 | sed 's/^stack_image_registry:[[:space:]]*//;s/[[:space:]]*$//')"
  tag="$(grep -E '^stack_image_tag:' "$CFG" | head -1 | sed 's/^stack_image_tag:[[:space:]]*//;s/[[:space:]]*$//')"
  [[ -n "$reg" && -n "$tag" ]] || {
    echo "[deploy] Could not parse stack_image_registry / stack_image_tag from $CFG" >&2
    exit 2
  }
  stats="${reg}/dronebros-stats:${tag}"
  hmi="${reg}/dronebros-hmi:${tag}"
  plat="$(_bundle_platform_from_config)"

  command -v docker >/dev/null 2>&1 || {
    echo "[deploy] Docker is required on this machine to pull and save images." >&2
    exit 2
  }
  ansible_inv="$(_ansible_inv)" || {
    echo "[deploy] ansible-inventory not found. Install: sudo apt install ansible-core  (or pipx install ansible-core)" >&2
    exit 2
  }

  ssh_base=(ssh -o StrictHostKeyChecking=accept-new)
  scp_base=(scp -o StrictHostKeyChecking=accept-new)
  [[ -n "$SSH_IDENTITY" ]] && ssh_base+=(-i "$SSH_IDENTITY") && scp_base+=(-i "$SSH_IDENTITY")

  echo "[deploy] bundle: pulling (this machine) $stats for $plat"
  docker pull --platform "$plat" "$stats"
  echo "[deploy] bundle: pulling (this machine) $hmi for $plat"
  docker pull --platform "$plat" "$hmi"

  tmp="$(mktemp -d "${TMPDIR:-/tmp}/diy-drone-bundle.XXXXXX")"
  trap 'rm -rf "$tmp"' RETURN

  _bundle_save_images_retry "$stats" "$hmi" "$plat" "$tmp/dronebros-stats.tar" "$tmp/dronebros-hmi.tar"

  # Use ansible-inventory JSON (ansible --list-hosts prints human headers in 2.16+, not plain names).
  # Pass --limit via env: with python -c, argv after the script string is not reliable for this pipeline.
  mapfile -t hosts < <(
    export DEPLOY_BUNDLE_LIMIT="${LIMIT:-}"
    export DEPLOY_BUNDLE_INV="$INV"
    "$ansible_inv" -i "$INV" --list 2>/dev/null | python3 -c "
import json, sys, os
limit = (os.environ.get('DEPLOY_BUNDLE_LIMIT') or '').strip()
inv_path = os.environ.get('DEPLOY_BUNDLE_INV') or ''
inv = json.load(sys.stdin)
names = set()
for gname, gbody in inv.items():
    if gname.startswith('_'):
        continue
    if isinstance(gbody, dict):
        for h in gbody.get('hosts') or []:
            names.add(h)
meta = inv.get('_meta') or {}
for h in (meta.get('hostvars') or {}):
    names.add(h)
out = sorted(names)
if limit:
    want = {x.strip() for x in limit.split(',') if x.strip()}
    out = [h for h in out if h in want]
    missing = want - set(out)
    if missing:
        sys.stderr.write('[deploy] --limit host(s) not in inventory: ' + ', '.join(sorted(missing)) + '\n')
        sys.exit(1)
if not out:
    sys.stderr.write('[deploy] No hosts in inventory JSON from ' + inv_path + '\n')
    sys.exit(1)
for h in out:
    print(h)
"
  )
  [[ "${#hosts[@]}" -gt 0 ]] || {
    echo "[deploy] No hosts resolved from inventory $INV." >&2
    exit 2
  }

  for inv_name in "${hosts[@]}"; do
    [[ -z "${inv_name// }" ]] && continue
    read -r ssh_host ssh_user <<<"$("$ansible_inv" -i "$INV" --host "$inv_name" | python3 -c "
import json, sys
d = json.load(sys.stdin)
h = d.get('ansible_host') or d.get('inventory_hostname') or ''
u = d.get('ansible_user') or 'root'
print(h, u)
")"
    [[ -n "$ssh_host" ]] || {
      echo "[deploy] Could not resolve ansible_host for inventory host '$inv_name'." >&2
      exit 2
    }
    dest="${ssh_user}@${ssh_host}"
    echo "[deploy] bundle: copying to $dest ($inv_name)"
    "${scp_base[@]}" "$tmp/dronebros-stats.tar" "$tmp/dronebros-hmi.tar" "${dest}:"
    echo "[deploy] bundle: loading on $dest"
    "${ssh_base[@]}" "$dest" "docker load -i dronebros-stats.tar && docker load -i dronebros-hmi.tar && rm -f dronebros-stats.tar dronebros-hmi.tar"
  done

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
      echo "[deploy] warning: --skip-pull is deprecated; use --skip-bundle (same: no local pull/save/scp)." >&2
      BUNDLE_IMAGES=0
      SKIP_PULL_EXTRA=(-e skip_image_pull=true)
      shift
      ;;
    --skip-bundle)
      BUNDLE_IMAGES=0
      SKIP_PULL_EXTRA=(-e skip_image_pull=true)
      shift
      ;;
    --pull-on-pi)
      PULL_ON_PI=1
      BUNDLE_IMAGES=0
      SKIP_PULL_EXTRA=()
      shift
      ;;
    --ask-become-pass|-K)
      EXPLICIT_ASK_BECOME=1
      shift
      ;;
    --no-ask-become-pass)
      EXPLICIT_NO_ASK_BECOME=1
      shift
      ;;
    --ask-pass|-k)
      SSH_ASK_PASS=(--ask-pass)
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

# Ansible dry-run: never touch Docker locally or over SSH for the bundle.
if [[ ${#CHECK[@]} -gt 0 ]]; then
  BUNDLE_IMAGES=0
fi

if [[ ${#CHECK[@]} -eq 0 && "$BUNDLE_IMAGES" -eq 1 && "$PULL_ON_PI" -eq 0 ]]; then
  SKIP_PULL_EXTRA=(-e skip_image_pull=true)
  if [[ -n "$SHIP_HOST" ]]; then
    _bundle_push_one_target "$SHIP_HOST" "$SHIP_USER"
  else
    _bundle_push_inventory_hosts
  fi
elif [[ "$PULL_ON_PI" -eq 1 ]]; then
  # Force registry pull on the Pi (overrides stack.yml); Pi must reach ghcr.io.
  SKIP_PULL_EXTRA=(-e skip_image_pull=false)
else
  # --skip-bundle / deprecated --skip-pull: images must already exist on the Pi.
  if [[ ${#SKIP_PULL_EXTRA[@]} -eq 0 && ${#CHECK[@]} -eq 0 ]]; then
    SKIP_PULL_EXTRA=(-e skip_image_pull=true)
  fi
fi

# Ansible needs -b (--become) plus -K (--ask-become-pass) for the sudo password prompt to appear.
if [[ "$EXPLICIT_NO_ASK_BECOME" -eq 1 ]]; then
  if _deploy_inventory_suggests_sudo "$INV"; then
    BECOME_FLAGS=(-b)
  fi
elif [[ "$EXPLICIT_ASK_BECOME" -eq 1 ]] || _deploy_inventory_suggests_sudo "$INV"; then
  BECOME_FLAGS=(-b --ask-become-pass)
  echo "" >&2
  echo "[deploy] Sudo required for deploy path — you will see: BECOME password:" >&2
  echo "[deploy] Enter the Pi user's sudo password (nothing echoes; that is normal.)" >&2
  echo "" >&2
fi

ANSIBLE_PLAYBOOK=""
if [[ -x "${HERE}/.venv/bin/ansible-playbook" ]]; then
  ANSIBLE_PLAYBOOK="${HERE}/.venv/bin/ansible-playbook"
elif command -v ansible-playbook >/dev/null 2>&1; then
  ANSIBLE_PLAYBOOK="ansible-playbook"
else
  echo "ansible-playbook not found. Install: sudo apt install ansible-core  (or: cd deploy && python3 -m venv .venv && . .venv/bin/activate && pip install ansible-core)" >&2
  exit 2
fi

CMD=(
  "$ANSIBLE_PLAYBOOK"
  "${SSH_ASK_PASS[@]}"
  "${BECOME_FLAGS[@]}"
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
