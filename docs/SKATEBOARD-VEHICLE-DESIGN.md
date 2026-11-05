# A deck that is a vehicle

## Why

Three carriers have now been played, and each one loses the same thing the others gain:

| carrier | moves the rider with | steering | jitter |
|---|---|---|---|
| `Drive = 0` (speed cap) | the client's own walk, capped | full | none of its own |
| `Drive = 1`, `Mode = 0` (knockback) | a jump arc every `ImpulseMs` | between impulses | none, but every impulse is a hop |
| `Drive = 1`, `Mode = 1` (spline) | a server polyline | only in the `GroundMs` window | the client does not blend segments |

The cap is the only one that steers without a hop, and it is not a carrier: the player walks, the
deck follows, and nothing on the deck changes how it feels. The other two move the rider, and both
take the steering away for the reason `MovementHandler.cpp:543` documents: while a player's spline
is unfinalized, `VerifyMovementInfo` drops every movement packet - position *and* facing - so the
server can neither accept a turn nor learn that one happened. A window (`Drive.GroundMs`) narrows
that to a stutter; it cannot remove it.

A vehicle moves the problem instead of fighting it: the *client* drives, so the steering is the
client's own movement path, and the deck and the rider are one object at the client's frame rate -
no follow logic, no spline, no teleport onto the board.

## The mechanism in this core

- `Creature::Create` builds a vehicle kit from `creature_template.VehicleId`
  (`ObjectMgr.cpp:655`, `Creature.cpp:1677`); a module can do the same at runtime with
  `Unit::CreateVehicleKit(id, entry)` (`Unit.h:1932`), which is what the
  `SPELL_AURA_SET_VEHICLE_ID` aura does (`SpellAuraEffects.cpp:5907`).
- A seat whose `VehicleSeat.dbc` row carries `VEHICLE_SEAT_FLAG_CAN_CONTROL` (`0x00000800`,
  `DBCEnums.h:455`) makes the passenger the driver: `Vehicle::AddPassenger` calls
  `_me->SetCharmedBy(player, CHARM_TYPE_VEHICLE)` (`Vehicle.cpp:427`), so the client controls the
  vehicle as a charmed unit and the vehicle becomes the session's mover - `HandleMovementOpcodes`
  reads `mover = _player->m_mover` (`MovementHandler.cpp:366`) and the speed and anti-cheat checks
  run against *that* mover.
- Because the vehicle is the mover, the deck speed cap already built for the player applies to it
  unchanged: `deck->SetSpeed(MOVE_RUN, v / BASE_RUN_RATE, true)` is the same cap, the same ACK gate
  and the same slope/friction integrator - the client simply gets to steer inside it.
- Passengers are attached from the seat's DBC offset
  (`VehicleSeatEntry::m_attachmentOffsetX/Y/Z`) into `m_movementInfo.transport`
  (`Vehicle.cpp:412-425`); the rider's place on the deck is therefore a data question, not a code
  one. `vehicle_seat_addon` can override the seat's *orientation* and exit parameters only
  (`ObjectMgr.cpp:4160`), not the offset.
- Missing `Vehicle.dbc` rows are a client crash: `ObjectMgr.cpp:1172` logs "This *WILL* cause the
  client to freeze!" for a `VehicleId` the store does not have. `Unit::CreateVehicleKit` returns
  false instead (`Unit.cpp:14616`), so the module can refuse to board rather than freeze anyone.

## What it would need

| piece | where | state |
|---|---|---|
| a `Vehicle.dbc` row with one `CAN_CONTROL` seat | client archive + server `data/dbc` | to author |
| a `VehicleSeat.dbc` row at offset (0, 0, ~0.1) with a standing ride animation | client archive + server `data/dbc` | to author |
| `CreateVehicleKit` at deck spawn, `EnterVehicle` to seat, `ExitVehicle` to leave | `mod-skateboard` | ~100 lines behind a key |
| the cap/slope integrator re-published on the vehicle instead of the player | `mod-skateboard` | reuses `SetSpeed` |
| drop the follow/spline/carrier code for the vehicle render | `mod-skateboard` | deletion |

The DBC route already exists: `scripts/skateboard/patch_deck_dbc.py` appends
`CreatureDisplayInfo`/`CreatureModelData` rows, writes both the server's copies and a client patch
archive through `scripts/lib/dbc_mpq.py`. A `Vehicle.dbc`/`VehicleSeat.dbc` patcher is the same
shape, and `scripts/item-dbc/patch_item_dbc.py` is the precedent for shipping a client patch from
the installer.

## What it fixes, what it costs

| fixes | costs |
|---|---|
| steering: the client moves, the server caps | the jump key does nothing on a vehicle - tricks would move to the add-on's push/brake messages, which already exist |
| segment jitter: there are no segments | the vehicle action bar replaces the rider's, so no casting or attacking while riding (3.3.5 rule) |
| teleport-onto-player and resummon pops: the rider is a seat, not a follow | enter/exit animations come from the seat row and must be chosen for standing on a board, not for sitting |
| one object: the deck *is* the vehicle, the rider is attached to it | a client patch (two DBC rows) that must be installed before any vehicle deck spawns |

## Staged plan

1. **Borrow a stock vehicle (no code, no patch).** `Skateboard.Vehicle.Probe = 1` lists every
   `Vehicle.dbc` row with a `CAN_CONTROL` seat, the seat's attachment offset, its ride animation and
   a creature the world DB spawns for it. Spawn that creature, spell-click it, drive it. This is the
   whole question: does the 3.3.5 client's own vehicle driving feel right for a deck? If it does
   not, nothing below is worth doing.
2. **Borrow a vehicle in the module.** `Skateboard.Vehicle = <id>`: `CreateVehicleKit` on the deck
   creature at spawn, `EnterVehicle` instead of the aura, the cap on the vehicle, `ExitVehicle` on
   the existing dismount paths. The rider sits at the borrowed seat's offset - visibly off the deck,
   but every other property is the real thing.
3. **Author the seat.** Add the `Vehicle.dbc`/`VehicleSeat.dbc` rows for the decks (offset 0, standing
   ride animation, `CAN_CONTROL | CAN_ENTER_OR_EXIT`), ship the archive, and the rider stands on the
   board.
4. **Retire the carriers.** With a vehicle deck proven, `Skateboard.Drive*` and the whole
   follow/spline path are dead weight and go.

## Open questions (need a client)

- The 3.3.5 vehicle camera: `Vehicle.dbc`'s `m_mouseLookOffsetPitch`, `cameraYawOffset` and the fade
  scalars were authored for vehicles, and a deck is not one. Whether a borrowed row's camera reads
  as a third-person deck camera or as a gunner seat is a judgement from play.
- The seat ride animation: whether any stock seat's `m_rideAnimStart` is a stand (a boat's deck seat)
  rather than a sit decides how readable stage 2 is.
- ConsolePort: it already knows about vehicle action bars, so it should handle the bar swap, but the
  R3 and modifier bindings need a pass.
- Anti-cheat: the vehicle is the mover, so the speed gate is the vehicle's speed - the ACK/gate code
  built for the player needs the same treatment against the vehicle's GUID.

## Decision

Worth it, staged: stage 1 is free and answers the only question that matters (does the client's own
vehicle driving feel right). The cap carrier is not the answer, the knockback hop is not, and the
spline cannot steer without a stutter - a vehicle is the only mechanism in 3.3.5 that keeps both the
client's steering and a single object under the rider.
