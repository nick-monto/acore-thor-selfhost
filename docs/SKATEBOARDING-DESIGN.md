# Skateboarding: a board you ride, with momentum and tricks

Companion to [`SERVER-STACK.md`](SERVER-STACK.md). The question: can this stack carry a
momentum-based board mode — rolling, accelerating downhill, popping off ramps, landing tricks —
using assets the client already has? Feasibility answer first, design second.

The build plan for the first slice — the module, the two shield decks, the packaging and the
acceptance check — is [`SKATEBOARD-FIRST-PASS.md`](SKATEBOARD-FIRST-PASS.md).

Confidence: **[V]** verified here from source, artifact or API, **[R]** reported,
**[I]** inference.

---

## 1. Answer in one table

| Layer | Feasible today? | What it needs | Where the limit is |
|---|---|---|---|
| **Board under the feet** | **yes**, stock display | module call to `Unit::Mount(26609)` — the quest's `Log Ride` log; no client data, no SQL (§4.1) | rider keeps the client's mounted pose (§4) |
| **Momentum (accel, friction, downhill, top speed)** | **yes, as an arcade approximation** | module: a velocity integrator on `PlayerScript::OnPlayerUpdate` + `MovementHandlerScript::OnPlayerMove`, pushing `Unit::SetSpeed(MOVE_RUN, …)` | the client is the mover; the server shapes a **cap**, it cannot push the player (§3) |
| **Ramps / ollies / airtime** | **yes** | `Unit::KnockbackFrom` arc (client plays it), or a `MoveJump` spline (authoritative, input locked) | knockback is client-interruptible; spline velocity is clamped to `max(28, 4×run)` `MoveSplineInit.cpp:106` |
| **Tricks** | **yes**, as animation + FX + timing | `Unit::HandleEmoteCommand` with stock trick emotes, `Unit::SendPlaySpellVisual`, fall-flag landing window | animation is a canned client clip; no skeletal manipulation (§5) |
| **True physics (impulse from input, wall bounce, lateral drift, spin)** | **no** | client executable change | out of scope for an MPQ-only client (§6) |

The honest summary: **the board, the tricks and a momentum *feel* are affordable; physics is not.**
Nothing here needs SQL and nothing in phases 0-1 needs a client patch.

## 2. What the stack already gives us

| Fact | Evidence |
|---|---|
| Every `MSG_MOVE_*` lands in one handler, and it accepts the client's position wholesale: the mover must be the session's mover, then `mover->UpdatePosition(movementInfo.pos)` and `mover->m_movementInfo = movementInfo` | `Handlers/MovementHandler.cpp:362` (handler), `:366` (`mover`), `:434-435` (apply) [V] |
| **There is no distance-per-interval speed check in this build.** The only hook that could reject a movement packet is `AnticheatCheckMovementInfo`, and no installed module implements it (the self-host modules are creature-gear, e-interact, hardcore, mercenaries, skateboard, survivor) | `ScriptDefines/PlayerScript.h:800`, call site `MovementHandler.cpp:575-579` [V] |
| Speed is **server state**: `GetSpeed` derives from `m_speed_rate × base speed` (walk 2.5, run 7.0, run_back 4.5) | `Unit.cpp:11062-11064`, `Unit.cpp:93-100` [V] |
| The client must honour the server's speed: it ACKs every change, and the ACK is compared with a **0.01 rate tolerance**; reporting higher than the server kicks the player with `"Incorrect speed"`, lower re-forces the server value | `MovementHandler.cpp:763` (tolerance), `:769` (re-force), `:775` (kick) [V] |
| A speed change is a single packet with an ACK counter, and only the last outstanding change is compared | `Unit::SendSpeedToController` `Unit.cpp:11117-11129`, coalescing `MovementHandler.cpp:757-762` [V] |
| The player tick is **10 ms** (`MapUpdateInterval`, default 10) | `worldserver.conf.dist:1323-1327`, `World.cpp:205`, hook site `PlayerUpdates.cpp:314` [V] |
| Every movement packet is visible to a module, with `MovementInfo` (flags, position, jump fields) | `MovementHandlerScript::OnPlayerMove` `ScriptDefines/MovementHandlerScript.h:38`, fired `MovementHandler.cpp:658-661` [V] |
| Ground height is queryable at any point; **slope is not** — it has to be derived from samples | `Map::GetHeight` `Maps/Map.cpp:1188`, `GetGridHeight` `:1222`; terrain surface exposes only area/height/liquid `Grids/GridTerrainData.h:248-252` [V] |
| A board-shaped visual exists in stock data: the `Log Ride` quest prop is a wooden log built to be ridden (display `26609`/`26610`), and the flying carpets (`25460 → 28082`, `32567 → 28117`, `32634 → 28063`) are flat cloth decks — creature → display from `creature_template_model.sql`, model path from `CreatureModelData.dbc` (§4.1) | `data/sql/base/db_world/creature_template.sql`; the pinned client `dbc/` [V] |
| Display-only mount needs one field write plus one flag: `UNIT_FIELD_MOUNTDISPLAYID` + `UNIT_FLAG_MOUNT` | `Unit::Mount` `Unit.cpp:10262-10267`, field `UpdateFields.h:126` [V] |

Two consequences shape everything below.

**The client is the engine.** It receives a cap, not a force. Holding forward at 21 yd/s and
letting go produce exactly the same deceleration (none), which is why the mode is arcade: the
board cannot move a player who is not pressing forward.

**The client's speed report is trusted** on this install, so the cap channel is safe to drive
from a timer. That is not a licence to change it every tick — `MovementHandler.cpp:763-776` kicks
on a *higher* report than the server currently holds, so **lowering** the cap while an ACK for a
higher one is in flight is the one dangerous operation (§9).

## 3. The momentum model that can be built

One scalar per player: `v`, the deck's speed in yd/s, integrated on the 10 ms tick and clamped to
`[0, vMax]`. The module publishes `v` as a speed cap, `SetSpeed(MOVE_RUN, v / 7.0f, true)` (7.0 is
the base run rate, `Unit.cpp:93-100`). Input can never exceed it, so `v` *is* the board's momentum
from the player's point of view.

| Term | Rule | Why it reads right |
|---|---|---|
| Slope | sample `GetGridHeight` 2 yd ahead and behind along the last movement heading; `a = -g · (dz/ds)` | downhill keeps speed without input, uphill bleeds it — needs no input data |
| Rolling friction | `a -= c0 + c1·v²` with `c0 ≈ 1.5`, `c1 ≈ 0.004` | a long, believable coast to a stop; `v²` makes the loss felt only at speed |
| Push | a command/spell impulse `+4 yd/s`, capped | one "kick" per second, not a hold-to-accelerate |
| Airtime | while `MOVEMENTFLAG_FALLING` is set, no friction, no slope; `v` frozen | air keeps speed, so ramp landings carry it |
| Landing | clean window from airtime vs the trick that was played | a mistimed trick costs `v` and health (§5) |
| Top speed | `vMax ≈ 21` yd/s (3× run), configurable | above that the client's own interpolation starts to look like teleporting |

Cap publication is rate-limited by ACKs rather than by the tick: the module counts
`SMSG_FORCE_RUN_SPEED_CHANGE` sends and the matching `CMSG_FORCE_RUN_SPEED_CHANGE_ACK` (both
visible through `ServerScript::CanPacketSend` / `CanPacketReceive`, `ScriptDefines/ServerScript.h:65,75`),
never sends a second change while one is outstanding, and never lowers the cap while the last
change was an increase that has not been ACKed. On a self-hosted realm RTT is ~0-5 ms, so this
still yields tens of updates per second; over a WAN it would degrade to RTT cadence.

**What the model deliberately does not have** — each is a limitation, not an oversight:

- **No lateral momentum.** Speeds exist per move type only (walk, run, run_back, swim, turn,
  flight, pitch); there is no strafe rate to shape, so side-slip is unrepresentable.
  `Unit.h:653-664` [V].
- **No turning from the board.** Orientation comes from the client (`MovementHandler.cpp:435`);
  the server can only set facing inside a spline, which locks input
  (`VerifyMovementInfo` rejects movement while `!movespline->Finalized()`,
  `MovementHandler.cpp:543-547`) [V].
- **No input magnitude.** The server sees positions and flags, never the stick. `v` is decayed by
  friction, not by "the player let go", so the feel is a speed cap the player leans into.
- **No wall collisions.** Collision is client-side; the server only samples terrain height.

## 4. The board itself

### 4.1 What the client already has

The client DBCs are not in this repository: they are the `dbc/` half of the pinned
`wowgaming/client-data` release the installer downloads (`thor-install.sh:87`, `pins.toml:59`).
Three of its tables answer the asset question — `CreatureModelData.dbc` (model paths, scale,
collision), `CreatureDisplayInfo.dbc` (display → model) and `GameObjectDisplayInfo.dbc` (prop →
model) — and one row is a wooden log built to be ridden [V].

| Display | Model | Display scale | `MountHeight` | Creature that owns it | Reads as |
|---|---|---|---|---|---|
| **26609** | `Creature\StoneTreeLog\StoneTreeLog02_Mount.mdx` | 2.00 | 0 | `Log Ride (Log A)` (29878) | **a long wooden log you ride** — the quest prop, already mount-safe |
| **26610** | `Creature\StoneTreeLog\StoneTreeLog03_Mount.mdx` | 2.00 | 0 | `Log Ride (Log B)` (29879) | the same log, second variant |
| 21602 | `Creature\Tauren_MountedCanoe\Tauren_MountedCanoe.mdx` | 1.00 | 0.37 | `Tauren Canoe (Northrend)` (23518) | a hollow wooden canoe; the rider sits inside |
| 30846 / 30874 / 30852 | `RowBoat01_vehicle` / `DarkshoreBoat_vehicle` / `BE_Rowboat_vehicle` | 1.00 | — | the `Love Boat` rowing boats | a deck with a seat and oars |
| 16910 / 21939 | `Creature\Broom\BE_Broom01` / `Creature\BroomMount\BroomMount` | 1.00 | 0 / 0.03 | `Broom`, `Flying Broom` | a stick, not a deck — sanity check only |
| 27784 | `SC_Wagon` | 1.00 | 0 | `Scourge War Machine` | a cart bed |

`Log Ride` is the find: the model exists to be ridden while it moves, it is already registered as a
creature display, and its display scale is 2.0, so `Unit::Mount(26609)` puts a wooden plank-like
deck under the player with **no client data, no SQL and no new DBC rows**. Every row in the table is
usable the same way — the module only names an id.

### 4.2 Flatter than a log: two DBC rows, no new art

The literal plank props exist too, but only as *gameobject* models, and none of them is registered
as a creature model (checked: 0 of the 7 below appear in `CreatureModelData.dbc`), so a creature
display has to be created for one:

| Prop | Model path |
|---|---|
| Coffin lid | `World\Azeroth\Duskwood\PassiveDoodads\CoffinLid\CoffinLid.mdx` |
| Boardwalk deck | `World\Azeroth\RedRidge\PassiveDoodads\DockPieces\RedRidgeDocksBoardwalk01.mdx` |
| Wood planks 01 / 03 / 06 / 07 | `World\Generic\DarkIronDwarf\Passive Doodads\WoodPlanks\DarkIronWoodPlanks0N.mdx` |
| Excavation barrier plank | `World\Generic\Dwarf\Passive Doodads\ExcavationBarrierPlank\ExcavationBarrierPlank.mdx` |

That is one `CreatureModelData` row (path, collision, `MountHeight`) plus one
`CreatureDisplayInfo` row — an MPQ patch carrying **existing client art**, not new art. Two caveats:
a doodad M2 is static and has no mount-special animation, so a trick cannot hang off it (that needs
an authored model, §4.3); and the two `bootybayplank.wmo` / `pirateship_plank.wmo` props in the same
table are WMOs, which cannot be creature models at all.

**Shields and doors are the better flat art**, and the `Iron Coffin Lid` is a real shield in this
data: item `43843`, `ItemDisplayInfo` `56501` [V].

| Item | entry | `ItemDisplayInfo` | Model | Why it reads as a deck |
|---|---|---|---|---|
| **Iron Coffin Lid** | 43843 | 56501 | `Shield_Northrend_C_02.mdx` | a coffin lid is a board with a frame |
| Splintered Door of the Citadel | 49835 | 64882 | `shield_northrend_d_01.mdx` | a door: flat, panelled, wooden |
| Engraved Wall | 10363 | 26283 | `Shield_Rectangle_B_01.mdx` | the flattest of the lot — literally a wall |
| Large Wooden Shield | 1200 | 18663 | `Shield_Oval_A_01.mdx` | wide wooden oval |
| Wooden Shield | 2215 | 18670 | `Shield_Round_A_01.mdx` | round wooden board |

The route is the same two rows, and the client demonstrably accepts an item model as a creature
model: `CreatureModelData` already carries `ITEM\OBJECTCOMPONENTS\WEAPON\Sword_2H_Deathknight_B_01_Q.mdx`
and its axe sibling as models 2820/2821, displayed by the `Runebladed Sword` and `Runebladed Axe`
creatures (displays 25398/25401) [V]. No `ITEM\OBJECTCOMPONENTS\SHIELD\` model is registered
today, so a shield deck needs a `CreatureModelData` row naming the full path, plus the
`CreatureDisplayInfo` row.

**Implemented:** `scripts/skateboard/patch_deck_dbc.py` writes both rows for `engravedwall` (90001,
`Shield_Rectangle_B_01.mdx`) and `coffinlid` (90002, `Shield_Northrend_C_02.mdx`) by cloning the
stock log's own rows - display 26609 → model 2920 - so only the id, the model path and the display
scale differ from a deck the client already draws a rider on. It ships the two patched DBCs to the
server's `data/dbc` and a patch MPQ to the client's `Data`, the same two halves as the loot-bag
`Item.dbc` patch. `Skateboard.Deck` ships as `engravedwall`; on a realm whose DBCs were not patched
the module warns once and uses the log instead. What the rows cannot fix is the model's *pose*: an
M2's rest orientation is in the model, not in the DBC, so whether a shield lies flat under the rider
or stands on edge is a device answer, and `--scale` / `--mount-height` are the only knobs. An item model's origin sits at its grip or attachment point — the middle
of the board — which is about where a deck wants its centre [I]; if it is off, only scale and
`MountHeight` are tunable in the DBC, so a shifted deck needs a re-rooted model.

Three caveats specific to shields. The item system itself cannot be used for this — the client
decides the attachment from the inventory type and the server only sends entry ids
(`PlayerStorage.cpp:2968`, `Unit.cpp:16848`), which is why the DBC registration is the route. The
skin variant is chosen by `ItemDisplayInfo`, not the model: `Shield_Northrend_C_02.mdx` sits beside
`Shield_Northrend_C_02Black`, so a creature registration renders whichever texture the M2 carries by
default — check on the device, and bake the wanted skin in only if it matters [I]. And like the
planks, a shield M2 is static: no mount-special animation, so no trick can live on it.

### 4.3 A real board is data, not code

The client attaches whatever `UNIT_FIELD_MOUNTDISPLAYID` names under the rider; the server ships
only the id (`UpdateFields.h:126`) [V]. For a purpose-built deck the rows are:

| Custom board needs | Evidence |
|---|---|
| `CreatureDisplayInfo.dbc` row (`Displayid`, `ModelId`, scale) | `DBCStructure.h:747-763` [V] |
| `CreatureModelData.dbc` row (flags, model path, scale, collision, **`MountHeight`** — the rider's seat height) | `DBCStructure.h:801-818` [V] |
| `.m2` + `.skin` + `.blp` in a patch MPQ | `scripts/item-dbc/patch_item_dbc.py:264` (`build_mpq(files)` is generic; stored single-unit blocks, no compression) [V] |
| server-side copies of both DBCs, because collision height for a mounted unit reads the mount display's `MountHeight` | `Unit.cpp:16790-16795`; `DataDir/dbc` load `DBCStores.cpp:265-269,294,297` [V] |
| a spell (or the module) naming the creature entry, if the stock mount spell route is used | `SpellAuraEffects.cpp:3368,3412` [V] |

**The rider pose is fixed by the client.** While `UNIT_FLAG_MOUNT` is set the player plays its own
riding animations, so the board cannot make the rider stand. Author the deck so that pose reads as
a skate stance — a longboard with the rider crouched, knees bent, is the same silhouette as the
log/carpet pose, which is why those displays are the right prototypes.

One interaction to decide early: `IsMounted()` is just `HasUnitFlag(UNIT_FLAG_MOUNT)`
(`Unit.h:1895`), and `UpdateSpeed` then takes the **mounted** branch, where any aura refresh
(`SpellAuraEffects.cpp:3817`, `Mount`/`Unmount` `Unit.cpp:10379`) recomputes the rate and wipes the
module's cap [V]. Either re-assert the cap on every tick where `GetSpeedRate(MOVE_RUN)` has
drifted by more than 0.01, or write `UNIT_FIELD_MOUNTDISPLAYID` **without** the flag and accept
whatever pose the client chooses [I]. Both are one-line decisions; the second needs a device check
before it is trusted.

## 5. Tricks

The server cannot pose a skeleton, but it can play any stock animation it likes — the
client→server emote path is restricted in `ChatHandler.cpp:671-673` (`"restrict to the only emotes
hardcoded in client"`), while the server-side `Unit::HandleEmoteCommand` sends `SMSG_EMOTE`
unrestricted (`ChatPackets.h:31`) [V]. The usable clips are in `SharedDefines.h:1893+`:

| Clip | Id | Reads as |
|---|---|---|
| `EMOTE_ONESHOT_MOUNT_SPECIAL` | 377 | the mount's own special animation — **the trick carrier** for a custom board M2 (§4) |
| `EMOTE_ONESHOT_JUMPSTART` / `JUMPLANDRUN` / `JUMPEND` | 399 / 394 / 395 | ollie, rolling jump, landing |
| `EMOTE_ONESHOT_STOMP` | 388 | stomp / grind kick |
| `EMOTE_STATE_DANCESPECIAL` / `ONESHOT_DANCESPECIAL` | 400 / 401 | looping board dance / one-shot flourish |
| `EMOTE_ONESHOT_LIFTOFF` / `LAND` | 254 / 293 | mount-like take-off and touchdown |
| `EMOTE_STATE_FLY_FALL` / `EMOTE_STATE_FALL` | 446 / 427 | air pose while `MOVEMENTFLAG_FALLING` |

Trails and impact FX are one call each: `Unit::SendPlaySpellVisual(id)` sends
`SMSG_PLAY_SPELL_VISUAL` to everyone in range (`Unit.cpp:14810-14816`) and modules already use it
(`mod-playerbots/src/Ai/Base/Actions/RtscAction.cpp:49`). The id must exist in the **client's**
`SpellVisualKit.dbc`; the server does not load that file, so a stock kit costs nothing and a custom
one costs a client DBC edit [V].

A trick is therefore a triple: an emote (visible), a visual kit (flair), and a **landing window**
derived from the fall time — `MOVEMENTFLAG_FALLING` set at take-off, cleared on landing
(`Object.h:328-334` carries the jump/fall fields through `m_movementInfo`) [V]. Clean landing adds
`v`; mistimed landing is the crash (§9).

## 6. What it cannot do

- **Physics.** The client integrates its own motion; nothing in the protocol lets the server apply
  a force to a player who is not pressing forward, and no lateral velocity exists to drift with
  (§3). A faithful skate model needs the client's movement code, which is not in this stack: the
  client is a third-party ARM64 port shipped as an executable, and the toolchain patches
  **data only** — `patch_item_dbc.py` writes MPQ archives, the launcher writes `realmlist.wtf`,
  `Config.wtf` and add-ons, and nothing else touches the client [V].
- **Wall bounces and grinds.** Grinding would be a *visual* fake along a rail-like
  gameobject path; collision response stays the client's.
- **Spin tricks.** The rider's facing is client-owned; a 360 is only possible as a baked animation
  in a custom model, not as a server-driven rotation.

## 7. Phases

| Phase | Content | Client data | Acceptance |
|---|---|---|---|
| **0 — proof** | module: `.board on/off/toggle`, stock deck display, `v` integrator with friction + slope + push, cap published on ACK cadence; add-on: the click bindings that activate it, the bar, and a `GetUnitSpeed` readout | none | on the Thor: roll downhill, feel the coast, stop when `v` hits zero; no `"Incorrect speed"` kick over a 10-minute ride; a bound key boards and leaves |
| **1 — tricks** | emote + visual kit per trick, landing window, crash (dismount + fall damage), the `.board trick` clips - shipped in the build plan's steps 1-3. Ramp gameobjects that call `KnockbackFrom` are not in those steps and remain unplanned | none | a jump lands clean or wipes out per timing; observers see the animation and the FX |
| **2 — the deck** | custom `.m2`/`.skin`/`.blp`, `CreatureDisplayInfo` + `CreatureModelData` rows, both packed through the existing MPQ writer (`patch_archive` generalised, the `block_count != 2` ownership test relaxed, `patch_item_dbc.py:354,450`) and installed server-side too | yes | the board renders under the rider on the device; the client boots with the patched DBC pair |
| **3 — physics** | not planned | — | needs client source |

Phase 0 is the whole experiment: if the speed-cap momentum does not *feel* like skating with a
log under the player, no amount of art fixes it.

## 8. Configuration and commands

All `Skateboard.*` keys read in `WorldScript::OnAfterConfigLoad`, so `.reload config` applies them:

| Key | Default | Meaning |
|---|---|---|
| `Skateboard.Enable` | 0 | whole feature |
| `Skateboard.Deck` | `log` | `log` is 26609, the stock `Log Ride (Log A)`; `engravedwall` and `coffinlid` are the two shield ids |
| `Skateboard.BoardCastMs` | 2000 | boarding delay; nothing is written until it completes, and the add-on's bar is driven by the module's own `c` push rather than by the status poll |
| `Skateboard.OffOnHit` | 1 | a hit from another unit takes the deck; the start of a fight does not, and neither does fall damage |
| `Skateboard.MaxSpeed` | 21.0 | yd/s ceiling |
| `Skateboard.PushImpulse` | 4.0 | yd/s per kick |
| `Skateboard.Friction.Static` / `.Rolling` | 1.5 / 0.004 | the `c0` / `c1` terms |
| `Skateboard.SlopeGain` | 9.0 | multiplier on the terrain gradient |
| `Skateboard.Crash.Enable` | 1 | mistimed landing costs health |
| `Skateboard.HardcoreSafe` | 1 | a crash can never kill a Hardcore character (§9) |

Commands: `.board on`, `.board off`, `.board toggle`, `.board status`, `.board speed` (print `v`),
`.board push [down|up]`, `.board trick <name|index>`, `.board tricks` (the list the add-on's menu is
built from).

Activation is a click binding in the launcher's add-on, not a console command: `SetBindingClick`
lists it in the stock Key Bindings frame, so a keyboard player binds a key there and a pad player
assigns a button through ConsolePort's `/cp` list without the add-on claiming a physical button.
`EInteract.lua:203-233` is the working precedent. The vehicle bar was costed as a way to get a
taking-over bar and rejected for now - its buttons are spells, so it needs `Spell.dbc` rows and it
moves the physics onto a vehicle creature. The cost table is in
[`SKATEBOARD-FIRST-PASS.md`](SKATEBOARD-FIRST-PASS.md) §2.9.

## 9. Edge cases and failure modes

| Case | Handling |
|---|---|
| Speed-ACK race | never lower the cap while an increase is un-ACKed; the comparison at `MovementHandler.cpp:763-776` kicks on a higher client report, not on a lower one |
| Already mounted, flying, in an instance/BG, in water | refuse `.board on`; force off on boarding a vehicle (`Vehicle::AddPassenger` takes the mover, `Vehicle.cpp:346`) - unless the board itself ever becomes one, see the plan's §2.9 |
| Being hit | `UnitScript::OnDamage` with a different unit as the attacker clears the deck and restores the run rate (`Skateboard.OffOnHit`): a 21 yd/s cap in combat is a free disengage. Starting a fight is not the same event - the deck survives it, and so do fall damage and a crash, whose attacker is the victim. `.board on` is refused while already in combat whatever the key says |
| Death, logout, map change | clear the deck state on `OnPlayerJustDied`, `OnBeforeLogout`, `OnMapChanged` |
| **Hardcore character crashes** | `mod-hardcore` turns any death into permadeath; with `Skateboard.HardcoreSafe = 1` a crash damage tick cannot bring health below 1 |
| Bots | playerbots are real `Player` objects; gate on a real session so no bot is ever "boarded" |
| Aura refresh wipes the cap | re-assert on drift (§4) |
| Player stands still at high `v` | friction still bleeds `v`; momentum is a cap, so nothing else is needed |

## 10. Verification plan

- **Server, no client**: log `v`, the slope term and the ACK cadence for a scripted ride (walk a
  downhill in Elwynn, push, coast); assert zero `"Incorrect speed"` kicks and that
  `GetSpeedRate(MOVE_RUN)` tracks the target within 0.01 between ACKs.
- **Device**: the log deck renders under the rider, the board turns with the camera, a ramp
  knockback arcs and a mistimed trick wipes out.
- **Phase 2**: client boots with the patched pair, the board loads, `--verify` finds the archive,
  and the server's `MountHeight` collision matches the DBC row.

## 11. Verdict

Feasible, and cheaper than it sounds: phases 0-1 are one module and no data — momentum as a speed
cap, ramps as knockback arcs, tricks as stock animations plus visual kits. It will feel like
*skating in spirit* rather than in physics, because the client remains the mover: no lateral
momentum, no server steering, no real collision response. The expensive part is the deck model, and
even that is only an MPQ patch through a writer this repository already has.

What would kill it: expecting the board to move a player who is not pressing forward (§3), or
expecting the rider to stand rather than sit in the client's mount pose (§4). Both are client
limitations, not module ones.
