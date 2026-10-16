# The server half: AzerothCore + playerbots on the Thor

Companion to [`LAUNCHER-HOSTING-DESIGN.md`](LAUNCHER-HOSTING-DESIGN.md). This document covers the server stack itself: what the Dad's MMO Lab flow builds and runs, how each piece maps to an on-device deployment, and what must be measured on hardware rather than assumed.

Confidence: **[V]** verified here from source, artifact or API, **[R]** reported, **[I]** inference.

---

## 1. Stack and pins

| Piece | Value | Evidence |
|---|---|---|
| Core | `github.com/mod-playerbots/azerothcore-wotlk`, branch **`Playerbot`** — the guide clones branch HEAD with **no commit pin**; inspection checkout was `8a4e76e` | `install-wow-wotlk.sh:1133-1136` [R]; local clone [V] |
| Bots | `github.com/mod-playerbots/mod-playerbots`, branch `master`, `--depth 1`, cloned into `<core>/modules/mod-playerbots` | `install-wow-wotlk.sh:1143-1163` [R]; local `d750b4b` [V] |
| Server data | `wowgaming/client-data` release **`v20.0`**, `Data.zip` = 1,196,168,257 B, sha256 `a3d4df635ae6c2c8f08052c32a79e0f806955150ad36b014a823dd08a32a4610`, unpacked into the data dir with a `data-version` stamp | `apps/installer/includes/functions.sh:156-185` [V] |
| Database (guide) | service `ac-database` running `mysql:8.4`, plus a separate `ac-db-import` service | `docker-compose.yml:9-11,27-29` [V] |
| Ports | 3306 DB, 3724 auth, 8085 world, 7878 SOAP (SOAP disabled by default) | `docker-compose.yml:8-160`, `worldserver.conf.dist:100` [V] |
| Build flags (guide) | `-G Ninja -DAPPS_BUILD=all -DTOOLS_BUILD=all -DSCRIPTS=static -DMODULES=static -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBoost_USE_STATIC_LIBS=ON`, clang + ccache | `apps/docker/Dockerfile`, guide §"What Happens During Install" [R] |

The core contains no Android-specific code and no `AC_PLATFORM_ANDROID` member (`AC_PLATFORM` only distinguishes Windows at `Config.cpp:708`, `Define.h:36-41`); the only `__ANDROID__` guards are upstream gSOAP portability fallbacks in `deps/gsoap/stdsoap2.{cpp,h}` [V]. There is also **no Linux aarch64 job in upstream CI** — the ubuntu matrices are x64 and `windows_build.yml` is x64 — although `macos_build.yml:28` runs on `macos-14` (Apple silicon), so arm64 compilation is exercised for macOS only, and no aarch64 Linux binary is published anywhere [V]. The aarch64 Linux binaries are ours to produce.

## 2. Where the server can live

| | Shape | Database | Assessment |
|---|---|---|---|
| **A1** | **Ubuntu arm64 rootfs under proot** (Termux + `proot-distro`, or a bundled rootfs in an app) with the distro's `mysql-server-8.0` (8.0.46 has arm64) and AzerothCore built for aarch64 | real MySQL 8 from the distro | **Closest to the guide**; avoids the MariaDB/MySQL-8 friction below; cost is proot's syscall overhead on a server [I] |
| **A2** | Termux-native: bionic `mariadb` package + AzerothCore built with the Android/NDK toolchain | MariaDB 11.x | No proot, native speed. The core's configure step requires a MySQL **client** ≥ 8.0 (`FindMySQL.cmake:54,63-64`), but that comparison is **numeric**, so MariaDB `10.x`/`11.x` sorts *above* `8.0` and passes — a patch is only needed if `mysql_config --version` prints a string that sorts lower (e.g. a `5.5.5-…` compatibility prefix). Unverified either way until a Termux build is attempted [I] |
| **B** | Inside the Thor WoW launcher app | any of the above | Needs the new platform subsystem described in the launcher design §4: no foreground service, one shared rootfs, UID-wide process sweeps [V] |
| **C** | Windows x64 server inside the launcher's existing Wine container | emulated MySQL | **No official prebuilt binaries**: upstream `azerothcore/azerothcore-wotlk` releases carry 0 assets and `mod-playerbots/*` has no releases at all; its Windows CI uploads no artifacts [V]. Community x64 zips exist (e.g. `onur/mod-playerbots-portable` v0.3.4, 352.8 MB, bundling a portable MySQL 8.0 and a `startup.exe` supervisor; forum repacks are login-gated) [R] — all x64, so the whole server *plus* its database would run under x86-64 emulation on a 16 GB handheld |
| **D** | Build the **SQLite-backed** AzerothCore fork instead of the MySQL one (`LonelyIceProject/azerothcore-wotlk`, described as "AzerothCore (Playerbot branch) with a SQLite backend, shared-library build and runtime plugins", GPL-2.0, publishes `linux-x64` and `windows-x64` bundles) | SQLite file | Removes `mysqld` entirely — the single hardest piece to host on Android — turning the server into one process plus a database file. **Unvetted**: single-maintainer fork, no independent reports, and its playerbots port has to be checked before relying on it [R] |

Recommendation: **A1** (or A2 if proot proves troublesome) for the first working stack, with **D** tracked as a possible much simpler long-term shape.

## 3. Server data: no client needed

The guide never extracts maps: the installer downloads prebuilt `dbc/`, `maps/`, `vmaps/` and `mmaps/` from `wowgaming/client-data` v20.0 (1.19 GB) and unpacks them into the data dir [V]. Consequences:

- The **server half needs no WoW client at all**; the client is only needed to play.
- The download is *stock* 3.3.5a data. That is correct for a stock core; a core with custom DBC rows would need its own tables (out of scope here).
- `Data.zip` is a plain archive: an app or script can download, verify the sha256 above, and unpack it without Docker.

## 4. Database

- Schemas: `acore_auth`, `acore_world`, `acore_characters` (+ `acore_playerbots` for the module) [V].
- Update policy: `Updates.EnableDatabases = 7` (world) and `1` (auth) [V]; the Docker flow turns in-server updates off and lets a dedicated `db-import` service populate first [R].
- Bootstrap order: empty schemas → import/populate base data → start authserver and worldserver.
- On arm64 use the distro package (`mysql-server-8.0` 8.0.46 arm64 exists); MySQL's own APT repo ships only amd64/i386 [V].
- Keep the DB off the SD card if the Thor's internal storage is available: InnoDB on a slow card plus the phantom-killer risk is a bad combination [I].

## 5. First run, realm pin, accounts

| Step | Value |
|---|---|
| Realm pin for on-device play | `UPDATE acore_auth.realmlist SET address='127.0.0.1', localAddress='127.0.0.1' WHERE id=1;` [R] |
| Admin account | `account create <NAME> <PASS>` then `account set gmlevel <NAME> 3 -1`, typed into the worldserver console (`AC>`) [R] |
| Ready signal | the worldserver logs `ready...`; the guide allows up to 1800 s on the first boot (base data import) and its desktop launcher waits 900 s while polling every 5 s [R] |
| Client pointer | the stock 3.3.5a client honours `Data/<locale>/realmlist.wtf` (`set realmlist 127.0.0.1`), so no glue patching is needed for a stock core [R] |

## 6. Bots

Defaults shipped by the module (local checkout `d750b4b`) [V]:

| Key | Value |
|---|---|
| `Playerbots.Enabled` | 1 |
| `Playerbots.RandomBotAutologin` | 1 |
| `Playerbots.MinRandomBots` / `MaxRandomBots` | 500 / 500 |
| `Playerbots.BotActiveAlone` | 10 |

**The guide's bot overrides are inert today.** AzerothCore maps environment variables to conf keys as `AC_` + the key uppercased with `_` separators (comment at `Config.cpp:371-374`, transform at `:375-432`, `GetEnvVarName` at `:435-438`, lookup at `:440-448`), the override only rewrites keys that were actually loaded, and the module renamed its keys from `AiPlayerbot.*` to `Playerbots.*` in commit `ed54b459` (2026-10-06) — whose own commit message says the old `AC_AI_PLAYERBOT_*` overrides "aren't read any more" and an unmigrated server falls "back to 500 random bots" [V]. So `AC_AI_PLAYERBOT_MIN_RANDOM_BOTS` (what the guide writes) matches no loaded key and the shipped 500/500 stands; the working names are `AC_PLAYERBOTS_MIN_RANDOM_BOTS`, `AC_PLAYERBOTS_MAX_RANDOM_BOTS`, `AC_PLAYERBOTS_RANDOM_BOT_AUTOLOGIN` [V]. The local `mod-playerbots` checkout contains no `AiPlayerbot` config key at all — the only remnant is a stale comment in `data/sql/playerbots/updates/2026_04_28_00_playerbots_bis_gear.sql:4` and the legacy `ai_playerbot_texts*` tables [V].

Also inconsistent in the guide: it targets 2000 bots while `PlayerLimit = 1000` (`worldserver.conf.dist:911`) bounds concurrent sessions; its own manager documents `PlayerLimit ≥ MaxRandomBots + 400` for city bots [R].

Thor sizing: start near 50 online bots restricted to the home maps, measure the worldserver tick and RSS, then scale. 500 is a desktop figure and the device also has to render the client [I].

## 7. Supervision on Android

What the guide does: start the containers, poll logs until `ready...`, wait for the game process to exit, then `docker compose down`, with a 3 h keep-alive fallback and a one-button launcher [R].

The portable shape is the pattern, not the code: start (DB → auth → world), poll for the readiness line, keep the process group alive, stop in reverse order, restart on crash. On Android that needs a foreground service with a notification (in an app) or `termux-wake-lock` plus a supervisor loop (Termux), because a cached background process is frozen or killed by the platform [V/I].

## 8. What transfers from the guide

| Portable (reimplement anywhere) | Docker- or host-specific (must be replaced) |
|---|---|
| Repo cloning + module placement, conf file writes (`env/dist/etc` → `<confdir>`), ALE/module/SQL management logic | Docker engine, `docker compose`/`buildx`, registry access, the `docker` group privilege change, `systemctl` |
| Client-data download + `data-version` stamping; db-import step; realm pin SQL; console account creation | `docker cp`/`docker exec`/`docker compose logs` as the control plane |
| Readiness polling, keep-alive and auto-shutdown behaviour, timeouts | Host package-manager installs and SELinux volume labels |
| Playerbots/AH-bot conf keys and the module's own defaults | The VM/container resource isolation (the Thor has one shared 16 GB budget) |

## 9. Gates to verify on hardware

- **G1** aarch64 build of core + module succeeds (no upstream arm64 CI exists): record the exact toolchain, flags and build time.
- **G2** the database daemon (MySQL 8 in a rootfs, or Termux MariaDB with the configure check patched) initialises, survives backgrounding, and recovers after a kill.
- **G3** authserver + worldserver reach `ready...` on device; a real 3.3.5a client logs in on `127.0.0.1` and the realm pin is visible.
- **G4** bot scaling: tick time and RSS at 50 / 150 / 300 online bots; confirm `PlayerLimit` headroom.
- **G5** crash/restart behaviour: kill the worldserver and confirm the supervisor brings it back without corrupting the DB.
