# Skateboard: the first pass

Companion to [`SKATEBOARDING-DESIGN.md`](SKATEBOARDING-DESIGN.md), which establishes what is
feasible and why. This document is the build plan for the first shippable slice: the module, the two
deck rows, the packaging, and the acceptance check for each step.

Status: **steps 1-3 and 5 implemented and gated; step 4 (the deck rows) is implemented and open on a
device check** - the first playtest answered its two open questions (a doodad, not a shield; a spline
follow, not a relocate) and found a third: the carry's arc was being read as a jump (§2.3). The
module, the add-on, the wiring and the offline checker are in the tree; the two doodad decks still ride on
the client patch from step 4, so the shipped deck is the stock log. The deck rows are chosen (the
Redridge boardwalk and the Duskwood coffin lid); the momentum model, the limits and the evidence are in
the design document.

Confidence: **[V]** verified here from source, artifact or API, **[R]** reported, **[I]** inference.

---

## 1. Packaging: one module, one client patch, no add-on yet

| Piece | Kind | First pass | Why |
|---|---|---|---|
| `modules/mod-skateboard` | C++ module | **yes** | every authoritative decision: the integrator, the carry impulse, airtime, tricks, crash |
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
| brake | `a -= Brake` while `MOVEMENTFLAG_BACKWARD` is set | the back key is a foot on the ground, on top of friction |
| airtime | friction and slope skipped while `airborne` | speed is kept through the arc |
| clamp | `v = clamp(v, 0, MaxSpeed)` | 21 yd/s default = 3× the 7.0 base run rate (`Unit.cpp:93-100`) |

The player is the mover (`MovementHandler.cpp:434`), so `v` is published as a **cap**:
`SetSpeed(MOVE_RUN, v / 7.0f, true)`. A cap alone means a rider who is not holding a key does not
move, so the deck also **carries** an idle rider (`Skateboard.Drive`): one impulse along
the facing every `Skateboard.Drive.ImpulseMs`, through the client's own knockback channel
(`Unit::KnockbackFrom` → `SMSG_MOVE_KNOCK_BACK`), its arc tilted by the terrain gradient so it
lands back on the slope it left.

The knockback is the only channel that moves a character the client still controls. A relocation is
undone by the next movement packet, and a server spline is obeyed only by a unit the client has
stopped steering - the core even drops a player's own movement while one runs (`VerifyMovementInfo`
rejects packets while `!movespline->Finalized()`).

| `Drive.Mode` | Carrier | Follows the hill | Steering | Playtest |
|---|---|---|---|---|
| `0` (default) | `SMSG_MOVE_KNOCK_BACK`, one arc per `Drive.ImpulseMs` | **mostly** - the arc is tilted by the terrain gradient, so it lands back on the surface after `Drive.ArcMs` on a rise as well as on the flat; a descent too steep for the tilt to follow hands the rider a longer flight instead | instant: the client keeps control between impulses, and a jump can start in the `GroundMs` gap | shipped; "uphill goes nowhere, downhill flies" was the untilted arc, fixed |
| `1` | a player spline per `Drive.SplineMs`, path sampled along the terrain | **yes** - the server builds the path, `Map::GetHeight` gives each point its height | only *between* segments, because the client's own packets are dropped while one runs - so no jump and no trick either | new; unverified on a device |

A level arc is why the knockback carrier read as "uphill goes nowhere, downhill adds velocity but the
plane does not follow the slope". The client ends a knockback where the arc meets the surface, so at
`Drive.ArcMs` 250 a rise takes the flight from 250 ms to 134 ms at 20%, to **zero** at 10% once `v` is
at `MaxSpeed` (`speedZ` below `grade × speedXY` - the rider stands still at 0 yd/s while the deck
still reports `Cruise`), and a 30% drop stretches it to 424 ms at cruise and 1164 ms at the ceiling.
Adding `grade × speedXY` to `speedZ` puts the landing back on the surface after exactly `ArcMs`, and
the module now carries the flight it actually got: a drop that outlives the arc is one glide, both
for the next impulse and for the trick window.

`KnockbackFrom(x, y, speedXY, speedZ)` takes the *source* of the knockback, not a direction: the core
normalizes `thisPosition - source` in `Position::GetSinCos`, so a source one yard behind the rider is
what makes the impulse run along the facing. The vertical half sizes the arc (`2 * (speedZ - grade *
speedXY) / g`, which the tilt makes exactly `Drive.ImpulseMs - Drive.GroundMs` on any hill the arc can
follow), and the horizontal half - divided by `sqrt(1 + grade²)`, because `v` is a surface speed -
covers the whole cycle inside it, which keeps the deck's average at its own `v` and leaves
`Drive.GroundMs` of standing per cycle - the only
window a jump can start in (`Unit::KnockbackFrom` states the rule for feared/confused units: the *client* is
what moves a controlled unit, so the server either sends it a movement message or takes control
away). The impulse keeps the character in control, so forward still pumps (the cap), backward still
brakes, and the camera still steers between impulses. Because `v` is a *surface* speed, a climb takes
less horizontal speed than a level run (`speedXY / sqrt(1 + grade²)`).

**The arc is not a jump, and telling the two apart is the part that broke.** The client reports
`MOVEMENTFLAG_FALLING` for the carry's own knockback, and it reports a *positive* `zspeed` while the
arc rises - the same thing a real jump reports - so no field in the packet distinguishes them. The
first build leaned on the sign anyway, which meant every arc queued a trick clip and **paid
`LandingBonus` on the landing**: +2 yd/s three times a second, faster than friction and slope
together could spend it, so `v` sat at `MaxSpeed` whichever way the hill went. The playtest read that
as "downhill runs flat". The record now carries the window the impulse opened
(`Deck::impulseArcMs`), and inside `impulseArcMs + CarryArcSlackMs` nothing is a trick. Jumps live in
the `Drive.GroundMs` window, which is what that key was always for.

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

Two shapes, and the playtest answered this one: writing the mount display field alone draws
nothing, so the deck has to be an object of its own.

| `Skateboard.Deck.Render` | What the module does | What it looks like |
|---|---|---|
| `unit` (default) | summons a creature with the deck's display and walks it under the rider in `Deck.FollowMs` segments (`Creature::SetDisplayId`, `TempSummon`) | the deck under a **standing** player — the pose a skateboard wants, and a display every client draws |
| `mount` | `Unit::Mount(id)` (`Unit.cpp:10262`), which writes the field *and* sets `UNIT_FLAG_MOUNT` | the deck for certain, but under a **sitting** rider, and `UpdateSpeed` takes the mounted branch |

`mount` remains because it is the only path the core itself exercises; it costs the standing pose.

The display id comes from config, validated once at load against `sCreatureDisplayInfoStore`
(`DBCStores.cpp:294`); a missing id warns once and falls back to the stock `Log Ride` display
`26609` (`§4` of the design document), which needs no patch at all. The shipped default is the
`plank`, which does need one: `scripts/skateboard/patch_deck_dbc.py` writes its
`CreatureModelData`/`CreatureDisplayInfo` rows into the server's `data/dbc` and the client's patch
MPQ. Until that runs, the fallback is the log.

**The deck is a world doodad, not a shield, and the playtest is why.** The first pair were shields
(`Shield_Rectangle_B_01`, `Shield_Northrend_C_02`), and on the device they drew white and on edge.
Both are properties of an *item* model: its M2 has no texture path (the client fills the material
from `ItemDisplayInfo` when it draws the item) and its authored frame is "held", not "on the ground".
A creature display supplies neither, and no DBC field can: there is no texture column and no rotation
column. A doodad model is self-skinned (its M2 names its own `.blp`) and authored flat, so the shipped
pair is now the Redridge boardwalk plank (90001) and the Duskwood coffin lid (90002). Two knobs came
with that:

| Knob | What it does |
|---|---|
| `--model ALIAS=PATH` | any doodad's model path for one of the two ids, so the look is a data change, not a rebuild |
| `--survey HINT [--listfile FILE] [--data-dir DIR]` | every model whose path contains `HINT`: the registered ones with their display ids, plus a 3.3.5a `listfile.txt` and the client's archive listfiles when they can be read. Each hit says whether it is self-skinned (drawable as a creature) and prints the `Skateboard.Deck` id or the `--model` line that would use it - which is how a surfboard, a plank or anything else board-shaped is found in the client the realm ships |

The deck's *placement* is `Deck.Render`. The old note "a flat object under your own feet has nothing
to interpolate" was wrong: `NearTeleportTo` sends an uninterpolated relocate, so at 25 object updates
a second the client snapped the deck from point to point and the rider saw it chasing them. The
follow is a `MoveSplineInit` over the same distance now, which the client interpolates.

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
| `Skateboard.Enable` | 1 | whole feature |
| `Skateboard.Deck` | `plank` | `plank` (90001, the Redridge boardwalk) or `coffinlid` (90002) once `scripts/skateboard/patch_deck_dbc.py` lands the rows, `log` (26609, no client patch), or a raw display id. `engravedwall` is the old name for 90001 and still resolves |
| `Skateboard.Deck.Render` | `unit` | §2.5: `unit` summons the deck under a standing rider, `mount` is the sitting fallback |
| `Skateboard.Deck.Carrier` / `.FollowMs` | `29443` / `40` | the inert stock creature template the deck is summoned from (`unit` mode; a row with an `AIName` or a `ScriptName` is refused), and the length of one follow segment - chained onto the last, aimed at the rider projected a segment ahead and past the staleness of their last movement packet |
| `Skateboard.MaxSpeed` | 21.0 | yd/s |
| `Skateboard.Cruise` | 4.0 | the floor a boarded deck never rolls below |
| `Skateboard.Drive` / `.ImpulseMs` / `.GroundMs` / `.MinSpeed` / `.Brake` | 1 / 350 / 100 / 3.0 / 6.0 | the carry: one knockback impulse per cycle along the facing, the gap the rider stands in, the speed the carry opens at (capped by `Cruise`) and the backward-key brake |
| `Skateboard.Drive.Mode` / `.ArcMs` / `.SplineMs` | 0 / 0 / 150 | which carrier moves the rider (`0` knockback arcs, `1` a terrain-following server spline), the knockback arc's length (`0` = `ImpulseMs - GroundMs`) and one spline segment |
| `Skateboard.PushImpulse` | 4.0 | yd/s per kick |
| `Skateboard.Push.Mode` | 0 | 0 = one kick per press, 1 = hold to pump (`push down` / `push up` from the add-on), one kick every `Push.IntervalMs`, capped by `Push.MaxHoldMs` |
| `Skateboard.Push.IntervalMs` / `.MaxHoldMs` | 500 / 2000 | the pump's rhythm and its lost-`up` backstop |
| `Skateboard.Friction.Static` / `.Rolling` | 1.5 / 0.004 | the `c0` / `c1` terms |
| `Skateboard.SlopeGain` | 30.0 | multiplier on the terrain gradient, sampled 6 yd ahead |
| `Skateboard.LandingBonus` | 2.0 | `v` added by a clean landing |
| `Skateboard.Tricks` | 1 | auto tricks on take-off, and the bar's trick slots |
| `Skateboard.Trick.VisualKit` | 5036 | default stock `SpellVisualKit` id for a list entry without its own, 0 = no flair |
| `Skateboard.Trick.List` | `kickflip:399:5036, ollie:394:5036, grind:388:5036` | the trick list: `name:emote[:kit]` per comma-separated entry, position = speed band, up to 8 |
| `Skateboard.Crash.Enable` / `.DamagePct` / `.StaggerMs` | 1 / 8 / 1500 | the wipeout |
| `Skateboard.HardcoreSafe` | 1 | a crash can never kill |
| `Skateboard.OffOnHit` | 1 | a 21 yd/s cap is a free disengage; a hit takes the deck, the start of a fight does not |
| `Skateboard.BoardCastMs` | 2000 | boarding takes this long, and a step, a hit or a fight stops it; the client's bar is driven by the module's `c` push |
| `Skateboard.AckTimeoutMs` | 500 | §2.4 |
| `Skateboard.Debug` | 0 | one log line per 5 s: `v`, slope, rate, `pendingAcks`, `grade` (the gradient the arc is tilted by) and `ground` (the rider's measured ground speed, which is what the deck follow leads with) |

Commands, `CommandScript` (`Acore::ChatCommands`, so both the chat line and the add-on channel can
reach them): `.board on`, `.board off`, `.board toggle`, `.board status`, `.board speed`,
`.board push [down|up]`, `.board trick <name|index>`, `.board tricks`. `.board on` starts `Skateboard.BoardCastMs` of boarding (0 boards instantly) and pushes `AzerothCore\tcbegin=<ms>` / `cdone` / `ccancel=<reason>` on the add-on channel, which is what draws and stops the add-on's bar. `.board status` answers one parseable line,
`boarded=<0|1> tricks=<n> push=<0|1> deck=<displayId> speed=<yd/s> drive=<0|1> mode=<n> impulse=<ms> brake=<yd/s^2>`,
which is what the add-on builds its slots from - the carry fields ride after the ones it parses, so
the add-on ignores them and `.board status` in chat shows them. `.board tricks` answers
`tricks=<n>` followed by `1 kickflip 399` per entry, which is the add-on's menu; `.board speed`
answers `v=<yd/s> rate=<rate> pending=<n>`. Config is read in
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
| `Skateboard: push` | `.board push down` / `.board push up` (one kick each when `Push.Mode = 0`) |
| `Skateboard: selected trick` | `.board trick <name>`, whatever the menu last selected |
| `Skateboard: tricks menu` | `.board tricks`, then the picker |
| `Skateboard: bail` | `.board off`, whatever the state |

Five bindings is the whole activation surface — the bar offers the same actions plus one button per
entry of `Skateboard.Trick.List`, and the menu is the list itself. None of
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
| Plank | `World\Azeroth\RedRidge\PassiveDoodads\DockPieces\RedRidgeDocksBoardwalk01.mdx` | `0x80` | 2.0 | 0.0 |
| Coffin Lid | `World\Azeroth\Duskwood\PassiveDoodads\CoffinLid\CoffinLid.mdx` | `0x80` | 2.0 | 0.0 |

Scale and seat height are starting guesses and are tunable in the display row alone, without
recompiling or re-extracting. Both are **world doodads**, not shields: a doodad M2 carries its own
texture path and is authored on the ground plane, so it draws textured and flat (§2.5). Either path
can be replaced at build time with `--model ALIAS=PATH`, and `--survey HINT` searches the client for
one - a surfboard, a plank, anything board-shaped - over the model table, a `listfile.txt` and the
client's own archives.

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
| deck | `.board on` | the deck renders under the player, standing rider (`Deck.Render = unit`) |
| activation | bind a key to `Skateboard` in the stock Key Bindings frame, press it | boards, and a second press leaves; the same action appears in `/cp` for a pad button; `/say .board toggle` works with the add-on removed from the folder (a bare `/say .board` prints the subcommand list) |
| momentum | ride downhill in Elwynn, then uphill | `v` climbs without input on the slope, bleeds uphill, coasts down to `Cruise` in ~8 s and never below it; `Debug` line shows `v`, slope and rate agreeing |
| cap | 10-minute ride, `Debug` on | zero `Incorrect speed` kicks, `pendingAcks` never stuck, the rate never above the target the module holds (the cap only lowers) |
| carry | board on flat ground and release every key | the rider travels along their own facing, in arcs of `Drive.ImpulseMs - Drive.GroundMs` with a standing gap between them; `Drive.ImpulseMs` 250-500 matches the impulse's own duration without a stutter; the deck stays under the feet instead of catching up, and no trick animation or `LandingBonus` fires during the glide |
| carry on a hill | ride a rise, then the same slope downhill, with `Drive.Mode = 0` | the rider holds `v` up the rise instead of standing at 0 yd/s, the arc lands back on the slope, `Debug` shows `grade` following the ground, and a descent steeper than the arc can follow is one glide with no take-off or `LandingBonus` |
| deck follow | ride at `MaxSpeed`, then stop dead, then start again | the deck sits under the feet at speed instead of trailing (within a yard at `MaxSpeed`, and it holds through the carry's own standing gap), no segment is relaunched before the last one finished (that was the stutter), it stops without walking on ahead of a parked rider, and after a standing start it closes the half-second it cannot see in under a second rather than snapping |
| deck follow, debug | the same, with `Skateboard.Debug = 1` | `ground` tracks `v` during a carry and drops to 0 when the rider stands, and no `NearTeleportTo` fires while riding (a snap every segment means the projection is wrong, not the packet rate) |
| carry on a hill, spline | the same slope with `Drive.Mode = 1` | the path follows the hill and each segment lasts `Drive.SplineMs` rather than overrunning on a climb; the rider turns between segments (not during them), and no jump or trick starts while a segment runs |
| brake | at 15+ yd/s, hold backward | `v` falls to 0 in a couple of seconds and the cap follows; no `Incorrect speed` kick from the backward walk |
| tricks | jump while boarded (the `Drive.GroundMs` gap is the window) | one clip per take-off, clean landing adds `v`, mistimed landing crashes once |
| cleanup | logout, die, change map, `.board off` | no `UNIT_FIELD_MOUNTDISPLAYID` left, no state left in the map |
| bar | click the add-on's board button, then its push button | boards and pushes the same as the bindings, and the first button becomes `Bail` while boarded; a trick slot the module has not advertised is hidden rather than failing |
| HUD | ride boarded with `Debug` on | the readout's `cap` matches the `Debug` line's `v` (it comes from the status poll, so it lags a poll); its speed half is `GetUnitSpeed`, which reads 0 at a standstill, so the two numbers together say whether the deck is moving |
| takeover (toggle) | board out of combat with the takeover on | the player's own action bar keys do board actions, and every original binding is restored on leave, on reload and on the next login after a crash |
| deck rows | step 4 on the device | both decks render textured and flat, `--verify` finds all three members in one archive, the server boots with the patched DBCs |

## 6. Risks and unknowns

| Risk | Handle |
|---|---|
| A mount display without `UNIT_FLAG_MOUNT` may not render | the first boot answers it; mode 1 is the fallback (§2.5) |
| A lost speed ACK wedges the gate | 500 ms timeout (§2.4) |
| The deck draws white or on edge | answered on the device: an item model has no texture path of its own and is authored held. Ship a doodad model instead, and pick it with `--survey` / `--model` (§2.5) |
| The doodad's origin is not the deck's centre | scale and `MountHeight` are the only DBC knobs; a real offset needs a re-rooted M2 |
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
| 4 | deck rows | rename and extend the patch tool, both doodad rows, installers, server-side DBCs | §5 deck-row row: both decks on the device, `--verify` clean |
| 5 | docs | this plan's status line, design-doc status, README, RUNBOOK | — |

The vehicle route is out of the plan, not deferred to a step: revisiting it means new `Spell.dbc`
rows and a retarget of the physics (§2.9).

Step 1 without step 4 is the whole experiment: if a stock log under a carried rider does not feel
like skating, the decks do not matter, and step 4 is two DBC rows and a tool change away from being
irrelevant.
