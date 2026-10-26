# mod-pawn

Pawns: named bots you recruit at any innkeeper, keep in your party, and level alongside. The
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
2. **A doorway**: one extra gossip line on every innkeeper, with a paged picker. Recruit,
   dismiss, and the party cap all live there. The picker shows a random 10-15 of the pool per
   visit, not the whole pool — see below.
3. **A level window** (`Pawn.LevelWindow`, default ±2). Each pool pawn gets a stable level inside
   the window, derived from the player and pawn guids. A pawn already inside the window keeps its
   level; one that has drifted outside it is re-levelled into it on its next login — so a pawn is
   never useless and never a clone of the player's level.
4. **An archetype**: one premade PvE spec plus a hard blacklist of the other specs' signature
   abilities. A prot warrior pawn without Shield Wall cannot tank; a shadow priest pawn without
   Resurrection cannot heal. The toolkit is the reason to swap.
5. **Gear for that spec at its level.** The spec is pinned *before* the equipment pass, the AI's
   strategies are rebuilt from it, and the glyphs are re-rolled against its tab, because playerbots
   scores every candidate item against the bot's active strategy (`TankWarriorStrategy` carries
   `STRATEGY_TYPE_TANK`) — a pawn equipped first and pinned second ends up a prot warrior in a fury
   two-hander with no shield. The pass is
   `PlayerbotFactory::AutoGear`'s sequence (equip, ammo, enchants and gems, durability) with the
   gear-score cap taken from the player, so a pawn is never better geared than its owner. It runs
   once, recorded by the roster's `built` column: after that the pawn keeps what it wears, and
   playerbots' own level-up maintenance keeps it current as it levels with you.

## The party cap

`Pawn.MaxPawns` (default 2) counts **pawns**, not group members. The installer sets
`Playerbots.AddClassCommand = 0`, which makes `.playerbots bot addclass` GM-only, so the innkeeper
line is the only doorway to a pool pawn. The picker therefore reads the pool itself — the type-2
accounts in `playerbots_account_type`, joined to `characters` — instead of
`sRandomPlayerbotMgr.addclassCache`, which is only filled when that command is on. Strangers met in the world are ordinary random bots, are not
counted, and are still the way to fill a five-player group — deliberately.

## Loot

Pawns never compete. The installer sets `Playerbots.LootNeedRollLevel = 0` (a bot's NEED becomes
PASS), `Playerbots.LootGreedRollLevel = 0` (GREED becomes PASS) and `Playerbots.FreeMethodLoot = 0`
(bots leave free-for-all corpses alone), so the player takes every drop under any loot method.

## How it works

| Piece | Where |
|---|---|
| Roster schema, I/O, offers, recruit/dismiss, the gear pass | `src/PawnMgr.cpp` |
| Spec pinning and the spellbook blacklist | `src/PawnArchetype.cpp` |
| The innkeeper line and the gossip menu | `src/PawnInnkeeper.cpp` |
| Login queue and the learn-spell gate | `src/PawnScripts.cpp` |
| Loader symbol `Addmod_pawnScripts` | `src/pawn_loader.cpp` |

Two hooks carry the behaviour. `PlayerScript::OnPlayerCanLearnSpell` refuses a blacklisted spell at
the top of `Player::learnSpell`, so no learn path — factory, trainer, level-up maintenance, quest —
can undo a restriction. `PlayerScript::OnLogin` only *marks* a pawn; `WorldScript::OnUpdate` applies
its level and archetype once it is in the world, which is what puts that work after playerbots' own
login re-level pass.

## Configuration

`conf/mod-pawn.conf.dist`: `Pawn.Enable`, `Pawn.MaxPawns`, `Pawn.LevelWindow`, `Pawn.OfferedPerPage`,
`Pawn.FeaturedMin`/`FeaturedMax`, `Pawn.PoolRefreshMinMinutes`/`PoolRefreshMaxMinutes`, `Pawn.Debug`,
and one `Pawn.Blacklist.<class>.<spec_no>` list per archetype. Spell names in the blacklist are
resolved once at startup; an unresolved name is logged as an error rather than ignored.

## The pool and the featured draw

The pool is every offline character on a type-2 AddClass account — about 150 with the installer's
`Playerbots.AddClassAccountPoolSize = 15`, both teams, every class. The module re-reads it at
startup, on a random 40-60 minute timer, and on demand if it is still empty (a boot can finish
before playerbots has created the pool characters).

Each time the picker opens it draws a random count in `[FeaturedMin, FeaturedMax]` (10-15) from the
pool with a seeded shuffle, restricted to the player's team and to candidates that are neither in
the party nor online. That draw is kept for the menu instance, so paging and dismissing do not move
rows under the pointer; recruiting removes the row; closing and reopening draws again. Level and
spec stay a pure function of the two guids, so a face you have seen keeps its level and spec
whenever it comes back.

## Why there is no SQL

The line is a `CreatureScript` hook gated on `UNIT_NPC_FLAG_INNKEEPER`, so it needs no template row,
no spawn and no `ScriptName`: it exists wherever an innkeeper does, and nowhere else. The roster
table is created in code for the reason in item 1 above. The module ships no `data/sql` tree at all.
