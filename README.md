# acore-thor-selfhost

Hosting a stock **AzerothCore WoTLK (3.3.5a) + `mod-playerbots`** realm on the **AYN Thor** (Snapdragon 8 Gen 2, Adreno 740, 16 GB LPDDR5X, Android 13) so it can be played on the device itself — no PC, no Docker.

Scope: upstream AzerothCore as used by the [Dad's MMO Lab WotLK guide](https://github.com/DadsMmoLab/dads-mmo-lab/blob/main/guides/wow-wotlk/WoW-WotLK-HOWTO.md) (`mod-playerbots/azerothcore-wotlk` + `mod-playerbots/mod-playerbots`). Out of scope: other AzerothCore forks and their clients.

## Status

The server half is implemented. `modules/mod-creature-gear` and `modules/mod-hardcore` are wired into the static module build with the rest of the core. Hardcore defaults to `ghost`; creature gear uses `scripts/creature-gear/`, which generates its dataset: every creature the rules admit - humanoids of level 1-80 with no quest, vendor or trainer flag and no hand-set equipment, 1274 of them against the pinned base dumps - gets per-spawn weapon pools, drops the gear it is wearing, and wears an eleven-slot armour outfit over the mirror-image packet; server-side data and logic only, no client patch. The gear fits the creature's class (`creature_template.unit_class`): a mage-class caster gets staves, wands, swords, daggers and cloth, a warrior plate and a shield, a paladin no ranged weapon at all - the proficiency lists come from the client's own `SkillLine`/`SkillLineAbility` data, and the validator rejects any pick a class could not use. It has been booted end to end in a real `worldserver` built from the pinned core with the pinned client data: the module loaded its dataset (1274 outfit rows and 5096 armour variants for 1274 creatures) and the worldserver logged no errors.

What is not verified yet: how a real client renders the outfit, a drop taken from a corpse in play, the hardcore death/relog path, and any of it on the device — nothing here has run on a Thor, and the arm64 build the workflow produces has not been booted anywhere. The player-centric randombot distribution compiles into the local worldserver but has not been exercised with a live client either.

Playing and hosting are separate here. Playing a self-hosted 3.3.5a realm needs the launcher app plus the client-side changes listed in [`docs/LAUNCHER-HOSTING-DESIGN.md`](docs/LAUNCHER-HOSTING-DESIGN.md); the app's shipped runtime already carries the 32-bit Windows client path. Hosting the server *inside* the app is out of scope for this repository: the app has no foreground service, one X environment, one shared rootfs and UID-wide process sweeps, so the guide's Docker flow does not transfer. The server runs beside the app, in Termux.

## Documents

| Doc | What it answers |
|---|---|
| [`docs/LAUNCHER-HOSTING-DESIGN.md`](docs/LAUNCHER-HOSTING-DESIGN.md) | Can `wyattabuntjer/AYN-Thor-WoW-Launcher` support the self-hosted stack, and what exactly must change |
| [`docs/SERVER-STACK.md`](docs/SERVER-STACK.md) | The server half: build, data, database, bots, supervision, and what of the guide transfers |
| [`docs/RUNBOOK.md`](docs/RUNBOOK.md) | The ordered, copy-pasteable device steps (Termux + proot Ubuntu), with the gates to check as you go |
| [`docs/CREATURE-GEAR-DESIGN.md`](docs/CREATURE-GEAR-DESIGN.md) | Mobs that wear what they drop: what needs a client patch (almost nothing), what needs code, and how the SQL rides the release — **implemented as `modules/mod-creature-gear`**; its status table lists what is verified and where |
| [`docs/LOOT-BAGS-DESIGN.md`](docs/LOOT-BAGS-DESIGN.md) | Level-banded loot bags on every lootable creature: quality tiers, money, cloth, potions and level-appropriate gear, delivered as a pure database patch in the core's CUSTOM SQL slot |
| [`docs/BOT-DISTRIBUTION-DESIGN.md`](docs/BOT-DISTRIBUTION-DESIGN.md) | Player-centric randombot distribution: levels within a band of online players and teleports near them, delivered as a patch on the pinned `mod-playerbots` |
| [`modules/mod-hardcore/README.md`](modules/mod-hardcore/README.md) | Opt-in permadeath ruleset, vendored from `maluramichael/mod-hardcore` with the Thor-local changes recorded in `UPSTREAM.md` |
| [`scripts/thor-install.sh`](scripts/thor-install.sh) | Does the whole device install for you: Termux packages, rootfs, server files, MySQL, configs, first boot, GM account, start/stop scripts |
| [`pins.toml`](pins.toml) | Exact commits, artifact hashes and ports every document refers to |
| [`.github/workflows/server-aarch64.yml`](.github/workflows/server-aarch64.yml) | Builds the linux/arm64 server + SQL sources on a native arm64 runner and attaches them to a release (the client data is fetched on the device: a single release asset stops at 2 GiB) |

## Install on the Thor

The Thor pulls two things from GitHub: this script, and the server artifact from a release. Both come from this public repository, so fetching needs no token:

```sh
curl -fLO https://raw.githubusercontent.com/nick-monto/acore-thor-selfhost/main/scripts/thor-install.sh
bash thor-install.sh --check      # report what it would do, change nothing
bash thor-install.sh              # do it (server download ~1.2 GB, or --from-source to compile)
```

A token is needed only if you have forked this repository privately or are pulling from a private fork: fetch the script from that fork's raw URL with an `Authorization: Bearer` header, and pass `--github-token` (a fine-grained token with `contents: read`) or set `THOR_GH_TOKEN` in the environment.

`--github-token` (or `THOR_GH_TOKEN` in the environment) is used for the release probe, the artifact download and its checksum file, and is recorded in the mode-600 state file so `--repair` can re-download later. Copying the script over with `scp` works too.

The creature-gear dataset is committed as generated SQL, so a normal install (`--from-source` included) builds it as-is with no extra step. To change it, regenerate from a checkout of the pinned core (`pins.toml`) and rebuild:

```sh
python3 scripts/creature-gear/gen_creature_gear.py --core <core> --out modules/mod-creature-gear/data/sql/world/base
python3 scripts/creature-gear/check_creature_gear.py --core <core>
```

`modules/mod-hardcore` is vendored at the upstream commit in [`pins.toml`](pins.toml); the local changes are listed in [`modules/mod-hardcore/UPSTREAM.md`](modules/mod-hardcore/UPSTREAM.md). It adds the opt-in `.hardcore on` ruleset, logs every hardcore death, and defaults to `Hardcore.DeathPolicy = ghost` (resurrection refused, graveyard release allowed). The build copies it beside `mod-creature-gear`; `thor-install.sh --from-source` fetches both module trees from this repository.

The loot-bag patch is also committed as generated SQL, at `sql/custom/db_world/`, and rides the artifact's `src/data/sql` tree into the core's CUSTOM slot; `thor-install.sh --from-source` fetches the same directory. Regenerate it with the pinned core:

```sh
python3 scripts/loot-bags/gen_loot_bags.py --core <core>
python3 scripts/loot-bags/gen_loot_bags.py --core <core> --check
```

The player-centric randombot pass is maintained as `patches/playerbots-player-centric.patch` against the pinned `mod-playerbots` commit. The workflow applies it before the arm64 build, `thor-install.sh --from-source` applies it after cloning the module, and the installer writes the near-player defaults into `playerbots.conf`: the bots assigned to the player are capped at 10 and placed at level-appropriate spots in the player's current zone, at least 100 yards away, never in a ring around them; each bot then visits for 15-40 minutes and spends 4-10 away in another zone, so the zone's population changes over time. The rest of the bots keep the normal world-wide distribution. See [`docs/BOT-DISTRIBUTION-DESIGN.md`](docs/BOT-DISTRIBUTION-DESIGN.md).

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

- One rootfs directory holds the Unix side: proot-distro keeps that filesystem in `$PREFIX/var/lib/proot-distro/containers/ubuntu/rootfs`, older installations in `installed-rootfs/ubuntu`, and the script probes for whichever is there (`THOR_ROOTFS_DIR` names it explicitly). One install root (`/opt/acore`, `--root` to move it) holds the server, the configs, the client data, the SQL sources, the logs **and the MySQL data directory** (`<root>/db`) with its socket (`<root>/run`). Nothing is left in `/var/lib/mysql`.
- Everything created *outside* those two — the two Termux wrappers, the script's copy in the rootfs, `/usr/sbin/policy-rc.d`, the `_CONF_DIR` symlink when `--root` is used — is appended to a manifest (`~/.thor-wow/manifest.txt` on the Termux side, `<root>/.thor-state/manifest.txt` inside), and removal walks that manifest, refusing any path that is not clearly owned by the install.
- Flags and secrets are recorded in `~/.thor-wow/state.env` / `<root>/.thor-state/state.env`, so `--status`, `--repair` and `--uninstall` work without re-passing `--bots`, `--account` or `--db-pass`.

Confidence labels: **[V]** verified against source or an artifact inspected here, **[R]** reported, **[I]** inference.

## Hard requirements

- A **3.3.5a (build 12340) Windows x86 client** — the game files are supplied by the user; nothing here redistributes them.
- ~10 GB for the client, ~5 GB for the server (binaries + `maps`/`vmaps`/`mmaps` + world/playerbots database).
- 16 GB RAM shared between server and client: bot count has to be measured, not copied from desktop guides.

## Licence

GPL-3.0. No Blizzard client data, no AzerothCore database dumps and no server binaries are committed: the repository holds recipes, pins, hashes and the module's sources, and the built server is published as a release asset.
