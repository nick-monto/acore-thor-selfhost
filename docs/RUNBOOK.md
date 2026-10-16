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

It installs the rootfs, unpacks the prebuilt aarch64 server (binaries, configs, client data and the SQL sources), initialises and starts MySQL, creates the databases and the `acore` user, writes `worldserver.conf`/`authserver.conf`/`playerbots.conf` in place, boots the world once to populate the databases, creates a GM account, and writes `~/start-wow-server.sh` + `~/stop-wow-server.sh`. Passwords land in `/opt/acore/credentials.txt`.

Useful flags: `--bots N` (default 50), `--lan-ip <ip>` (bind to the LAN and pin the realm to it), `--data-zip none|<path>` (skip or supply the client data yourself), `--root DIR`, `--boot-timeout SECS`, `--force`, `--from-source`. Re-running is safe.

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
  ca-certificates curl unzip zstd git cmake ninja-build make clang lld \
  libboost-all-dev libssl-dev libreadline-dev zlib1g-dev libncurses-dev \
  libmysqlclient-dev mysql-server
mysql --version        # expect 8.0.x — the arm64 package exists in noble [V]
```

## Part 2 — Get the server binaries

**Preferred: use the CI artifact** (no compilation on the device). It contains the binaries, the configs, the client data *and* the SQL sources, already laid out as the server expects them:

```bash
curl -fLO https://github.com/nick-monto/acore-thor-selfhost/releases/latest/download/acore-server-arm64.tar.zst
curl -fLO https://github.com/nick-monto/acore-thor-selfhost/releases/latest/download/acore-server-arm64.tar.zst.sha256
sha256sum -c acore-server-arm64.tar.zst.sha256
tar --zstd -xf acore-server-arm64.tar.zst -C /opt/acore   # → /opt/acore/{bin,etc,data,src}
```

`bin/` is the server, `etc/` the config templates, `data/` the `dbc`/`maps`/`vmaps`/`mmaps` trees, and `src/` the SQL the DB updater reads (`src/data/sql`, `src/modules/mod-playerbots/data/sql`). That last part is why the artifact is not just a binary drop: `SourceDirectory` must point at a tree containing those paths (`DBUpdater.cpp:98-195`, `mod-playerbots/src/Script/Playerbots.cpp:77-78`) [V].

**Fallback: build in place** (hours; the workflow exists to avoid this):

```bash
cd /opt && git clone --depth 1 -b Playerbot https://github.com/mod-playerbots/azerothcore-wotlk.git acore-src
cd acore-src && git fetch --depth 1 origin 8a4e76eac8c4ccebf547113afe7b51d6c3e91641 && git checkout FETCH_HEAD
git clone --depth 1 https://github.com/mod-playerbots/mod-playerbots.git /tmp/pb
cd /tmp/pb && git fetch --depth 1 origin d750b4b3a50effdf22bedc8032f0e8e5f7cb8091 && git checkout FETCH_HEAD
cp -a /tmp/pb /opt/acore-src/modules/mod-playerbots        # path is load-bearing
cd /opt/acore-src && cmake -S . -B build -G Ninja -DCMAKE_INSTALL_PREFIX=/opt/acore \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DAPPS_BUILD=all -DTOOLS_BUILD=all \
  -DSCRIPTS=static -DMODULES=static -DBoost_USE_STATIC_LIBS=ON
cmake --build build --parallel "$(nproc)" && cmake --install build
mkdir -p /opt/acore/data && curl -fL -o /tmp/data.zip \
  https://github.com/wowgaming/client-data/releases/download/v20.0/data.zip
echo "a3d4df635ae6c2c8f08052c32a79e0f806955150ad36b014a823dd08a32a4610  /tmp/data.zip" | sha256sum -c -
unzip -q -o /tmp/data.zip -d /opt/acore/data && rm /tmp/data.zip
```

The client-data download supplies `dbc/`, `maps/`, `vmaps/` and `mmaps/` — **no WoW client is needed for the server** [V].

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
| Login/world/character database info | `127.0.0.1;3306;acore;changeme;acore_*` | match Part 3 |

`/opt/acore/etc/modules/playerbots.conf` — the module ships `Playerbots.Enabled = 1` and **500/500** bots by default; start smaller (Part 5). It also carries its own `PlayerbotsDatabaseInfo` (default database name `acore_playerbots`) and `Playerbots.Updates.EnableDatabases = 1` [V].

One trap worth knowing before you debug a "bots do not load" symptom: **module configs are read from the compile-time `_CONF_DIR`, not from the `-c` path** (`Config.cpp:704-711`, `:766`) [V]. The artifact is built with `-DCMAKE_INSTALL_PREFIX=/opt/acore`, so `/opt/acore/etc/modules/playerbots.conf` is the file it reads. If you install the tarball somewhere else (`--root`), the script links that baked path to your `etc/` directory.

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

## Part 6 — Prove it with a client before touching the launcher

On a PC with the same 3.3.5a client: set `Data/<locale>/realmlist.wtf` to `set realmlist <thor-lan-ip>`, log in with the account from Part 5. This closes **G3** without any Android-specific risk.

`bash thor-install.sh --lan-ip <thor-lan-ip>` does both halves of the switch for you (binds the server and pins realm 1); re-run without `--lan-ip` afterwards to go back to loopback-only. Android will prompt for the LAN permission the first time the server binds off-loopback [I].

If that fails, the launcher work is pointless — fix the server first.

## Part 7 — Supervision

The install script writes these: `~/start-wow-server.sh` (wake lock + `proot-distro login ubuntu -- /opt/acore/run.sh`), `~/stop-wow-server.sh`, and inside the rootfs `/opt/acore/run.sh` (mysqld → authserver → worldserver, restarting the world if it dies, Ctrl-C stops everything), `/opt/acore/stop.sh`, `start-db.sh`, `stop-db.sh`.

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

## Part 10 — Fallbacks if a gate fails

| Failure | Next thing to try |
|---|---|
| 32-bit exe will not start | bundle a 32-bit DXVK component (the shipped one has no `syswow64`), or port `wow-mobile`'s box64+glibc container configuration |
| `mysqld` unstable under proot | native Termux MariaDB (the `≥ 8.0` configure check passes numerically [V]) or a pre-initialised database file restored into the rootfs |
| Aarch64 build fails in CI | the failing file is the answer: the core has no Android-specific code, so an aarch64 portability break is a bug report against the fork |
| Everything works but you want one app | the in-launcher server subsystem in `docs/LAUNCHER-HOSTING-DESIGN.md` §4 becomes worth its cost |
