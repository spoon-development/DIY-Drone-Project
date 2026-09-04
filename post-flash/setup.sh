#!/bin/bash
# Post-flash setup for Raspberry Pi 5 — Raspberry Pi OS Lite 64-bit (Bookworm)
# Run once after flashing: sudo bash setup.sh
# Installs everything needed to build, deploy, and visualize the drone stack.

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

log()  { echo -e "${CYAN}[SETUP]${NC} $*"; }
ok()   { echo -e "${GREEN}[OK]${NC} $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }
die()  { echo -e "${RED}[ERROR]${NC} $*"; exit 1; }

[[ $EUID -ne 0 ]] && die "Run as root: sudo bash setup.sh"

SETUP_USER="${SUDO_USER:-$(logname 2>/dev/null || echo pi)}"
log "Setting up for user: $SETUP_USER"

# ─── 1. System update ────────────────────────────────────────────────────────
log "Updating package lists..."
apt-get update -y
apt-get upgrade -y

# ─── 2. Core system tools ────────────────────────────────────────────────────
log "Installing core tools..."
apt-get install -y \
    ca-certificates \
    curl \
    wget \
    gnupg \
    lsb-release \
    apt-transport-https \
    net-tools \
    util-linux \
    git \
    unzip \
    htop \
    nano \
    openssh-server \
    python3 \
    python3-pip \
    python3-venv

# ─── 3. C++ build tools ──────────────────────────────────────────────────────
log "Installing C++ build tools..."
apt-get install -y \
    cmake \
    g++ \
    make \
    ninja-build \
    meson \
    pkg-config \
    build-essential \
    libturbojpeg0-dev

# ─── 4. rpicam-apps (MIPI camera — Pi OS ships these natively) ───────────────
log "Installing rpicam-apps and libcamera..."
apt-get install -y \
    rpicam-apps \
    libcamera-dev \
    libcamera-tools \
    v4l-utils

# ─── 5. Docker Engine ────────────────────────────────────────────────────────
log "Installing Docker Engine..."

if command -v docker &>/dev/null; then
    warn "Docker already installed: $(docker --version) — skipping."
else
    install -m 0755 -d /etc/apt/keyrings
    curl -fsSL https://download.docker.com/linux/debian/gpg \
        | gpg --dearmor -o /etc/apt/keyrings/docker.gpg
    chmod a+r /etc/apt/keyrings/docker.gpg

    echo \
        "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] \
https://download.docker.com/linux/debian \
$(. /etc/os-release && echo "$VERSION_CODENAME") stable" \
        | tee /etc/apt/sources.list.d/docker.list > /dev/null

    apt-get update -y
    apt-get install -y \
        docker-ce \
        docker-ce-cli \
        containerd.io \
        docker-buildx-plugin \
        docker-compose-plugin

    ok "Docker installed: $(docker --version)"
fi

systemctl enable docker
systemctl start docker

usermod -aG docker "$SETUP_USER"
ok "Added $SETUP_USER to docker group (re-login required to take effect)."

# ─── 6. Serial / UART (flight controller) ────────────────────────────────────
log "Adding $SETUP_USER to dialout group for serial port access..."
usermod -aG dialout "$SETUP_USER"

# ─── 7. Ansible (for deploy.sh) ──────────────────────────────────────────────
log "Installing Ansible..."
apt-get install -y ansible || {
    warn "ansible not in apt — installing via pip..."
    pip3 install --break-system-packages ansible-core
}

# ─── 8. Boot config (UART + camera overlay) ──────────────────────────────────
BOOT_CONFIG="/boot/firmware/config.txt"
CMDLINE_TXT="/boot/firmware/cmdline.txt"

if [[ -f "$BOOT_CONFIG" ]]; then
    log "Configuring UART in $BOOT_CONFIG..."

    if ! grep -q "^enable_uart=1" "$BOOT_CONFIG"; then
        echo "enable_uart=1" >> "$BOOT_CONFIG"
        ok "enable_uart=1 added."
    else
        warn "enable_uart=1 already present."
    fi

    if ! grep -q "^dtoverlay=disable-bt" "$BOOT_CONFIG"; then
        echo "dtoverlay=disable-bt" >> "$BOOT_CONFIG"
        ok "dtoverlay=disable-bt added."
    else
        warn "dtoverlay=disable-bt already present."
    fi

    # Ensure camera_auto_detect is off so manual overlay takes effect
    if grep -q "^camera_auto_detect=1" "$BOOT_CONFIG"; then
        sed -i 's/^camera_auto_detect=1/camera_auto_detect=0/' "$BOOT_CONFIG"
        ok "camera_auto_detect set to 0."
    elif ! grep -q "^camera_auto_detect" "$BOOT_CONFIG"; then
        echo "camera_auto_detect=0" >> "$BOOT_CONFIG"
        ok "camera_auto_detect=0 added."
    else
        warn "camera_auto_detect already set."
    fi
else
    warn "$BOOT_CONFIG not found — skipping boot config. Apply manually."
fi

if [[ -f "$CMDLINE_TXT" ]]; then
    if grep -q "console=serial0" "$CMDLINE_TXT"; then
        sed -i 's/console=serial0,[0-9]* //g' "$CMDLINE_TXT"
        ok "Removed serial0 console from $CMDLINE_TXT."
    else
        warn "console=serial0 not found in $CMDLINE_TXT — no change needed."
    fi
fi

# ─── 9. DroneNet hotspot (Alfa USB adapter on wlan1) ────────────────────────
log "Installing DroneNet hotspot dispatcher script..."
cat > /etc/NetworkManager/dispatcher.d/99-dronenet-masquerade <<'DISPEOF'
#!/bin/sh
[ "$1" = "wlan1" ] && [ "$2" = "up" ] || exit 0
iptables -t nat -C POSTROUTING -s 10.0.69.0/24 ! -d 10.0.69.0/24 -j MASQUERADE 2>/dev/null || \
    iptables -t nat -A POSTROUTING -s 10.0.69.0/24 ! -d 10.0.69.0/24 -j MASQUERADE
DISPEOF
chmod +x /etc/NetworkManager/dispatcher.d/99-dronenet-masquerade
ok "DroneNet masquerade dispatcher installed."

# ─── 10. Optional: MIPI camera overlay ───────────────────────────────────────
echo ""
echo -e "${YELLOW}Optional: MIPI camera overlay (Arducam UC-873 / Pi Camera)${NC}"
echo "  1) imx519  — Arducam UC-873"
echo "  2) imx219  — Raspberry Pi Camera Module 2"
echo "  3) Skip"
read -rp "Select overlay [1/2/3]: " CAM_CHOICE

if [[ -f "$BOOT_CONFIG" ]]; then
    case "$CAM_CHOICE" in
        1)
            if ! grep -q "^dtoverlay=imx519" "$BOOT_CONFIG"; then
                echo "dtoverlay=imx519" >> "$BOOT_CONFIG"
                ok "imx519 overlay added."
            else
                warn "imx519 overlay already present."
            fi
            ;;
        2)
            if ! grep -q "^dtoverlay=imx219" "$BOOT_CONFIG"; then
                echo "dtoverlay=imx219" >> "$BOOT_CONFIG"
                ok "imx219 overlay added."
            else
                warn "imx219 overlay already present."
            fi
            ;;
        *)
            log "Skipping camera overlay."
            ;;
    esac
else
    warn "Boot config not found — apply camera overlay manually if needed."
fi

# ─── 11. Summary ─────────────────────────────────────────────────────────────
echo ""
echo -e "${GREEN}════════════════════════════════════════════${NC}"
echo -e "${GREEN}  Setup complete!${NC}"
echo -e "${GREEN}════════════════════════════════════════════${NC}"
echo ""
echo "Next steps:"
echo "  1. REBOOT the Pi to apply boot config changes:"
echo "       sudo reboot"
echo ""
echo "  2. After reboot, verify camera is detected:"
echo "       rpicam-hello --list-cameras"
echo ""
echo "  3. Verify Docker works:"
echo "       docker run --rm hello-world"
echo ""
echo "  4. Copy SSH key from dev machine then deploy:"
echo "       ssh-copy-id drone@<pi-ip>"
echo "       bash deploy/deploy.sh   (from dev machine)"
echo ""
echo "  5. Services after deploy:"
echo "       HMI:   http://<pi-ip>:8080"
echo "       Video: http://<pi-ip>:8765"
echo "       Stats: http://<pi-ip>:9101"
echo ""
echo -e "${YELLOW}Note: Re-login or reboot for docker/dialout group membership to take effect.${NC}"
