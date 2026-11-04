# acore-thor-selfhost

Hosting a stock **AzerothCore WoTLK (3.3.5a) + `mod-playerbots`** realm on the **AYN Thor** (Snapdragon 8 Gen 2, Adreno 740, 16 GB LPDDR5X, Android 13) so it can be played on the device itself — no PC, no Docker.

Scope: upstream AzerothCore as used by the [Dad's MMO Lab WotLK guide](https://github.com/DadsMmoLab/dads-mmo-lab/blob/main/guides/wow-wotlk/WoW-WotLK-HOWTO.md) (`mod-playerbots/azerothcore-wotlk` + `mod-playerbots/mod-playerbots`). Out of scope: other AzerothCore forks and their clients.

## Status

The server half is built and running on the Thor. Nine pieces ride the same arm64 artifact:

- **Core and bots.** AzerothCore linked statically with `mod-playerbots`, `mod-creature-gear`, `mod-hardcore`, `mod-e-interact`, `mod-survivor`, `mod-mercenaries` and `mod-skateboard`, plus `patches/playerbots-player-centric.patch`.
- **Creature gear.** Every creature the rules admit - humanoids of level 1-80 with no quest, vendor or trainer flag and no hand-set equipment, 1274 of them against the pinned base dumps - gets per-spawn weapon pools, drops the gear it is wearing, and wears an eleven-slot armour outfit over the mirror-image packet; server-side data and logic only, no client patch. Class proficiency comes from the client's own `SkillLine`/`SkillLineAbility` data, and the validator rejects any pick a class could not use.
- **Hardcore.** Opt-in ruleset, `Hardcore.DeathPolicy = ghost` by default, bot-controlled sessions excluded. It is the top rung of the one-way mode ladder: `.hardcore on` also accepts a Survivor-flagged character, drops that flag, and a Hardcore death leaves its gear and bags in a container for any Survivor-flagged character to pick up (announced when `Survivor.AnnounceDeaths = 1`).
- **Loot bags.** Level-banded bags on every lootable creature, delivered as pure database SQL in the core's CUSTOM slot.
- **One interact key.** `mod-e-interact` plus the launcher's EInteract add-on: one bound key loots the nearest corpse, skins or gathers it, opens a chest, herb or ore node, or talks to the nearest NPC — with the gameobject pass added here, which upstream does not have. The add-on declares a client binding (`CLICK EInteractButton:LeftButton`) instead of taking a literal key, and adds a named row for it to ConsolePort's `/cp` menu, so a pad button is assigned there.
- **Mercenaries.** Every innkeeper carries one extra gossip line: recruit up to two named bots and keep them as a party. A mercenary is a playerbots "AddClass" character, so it keeps its gear and saves itself; it arrives within ±2 levels of you, is locked to one premade PvE spec that decides its talents and its gear (it keeps every ability its class and level give it), and passes on every loot roll so the loot-bag economy stays yours. The picker shows a random 10-15 of a ~150-character pool as one selection per pool cycle: the first open after the database is re-read (every 40-60 minutes, or on a restart) deals it, and every open until then shows the same mercenaries, so the crowd cannot be rerolled by walking back into the inn. A third mercenary is refused — fill the group with strangers from the world. The roster survives a relog and a server restart. A party row now opens that mercenary's page — follow me, hold here, attack my target, restock and repair, retrain to the level it would be offered at now, dismiss — so an outgrown mercenary stays useful without waiting for its face to come round again. Restock repairs it and replaces the food, potions, reagents and ammo it carries for a level-squared fee; fighting and questing beside you earns it affinity, which hands back one of the abilities it was not dealt and raises its next build's gear ceiling without raising its price; and each voice speaks its own lines on a kill, its death and its dismissal.
- **Survivor.** Opt-in ruleset, one rung below Hardcore on a one-way ladder: dying takes the equipped items and the bag contents, lays them in a lootable container at the death spot, marks that spot on the map through the add-on channel, and re-rolls a bot's gear while leaving what it carried in a lootable container any Survivor-flagged character may take. Any Survivor may open any death container — its own, another player's, a bot's, a fallen Hardcore character's — and a character that never opted in can open none. There is no ghost and no corpse run: the character wakes alive at the closest graveyard, at full health, without resurrection sickness, with bones where the corpse was, and walks back to the container. Battlegrounds and arenas are untouched. Bot-controlled sessions cannot opt in, but they follow the ruleset when `Survivor.ApplyToBots = 1`.
- **Skateboard.** An opt-in board, off until `Skateboard.Enable = 1` in `etc/modules/mod-skateboard.conf` (`SKATEBOARD_ENABLE=1` on the Windows container, which renders that key at every start because `/opt/acore/etc` is not a volume): the module integrates momentum from the terrain slope, one push impulse per press and a two-term friction and publishes it as a run-speed *cap* (`SetSpeed(MOVE_RUN, v / 7.0f, true)`) behind the `SMSG_FORCE_RUN_SPEED_CHANGE` / `CMSG_FORCE_RUN_SPEED_CHANGE_ACK` gate, so the client is never ahead of the server and the core never kicks for "Incorrect speed". The deck is a mount display id, stock `Log Ride` 26609 unless the client patch installs the two shield decks (`engravedwall` 90001, `coffinlid` 90002); tricks are stock jump emotes with a configurable crash. Momentum is the deck's: with no key held it carries the rider with one horizontal impulse along the facing every `Skateboard.Drive.ImpulseMs` (`Unit::KnockbackFrom`, so the character stays in control), forward pumps and backward brakes (`Skateboard.Brake`). Commands are `.board on|off|toggle`, `.board status`, `.board speed`, `.board push` and `.board trick <1|2|3>`, and the launcher's Skateboard add-on turns them into click bindings and a speed readout. It ships no SQL and stores nothing.
- **Travel.** A flight is dead time on a handheld, so the installer writes `InstantFlightPaths = 2` (the instant teleport still charges the fare and each flight master keeps a toggle, so a player can fly normally when wanted), `AllFlightPaths = 1` (every taxi node known from login) and `TaxiFlightSpeed = 50.0`, the top of the core's clamp — anything above 50 is rejected and falls back to the 32 default. The same install adds a two-way capital network as pure custom SQL: 16 portal spawns (guids 9800000-9800015), two per capital city standing at that city's own teleport arrival point and casting the two same-faction cities on the other continent. They reuse stock `gameobject_template` type-22 portal entries, so there is no DBC change, no new template and no module.

Runtime is verified on the Thor itself: the installer built the rootfs, unpacked the arm64 artifact and created the database, the first boot populated the three `acore_*` schemas and the GM account, a real 3.3.5a client logged in, and the realm ran with **up to 50 randombots** on the device.

Not verified yet: how a real client renders a creature-gear outfit, a gear item or a loot bag taken off a corpse in play, the hardcore death/relog path, the portal rows, the survivor container, its loot window, its map pin and the revival at the graveyard, every mercenary behaviour — the innkeeper line, the level window, the archetype restrictions and the loot pass — and the whole skateboard: whether `Skateboard.MountedPose = 0` draws a mount display id under a standing player at all, the deck rows, the speed cap under a real client's movement, and the add-on's bindings. No portal has been clicked in a client, no innkeeper has been talked to in one and no board has been ridden in one. The portal patch is database rows, and the mechanism is the one the stock Shattrath portals already use.

Playing and hosting are separate here. Playing a self-hosted 3.3.5a realm needs the launcher app plus the client-side changes listed in [`docs/LAUNCHER-HOSTING-DESIGN.md`](docs/LAUNCHER-HOSTING-DESIGN.md); the app's shipped runtime already carries the 32-bit Windows client path. Hosting the server *inside* the app is out of scope for this repository: the app has no foreground service, one X environment, one shared rootfs and UID-wide process sweeps, so the guide's Docker flow does not transfer. The server runs beside the app, in Termux.

## Documents

| Doc | What it answers |
|---|---|
| [`docs/LAUNCHER-HOSTING-DESIGN.md`](docs/LAUNCHER-HOSTING-DESIGN.md) | Can `wyattabuntjer/AYN-Thor-WoW-Launcher` support the self-hosted stack, and what exactly must change |
| [`docs/SERVER-STACK.md`](docs/SERVER-STACK.md) | The server half: build, data, database, bots, supervision, and what of the guide transfers |
| [`docs/WINDOWS-HOST.md`](docs/WINDOWS-HOST.md) | The same realm on a Windows x64 host: the flat artifact and why its configs live in `configs/`, the release tag that must not be latest, every installer flag, the root layout, what a Windows install differs in, and the container path with its four volumes and its delete command |
| [`docs/RUNBOOK.md`](docs/RUNBOOK.md) | The ordered, copy-pasteable device steps (Termux + proot Ubuntu), with the gates to check as you go |
| [`docs/CREATURE-GEAR-DESIGN.md`](docs/CREATURE-GEAR-DESIGN.md) | Mobs that wear what they drop: what needs a client patch (almost nothing), what needs code, and how the SQL rides the release — **implemented as `modules/mod-creature-gear`**; its status table lists what is verified and where |
| [`docs/LOOT-BAGS-DESIGN.md`](docs/LOOT-BAGS-DESIGN.md) | Level-banded loot bags on every lootable creature: quality tiers, money, cloth, potions and level-appropriate gear, delivered as a pure database patch in the core's CUSTOM SQL slot |
| [`docs/TRAVEL-PORTALS-DESIGN.md`](docs/TRAVEL-PORTALS-DESIGN.md) | A two-way capital portal network: the 16 spawns and the stock type-22 gameobject templates and `spell_target_position` rows behind them, why no DBC, template or module is needed, and what is verified |
| [`docs/GRAVEYARDS-DESIGN.md`](docs/GRAVEYARDS-DESIGN.md) | A rejected patch, not a shipped feature: the custom graveyard SQL (`graveyards_10_zones.sql`) was generated, reviewed and reverted — all 15 ids were sub-areas whose parent zone is already linked, and stock coverage of the open-world questing zones is complete |
| [`docs/BOT-DISTRIBUTION-DESIGN.md`](docs/BOT-DISTRIBUTION-DESIGN.md) | Player-centric randombot distribution: levels within a band of online players and teleports near them, delivered as a patch on the pinned `mod-playerbots` |
| [`docs/INTERACT-KEY-DESIGN.md`](docs/INTERACT-KEY-DESIGN.md) | One key for loot, gathering, NPC windows, chests and nodes: the addon-whisper RPC, the local gameobject pass, and what is verified — **implemented as `modules/mod-e-interact`** |
| [`docs/MERCENARY-SYSTEM-DESIGN.md`](docs/MERCENARY-SYSTEM-DESIGN.md) | Recruiting chosen bots as a persistent party ("mercenaries") from an obelisk in town: which parts are already upstream playerbots behaviour, the roster and login hook that were missing, the gameobject gossip menu and its 16-per-page limit — **implemented as `modules/mod-mercenaries`** |
| [`docs/MERCENARY-INTEGRATION-PLAN.md`](docs/MERCENARY-INTEGRATION-PLAN.md) | The mercenary integration plan: the module route and the four hooks that make a patch unnecessary, the ±2 level window, the per-mercenary archetype and its spellbook blacklist, the two-mercenary party cap, the loot keys, the ordered phases and the acceptance criteria |
| [`docs/MERCENARY-EXPANSION-PLAN.md`](docs/MERCENARY-EXPANSION-PLAN.md) | The mercenary expansion plan: the innkeeper's per-mercenary page and its orders, repair and restock, retraining, affinity and voice lines, each with its config keys, schema, API, UI and acceptance test — **implemented in `modules/mod-mercenaries`** |
| [`docs/SURVIVOR-DESIGN.md`](docs/SURVIVOR-DESIGN.md) | Death that costs the gear instead of the character: the item-instance snapshot, the summoned chest with a pre-filled loot, ruleset-wide access to any container, the map-marker add-on message, the bot strip-and-re-roll, the Survivor → Hardcore ladder and the Hardcore death container, the schema and every core hook — **implemented as `modules/mod-survivor`**; what is verified and what needs a client is in its verification section |
| [`docs/SKATEBOARDING-DESIGN.md`](docs/SKATEBOARDING-DESIGN.md) | A feasibility study behind `modules/mod-skateboard`: a board, momentum as a server-side speed cap, ramps as knockback arcs and tricks as stock animations — and why true physics needs the client's movement code, which an MPQ-only toolchain cannot reach |
| [`docs/SKATEBOARD-FIRST-PASS.md`](docs/SKATEBOARD-FIRST-PASS.md) | The build plan for that study's first slice: a C++ module and no SQL, the two shield decks carried in the existing client patch archive, the speed-ACK gate, the wiring checklist and the per-step acceptance — **implemented as `modules/mod-skateboard`** |
| [`modules/mod-hardcore/README.md`](modules/mod-hardcore/README.md) | Opt-in permadeath ruleset, vendored from `maluramichael/mod-hardcore` with the Thor-local changes recorded in `UPSTREAM.md` |
| [`scripts/thor-install.sh`](scripts/thor-install.sh) | Does the whole device install for you: Termux packages, rootfs, server files, MySQL, configs, first boot, GM account, start/stop scripts |
| [`scripts/windows-install.ps1`](scripts/windows-install.ps1) | Does the whole Windows x64 install for you, in one elevated PowerShell 5.1 session: artifact, portable MySQL, client data, configs, first boot, GM account, start/stop scripts |
| [`scripts/syntax-check.sh`](scripts/syntax-check.sh) | Compile-checks every module translation unit with `-fsyntax-only` and the core's own flags, so a misspelt core symbol fails in seconds instead of in the hour-long arm64 build |
| [`scripts/mercenaries/check_mercenaries.py`](scripts/mercenaries/check_mercenaries.py) | Offline checks on `mod-mercenaries`: conf keys against the code and the installer, dials against playerbots' strategy vocabulary, hooks against the core's headers, no database-bound script, the tables built after the spell store, temperament keys enumerated |
| [`pins.toml`](pins.toml) | Exact commits, artifact hashes and ports every document refers to |
| [`docker/`](docker/) | The container path: a headless `ubuntu:24.04` image that downloads the published artifact instead of compiling, a `mysql:8.4` service with no published port, four named volumes (`db`, `data`, `logs`, `state`), and an entrypoint that waits for the database, creates the schemas, fetches and verifies the client data, writes the confs with `scripts/write-confs.sh`, then runs auth and world - `docker compose down -v` deletes the realm |
| [`.github/workflows/server-aarch64.yml`](.github/workflows/server-aarch64.yml) | Builds the linux/arm64 server + SQL sources on a native arm64 runner and attaches them to a release (the client data is fetched on the device: a single release asset stops at 2 GiB) |
| [`.github/workflows/server-x86_64.yml`](.github/workflows/server-x86_64.yml) | Builds the same server for linux/x86_64 on a native `ubuntu-24.04` runner and publishes `acore-server-x86_64.tar.zst` to the rolling `server-build-x86_64` release, also deliberately not marked latest so the Thor's `releases/latest` download keeps resolving - it is the payload the container image downloads for `amd64` |
| [`.github/workflows/server-win64.yml`](.github/workflows/server-win64.yml) | Builds the same server with MSVC on `windows-2022` and publishes a flat `acore-server-win64.zip` to the rolling `server-build-win64` release, which is deliberately not marked latest so the Thor's `releases/latest` download keeps resolving |
| [`.github/workflows/module-checks.yml`](.github/workflows/module-checks.yml) | Runs on every push that touches a module: compile-checks the module sources against the pinned core's own flags, then runs the offline checks. Nothing is linked or published, so it takes about two minutes instead of the arm64 job's hour |

## Install on the Thor

The Thor pulls three things from GitHub: this script, the server artifact from a release, and the `sql/custom` tree (a few small files, fetched by the installer on every run so the CUSTOM SQL is never a stale build-time snapshot). All come from this public repository, so fetching needs no token:

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

`modules/mod-hardcore` is vendored at the upstream commit in [`pins.toml`](pins.toml); the local changes are listed in [`modules/mod-hardcore/UPSTREAM.md`](modules/mod-hardcore/UPSTREAM.md). It adds the opt-in `.hardcore on` ruleset, logs every hardcore death, and defaults to `Hardcore.DeathPolicy = ghost` (resurrection refused, graveyard release allowed). The build copies it beside `mod-creature-gear`, `mod-e-interact` and `mod-survivor`; `thor-install.sh --from-source` fetches every in-repo module tree from this repository.

`modules/mod-e-interact` is vendored the same way (upstream commit in [`pins.toml`](pins.toml), local gameobject pass recorded in [`modules/mod-e-interact/UPSTREAM.md`](modules/mod-e-interact/UPSTREAM.md)). It has no SQL tree: it answers the EInteract add-on's self-addressed whisper and runs the client's own loot, gossip and object-use handlers. The launcher ships the add-on.

`modules/mod-survivor` is an in-repo module like `mod-creature-gear` (pinned by this repository's own ref, not a third-party commit): `server-aarch64.yml` copies it beside the other modules before configure, and `thor-install.sh --from-source` extracts the same tree. It ships one SQL file, `data/sql/world/base/survivor_10_gameobject.sql` (the container gameobject template, entry 5000000), which the workflow packs as `$PREFIX/src/modules/mod-survivor/data/sql` so the DB updater applies it; its three character tables are created programmatically at startup instead of by module SQL, because a failing module SQL file aborts the boot on this fork. The conf is `/opt/acore/etc/modules/mod-survivor.conf` and the installer restores any key that is missing or outside its range. Hardcore and survivor are both per character and form a one-way ladder: `.survivor on` refuses a Hardcore-flagged character, while `.hardcore on` accepts a Survivor-flagged one and drops that flag.

`modules/mod-mercenaries` is an in-repo module like `mod-survivor`: `server-aarch64.yml` copies it beside the other modules, and `thor-install.sh --from-source` extracts the same tree. It ships no SQL at all: the picker is one gossip line on every innkeeper, hung on the two `AllCreatureScript` gossip hooks so that an innkeeper with an AI of its own keeps it, and so it needs no template row and no spawn, and its roster table is created programmatically at startup for the same reason `mod-survivor`'s tables are. A mercenary is a playerbots "AddClass" character recruited into the party, so it levels, gears and saves itself; the module adds the roster, the doorway, the ±2 level window and a premade-spec archetype whose signature spells of other specs it can never learn. The installer writes `Playerbots.AddClassCommand = 0` so the innkeeper line is the only doorway, caps the pool at five accounts, and sets the three loot keys that keep mercenaries from competing for drops. `python3 scripts/mercenaries/check_mercenaries.py` checks the conf, SQL, schema, loader symbol and wiring offline.

`modules/mod-skateboard` is an in-repo module like `mod-survivor` and `mod-mercenaries`: `server-aarch64.yml`, `server-x86_64.yml` and `server-win64.yml` copy it beside the other modules before configure, and `thor-install.sh --from-source` extracts the same tree (loader symbol `Addmod_skateboardScripts()`). It ships no SQL and is config-only: every knob lives in `conf/mod-skateboard.conf.dist`, installed as `/opt/acore/etc/modules/mod-skateboard.conf`, and nothing is persisted, so no table is created and the DB updater has nothing to apply. `Skateboard.Enable = 0` is the shipped default — the feature is opt-in. `Skateboard.Deck` takes `log` (the stock `Log Ride` display id 26609, no client patch), `engravedwall` (90001) or `coffinlid` (90002), the two shield decks the client patch adds; a deck the client's DBC does not know warns once and falls back to 26609. The deck is written as `UNIT_FIELD_MOUNTDISPLAYID` alone with `Skateboard.MountedPose = 0` and through `Mount()` with `1`. Commands are `.board on`, `off`, `toggle`, `status`, `speed`, `push` and `trick <1|2|3>`; `status` answers `boarded=<0|1> tricks=<n> push=<0|1> deck=<displayId>` on one line, which is what the add-on parses. The client half is the launcher's Skateboard add-on; `docs/RUNBOOK.md` Part 5d has the operator steps. Design and build plan: [`docs/SKATEBOARDING-DESIGN.md`](docs/SKATEBOARDING-DESIGN.md), [`docs/SKATEBOARD-FIRST-PASS.md`](docs/SKATEBOARD-FIRST-PASS.md).

The loot-bag and portal patches are also committed as generated SQL, at `sql/custom/db_world/`, and land in the core's CUSTOM slot under `SourceDirectory`: the installer's `refresh_custom_sql()` fetches `sql/custom` from this repository at `SELF_REF` (default `main`, resolved to a commit on the first fetch and recorded as `SELF_REF_SHA`, see the runbook's Part 0b) on every install and re-run, and `--from-source` extracts the same directory with the module sources. A rebuilt artifact is not needed to ship a SQL change — but the commit has to be on the ref that `SELF_REF` names. Regenerate them with the pinned core:

```sh
python3 scripts/loot-bags/gen_loot_bags.py --core <core>
python3 scripts/loot-bags/gen_loot_bags.py --core <core> --check
python3 scripts/travel/gen_portals.py --core <core>
python3 scripts/travel/gen_portals.py --core <core> --check
```

The player-centric randombot pass is maintained as `patches/playerbots-player-centric.patch` against the pinned `mod-playerbots` commit. The workflow applies it before the arm64 build, `thor-install.sh --from-source` applies it after cloning the module, and the installer writes the near-player defaults into `playerbots.conf`: the bots assigned to the player are capped at 10 and placed at level-appropriate spots in the player's current zone, at least 100 yards away, never in a ring around them; each bot then visits for 15-40 minutes and spends 4-10 away in another zone, so the zone's population changes over time. The rest of the bots keep the normal world-wide distribution. See [`docs/BOT-DISTRIBUTION-DESIGN.md`](docs/BOT-DISTRIBUTION-DESIGN.md).

Then `~/start-wow-server.sh`. `docs/RUNBOOK.md` explains each step and every flag.

## Install on Windows

The same realm builds for a Windows x64 host: the same core pin, the same seven modules and the same custom SQL, compiled with MSVC on `windows-2022` and published as a flat `acore-server-win64.zip` (both binaries, their DLLs, `configs\` and the SQL sources — no client data, the installer fetches and verifies that). Run this from an elevated Windows PowerShell 5.1 session:

```powershell
Invoke-WebRequest -Uri https://raw.githubusercontent.com/nick-monto/acore-thor-selfhost/main/scripts/windows-install.ps1 -OutFile windows-install.ps1
.\windows-install.ps1 -Check      # report what it would do, change nothing
.\windows-install.ps1             # install to C:\acore (-Root to move it)
```

It installs portable MySQL, fetches the pinned client data, writes the same conf keys, first-boots the realm to populate the databases and create the GM account, and leaves `start.ps1` / `stop.ps1` / `set-bots.ps1` behind. [`docs/WINDOWS-HOST.md`](docs/WINDOWS-HOST.md) has every flag, the root layout and what differs from the Thor install. The Windows build and install have not been run yet: the first `server-win64` workflow run is the gate.

The same script also has a container mode, for a host that would rather have neither MySQL nor the server as native processes: `.\windows-install.ps1 -Docker` fetches the four compose files, builds the image and starts the realm, and `docker compose down -v` deletes it — database, client data and logs included. It needs a reachable Docker daemon, and the same caveat holds: no image build and no container start has been run yet.

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
- 16 GB RAM shared between server and client: 50 randombots is the measured ceiling so far on the Thor, not a desktop-guide figure.

## Licence

GPL-3.0. No Blizzard client data, no AzerothCore database dumps and no server binaries are committed: the repository holds recipes, pins, hashes and the module's sources, and the built server is published as a release asset.
