# Drone Bros Inc — Pi stats & flight deck HMI

A minimal Docker stack for **Raspberry Pi** (and similar ARM boards): a **stats** service reads host telemetry from the kernel, and an **HMI** serves a single-page **“Drone Bros Inc”** dashboard on **one TCP port**. The browser never talks to the stats container directly; nginx proxies **`/api/stats`** on the same origin.

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
| `dronebros-stats` | `ghcr.io/<owner>/dronebros-stats` | *none* | JSON metrics: temp, load, memory, uptime, disk |
| `dronebros-hmi` | `ghcr.io/<owner>/dronebros-hmi` | **8080** (configurable) | Web UI + reverse proxy |

## Requirements

- **On the Pi:** Docker Engine + Compose plugin v2, user in `docker` group (or use `sudo` with Docker).
- **64-bit OS** on the Pi (`aarch64`) is typical for Pi 5 / modern Ubuntu on Pi.
- **On your laptop (optional):** Docker if you pull from GHCR or build `export-images.sh` bundles; WSL2 works well on Windows.

## Configuration

Copy [`.env.example`](.env.example) to **`.env`** next to `docker-compose.yml`:

| Variable | Meaning |
|----------|---------|
| `IMAGE_REGISTRY` | e.g. `ghcr.io/spoon-development` |
| `IMAGE_TAG` | e.g. `latest` |
| `HMI_HOST_PORT` | Host port mapped to the HMI (default **8080**) |

## CI — pre-built images (GHCR)

Workflow: [`.github/workflows/build-push-images.yml`](.github/workflows/build-push-images.yml)

- Builds **multi-arch** images: **`linux/amd64`** and **`linux/arm64`** (PC/WSL and Raspberry Pi).
- Pushes to **GitHub Container Registry** under your repo owner (lowercase):

  - `ghcr.io/<owner>/dronebros-stats:latest`
  - `ghcr.io/<owner>/dronebros-hmi:latest`

After a green run, open your org/user **Packages** on GitHub to confirm they exist.

**Private packages:** `docker login ghcr.io` with your **GitHub username** and a **Personal Access Token** (`read:packages`; authorize **SSO** for the org if required). This is separate from **SSH keys** used for `git`.

**Legacy / ARM-only tags:** If you ever pull an old image that is **arm64-only** from an **amd64** machine, use:

```bash
docker pull --platform linux/arm64 ghcr.io/<owner>/dronebros-stats:latest
```

## Deploy on the Pi (with internet — pull from GHCR)

On the Pi, in the directory that contains `docker-compose.yml` and `.env`:

```bash
docker compose pull
docker compose up -d
docker compose ps
```

Open the UI from another machine on the same LAN:

```text
http://<PI_IP>:8080
```

Use the Pi’s real address (e.g. `192.168.0.52`). **Do not** use `http://0.0.0.0:8080` in the browser — `0.0.0.0` is not a valid client URL.

## Deploy on the Pi (offline — image bundle)

On a machine with Docker (e.g. WSL), pull the images for the Pi (ARM), then save:

```bash
cd /path/to/DIY-Drone-Project
docker pull --platform linux/arm64 ghcr.io/<owner>/dronebros-stats:latest
docker pull --platform linux/arm64 ghcr.io/<owner>/dronebros-hmi:latest
chmod +x scripts/export-images.sh
./scripts/export-images.sh ./dronebros-bundle.tar
```

Copy to the Pi: `dronebros-bundle.tar`, `docker-compose.yml`, `.env.example`.

On the Pi:

```bash
docker load -i dronebros-bundle.tar
cp .env.example .env   # if .env does not exist yet
docker compose up -d
```

Do **not** run `docker compose pull` if the Pi has no registry access.

### Example: copy from WSL, then run on Pi

```bash
# WSL — adjust user, IP, and paths
scp dronebros-bundle.tar docker-compose.yml .env.example spoon@192.168.0.52:~/

# SSH to Pi
ssh spoon@192.168.0.52
docker load -i ~/dronebros-bundle.tar
test -f ~/.env || cp ~/.env.example ~/.env
docker compose up -d
```

## Remote deploy helpers

- **Ansible:** [`deploy/ansible/playbooks/deploy_stack.yml`](deploy/ansible/playbooks/deploy_stack.yml) — copies compose + seeds `.env`, optional `skip_image_pull` after `docker load`.
- **Bash:** [`scripts/deploy-remote.sh`](scripts/deploy-remote.sh) — `scp` + `docker compose pull` or `--skip-pull`.

## Local development (build from source)

```bash
docker compose -f docker-compose.yml -f docker-compose.build.yml build
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
|---------|----------------|
| Browser **ERR_ADDRESS_INVALID** for `0.0.0.0` | Use **`http://<pi-ip>:8080`**, not `0.0.0.0`. |
| **denied** on `docker pull` | Private GHCR: `docker login ghcr.io` + PAT; or make packages public. |
| **no matching manifest for linux/amd64** | Use multi-arch images from current CI, or `docker pull --platform linux/arm64`. |
| **`bash\r` / script errors in WSL | CRLF in shell scripts: `sed -i 's/\r$//' scripts/*.sh` or rely on [`.gitattributes`](.gitattributes). |
| **permission denied** on Docker socket | `sudo usermod -aG docker $USER` then re-login; or use Docker Desktop WSL integration. |
| HMI loads but stats show offline | `docker compose logs stats hmi`; ensure stats container has `/proc`, `/sys`, `/` mounts. |
| Firewall on Pi | `sudo ufw allow 8080/tcp` if UFW is enabled. |

## Repository layout

```
├── docker-compose.yml          # Runtime stack
├── docker-compose.build.yml    # Optional local build overrides
├── .env.example
├── services/
│   ├── stats/                  # Python metrics server
│   └── hmi/                    # nginx + static “Drone Bros Inc” UI
├── scripts/
│   ├── export-images.sh        # docker save bundle for offline Pi
│   └── deploy-remote.sh        # scp + compose over SSH
├── deploy/ansible/             # Playbooks for fleet-style deploys
└── .github/workflows/          # Build & push to GHCR
```

## License / project

Part of the **DIY-Drone-Project** effort — stats + HMI slice only; extend with more services and compose profiles as needed.
