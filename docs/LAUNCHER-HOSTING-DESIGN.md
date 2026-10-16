# Can the AYN Thor launcher support the self-hosted AzerothCore stack?

Target: `wyattabuntjer/AYN-Thor-WoW-Launcher` v2.6.0 (`app.aynthorwow`), a GameNative/Winlator-derived Android app that runs Windows WoW clients under a bundled Proton. Question: can the process set from the Dad's MMO Lab guide — stock AzerothCore + `mod-playerbots`, hosted on the device — be adapted into that app?

Local checkouts used for evidence: `/tmp/launcher-inspect` (launcher v2.6.0), `/tmp/wow-mobile-inspect` (`marcocastignoli/wow-mobile`, the Winlator-derived fork that already runs the 3.3.5a client), plus the shipped runtime components (verified by sha256 against `tools/components.sha256`).

Confidence: **[V]** verified here from source or artifact, **[R]** reported, **[I]** inference.

---

## 1. Verdict

**Split the question; the two halves have different answers.**

| Half | Answer | Why |
|---|---|---|
| **Client:** play a self-hosted 3.3.5a realm in the launcher | **Yes — bounded change list** (§3) | The app's launch path already accepts a non-ARM64 exe (`XServerScreen.kt:3888-3908`), the shipped Proton contains the 32-bit Windows DLL tree (`lib/wine/i386-windows/`, 1013 files) *including a full D3D8/9/11 + wined3d*, and `wowbox64.dll` (the 32-bit WoW64 emulation DLL) is bundled in the APK. What is missing is a *flavor*, a *container profile* and any **realm/port plumbing at all** — `grep -rIn "realmlist\|3724\|8085"` over `app/src/main` returns **nothing** [V] |
| **Server:** host AzerothCore inside the launcher app | **Not by adapting the guide — new subsystem** (§4) | The app has **no** `<service>`, no notification, no wakelock [V]; one `XEnvironment` and one global `imagefs` for the whole app [V]; guest processes are killed by a **UID-wide** sweep on session exit [V]; and Gradle never compiles `app/src/main/cpp`, so PRoot cannot simply be switched on [V] |
| **Server, outside the app** (Termux / second app) paired with the launcher's client half | **Yes — this is the recommended first delivery** (§5) | Termux supplies a Linux userland, a bionic `mariadb`, and a real proot; the launcher only has to play the client. No launcher platform work beyond §3 |

The guide's *server* content (module set, bot settings, database bootstrap, ports, readiness/watchdog behaviour) transfers to either server path. Its Docker orchestration and host package manager do not (§6).

---

## 2. What the shipped runtime actually supports (artifact evidence)

Both component archives were downloaded and hash-checked against `tools/components.sha256` (`tools/fetch_components.sh:12-17`): `proton-11.0-90624-arm64ec.wcp` = `137807f4…`, `dxvk-2.4.1-wow-aarch64-test.wcp` = `f660b5c6…` [V].

| Question | Finding |
|---|---|
| Can this Wine run 32-bit Windows programs? | **Yes, structurally**: the Proton pattern ships `lib/wine/i386-windows/` (1013 files) next to `aarch64-windows/` (1026) and `aarch64-unix/`, including `libwine/aarch64-windows/{wow64.dll,wow64win.dll,libwow64.a}` [V]. 32-bit PE loading is therefore a Wine WoW64 feature, with the x86 instruction execution delegated to an `HODLL` |
| Which HODLL is available? | `emulator="Box64"` selects the bundled `assets/wowbox64/wowbox64-0.4.2.tzst` (1,030,182 B); the FEXCore route (`libwow64fex.dll`) is *code-referenced but unobtainable in this fork* — its asset `fexcore-2609-0.tzst` is absent and the remote profile feed has no caller [V] |
| D3D9 for a 3.3.5a client? | **Wine's own wined3d is present in 32-bit form**: `lib/wine/i386-windows/{d3d9,d3d8,d3d11,dxgi,ddraw,wined3d,d3dcompiler_43,opengl32}.dll` [V]. The bundled DXVK component is **aarch64-only** (`system32/d3d11.dll`, `system32/dxgi.dll`, no `syswow64/`) and has no `d3d9` at all [V], so the 32-bit client must use wined3d (`wincomponents direct3d=1`), not DXVK |
| Is a 32-bit flow wired up? | Partly: `executablePath.contains("ARM64")` → direct launch, otherwise `winhandler.exe "<path>"` (`XServerScreen.kt:3888-3908`); `wow64Mode=true` by default (`Container.java:102`); `HODLL` + `i386-windows`→`syswow64` provisioning in `BionicProgramLauncherComponent.java:438-448,484-508` [V] |
| Is any of it proven? | **No.** No in-world 3.3.5a run on this stack is documented; `wow-mobile` proves the *sibling* stack (box64 + glibc Wine + `targetSdk 28`), not this one (Proton ARM64EC/bionic). Treat "32-bit client boots" as the first gate, not a given [I] |

---

## 3. Client half — the change list

Everything the guide does on the client side (`realmlist.wtf`, Proton launch, ports 3724/8085) has an equivalent here; the app simply has no notion of a realm yet.

1. **Add a flavor** (e.g. `LOCAL_AC`) in `WowFlavor.kt:25-81`: `product="wow_ac_335"`, root-level layout (`Wow.exe` + `Data/` + `WTF/` together), `exeNames=listOf("Wow.exe")`, no region/portal, plus an `isLocal` flag.
2. **Bypass the Blizzard gates** for that flavor: `.build.info` requirement (`WoWForeverScreen.kt:186-187,933-934,1240`), product match (`:923,955`), the ARM64 CDN downloader and its `check(arm64Exe.exists())` (`:1258-1259`, `WowClientDownloader.kt:21-22,156-157`).
3. **Container profile**: per-flavor container id, `emulator="Box64"`, `wow64Mode=true`, **no** `-d3d11` arg (`WoWForeverScreen.kt:1309`), `wincomponents` so the 32-bit d3d9 comes from Wine, drives like `wow-mobile`'s (`WowContainerHelper.java:34-50`).
4. **Realm plumbing (new code)**: write `Data/<locale>/realmlist.wtf` (`set realmlist 127.0.0.1`) and force `realmList` in `WTF/Config.wtf`, modelled on `wow-mobile`'s `Provisioner.java:197-236`; drop the retail-only `portal`/`agentUID`/`gxApi "D3D11"` forcing (`WoWForeverScreen.kt:1316-1350`). For a stock 3.3.5a client `realmlist.wtf` is honoured, so no glue patching is needed (unlike the Ascension client).
5. **Login**: 3.3.5a has no `login.txt` mechanism; AC accounts are created in the server console, and the client's normal login screen is used. The flavor's `loginReady` gate must therefore be relaxed for `isLocal`.
6. **Optional per-flavor niceties**: hide the Blizzard updater UI and the `.build.info` wording for this flavor.

Costs: mostly Kotlin/Compose, one flavor + validation branches + a container JSON; the risky unknown is step 3's actual 32-bit boot (§2). The dwarfing unknown is that this is a **fork**, so upstream drift is manual.

---

## 4. Server half inside the launcher app — what is missing

Facts that make "just add a server container" a new subsystem, not a patch [V]:

| Obstacle | Evidence |
|---|---|
| No Android host component: zero `<service>`/`<receiver>`, no `startForeground`, no notification, no wakelock; the session lives only as long as `MainActivity` | `AndroidManifest.xml:11-43`; `MainActivity.kt:107,193-217`; grep for `startForeground\|foregroundServiceType\|NotificationManager\|PARTIAL_WAKE_LOCK` → 0 hits |
| One environment, one rootfs: `PluviaApp.xEnvironment` static, `ImageFs.INSTANCE` singleton, `containerVariant` is **global** (re-extracts the whole rootfs on change) | `PluviaApp.kt:59-66`; `ImageFs.java:12,48-57`; `ImageFsInstaller.java:91-101,127-166` |
| Two containers cannot run at once: a second launch reuses the first environment; X/SHM/Pulse sockets have fixed names and are unlinked on re-create | `XServerScreen.kt:2094-2244`; `ContainerManager.java:76-82`; `UnixSocketConfig.java:8-33` |
| Teardown kills the server: `stop()` SIGKILLs **every process of the app UID** (matched by `ps -A` + `USER`), and backgrounding `SIGSTOP`s every process whose cmdline contains `wine`/`exe` | `ProcessHelper.java:99-140,359-406`; `BionicProgramLauncherComponent.java:124-138`; `PluviaApp.kt:88-111`; `XServerScreen.kt:3576` |
| PRoot is vendored but dead: `#add_subdirectory(proot)` commented, and its only caller (`GuestProgramLauncherComponent`) is never instantiated; **Gradle does not compile `app/src/main/cpp` at all** (no `externalNativeBuild`), so uncommenting changes nothing | `cpp/CMakeLists.txt:8`; `GuestProgramLauncherComponent.java:196-299`; `app/build.gradle.kts` (no `externalNativeBuild`) |
| The glibc variant is unprovisioned: the code expects `imagefs_gamenative.txz`, but the app only downloads the bionic tarball | `ImageFsInstaller.java:127-129` vs `PluviaMain.kt:515-518` |
| Executing a **glibc** aarch64 server binary from app-private storage at `targetSdk 36` is the untested core risk: the in-tree mitigation is `/system/bin/linker64 <path>` (bionic/linker-able ELFs only) plus the **closed-source** `libredirect.so`; the ELF-interp patcher (`patchelf`) is a stub | `ProcessHelper.java:337-341`; `patchelf_wrapper.cpp:1-84`; `THIRD_PARTY_NOTICES:40-102` |

**Feasible shape if it is done anyway** (weeks, not days):

- A `ServerComponent` modelled on `PulseAudioComponent` — an `EnvironmentComponent` that daemons a binary out of `nativeLibraryDir` with its own `LD_LIBRARY_PATH`, no X server and no Wine (`PulseAudioComponent.java:180-215`) — plus a second, **separate** rootfs directory that is not `imagefs`, so it cannot be clobbered by container-variant changes.
- A foreground service (`specialUse`), notification with stop/restart/log, `PARTIAL_WAKE_LOCK`, `START_STICKY`-style watchdog, and a log pane fed by `ProcessHelper.addDebugCallback` (the `wow-mobile` fork has exactly this viewer to copy).
- Exclusions in `ProcessHelper`'s UID-wide sweeps for server PIDs (they must survive session exit), and a decision on whether the server keeps running while the game runs — with one `imagefs` and fixed socket names, the server must *not* be a container in the existing sense.
- Server binaries built for the target's libc: either NDK/bionic (no proot, but AzerothCore has never been built against bionic) or glibc + PRoot built out-of-tree and dropped into `jniLibs` (PRoot must then be exec'd from `nativeLibraryDir`, and its ptrace mechanism must survive this device's SELinux policy — unverified).

---

## 5. Recommended delivery order

| | A. Server in Termux (or a companion app) + launcher client half | B. Everything inside the launcher | C. Windows server binaries in the existing Wine container |
|---|---|---|---|
| Server | Termux: bionic `mariadb` + AzerothCore built for aarch64, or a proot-distro Ubuntu rootfs with `mysql-server` and a CI-built aarch64 tarball | new platform subsystem (§4) | none official [V]: 0 release assets on `azerothcore/azerothcore-wotlk`, no releases at all on `mod-playerbots/{mod-playerbots,azerothcore-wotlk}`; community x64 zips exist (`onur/mod-playerbots-portable` v0.3.4, 352.8 MB, bundling MySQL 8.0 and a supervisor) [R] |
| Launcher change | §3 only | §3 + §4 | ~0 |
| Risk | Termux/Android process lifetime (phantom killer, freezer), MariaDB vs the MySQL-8 client-library check (`FindMySQL.cmake:54,63-64`) | exec/ELF-interp at `targetSdk 36`, PRoot on this kernel policy, single-environment refactor | needs a maintained fork that publishes Windows binaries; x86 translation of a heavily threaded server |
| Verdict | **start here** | plan, do not start | dead end as of today |

Server-side variants — Ubuntu arm64 rootfs with the distro's MySQL 8, Termux-native MariaDB (the core's configure step enforces a MySQL 8.0+ client, so it needs patching), and a SQLite-backend AzerothCore fork that removes `mysqld` altogether — are compared in [`SERVER-STACK.md`](SERVER-STACK.md) §2. That document also records that the guide's bot overrides (`AC_AI_PLAYERBOT_*`) no longer match the module's renamed keys, so its 1600/2000 figures do not apply (§6 there).

For A, the two apps meet at `127.0.0.1`: the server binds loopback (`BindIP`, ports 3724/8085), the client writes `realmlist` `127.0.0.1`. Loopback between an app and Termux is plain TCP on the same network namespace — but note the launcher's guest `/etc/hosts` rewrite maps `localhost` to the device's LAN IP (`NetworkInfoUpdateComponent.java:46-50`), so always use the **numeric** address [V].
