# Drone Bros Inc — Pi stats & flight deck HMI

A minimal Docker stack for **Raspberry Pi** (and similar ARM boards): a **stats** service reads host telemetry from the kernel, and an **HMI** serves a single-page dashboard on **one TCP port**. The browser never talks to the stats container directly; the HMI app proxies **`/api/stats`** on the same origin.

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
│  dronebros-hmi (Python aiohttp)                          │
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
| `dronebros-hmi` | `ghcr.io/<owner>/dronebros-hmi` | **8080** (configurable) | Web UI + reverse proxy; **`hmi-data`** on the host stores UI settings |

## Requirements

- **On the Pi:** Docker Engine + Compose plugin v2, user in `docker` group (or use `sudo` with Docker).
- **64-bit OS** on the Pi (`aarch64`) is typical for Pi 5 / modern Ubuntu on Pi.
- **On your laptop:** Docker for **`build/build.sh`** (local images) and **`build/push-images.sh`** (GHCR, after **`docker login ghcr.io`**). **Ansible** (`ansible-playbook` / **`ansible-inventory`** on your PATH, e.g. **`sudo apt install ansible-core`**) + SSH for **`deploy/deploy.sh`** (ship stack to the Pi).

## Build, push, deploy (local only)

| Step | You use | What it does |
|------|---------|--------------|
| **Build** | **`bash ./build/build.sh`** (optional **`--arm`** for Pi / **`--amd`** for PC) | **`docker compose build`** for **`stats`** + **`hmi`**. |
| **Push** | **`bash ./build/push-images.sh`** (optional **`--image multi`** / **`amd64`** / **`arm64`**) | **buildx** push to GHCR using **`deploy/config/stack.yml`** registry/tag (needs **buildx** + login). |
| **Git** | *Your choice* | Commit and push source (backup / collaboration only — images reach GHCR via **`push-images.sh`**). |
| **Deploy** | **`bash ./deploy/deploy.sh`** | Pull images **on your machine**, **save** tars, **scp** + **`docker load`** on the Pi, then Ansible: compose + **`.env`**, tear down, **up** (Pi does **not** pull GHCR by default). **`--pull-on-pi`** restores registry pull on the Pi; **`--skip-bundle`** if images are already loaded. |

## Standard process when you change code

The Pi runs **images from GHCR**, not your laptop’s source tree. If you changed anything that ships **inside** an image (**`services/stats`**, **`services/hmi`**, Dockerfiles) or you need the Pi on a **new image tag**, always do **push then deploy** in that order.

From the **repo root** on your dev machine (WSL/Linux or macOS; use **`bash`** so scripts don’t hit CRLF issues):

1. **`docker login ghcr.io`** — when needed (PAT with **`write:packages`** for push; Pi needs **`read:packages`** if packages are private).
2. **`bash ./build/build.sh`** — optional but catches build errors early. Use **`--arm`** or **`--amd`** if you want a single-arch local build.
3. **`bash ./build/push-images.sh`** — publishes **`dronebros-stats`** and **`dronebros-hmi`** to **`stack_image_registry` / `stack_image_tag`** in [`deploy/config/stack.yml`](deploy/config/stack.yml). Default is multi-arch; use **`--image arm64`** if you only push for the Pi.
4. **`bash ./deploy/deploy.sh`** — Pulls images on your laptop, copies them to the Pi, **`docker load`** there, then Ansible refreshes **`docker-compose.yml`** and **`.env`** and **recreates** the stack (no registry access required on the Pi).

Until step **4** finishes, the vehicle can still be running **old** images — the UI will not match your latest edits.

**Compose-only edits** (root **`docker-compose.yml`**, no new image): step **4** alone updates the file on the Pi, but the Pi still uses whatever images that compose references until you also run **3** (and **2** if you changed Dockerfiles).

**Git**: commit and push whenever you want source backup; it does **not** replace steps **3–4** for updating the Pi from your laptop.

**Deploy directory:** Ansible defaults to **`~/dronebros`** (**`/home/<user>/dronebros`**) so **no sudo** is needed to create the folder (same behavior as before the `/opt` experiment). To use **`/opt/dronebros`** instead, set **`dronebros_deploy_dir`** in inventory — then **`deploy.sh`** adds **`-b --ask-become-pass`** and you must enter the **sudo** password when prompted (use a real terminal). **`--ask-pass`** / **`-k`** is for **SSH** password login. **`--no-ask-become-pass`** skips the sudo prompt only when NOPASSWD is configured for that path.

## Why two `docker-compose` files?

- **`docker-compose.yml`** — What runs on the **Pi**: **pull images** from GHCR only. It must not list `build: context: ./services/...` because the Pi does not get your source tree.
- **`docker-compose.build.yml`** — **Extra** definitions for your **laptop**: `build` contexts under [`services/`](services/). **`build/build.sh`** merges both files so you can build and run locally without a registry.

Compose variable defaults (`${IMAGE_REGISTRY:-…}`) work without a `.env` file on your machine. On the Pi, **`deploy/deploy.sh`** writes `.env` from **`deploy/config/stack.yml`** so the device matches the registry/tag you chose.

## Build vs deploy layout

| Area | Path | Purpose |
|------|------|---------|
| **Build** | [`build/build.sh`](build/build.sh) | Local **`docker compose build`**; **`--arm`** (**linux/arm64**, Pi) or **`--amd`** (**linux/amd64**, PC). |
| **Push** | [`build/push-images.sh`](build/push-images.sh) | **buildx** push **`stats`** + **`hmi`** to GHCR (registry/tag from [`deploy/config/stack.yml`](deploy/config/stack.yml)). |
| **Deploy** | [`deploy/`](deploy/) | Inventory, **`stack.yml`**, Ansible, **`deploy.sh`** only (no image build here). |

| Path | Purpose |
|------|---------|
| [`deploy/inventory/hosts.yml`](deploy/inventory/hosts.yml) | **Where** to deploy: `ansible_host`, `ansible_user`, optional `ansible_ssh_private_key_file`, optional **`dronebros_deploy_dir`** (default **`~/dronebros`**) |
| [`deploy/config/stack.yml`](deploy/config/stack.yml) | **Which images**: `stack_image_registry`, `stack_image_tag`, `skip_image_pull`, `stack_bundle_platform`, optional `hmi_host_port` |
| [`deploy/deploy.sh`](deploy/deploy.sh) | Entry point: local **pull/save/scp/load**, then Ansible copies compose + **`.env`** and runs **compose up**; **`--pull-on-pi`** for Pi-side pull; **`--skip-bundle`** if images already on the Pi |
| [`deploy/ansible/`](deploy/ansible/) | `ansible.cfg` + [`playbooks/deploy.yml`](deploy/ansible/playbooks/deploy.yml) |

### Publish images to GHCR (your machine)

1. **Once:** **`docker login ghcr.io`** — GitHub username + a **PAT** with **`write:packages`** (and **`read:packages`**). Image names must match **`stack_image_registry`** / **`stack_image_tag`** in **`deploy/config/stack.yml`** (typically **`ghcr.io/<repo-owner-lowercase>/...`**).
2. **`bash ./build/build.sh`** then **`bash ./build/push-images.sh`** — **buildx**-pushes both images (default **multi-arch** so the Pi gets **arm64**). Use **`bash ./build/push-images.sh --image arm64`** for a single-arch push if you want.
3. Deploy with **`bash ./deploy/deploy.sh`** (default: pull on your laptop, tarball, **`docker load`** on the Pi — no registry on the Pi).

**Private packages:** the Pi (or your laptop when pulling) needs **`docker login ghcr.io`** with **`read:packages`**, or make the GHCR package **Public**.

### Bundle-only deploy (no `docker push` from you)

On a machine with Docker + SSH to the Pi (e.g. a new laptop), from the repo root:

1. **`docker login ghcr.io`** if packages are **private** (**`read:packages`** on the PAT).
2. **`bash ./deploy/deploy.sh -i ~/.ssh/id_ed25519`** — same as the default flow (inventory defines the Pi). Optional **`--ship <ip> <user>`** bundles only to that SSH target if you do not want to use inventory for the copy step.

### Optional: local compose run (dev only)

```bash
bash ./build/build.sh
docker compose -f docker-compose.yml -f docker-compose.build.yml up -d
```

This does **not** update the Pi by itself; use **`build/push-images.sh`** and **`deploy/deploy.sh`** when you want GHCR and the vehicle updated.

### Deploy to hosts (Ansible)

**One-time — Ansible on your laptop** (Ubuntu / WSL):

```bash
sudo apt update && sudo apt install -y ansible-core
# or: pipx install ansible-core
```

**Every deploy** (from repo root):

```bash
bash ./deploy/deploy.sh
# One host:   bash ./deploy/deploy.sh --limit pi
# Dry run:    bash ./deploy/deploy.sh --check
# Pi pulls GHCR itself (needs internet on Pi):  bash ./deploy/deploy.sh --pull-on-pi
# Images already on Pi:  bash ./deploy/deploy.sh --skip-bundle
# Bundle only to explicit SSH:  bash ./deploy/deploy.sh -i ~/.ssh/key --ship 192.168.0.52 pi
```

Each run **replaces** the prior stack on the Pi: **`docker compose down`**, removes stray containers, then **`docker compose up -d --force-recreate`**. By default **`deploy.sh`** bundles images from your machine (no **`docker compose pull`** on the Pi). Use **`--pull-on-pi`** only when the Pi can reach GHCR.

**Offline / air-gapped Pi:** default **`deploy.sh`** already avoids registry pulls on the Pi. If you pre-loaded images another way, use **`bash ./deploy/deploy.sh --skip-bundle`**.

Optional: create **`deploy/.venv`** yourself and `pip install ansible-core`; **`deploy.sh`** uses **`deploy/.venv/bin/ansible-playbook`** when that path exists.

Ansible copies [`docker-compose.yml`](docker-compose.yml) and writes **`.env`** on the target from [`deploy/config/stack.yml`](deploy/config/stack.yml):

| Variable (on Pi `.env`) | Source |
|-------------------------|--------|
| `IMAGE_REGISTRY` | `stack_image_registry` (default `ghcr.io/spoon-development`) |
| `IMAGE_TAG` | `stack_image_tag` (default `latest`) |
| `HMI_HOST_PORT` | `hmi_host_port` or **8080** |

Optional: `ssh-copy-id user@<pi-ip>` once so Ansible does not prompt every time.

## Windows and WSL: git

Use **one WSL shell** for **Git over SSH** when your keys live in `~/.ssh` there. PowerShell on `C:\` can use a different agent layout. If `ssh -T git@github.com` works in WSL, use that same shell for `git push`.

## On the Pi (manual)

In the directory with `docker-compose.yml` and `.env` (Ansible defaults to **`~/dronebros`**; override with **`dronebros_deploy_dir`** in inventory):

```bash
docker compose pull
docker compose up -d
docker compose ps
```

Browser: `http://<PI_IP>:8080` (not `0.0.0.0`).

### Offline (no registry on the Pi)

Load images with **`docker load`**, then **`bash ./deploy/deploy.sh --skip-bundle`** (no local pull and no Pi pull).

## Local development (compose build)

```bash
bash ./build/build.sh
docker compose -f docker-compose.yml -f docker-compose.build.yml up -d
```

Then open **http://localhost:8080** (or your `HMI_HOST_PORT`). For the Pi, use **`deploy/deploy.sh`** (bundles from your machine by default); publish updated images with **`build/push-images.sh`** before deploy when you changed **`services/`** or Dockerfiles.

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
| **no matching manifest for linux/amd64** | Use a multi-arch push from **`push-images.sh`**, or `docker pull --platform linux/arm64`. |
| **`bash\r`**, **`set: pipefail`**, or **`invalid option`** under WSL | Scripts have **CRLF** line endings. [`.gitattributes`](.gitattributes) forces **LF** for `*.sh`; re-save or run `sed -i 's/\r$//' deploy/*.sh` in WSL, then **`git add --renormalize deploy/*.sh`**. Always invoke with **`bash ./deploy/....sh`**. |
| **permission denied** on Docker socket | `sudo usermod -aG docker $USER` then re-login; or use Docker Desktop WSL integration. |
| HMI loads but stats show offline | `docker compose logs stats hmi`; ensure stats container has `/proc`, `/sys`, `/` mounts. |
| **`unknown shorthand flag: 'f'`** when running **`build/build.sh`** | WSL/Ubuntu often has Docker **without** the Compose v2 plugin. Install: **`sudo apt update && sudo apt install -y docker-compose-v2`**, then **`docker compose version`** should work. The script also tries the legacy **`docker-compose`** binary if present. |
| Firewall on Pi | `sudo ufw allow 8080/tcp` if UFW is enabled. |

## Repository layout

```
├── README.md
├── docker-compose.yml
├── docker-compose.build.yml
├── build/
│   ├── build.sh
│   └── push-images.sh
├── deploy/
│   ├── deploy.sh             # entry: bundle images + ansible to Pi
│   ├── config/stack.yml
│   ├── inventory/hosts.yml
│   └── ansible/              # ansible.cfg + playbooks/deploy.yml
├── services/
│   ├── stats/
│   └── hmi/
```

## License / project

Part of the **DIY-Drone-Project** effort — stats + HMI slice only; extend with more services and compose profiles as needed.
