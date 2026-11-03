# Skateboard: the first pass

Companion to [`SKATEBOARDING-DESIGN.md`](SKATEBOARDING-DESIGN.md), which establishes what is
feasible and why. This document is the build plan for the first shippable slice: the module, the two
shield decks, the packaging, and the acceptance check for each step.

Status: **plan only, nothing implemented.** The deck rows are chosen (`Engraved Wall`, `Iron Coffin
Lid`); the momentum model, the limits and the evidence are in the design document.

Confidence: **[V]** verified here from source, artifact or API, **[R]** reported, **[I]** inference.

---

## 1. Packaging: one module, one client patch, no add-on yet

| Piece | Kind | First pass | Why |
|---|---|---|---|
| `modules/mod-skateboard` | C++ module | **yes** | every authoritative decision: the speed cap, the integrator, airtime, tricks, crash |
| `conf/mod-skateboard.conf.dist` | config | **yes** | all tuning; no SQL, nothing stored |
| `scripts/client-patch/patch_client_dbc.py` | DBC + MPQ | **yes** | the two deck rows, in the same patch archive the realm already ships for `Item.dbc` |
| launcher add-on (Lua) | client-side | **no** | it can only add trick *input* and a HUD, and phase 1 fires tricks from airtime |

**An add-on is not a physics path.** It runs inside the client, sandboxed, and cannot change
movement. Two things it could do: read `GetUnitSpeed("player")` for a speed/combo HUD, and deliver a
button press to the module. The second needs a whisper RPC because a pad-bound button cannot type
chat, and that pattern already exists in this stack — `modules/mod-e-interact` answers the add-on's
whisper, and `EInteract.lua` appends a row to `ConsolePort:GetCustomBindings()`. That is phase 3
work; deferring it is the difference between a playable first pass and a playable first pass with a
speedometer.

**Not Eluna.** Server-side Lua appears nowhere in this repository: modules here are C++,
compile-checked in `module-checks.yml` against the pinned core in about two minutes, and the loop
needs `PlayerScript::OnPlayerUpdate` at 10 ms plus two packet hooks. C++ is the house path and the
only one that gets CI coverage.

## 2. The module

### 2.1 Files

```
modules/mod-skateboard/
  src/skateboard_common.h        Config struct, Deck state, constants, the trick table
  src/Skateboard.cpp             Manager (tick, cap gate, slope), the four script overrides
  src/mod_skateboard_loader.cpp  the loader
  conf/mod-skateboard.conf.dist
```

No `data/sql`: nothing is persisted. (Scores, if they ever exist, belong in the characters DB, built
programmatically like `mod-survivor`'s tables — a failing module SQL file aborts the boot on this
fork.)

### 2.2 State, one record per boarded player

| Field | Type | Meaning |
|---|---|---|
| `v` | `float` | deck speed in yd/s, `0..MaxSpeed` |
| `headingX/Y` | `float` | last movement direction, from the last two samples |
| `airborne` | `bool` | `MOVEMENTFLAG_FALLING` was set on the last packet |
| `takeoffMs` | `uint32` | when the fall started |
| `trickEndMs` | `uint32` | when the queued clip finishes |
| `publishedRate` | `float` | the rate the server currently holds |
| `pendingAcks` | `uint8` | `SMSG_FORCE_RUN_SPEED_CHANGE` sent, ACK not yet seen |
| `lastRaise` | `bool` | the last published change was an increase |
| `gateDeadlineMs` | `uint32` | ACK timeout (§2.4) |
| `displayId` | `uint32` | what was written to `UNIT_FIELD_MOUNTDISPLAYID` |

A `std::unordered_map<ObjectGuid, Deck>` in the manager; every hook returns early unless the player
is in it, so the 10 ms tick costs one hash lookup for everyone else.

### 2.3 The integrator

Runs from `PlayerScript::OnPlayerUpdate(Player*, uint32 p_time)` (`PlayerUpdates.cpp:314`), and reads
the movement packets through `MovementHandlerScript::OnPlayerMove` (`MovementHandler.cpp:658-661`):

| Term | Formula | Notes |
|---|---|---|
| slope | `a = SlopeGain * -(h(pos + heading*2) - h(pos)) / 2` | `Map::GetGridHeight` (`Map.cpp:1222`) — terrain only, no `z` needed, no normals exist |
| friction | `a -= FrictionStatic + FrictionRolling * v * v` | stops in ~8 s from 21 yd/s at the defaults |
| push | `v += PushImpulse` on the command, clamped | one kick per press, not hold-to-accelerate |
| airtime | friction and slope skipped while `airborne` | speed is kept through the arc |
| clamp | `v = clamp(v, 0, MaxSpeed)` | 21 yd/s default = 3× the 7.0 base run rate (`Unit.cpp:93-100`) |

The player is the mover (`MovementHandler.cpp:434`), so `v` is published as a **cap**:
`SetSpeed(MOVE_RUN, v / 7.0f, true)`. A player who is not pressing forward does not move — that is
the model, not a bug (design §3).

### 2.4 The cap gate — the one dangerous operation

`MovementHandler.cpp:763-776` compares the client's reported speed with the server's with a 0.01
tolerance and kicks on `"Incorrect speed"` when the **client is higher**. So:

1. observe `SMSG_FORCE_RUN_SPEED_CHANGE` behind `ServerScript::CanPacketSend` → `pendingAcks++`;
2. observe `CMSG_FORCE_RUN_SPEED_CHANGE_ACK` behind `CanPacketReceive` → `pendingAcks--`;
3. never send a second change while `pendingAcks > 0`;
4. never lower while `lastRaise` and the ACK has not arrived;
5. **timeout**: if 500 ms pass with `pendingAcks > 0`, clear the gate and log once — the counter the
   core keeps is a `uint8` and a lost ACK must not wedge the deck forever;
6. re-assert on drift: if `fabs(GetSpeedRate(MOVE_RUN) - target) > 0.01`, publish again (aura
   refreshes call `UpdateSpeed` and wipe the rate — `SpellAuraEffects.cpp:3817`,
   `Unit.cpp:10379`), through the same gate.

On a self-hosted realm the RTT is ~1-5 ms, so the gate still allows tens of updates per second.

### 2.5 The deck visual

First experiment, one config line apart:

| `Skateboard.MountedPose` | What the module writes | What it should look like |
|---|---|---|
| `0` (first try) | `SetUInt32Value(UNIT_FIELD_MOUNTDISPLAYID, id)` only (`UpdateFields.h:126`) | the deck under a **standing** player — the pose a skateboard wants |
| `1` | `Unit::Mount(id)` (`Unit.cpp:10262`), which also sets `UNIT_FLAG_MOUNT` | the deck under a **sitting** rider, and `UpdateSpeed` takes the mounted branch |

Mode 0 is unverified [I]: whether the 3.3.5a client draws a mount display without the flag is a
device question, and answering it is the first thing the first boot is for. If it does not render,
mode 1 is the fallback and §2.4's drift re-assert becomes mandatory rather than defensive.

The display id comes from config, validated once at load against `sCreatureDisplayInfoStore`
(`DBCStores.cpp:294`); a missing id warns once and falls back to the stock `Log Ride` display
`26609` (`§4` of the design document), which needs no patch at all.

### 2.6 Tricks

No input in the first pass — the existing jump button is the trick button.

| Event | Detection | Action |
|---|---|---|
| take-off | `MOVEMENTFLAG_FALLING` set on a packet | record `takeoffMs`, queue a clip by airtime band |
| clip | `Unit::HandleEmoteCommand` (`SMSG_EMOTE`, `ChatPackets.h:31`) | `EMOTE_ONESHOT_JUMPSTART` 399 short, `JUMPLANDRUN` 394 medium, `STOMP` 388 long; `MOUNT_SPECIAL` 377 reserved for a deck that carries the animation |
| flair | `Unit::SendPlaySpellVisual(id)` (`Unit.cpp:14810`) | one stock kit per clip, ids from config |
| landing | `MOVEMENTFLAG_FALLING` cleared | clean: `v += LandingBonus`; mistimed (landed before `trickEndMs`): crash |

### 2.7 Crash and cleanup

Crash = `Unmount`, damage (`CrashDamage` percent of max health, clamped to `health - 1` when
`HardcoreSafe`), and `UNIT_FLAG_DISABLE_MOVE` for `CrashStaggerMs`.

The deck must never outlive its session: clear on `OnPlayerJustDied`, `OnBeforeLogout`,
`OnMapChanged`, seated on a vehicle, `.board off`, and (config) entering combat. The bot gate is
`WorldSession::IsHeadless()` — the same test `mod-hardcore` uses (`Hardcore.cpp:140-149`) — so no bot
is ever boarded.

### 2.8 Config and commands

| Key | Default | Meaning |
|---|---|---|
| `Skateboard.Enable` | 0 | whole feature |
| `Skateboard.Deck` | `engravedwall` | `engravedwall`, `coffinlid`, `log`, or a raw display id |
| `Skateboard.MountedPose` | 0 | §2.5 |
| `Skateboard.MaxSpeed` | 21.0 | yd/s |
| `Skateboard.PushImpulse` | 4.0 | yd/s per kick |
| `Skateboard.Friction.Static` / `.Rolling` | 1.5 / 0.004 | the `c0` / `c1` terms |
| `Skateboard.SlopeGain` | 9.0 | multiplier on the terrain gradient |
| `Skateboard.LandingBonus` | 2.0 | `v` added by a clean landing |
| `Skateboard.Crash.Enable` / `.DamagePct` / `.StaggerMs` | 1 / 8 / 1500 | the wipeout |
| `Skateboard.HardcoreSafe` | 1 | a crash can never kill |
| `Skateboard.OffInCombat` | 1 | a 21 yd/s cap is a free disengage |
| `Skateboard.AckTimeoutMs` | 500 | §2.4 |
| `Skateboard.Debug` | 0 | one log line per 5 s: `v`, slope, rate, `pendingAcks` |

Commands, `CommandScript`: `.board on|off`, `.board speed`, `.board trick <n>`. All read in
`WorldScript::OnAfterConfigLoad`, so `.reload config` applies them without a restart.

## 3. The deck rows

Two rows per deck, both appended with ids above the current maximum so the file stays sorted — the
existing `Item.dbc` tooling already refuses to break id order for exactly this reason (binary search
on the client side).

| DBC | Fields written | Source |
|---|---|---|
| `CreatureModelData.dbc` | `Id`, `Flags = 0x80` (`CREATURE_MODEL_DATA_FLAGS_CAN_MOUNT`), `ModelPath`, `ModelScale`, `CollisionWidth`, `CollisionHeight`, `MountHeight` | `DBCStructure.h:797-799` (flags), `:801-818` (fields) |
| `CreatureDisplayInfo.dbc` | `Displayid`, `ModelId`, `ExtendedDisplayInfoID = 0`, `scale` | `DBCStructure.h:747-763` |

| Deck | Model path | Flags | `scale` | `MountHeight` |
|---|---|---|---|---|
| Engraved Wall | `ITEM\OBJECTCOMPONENTS\SHIELD\Shield_Rectangle_B_01.mdx` | `0x80` | 1.6 | 0.15 |
| Iron Coffin Lid | `ITEM\OBJECTCOMPONENTS\SHIELD\Shield_Northrend_C_02.mdx` | `0x80` | 1.6 | 0.15 |

Scale and seat height are starting guesses and are tunable in the display row alone, without
recompiling or re-extracting. Both paths come from `ItemDisplayInfo.dbc` rows 26283 and 56501.

### 3.1 Tool changes

`scripts/item-dbc/patch_item_dbc.py` becomes `scripts/client-patch/patch_client_dbc.py` and carries
three members instead of one:

1. `patch_archive()` stops hard-wiring `(DBFilesClient\Item.dbc, (listfile))` and takes a member
   list;
2. the ownership test `block_count != 2` (`:450`) becomes "the listfile is a subset of our known
   member names **and** contains `DBFilesClient\Item.dbc`" — that test must match the *old* two-member
   archive too, or an upgrade writes a second archive and leaves the old `patch-9` winning for
   `Item.dbc`, silently shadowing the new rows;
3. `--verify` reports the winning archive per file, not just for `Item.dbc`;
4. `--server-dbc` writes all three into `DataDir/dbc`, because the server reads the mount row's
   `MountHeight` for collision (`Unit.cpp:16790-16795`) and validates the display id at load.

`build_mpq(files)` needs no change: it already takes an arbitrary member list, stored single-unit,
no compression (`patch_item_dbc.py:264-305`). Three DBCs are ~9 MB raw; that is fine for an archive
the client reads.

## 4. Wiring checklist

Every place an in-tree module appears today, so `mod-skateboard` does not get a silent half-install:

| Place | What to add | Existing reference |
|---|---|---|
| `pins.toml` | `[module.skateboard]` with this repo and the commit | `pins.toml:49` (`[module.survivor]`) |
| `.github/workflows/server-aarch64.yml` | `cp -a "$GITHUB_WORKSPACE/modules/mod-skateboard" core/modules/mod-skateboard` before configure | `:101-104` |
| `scripts/thor-install.sh` | module-list comments and the `fetch_self_module` extract globs/asserts, conf creation, `--status` | `:69`, `:1397`, `:1479-1534`, `:1711`, `:2058-2061` |
| `scripts/windows-install.ps1` | `$SkateboardConfDefaults`, `Write-Configs`, `Invoke-Checks`, and the client-patch invocation | `:248-262`, `:2959-3049` |
| `.github/workflows/module-checks.yml` | picks the new module up by glob — verify, then add `scripts/skateboard/check_skateboard.py` | the `on.push.paths` gate |
| `scripts/skateboard/check_skateboard.py` | conf keys against the code and both installers, hook names against the core headers, emote/visual ids against `SharedDefines.h`, deck ids against the patch tool | pattern of `scripts/creature-gear/check_creature_gear.py` |
| `docs/` | this plan, the status line in `SKATEBOARDING-DESIGN.md`, the `README.md` index row, the `RUNBOOK.md` device steps | — |

## 5. Acceptance

| Step | Check | Pass |
|---|---|---|
| offline | `scripts/syntax-check.sh` on the module; `check_skateboard.py` | compiles against the pinned core, every conf key read by the code, every emote id exists |
| boot | worldserver with the module | `mod-skateboard: loaded … deck display 90001`, conf read, zero ERROR lines |
| deck | `.board on` | the deck renders under the player; `MountedPose` decided and recorded |
| momentum | ride downhill in Elwynn, then uphill | `v` climbs without input on the slope, bleeds uphill, coasts to a stop in ~8 s; `Debug` line shows `v`, slope and rate agreeing |
| cap | 10-minute ride, `Debug` on | zero `Incorrect speed` kicks, `pendingAcks` never stuck, rate within 0.01 of target between changes |
| tricks | jump while boarded | one clip per take-off, clean landing adds `v`, mistimed landing crashes once |
| cleanup | logout, die, change map, `.board off` | no `UNIT_FIELD_MOUNTDISPLAYID` left, no state left in the map |
| deck rows | step 4 on the device | both shields render, `--verify` finds all three members in one archive, the server boots with the patched DBCs |

## 6. Risks and unknowns

| Risk | Handle |
|---|---|
| A mount display without `UNIT_FLAG_MOUNT` may not render | the first boot answers it; mode 1 is the fallback (§2.5) |
| A lost speed ACK wedges the gate | 500 ms timeout (§2.4) |
| The shield renders with its default skin, not the item's variant | `ItemDisplayInfo` picks the skin, not the model; check on the device and bake it only if it matters |
| The shield's origin is not the deck's centre | scale and `MountHeight` are the only DBC knobs; a real offset needs a re-rooted M2 |
| New DBC ids collide with a future client-data release | ids above the current maxima, derived by the tool, and `--check` fails if the file already holds them |
| A crash kills a Hardcore character | `HardcoreSafe` clamps damage to `health - 1` |
| Combat abuse, water, instances, flying | refused on board, forced off on entering combat/water/vehicle (config) |
| A bot is boarded | `WorldSession::IsHeadless()` gate (`Hardcore.cpp:140-149`) |

## 7. Order of work

Each step lands alone and is verifiable on its own.

| # | Step | Content | Acceptance |
|---|---|---|---|
| 1 | skeleton | module dirs, loader, config, `.board on/off`, deck display (stock `26609`), all wiring, the offline checker | step 1 of §5: boots, deck renders, pose question answered |
| 2 | momentum | integrator, cap gate, drift re-assert, `Debug` logging | steps 4-5: the curve, no kicks |
| 3 | tricks and crash | airtime bands, emote + visual, landing bonus, crash, cleanup hooks | steps 6-7 |
| 4 | deck rows | rename and extend the patch tool, both shield rows, installers, server-side DBCs | step 8: both decks on the device, `--verify` clean |
| 5 | docs | this plan's status line, design-doc status, README, RUNBOOK | — |

Step 1 without step 4 is the whole experiment: if a stock log and a speed cap do not feel like
skating, the decks do not matter, and step 4 is two DBC rows and a tool change away from being
irrelevant.
