# mod-pawn

Pawns: named bots you recruit at any innkeeper, keep in your party, and level alongside. The
design is in [docs/PAWN-SYSTEM-DESIGN.md](../../docs/PAWN-SYSTEM-DESIGN.md) and the integration plan
in [docs/PAWN-INTEGRATION-PLAN.md](../../docs/PAWN-INTEGRATION-PLAN.md).

A pawn is a **type-2 "AddClass" character** from the playerbots pool — a real character on a real
account, in a real group. Everything that makes that work already exists upstream: the character
keeps its gear and saves itself. It does **not** gain XP — a pawn is pinned to the level it was hired
at (see "A pawn is what you hired" below). This module adds the six things the pool does not have:

1. **A roster** (`pawn_roster`, characters database, created at startup by
   `PawnMgr::EnsureSchema`) so a recruited pawn comes back after a relog and after a world-server
   restart. A dismissed row is kept, so re-recruiting the same pawn gives the same pawn. The table
   is created in code rather than by module SQL because a failing module SQL file aborts the boot
   on this fork (`docs/SERVER-STACK.md` §13).
2. **A doorway**: one extra gossip line on every innkeeper, with a paged picker. Recruiting and the
   party cap live there; a party row opens that pawn's own page, where its orders and its dismissal
   now live (see "The pawn page" below). The picker shows a random 10-15 of the pool per
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
6. **A temperament, a voice, and the vanity that comes with them.** Two pawns of one spec already
   differ in what they carry; they now differ in how they fight and how they talk, and the mount and
   pet each one ends up with are read back onto the roster. See "How they fight, and how they talk"
   and "What they ride" below.

Those six are the model. What a player does with a pawn afterwards — the page behind a party row and
the orders on it, the repair and restock, the retraining, and the affinity and voice lines a pawn
keeps by being kept — is the rest of this document: "The pawn page", "Upkeep: repair and restock",
"Retraining", "Affinity" and "What they say".

## Two pawns of one spec

The blacklist makes a pawn a specialist; the variant pool makes two specialists of the same spec
different. `Pawn.Variant.<class>.<spec_no>` is a short list of the spec's *situational* abilities —
interrupts, taunts, cooldowns, dispels, crowd control — and each pawn is dealt a subset of it:
`PawnMgr::WithheldVariants` draws a count uniformly from `Pawn.VariantWithholdMin` to
`Pawn.VariantWithholdMax` (default 1 to 2), capped by the pool size, and picks that many entries by
the lowest hash of their spell id, all from the same seed as the pawn's level, spec and gear ratio.

So the same face always arrives with the same toolkit for the same player, and two fury warriors do
not: with the shipped pool (`Pummel, Recklessness, Death Wish`) and a range of 1 to 2, a pawn is
missing one or two of them in equal measure — six distinct kits over five thousand pawn pairs, and
every one of them a fury warrior that fights a little differently. The floor is the point: with a
floor of 0 a third of all pawns would be missing nothing at all, which is the identical-twin case
the pools exist to avoid.

The withheld set is enforced in the same place as the blacklist — `PlayerScript::OnPlayerCanLearnSpell`
through `PawnMgr::IsSpellForbidden` — so no learn path, factory or trainer, can undo it, and
`PawnArchetype::Apply` prunes the ones a rebuild would otherwise leave in the book. The picker row
says what the pawn lacks (`no Pummel/Recklessness`), because that is part of what the player is
choosing between.

Nothing rotational is ever in a pool: a pawn missing its rotation is broken, not different. Nor are
capstone talents — the pools ship trainer abilities and small talents only.

Every shipped name was validated against the 3.3.5 spell table on 2026-10-26: 152 distinct names over
the 293 entries of the 31 blacklists and 31 variant pools, all resolving, none naming an ability only
another class can learn. That check is not part of the build, so a name added later is proven by the
`archetypes:` line in `Server.log` at the next startup.

## How they fight, and how they talk

The variant pool decides what a pawn carries. Two further draws decide how it behaves, and both come
from the same seed as its level, spec and gear ratio, so a face keeps them for as long as its owner
keeps the face.

**Temperament** is the combat half. `Pawn.Temperament.<class>.<spec_no>` is a pool of named profiles
per spec, and a profile is exactly what `PlayerbotAI::ChangeStrategy` takes: a label and a
comma-separated list of `+name` / `-name` tokens, such as the shipped
`Relentless: +aggressive,+close` against `Patient: +wait for attack`. Each pawn is dealt one profile
from its own spec's pool and it is applied to the combat engine, so two fury warriors now differ in
how they fight as well as in what they carry. The loader enumerates the keys the conf actually
defines rather than probing the class × spec grid, so a spec with no `Pawn.Temperament` key simply has
no temperament: nothing is rolled for it and boot stays quiet about it.

**Voice** is the non-combat half. It is one global pool, `Pawn.Voice`, because small talk is not a
class skill: `Talkative: +emote` against `Quiet: -emote`, applied to the non-combat engine. `emote`
is the strategy that drives a bot's speech and emotes; `chat` is chat-command handling — `follow`,
`stay`, trade — and is deliberately not a voice dial, because a quiet pawn must still obey.

A profile moves a pawn *inside* the server's own allowance, not around it: the global
`Playerbots.RandomBotTalk` and `Playerbots.RandomBotEmote` gates still decide whether random bots
talk at all, so `Talkative` on a server with speech switched off changes nothing.

Names are checked twice, for the reason `Engine::addStrategy` gives: it ignores an unknown one without
a word, so a typo would be a silent no-op. A malformed profile is dropped with an error when the
pools load, and a `+name` the bot does not actually have is reported once, the first time a pawn
draws it — one mistake shared by every pawn, not one line per login. The offline check
(`scripts/pawn/check_pawn.py`) reads the Strategy contexts only — the base, class, raid and dungeon
`Strategy` contexts of the playerbots tree — because a name is a dial only if `GetStrategy` resolves
it: a trigger, action or value of the same name fails that check, and would have been dropped without
a word at runtime.

The picker row names the pair it is offering. Vanity below does not, because it does not exist yet at
that point.

### Where the traits land

On a pawn's first build the traits land on the role defaults: `ResetStrategies` puts
`tank face` back on a tank, `behind` back on melee dps and `save mana` back on a healer, and the
pawn's own build calls it too, to score gear against the pinned spec. The profiles are therefore
applied at the *end* of `PawnMgr::ApplyPawn`, the one path every pawn login passes through: after the
build and after the last strategy rebuild. A trait applied earlier in the build would be gone before
the pawn ever fought.

From the second login that engine is no longer the role defaults. playerbots saves the strategy
list at logout and replays it on login (`PlayerbotMgr.cpp:572-574` → `PlayerbotRepository::Load`,
which clears the combat state and re-applies the saved string), so the trait lands on last session's
list carrying last session's trait. `PawnPersonality::Apply` therefore draws both profiles itself
and, through `ApplyPool`, undoes every dial that another profile of the *same pool* names and the
chosen one does not, before applying the chosen dials. Without that inverse pass, editing a pool
would never reach an already-hired pawn: its stored list would keep the old trait for good.

The limit is worth knowing: a dial that is deleted from *every* profile of a pool is no longer named
by the pool, so nothing undoes it. It survives until the pawn is re-hired or its stored strategy list
is cleared.

### What they say

`Pawn.Voice` decides how a pawn behaves between fights. `Pawn.VoiceLine.<label>` decides what it
actually says, and the label is the voice it drew, so a line block belongs to a voice rather than to
a spec or a class.

A value is `event: text` chunks separated by `|`, and the events are `kill`, `death` and `dismiss` —
the three moments the module sees for itself. `PawnPersonality::Load` enumerates the keys the conf
defines with `GetKeysByString` rather than probing a fixed set, so a voice with no line block is
silent rather than an error. A block whose label is not in `Pawn.Voice`, an event outside those three,
and an empty text are each logged and dropped at load. Text may not contain a `|`: the conf format
splits on it, so a line that needs one is not expressible.

The line is drawn from the `(owner, pawn)` seed salted with the event — the same seed as the pawn's
level, spec and temperament. A pawn therefore says the same thing at the same moment every time, and
its death line is not its kill line again. The global RNG is never consulted: a hundred other things
move it, and the draw has to be reproducible from the two guids alone.

`PawnPersonality::SayLine` resolves the pawn's voice and speaks through `bot->Say`, in the pawn's own
faction language (`LANG_COMMON` for Alliance, `LANG_ORCISH` for Horde) — the same choice playerbots'
own say action makes (`SayAction.cpp:139`), so the other faction does not read a pawn's line as
universal speech. Two scalars pace it:

- `Pawn.VoiceKillChancePercent` (default 10) applies to kill lines only: `urand(1, 100)` above the
  chance and the pawn says nothing, so `0` never speaks and `100` always does. A death or a farewell
  is rare enough to be said every time a line exists.
- `Pawn.VoiceCooldownSeconds` (default 120) is per pawn and per event, so a pawn pulled into a fast
  fight cannot repeat one line and its kill line does not silence its farewell. The elapsed time is an
  unsigned subtraction, so the cooldown survives the millisecond counter wrapping.

The three call sites are `OnPlayerCreatureKill` (a pawn landed the kill, event `kill`),
`OnPlayerJustDied` (event `death`, said before the pawn is released — the last moment it is a player
in the world) and `PawnMgr::Dismiss` (event `dismiss`, before `LogoutPlayerBot` deletes the Player
object). Only a pawn speaks: a real player's kills are the player's to narrate. The dial and the lines
are independent, so `Quiet` keeps its `death` line and says nothing on kills.

## What they ride

playerbots rolls a pawn's mount and pet during the build, so neither is known before the pawn exists
— it is not drawn from the seed, it is whatever the build rolled. The module therefore reads both
back after the build and records them on the roster row, `mount_spell` and `pet_name`, and the party
row draws them from there (`rides Black Stallion, with Wolf`): a pawn in that list may be offline, so
there is no object to ask. A pawn too low for a mount records 0, and an empty `pet_name` means no pet
— the row then says nothing rather than inventing one.

The recorded mount is the *fastest* one the pawn owns, ranked by the largest mount-speed aura value
on the spell and tie-broken by the lowest spell id (`PawnMgr::FindMountSpell` with `MountSpeed`).
`PlayerbotFactory::InitMounts` learns one mount per type — slow, fast, flying slow, flying fast — so
a pawn owns several, and the spell map has no order: taking the first hit would name whichever mount
the hash table happened to yield, which can change when the table rehashes and would rewrite the row
on every login.

They are deliberately not on the offer row. Before hiring there is nothing to promise, and a row
that promised a mount would be guessing at what the build will roll.

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

## The pawn page

A party row opens that pawn's page, and dismissal lives there with everything else a pawn can be
told to do, because the offers page has no rows to spare: the gossip packet carries at most sixteen
rows (`src/server/game/Entities/Creature/GossipDef.cpp:199-201`) and the server aborts on a 33rd
(`:43`), while the offers page is already at twelve offers plus two party rows plus two navigation
rows with two pawns. One page per pawn has seven rows and does not grow with the party.

The page names the pawn on every row that acts on it, because the gossip title does not: a page of
bare "follow me" rows would be indistinguishable from the same page for the other companion. The
rows are retrain (with the level it would reach and its fee), follow me, hold here, attack my
target, restock and repair (with the fee), dismiss and back. Back returns to the offers page the
player came from at the same page index — the page keeps the offers page's cursor untouched while it
is open — and a dismissal lands there too, because the pawn the page was about is no longer in the
party. A menu that outlived the party it was drawn for is re-checked against the live roster on the
click, so a stale row goes back to the offers page instead of acting on a guid it no longer knows.

The action code carries both the pawn and the verb, `1300 + party_row * 10 + verb`, because the page
is one pawn rather than a position on a list; `Pawn.MaxPawns` is clamped to four, so the whole range
is `1300..1339`. The two rows that spend money (retrain and restock) and the one that ends the
contract (dismiss) carry a confirmation box, and every box carries zero money: the charge happens
inside `PawnMgr` after its own checks, for the reason "The hire fee" gives. An order can be given
again, so the order rows ask nothing.

**Orders.** `PawnMgr::Order` forwards one verb through
`PlayerbotAI::HandleCommand(CHAT_MSG_WHISPER, verb, *player)` (`PlayerbotAI.h:638`) — the same entry
point a whisper from the owner reaches, with the owner as the sender, so playerbots' own
sender-is-owner check passes exactly as it does for a whisper. The verbs are playerbots' own whisper
vocabulary (`ChatCommandHandlerStrategy.cpp`): `follow`, `stay`, `attack` and `flee`. The page has
rows for `follow`, `stay` and `attack`; `Order` accepts all four, and refuses anything else rather
than passing it on — the string would otherwise be a second way into playerbots' command parser, and
one nobody reviewed.

Each order answers with the pawn's name and what it did — "<name> falls in behind you.", "<name>
holds here.", "<name> takes your target.", "<name> breaks off." Every verb is legal in combat:
`stay` and `follow` are wanted exactly when a fight starts. A pawn that is not in the world is told
so and nothing is queued, because an order is a moment, not a state.

## Upkeep: repair and restock

Repair and restock is one row and one price: the pawn's gear comes back at full durability and the
supplies the factory would have given it are replaced. The order matters.

1. `DurabilityRepairAll(false, 1.0f, false)` puts every equipped item at full durability.
2. Every carried item in the item categories the factory replenishes is destroyed:
   `ITEM_CLASS_CONSUMABLE` with subclass potion, elixir, flask or food, plus `ITEM_CLASS_REAGENT`
   and `ITEM_CLASS_PROJECTILE`, over the backpack (`INVENTORY_SLOT_ITEM_START..END`) and every
   equipped bag (`INVENTORY_SLOT_BAG_START..END`, `Bag::GetBagSize`).
3. `PlayerbotFactory` at the pawn's level runs `InitFood`, `InitPotions`, `InitReagents` and
   `InitAmmo`.

The purge is what makes the action idempotent. `InitFood` stores two stacks per food category per
call and `InitPotions` does the same, and both append; run on a pawn that still carries its
supplies, the init alone would hand it a second copy of everything and slowly fill its bags. Purging
first means restocking twice in a row leaves the same food count, not double. Only the categories the
factory replenishes are cleared — containers, gear, quest items, glyphs and trade goods are untouched
— so a bag full of gear the player handed over keeps the gear and simply gets less back.

The fee is `Pawn.ResupplyFeePercent` (default 100) of `player_level^2 * 10` copper: 40s at 20,
6g 40s at 80. It is charged to the player, checked before anything is touched, and refused when
unaffordable. A square rather than the hire's cube, because upkeep is paid many times over a level
and has to stay a small fraction of what the player earns at that level; `0` makes every restock
free. The action is allowed in combat — a wipe is when a restock is needed — and refused when the
module is off, when the pawn is not this player's recruited pawn, when it is not in the world, and
when it is dead.

## Retraining

A pawn is pinned to the level it was hired at, and the deck deals only part of the pool per cycle,
so a companion the player has outgrown may not come round again for hours while its party slot stays
spent. Retraining keeps the *same character* — name, spec, temperament, voice, affinity — and
rebuilds it at the level it would be offered at now.

`PawnMgr::OfferFor` builds the offer the roster row would get at the player's current level: the same
bot, the level re-derived through `DeriveLevel`, the row's spec pinned back over the seed's draw
(with the name, the role, the withheld variants and the temperament re-derived from the pinned spec,
so the offer describes one character rather than a blend of two), and the gear ratio from the
roster's base ratio. `Retrain` charges `HireFee` for that offer — the fee of hiring this pawn at the
new level, gear ratio and held-party multiplier included — and refuses when the player cannot afford
it. It also refuses while the player is in combat.

Then it sets `target_level`, clears `built`, records `paid` when money moved, writes the row and
calls `ApplyPawn` inline: the pawn is in the world, so the build does not go through the login queue.
What the build replaces is what a re-hire at that level replaces — the equipped gear is destroyed and
built again for the new level. What the pawn carries in its bags is not touched, so gear handed over
in a trade and never equipped survives, and the result message says so: "What it carries in its bags
is untouched; what it wears is replaced for the new level."

A pawn already at the level the offer names is refused — "<name> is already trained for level N." —
and the row is drawn anyway: a row that appeared and disappeared under a menu sent a moment earlier
is worse than an honest refusal.

## Affinity

Everything else about a pawn is fixed at hire time. Affinity is the one counter that moves with
play: a per-(owner, pawn) point total in the roster's `affinity` column, credited on every kill and
every completed quest to **every recruited pawn of that owner** — the pawns are the party, so they
bond together. A kill counts whether the owner or a pawn landed it: `OnPlayerCreatureKill` resolves a
pawn killer back to its owner, and `OnPlayerCreatureKillCredit` fires for the credited player of a
group kill, so a killing blow by a grouped pawn credits the owner twice. That is deliberate —
affinity is a participation counter, not a score, and the bookkeeping to recognise the duplicate
would cost more than the step it saves. Quest completion comes from `OnPlayerCompleteQuest`, which
only ever fires for a real player's own turn-in.

Points are counted in whole steps of `Pawn.AffinityStep` (default 100), and a step has exactly two
effects, both on draws the module already makes:

- **It learns what it was missing.** `WithheldVariants` subtracts the step count from the withheld
  draw, clamped to the pool, so enough steps and a pawn is missing nothing it was not dealt.
  `IsSpellForbidden` reads the same function, so the spell gate lets an ability through on the same
  build that grants it.
- **It arrives better geared at its next build.** `ApplyPawn` adds one `Pawn.AffinityGearPercent`
  (default 5) per step to the gear-score ceiling, capped at `Pawn.GearCapPercent`. The hire fee keeps
  the base ratio, so a veteran pawn is geared better than its price suggests — the reward for
  keeping it, not a tax on the player who does.

The step count is clamped to `Pawn.VariantWithholdMax`, because a step exists to hand back one
withheld ability and a bond that ran past its own denominator would be buying nothing. That clamp is
also the `M` in `bonded N/M` on the party row and on the retrain row: the most any pawn could learn.
A pawn that has earned nothing keeps a bare row, since the suffix is a reward rather than a
baseline.

The write is the one thing affinity is careful about. Rows move in memory on every award and the
database is written only when the step count changes, because this runs once per kill on the world
thread and a kill must not cost a write. Points inside the current step are lost on a crash and
persisted by the next build, login or step. `Pawn.AffinityEnabled = 0` freezes the counters and
makes the feature inert; nothing is deleted.

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
| Roster schema, I/O, offers, recruit/dismiss, orders, restock, retrain, affinity, the gear pass | `src/PawnMgr.cpp` |
| Spec pinning and the spellbook blacklist | `src/PawnArchetype.cpp` |
| Temperament and voice pools, their lines, and applying them | `src/PawnPersonality.cpp` |
| The innkeeper line, the offers page and the pawn page | `src/PawnInnkeeper.cpp` |
| Login queue, the learn-spell gate, the affinity hooks and the kill and death lines | `src/PawnScripts.cpp` |
| Loader symbol `Addmod_pawnScripts` | `src/pawn_loader.cpp` |

Two hooks carry the behaviour. `PlayerScript::OnPlayerCanLearnSpell` refuses a blacklisted spell at
the top of `Player::learnSpell`, so no learn path — factory, trainer, level-up maintenance, quest —
can undo a restriction. `PlayerScript::OnPlayerLogin` only *marks* a pawn; `WorldScript::OnUpdate` applies
its level and archetype once it is in the world, which is what puts that work after playerbots' own
login pass — the pass that would otherwise rebuild the pawn at its owner's level.

## Configuration

`conf/mod-pawn.conf.dist`: `Pawn.Enable`, `Pawn.MaxPawns`, `Pawn.LevelWindow`, `Pawn.GainXP`,
`Pawn.HireFeePercent`, `Pawn.HireFeeHeldPercent`, `Pawn.GearFloorPercent`, `Pawn.GearCapPercent`,
`Pawn.ResupplyFeePercent`, `Pawn.OfferedPerPage`, `Pawn.FeaturedMin`/`FeaturedMax`,
`Pawn.PoolRefreshMinMinutes`/`PoolRefreshMaxMinutes`, `Pawn.Debug`, the affinity scalars
`Pawn.AffinityEnabled`, `Pawn.AffinityKillPoints`, `Pawn.AffinityQuestPoints`, `Pawn.AffinityStep`
and `Pawn.AffinityGearPercent`, the voice scalars `Pawn.VoiceKillChancePercent` and
`Pawn.VoiceCooldownSeconds`, one `Pawn.Voice` pool and one `Pawn.VoiceLine.<label>` pool per voice,
and one `Pawn.Blacklist.<class>.<spec_no>` list and one `Pawn.Variant.<class>.<spec_no>` pool and one
`Pawn.Temperament.<class>.<spec_no>` pool per archetype. The shipped values are
`Pawn.ResupplyFeePercent = 100`, `Pawn.AffinityEnabled = 1`, `Pawn.AffinityKillPoints = 10`,
`Pawn.AffinityQuestPoints = 25`, `Pawn.AffinityStep = 100`, `Pawn.AffinityGearPercent = 5`,
`Pawn.VoiceKillChancePercent = 10` and `Pawn.VoiceCooldownSeconds = 120`. The two shipped line pools
match the two shipped voice labels:
`Pawn.VoiceLine.Talkative = kill: Well fought! | kill: Next one, then. | kill: That is how it is done. | death: Avenge me, friend. | dismiss: Until next time.`
and `Pawn.VoiceLine.Quiet = death: ... | dismiss: *nods*`. Spell names in the blacklist are resolved
once at startup; an unresolved name is logged as an error rather than ignored. That resolution
happens in `OnStartup`, not at the first config load: `OnAfterConfigLoad` runs at
`World.cpp:302` and `sSpellMgr->LoadSpellInfoStore` at `World.cpp:407`, so a build there finds an
empty spell store and every blacklist and variant name fails to resolve. A reload rebuilds both
tables, on a running server where the store is already loaded.

The pool keys are where an upgrade shows. `scripts/thor-install.sh` copies the `.dist` only when the
live conf is missing, so an install made before a pool shipped would keep a conf with no
`Pawn.Temperament.*` block at all and `PawnPersonality::Load` would have nothing to roll from. The
installer therefore back-fills them (`pawn_append_missing_conf`): every shipped `Pawn.Voice`,
`Pawn.VoiceLine.<label>`, `Pawn.Blacklist.<cls>.<spec>`, `Pawn.Variant.<cls>.<spec>` and
`Pawn.Temperament.<cls>.<spec>` line
the live conf does not define is appended with its shipped value, under a comment saying so. It is
append-only — no existing value is rewritten, and a pool the admin trimmed is left alone — and
idempotent: the second run appends nothing.

## The pool and the featured draw

The pool is every offline character on a type-2 AddClass account — about 150 with the installer's
`Playerbots.AddClassAccountPoolSize = 15`, both teams, every class. The module re-reads it at
startup, on a random 40-60 minute timer, and on demand if it is still empty (a boot can finish
before playerbots has created the pool characters).

Each player gets a **selection**: a random count in `[FeaturedMin, FeaturedMax]` (10-15) of the
pool, restricted to the player's team and to candidates that are neither in the party nor online. It
is dealt the first time the player opens the picker after a pool refresh or a server restart, and
kept from then on — opening the picker again, at any innkeeper, shows the same pawns at the same spec
until the pool is re-read. Recruiting one takes it out of the selection for good; dismissing it does
not put it back.

It **deals rather than samples**: `PawnMgr::SelectionFor` keeps a per-player deck — the eligible pool
in a shuffled order, with a cursor — and takes the selection off the top when it is dealt. No face
comes round again until the deck has been dealt out; a pool refresh reshuffles it.

Two rules keep cycles from blurring together. A selection never straddles a reshuffle — if the deck
cannot cover the whole selection it is reshuffled first, and the tail it skips goes back into the
next shuffle. And the faces of the previous selection are dealt *last* in a new order, so the one
moment a repeat could land in the very next selection does not. A dealt counter that has stopped
being a candidate (recruited, logged in, dropped by a refresh) is skipped when it comes up, so the
deck heals itself.

On a pool of ~75 eligible characters that is a face coming back after about six pool cycles.

The selection is the crowd, not the numbers: the rows are rebuilt from it on every open, so level,
gear ratio and hire fee follow the player's own level. Spec, temperament and voice stay a pure
function of the two guids, so a face you have seen keeps them whenever it comes back.

## Why there is no SQL

The line is a `CreatureAI` handed out by an `AllCreatureScript` gated on `UNIT_NPC_FLAG_INNKEEPER`,
so it needs no template row, no spawn and no `ScriptName`: it exists wherever an innkeeper does, and
nowhere else. A named `CreatureScript` cannot serve it: those are database-bound, so the manager looks
the name up in `script_name` and registers nothing when there is no row, and gossip is dispatched by
the creature's own script id, which for an innkeeper is the core's `npc_innkeeper`. The roster
table is created in code for the reason in item 1 above. The module ships no `data/sql` tree at all.
