# Skateboard: the first pass

Companion to [`SKATEBOARDING-DESIGN.md`](SKATEBOARDING-DESIGN.md), which establishes what is
feasible and why. This document is the build plan for the first shippable slice: the module, the two
shield decks, the packaging, and the acceptance check for each step.

Status: **steps 1-3 and 5 implemented and gated; step 4 (the deck rows) is open.** The
module, the add-on, the wiring and the offline checker are in the tree; the two shields still ride on
the client patch from step 4, so the shipped deck is the stock log. The deck rows are chosen (`Engraved Wall`, `Iron Coffin
Lid`); the momentum model, the limits and the evidence are in the design document.

Confidence: **[V]** verified here from source, artifact or API, **[R]** reported, **[I]** inference.

---

## 1. Packaging: one module, one client patch, no add-on yet

| Piece | Kind | First pass | Why |
|---|---|---|---|
| `modules/mod-skateboard` | C++ module | **yes** | every authoritative decision: the speed cap, the integrator, airtime, tricks, crash |
| `conf/mod-skateboard.conf.dist` | config | **yes** | all tuning; no SQL, nothing stored |
| `scripts/client-patch/patch_client_dbc.py` | DBC + MPQ | **yes** | the two deck rows, in the same patch archive the realm already ships for `Item.dbc` |
| launcher add-on (Lua) | client-side | **yes — activation, bar and HUD** | `SetBindingClick` puts a named action in the *stock* Key Bindings frame, and the bar and HUD are pure Lua; nothing else can do either (§2.9) |

**An add-on is not a physics path.** It runs inside the client, sandboxed, and cannot change
movement. What it can do is own the *input*: a click binding gives a named action that the stock Key
Bindings frame lists — a keyboard player rebinds it there, a pad player assigns a button through
ConsolePort's `/cp` list, and neither path claims a physical button of its own. `EInteract.lua`
already does exactly this in this stack (`SetBindingClick` at `:203-213`, the `BINDING_NAME` global,
the `ConsolePort:GetCustomBindings()` row at `:221-233`), so it is proven rather than plausible.

That is the one thing the module cannot do for itself, which is why the add-on moves into the first
pass. It carries three things, all pure Lua: the activation bindings (step 1), the speed readout over
`GetUnitSpeed` (step 2), and the bar whose trick slots appear once the module advertises them (step 3).

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
| `airborne` | `bool` | `MOVEMENTFLAG_FALLING` was set on the last packet; heading is not stored, the tick reads `player->GetOrientation()` |
| `takeoffMs` | `uint32` | when the fall started |
| `trickEndMs` | `uint32` | when the queued clip finishes |
| `publishedRate` | `float` | the rate the server currently holds |
| `pendingAcks` | `uint8` | `SMSG_FORCE_RUN_SPEED_CHANGE` sent, ACK not yet seen |
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

1. `PublishRate()` sets `pendingAcks = 1` in the same call that forces the change, so there is
   never a window in which the packet is out and the gate is open;
2. `ServerScript::CanPacketReceive` watches `CMSG_FORCE_RUN_SPEED_CHANGE_ACK` and `OnAck()` clears
   `pendingAcks` back to `0` and arms `PublishThrottleMs` before the next send;
3. never send a second change while `pendingAcks > 0`, and never lower the cap while one is in
   flight: lowering needs the client's ACK for the higher rate first, so the tick simply waits;
4. **timeout**: if `AckTimeoutMs` passes with `pendingAcks > 0`, clear the gate and log once — the
   counter the core keeps is a `uint8` and a lost ACK must not wedge the deck forever;
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
| take-off | `MOVEMENTFLAG_FALLING` set on a packet | record `takeoffMs`, queue a clip by the speed band `v` is in |
| clip | `Unit::HandleEmoteCommand` (`SMSG_EMOTE`, `ChatPackets.h:31`) | `EMOTE_ONESHOT_JUMPSTART` 399 short, `JUMPLANDRUN` 394 medium, `STOMP` 388 long; `MOUNT_SPECIAL` 377 reserved for a deck that carries the animation |
| flair | `Unit::SendPlaySpellVisual(id)` (`Unit.cpp:14810`) | one stock kit per clip, ids from config |
| landing | `MOVEMENTFLAG_FALLING` cleared | clean: `v += LandingBonus`; mistimed (landed before `trickEndMs`): crash |

### 2.7 Crash and cleanup

Crash = the deck is cleared and the run rate restored, damage (`Crash.DamagePct` percent of max
health, clamped to leave at least one point of health when `HardcoreSafe`, and to zero at exactly one
point), then `SetControlled(true, UNIT_STATE_ROOT)` for `Crash.StaggerMs`. The stagger shares the
core's root state, so a spell root that expires inside the window ends it early.

The deck must never outlive its session: clear on `OnPlayerJustDied`, `OnBeforeLogout`, a map change
(detected on the next tick, not through a hook), seated on a vehicle, `.board off`, and (config) being
hit by another unit - a fight that merely starts is not a hit. `.board on` is refused while already in
combat whatever the config says. A boarding cast is not the deck: nothing is written until it
completes, so a step, a hit, a fight, a death or a teleport simply drops the record. The bot gate is
`WorldSession::IsHeadless()` — the same test `mod-hardcore` uses (`Hardcore.cpp:140-149`) — so no bot
is ever boarded.

### 2.8 Config and commands

| Key | Default | Meaning |
|---|---|---|
| `Skateboard.Enable` | 0 | whole feature |
| `Skateboard.Deck` | `log` | `log` (26609, no client patch), `engravedwall` (90001) or `coffinlid` (90002) once step 4 lands the rows, or a raw display id |
| `Skateboard.MountedPose` | 0 | §2.5 |
| `Skateboard.MaxSpeed` | 21.0 | yd/s |
| `Skateboard.PushImpulse` | 4.0 | yd/s per kick |
| `Skateboard.Friction.Static` / `.Rolling` | 1.5 / 0.004 | the `c0` / `c1` terms |
| `Skateboard.SlopeGain` | 9.0 | multiplier on the terrain gradient |
| `Skateboard.LandingBonus` | 2.0 | `v` added by a clean landing |
| `Skateboard.Tricks` | 1 | auto tricks on take-off, and the bar's trick slots |
| `Skateboard.Trick.VisualKit` | 5036 | stock `SpellVisualKit` id sent with each clip, 0 = no flair |
| `Skateboard.Crash.Enable` / `.DamagePct` / `.StaggerMs` | 1 / 8 / 1500 | the wipeout |
| `Skateboard.HardcoreSafe` | 1 | a crash can never kill |
| `Skateboard.OffOnHit` | 1 | a 21 yd/s cap is a free disengage; a hit takes the deck, the start of a fight does not |
| `Skateboard.BoardCastMs` | 2000 | boarding takes this long, and a step, a hit or a fight stops it |
| `Skateboard.BoardCastMs` | 2000 | boarding delay; the client's bar is driven by the module's `c` push |
| `Skateboard.AckTimeoutMs` | 500 | §2.4 |
| `Skateboard.Debug` | 0 | one log line per 5 s: `v`, slope, rate, `pendingAcks` |

Commands, `CommandScript` (`Acore::ChatCommands`, so both the chat line and the add-on channel can
reach them): `.board on`, `.board off`, `.board toggle`, `.board status`, `.board speed`,
`.board push`, `.board trick <1|2|3>`. `.board on` starts `Skateboard.BoardCastMs` of boarding (0 boards instantly) and pushes `AzerothCore\tcbegin=<ms>` / `cdone` / `ccancel=<reason>` on the add-on channel, which is what draws and stops the add-on's bar. `.board status` answers one parseable line,
`boarded=<0|1> tricks=<n> push=<0|1> deck=<displayId> speed=<yd/s>`, which is what the add-on builds its slots
from, and `.board speed` answers `v=<yd/s> rate=<rate> pending=<n>`. Config is read in
`WorldScript::OnAfterConfigLoad`, so `.reload config` applies it without a restart.

## 2.9 Activation, and how the binds take over

The requirement: one key boards and leaves; while boarded, other keys do board things; it has to
work on a keyboard, on a pad through ConsolePort, and with no pad at all; and it must not be buried
inside ConsolePort's own binding set.

### The activation is a click binding in the add-on

`SetBindingClick(key, "SkateboardButton")` plus a `BINDING_NAME_CLICK SkateboardButton:LeftButton`
global gives a named action that the **stock** Key Bindings frame lists — the player rebinds it there,
and ConsolePort neither owns nor forbids it. `/sb` is the add-on's own client-side command
(`on|off|toggle|status|keys|takeover|hud|bar`); it is not a server command and takes no `key` verb. The add-on appends one row to
`ConsolePort:GetCustomBindings()` so the same action can be assigned to a pad button, and claims no
button itself. `EInteract.lua:203-233` is the working precedent.

| Binding | Action |
|---|---|
| `Skateboard` | board / leave (`.board toggle`) |
| `Skateboard: push` | `.board push` |
| `Skateboard: trick 1`, `Skateboard: trick 2` | `.board trick 1`, `.board trick 2` |
| `Skateboard: bail` | `.board off`, whatever the state |

Five bindings is the whole activation surface — the bar offers the same actions plus trick 3. None of
them needs a client DBC, an item or a spell, and all of them are reachable from the launcher's
external pad, which emits key presses.

### How a press reaches the server

Two paths, both native to this stack:

| Path | When | Evidence |
|---|---|---|
| add-on command channel — `SendAddonMessage("AzerothCore", "i" .. counter .. command, "WHISPER", UnitName("player"))`, which the client puts on the wire as `AzerothCore\t` + body: the tab belongs to `SendAddonMessage`, and sending it in the prefix makes the core read a second tab as the opcode and drop the line (`Chat.cpp:993-996`). The reply arrives as `CHAT_MSG_ADDON` - `arg1` is the prefix, `arg2` the body - and carries `a` (ack), `o` (ok), `f` (failed) or `m` (message) | `Chat.cpp:993-1080`, `Chat.cpp:1033`; parsed before any channel or target check, and never relayed |
| a stock `/say .board` macro | the add-on is missing, or a player simply wants a macro | any chat line starting with `.` or `!` is executed and returns before broadcast (`Chat.cpp:250-270`, `ChatHandler.cpp:255-310`) |

The macro path is the safety net, and it is what step 1 tests first: the whole feature is reachable
with **no client files at all**, so a missing add-on can only cost the bar, never the activation.

### "The other binds take over"

Three client-side options, all in the add-on, all reachable without server work beyond the commands:

1. **binding takeover** — while boarded and out of combat, the add-on remembers
   `GetBindingKey("ACTIONBUTTON1..3")` and re-points those keys at the skateboard buttons
   (`SetBinding`), restoring them on leave. It is the most literal reading of "the other binds take
   over": the keys the player already presses for the action bar become board actions, and nothing
   touches ConsolePort or the action bar pages. Guarded by `InCombatLockdown()`, and the saved table
   is restored on the next login if a reload or a crash cut the ride short. This is what ships.
2. **its own bar** — the add-on's frame with push, trick and bail buttons, plus the five click
   bindings. Always available; it is what the takeover re-points at.
3. **bar page** — a spare action bar page filled with macros. Dropped: `PlaceAction` with a macro
   action needs the macro to exist client-side first, so it is more moving parts than the binding swap
   for the same result.

The HUD is the fourth piece and needs nothing from the server: `GetUnitSpeed("player")` returns the
rate the module publishes as the cap, so the readout is the same number the `Debug` log line carries.
It reads non-zero while coasting without input, because the cap is momentum and the client's
displacement is its own.

### Why not the vehicle system

The vehicle bar is the only client-native "a bar takes over" mechanism, so it was costed:

| Requirement | Cost | Evidence |
|---|---|---|
| vehicle bar buttons | the bar *is* the player's charm action buttons built from the vehicle's **spells**; each trick needs a `Spell.dbc` row | `Player.cpp:9771-9800` |
| a board vehicle | `Vehicle.dbc` row (8 seat ids, locomotion type) + `VehicleSeat.dbc` row (skin, attachment, `VEHICLE_SEAT_FLAG_CAN_CONTROL`) | `DBCStructure.h:2062-2100`, `DBCEnums.h:444-475` |
| something to hold it | `creature_template.VehicleId` plus a spawn, and `Unit::CreateVehicleKit` at load | `CreatureData.h:226`, `Creature.cpp:1678-1681`, `Unit.cpp:14614` |
| boarding | a spell carrying `SPELL_AURA_CONTROL_VEHICLE` — the only entry path in this core | `SpellAuraEffects.cpp:5911` |
| the physics | the mover becomes the vehicle creature, so the integrator, the cap and the ACK gate all retarget, and every `GetVehicle()` special case in the modules has to learn about our board | §2.3, §2.4 |

`Spell.dbc` is the one client table this plan deliberately never touches: unlike `Item.dbc`,
`CreatureModelData` and `CreatureDisplayInfo`, it is not a plain data table — attributes, targets,
cast times, icons and effects all have to be coherent or the client refuses the row. Four spell rows
plus a retarget of the physics is a bad trade for a bar that a click binding already activates.

Revisit only if the page takeover fails on the device *and* a taking-over bar turns out to matter more
than this plan assumes.

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
| `scripts/skateboard/check_skateboard.py` | conf keys against the module's fallbacks and both installers' lists, the `.board` verbs against docs §2.8 and the add-on, the module's presence in pins and all three workflows, the deck aliases against this plan's §3 ids, the loader symbol against `modules/CMakeLists.txt`, and the absence of `DirectCommitTransaction`. It does not check hook names or emote ids against the core headers: those are compile-time facts, and the syntax-check is what proves them | pattern of `scripts/creature-gear/check_creature_gear.py` |
| `docs/` | this plan, the status line in `SKATEBOARDING-DESIGN.md`, the `README.md` index row, the `RUNBOOK.md` device steps | — |

## 5. Acceptance

| Step | Check | Pass |
|---|---|---|
| offline | `scripts/syntax-check.sh` on the module; `check_skateboard.py` | compiles against the pinned core, every conf key read by the code, every emote id exists |
| boot | worldserver with the module | `Skateboard: <enabled|disabled> (deck '<name>', resolved on the first board).`, conf read, zero ERROR lines |
| deck | `.board on` | the deck renders under the player; `MountedPose` decided and recorded |
| activation | bind a key to `Skateboard` in the stock Key Bindings frame, press it | boards, and a second press leaves; the same action appears in `/cp` for a pad button; `/say .board toggle` works with the add-on removed from the folder (a bare `/say .board` prints the subcommand list) |
| momentum | ride downhill in Elwynn, then uphill | `v` climbs without input on the slope, bleeds uphill, coasts to a stop in ~8 s; `Debug` line shows `v`, slope and rate agreeing |
| cap | 10-minute ride, `Debug` on | zero `Incorrect speed` kicks, `pendingAcks` never stuck, rate within 0.01 of target between changes |
| tricks | jump while boarded | one clip per take-off, clean landing adds `v`, mistimed landing crashes once |
| cleanup | logout, die, change map, `.board off` | no `UNIT_FIELD_MOUNTDISPLAYID` left, no state left in the map |
| bar | click the add-on's board button, then its push button | boards and pushes the same as the bindings, and the first button becomes `Bail` while boarded; a trick slot the module has not advertised is hidden rather than failing |
| HUD | ride boarded with `Debug` on | the readout's `cap` matches the `Debug` line's `v` (it comes from the status poll, so it lags a poll); its speed half is `GetUnitSpeed`, which reads 0 at a standstill, so the two numbers together say whether the deck is moving |
| takeover (toggle) | board out of combat with the takeover on | the player's own action bar keys do board actions, and every original binding is restored on leave, on reload and on the next login after a crash |
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
| Combat abuse, water, instances, flying | boarding is refused in combat, in water, in an instance, in a BG/arena, while flying or mounted; water/a vehicle/a map change/being hit force the deck off (config where noted) |
| A bot is boarded | `WorldSession::IsHeadless()` gate (`Hardcore.cpp:140-149`) |
| ConsolePort hides or rewrites the click binding | it lists custom bindings by design (`EInteract.lua:221-233`), and the `/say` macro path needs no add-on at all |
| The add-on command channel is refused with no party and no guild | send the add-on message as a self-whisper; the parse happens before any target or channel check (`ChatHandler.cpp:255-310`), and the macro path stays as the fallback |
| Rebinding the player's action bar keys fails or is refused in combat | guarded by `InCombatLockdown()`; the saved table restores on leave and on the next login, and the bar's own buttons work either way |
| A bar takeover is a spell bar (the vehicle route) | deferred with the cost table in §2.9; four `Spell.dbc` rows plus a physics retarget |

## 7. Order of work

Each step lands alone and is verifiable on its own.

The bar and the HUD are not their own step any more: both are pure Lua with no dependency on the
module beyond the commands it already has, and the HUD reads `GetUnitSpeed("player")`, which is the
same number the module publishes as the cap. The only client behaviour that is genuinely unknown is
the **binding takeover** (mutating the player's own bindings out of combat), so it stays a toggle
inside step 3's bar rather than a step of its own.

| # | Step | Content | Acceptance |
|---|---|---|---|
| 1 | skeleton, activation and bar | module dirs, loader, config, `.board on/off/toggle`, deck display (stock `26609`), all wiring, the offline checker; add-on: the click bindings, the bar (board/leave, push), the `/say .board` macro fallback | §5 rows 1-4 and the bar row: boots, deck renders, pose answered, a bound key and the bar both board and leave |
| 2 | momentum and HUD | integrator, cap gate, drift re-assert, `Debug` logging; the readout over `GetUnitSpeed` | §5 momentum, cap and HUD rows: the curve, no kicks, the readout matches the log |
| 3 | tricks, crash and takeover | airtime bands, emote + visual, landing bonus, crash, cleanup hooks; the bar's trick slots appear once `.board status` advertises them; the binding takeover behind a toggle | §5 tricks, cleanup and takeover rows |
| 4 | deck rows | rename and extend the patch tool, both shield rows, installers, server-side DBCs | §5 deck-row row: both decks on the device, `--verify` clean |
| 5 | docs | this plan's status line, design-doc status, README, RUNBOOK | — |

The vehicle route is out of the plan, not deferred to a step: revisiting it means new `Spell.dbc`
rows and a retarget of the physics (§2.9).

Step 1 without step 4 is the whole experiment: if a stock log and a speed cap do not feel like
skating, the decks do not matter, and step 4 is two DBC rows and a tool change away from being
irrelevant.
