# Drone Bros Inc — Pi stats & flight deck HMI

A minimal Docker stack for **Raspberry Pi** (and similar ARM boards): a **stats** service reads host telemetry from the kernel, and an **HMI** serves a single-page dashboard on **one TCP port**. The browser never talks to the stats container directly; nginx proxies **`/api/stats`** on the same origin.

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│  Your PC / phone browser                                 │
│  http://<pi-ip>:8080                                     │
└───────────────────────────┬─────────────────────────────┘
                            │
                     host port 8080
                            │
┌───────────────────────────▼─────────────────────────────┐
│  dronebros-hmi (nginx)                                   │
│  /  → static UI   |   /api/stats → proxy to stats       │
└───────────────────────────┬─────────────────────────────┘
                   Docker network (internal)
                            │
┌───────────────────────────▼─────────────────────────────┐
│  dronebros-stats (Python)  :9101  (not published)        │
│  mounts: /proc, /sys, / → read-only host metrics         │
└─────────────────────────────────────────────────────────┘
```

| Container | Image | Host port | Purpose |
|-----------|--------|-----------|---------|
| `dronebros-stats` | `ghcr.io/<owner>/dronebros-stats` | *none* | JSON metrics: temp, load, memory, uptime, disk, compute, network |
| `dronebros-hmi` | `ghcr.io/<owner>/dronebros-hmi` | **8080** (configurable) | Web UI + reverse proxy |

## Requirements

- **On the Pi:** Docker Engine + Compose plugin v2, user in `docker` group (or use `sudo` with Docker).
- **64-bit OS** on the Pi (`aarch64`) is typical for Pi 5 / modern Ubuntu on Pi.
- **On your laptop:** Docker (for local builds), Ansible for remote deploy (see **`deploy/setup-venv.sh`** on Ubuntu/WSL), and SSH to the Pi. WSL2 works well on Windows — use Bash there for `deploy/deploy.sh`.

## Why two `docker-compose` files?

- **`docker-compose.yml`** — What runs on the **Pi** (and in CI): **pull images** from GHCR only. It must not list `build: context: ./services/...` because the Pi does not get your source tree.
- **`docker-compose.build.yml`** — **Extra** definitions for your **laptop**: `build` contexts under [`services/`](services/). `deploy/build.sh` merges both files so you can build and run locally without a registry.

Compose variable defaults (`${IMAGE_REGISTRY:-…}`) work without a `.env` file on your machine. On the Pi, **`deploy/deploy.sh`** writes `.env` from **`deploy/config/stack.yml`** so the device matches the registry/tag you chose.

## Deploy folder (inventory, config, scripts)

Everything for **building** and **shipping** the stack lives under **`deploy/`**:

| Path | Purpose |
|------|---------|
| [`deploy/inventory/hosts.yml`](deploy/inventory/hosts.yml) | **Where** to deploy: `ansible_host`, `ansible_user`, optional `ansible_ssh_private_key_file`, optional `dronebros_deploy_dir` |
| [`deploy/config/stack.yml`](deploy/config/stack.yml) | **Which images**: `stack_image_registry`, `stack_image_tag`, `skip_image_pull`, optional `hmi_host_port` |
| [`deploy/build.sh`](deploy/build.sh) | Build images locally from [`services/`](services/) |
| [`deploy/setup-venv.sh`](deploy/setup-venv.sh) | Create **`deploy/.venv`** and install **ansible-core** (recommended on Ubuntu/WSL) |
| [`deploy/deploy.sh`](deploy/deploy.sh) | Run Ansible (prefers **`deploy/.venv`** if present) |
| [`deploy/ansible/`](deploy/ansible/) | `ansible.cfg` + [`playbooks/deploy.yml`](deploy/ansible/playbooks/deploy.yml) (used only by `deploy.sh`; **`build.sh` does not use Ansible**) |

### Build images (local)

```bash
cd /path/to/DIY-Drone-Project
bash ./deploy/build.sh
# Optional:  bash ./deploy/build.sh stats hmi
```

### Deploy to hosts (Ansible)

**One-time — virtualenv under `deploy/`** (avoids system `pip` / PEP 668 errors on Ubuntu and WSL):

```bash
# If venv fails with "ensurepip is not available" (common on minimal Ubuntu/WSL):
sudo apt update && sudo apt install -y python3-venv

cd /path/to/DIY-Drone-Project
bash ./deploy/setup-venv.sh
```

**Every deploy:**

```bash
cd /path/to/DIY-Drone-Project
bash ./deploy/deploy.sh
# One host:   bash ./deploy/deploy.sh --limit pi
# Dry run:    bash ./deploy/deploy.sh --check
```

If `deploy/.venv` is missing, `deploy.sh` falls back to **`ansible-playbook` on your PATH** (e.g. `apt install ansible-core` or `pipx install ansible-core`).

Ansible copies [`docker-compose.yml`](docker-compose.yml) and writes **`.env`** on the target from [`deploy/config/stack.yml`](deploy/config/stack.yml):

| Variable (on Pi `.env`) | Source |
|-------------------------|--------|
| `IMAGE_REGISTRY` | `stack_image_registry` (default `ghcr.io/spoon-development`) |
| `IMAGE_TAG` | `stack_image_tag` (default `latest`) |
| `HMI_HOST_PORT` | `hmi_host_port` or **8080** |

Optional: `ssh-copy-id user@<pi-ip>` once so Ansible does not prompt every time.

## Windows and WSL: git

Use **one WSL shell** for **Git over SSH** when your keys live in `~/.ssh` there. PowerShell on `C:\` can use a different agent layout. If `ssh -T git@github.com` works in WSL, use that same shell for `git push`.

## CI — pre-built images (GHCR)

Workflow: [`.github/workflows/build-push-images.yml`](.github/workflows/build-push-images.yml)

- Builds **multi-arch** images: **`linux/amd64`** and **`linux/arm64`**.
- Pushes to **GitHub Container Registry** under your repo owner (lowercase):

  - `ghcr.io/<owner>/dronebros-stats:latest`
  - `ghcr.io/<owner>/dronebros-hmi:latest`

**Private packages:** on the Pi (once), run **`docker login ghcr.io`** with a PAT that has `read:packages`.

**Legacy / ARM-only tags:** `docker pull --platform linux/arm64 ghcr.io/<owner>/dronebros-stats:latest`

## On the Pi (manual)

In the directory with `docker-compose.yml` and `.env` (Ansible defaults to **`~/dronebros`**):

```bash
docker compose pull
docker compose up -d
docker compose ps
```

Browser: `http://<PI_IP>:8080` (not `0.0.0.0`).

### Offline (no registry on the Pi)

On a machine with Docker, save images and copy them plus `docker-compose.yml` to the Pi, then `docker load` each tar, set `skip_image_pull: true` in `deploy/config/stack.yml`, and run `bash ./deploy/deploy.sh` from your PC (it will write `.env`) — or on the Pi create `.env` manually and run `docker compose up -d` without pull.

## Local development (compose build)

```bash
bash ./deploy/build.sh
docker compose -f docker-compose.yml -f docker-compose.build.yml up -d
```

Then open **http://localhost:8080** (or your `HMI_HOST_PORT`).

## Operations

```bash
docker compose logs -f
docker compose restart
docker compose down
```

## Troubleshooting

| Symptom | What to check |
|---------|---------------|
| Browser **ERR_ADDRESS_INVALID** for `0.0.0.0` | Use **`http://<pi-ip>:8080`**, not `0.0.0.0`. |
| **denied** on `docker pull` | Private GHCR: `docker login ghcr.io` + PAT; or make packages public. |
| **no matching manifest for linux/amd64** | Use multi-arch images from current CI, or `docker pull --platform linux/arm64`. |
| **`bash\r`**, **`set: pipefail`**, or **`invalid option`** under WSL | Scripts have **CRLF** line endings. [`.gitattributes`](.gitattributes) forces **LF** for `*.sh`; re-save or run `sed -i 's/\r$//' deploy/*.sh` in WSL, then **`git add --renormalize deploy/*.sh`**. Always invoke with **`bash ./deploy/....sh`**. |
| **permission denied** on Docker socket | `sudo usermod -aG docker $USER` then re-login; or use Docker Desktop WSL integration. |
| HMI loads but stats show offline | `docker compose logs stats hmi`; ensure stats container has `/proc`, `/sys`, `/` mounts. |
| Firewall on Pi | `sudo ufw allow 8080/tcp` if UFW is enabled. |

## Repository layout

```
├── README.md
├── docker-compose.yml
├── docker-compose.build.yml
├── deploy/
│   ├── build.sh
│   ├── setup-venv.sh
│   ├── deploy.sh
│   ├── config/stack.yml
│   ├── inventory/hosts.yml
│   └── ansible/              # ansible.cfg + playbooks/deploy.yml
├── services/
│   ├── stats/
│   └── hmi/
└── .github/workflows/
```

## License / project

Part of the **DIY-Drone-Project** effort — stats + HMI slice only; extend with more services and compose profiles as needed.
