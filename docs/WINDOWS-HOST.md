# The same realm on a Windows x64 host

Companion to [`SERVER-STACK.md`](SERVER-STACK.md), which is about the Thor. This document is about
the other host shape: the identical server, built for Windows x64 by GitHub Actions and installed
by one PowerShell script.

Confidence: **[V]** verified here from source or an artifact, **[R]** reported, **[I]** inference.
Nothing in this document has been run against a real Windows installation; see
[Verified and not verified](#verified-and-not-verified).

---

## 1. What it is

One artifact, one installer, one host.

- **Same core.** `github.com/mod-playerbots/azerothcore-wotlk` at the commit in `pins.toml`,
  branch `Playerbot`, with `patches/playerbots-player-centric.patch` applied before configure -
  the same pin and the same patch the arm64 build uses.
- **Same six modules.** `mod-playerbots`, `mod-creature-gear`, `mod-hardcore`,
  `mod-e-interact`, `mod-survivor` and `mod-mercenaries`, all pinned in `pins.toml`, all compiled
  statically into the server (`-DMODULES=static`), exactly as on the Thor.
- **Same custom SQL.** The loot-bag and portal patches under `sql/custom/db_world/`, the
  creature-gear dataset, and `mod-survivor`'s container template ride the artifact's `src/` tree,
  where the core's DB updater finds them under `SourceDirectory`.
- **Same client data.** `wowgaming/client-data` v20.0, sha256
  `a3d4df635ae6c2c8f08052c32a79e0f806955150ad36b014a823dd08a32a4610`, fetched and verified by the
  installer instead of shipped in the artifact.
- **Same config values.** The installer writes every conf with the keys and values the Thor
  installer writes: bots, near-player distribution, the three loot keys that make mercenaries pass,
  `Playerbots.AddClassCommand = 0`, instant flight paths, the hardcore death policy and the survivor
  and interact dials.

What differs is only the host: an MSVC build instead of an aarch64 build, a flat directory instead
of a rootfs, portable MySQL instead of a distro package, and Windows-native start/stop scripts
where the Thor has Termux wrappers. The realm a client sees is the same realm.

| | Thor (arm64) | Windows x64 |
|---|---|---|
| Builder | `server-aarch64.yml` on `ubuntu-24.04-arm` | `server-win64.yml` on `windows-2022` |
| Artifact | `acore-server-arm64.tar.zst` | `acore-server-win64.zip` |
| Release tag | `server-build` (marked latest) | `server-build-win64` (**not** latest) |
| Installer | `scripts/thor-install.sh` (bash, Termux) | `scripts/windows-install.ps1` (PowerShell 5.1) |
| Compile on host | optional, `--from-source` | never - the workflow is the only builder |
| Config directory | `<prefix>/etc` (`_CONF_DIR`) | `<prefix>/configs` (CWD-relative) |
| Database | `mysql-server 8.0.46` in the rootfs | portable MySQL 8.4.9 in `<prefix>/mysql` |

## 2. Artifact

`acore-server-win64.zip`, with `acore-server-win64.zip.sha256` beside it.

```text
acore-server-win64.zip
├── worldserver.exe
├── authserver.exe
├── *.dll                          # the non-OS DLLs the binaries link against
├── configs/
│   ├── worldserver.conf.dist
│   ├── authserver.conf.dist
│   └── modules/*.conf.dist        # playerbots, hardcore, e_interact, survivor, mercenaries, ...
└── src/
    ├── data/sql/                  # base + custom SQL sources, the CUSTOM and MODULE slots
    ├── modules/mod-playerbots/data/sql/
    ├── modules/mod-creature-gear/data/sql/
    └── modules/mod-survivor/data/sql/
```

**The layout is flat because AzerothCore's Windows install is flat.** The install destinations are
hardcoded to `<prefix>/configs` and `<prefix>/configs/modules` on WIN32: the `elseif(WIN32)` branch of
`src/cmake/macros/ConfigInstall.cmake:40-42`, `:68-70` and `:101-103` spells out
`"${CMAKE_INSTALL_PREFIX}/configs"` instead of using `CONF_DIR`, and the binaries install to the
prefix itself (`src/server/apps/CMakeLists.txt:241-243`). [V] `CONF_DIR` is therefore irrelevant
here: Windows sets none of its own and falls back to the prefix
(`src/cmake/platform/after_platform.cmake:13-14`) where Linux uses `<prefix>/etc`
(`src/cmake/platform/unix/settings.cmake:22-23`), but neither the WIN32 install branch nor the
Windows runtime path ever reads it - so the workflow passes no `-DCONF_DIR` and there is nothing to
keep in step. The result is a flat tree: `worldserver.exe` and `authserver.exe` at the zip root,
their non-OS DLLs beside them.

**Why `configs/` and not `etc/`.** `ConfigMgr::GetConfigPath()` returns the literal `"configs/"`
under `AC_PLATFORM_WINDOWS` and the compile-time `_CONF_DIR` everywhere else
(`src/common/Configuration/Config.cpp:704-712`; the Windows branch is the `return "configs/";` at
`:709`). Module configs are then loaded from `GetConfigPath() + "modules/"` (`:766`). Two
consequences:

1. The path is **CWD-relative**, so the server must be started with its working directory at the
   install root. No absolute path is compiled in on Windows; moving the root does not break it, but
   starting `worldserver.exe` from `C:\Windows\System32` (a shortcut with no "Start in") does.
2. `-c` cannot relocate a module config, because `:766` builds that directory from
   `GetConfigPath()` and not from the argument. This is why the Thor's `_CONF_DIR` repair step has
   no Windows counterpart: on Linux the conf directory is baked in at compile time, and Windows
   has nothing to repair.

**Deliberately not in the zip:**

- **No PDBs.** A `RelWithDebInfo` MSVC build carries full debug information, and the upload would
  be mostly symbols.
- **No client data.** `dbc`, `maps`, `vmaps` and `mmaps` are ~1.2 GiB on their own, and GitHub
  refuses a release asset of 2 GiB or more (`server-aarch64.yml:20-24` for the reason, `:368-371`
  for the size check that fails the arm64 job early). The same ceiling applies to a second asset on
  the same release, so the Windows artifact carries no `data/` and the installer fetches the pinned
  `wowgaming/client-data` zip and verifies its sha256 like the Thor installer does.
- **No `data/` directory at all** in the artifact: the installer creates `<root>\data` and unpacks
  into it.

## 3. Why the release tag is not latest

The Thor installer resolves its artifact by the *latest* release:

```text
https://github.com/nick-monto/acore-thor-selfhost/releases/latest/download/acore-server-arm64.tar.zst
```

There is one `latest` per repository, and `/releases/latest/download/<asset>` redirects to it, so a
Windows release marked latest would make that URL 404 for the arm64 asset and silently break every
Thor install that expects to re-download. `server-build-win64` is therefore a rolling release like
`server-build`, carrying only the Windows asset and deliberately not marked latest; the Windows
installer's default `-ReleaseUrl` points at the tag explicitly:

```text
https://github.com/nick-monto/acore-thor-selfhost/releases/download/server-build-win64/acore-server-win64.zip
```

Tagged `server-v*` releases carry both artifacts, so a pinned version installs from a pinned
release either way. [V]

## 4. Install

Run it from an **elevated** PowerShell 5.1 session (the Windows PowerShell that ships with
Windows 10/11, not PowerShell 7):

```powershell
# get the script, then dry-run it
Invoke-WebRequest -Uri https://raw.githubusercontent.com/nick-monto/acore-thor-selfhost/main/scripts/windows-install.ps1 -OutFile windows-install.ps1
.\windows-install.ps1 -Check      # report what it would do, change nothing
.\windows-install.ps1             # do it
```

The default install is `C:\acore`. Add `-Root D:\acore` to move it.

### Flags

| Flag | Default | What it does |
|---|---|---|
| `-Check` | | Dry run: walks every phase the chosen mode would run and prints each command and file it would touch, without writing anything |
| `-Status` | | Reports every component (binaries, configs and their keys, client data, SQL sources, database, schemas, account, bot count) and exits non-zero if something needs repair |
| `-Repair` | | Runs the same checks and fixes what it can, then re-checks and reports what is left |
| `-Uninstall` | | Stops everything and removes the install root and every path in the manifest |
| `-KeepDb` | | With `-Uninstall`: dump each `acore_*` schema to `%USERPROFILE%\.win-wow\dumps\<stamp>-<schema>.sql` first |
| `-Force` | | Replace an installed build: re-download and re-unpack over it. Without it an existing `<root>` is left alone and only the missing pieces are added |
| `-Help` | | Print the usage text and exit |
| `-Bots N` | `50` | Number of random bots (`Playerbots.MinRandomBots` and `MaxRandomBots`) |
| `-HordeRatio P` | `50` | Percentage of random bots on the Horde side, 0-100; Alliance is the remainder, so the pair always sums to 100. Under `-Docker` it is the container's `HORDE_RATIO` and `write-confs.sh` renders it at every start; natively it is written into `playerbots.conf` and reconciled by `-Repair` |
| `-DbUser` | `acore` | MySQL user created for the server, over `127.0.0.1` |
| `-DbPass` | generated | That user's password; generated and recorded in `credentials.txt` when not given |
| `-Account` | `admin` | First GM account name |
| `-AccountPass` | `password` | That account's password |
| `-LanIp IP` | | Bind and pin the realm to the LAN address, and add inbound firewall rules for 3724 and 8085 |
| `-Root DIR` | `C:\acore` | Install root |
| `-ReleaseUrl URL` | the `server-build-win64` asset | Where to download the artifact zip |
| `-DataZip URL\|PATH\|none` | the pinned client-data URL | Source of the client data (`none` skips the fetch) |
| `-ClientData DIR\|none` | remembered, else one question | The 3.3.5a client folder: the World of Warcraft folder itself, the one holding `Data\common.MPQ` (its `Data` folder is accepted too, a quoted path is accepted, and the value is stored absolute). Writes the loot-bag `Item.dbc` records into the server's `data\dbc` and a patch archive into `<client>\Data`. `none` skips it; the answer is kept in `%USERPROFILE%\.win-wow\client-data`. Needs Python 3 on the host |
| `-BootTimeout SECS` | `1800` | How long the first boot may take to report the world is up: `World initialized` in the log on the native path, the entrypoint's readiness line under `-Docker` |
| `-MysqlUrl URL` | the pinned portable MySQL zip | Where to download MySQL 8.4.9 winx64 |

Install is the default action: passing no action flag installs, and `-Bots`, `-LanIp` and the rest
modify that install.

### What each phase does

1. **Preflight.** Confirms the session is elevated (except for `-Status`, which only reads and warns
   instead), that this is 64-bit Windows x64, that the drive has room (a warning below 6 GB), and
   that the root is not `Program Files` or a path with a space unless `-Root` was passed on purpose.
   It reports the PowerShell version it is running under.
   `-Check` does not stop here: it runs every phase below in dry-run, printing the `would:` line for
   each command and each file.
2. **Artifact.** Downloads `acore-server-win64.zip` to a temporary directory, reads the sibling
   `.sha256` and fails on a mismatch, then unpacks the flat layout into the root.
3. **MySQL.** Downloads the portable MySQL 8.4.9 winx64 zip, unpacks it to `<root>\mysql`,
   initialises a data directory at `<root>\db` with `mysqld --initialize-insecure`, starts `mysqld`
   on `127.0.0.1:3306` (the local root account has no password), creates `acore_auth`,
   `acore_world`, `acore_characters` and `acore_playerbots`, and creates the `-DbUser` account with
   `-DbPass` over `127.0.0.1`.
4. **Client data.** Fetches the pinned `wowgaming/client-data` v20.0 zip, verifies the sha256 from
   section 1, and unpacks `dbc`, `maps`, `vmaps` and `mmaps` into `<root>\data`.
5. **Configs.** Writes `configs\*.conf` and `configs\modules\*.conf` from the artifact's
   `.conf.dist` files with the database credentials, the realm address (or the `-LanIp` pin), the
   bot count and the same feature keys the Thor installer writes.
6. **First boot.** Starts `mysqld` → `authserver.exe` → `worldserver.exe` with the working
   directory at the root, polls the logs until the readiness line or `-BootTimeout`, creates the
   `-Account` GM account at `gmlevel 3`, and stops the servers. The first boot runs the worldserver
   against a copy of its conf with `Console.Enable = 1`, because `Main.cpp:402-408` starts the CLI
   thread - the only reader of stdin - only when that key is 1; the live conf keeps the `0` the
   realm wants, so a first boot that dies half way leaves nothing behind that would kill the next
   start on stdin EOF.
7. **Loot-bag items.** Runs `scripts/item-dbc/patch_item_dbc.py` (fetched at `-SelfRef`) with the
   client folder: it adds the 64 bag records (91000-91063) to the server's `data\dbc\Item.dbc` and
   writes a patch archive under the highest free name into `<client>\Data`, then verifies that the
   client's patch load order picks that archive up. Skipped with a warning when Python 3 is missing
   or no client folder was chosen; the server half still runs.
8. **Helpers and state.** Writes the start/stop scripts described below, `credentials.txt`, the
   state and the manifest, and with `-LanIp` adds the inbound firewall rules for 3724 and 8085.

### Root layout

```text
C:\acore\
├── worldserver.exe  authserver.exe  *.dll     # the artifact, flat
├── configs\                                   # *.conf and modules\*.conf - CWD-relative
├── src\                                       # SQL sources: base, custom, module trees
├── data\                                      # dbc, maps, vmaps, mmaps
├── mysql\                                     # portable MySQL 8.4.9 binaries
├── db\                                        # the MySQL data directory
├── logs\                                      # authserver.out, worldserver.out, mysql.err
├── .win-state\state.env                       # flags and secrets, so -Status/-Repair/-Uninstall need no re-pass
├── credentials.txt                            # DB password, GM account, ports
└── start.ps1  stop.ps1  set-bots.ps1  start-db.cmd  stop-db.cmd
```

`%USERPROFILE%\start-wow-server.cmd` and `%USERPROFILE%\stop-wow-server.cmd` sit beside the root,
not in it: they are the shortcuts a pinned taskbar or desktop icon points at, and they call into
`<root>\start.ps1` / `<root>\stop.ps1`.

Host state and the manifest live in `%USERPROFILE%\.win-wow\`: the manifest lists every path the
install created outside `<root>` (the two `%USERPROFILE%` wrappers, and any firewall rule it added),
and removal walks it, refusing any path that is not clearly owned by the install.

The helpers are the Windows equivalents of the Thor's `wow-server.sh`:

| File | What it does |
|---|---|
| `start.ps1` | Starts database, authserver and worldserver in order, waits for readiness, keeps the worldserver's console window open |
| `stop.ps1` | Stops in reverse order |
| `set-bots.ps1 N [-HordeRatio P]` | Sets `Playerbots.MinRandomBots` and `Playerbots.MaxRandomBots` together (the Thor's `start-wow-server.sh --bots N`); `-HordeRatio` also sets the faction split, Alliance the remainder (the Thor's `start-wow-server.sh --horde-ratio P`) |
| `start-db.cmd` / `stop-db.cmd` | Start/stop only MySQL |
| `%USERPROFILE%\start-wow-server.cmd` | One-click start; accepts `-Bots N` |
| `%USERPROFILE%\stop-wow-server.cmd` | One-click stop; accepts `-Bots N` |

### Manage it afterwards

```powershell
.\windows-install.ps1 -Status                    # check, exit non-zero if repair is needed
.\windows-install.ps1 -Repair                    # check and fix
.\windows-install.ps1 -Uninstall                 # remove
.\windows-install.ps1 -Uninstall -KeepDb         # dump acore_* to a .sql first
```

`-Status` reads back what `-Repair` can fix: both binaries and the DLLs they need, every conf file
and the keys it expects (including the activating keys of each module and the bot count), the
client-data trees, the four SQL source trees, whether `mysqld` is running and whether the three
schemas it creates hold tables, and whether the GM account exists. `acore_playerbots` is created by
the module's own updater on first boot, so `-Status` reports it only through the bot count. `-Repair` re-downloads a missing artifact,
rewrites drifted conf keys, restores the bot count, starts a stopped database, re-populates an empty
database, re-creates a missing account, and then re-checks. `-Uninstall` stops both servers and
MySQL, then deletes the root and every manifest path; `-KeepDb` dumps the four schemas to
one dump per schema under `%USERPROFILE%\.win-wow\dumps\<stamp>-<schema>.sql` first.

## The client half: the bags need `Item.dbc`

The loot bags are custom items, and a 3.3.5a client draws an inventory item from its **own**
`Item.dbc`: without a record there the bag shows the red question mark once it is in the player's
bags, even though the loot window draws the icon from the packet. Two halves close that gap, and
this installer writes both:

| Half | Where | Written by |
|---|---|---|
| server | `<root>\data\dbc\Item.dbc` | phase 7, so `sItemStore` agrees with `item_template` and the DBC attribute checks run |
| client | `<client>\Data\patch-*.MPQ` | phase 7, under the highest free patch name so it loads after everything already there |

The client is part of neither install - it lives wherever the player put it - so this runs on the
host in **both** modes. Under `-Docker` the server half is copied out of the data volume with
`docker compose cp`, patched, copied back, and the `acore` service is restarted so the running world
reads it. `-Status` reports the remembered folder, whether a `patch-*.MPQ` is in it, and whether the
server's `Item.dbc` is present; `-Repair` re-applies the patch.

Python 3 is the one extra requirement, and a fresh Windows often resolves `python.exe` to the
Microsoft Store alias instead of an interpreter - the installer says so by name and points at
`winget install --id Python.Python.3.12 -e`.

## The container path: easy to serve, easy to delete

The third host shape is no host at all: the same realm as two containers, built from `docker/` and
driven by `scripts/windows-install.ps1 -Docker`. Nothing is installed into the host - no MySQL
service, no directory tree, no registry entries - and one command deletes the database, the client
data and the logs together.

| File | What it is |
|---|---|
| `docker/Dockerfile` | `FROM ubuntu:24.04`, headless: no display, no systemd, no compiler. Downloads the server artifact, verifies it, unpacks it at `/opt/acore` |
| `docker/docker-compose.yml` | The two services, their environment, the four named volumes and the two published ports |
| `docker/entrypoint.sh` | Everything between "container started" and "realm answering": database wait, schemas, client data, confs, first boot, then auth + world |
| `.dockerignore` (repository root) | Keeps the build context to `docker/entrypoint.sh`, `scripts/write-confs.sh` and the `docker/` directory |

### The two containers

- **`db`** - the stock `mysql:8.4` image, with a healthcheck so `acore` starts only once the server
  answers, and **no published port**: the realm reaches it as `db:3306` on the compose network, and
  publishing 3306 would expose the database to the host and its LAN for nothing. Its data directory
  is the `db` volume. Only the root password is handed to the image; the realm's own user is created
  and kept in step by the entrypoint, so changing `DB_PASS` later works.
- **`acore`** - the image built from `docker/Dockerfile`, publishing **3724** (authserver) and
  **8085** (worldserver). It runs as many processes as you would expect from a foreground container:
  the entrypoint waits for `db`, creates `acore_auth`, `acore_world`, `acore_characters` and
  `acore_playerbots` plus the realm's user, installs the client data, writes the confs, and on the
  first start only boots worldserver to populate the databases and create the GM account. After the
  client data is unpacked and before anything reads it, the entrypoint runs
  `scripts/item-dbc/patch_item_dbc.py` against `data/dbc/Item.dbc`: the same start imports the
  loot-bag items into `item_template`, and a `Item.dbc` without them is what makes the bags
  unusable. The tool replaces its own 64 records rather than appending, so this runs on every start
  as a no-op rewrite, and the installer no longer writes that file or restarts the realm. It then runs
  both servers under one supervisor: a crash of either restarts it after 5 s, SIGTERM is forwarded
  to both, so `docker stop` is a clean shutdown rather than a half-written database. The authserver
  is started once the world reports "World initialized", not alongside it, because the worldserver
  holds its realm row at `VERSION_MISMATCH` for the whole of its startup and an authserver that
  reads that row inside the window ORs `OFFLINE` onto it, sees `flag = 3`, and exits with
  "No valid realms specified" - leaving the login port dead while the world runs. The healthcheck
  requires both `.state/world-ready` and `.state/auth-ready` for the same reason. The conf copy the
  entrypoint stages for that first boot has `Console.Enable = 1`, because `Main.cpp:402-408` starts the CLI
  thread - the only stdin reader - only when the key is 1; the live conf keeps `0`.

Every value in the compose file is a `${VAR:-default}`, so it runs with no `.env` at all, and the
defaults are the Thor installer's: 50 bots, `acore` / `admin` accounts, the same near-player window.
The project name is pinned to `acore-realm`, so the volumes are named the same wherever compose is
run from - which matters because the Windows installer stages these files elsewhere before running
compose there.

### Why the image downloads instead of compiling

A rebuild is a download. The Dockerfile has no compiler, no headers and no git, because nothing in
the image builds anything: `ARG TARGETARCH` selects the published artifact for the platform being
built - `acore-server-x86_64.tar.zst` for `amd64`, `acore-server-arm64.tar.zst` for `arm64` - and
`ARTIFACT_URL` / `ARTIFACT_SHA256` override both. When neither the argument nor the `.sha256`
published beside the asset supplies a hash the build stops rather than unpacking bytes nobody
verified, and it checks that `bin/`, `etc/`, `lib/` and `src/` are all there before declaring
success. The payload is therefore the same tarball the Thor and Windows hosts unpack: the container
runs exactly the binaries they run, and it cannot drift from the pins.

The x86_64 tarball comes from the new `.github/workflows/server-x86_64.yml` and lives under the
release tag **`server-build-x86_64`**, deliberately not marked latest so that the Thor's
`releases/latest/download/acore-server-arm64.tar.zst` keeps resolving - the same reason
`server-build-win64` is not latest (section 3).

The build context is the **repository root** (`context: ..`), because the Dockerfile copies
`docker/entrypoint.sh` and `scripts/write-confs.sh` out of the checkout; `.dockerignore` keeps that
context from becoming the whole tree.

### The four volumes, and what each holds

| Volume | Mount | Holds |
|---|---|---|
| `db` | `/var/lib/mysql` | The MySQL data directory: all four `acore_*` schemas |
| `data` | `/opt/acore/data` | `dbc`, `maps`, `vmaps`, `mmaps` - the client data |
| `logs` | `/opt/acore/logs` | `worldserver.out` and `authserver.out`, including the first boot's SQL import |
| `state` | `/opt/acore/.state` | The first-boot marker, and anything else that must outlive the container but is not the realm |

The client data is a **volume, not an image layer**: it is ~1.2 GiB, it is fetched from the pinned
`wowgaming/client-data` v20.0 archive and sha256-verified on the first start, and it is reused on
every later start. Baked into the image it would be a layer that every rebuild re-downloads and that
every data change invalidates; in a volume it survives a rebuild of the image and the entrypoint only
installs it once.

Because the client data lives in `data` and the schemas in `db`, a rebuild (`up -d --build`) keeps a
populated realm untouched: only the binaries under `/opt/acore` are replaced.

`/opt/acore/etc` is *not* a volume. It is the image's own layer, laid down from the artifact's
`etc/` at build time, and the confs in it are re-rendered from the artifact's `.dist` files at every
start. So a hand edit to `/opt/acore/etc/modules/*.conf` survives a `docker compose restart` and is
gone at the next recreate; the switch for anything the entrypoint knows about is the compose
environment (see `SKATEBOARD_ENABLE`, `BOTS`, `HORDE_RATIO`).

### A new release reaches an image only through the artifact pin

The artifact step is one `RUN` that downloads `releases/download/<tag>/acore-server-x86_64.tar.zst`.
That tag is a moving name, and the sha256 is fetched *inside* the layer, so nothing in the layer's
cache key changes when a new build is published: `up -d --build` and `-Repair` replay the tarball
they downloaded the first time. The installer therefore reads the URL out of the staged Dockerfile,
fetches the `.sha256` published beside it, and passes both as build args (`ARTIFACT_URL`,
`ARTIFACT_SHA256`). A new release changes the sha, which changes the key, which re-runs the layer -
and the bytes are verified against the hash the release published, not one fetched a month ago.

If the sha256 cannot be read (offline, or a release that published none), the installer says so and
leaves the build to its own fetch: correct bytes, but no cache invalidation. The escape hatch is then

```powershell
docker compose -f C:\acore\docker-src\docker\docker-compose.yml build --no-cache acore
docker compose -f C:\acore\docker-src\docker\docker-compose.yml up -d acore
```

### The confs come from one writer

The entrypoint sources `scripts/write-confs.sh` and calls `wc_write_all`, which is the same library
`scripts/thor-install.sh` delegates to - its `write_configs()` maps the installer's variables to the
`WC_*` ones and calls the same function. All three paths therefore write the same keys with the same
values, with one deliberate exception: the faction split. The container renders it from
`HORDE_RATIO` on every start (default 50, `-HordeRatio` sets it) and the device leaves it to
`set-ratio.sh` (`--horde-ratio`), so a device render never overwrites a split set there; both keep
Alliance as the remainder so the pair sums to 100. The rest is identical: bots and the near-player
window, the loot keys that make mercenaries pass,
`Playerbots.AddClassCommand = 0`, instant flight paths, the hardcore death policy, and the survivor
and interact dials. That is the point of the refactor: a key is added or changed in one file, and no
host shape can silently disagree with another. The library produces byte-identical confs to the
pre-refactor installer [V].

### The Windows mode: `-Docker`

```powershell
.\windows-install.ps1 -Docker -Check     # print every command, touch nothing
.\windows-install.ps1 -Docker            # fetch the compose files, build and start
```

| Flag | Default | What it does |
|---|---|---|
| `-Docker` | | Selects the container mode instead of a native install |
| `-DockerDir DIR` | `<root>\docker-src` | Staging directory the container files are fetched into |
| `-SelfRef REF` | `main` | Branch or tag the files are fetched from (`raw.githubusercontent.com/nick-monto/acore-thor-selfhost/<ref>/...`) |
| `-Check` | | Prints every command it would run and touches nothing |
| `-Status` | | Reads `docker compose ps` and reports the services |
| `-Repair` | | Re-fetches the container files and rebuilds |
| `-Uninstall` | | `docker compose down -v --remove-orphans`, then removes the staging directory |
| `-KeepDb` | | With `-Uninstall`: dumps each schema with `docker compose exec -T db mysqldump` into `%USERPROFILE%\.win-wow\dumps\` first |

The mode needs a reachable Docker daemon (Docker Desktop with the Linux engine). It refuses to mix
with a native install: the chosen mode is recorded in `<root>\.win-state\mode`, and a `-Docker` run
against a native root - or the reverse - stops and says so rather than producing two realms that
fight over port 3724.

### When the client is disconnected at the login screen

`docker compose ps` can say `running (healthy)` while nothing answers the login: the two processes
are independent, and the authserver is the only one that speaks the login protocol. Since the
supervisor restarts it, a dead authserver now makes the container `unhealthy` instead (the
`auth-ready` marker is removed), and the entrypoint logs `authserver exited with status N`.

If that status is `1`, read its log:

```
docker compose logs acore | grep -i 'authserver'
docker compose exec -T acore tail -20 /opt/acore/logs/Auth.log
```

`No valid realms specified` means no row in `acore_auth.realmlist` passed the query, which is
`WHERE flag <> 3` - the state a worldserver leaves behind when it is killed between marking its
realm `VERSION_MISMATCH` at start and clearing it once the world is up. The entrypoint clears that
bit before every authserver start, so this should not survive a restart; to confirm what is stored:

```
docker compose exec -T db mysql -uroot -p"$DB_ROOT_PASS" -e \
  "SELECT id, name, address, port, flag, gamebuild FROM acore_auth.realmlist;"
```

A healthy row is `flag = 0` (or `2` while the world is starting), `address = 127.0.0.1`,
`port = 8085`, `gamebuild = 12340`. A client on another machine needs `address` set to this host's
LAN address instead of `127.0.0.1` - that is the realm's own pointer, and Docker's port mapping
cannot substitute for it.

### Deleting the realm

Deleting the realm is the point of this shape:

```powershell
docker compose -f <root>\docker-src\docker\docker-compose.yml down -v --remove-orphans
```

`-v` takes the volumes with it, so the database, the client data and the logs all go; `-Uninstall`
runs that and then removes the staging directory. What is left is the host's Docker resources and
nothing else - no service, no data directory, no droppings to hunt for.

### On a Linux host

The same compose file runs on any Linux host with a Docker daemon, no PowerShell involved:

```sh
git clone https://github.com/nick-monto/acore-thor-selfhost
cd acore-thor-selfhost
docker compose -f docker/docker-compose.yml up -d --build
```

Same image, same volumes, same published ports; `docker compose -f docker/docker-compose.yml down -v`
is the same delete. The PowerShell wrapper only exists to fetch the container files and to keep the mode
record in step with the native installer.

### What is verified about this path

Verified here: the conf library produces byte-identical confs to the pre-refactor installer, the
entrypoint passes `bash -n`, the compose file parses, and the installer's `-Check` and `-Status` run
on a real Windows host without touching the root.

Not verified: **no image has been built, no `docker compose up` has run, and the x86_64 artifact's
workflow has never executed** - its first run is the gate that produces
`acore-server-x86_64.tar.zst`, and until then the `amd64` image has nothing to download. A full
Windows `-Docker` install has not been run either. [I] Treat the first `up -d --build` as the test,
and `docker compose ps` followed by the `logs` volume as its verdict.

## 5. Differences from the Thor install

| | Thor | Windows |
|---|---|---|
| Process isolation | Ubuntu arm64 rootfs under proot-distro inside Termux | none - the server runs as a native Windows process |
| Shell | bash in Termux and a second bash inside the rootfs | one PowerShell 5.1 script, elevated |
| Compile path | `--from-source` builds the core and modules on the device | **no `-FromSource` equivalent**: the workflow is the only builder, so the host never needs MSVC, Boost or a 40-minute compile, and there is no second compile path to keep in step with the pins |
| Config directory | `<prefix>/etc` and `<prefix>/etc/modules`, baked in as `_CONF_DIR` | `<prefix>/configs` and `<prefix>/configs/modules`, CWD-relative |
| `_CONF_DIR` repair | the installer symlinks or repairs the compile-time path when `--root` moves | nothing to repair: the path carries no prefix |
| Database | `mysql-server 8.0.46 (Ubuntu noble arm64)` from the rootfs | portable MySQL 8.4.9 unpacked into `<root>\mysql`, data in `<root>\db` |
| Network exposure | loopback only, nothing opened | loopback by default; with `-LanIp`, a realm pin and inbound firewall rules for 3724 and 8085 |
| State and manifest | `~/.thor-wow/state.env`, `~/.thor-wow/manifest.txt`, `<root>/.thor-state/manifest.txt` | `%USERPROFILE%\.win-wow\` plus `<root>\.win-state\state.env` |
| Removal | manifest walk with a rootfs decision (`--purge-rootfs`) | manifest walk; there is no rootfs, so no second switch |
| Elevation | none | required: the firewall rules and, on the default root, the unwritable `C:\` |
| Second shape | none | `-Docker`: two containers, no host install, `docker compose down -v` to delete (see the section above) |

## 6. Verified and not verified

**Verified, today, here:**

- The core facts this document rests on, with their lines: `ConfigMgr::GetConfigPath()` returns
  `"configs/"` on Windows and `_CONF_DIR` otherwise (`Config.cpp:704-712`, the Windows `return` at
  `:709`), module configs load from `GetConfigPath() + "modules/"` (`:766`), and the WIN32 branches
  of `ConfigInstall.cmake` install the app and module dists to `${CMAKE_INSTALL_PREFIX}/configs` and
  `${CMAKE_INSTALL_PREFIX}/configs/modules` (`:41-42`, `:69-70`, `:102-103`) rather than to
  `CONF_DIR`, while the apps install into the prefix itself
  (`src/server/apps/CMakeLists.txt:241-243`). [V]
- The 2 GiB release-asset ceiling and its size gate in the arm64 workflow
  (`server-aarch64.yml:20-24`, `:368-371`), which is why no client data rides either artifact. [V]
- The pins: core `8a4e76eac8c4ccebf547113afe7b51d6c3e91641`, the six module commits and the client
  data hash, all as `pins.toml` records them. [V]
- `scripts/windows-install.ps1` is written for Windows PowerShell 5.1, not 7: it is syntax-checked
  with the PowerShell parser and uses no PowerShell 7-only construct (no ternary, no `??`, no
  `-Parallel`). [V]
- `-Check` and `-Status` reach no write before they return: they read the state file if it exists
  and otherwise report an install that is not there, without creating the root, the database or the
  state file. [V]
- The container path's static checks: `bash -n docker/entrypoint.sh`, the compose file parsing, and
  the conf library writing byte-identical confs to the pre-refactor installer. [V]

**Not verified:**

- **Any image build and any container start.** `docker/Dockerfile` has never been built and
  `docker compose up` has never run; the entrypoint's six steps are its code paths, not runs. [I]
- **The x86_64 artifact.** `server-x86_64.yml` has never run, so `acore-server-x86_64.tar.zst` does
  not exist yet and the `amd64` image has nothing to download until it does. [I]
- **The MSVC build.** `server-win64.yml` has never run. Its first `workflow_dispatch` is the gate:
  either it produces `acore-server-win64.zip` or it names the file that does not compile under
  Visual Studio 2022. [I]
- **The artifact's DLL bundle.** Which non-OS DLLs MSVC and the vcpkg Boost/MySQL packages leave
  the binaries dependent on is decided by that first run, not by this document. [I]
- **A full install on a real Windows host.** No step from the download through the first boot and a
  client login has been executed here; the phases above describe the script's code paths, not runs.
  The threat is exactly the Thor installer's first day: a path that assumes Unix, a quoting mistake
  in a conf value, or a MySQL option that behaves differently on Windows. [I]

Treat the first Windows install as the test, and `-Status` afterwards as its verdict.
