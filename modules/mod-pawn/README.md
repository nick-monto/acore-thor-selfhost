# mod-pawn

Pawns: named bots you recruit at an obelisk in town, keep in your party, and level alongside. The
design is in [docs/PAWN-SYSTEM-DESIGN.md](../../docs/PAWN-SYSTEM-DESIGN.md) and the integration plan
in [docs/PAWN-INTEGRATION-PLAN.md](../../docs/PAWN-INTEGRATION-PLAN.md).

A pawn is a **type-2 "AddClass" character** from the playerbots pool — a real character on a real
account, in a real group. Everything that makes that work already exists upstream: the character
earns group XP at the normal rate, keeps its gear, and saves itself. This module adds the four
things the pool does not have:

1. **A roster** (`pawn_roster`, characters database, created at startup by
   `PawnMgr::EnsureSchema`) so a recruited pawn comes back after a relog and after a world-server
   restart. A dismissed row is kept, so re-recruiting the same pawn gives the same pawn. The table
   is created in code rather than by module SQL because a failing module SQL file aborts the boot
   on this fork (`docs/SERVER-STACK.md` §13).
2. **A doorway**: the obelisk gameobject (entry 5000010) with a paged gossip picker. Recruit,
   dismiss, and the party cap all live there.
3. **A level window** (`Pawn.LevelWindow`, default ±2). Each pool pawn gets a stable level inside
   the window, derived from the player and pawn guids, and is kept there on every login — so a pawn
   is never useless and never a clone of the player's level.
4. **An archetype**: one premade PvE spec plus a hard blacklist of the other specs' signature
   abilities. A prot warrior pawn without Shield Wall cannot tank; a shadow priest pawn without
   Resurrection cannot heal. The toolkit is the reason to swap.

## The party cap

`Pawn.MaxPawns` (default 2) counts **pawns**, not group members. The installer sets
`Playerbots.AddClassCommand = 0`, which makes `.playerbots bot addclass` GM-only, so the obelisk is
the only doorway to a pool pawn. Strangers met in the world are ordinary random bots, are not
counted, and are still the way to fill a five-player group — deliberately.

## Loot

Pawns never compete. The installer sets `Playerbots.LootNeedRollLevel = 0` (a bot's NEED becomes
PASS), `Playerbots.LootGreedRollLevel = 0` (GREED becomes PASS) and `Playerbots.FreeMethodLoot = 0`
(bots leave free-for-all corpses alone), so the player takes every drop under any loot method.

## How it works

| Piece | Where |
|---|---|
| Roster schema, I/O, offers, recruit/dismiss | `src/PawnMgr.cpp` |
| Spec pinning and the spellbook blacklist | `src/PawnArchetype.cpp` |
| The obelisk gossip menu | `src/PawnObelisk.cpp` |
| Login queue and the learn-spell gate | `src/PawnScripts.cpp` |
| Loader symbol `Addmod_pawnScripts` | `src/pawn_loader.cpp` |

Two hooks carry the behaviour. `PlayerScript::OnPlayerCanLearnSpell` refuses a blacklisted spell at
the top of `Player::learnSpell`, so no learn path — factory, trainer, level-up maintenance, quest —
can undo a restriction. `PlayerScript::OnLogin` only *marks* a pawn; `WorldScript::OnUpdate` applies
its level and archetype once it is in the world, which is what puts that work after playerbots' own
login re-level pass.

## Configuration

`conf/mod-pawn.conf.dist`: `Pawn.Enable`, `Pawn.MaxPawns`, `Pawn.LevelWindow`, `Pawn.OfferedPerPage`,
`Pawn.ObeliskEntry`, `Pawn.Debug`, and one `Pawn.Blacklist.<class>.<spec_no>` list per archetype.
Spell names in the blacklist are resolved once at startup; an unresolved name is logged as an error
rather than ignored.

## Moving the obelisk

The two spawns are rows in `data/sql/world/base/pawn_10_obelisk.sql` (Stormwind Trade District,
Orgrimmar Valley of Strength). The code reads `Pawn.ObeliskEntry`, never the spawn guids, so moving
an obelisk is a SQL edit and a restart.
