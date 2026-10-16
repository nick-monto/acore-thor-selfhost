# acore-thor-selfhost

Hosting a stock **AzerothCore WoTLK (3.3.5a) + `mod-playerbots`** realm on the **AYN Thor** (Snapdragon 8 Gen 2, Adreno 740, 16 GB LPDDR5X, Android 13) so it can be played on the device itself — no PC, no Docker.

Scope: upstream AzerothCore as used by the [Dad's MMO Lab WotLK guide](https://github.com/DadsMmoLab/dads-mmo-lab/blob/main/guides/wow-wotlk/WoW-WotLK-HOWTO.md) (`mod-playerbots/azerothcore-wotlk` + `mod-playerbots/mod-playerbots`). Not in scope: the CoA/Ascension fork and its client.

## Status

Design phase. Nothing here has been built or run on a Thor yet; this repo currently holds only findings and design. The two questions that shape everything:

1. **Can the target launcher play a self-hosted 3.3.5a realm?** Yes, with a bounded change list (`docs/LAUNCHER-HOSTING-DESIGN.md` §3) — the 32-bit Windows client path exists in the shipped runtime.
2. **Can the target launcher also host the server?** Not by adapting the guide's Docker flow: this app has no foreground service, one X environment, one shared rootfs, and UID-wide process sweeps. It is a new subsystem (§4), and the pragmatic path is to run the server outside the app (§5).

## Documents

| Doc | What it answers |
|---|---|
| [`docs/LAUNCHER-HOSTING-DESIGN.md`](docs/LAUNCHER-HOSTING-DESIGN.md) | Can `wyattabuntjer/AYN-Thor-WoW-Launcher` support the self-hosted stack, and what exactly must change |
| [`docs/SERVER-STACK.md`](docs/SERVER-STACK.md) | The server half: build, data, database, bots, supervision, and what of the guide transfers |
| [`docs/RUNBOOK.md`](docs/RUNBOOK.md) | The ordered, copy-pasteable device steps (Termux + proot Ubuntu), with the gates to check as you go |
| [`scripts/thor-install.sh`](scripts/thor-install.sh) | Does the whole device install for you: Termux packages, rootfs, server files, MySQL, configs, first boot, GM account, start/stop scripts |
| [`pins.toml`](pins.toml) | Exact commits, artifact hashes and ports every document refers to |
| [`.github/workflows/server-aarch64.yml`](.github/workflows/server-aarch64.yml) | Builds the linux/arm64 server + client data + SQL sources on a native arm64 runner and attaches them to a release |

## Install on the Thor

In Termux:

```sh
curl -fLO https://raw.githubusercontent.com/nick-monto/acore-thor-selfhost/main/scripts/thor-install.sh
bash thor-install.sh --check      # report what it would do, change nothing
bash thor-install.sh              # do it (server download ~1.2 GB, or --from-source to compile)
```

Then `~/start-wow-server.sh`. `docs/RUNBOOK.md` explains each step and every flag.

## Manage it afterwards

| Command | What it does |
|---|---|
| `bash thor-install.sh --status` | Checks every component (binaries + libraries, configs and their keys, client data, SQL sources, database, schemas, tables, account, bot count) and exits non-zero if something needs repair |
| `bash thor-install.sh --repair` | Runs the same checks, fixes what it can (configs, bot count, missing libraries, database not started, empty databases, missing account), then re-checks and reports what is left |
| `bash thor-install.sh --uninstall` | Stops everything, removes the install root and every file listed in the manifest |
| `bash thor-install.sh --uninstall --keep-db` | The same, but dumps the `acore_*` databases to `~/.thor-wow/db-dump-<timestamp>.sql` first |
| `bash thor-install.sh --uninstall --purge-rootfs` | Also removes the Ubuntu rootfs, i.e. deletes the apt packages too |

### How removal stays exact

- One rootfs directory (`$PREFIX/var/lib/proot-distro/installed-rootfs/ubuntu`) holds the Unix side; one install root (`/opt/acore`, `--root` to move it) holds the server, the configs, the client data, the SQL sources, the logs **and the MySQL data directory** (`<root>/db`) with its socket (`<root>/run`). Nothing is left in `/var/lib/mysql`.
- Everything created *outside* those two — the two Termux wrappers, the script's copy in the rootfs, `/usr/sbin/policy-rc.d`, the `_CONF_DIR` symlink when `--root` is used — is appended to a manifest (`~/.thor-wow/manifest.txt` on the Termux side, `<root>/.thor-state/manifest.txt` inside), and removal walks that manifest, refusing any path that is not clearly owned by the install.
- Flags and secrets are recorded in `~/.thor-wow/state.env` / `<root>/.thor-state/state.env`, so `--status`, `--repair` and `--uninstall` work without re-passing `--bots`, `--account` or `--db-pass`.

Confidence labels: **[V]** verified against source or an artifact inspected here, **[R]** reported, **[I]** inference.

## Hard requirements

- A **3.3.5a (build 12340) Windows x86 client** — the game files are supplied by the user; nothing here redistributes them.
- ~10 GB for the client, ~5 GB for the server (binaries + `maps`/`vmaps`/`mmaps` + world/playerbots database).
- 16 GB RAM shared between server and client: bot count has to be measured, not copied from desktop guides.

## Licence

GPL-3.0. No Blizzard client data, no AzerothCore database dumps and no server binaries are committed: the repo holds recipes, pins, hashes and (later) build artifacts published as releases.
