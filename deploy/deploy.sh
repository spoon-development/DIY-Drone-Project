#!/usr/bin/env bash
# Push the stack to hosts: reads deploy/inventory + deploy/config/stack.yml, runs Ansible.
#
# Image bundle modes (pick one):
#   --local      Ship images already built on this machine (run build/build.sh first).
#                No GHCR pull or push. Optional --build to rebuild inside deploy.
#   (default)    Pull from GHCR on this machine, then save/scp/load (same as --registry).
#   --registry   Same as default; explicit pull-from-GHCR then ship.
#   --skip-bundle  Skip image transfer; Pi must already have the right tags loaded.
#   --pull-on-pi   Pi pulls from registry directly (Pi must reach ghcr.io).
#
# Default flow (--registry):
#   1) docker pull --platform … stack images on *this* machine (see stack_bundle_platform in stack.yml)
#   2) docker save → tarballs
#   3) scp tarballs to each target host from inventory (SSH)
#   4) ssh docker load on each Pi
#   5) ansible: compose file + .env on Pi, compose up (skip_image_pull true — no ghcr on Pi)
#
# Local flow (--local):
#   1) You run bash ./build/build.sh as usual (or pass --build to deploy to rebuild)
#   2) docker save local tags → scp → docker load on Pi
#   3) ansible as above
#
# Usage (from repo root):
#   bash ./build/build.sh --arm              # your normal build
#   bash ./deploy/deploy.sh --local          # ship local images (edit ansible_host in inventory first)
#   bash ./build/push-images.sh              # after testing, publish to GHCR
#   bash ./deploy/deploy.sh                  # pull from GHCR + deploy
#   bash ./deploy/deploy.sh --limit pi
#   bash ./deploy/deploy.sh --skip-bundle
#   bash ./deploy/deploy.sh --pull-on-pi
#   bash ./deploy/deploy.sh --check
#
# Uses deploy/.venv/bin/ansible-playbook if you created deploy/.venv yourself, otherwise ansible-playbook on PATH
# (install: sudo apt install ansible-core   or   pipx install ansible-core).
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/.." && pwd)"
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
# Default: bundle images from GHCR on this machine to every Ansible target (Pi never pulls GHCR).
BUNDLE_IMAGES=1
BUNDLE_SOURCE=registry
LOCAL_BUILD=0
PULL_ON_PI=0
SHIP_HOST=""
SHIP_USER=""

usage() {
  sed -n '1,40p' "$0"
}

_stack_image_refs() {
  local reg tag
  reg="$(grep -E '^stack_image_registry:' "$CFG" | head -1 | sed 's/^stack_image_registry:[[:space:]]*//;s/[[:space:]]*$//')"
  tag="$(grep -E '^stack_image_tag:' "$CFG" | head -1 | sed 's/^stack_image_tag:[[:space:]]*//;s/[[:space:]]*$//')"
  reg="${reg//\"/}"
  reg="${reg//\'/}"
  tag="${tag//\"/}"
  tag="${tag//\'/}"
  [[ -n "$reg" && -n "$tag" ]] || {
    echo "[deploy] Could not parse stack_image_registry / stack_image_tag from $CFG" >&2
    exit 2
  }
  STACK_REG="$reg"
  STACK_TAG="$tag"
  STACK_STATS="${reg}/dronebros-stats:${tag}"
  STACK_HMI="${reg}/dronebros-hmi:${tag}"
  STACK_VIDEO="${reg}/dronebros-video:${tag}"
}

_bundle_build_platform_flag() {
  case "$(_bundle_platform_from_config)" in
    linux/amd64) echo --amd ;;
    *) echo --arm ;;
  esac
}

_bundle_ensure_local_images() {
  _stack_image_refs
  local plat build_flag
  plat="$(_bundle_platform_from_config)"
  if [[ "$LOCAL_BUILD" -eq 1 ]]; then
    build_flag="$(_bundle_build_platform_flag)"
    echo "[deploy] local: rebuilding (build/build.sh ${build_flag} --force) for $plat"
    bash "${ROOT}/build/build.sh" "$build_flag" --force
  else
    echo "[deploy] local: shipping existing local tags (${STACK_REG}/dronebros-*:${STACK_TAG})"
    echo "[deploy] local: (run bash ./build/build.sh first, or pass --build to rebuild here)"
  fi
  for r in "$STACK_STATS" "$STACK_HMI" "$STACK_VIDEO"; do
    if ! docker image inspect "$r" >/dev/null 2>&1; then
      echo "[deploy] local: missing image $r" >&2
      echo "[deploy] Run: bash ./build/build.sh $(_bundle_build_platform_flag)" >&2
      echo "[deploy] Or:  bash ./deploy/deploy.sh --local --build" >&2
      exit 1
    fi
  done
}

_bundle_pull_registry_images() {
  _stack_image_refs
  local plat="$(_bundle_platform_from_config)"
  echo "[deploy] registry: pulling $STACK_STATS for $plat"
  docker pull --platform "$plat" "$STACK_STATS"
  echo "[deploy] registry: pulling $STACK_HMI for $plat"
  docker pull --platform "$plat" "$STACK_HMI"
  echo "[deploy] registry: pulling $STACK_VIDEO for $plat"
  docker pull --platform "$plat" "$STACK_VIDEO"
}

_bundle_acquire_images() {
  command -v docker >/dev/null 2>&1 || {
    echo "[deploy] Docker is required on this machine." >&2
    exit 2
  }
  if [[ "$BUNDLE_SOURCE" == local ]]; then
    _bundle_ensure_local_images
  else
    _bundle_pull_registry_images
  fi
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
# Usage: _bundle_save_images_retry PLATFORM IMAGE TAR [IMAGE TAR ...]
_bundle_save_images_retry() {
  local plat="$1" attempt sid
  shift
  local -a refs=() tars=()
  while [[ $# -ge 2 ]]; do
    refs+=("$1")
    tars+=("$2")
    shift 2
  done
  [[ ${#refs[@]} -ge 1 ]] || return 1

  for attempt in 1 2; do
    local -a ids=()
    for r in "${refs[@]}"; do
      sid="$(docker image inspect --format '{{.Id}}' "$r")" || {
        echo "[deploy] docker image inspect failed for $r" >&2
        return 1
      }
      ids+=("$sid")
    done

    local names=()
    for ((i = 0; i < ${#refs[@]}; i++)); do
      names+=("$(basename "${tars[i]}")")
    done
    echo "[deploy] bundle: docker save → ${names[*]} (platform $plat)"

    local ok=1
    for ((i = 0; i < ${#refs[@]}; i++)); do
      # Prefer tagged ref so docker load restores :latest; fall back to ID (WSL multi-arch).
      if ! docker save -o "${tars[i]}" "${refs[i]}" && ! docker save -o "${tars[i]}" "${ids[i]}"; then
        ok=0
        break
      fi
    done
    if [[ "$ok" -eq 1 ]]; then
      return 0
    fi

    if command -v skopeo >/dev/null 2>&1; then
      echo "[deploy] bundle: docker save failed (common on WSL + multi-arch); trying skopeo copy…" >&2
      for t in "${tars[@]}"; do rm -f "$t"; done
      read -r -a skf <<<"$(_bundle_skopeo_flags "$plat")"
      local sk_ok=1
      for ((i = 0; i < ${#refs[@]}; i++)); do
        if ! skopeo copy "${skf[@]}" "docker://${refs[i]}" "docker-archive:${tars[i]}:${refs[i]}"; then
          sk_ok=0
          break
        fi
      done
      if [[ "$sk_ok" -eq 1 ]]; then
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
    if [[ "${BUNDLE_SOURCE:-registry}" != registry ]]; then
      echo "[deploy] bundle: save failed; not re-pulling from registry (--local mode)." >&2
      return 1
    fi
    echo "[deploy] bundle: removing local tags and re-pulling for $plat…" >&2
    for r in "${refs[@]}"; do docker rmi -f "$r" 2>/dev/null || true; done
    for r in "${refs[@]}"; do docker pull --platform "$plat" "$r"; done
  done
  return 1
}

# Default deploy dir is ~/dronebros (no sudo). This script adds -b --ask-become-pass if inventory
# sets dronebros_deploy_dir under /opt or /srv, or stack.yml enables mipi_camera_configure (boot config).
_deploy_stack_mipi_camera() {
  local cfg="${1:-$CFG}"
  local v
  v="$(grep -E '^mipi_camera_configure:' "$cfg" 2>/dev/null | head -1 | sed 's/^mipi_camera_configure:[[:space:]]*//;s/[[:space:]]*$//')"
  v="${v//\"/}"
  v="${v//\'/}"
  [[ -z "$v" || "$v" == "true" ]]
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

# docker save by image ID drops repo tags; retag after load so compose :latest is the shipped image.
_bundle_ssh_load_images() {
  local dest="$1"
  shift
  local -a ssh_base=("$@")
  _stack_image_refs
  "${ssh_base[@]}" "$dest" bash -s -- "$STACK_STATS" "$STACK_HMI" "$STACK_VIDEO" <<'EOF'
set -euo pipefail
load_tag() {
  local tar="$1" ref="$2"
  local line id loaded_ref
  line="$(docker load -i "$tar" | tail -1)"
  id="$(echo "$line" | sed -n 's/^Loaded image ID: //p')"
  if [[ -z "$id" ]]; then
    loaded_ref="$(echo "$line" | sed -n 's/^Loaded image: //p')"
    if [[ -n "$loaded_ref" ]]; then
      echo "[deploy] bundle: loaded $loaded_ref"
      if [[ "$loaded_ref" != "$ref" ]]; then
        docker tag "$loaded_ref" "$ref"
        echo "[deploy] bundle: tagged $ref"
      fi
      rm -f "$tar"
      return
    fi
    echo "[deploy] docker load produced no image ID for $tar" >&2
    exit 1
  fi
  docker tag "$id" "$ref"
  echo "[deploy] bundle: tagged $ref"
  rm -f "$tar"
}
load_tag dronebros-stats.tar "$1"
load_tag dronebros-hmi.tar "$2"
load_tag dronebros-video.tar "$3"
EOF
}

# Pull/save locally, then scp + docker load on dest (SSH_USER@SSH_HOST).
_bundle_push_one_target() {
  local plat tmp ssh_base scp_base dest
  local ssh_host="$1" ssh_user="$2"

  plat="$(_bundle_platform_from_config)"
  _stack_image_refs

  ssh_base=(ssh -o StrictHostKeyChecking=accept-new)
  scp_base=(scp -o StrictHostKeyChecking=accept-new)
  [[ -n "$SSH_IDENTITY" ]] && ssh_base+=(-i "$SSH_IDENTITY") && scp_base+=(-i "$SSH_IDENTITY")
  dest="${ssh_user}@${ssh_host}"

  _bundle_acquire_images

  tmp="$(mktemp -d "${TMPDIR:-/tmp}/diy-drone-bundle.XXXXXX")"
  trap 'rm -rf "$tmp"' RETURN

  _bundle_save_images_retry "$plat" "$STACK_STATS" "$tmp/dronebros-stats.tar" "$STACK_HMI" "$tmp/dronebros-hmi.tar" "$STACK_VIDEO" "$tmp/dronebros-video.tar"

  echo "[deploy] bundle ($BUNDLE_SOURCE): copying to $dest"
  "${scp_base[@]}" "$tmp/dronebros-stats.tar" "$tmp/dronebros-hmi.tar" "$tmp/dronebros-video.tar" "${dest}:"

  echo "[deploy] bundle: loading on $dest"
  _bundle_ssh_load_images "$dest" "${ssh_base[@]}"

  rm -rf "$tmp"
  trap - RETURN
}

# One acquire/save on this machine, then push the same tarballs to every inventory host.
_bundle_push_inventory_hosts() {
  local plat tmp ssh_base scp_base ansible_inv hosts inv_name

  plat="$(_bundle_platform_from_config)"
  _stack_image_refs

  ansible_inv="$(_ansible_inv)" || {
    echo "[deploy] ansible-inventory not found. Install: sudo apt install ansible-core  (or pipx install ansible-core)" >&2
    exit 2
  }

  ssh_base=(ssh -o StrictHostKeyChecking=accept-new)
  scp_base=(scp -o StrictHostKeyChecking=accept-new)
  [[ -n "$SSH_IDENTITY" ]] && ssh_base+=(-i "$SSH_IDENTITY") && scp_base+=(-i "$SSH_IDENTITY")

  _bundle_acquire_images

  tmp="$(mktemp -d "${TMPDIR:-/tmp}/diy-drone-bundle.XXXXXX")"
  trap 'rm -rf "$tmp"' RETURN

  _bundle_save_images_retry "$plat" "$STACK_STATS" "$tmp/dronebros-stats.tar" "$STACK_HMI" "$tmp/dronebros-hmi.tar" "$STACK_VIDEO" "$tmp/dronebros-video.tar"

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
    echo "[deploy] bundle ($BUNDLE_SOURCE): copying to $dest ($inv_name)"
    "${scp_base[@]}" "$tmp/dronebros-stats.tar" "$tmp/dronebros-hmi.tar" "$tmp/dronebros-video.tar" "${dest}:"
    echo "[deploy] bundle: loading on $dest"
    _bundle_ssh_load_images "$dest" "${ssh_base[@]}"
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
    --local)
      BUNDLE_SOURCE=local
      BUNDLE_IMAGES=1
      shift
      ;;
    --build)
      LOCAL_BUILD=1
      shift
      ;;
    --no-build)
      echo "[deploy] warning: --no-build is deprecated; --local already ships existing images (use --build to rebuild)." >&2
      LOCAL_BUILD=0
      shift
      ;;
    --registry)
      BUNDLE_SOURCE=registry
      shift
      ;;
    --pull-on-pi)
      if [[ "$BUNDLE_SOURCE" == local ]]; then
        echo "[deploy] --pull-on-pi cannot be used with --local" >&2
        exit 2
      fi
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

if [[ "$BUNDLE_SOURCE" == local && "$BUNDLE_IMAGES" -eq 0 ]]; then
  echo "[deploy] warning: --local ignored because --skip-bundle was set (compose/config only)." >&2
fi
if [[ "$LOCAL_BUILD" -eq 1 && "$BUNDLE_SOURCE" != local ]]; then
  echo "[deploy] note: --build only applies with --local; ignored." >&2
fi

# Ansible dry-run: never touch Docker locally or over SSH for the bundle.
if [[ ${#CHECK[@]} -gt 0 ]]; then
  BUNDLE_IMAGES=0
fi

if [[ ${#CHECK[@]} -eq 0 && "$BUNDLE_IMAGES" -eq 1 && "$PULL_ON_PI" -eq 0 ]]; then
  SKIP_PULL_EXTRA=(-e skip_image_pull=true)
  echo "[deploy] image source: $BUNDLE_SOURCE"
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

# Ask for sudo password only when a task uses become (MIPI boot config, /opt deploy).
# Do not pass -b globally — it made every task try sudo and hang on "Install docker compose file".
if [[ "$EXPLICIT_NO_ASK_BECOME" -eq 1 ]]; then
  BECOME_FLAGS=()
elif [[ "$EXPLICIT_ASK_BECOME" -eq 1 ]] || _deploy_inventory_suggests_sudo "$INV" || _deploy_stack_mipi_camera "$CFG"; then
  BECOME_FLAGS=(--ask-become-pass)
  echo "" >&2
  echo "[deploy] Sudo password may be required for MIPI camera boot config (and /opt deploy path)." >&2
  echo "[deploy] If prompted: BECOME password: (Pi user sudo — nothing echoes; that is normal.)" >&2
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
