# Runbook: bring the server up on the Thor

Ordered, copy-pasteable steps for delivery option **A1** (`docs/SERVER-STACK.md` §2): AzerothCore + `mod-playerbots` in an Ubuntu arm64 rootfs under proot, played by the launcher's client half.

**Nothing in this document has been executed on a Thor yet.** Steps marked [V] come from verified sources (repo code, release artifacts); steps marked [I] are inferences that the device run has to confirm. Numbers such as ports, timeouts and bot counts come from the upstream configs and the guide; where they were wrong, this runbook uses the corrected value.

Pins: `pins.toml`. Gates referenced below: `docs/SERVER-STACK.md` §9.

---

## Fast path

The whole install is one script. In Termux:

```sh
curl -fLO https://raw.githubusercontent.com/nick-monto/acore-thor-selfhost/main/scripts/thor-install.sh
bash thor-install.sh --check     # preflight: rootfs, disk, release asset, no changes
bash thor-install.sh             # do it: ~1.2 GB download, or --from-source to compile in place
```

This runs for an hour or more — apt, the client-data download, then a first boot that imports the base SQL and updates and waits up to `--boot-timeout` (1800 s by default). Keep Termux in the foreground with the screen on while it runs: it takes a wake lock before entering the rootfs, but Android can still freeze or kill a backgrounded Termux, and a frozen install stops with no further output, which looks like a crash. `--check` first if you want to know what it will do without the wait.

**Private repository?** Both the script fetch above and the artifact download need a token: fetch it with `curl -H "Authorization: Bearer $TOK"` and run the script with `--github-token "$TOK"` (or `THOR_GH_TOKEN=$TOK`). See the README for the exact commands; the flag is also recorded in the state file, so `--repair` keeps working.

The artifact the script downloads comes from the rolling `server-build` release that `.github/workflows/server-aarch64.yml` refreshes on every successful run (and from a `server-v*` tag release if you cut one). `--check` probes that URL and says so if it is not there yet. The release notes are rewritten on every run and name this repository's commit, the run that produced the asset and the asset's sha256, which is the value the installer records as `ARTIFACT_SHA` in `/opt/acore/.thor-state/state.env`: compare the two to tell which build a device is actually running.

It installs the rootfs, unpacks the prebuilt aarch64 server (binaries, configs and the SQL sources), fetches and verifies the client data, initialises and starts MySQL, creates the databases and the `acore` user, writes `worldserver.conf`/`authserver.conf`/`playerbots.conf` in place, boots the world once to populate the databases, creates a GM account, and writes `~/start-wow-server.sh` + `~/stop-wow-server.sh`. The GM account is `admin`/`password` (pass `--account-name`/`--account-pass` to choose differently); the generated MySQL password and both credentials land in `/opt/acore/credentials.txt`, readable from Termux at `$PREFIX/var/lib/proot-distro/containers/ubuntu/rootfs/opt/acore/credentials.txt`.

Useful flags: `--bots N` (default 50), `--lan-ip <ip>` (bind to the LAN and pin the realm to it), `--data-zip none|<path>` (skip or supply the client data yourself), `--root DIR`, `--boot-timeout SECS`, `--force`, `--from-source`. Re-running is safe, but it does not replace the server files: `fetch_server` skips the download whenever `/opt/acore/bin/worldserver` exists, so a re-run of a newer script over an older install keeps the older server and warns about every module that build lacks. `--force` is what re-downloads and overwrites the server tree; the databases, the configs and the client data are left alone.

Afterwards:

```sh
bash thor-install.sh --status       # component-by-component report; exit 1 if something needs repair
bash thor-install.sh --repair       # fix what it can, then re-check
bash thor-install.sh --uninstall --keep-db   # dump the databases, then remove everything
bash thor-install.sh --uninstall --keep-db --purge-rootfs   # ... and delete the rootfs too
```

`--status` and `--repair` read `~/.thor-wow/state.env` (Termux side) and `/opt/acore/.thor-state/state.env` (inside), so they know your bot count, account and database password without being told again.

What removal does, in order: stop the server (`<root>/stop.sh`) → inside cleanup (dump if `--keep-db`, remove the manifest entries: `policy-rc.d`, the `_CONF_DIR` symlink, stop MySQL) → move the dump to `~/.thor-wow/db-dump-<utc>.sql` → `rm -rf` the install root → remove the manifest paths on the Termux side (wrappers, the script's copy) → optionally `proot-distro remove ubuntu`.

Parts 1–9 below are exactly what the install does, in order — read them to know what to expect, or follow them by hand if you would rather drive each step.

---

## Part 0 — What you need

1. Termux from **F-Droid** (the Play Store build is deprecated) [R].
2. ~12 GB free: server ~5 GB (binaries, `maps`/`vmaps`/`mmaps` ~1.2 GB packed, world/characters DB ~1.5–2 GB) + client ~10 GB.
3. A **3.3.5a client** (`Wow.exe` + `Data/`), e.g. Chromiecraft HD or your own copy. Keep a PC copy for step 8.
4. Optional but recommended: an arm64 build of the server produced by `.github/workflows/server-aarch64.yml` (see Part 2), so the phone never compiles.

## Part 1 — Termux base

```bash
pkg update && pkg upgrade -y
pkg install -y proot-distro termux-api git curl zstd
termux-wake-lock            # keep the CPU alive while the server runs
proot-distro install ubuntu
proot-distro login ubuntu
```

Inside the rootfs:

```bash
apt-get update
apt-get install -y --no-install-recommends \
  ca-certificates curl unzip zstd git cmake ninja-build make gcc g++ clang lld \
  libboost-all-dev libssl-dev libbz2-dev liblzma-dev zlib1g-dev libzstd-dev \
  libicu-dev libreadline-dev libncurses-dev libmysqlclient-dev \
  mysql-client mysql-server
mysql --version        # expect 8.0.x — the arm64 package exists in noble [V]
```

## Part 2 — Get the server binaries

**Preferred: use the CI artifact** (no compilation on the device). It contains the binaries, the configs and the SQL sources, already laid out as the server expects them, and stays well under GitHub's 2 GiB release-asset limit because the client data is not in it — `thor-install.sh` fetches that from the pinned upstream zip itself, sha256 checked [V]:

```bash
curl -fLO https://github.com/nick-monto/acore-thor-selfhost/releases/latest/download/acore-server-arm64.tar.zst
curl -fLO https://github.com/nick-monto/acore-thor-selfhost/releases/latest/download/acore-server-arm64.tar.zst.sha256
sha256sum -c acore-server-arm64.tar.zst.sha256
tar --zstd -xf acore-server-arm64.tar.zst -C /opt/acore   # → /opt/acore/{bin,etc,lib,src}
```

`bin/` is the stripped server, `etc/` the config templates, and `src/` the SQL the DB updater reads (`src/data/sql`, `src/modules/mod-playerbots/data/sql`, `src/modules/mod-creature-gear/data/sql`, `src/modules/mod-survivor/data/sql` and the loot-bag and portal patches in `src/data/sql/custom/db_world`). That last part is why the artifact is not just a binary drop: `SourceDirectory` must point at a tree containing those paths (`DBUpdater.cpp:98-195`, `mod-playerbots/src/Script/Playerbots.cpp:77-78`) [V]. The CUSTOM slice is refreshed from this repository after unpacking (Part 4a), so the artifact's copy is only a snapshot.

**Fallback: build in place** (hours; the workflow exists to avoid this):

```bash
cd /opt && git clone --depth 1 -b Playerbot https://github.com/mod-playerbots/azerothcore-wotlk.git acore-src
cd acore-src && git fetch --depth 1 origin 8a4e76eac8c4ccebf547113afe7b51d6c3e91641 && git checkout FETCH_HEAD
git clone --depth 1 https://github.com/mod-playerbots/mod-playerbots.git /tmp/pb
cd /tmp/pb && git fetch --depth 1 origin d750b4b3a50effdf22bedc8032f0e8e5f7cb8091 && git checkout FETCH_HEAD
cp -a /tmp/pb /opt/acore-src/modules/mod-playerbots        # path is load-bearing
cd /opt/acore-src && cmake -S . -B build -G Ninja -DCMAKE_INSTALL_PREFIX=/opt/acore \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DAPPS_BUILD=all -DTOOLS_BUILD=all \
  -DSCRIPTS=static -DMODULES=static -DBoost_USE_STATIC_LIBS=ON \
  -DCMAKE_CXX_FLAGS=-Wno-psabi          # aarch64 -Wpsabi notes: see the workflow's Configure step
cmake --build build --parallel "$(nproc)" && cmake --install build
mkdir -p /opt/acore/data && curl -fL -o /tmp/data.zip \
  https://github.com/wowgaming/client-data/releases/download/v20.0/data.zip
echo "a3d4df635ae6c2c8f08052c32a79e0f806955150ad36b014a823dd08a32a4610  /tmp/data.zip" | sha256sum -c -
unzip -q -o /tmp/data.zip -d /opt/acore/data && rm /tmp/data.zip
```

The client-data download supplies `dbc/`, `maps/`, `vmaps/` and `mmaps/` — **no WoW client is needed for the server** [V]. `thor-install.sh` runs exactly this download (curl, sha256 against the pinned hash, `unzip`) as its own step, so the artifact can stay a binary drop.

## Part 3 — Database

There is no systemd inside proot, so start the daemon directly. The install keeps the data directory **inside the install root** (`/opt/acore/db`) with its socket in `/opt/acore/run`, so removing the install removes the database with it and nothing is left in `/var/lib/mysql`:

```bash
mkdir -p /opt/acore/run /opt/acore/db /opt/acore/logs
chown mysql:mysql /opt/acore/db /opt/acore/run
mysqld --initialize-insecure --user=mysql --datadir=/opt/acore/db --log-error=/opt/acore/logs/mysql.err   # first run only
mysqld --user=mysql --datadir=/opt/acore/db --socket=/opt/acore/run/mysqld.sock \
       --bind-address=127.0.0.1 --port=3306 --skip-name-resolve \
       --pid-file=/opt/acore/run/mysqld.pid --log-error=/opt/acore/logs/mysql.err --daemonize
mysql --protocol=socket --socket=/opt/acore/run/mysqld.sock -uroot -e "SELECT VERSION();"
```

To re-verify a package name for arm64 across every pocket (do not trust `packages.ubuntu.com/<release>/arm64/<pkg>` here: it answered 200 for `libreadline8`, which the archive index shows does not exist for noble/arm64 at all):

```bash
for p in noble noble-updates noble-security; do
  curl -fsSL "http://ports.ubuntu.com/ubuntu-ports/dists/$p/main/binary-arm64/Packages.gz" | zcat | grep -qx "Package: libssl3t64" && echo "$p: yes"
done
```

`liblzma-dev`, `zlib1g-dev` and `libzstd-dev` are as required as `libbz2-dev`, and nothing pulls them in: the static `Boost::iostreams` link line ends in `-lbz2 -llzma -lz -lzstd`, while `libboost-iostreams1.83-dev` declares only boost packages as its dependencies [V]. `libicu-dev` comes with `libboost-regex1.83-dev` [V].

Two notes on packages. `libbz2-dev` is required by every configure, not just when building: `deps/bzip2/CMakeLists.txt:15` calls `find_package(BZip2 REQUIRED)` on UNIX [V] — leaving it out is what failed the first arm64 CI run. The *runtime* libraries the prebuilt binaries need are not listed by hand anywhere: the script resolves them from `ldd` (`install_missing_libs`) because Ubuntu 24.04 renamed them (`libssl3`→`libssl3t64`, `libreadline8`→`libreadline8t64`) [V].

`--skip-name-resolve` avoids DNS lookups inside proot [I]. Then the schemas and the user (all further `mysql` calls need `--protocol=socket --socket=/opt/acore/run/mysqld.sock`):

```bash
mysql --protocol=socket --socket=/opt/acore/run/mysqld.sock -uroot <<'SQL'
CREATE DATABASE IF NOT EXISTS acore_auth;
CREATE DATABASE IF NOT EXISTS acore_world;
CREATE DATABASE IF NOT EXISTS acore_characters;
CREATE USER IF NOT EXISTS 'acore'@'127.0.0.1' IDENTIFIED BY 'changeme';
GRANT ALL PRIVILEGES ON acore_auth.*       TO 'acore'@'127.0.0.1';
GRANT ALL PRIVILEGES ON acore_world.*      TO 'acore'@'127.0.0.1';
GRANT ALL PRIVILEGES ON acore_characters.* TO 'acore'@'127.0.0.1';
GRANT ALL PRIVILEGES ON acore_playerbots.* TO 'acore'@'127.0.0.1';
FLUSH PRIVILEGES;
SQL
```

Do **not** create `acore_playerbots` by hand: the module's updater creates and populates it when `mod-playerbots` is present [V].

## Part 4 — Configuration

Edit `/opt/acore/etc/worldserver.conf` and `authserver.conf`:

| Key | Value | Why |
|---|---|---|
| `DataDir` | `/opt/acore/data` | holds `dbc/maps/vmaps/mmaps` |
| `LogsDir` | `/opt/acore/logs` | keeps logs off the rootfs' tmpfs |
| `SourceDirectory` | `/opt/acore/src` | **must** be set: the DB updater reads base SQL and updates from this tree, and the module reads its SQL from `<it>/modules/mod-playerbots/data/sql` [V]. Empty falls back to the build machine's path |
| `MySQLExecutable` | `/usr/bin/mysql` | the updater shells out to the `mysql` client; empty falls back to the build machine's path [V] |
| `WorldServerPort` / `RealmServerPort` | `8085` / `3724` | guide defaults |
| `BindIP` | `127.0.0.1` | loopback-only while testing on-device (use the LAN IP only for the test in Part 6) |
| `Updates.EnableDatabases` | `7` (world) / `1` (auth) | lets the first start populate base data |
| `InstantFlightPaths` | `2` | 2 = instant: the flight teleports to the last node and still charges the fare, but each flight master keeps a toggle so the player can fly normally; 1 would force it on everyone [V] |
| `AllFlightPaths` | `1` | every taxi node known from login (`SetTaxiCheater(true)` on the first login packet, `CharacterHandler.cpp:1055`) [V] |
| `TaxiFlightSpeed` | `50.0` | 50 is the top of the core's clamp (`WorldConfig.cpp:265`: accepted only when `> 0.01` and `<= 50`); anything higher is rejected and the default 32 stands [V] |
| Login/world/character database info | `127.0.0.1;3306;acore;changeme;acore_*` | match Part 3 |

The three travel keys are what `thor-install.sh` writes (`scripts/thor-install.sh:1245-1247`) and logs at the end of `write_configs()`. `--status` reads each one back and prints `ok` or `bad` with the value it found, and `--repair` puts all three back to `2` / `1` / `50.0` when any is wrong (`run_checks`, `:1640-1674`). `--check` is a dry run: it calls neither, so it reports nothing about these keys (or about the SQL families in Part 4a). The gate on the third key is `(32, 50]`, not just "not empty": the stock `32.0` counts as not set, and anything above 50 is rejected by the core and falls back to 32. `TaxiFlightSpeed` is `Reloadable::Yes` (`WorldConfig.cpp:265`), so a new value applies with `.reload config` in the worldserver console and no restart; `AllFlightPaths` is granted when a character logs in (`CharacterHandler.cpp:1055`), so a character already online needs a relog before it sees the full taxi map.

`/opt/acore/etc/modules/mod_hardcore.conf` — the installer creates it from the shipped `.dist` and keeps `Hardcore.DeathPolicy = ghost` unless the file already names `ghost`, `archive` or `delete`. In `ghost` policy, resurrection is refused but graveyard release is allowed so under-map/fatigue deaths can reach a spirit healer. Players opt in with `.hardcore on`; bot-controlled sessions are refused. Hardcore is the top rung of a one-way ladder: `.hardcore on` also accepts a Survivor-flagged character and drops that flag (Survivor mode is dropped, Hardcore is terminal), and a Hardcore death leaves the character's gear and bags in a container at the death spot for any Survivor-flagged character to pick up. There is no `.hardcore off`.

`/opt/acore/etc/modules/mod_e_interact.conf` — the interact key: `EInteract.Enable`, `.Loot`, `.Gather`, `.Talk` and `.Use` all default to 1, and the installer puts any of them back to 1 if it finds it off (a silent 0 looks exactly like a dead key in game). The key is a client binding (`Interact with the nearest thing`) registered by the launcher's bundled EInteract add-on: ConsolePort's `/cp` binding menu lists it, so any pad button can be assigned to it, and `/ei key <key>` moves it to a plain key; `/ei` reports whether the server answered. The add-on binds **E** on its first run and the pad profile sends E on the left stick click, so the button works before `/cp` is touched; ConsolePort's own left click can be given that button instead, in `/cp`. If the key says the module is missing, the running worldserver was built without it — check `thor-install.sh --check` for the config warning and rebuild.

`/opt/acore/etc/modules/mod-survivor.conf` — the survivor ruleset: `Survivor.Enable` (master switch), `MinLevelToFlag` (default 10, so a brand-new character cannot opt in before it owns anything), `AnnounceDeaths`, `DropEquipped`, `DropBags`, `ContainerLifetimeDays` (7), `MarkOnMap`, `ContainerEntry` (5000000, the gameobject template the module's SQL creates), `ApplyToBots`, `BotRegenOnDeath` and `BotDropContainer` (1: a bot death leaves a lootable container any Survivor-flagged character may take, and the bot is re-rolled; 0: the gear is destroyed outright). The installer creates the file from the shipped `.dist` and restores any key that is missing or outside its range — the 0|1 switches, `MinLevelToFlag` 1-80, `ContainerLifetimeDays` 1-365, `ContainerEntry` a positive integer — while leaving a deliberate 0 alone. **The two rulesets form a one-way ladder** (normal -> Survivor -> Hardcore, and normal -> Hardcore): `.survivor on` refuses a Hardcore-flagged character, while `.hardcore on` accepts a Survivor-flagged one and drops that flag - the promotion is one-way and cannot be undone. Only `.survivor on` and `.survivor status` exist, and both refuse bot-controlled sessions. The death flow has no ghost phase: the gear and bags go into a container at the death spot, the character is revived alive at the closest graveyard at full health and without resurrection sickness, and the corpse becomes bones, so the walk back is to the container; battleground and arena deaths keep the core's own handling ([`SURVIVOR-DESIGN.md`](SURVIVOR-DESIGN.md) §5.9).

`/opt/acore/etc/modules/playerbots.conf` — the module ships `Playerbots.Enabled = 1` and **500/500** bots by default; start smaller (Part 5). It also carries its own `PlayerbotsDatabaseInfo` (default database name `acore_playerbots`) and `Playerbots.Updates.EnableDatabases = 1` [V]. The installer sets `Playerbots.RandomBotNearPlayers = 1` with a +/-5 level window and at most 10 bots tied to each player, placed at level-appropriate spots inside the player's zone at least 100 yards away (no rings around the player; 100 is about where the client stops drawing other players), each visiting for 15-40 minutes and away in another zone for 4-10 before it returns. The rest stay normally distributed. Re-running the installer rewrites those keys to the same defaults and drops the retired ring keys. See `docs/BOT-DISTRIBUTION-DESIGN.md` [V].

One trap worth knowing before you debug a "bots do not load" symptom: **module configs are read from the compile-time `_CONF_DIR`, not from the `-c` path** (`Config.cpp:704-711`, `:766`) [V]. The artifact is built with `-DCMAKE_INSTALL_PREFIX=/opt/acore`, so `/opt/acore/etc/modules/playerbots.conf` is the file it reads. If you install the tarball somewhere else (`--root`), the script links that baked path to your `etc/` directory.

## Part 4a — The CUSTOM SQL patches

Two patches ride the core's CUSTOM slot rather than a module: the loot bags (Part 9a) and the capital portals. The installer puts `sql/custom/db_world/*.sql` under `<core>/data/sql/custom/db_world/`, i.e. under `SourceDirectory` (Part 4), and the world database updater applies each CUSTOM file **once per file name**, recording its SHA-1 hash in the world database's `updates` table [V].

The SQL no longer has to ride the artifact. `refresh_custom_sql()` runs right after `fetch_server` on every install and re-run — it is skipped under `--from-source`, which already extracts the same tree with the module sources — and it downloads `sql/custom*` from `SELF_REPO@SELF_REF` (`nick-monto/acore-thor-selfhost` at `SELF_REF`, default `main`) into `$SRC_DIR/data/sql`. A SQL change therefore reaches the device with an ordinary re-run, and the commit only has to be on the ref that `SELF_REF` names; no rebuilt artifact is involved.

| File (in `/opt/acore/src/data/sql/custom/db_world/`) | What it inserts | Generated by |
|---|---|---|
| `travel_portals_10_spawns.sql` | 16 `gameobject` spawns, guids 9800000-9800015: two per capital city, standing at that city's own teleport arrival point and casting the two same-faction cities on the other continent. Stock `gameobject_template` type-22 (spellcaster) entries — no DBC, no new template, no module [V] | `scripts/travel/gen_portals.py` |
| `loot_bags_*.sql` (three files) | the bag items, drop tables and creature drops from Part 9a | `scripts/loot-bags/gen_loot_bags.py` |

The portal file is the one that must be careful with its `DELETE`: it names its 16 guids explicitly (`WHERE guid IN (9800000, …, 9800015)`), never a range, because the core seeds its gameobject guid allocator from `MAX(guid) + 1` — once the file has applied, objects the server creates are allocated above these guids, and a range delete would take them out on re-apply. The loot-bag files delete their own private ranges instead (`item_template` / `item_loot_template` 91000-91099, `reference_loot_template` / `creature_loot_template` 9800100-9800199) [V]. Every CUSTOM file starts with a `DELETE`, which is what makes re-application idempotent.

Faction, since it is easy to misread as an access rule: seven of the eight portal templates carry Alliance 1732 (Stormwind, Darnassus, Exodar) or Horde 1735 (Orgrimmar, Undercity, Thunder Bluff, Silvermoon) in `gameobject_template_addon`, and only Ironforge's 176497 is neutral (0), used only as an Alliance destination. The server-side use path never tests that faction, so it does not gate who may click a portal [V].

Neither patch has been exercised in a client: the portal mechanism is the one the stock Shattrath portals already use with seven of the same eight templates.

Check what applied, and which files the server is carrying:

```bash
mysql --protocol=socket --socket=/opt/acore/run/mysqld.sock -uroot acore_world \
  -e "SELECT name, hash, state, timestamp FROM updates WHERE name LIKE 'travel_portals%';"
mysql --protocol=socket --socket=/opt/acore/run/mysqld.sock -uroot acore_world \
  -e "SELECT COUNT(*) AS portals FROM gameobject WHERE guid BETWEEN 9800000 AND 9800015;"
```

The `updates` table is keyed by `name`; its columns are `name, hash, state, timestamp, speed` [V], so the query above and the deletes below use `name`. `bash thor-install.sh --status` also reports each SQL family as present or missing in `$SRC_DIR/data/sql/custom/db_world/` (`scripts/thor-install.sh:1831-1844`). `--check` does not: it is a pre-install dry run and never calls the check code.

To change one, edit the generator and regenerate (the README has the commands); do **not** hand-edit the committed SQL. With the shipped `Updates.Redundancy = 1` the updater compares hashes, so a CUSTOM file whose content changed **is** re-applied on the next boot, logged as `Reapplying update "<name>" (it changed)` [V]; the `DELETE` at the top of the file makes that safe. Removing its `updates` row or renaming the file is only needed when redundancy checking is off:

```bash
# only for a server running Updates.Redundancy = 0: make the updater treat the file as new
mysql --protocol=socket --socket=/opt/acore/run/mysqld.sock -uroot acore_world \
  -e "DELETE FROM updates WHERE name = 'travel_portals_10_spawns.sql';"
```

To remove a patch for good, delete the rows it inserted, then its `updates` row (in that order the server never sees a half state; the reverse order just re-inserts them on the next start if the file is still there):

```bash
mysql --protocol=socket --socket=/opt/acore/run/mysqld.sock -uroot acore_world -e "
DELETE FROM gameobject WHERE guid IN (9800000, 9800001, 9800002, 9800003, 9800004, 9800005, 9800006, 9800007, 9800008, 9800009, 9800010, 9800011, 9800012, 9800013, 9800014, 9800015);
DELETE FROM updates WHERE name = 'travel_portals_10_spawns.sql';"
```

Then delete the file from `$SRC_DIR/data/sql/custom/db_world/` and from `sql/custom/db_world/` in the repository — `refresh_custom_sql()` would fetch it back on the next install or `--repair` otherwise — or point `SELF_REF` at a commit that no longer carries it. Restart the worldserver afterwards, since inventory rows are read at start; after that `--status` reporting the family as missing is the correct state for a patch you removed on purpose.

## Part 5 — First start

```bash
cd /opt/acore
./bin/authserver -c /opt/acore/etc/authserver.conf &   # logs to LogsDir
./bin/worldserver -c /opt/acore/etc/worldserver.conf   # first run imports base data: minutes, not seconds
```

`worldserver` prints `World initialized` when loaded; the guide allows up to 1800 s for the first import and polls the log every 5–10 s [R]. In the console:

```
account create admin changeme
account set gmlevel admin 3 -1
```

**No realm pin is needed for on-device play**: the auth database's base SQL already seeds realm 1 as `127.0.0.1`/`localAddress 127.0.0.1`/port `8085`/build `12340`, which is exactly what a loopback client wants (`data/sql/base/db_auth/realmlist.sql:48-49`) [V]. Only the LAN test in Part 6 needs a pin:

```bash
mysql -uroot acore_auth -e "UPDATE realmlist SET address='<thor-lan-ip>', localAddress='<thor-lan-ip>' WHERE id=1; SELECT id,name,address,localAddress,port FROM realmlist;"
```

Bots — the guide's `AC_AI_PLAYERBOT_*` overrides **no longer work** [V]; either edit the conf or use the current names:

```bash
# in worldserver.conf (or the container's environment if you run one later)
AC_PLAYERBOTS_MIN_RANDOM_BOTS=50
AC_PLAYERBOTS_MAX_RANDOM_BOTS=50
AC_PLAYERBOTS_RANDOM_BOT_AUTOLOGIN=1
```

Keep the bot count under `PlayerLimit` (default 1000) and remember the module's own guidance of leaving ~400 slots for city bots [R].

## Part 5a — Prove the creature-gear module is live

The module ships its own conf and SQL. Three checks, all inside the rootfs, prove it loaded:

```bash
# 1. the module conf the worldserver actually reads (compile-time _CONF_DIR, Part 4)
ls -l /opt/acore/etc/modules/mod-creature-gear.conf

# 2. the generated dataset reached the world database
mysql --protocol=socket --socket=/opt/acore/run/mysqld.sock -uroot acore_world \
  -e "SELECT COUNT(*) AS outfit_rows FROM mod_creature_gear_outfit;
      SELECT COUNT(*) AS variant_rows FROM mod_creature_gear_outfit_variant;"

# 3. the creature_equip_template load path logged no rejections
#    the server's own log is $LogsDir/Server.log; the installer also keeps the
#    redirected stdout in logs/worldserver.out
grep -E "Unknown item \(ID=|is not equipable in a hand" /opt/acore/logs/Server.log \
  && echo "REJECTIONS PRESENT" || echo "clean"
```

- (1) prints the file. If it is missing, the artifact was built without the module (Part 2) — creatures keep their stock gear.
- (2) prints a non-zero `outfit_rows` (the shipped dataset is 1274 outfit rows, one per creature) and `variant_rows` (5096 = 1274 creatures x 4 armour variants).
- (3) prints `clean`. Those two strings are the load-time rejections the core logs for bad `creature_equip_template` rows (`ObjectMgr.cpp:1522-1550`); any hit means a phase A pool row references an item the server does not know or cannot hold in a hand.

The module's own detail (`mod-creature-gear: loaded N outfit row(s) and M variant row(s) for K creature(s)`, the per-variant / per-drop / per-morph lines) is `LOG_DEBUG`, and `worldserver.conf.dist` ships `Logger.module=4,Console Server` (Info), which filters debug out — set `Logger.module=5` in `worldserver.conf` to see those lines at all. `CreatureGear.Debug` only gates the module's own lines; the logger level gates the channel.

`bash thor-install.sh --status` reports the four keys that activate the three halves (`CreatureGear.Enable`, `CreatureGear.Drop.Enable`, `CreatureGear.Appearance.Enable`, `CreatureGear.Weapons.RerollOnRespawn`); to switch one half off, set it to 0 and type `.reload config` in the worldserver console — no restart is needed. Two further keys tune rather than activate: `CreatureGear.Appearance.VaryPerSpawn` (default 1) gives each spawn point its own sampled face and hair, and `CreatureGear.Debug` (default 0, needs `Logger.module=5` to be visible) logs the sampled values. Which armour a variant carries is data, not a key: `mod_creature_gear_outfit_variant` holds one row per variant per creature and the module picks one per spawn point (`grep "wears outfit variant"` in the log with `Debug = 1`). To stop a worldserver you started by hand in Part 5, type `server exit` in its console (`quit`/`exit` are not console commands in this core; `server shutdown 1` is the timed variant the installer uses).

## Part 5b — Prove the survivor module is live

Same shape as Part 5a, for `mod-survivor`:

```bash
# 1. the module conf the worldserver actually reads
ls -l /opt/acore/etc/modules/mod-survivor.conf

# 2. the gameobject template the container is summoned from reached the world database
mysql --protocol=socket --socket=/opt/acore/run/mysqld.sock -uroot acore_world \
  -e "SELECT entry, type, displayId, name FROM gameobject_template WHERE entry = 5000000;"

# 3. the three character tables were created at startup (programmatically, not by module SQL)
mysql --protocol=socket --socket=/opt/acore/run/mysqld.sock -uroot acore_characters \
  -e "SELECT COUNT(*) AS flags FROM survivor_flags;
      SELECT COUNT(*) AS corpses FROM survivor_corpses;
      SELECT COUNT(*) AS items FROM survivor_corpse_items;"

# 4. the module registered itself
grep -i "mod-survivor\|survivor" /opt/acore/logs/Server.log | tail -5
```

- (1) prints the file. If it is missing, the artifact was built without the module and `.survivor` does not exist.
- (2) prints exactly one row: `5000000`, type `3` (chest), `displayId` 4093, name `Remains`. No row means the module SQL never applied - the container is then never summoned and the death drop is lost after the items are taken off the character.
- (3) prints `0` for all three counters on a fresh realm: the tables exist but nothing has died yet. A non-zero `survivor_flags` means a character opted in, `survivor_corpses` and `survivor_corpse_items` grow with each survivor death.
- (4) names the module's own lines. The module logs through the core's `module` logger, which `worldserver.conf.dist` ships at `Logger.module=4` (Info); set `Logger.module=5` if you want its debug detail.

Then opt a character in with `.survivor on` and die once (opt-in is one-way: there is no `.survivor off`, and a character promoted with `.hardcore on` loses the Survivor flag and can never get it back). That is the point where the container, the loot window, the map pin and the recovered item's enchantments need a client - and where the revival has to be watched: the character should wake alive at the closest graveyard, full health, no resurrection sickness, bones where the corpse was. None of it has been exercised in a client or on the Thor.

## Part 6 — Prove it with a client before touching the launcher

On a PC with the same 3.3.5a client: set `Data/<locale>/realmlist.wtf` to `set realmlist <thor-lan-ip>`, log in with the account from Part 5. This closes **G3** without any Android-specific risk.

`bash thor-install.sh --lan-ip <thor-lan-ip>` does both halves of the switch for you (binds the server and pins realm 1); re-run without `--lan-ip` afterwards to go back to loopback-only. Android will prompt for the LAN permission the first time the server binds off-loopback [I].

If that fails, the launcher work is pointless — fix the server first.

## Part 7 — Supervision

The install script writes these: `~/start-wow-server.sh [--bots N]` (wake lock + `proot-distro login ubuntu -- /opt/acore/run.sh`), `~/stop-wow-server.sh`, and inside the rootfs `/opt/acore/run.sh` (mysqld → authserver → worldserver, restarting the world if it dies, Ctrl-C stops everything), `/opt/acore/stop.sh`, `start-db.sh`, `stop-db.sh`, `set-bots.sh` (sets `Playerbots.MinRandomBots`/`MaxRandomBots` together and leaves the rest of `playerbots.conf` alone; the world reads it at its next start). `--bots N` runs `set-bots.sh` and updates the installer's recorded count before starting, so `--status` and `--repair` agree with `playerbots.conf`.

Start order is DB → authserver → worldserver; stop in reverse. Nothing here involves Wine.

If you would rather run them by hand, the loop is the same shape:

```bash
termux-wake-lock
cd /opt/acore
while true; do
  pgrep -f 'bin/worldserver' >/dev/null || ( ./bin/worldserver -c etc/worldserver.conf >> logs/supervisor.log 2>&1 & )
  sleep 15
done
```

## Part 8 — Measuring the bot ceiling (G4)

Run the worldserver with `Playerbots.MinRandomBots/MaxRandomBots` at **50**, then 150, then 300, and record:

- `worldserver` tick: the `MapUpdate` diff printed periodically in the console/log;
- RSS of `worldserver` and `mysqld` (`ps -o rss,cmd -C worldserver,mysqld`);
- whether the client still holds 60 fps while the bots are online.

Stop scaling when the tick doubles or the client drops frames: the phone has one 16 GB budget for both.

## Part 9 — Launcher side (client half)

`docs/LAUNCHER-HOSTING-DESIGN.md` §3 is the change list: a `LOCAL_AC` flavor (no `.build.info`, `Wow.exe` + `Data/`), relaxed version/login gates, container profile with `emulator="Box64"`, `wow64Mode=true`, no `-d3d11`, and `realmlist`/`realmList` writing to `127.0.0.1`.

Four tests, in this order — each one is a gate:

1. **Container boots a 32-bit exe at all** (`wowbox64.dll` HODLL path). Nothing else matters if this fails.
2. **Login screen appears** (rendering through Wine's 32-bit `wined3d`; if the screen is black, try `wincomponents direct3d=1` explicitly).
3. **Realm appears** in the list (the server's `realmlist` row is what the client asks for).
4. **In-world**, with the server's bots online, at a frame rate you are willing to live with.

## Part 9a — Loot-bag icons

The bag items are custom entries (91000-91063), so the client has no `Item.dbc` record for them: the loot window shows the icon because that packet carries it, and the player's bags show a red question mark. One command fixes it, run once per client (and again after a client update replaces `Item.dbc`):

```bash
pkg install python          # once, in Termux
RF=$PREFIX/var/lib/proot-distro/installed-rootfs/ubuntu
python3 scripts/item-dbc/patch_item_dbc.py \
    --dbc "$RF/opt/acore/data/dbc/Item.dbc" \
    --client-data "/sdcard/AzerothCore 3.3.5a/Data" \
    --server-dbc "$RF/opt/acore/data/dbc"
```

It appends the 64 records to `Item.dbc`, writes `patch-4.MPQ` into the client's `Data` directory (first free name from `patch-4` to `patch-9`, then `patch-A` up), and puts the patched `Item.dbc` where the server reads it. Restart the client afterwards.

Check: bags in the player's bags show the tier icon instead of a question mark, and `Item (Entry: 910xx) does not exist in item.dbc!` no longer appears in the worldserver log at debug level. `docs/LOOT-BAGS-DESIGN.md` § "The bag icon" has the mechanism and the archive layout.

## Part 10 — Fallbacks if a gate fails

| Failure | Next thing to try |
|---|---|
| The install goes quiet mid-way and never prints again | Two causes, both now made visible. Android freezing a backgrounded Termux: check from a second Termux session with `ps -A -o pid,stat,args \| grep -E 'thor-install\|mysqld'` — a `T` state means frozen (`kill -CONT <pid>`, or bring Termux to the front). Or a wedged `mysqld`: it answers `ping` but never finishes the first statement that writes, because proot has no `libaio` and InnoDB's native AIO path blocks. The installer now starts every server with `--innodb-use-native-aio=0` and probes a write with a timeout before using it, so this fails with a message instead of hanging: stop any server left from an earlier run (`mysqladmin --protocol=socket --socket=/opt/acore/run/mysqld.sock -uroot shutdown`, or `pkill -9 mysqld`) and re-run. Re-running is safe — every step is idempotent |
| `bash: /opt/thor/thor-install.sh: no such file or directory` after "Running the install inside the rootfs" | the script's copy went to a directory that is not the filesystem proot-distro boots: current releases store containers in `containers/ubuntu/rootfs`, older ones in `installed-rootfs/ubuntu`. `thor-install.sh` probes both and checks from inside; if it still fails, `ls -d $PREFIX/var/lib/proot-distro/*/*/` and re-run with `THOR_ROOTFS_DIR=<the right one>` |
| `worldserver: error while loading shared libraries: libmysqlclient.so.21: cannot open shared object file` | Since the 2026-10-19 build the tarball carries `lib/libmysqlclient.so.21` and every binary carries `$ORIGIN/../lib`, so this should no longer happen: if it does, the tarball was unpacked without its `lib/` directory, or the binary is not the published one. Older artifacts carried neither, and then: first, just `ldconfig` and retry: dpkg's triggers are easily skipped inside proot, and a stale cache gives exactly this message. If it persists, the library is genuinely absent: the prebuilt server comes from `ubuntu-24.04-arm`, whose MySQL is 8.0 with soname `.21`, while a newer rootfs ships MySQL 8.4 under a different soname — nothing apt has there can satisfy the binary, however successfully it installs. `install_missing_libs` now checks that the soname actually resolves after each candidate (apt accepting a package is not proof) and falls back to `reference_deb_for_soname`, which takes `libmysqlclient21` or `libicu74` from noble. By hand: `cd /tmp && curl -fsSLO http://ports.ubuntu.com/ubuntu-ports/pool/main/m/mysql-8.0/libmysqlclient21_8.0.36-2ubuntu3_arm64.deb && apt-get install -y ./libmysqlclient21_*.deb && ldconfig` (apt, not dpkg: the deb's dependencies may only exist under another name in a later release), then `ldd /opt/acore/bin/worldserver \| grep 'not found'`. The artifact now carries these libraries itself (`lib/`, found through a `$ORIGIN/../lib` rpath), so an install from a build after that one needs neither the fallback nor a pinned rootfs; `--from-source` remains the way to link the rootfs's own MySQL instead |
| The terminal fills with `AC>` after startup | The world server's CLI prompt, printed by GNU readline (`CliRunnable.cpp`); at end of input it returns immediately, so the prompt repeats forever. `run.sh` used to start the server as a background job, whose stdin is `/dev/null` — hence the loop. The installer now holds stdin open on a pipe and sets `Console.Enable = 0`, because the realm is headless and GM commands go in-game. To keep a console, set `Console.Enable = 1` and start it under `tmux` inside the rootfs |
| 32-bit exe will not start | bundle a 32-bit DXVK component (the shipped one has no `syswow64`), or port `wow-mobile`'s box64+glibc container configuration |
| `mysqld` unstable under proot | native Termux MariaDB (the `≥ 8.0` configure check passes numerically [V]) or a pre-initialised database file restored into the rootfs |
| Aarch64 build fails in CI | the failing file is the answer: the core has no Android-specific code, so an aarch64 portability break is a bug report against the fork |
| Everything works but you want one app | the in-launcher server subsystem in `docs/LAUNCHER-HOSTING-DESIGN.md` §4 becomes worth its cost |
