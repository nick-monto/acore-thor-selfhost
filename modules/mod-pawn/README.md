# mod-pawn

Pawns: named bots you recruit at any innkeeper, keep in your party, and level alongside. The
design is in [docs/PAWN-SYSTEM-DESIGN.md](../../docs/PAWN-SYSTEM-DESIGN.md) and the integration plan
in [docs/PAWN-INTEGRATION-PLAN.md](../../docs/PAWN-INTEGRATION-PLAN.md).

A pawn is a **type-2 "AddClass" character** from the playerbots pool — a real character on a real
account, in a real group. Everything that makes that work already exists upstream: the character
keeps its gear and saves itself. It does **not** gain XP — a pawn is pinned to the level it was hired
at (see "A pawn is what you hired" below). This module adds the four things the pool does not have:

1. **A roster** (`pawn_roster`, characters database, created at startup by
   `PawnMgr::EnsureSchema`) so a recruited pawn comes back after a relog and after a world-server
   restart. A dismissed row is kept, so re-recruiting the same pawn gives the same pawn. The table
   is created in code rather than by module SQL because a failing module SQL file aborts the boot
   on this fork (`docs/SERVER-STACK.md` §13).
2. **A doorway**: one extra gossip line on every innkeeper, with a paged picker. Recruit,
   dismiss, and the party cap all live there. The picker shows a random 10-15 of the pool per
   visit, not the whole pool — see below.
3. **A level window** (`Pawn.LevelWindow`, default ±2). Each pool pawn gets a stable level inside
   the window, derived from the player and pawn guids, so offers vary instead of all snapping to the
   player's exact level. The same window is what makes a pawn *current*: once the player is more
   than that many levels ahead of it, the party row at the inn says it has fallen behind.
4. **An archetype**: one premade PvE spec plus a hard blacklist of the other specs' signature
   abilities. A prot warrior pawn without Shield Wall cannot tank; a shadow priest pawn without
   Resurrection cannot heal. The toolkit is the reason to swap. On top of that, each pawn is dealt a
   *variant* of its own spec's situational abilities, so two fury warriors are different companions —
   see "Two pawns of one spec" below.
5. **Gear for that spec at its level.** The spec is pinned *before* the equipment pass, the AI's
   strategies are rebuilt from it, and the glyphs are re-rolled against its tab, because playerbots
   scores every candidate item against the bot's active strategy (`TankWarriorStrategy` carries
   `STRATEGY_TYPE_TANK`) — a pawn equipped first and pinned second ends up a prot warrior in a fury
   two-hander with no shield. The pass is
   `PlayerbotFactory::AutoGear`'s sequence (equip, ammo, enchants and gems, durability) with the
   gear-score cap taken from the player, so a pawn is never better geared than its owner. It runs
   once, recorded by the roster's `built` column: after that the pawn keeps what it wears, and the
   pass only runs again if the level it was hired at has to be restored.

## Two pawns of one spec

The blacklist makes a pawn a specialist; the variant pool makes two specialists of the same spec
different. `Pawn.Variant.<class>.<spec_no>` is a short list of the spec's *situational* abilities —
interrupts, taunts, cooldowns, dispels, crowd control — and each pawn is dealt a subset of it:
`PawnMgr::WithheldVariants` draws a count uniformly from `0..Pawn.VariantWithholdMax` (default 2),
capped by the pool size, and picks that many entries by the lowest hash of their spell id, all from
the same seed as the pawn's level, spec and gear ratio.

So the same face always arrives with the same toolkit for the same player, and two fury warriors do
not: with the shipped pool (`Pummel, Recklessness, Death Wish`) and a cap of 2, a pawn is missing
none, one or two of them about a third of the time each — seven distinct kits over five thousand
pawn pairs, and every one of them a fury warrior that fights a little differently.

The withheld set is enforced in the same place as the blacklist — `PlayerScript::OnPlayerCanLearnSpell`
through `PawnMgr::IsSpellForbidden` — so no learn path, factory or trainer, can undo it, and
`PawnArchetype::Apply` prunes the ones a rebuild would otherwise leave in the book. The picker row
says what the pawn lacks (`no Pummel/Recklessness`), because that is part of what the player is
choosing between.

Nothing rotational is ever in a pool: a pawn missing its rotation is broken, not different. Nor are
capstone talents — the pools ship trainer abilities and small talents only.

## A pawn is what you hired

A pawn does not level with its owner. It is pinned to the level it was offered at, and the pin is
enforced two ways:

- **No XP.** With `Pawn.GainXP = 0` (the default) the pawn carries the core's
  `PLAYER_FLAGS_NO_XP_GAIN` player flag. `Player::GiveXP` checks it before anything else, and every
  XP source in the core funnels through that one call — kill credit (`KillRewarder.cpp:185`), quests
  (`PlayerQuest.cpp:773`), exploration (`Player.cpp:6000`) and battlegrounds (`Player.cpp:6407`) —
  so there is no path around it. The flag lives on the character and is saved with it.
- **A level pin.** playerbots rebuilds a type-2 bot at its master's exact level on login when the gap
  is more than three (`PlayerbotMgr.cpp:640`), so the module applies the pin *after* that pass: the
  login drain runs on the world thread from `WorldScript::OnUpdate`, later than the bot's own login
  hook. A pawn the snap moved is rebuilt at the level recorded in `pawn_roster.target_level`.

`Pawn.GainXP = 1` turns the whole thing off: the pawn levels in the party, the roster follows its
level instead of holding it, and exchanging becomes a choice rather than something progression
forces.

So as the player levels, a pawn falls behind by design. That is what the roster is for: the inn is
where you exchange it. A pawn more than `Pawn.LevelWindow` levels behind is marked `fallen behind` in
the party row, and hiring a level-appropriate replacement costs a fee — the same fee whether you
pick a new face or the same one, because taking the same pawn on at a new level is a new build with
new gear. Re-hiring at the level it is already standing at stays free.

## The hire fee

The **baseline** is the player's own level cubed, in copper (`Pawn.HireFeePercent`, default 100):
that is what a pawn geared exactly like the player costs, and each pawn already in the party adds
that much again (`Pawn.HireFeeHeldPercent`, default 100 — so the second pawn is twice the price).
Level cubed is calibrated against this server's own money supply, the loot-bag table in
`scripts/loot-bags/gen_loot_bags.py`: 80s at level 20, 2g 70s at 30, 6g 40s at 40, 21g 60s at 60,
51g 20s at 80 — ten to twenty minutes of ordinary farming at every level that matters, so the work
asked for stays constant instead of evaporating at 80 or being impossible at 10.

The **gear ratio** is what moves a pawn off the baseline. It is the pawn's strength in percent of the
player's gear score, and it is both the price multiplier and the ceiling the equipment pass is held
to (`GetMixedGearScore(owner) * ratio / 100`, per item). It is drawn per pawn from the same seed as
its level and spec: triangular, with its peak exactly at 100, over
`[Pawn.GearFloorPercent, Pawn.GearCapPercent]` (defaults 85 and 150). So a pawn geared like the player
costs the baseline, one a seventh weaker costs six sevenths of it, one half again as strong costs half
again as much, and it really does arrive in that gear — the cap is a per-item score ceiling, and
playerbots picks from the ten levels below the pawn at the best quality it can find, so the cap is
what decides which candidates pass.

The result is a band that does not move with level: a draw costs between 0.85x and 1.5x the baseline,
whatever the player's level, with a median exactly at the baseline and about two thirds of the pawns
within ±10% of it. The pawn's own level is deliberately not in the price — inside the level window it
moves power less than the gear does, and pricing it made a level-20 draw twice as wide as a level-80
one.

The picker row says which way each pawn went — `+20% gear`, `-11% gear` — beside the price. Gear score
itself stays invisible, as it is in the vanilla client, and the price is the number the player acts
on.

The price is on the picker row and in a confirmation box. The charge itself happens inside
`PawnMgr::Recruit`, after every check has passed, and not through the gossip box's money: the core
deducts box money *after* the script hook and unconditionally, so a recruit refused for a full party
would still be paid for, and an empty purse would still hire. A pawn this owner has already paid for
at the level it is standing at is free to call back (the roster's `paid` column), so dismissing and
re-hiring to rearrange a party costs nothing. A hire at a *new* level is a new fee: the fee buys a
companion at that level, and a pawn you have outgrown is a different companion.

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
login pass — the pass that would otherwise rebuild the pawn at its owner's level.

## Configuration

`conf/mod-pawn.conf.dist`: `Pawn.Enable`, `Pawn.MaxPawns`, `Pawn.LevelWindow`, `Pawn.GainXP`,
`Pawn.HireFeePercent`, `Pawn.HireFeeHeldPercent`, `Pawn.GearFloorPercent`, `Pawn.GearCapPercent`,
`Pawn.OfferedPerPage`, `Pawn.FeaturedMin`/`FeaturedMax`,
`Pawn.PoolRefreshMinMinutes`/`PoolRefreshMaxMinutes`, `Pawn.Debug`, and one
`Pawn.Blacklist.<class>.<spec_no>` list per archetype. Spell names in the blacklist are
resolved once at startup; an unresolved name is logged as an error rather than ignored.

## The pool and the featured draw

The pool is every offline character on a type-2 AddClass account — about 150 with the installer's
`Playerbots.AddClassAccountPoolSize = 15`, both teams, every class. The module re-reads it at
startup, on a random 40-60 minute timer, and on demand if it is still empty (a boot can finish
before playerbots has created the pool characters).

Each time the picker opens it takes a random count in `[FeaturedMin, FeaturedMax]` (10-15) from the
pool, restricted to the player's team and to candidates that are neither in the party nor online.

It **deals rather than samples**: `PawnMgr::DrawOffers` keeps a per-player deck — the eligible pool in
a shuffled order, with a cursor — and hands out the next rows off the top. No face comes round again
until the deck has been dealt out; a pool refresh reshuffles it, and the deck is dropped at logout.

Two rules keep visits from blurring together. A visit never straddles a reshuffle — if the deck
cannot cover the whole visit it is reshuffled first, and the tail it skips goes back into the next
shuffle. And the faces the previous visit showed are dealt *last* in a new order, so the one moment a
repeat could land in the very next visit does not. A dealt counter that has stopped being a candidate
(recruited, logged in, dropped by a refresh) is skipped when it comes up, so the deck heals itself.

On a pool of ~75 eligible characters that is a face coming back after about six visits, where a
straight sample of 12 from 75 repeats two or three faces on every visit.

The result is kept for the menu instance, so paging and dismissing do not move rows under the
pointer; recruiting removes the row; closing and reopening deals again. Level, spec and gear ratio
stay a pure function of the two guids, so a face you have seen keeps its level and spec whenever it
comes back.

## Why there is no SQL

The line is a `CreatureScript` hook gated on `UNIT_NPC_FLAG_INNKEEPER`, so it needs no template row,
no spawn and no `ScriptName`: it exists wherever an innkeeper does, and nowhere else. The roster
table is created in code for the reason in item 1 above. The module ships no `data/sql` tree at all.
