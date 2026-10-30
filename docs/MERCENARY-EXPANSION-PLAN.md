# Mercenary system: expansion plan

This plan extends the shipped mercenary system ([MERCENARY-INTEGRATION-PLAN.md](MERCENARY-INTEGRATION-PLAN.md))
along the five seams the module already has. It is written to be implemented, not studied: every
feature below states its config, its schema, its API, its UI and its acceptance test.

Order of work: **orders → repair and restock → retrain → affinity → voice lines**. Each phase is
shipped and verified on its own; the later ones consume the picker's mercenary page, so the order is a
dependency, not a preference.

## Shared rules

These hold for every phase, and `scripts/mercenaries/check_mercenaries.py` enforces most of them.

1. **The offline gate is the contract.** Every `Mercenary.*` key read with `GetOption<T>("...")` must be
   defined in `modules/mod-mercenaries/conf/mod-mercenaries.conf.dist`; every non-pool key must also be in
   `MERCENARY_CONF_DEFAULTS` in `scripts/thor-install.sh` **with the same value**, and `mercenary_value_ok`
   must accept it (its `*) return 1` fallback rejects anything without a case arm).
2. **Pool keys are the exception**: `Mercenary.VoiceLine.<label>` joins `Mercenary.Voice`, `Mercenary.Blacklist.*`,
   `Mercenary.Variant.*` and `Mercenary.Temperament.*` in the installer's `mercenary_append_missing_conf` case and
   in the checker's exemption tuple, because a repair must never overwrite a pool an admin trimmed.
3. **No new SQL file.** The roster is created programmatically; a new column goes through
   `EnsureSchema`'s `CREATE TABLE` **and** `AddMissingColumn` (MariaDB-only syntax is not used), and
   into `LoadRoster` and the `REPLACE INTO` column list in `Upsert` or it is dropped on the next
   save.
4. **The gossip page is capped at 16 rows** (packet `GossipDef.cpp:199-201`), and a 33rd item
   aborts the server (`GossipDef.cpp:43`). The offers page is already at 16 with two mercenaries
   (12 offers + 2 party + 2 navigation), so every new per-mercenary action lives on a **mercenary page**, not
   on the offers page.
5. **Action codes** stay above the core's own option range (`GOSSIP_OPTION_MAX` 21):

   | Range | Meaning |
   |---|---|
   | `1000` | open the picker |
   | `1010..1099` | recruit the offer at this page index |
   | `1110..1119` | open mercenary page `n` (party row `n`) |
   | `1200`, `1201` | next page, previous page |
   | `1300 + n*10 + verb` | action `verb` on mercenary `n`: 0 retrain, 1 restock, 2 follow, 3 stay, 4 attack, 5 dismiss, 6 back |

   `MaxMercenaries` is clamped to 4, so `1300..1339` is the whole mercenary range.
6. **Threading.** Everything here runs on the world thread: gossip handlers, script hooks and
   `MercenaryMgr::Update`. No new queues, no timers beyond the existing `Update` drain.
7. **Compile check.** `scripts/syntax-check.sh` (`-fsyntax-only` against the pinned core) is the
   only compiler here. Every unit ends with it passing.

## Feature 2 — orders on the mercenary page

**Problem.** Playerbots already accepts the order vocabulary as whispers (`follow`, `stay`,
`attack`, `flee`, `grind`, `e <link>`, `t` — `ChatCommandHandlerStrategy.cpp`), but a solo player
has to know the syntax to use it. Nothing in the picker exposes it.

**Design.** The party rows stop dismissing on click and open a **mercenary page**: one page per mercenary with
its orders, its retrain, its restock and its dismissal. `MercenaryMgr::Order` forwards a verb through
`PlayerbotAI::HandleCommand(CHAT_MSG_WHISPER, verb, *player)` (`PlayerbotAI.h:638`) — the same entry
point a whisper reaches, with the owner as the sender, so the security check passes exactly as it
does for a whisper.

**API.**

```cpp
// MercenaryMgr.h
// Forwards one order to an online mercenary of this player. The verb is the whisper vocabulary
// playerbots already implements; anything else is refused rather than sent.
bool Order(Player* player, ObjectGuid bot, char const* verb, std::string& message);
```

**Verbs.** `follow`, `stay`, `attack`, `flee`. Each has a reply line ("<name> falls in behind you.",
"<name> holds here.", "<name> takes your target.", "<name> breaks off."). `attack` maps to the
`attack` trigger ("attack my target"); `flee` to `flee`.

**Refusals.** Not enabled; not this player's recruited mercenary; the mercenary is offline (nothing is
queued — an order is a moment, not a state); the owner is in combat *only* for the non-combat
verbs? No: every verb is legal in combat, and `stay`/`follow` in combat is exactly when they are
wanted.

**Acceptance.** At an innkeeper, click a party row → the mercenary page opens; "Follow me" makes the
mercenary follow, "Hold here" makes it stay, "Attack my target" attacks the player's target; "Back"
returns to the offers page with the same page index.

## Feature 3 — repair and restock

**Problem.** A mercenary's gear degrades and its food, potions, reagents and ammo run out; nothing in
the module restores them, and a mercenary that cannot drink is a mercenary that dies.

**Design.** One action on the mercenary page: repair, then replace what it carries. The replacement is
**destroy-then-init**, not init alone: `PlayerbotFactory::InitFood/InitPotions/InitReagents/InitAmmo`
append (`InitFood` stores two stacks per food category per call), so calling them on a mercenary that
already has food fills its bags. Purging the four categories first makes the action idempotent.

**Fee.** `Mercenary.ResupplyFeePercent` of `player_level^2 * 10` copper (level 80: 64,000 copper = 6.4g at
100%). Charged from the player, not the mercenary: a mercenary's own purse is the factory's, and a fee the
player cannot see is not a fee.

**API.**

```cpp
uint32 ResupplyFee(Player* player) const;   // 0 when the fee is switched off
bool Resupply(Player* player, ObjectGuid bot, std::string& message);
```

**Steps.** Refuse when disabled, not this player's mercenary, offline, or unaffordable. Charge. Then on
the mercenary: `DurabilityRepairAll(false, 1.0f, false)`; destroy every carried item whose template is
`ITEM_CLASS_CONSUMABLE` (subclass food, drink or potion), `ITEM_CLASS_REAGENT` or
`ITEM_CLASS_PROJECTILE`, over the backpack (`INVENTORY_SLOT_ITEM_START..END`) and every equipped bag
(`INVENTORY_SLOT_BAG_START..END`, `Bag::GetBagSize`); then
`PlayerbotFactory(bot, bot->GetLevel()).InitFood(); InitPotions(); InitReagents(); InitAmmo();`.

**Edge cases.** The mercenary's bags may be full of gear the player gave it: the purge only removes the
four consumable classes, so gear is untouched and a full bag simply gets less back. The action is
allowed in combat (a wipe is when it is needed) but not while the mercenary is dead.

**Acceptance.** Restock twice in a row: the second run leaves the mercenary with the same food count, not
double. Repair a damaged mercenary: durability is full. The fee is deducted once per click.

## Feature 1 — retrain

**Problem.** A mercenary is pinned to the level it was hired at. The offers are dealt from a deck (12 of
~75 per cycle, one cycle per 40-60 minutes), so a companion the player has outgrown may not come
round again for hours, and `Mercenary.MaxMercenaries = 2` means the slot is spent. Today the only way to have a
current mercenary is to dismiss and hire a different face.

**Design.** Retrain keeps the **same character**: the same name, the same spec, the same
temperament, the same voice, the same affinity. It re-derives the level at the player's current
level and runs the existing build pass again. That is exactly what re-hiring the same mercenary at a new
offer level does today (new level, new fee, `built = 0`, fresh gear), minus the requirement that the
face comes round in the selection.

**API.**

```cpp
// The offer this roster row would be at the player's current level: same bot, same spec,
// level re-derived through DeriveLevel, gear ratio from the roster's affinity.
MercenaryOffer OfferFor(Player* player, MercenaryRosterRow const& row) const;
bool Retrain(Player* player, ObjectGuid bot, std::string& message);
```

**Steps.** Refuse when disabled, not this player's recruited mercenary, offline, in combat, or when the
re-derived level equals `row.TargetLevel` ("<name> is already trained for this"). Compute the fee
with `HireFee(player, offer)`; refuse when unaffordable. Charge. Set `TargetLevel = offer.Level`,
`Built = false`, `Paid = true` when a fee was paid, `Upsert`, then `ApplyMercenary(bot)` — the mercenary is in
the world, so the build runs inline rather than through the login queue.

**Gear.** The build pass re-equips the mercenary (`DestroyEquippedGear` + `InitEquipment`), which is the
same behaviour a re-hire at a new level has. The rebuild is a full inventory reset in playerbots
(`PlayerbotFactory::Randomize` calls `ClearInventory`, which destroys the backpack, the keyring and
every equipped bag), so `ApplyMercenary` snapshots what the mercenary carries before the build and stores each
stack back into the fresh bags afterwards. The items the player handed over survive; their
enchantments and durability do not, because a carried item's state is not what was handed over. The
confirmation box and the success message say the bags are kept.

**Acceptance.** Outlevel a mercenary by more than `Mercenary.LevelWindow`, retrain it: it comes back at the new
level with the same spec and name, the fee is charged once, and clicking retrain again refuses
because it is already at that level. Its affinity and voice are unchanged.

## Feature 4 — affinity

**Problem.** Everything about a mercenary is decided at hire time and frozen: level, spec, gear ratio,
which variant abilities it was dealt, its temperament, its voice. A companion the player has fought
beside for twenty levels is mechanically the same stranger it was on day one.

**Design.** Affinity is a per-(owner, mercenary) point count that moves with play. It has exactly two
effects, both on draws the module already makes:

1. **It learns what it was missing.** The withheld variant abilities are a seeded draw from
   `[VariantWithholdMin, VariantWithholdMax]`. Every `Mercenary.AffinityStep` points returns one of them:
   `withheld = max(draw - steps, 0)`, and `steps` is clamped to `VariantWithholdMax`, so affinity
   can never conjure abilities the spec's variant pool does not have. `MercenaryMgr::IsSpellForbidden`
   uses the same function, so the spell gate and the build stay in agreement.
2. **It arrives better geared at its next build.** The gear-score ceiling in `ApplyMercenary` is
   `owner_gear_score * ratio / 100`; affinity adds `steps * Mercenary.AffinityGearPercent` to the ratio,
   capped at `Mercenary.GearCapPercent`. The **fee** keeps using the base ratio: a veteran mercenary is geared
   better than its price suggests, which is the reward, not a tax.

**Points.** A kill credits `Mercenary.AffinityKillPoints`, a completed quest `Mercenary.AffinityQuestPoints`,
to **every recruited mercenary of the owner** — the mercenaries are the party. The award happens in exactly one
place, `PlayerScript::OnPlayerCreatureKillCredit`: the core calls it once per creature death with the
responsible player (`killer->GetCharmerOrOwnerPlayerOrPlayerItself()`, `Unit.cpp:13858`), so an
owner's kill, a mercenary's kill, a pet's kill and a totem's kill all credit the owner exactly once. The
killer hook (`Unit.cpp:13916`) fires only when the killer is itself a player, which would
double-count every ordinary kill and miss every pet kill, so it carries the voice line and no award.
Quest completion comes from `OnPlayerCompleteQuest`, which only ever fires for a real player's own
turn-in.

**Persistence.** The row is updated in memory on every award; the database is written when the
**step count** changes (a step is 100 points by default), so a kill does not cost a DB write, and
`ApplyMercenary` writes the row unconditionally at the end of a login, so the points inside the current
step are flushed there too. A crash between awards loses at most the points since the last step or
login; the trade is deliberate.

**Schema.** `affinity INT UNSIGNED NOT NULL DEFAULT 0` in `mercenary_roster`, through `EnsureSchema`,
`AddMissingColumn`, `LoadRoster` and the `REPLACE INTO` list.

**Config.** `Mercenary.AffinityEnabled` (1), `Mercenary.AffinityKillPoints` (10), `Mercenary.AffinityQuestPoints`
(25), `Mercenary.AffinityStep` (100), `Mercenary.AffinityGearPercent` (5).

**API.**

```cpp
uint32 AffinitySteps(MercenaryRosterRow const& row) const;   // points / step, clamped to the withhold cap
void AwardAffinity(Player* player, uint32 points);      // every recruited mercenary of this player
```

**UI.** The party row and the mercenary page show the bond: `bonded 2/2` when every variant has been
returned, `bonded 1/2` at one step.

**Acceptance.** Kill ten creatures with two mercenaries in the party: both rows gain
`10 * AffinityKillPoints` in memory, and at 100 points the withheld list on the mercenary page shrinks by
one entry and the mercenary learns that ability on its next login (the spell gate no longer refuses it).

## Feature 6 — voice lines

**Problem.** `Mercenary.Voice` currently only toggles the `emote` strategy: a talkative mercenary emotes, a
quiet one does not. Neither ever says anything.

**Design.** Per-voice **line pools**, spoken by the mercenary at three moments the module already sees:
a kill, its own death, and its dismissal. The module triggers the lines itself (`bot->Say`, the
same call `SayAction.cpp:139` makes), so no playerbots hook is needed.

**Config.**

```ini
Mercenary.VoiceKillChancePercent = 10
Mercenary.VoiceCooldownSeconds = 120
Mercenary.VoiceLine.Talkative = kill: Well fought! | kill: Next! | death: Avenge me, friend. | dismiss: Until next time.
Mercenary.VoiceLine.Quiet = death: ... | dismiss: *nods*
```

**Parsing.** `MercenaryPersonality::Load` enumerates `Mercenary.VoiceLine.` with `GetKeysByString` (never
probed: an absent key must be silent, the same rule the temperament keys follow), splits the value
on `|` into `event: text` pairs, and keeps `kill`, `death` and `dismiss`. A label that is not one of
the `Mercenary.Voice` labels, an unknown event, or an empty text is logged and dropped. The line for a
mercenary is drawn from the `(owner, bot)` seed, so a mercenary says the same thing at the same moment every
time — the same determinism as its temperament.

**API.**

```cpp
// The line this mercenary would say for this event, or nullptr when its voice has none.
std::string const* Line(ObjectGuid owner, ObjectGuid bot, char const* event) const;

// Says it: resolves the mercenary's voice, applies the kill chance and the per-mercenary cooldown, and
// speaks in the mercenary's faction language. Silent when there is nothing to say.
void SayLine(Player* bot, ObjectGuid owner, char const* event) const;
```

**Call sites.** `MercenaryScripts.cpp`: `OnPlayerCreatureKill` (mercenary is the killer, event `kill`) and
`OnPlayerJustDied` (mercenary, event `death`). `MercenaryMgr::Dismiss`: event `dismiss`, before
`LogoutPlayerBot` (which deletes the Player object).

**Acceptance.** With `Mercenary.VoiceKillChancePercent = 100` and `Mercenary.VoiceCooldownSeconds = 0`, a
talkative mercenary says a kill line when it lands a kill; a quiet one stays silent on kills but speaks
its death line; no line is said twice inside the cooldown.

## Phases and parallel units

Every unit owns the files it is given and nothing else. The orchestrator writes the headers, the
conf keys and the installer/checker wiring before the units start, then verifies with
`scripts/syntax-check.sh` and `scripts/mercenaries/check_mercenaries.py`, then commits the phase.

| Phase | Feature | Unit | Files | Work |
|---|---|---|---|---|
| P1 | orders | M | `MercenaryMgr.cpp` | `Order` |
| P1 | orders | U | `MercenaryInnkeeper.cpp` | mercenary page, view state, four order rows, dismiss moved onto it |
| P2 | restock | M | `MercenaryMgr.cpp` | `ResupplyFee`, `Resupply` |
| P2 | restock | U | `MercenaryInnkeeper.cpp` | one restock row with its fee and confirmation |
| P3 | retrain | M | `MercenaryMgr.cpp` | `OfferFor`, `Retrain` |
| P3 | retrain | U | `MercenaryInnkeeper.cpp` | one retrain row, level and fee in the label |
| P4 | affinity | M | `MercenaryMgr.cpp` | schema, roster I/O, `AffinitySteps`, `AwardAffinity`, withheld and gear ceiling |
| P4 | affinity | S | `MercenaryScripts.cpp` | `OnPlayerCreatureKill`, `OnPlayerCreatureKillCredit`, `OnPlayerCompleteQuest` |
| P4 | affinity | U | `MercenaryInnkeeper.cpp` | bond on the party row and the mercenary page |
| P5 | voice | P | `MercenaryPersonality.h/.cpp` | line pools, `Line`, `SayLine` |
| P5 | voice | S | `MercenaryScripts.cpp` | kill and death call sites |
| P5 | voice | M | `MercenaryMgr.cpp` | dismiss call site |

## Verification

Per phase, in this order:

1. `scripts/syntax-check.sh` — all 16 translation units compile against the pinned core.
2. `python3 scripts/mercenaries/check_mercenaries.py` — the conf/installer/schema/picker/hook agreement.
3. A read of the diff for the invariants the gate cannot see: the row budget, the action ranges, the
   escaping of any text bound into SQL, and that no new code path calls a prepared-statement
   transaction on the sync characters connection.

Nothing here can be proved in game from this host: the runtime checks are the ones in each feature's
acceptance section, on the device.

## Risks

| Risk | Why it matters | Mitigation |
|---|---|---|
| the mercenary page grows past 16 rows | a 17th row is invisible, a 33rd aborts the server | one page per mercenary, 8 rows, fixed verbs |
| restock fills bags | `InitFood`/`InitPotions` append per call | purge the four consumable classes first, then init |
| retrain destroys gifted gear | `PlayerbotFactory::Randomize` calls `ClearInventory`, which wipes the backpack, the keyring and every equipped bag (`PlayerbotFactory.cpp:857`, `:3917-3921`) | `ApplyMercenary` snapshots every carried stack before the build and stores it back into the bags afterwards — bags only, never an equipment slot (`Player.cpp:759-790`) — so a gift survives; its enchantments and durability do not, and the log names anything that did not fit |
| affinity writes per kill | a DB write per kill on a busy world thread | write only when the step count moves |
| a voice line with a `|` or a newline | the conf format splits on `|` | the parser takes `event: text` chunks and trims; a text containing `|` is not expressible, and is documented as such |
| a label rename loses its lines | lines are keyed by the voice label | an unknown label is logged at load, and the shipped conf keeps the two labels |
