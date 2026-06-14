#!/bin/bash
# Pi 5 first-boot setup for DIY Drone Project
# Run once on a freshly flashed Pi after SSH access is available.
# Usage: bash setup-pi.sh
set -euo pipefail

YELLOW='\033[1;33m'; GREEN='\033[0;32m'; RED='\033[0;31m'; NC='\033[0m'
info()  { echo -e "${GREEN}[setup]${NC} $*"; }
warn()  { echo -e "${YELLOW}[setup]${NC} $*"; }
die()   { echo -e "${RED}[setup] ERROR:${NC} $*" >&2; exit 1; }

[[ "$(uname -m)" == "aarch64" ]] || die "Run this on the Pi (aarch64), not your dev machine."

CURRENT_USER="${SUDO_USER:-$USER}"
info "Setting up Pi for user: $CURRENT_USER"

# ── 1. System packages ────────────────────────────────────────────────────────
info "Updating package lists..."
sudo apt-get update -qq

info "Installing dependencies..."
sudo apt-get install -y -qq \
  ca-certificates curl gnupg lsb-release \
  python3 python3-pip \
  git \
  net-tools

# ── 2. Docker Engine ──────────────────────────────────────────────────────────
if command -v docker &>/dev/null && docker compose version &>/dev/null 2>&1; then
  info "Docker + Compose already installed: $(docker --version)"
else
  info "Installing Docker Engine..."
  sudo install -m 0755 -d /etc/apt/keyrings
  curl -fsSL https://download.docker.com/linux/debian/gpg \
    | sudo gpg --dearmor --yes -o /etc/apt/keyrings/docker.gpg
  sudo chmod a+r /etc/apt/keyrings/docker.gpg

  # Ubuntu and Pi OS both work; detect which
  DISTRO_ID="$(. /etc/os-release && echo "$ID")"
  DISTRO_CODENAME="$(. /etc/os-release && echo "$VERSION_CODENAME")"
  if [[ "$DISTRO_ID" == "ubuntu" ]]; then
    echo \
      "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] \
https://download.docker.com/linux/ubuntu ${DISTRO_CODENAME} stable" \
      | sudo tee /etc/apt/sources.list.d/docker.list > /dev/null
  else
    # Raspberry Pi OS (Debian-based)
    echo \
      "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] \
https://download.docker.com/linux/debian ${DISTRO_CODENAME} stable" \
      | sudo tee /etc/apt/sources.list.d/docker.list > /dev/null
  fi

  sudo apt-get update -qq
  sudo apt-get install -y -qq \
    docker-ce docker-ce-cli containerd.io \
    docker-buildx-plugin docker-compose-plugin

  info "Docker installed: $(docker --version)"
fi

# ── 3. User groups ────────────────────────────────────────────────────────────
if ! groups "$CURRENT_USER" | grep -q '\bdocker\b'; then
  info "Adding $CURRENT_USER to docker group..."
  sudo usermod -aG docker "$CURRENT_USER"
  warn "Docker group added — you must log out and back in (or reboot) before docker works without sudo."
else
  info "$CURRENT_USER already in docker group."
fi

if ! groups "$CURRENT_USER" | grep -q '\bdialout\b'; then
  info "Adding $CURRENT_USER to dialout group (serial/UART access)..."
  sudo usermod -aG dialout "$CURRENT_USER"
else
  info "$CURRENT_USER already in dialout group."
fi

# ── 4. Enable Docker on boot ──────────────────────────────────────────────────
sudo systemctl enable --now docker

# ── 5. UART for MAVLink (Pi GPIO14/15 = /dev/ttyAMA0) ────────────────────────
BOOT_CONFIG=""
for p in /boot/firmware/config.txt /boot/config.txt; do
  [[ -f "$p" ]] && BOOT_CONFIG="$p" && break
done

if [[ -n "$BOOT_CONFIG" ]]; then
  if ! grep -q "^enable_uart=1" "$BOOT_CONFIG"; then
    info "Enabling UART in $BOOT_CONFIG..."
    echo "enable_uart=1" | sudo tee -a "$BOOT_CONFIG" > /dev/null
  else
    info "UART already enabled in $BOOT_CONFIG."
  fi

  # Pi 5: disable the built-in Bluetooth UART conflict if present
  if ! grep -q "dtoverlay=disable-bt" "$BOOT_CONFIG"; then
    info "Disabling Bluetooth UART conflict (Pi 5)..."
    echo "dtoverlay=disable-bt" | sudo tee -a "$BOOT_CONFIG" > /dev/null
  fi
else
  warn "Could not find boot config.txt — enable_uart=1 must be set manually."
fi

# Remove serial console from cmdline so ttyAMA0 is free for MAVLink
CMDLINE=""
for p in /boot/firmware/cmdline.txt /boot/cmdline.txt; do
  [[ -f "$p" ]] && CMDLINE="$p" && break
done

if [[ -n "$CMDLINE" ]]; then
  if grep -qE "console=(serial0|ttyAMA0)" "$CMDLINE"; then
    info "Removing serial console from $CMDLINE (freeing ttyAMA0 for MAVLink)..."
    sudo sed -i -E 's/console=(serial0|ttyAMA0),?[0-9]* ?//g' "$CMDLINE"
    info "Done. Cmdline is now: $(cat "$CMDLINE")"
  else
    info "No serial console entry in $CMDLINE — UART is free."
  fi
else
  warn "Could not find cmdline.txt — verify ttyAMA0 is not a serial console."
fi

# ── 6. Deploy directory ───────────────────────────────────────────────────────
DEPLOY_DIR="/home/$CURRENT_USER/dronebros"
mkdir -p "$DEPLOY_DIR" "$DEPLOY_DIR/hmi-data"
info "Deploy directory ready: $DEPLOY_DIR"

# ── 7. Done ───────────────────────────────────────────────────────────────────
echo
echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${GREEN}  Pi setup complete.${NC}"
echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo
echo "Next steps:"
echo
echo "  1. REBOOT the Pi so UART and group changes take effect:"
echo "       sudo reboot"
echo
echo "  2. After reboot, from your dev machine run the deploy:"
echo "       bash ./deploy/deploy.sh"
echo
echo "     Or if images are already pushed to GHCR:"
echo "       bash ./deploy/deploy.sh --pull-on-pi"
echo
echo "  3. Open http://$(hostname -I | awk '{print $1}'):8080 in a browser."
echo
