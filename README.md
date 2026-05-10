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
- **On your laptop:** Docker for **`deploy/build.sh`** (frontend: local images, optional **`--push`** to GHCR). Ansible + SSH for **`deploy/deploy.sh`** (backend: push stack to the Pi). See **`deploy/setup-venv.sh`** for a local Ansible venv.

## How the workflow splits

| Step | You use | What it does |
|------|---------|--------------|
| **Frontend** | **`bash ./deploy/build.sh`** | Builds **`stats`** + **`hmi`** from [`services/`](services/) via compose. Add **`--push`** (after **`docker login ghcr.io`**) to push those images to the registry/tag in [`deploy/config/stack.yml`](deploy/config/stack.yml). |
| **Middle** | *Your choice* | Commit and push to GitHub when you want. That can drive **CI** to build multi-arch images to GHCR; you can skip this if you only care about local **`--push`**. |
| **Backend** | **`bash ./deploy/deploy.sh`** | Ansible copies compose, writes **`.env`**, then on the Pi: tear down, **pull** (default), **up**. Alternatives: **`--ship <host> <user>`** pulls on *your* machine, tarballs, **scp**, **`docker load`** on the Pi, then deploys with **no registry pull** on the Pi; **`--skip-pull`** if images are already loaded. |

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
| [`deploy/build.sh`](deploy/build.sh) | **Frontend** — local **`docker compose build`**; optional **`--push`** to GHCR |
| [`deploy/setup-venv.sh`](deploy/setup-venv.sh) | Create **`deploy/.venv`** and install **ansible-core** (recommended on Ubuntu/WSL) |
| [`deploy/deploy.sh`](deploy/deploy.sh) | **Backend** — Ansible: compose + **`.env`**, then Pi **pull** + **up**, or **`--ship`**, or **`--skip-pull`** |
| [`deploy/ansible/`](deploy/ansible/) | `ansible.cfg` + [`playbooks/deploy.yml`](deploy/ansible/playbooks/deploy.yml) (used only by `deploy.sh`; **`build.sh` does not use Ansible**) |

### Path A — CI builds images (no `docker push` on your laptop)

1. **Commit and push** your code to GitHub (branch **`main`** or **`feature/pi-docker-stack`**, or run the workflow manually — see below).
2. Wait for **[Build and push images (GHCR)](.github/workflows/build-push-images.yml)** to finish green. It builds **linux/amd64** and **linux/arm64** and pushes:
   - `ghcr.io/<repo-owner-lowercase>/dronebros-stats:latest`
   - `ghcr.io/<repo-owner-lowercase>/dronebros-hmi:latest`  
   (`<repo-owner-lowercase>` is the GitHub user or org that **owns the repository**, same as in the repo URL.)
3. Set **`stack_image_registry`** in [`deploy/config/stack.yml`](deploy/config/stack.yml) to **`ghcr.io/<that same owner>`** (must match CI).
4. From your PC: **`bash ./deploy/deploy.sh`** — the Pi **pulls** those images and recreates the stack.

**Other branches:** add the branch under `on.push.branches` in the workflow file, or in GitHub go to **Actions → Build and push images (GHCR) → Run workflow**.

**Pull on the Pi without login:** in the package’s **Settings** on GitHub, set visibility to **Public** (or keep private and run **`docker login ghcr.io`** once on the Pi with a PAT that has **`read:packages`**).

### Path B — You build and push images from your dev machine (GHCR)

Use this when you want registry images **without waiting for CI**.

1. **Once:** **`docker login ghcr.io`** — GitHub username + a **PAT** with **`write:packages`** (and **`read:packages`**). Image names must match **`stack_image_registry`** / **`stack_image_tag`** in **`deploy/config/stack.yml`**.
2. **`bash ./deploy/build.sh --push`** — builds, then pushes **`dronebros-stats`** and **`dronebros-hmi`**.
3. Deploy the Pi with **`bash ./deploy/deploy.sh`** (Pi pulls), or use Path C.

### Path C — Bundle from your laptop: pull, tarball, load on Pi, then deploy

On a machine with Docker + SSH to the Pi (e.g. a new laptop), from the repo root:

1. **`docker login ghcr.io`** if packages are **private** (**`read:packages`** on the PAT).
2. **`bash ./deploy/deploy.sh -i ~/.ssh/id_ed25519 --ship 192.168.0.52 pi`** (host, user, and optional **`-i`** as you need). This **pulls** on your machine, **saves** tars, **scp** to the Pi, **`docker load`**, then Ansible runs with **skip pull** on the Pi.

### Optional: local compose run (dev only)

```bash
bash ./deploy/build.sh
docker compose -f docker-compose.yml -f docker-compose.build.yml up -d
```

This does **not** update the Pi by itself; use **`deploy/deploy.sh`** (and optionally **`build.sh --push`** + git) when you want the Pi or GHCR updated.

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
# Tarball ship + deploy:  bash ./deploy/deploy.sh --ship <pi-ip> <ssh-user>
#                         bash ./deploy/deploy.sh -i ~/.ssh/key --ship 192.168.0.52 pi
```

Each run **replaces** the prior stack on the Pi: **`docker compose down`** (with volumes), removes stray containers, **drops cached images** for your registry+tag (so **`:latest`** is not stale locally), **`docker compose pull`**, then **`docker compose up -d --force-recreate`**. Run this **after** CI has pushed new images (or the pull will re-fetch the same digest).

**Offline / air-gapped:** load images on the Pi manually, set **`skip_image_pull: true`** in [`deploy/config/stack.yml`](deploy/config/stack.yml), or pass **`bash ./deploy/deploy.sh --skip-pull`** (skips pull and image delete steps that need a registry).

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

## CI — build & push (this is how images get to GHCR)

Workflow: [`.github/workflows/build-push-images.yml`](.github/workflows/build-push-images.yml)

- Runs on push to **`main`** or **`feature/pi-docker-stack`**, or **manually** (Actions tab).
- Uses **`GITHUB_TOKEN`** to push to GHCR — **no personal `docker login` on your PC**.
- Tags: **`ghcr.io/<repo-owner-lowercase>/dronebros-stats:latest`** and **`dronebros-hmi:latest`** (same owner as the GitHub repo).

Match **`stack_image_registry`** in `deploy/config/stack.yml` to **`ghcr.io/<repo-owner-lowercase>`**.

**Private packages on the Pi:** one-time **`docker login ghcr.io`** with a PAT with **`read:packages`**, or make the package **Public** to skip login.

## On the Pi (manual)

In the directory with `docker-compose.yml` and `.env` (Ansible defaults to **`~/dronebros`**):

```bash
docker compose pull
docker compose up -d
docker compose ps
```

Browser: `http://<PI_IP>:8080` (not `0.0.0.0`).

### Offline (no registry on the Pi)

Load images with **`docker load`**, set **`skip_image_pull: true`** in `deploy/config/stack.yml` (or use **`bash ./deploy/deploy.sh --skip-pull`**), then deploy.

## Local development (compose build)

```bash
bash ./deploy/build.sh
docker compose -f docker-compose.yml -f docker-compose.build.yml up -d
```

Then open **http://localhost:8080** (or your `HMI_HOST_PORT`). For the Pi, use **`deploy/deploy.sh`**; push images with **CI**, **`build.sh --push`**, or **`deploy.sh --ship`** as you prefer.

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
| **`unknown shorthand flag: 'f'`** when running `deploy/build.sh` | WSL/Ubuntu often has Docker **without** the Compose v2 plugin. Install: **`sudo apt update && sudo apt install -y docker-compose-v2`**, then **`docker compose version`** should work. `deploy/build.sh` also tries the legacy **`docker-compose`** binary if present. |
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
