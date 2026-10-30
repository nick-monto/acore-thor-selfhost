# Creature gear: mobs that wear what they drop

Companion to [`SERVER-STACK.md`](SERVER-STACK.md). The question: humanoids that visibly wear gear, whose stats come from that gear, and which can drop what they are wearing. Does that need a forked client and edited client data, or can it be database data applied when the release is built?

Confidence: **[V]** verified here from source, artifact or API, **[R]** reported, **[I]** inference.

---

## Implementation status

This design is now implemented in this repository:

- **Module**: `modules/mod-creature-gear` — phases A, B and C (`src/creature_gear_common.h` contract, `src/CreatureGearMgr.cpp` config + outfit storage + phase A re-roll, `src/CreatureGearDrop.cpp` phase B, `src/CreatureGearAppearance.cpp` phase C, `src/creature_gear_loader.cpp` entry point), plus `conf/mod-creature-gear.conf.dist`.
- **Data pipeline**: `scripts/creature-gear/{gen_creature_gear.py,check_creature_gear.py,gear.toml}`, emitting the five generated files under `modules/mod-creature-gear/data/sql/world/base/`.
- **Delivery wiring**: `.github/workflows/server-aarch64.yml` (checkout of this repo, module copy into the core tree, module SQL into `$PREFIX/src/modules/mod-creature-gear/data/sql`), `scripts/thor-install.sh` (`fetch_self_module` for `--from-source`, `modules/mod-creature-gear.conf` creation and defaults check, `--status` conf and SQL-tree checks), `pins.toml` (`[module.creature_gear]`).
- **Phase D (stats from worn gear) is not implemented.**

Verification level actually achieved — no device-run claims:

| Check | Result |
|---|---|
| Compiles against the pinned upstream core (`pins.toml`) | yes — x86_64, RelWithDebInfo, `-DMODULES=static`; the module builds as one target into `libmodules.a` and the generated `ModulesLoader.cpp` declares `Addmod_creature_gearScripts()` [V] |
| Config install | `cmake --install` populated the booted tree's `etc/`, so `etc/modules/mod-creature-gear.conf.dist` came from the core's own install rules, not a hand copy [V] |
| End-to-end boot (dev workstation, x86_64) | a real `worldserver` (pinned core + playerbots + this module) booted from a device-shaped tree — `bin/`, `etc/`, `src/` holding `data/sql` plus both modules' `data/sql` — against virgin databases and the pinned client data. Its own DB updater applied, in order: base population → the `RELEASED` update `2026_06_16_00.sql` (renames `creature.id1` → `id`, drops `id2`/`id3`) → our four files with state `MODULE` (`creature_gear_10_schema`, `20_weapon_pools`, `30_outfits`, `40_spawn_equipment`, timestamps 1 s apart in that order; the dataset has since grown a fifth file, `35_outfit_variants`, see §12). Log: `mod-creature-gear: loaded 400 outfit(s)`, zero `Unknown item (ID=` lines, zero `is not equipable in a hand` lines, zero ERROR-level lines in the whole log, and `WORLD: World Initialized In 0 Minutes 3 Seconds` — 90 s from launch to initialized on this workstation (**dev-workstation number, not a Thor measurement**) [V] |
| Post-boot SQL (same run) | 400 `mod_creature_gear_outfit` rows; 1200 `creature_equip_template` rows joined to them, 400 of them with a non-zero off-hand; 3478 spawn rows at `equipment_id = -1` across 400 distinct entries [V] |
| Terrain hooks (same run) | ran for the 16 creature entries whose grids loaded during that boot: 5 morphs to a player-race model across 3 entries, 13 kept their stock model, zero race-resolution failures — the expected shape, since only a creature whose model is a player-race model can carry an outfit [V] |
| Re-run with the respawn hooks (§10, corrections 7 and 8) | same device-shaped tree and the same databases, binary sha256 `3ddc76a5e7b4d9ff9c82cddb8ed59ebcee4a36e1ead6870f4b9daa450f78c8c2`: the module conf was read, `loaded 400 outfit(s)`, 4 morph lines / 11 stock-model lines for the entries whose grids loaded, zero ERROR lines, zero item rejections, `World Initialized In 0 Minutes 3 Seconds`, clean `server exit` (rc 0). Nothing died in that run, so it is the add-world path again — the respawn and entry-switch halves are source-verified (`SelectLevel()` at `Creature.cpp:631` and `:2108`, never at creation), and their client-visible effect still needs the device [V/I] |
| Per-spawn customisation (two further boots, same databases) | with `CreatureGear.Appearance.VaryPerSpawn` on, the module logged derived option counts for all 42 race/gender pairs of the pinned `CharSections.dbc`; entry 14121's three spawn points got three different (skin, face, hair, colour, facial hair) sets; the five spawn points seen in both boots reproduced byte-identical values; zero ERROR lines both runs [V] |
| Per-spawn armour variants (three boots, same databases, last one the shipped artifact) | the worldserver applied `creature_gear_35_outfit_variants.sql` with state `MODULE`, logged `loaded 400 outfit row(s) and 1600 variant row(s) for 400 creature(s)` and zero ERROR lines in all three runs; eight spawn points across six entries wore variants matching `spawnId % 4 + 1`, entry 14121's three spawn points among them with three different five-piece sets, and every spawn point that appeared in more than one boot repeated byte for byte (§12) [V] |
| Worn-gear drop vs the loot table's own roll (probe build, same tree) | a temporary probe seeded a real `Loot` object with one of the creature's own worn items - exactly what the template would have rolled for it - and ran the shipped injection against it: 6 of 6 probes logged `wears 1 item(s) its loot template already rolled, left out of the injection`, the loot object kept exactly one row for that item, and on the creature with two eligible worn items the injection added the other one instead (§5) [V] |
| Phase A on alternate-entry spawns (SQLite replay of the base dumps, the two `creature_multispawn` updates and the module's pool rows: 149,879 spawns, 984 alternates) | the previous single statement left 41 spawn/entry pairs whose shared `equipment_id` is missing from the alternate — spawn 39353 / entry 11553 is the one the worldserver logged; the three guarded statements leave 0, both from the dump state and from a database that already ran the previous generation (every covered spawn at `-1`), and a second run changes nothing |
| Dataset uncapped to every eligible creature (same tree and databases, module SQL replay) | the generator selected all 1274 creatures the rules admit (entries 127–40356) and the updater re-applied the five `MODULE` files onto a database that already held the 400-creature dataset: the module logged `loaded 1274 outfit row(s) and 5096 variant row(s) for 1274 creature(s)`, zero ERROR lines, and post-boot SQL shows 1274 outfit rows, 3822 pool rows, 5096 variant rows and every `creature` row of those entries at `equipment_id = -1` (9915); the previously covered creatures' outfit and variant rows are byte-identical to before the replay (counts and CRC32 sums unchanged), and the `creature_multispawn` equipment warnings in that boot were later traced to this dataset: the uncovered entry they name is the *alternate* of a covered spawn, whose shared `equipment_id` was resolved against the covered entry (§14) [V] |
| Class-appropriate gear (same tree and databases, module SQL replay) | the generator selected the same 1274 creatures per class (warrior 860, paladin 292, mage 119, rogue 3, zero fallbacks) and the updater re-applied the five `MODULE` files onto the database that already held the previous dataset: 3527 pool rows (295 fewer - 292 paladin ranged and 3 mage wand rows, both classes filling two roles), 1274 outfit rows, 5096 variant rows, 9917 spawn rows. The boot logged `loaded 1274 outfit row(s) and 5096 variant row(s) for 1274 creature(s)` with zero ERROR lines, and `Loaded 14385 Equipment Templates` where the previous dataset left 14680 - the 295 stale rows are gone. Post-boot SQL holds 3527 rows, none above a creature's role count, and the two-role set is exactly the 292 paladins and 3 mages; BoE coverage 73.39% (935/1274) against the 0.70 floor [V] |
| Still unverified | the client-visible outfit (needs a real client), the rendered face and hair (same client, same reason), the drop path in a real corpse loot (the probe exercises the injection, not `FillLoot`), the arm64/Thor device run, and CI [V] |

The code does not follow this document everywhere. §10 lists the eight divergences.

---

## 1. Answer in one table

| Effect | Client data change? | Code change? | Mechanism |
|---|---|---|---|
| Visible weapon / shield / ranged | **no** (stock items) | **none** — SQL only | `creature_equip_template` pool (keyed by the creature's own entry) selected per spawn row by `creature.equipment_id` → `UNIT_VIRTUAL_ITEM_SLOT_ID` (3 slots) |
| Visible armour, 11 slots, dynamic | **no** (stock display ids) | module | module sets `UNIT_FLAG2_MIRROR_IMAGE` + a player-race model and answers `CMSG_GET_MIRRORIMAGE_DATA` itself with `SMSG_MIRRORIMAGE_DATA`; no core helper for arbitrary units |
| Per-spawn face and hair on that model | **no** (stock client data) | module | `CreatureGear.Appearance.VaryPerSpawn` draws skin/face/hair style/hair colour/facial hair per spawn point from `CharSections.dbc`, the file the client validates those bytes against (§11) |
| Per-spawn armour variant (helm/chest/legs/feet/hands) | **no** (stock items) | module | `mod_creature_gear_outfit_variant` carries one row per variant for those five slots and the module picks the variant per spawn point from the spawn id, using the same pick for that creature's drop (§12) |
| Visible armour, static look | **yes** — `CreatureDisplayInfoExtra.dbc` | client patch only | DBC row with 11 `ItemDisplayInfo` ids |
| Drop what it wears | no | module (or core patch) | `Loot::AddItem` from a loot or death hook |
| Stats from worn gear | no | module — **new code, nothing implements it** | `SetStatFlatModifier(..., TOTAL_VALUE, ...)` at spawn |
| Custom item art (new model) | **yes** — client `Data/patch-<n>.MPQ` *and* server `Data/dbc/Item.dbc` | DBC tooling | `Item.dbc` + `ItemDisplayInfo.dbc` + `.m2`/`.blp`; `scripts/item-dbc/patch_item_dbc.py` already writes the `Item.dbc` half and the patch archive for the loot bags, so a new display id needs only the `ItemDisplayInfo` row and the model/texture added to it |

Only the last row forces a client data fork, and it is orthogonal to the feature: it applies only if the gear must use artwork the stock 3.3.5a client does not have.

## 2. Why the mechanic itself needs no client fork

The client resolves appearance locally; the server only ever sends ids.

| Fact | Evidence |
|---|---|
| Held items are 3 `uint32` **item entries** inside the unit update block, PUBLIC, so a runtime write is broadcast to clients in range | `UpdateFields.h:116` (`Size: 3`), `UpdateFieldFlags.cpp:220-222`; written by `Creature::LoadEquipment` `Creature.cpp:1808-1828` [V] |
| Equipment selection is **per spawn row**, on `creature.equipment_id` (signed `tinyint`); on this pin `creature_template` has **no** `equipment_id` column, and `creature_equip_template` rows are keyed by the creature's **own entry** (`CreatureID`, contiguous `ID = 1..n`): `0` = no equipment, `-1` = random variant, `n` = variant `n` | loader `ObjectMgr.cpp:1485-1515` (id 0 skipped, rows stored under `_equipmentInfoStore[entry][id]`); `-1` resolved in `ObjectMgr::GetEquipmentInfo` `ObjectMgr.cpp:1454-1470`; applied at `ObjectMgr.cpp:2429-2438` [V] |
| The core rejects anything not hand-equippable, and anything absent from the **server's** Item.dbc | `ObjectMgr.cpp:1522-1550`: 9 `InventoryType`s whitelisted, else `"... is not equipable in a hand, forced to 0"`; `sItemStore.LookupEntry` misses → `"Unknown item (ID=...) ... forced to 0"` [V] |
| Client chain for a held item: item entry → `Item.dbc` → `ItemDisplayInfo.dbc` (`.M2` + `.blp`) → model files | [R] wowdev/TrinityCore DBC docs |
| Full armour (head…tabard, 11 slots) reaches the client as **display ids** through the mage clone packet, native client support, no patch | `Handlers/SpellHandler.cpp:768-834` (`SMSG_MIRRORIMAGE_DATA`), `SpellAuraEffects.cpp:2912-2918`; AC port `azerothcore/mod-dress-npc` [V/R] |
| The only `SMSG_MIRRORIMAGE_DATA` builder in this core is that clone-caster handler: 68 bytes, 11 display ids, and it sends **nothing** for a creature (no `SPELL_AURA_CLONE_CASTER`). There is **no** helper for arbitrary units and **no** update field carrying the 11 ids for a creature | `Handlers/SpellHandler.cpp:768-834` [V] |
| So phase C must insist on a **player-race model** (`ChrRaces.model_m/model_f`, race derived from the creature's own baked display via `CreatureDisplayInfo` → `CreatureDisplayInfoExtra.DisplayRaceID`), set `UNIT_FLAG2_MIRROR_IMAGE`, and answer `CMSG_GET_MIRRORIMAGE_DATA` itself from `ServerScript::CanPacketReceive` (returning `false` to suppress the core handler); the packet is rebuilt per requester | `CreatureGearAppearance.cpp:32-105` (resolve), `:114-192` (apply + both hooks), `:194-268` (packet) [V] |
| NPC armour baked into the client: `CreatureDisplayInfoExtra.dbc` cols 8-18 are `ItemDisplayInfo` ids, one static set per display id | [R] wowdev/TrinityCore DBC docs |
| Creature stats never read equipment; equipment is visual only in stock cores | no item term in `Creature::LoadEquipment` `Creature.cpp:1808-1828`; the only gameplay reads are `HasWeapon()` (`Creature.cpp:3390-3393`) and spell requirement checks [V] |

So with stock items: weapons and shields are a pure database edit, armour is a database edit plus one packet the core already knows how to send. Stats and drops are server-side logic. **Nothing about the feature requires touching `Data/*.MPQ`.**

## 3. Design constraints this feature inherits

Three constraints shape everything below, each already established in this document or by the shipped code:

- **Stock cores never derive stats from equipment.** Equipment is visual only: `Creature::LoadEquipment` has no item term, and the only gameplay reads are `HasWeapon()` and spell requirement checks (§2, last row) [V]. The *stat* half of "stats from the gear it wears" is therefore new work with nothing to reuse (§6); its appearance/drop half is not.
- **This stays in a module, so the pinned core is never patched.** The implementation is `modules/mod-creature-gear`, declared in `pins.toml` (`[module.creature_gear]`) and wired in at build time (§7). Both halves attach through script hooks, so no core file is edited and a pin bump is followed by a rebuild rather than a rebase (§4, §7).
- **Nothing here needs client data.** The client resolves appearance locally from ids the server already sends: weapons through the 3 virtual item slots, armour through the mirror-image packet (§2). No `Data/*.MPQ` file is touched; a client data fork is only ever needed for artwork the stock client does not carry, which is orthogonal to the feature (§1) [V].

Consequence for the plan: the drop half is ordinary module logic over the module's own data (§5), and the stat half is the only genuinely new code (§6).

## 4. Ways to get it here, and the one taken

| Path | Core patch | What we add | Risk |
|---|---|---|---|
| **A. SQL only** | no | `creature_equip_template` rows + `creature.equipment_id = -1` for per-spawn random weapons; no drops, no stats | none; today's behaviour, just more data |
| **B. Module** (recommended) | no | one module: drop injection + stat derivation, SQL data for loadouts | module-only; two hooks where filters must be exact |
| **C. Carry a core patch** | **yes** (loot manager + appearance manager) | the same injection and outfit logic, compiled into the core | a permanent core diff: every pin bump is a rebase, and the patch is invisible to the module build |
| **D. Drop only what the creature already wears** | no | the module's drop injection over its own generated data: the candidates are the creature's own worn item entries — its three weapon-pool entries plus its eleven outfit entries, deduplicated — filtered by inventory type, bind-on-equip, quality and level, then chance-rolled per corpse (§3, §5) | the item set is fixed when the data is generated, not matched at runtime, so a creature drops exactly the gear it is wearing and nothing else |

Path B is available because both halves have clean module hooks (§5, §6); this repository took it, and D is the shape its drop half has. C is not offered: a permanent core diff to rebase on every pin bump buys nothing the module hooks do not already provide.

## 5. Dropping what it wears

| Hook | Fires | Creature reachable | Caveat |
|---|---|---|---|
| `MiscScript::OnAfterLootTemplateProcess(Loot*, …)` — `MiscScript.h:84`, called `LootMgr.cpp:563` | once per corpse loot fill, **before** group-threshold and `unlootedCount` finalisation | yes, via `loot->sourceWorldObjectGUID` | only runs when a template was processed: `Unit.cpp:13686` fills loot `if (uint32 lootid = creature->GetCreatureTemplate()->lootid)` — a mob with `lootid == 0` never gets here [V] |
| `UnitScript::OnUnitDeath(Unit*, Unit*)` — `Unit.cpp:13925` | once per death, after loot was filled (`:13687`), **before** the corpse is opened | yes, `unit->ToCreature()->loot` | fixed entries must respect `MAX_NR_LOOT_ITEMS 18` (`LootMgr.h:51`). They are **not** exempt from group threshold/roll logic: `Group::GroupLoot` re-scans the live `loot->items` list when the corpse is opened (`Group.cpp:1123`). The hook must also set `UNIT_DYNFLAG_LOOTABLE` itself — the flag is decided in `Unit::Kill` *before* the hook (`Unit.cpp:13819-13830`) — and a corpse the core never filled arrives without a resolvable `loot.lootOwnerGUID` [V] |

`Loot::AddItem(LootStoreItem const&)` is public (`LootMgr.h:380`) [V], and per-spawn variety is free: the virtual items were already chosen per spawn, so "drops what it has on" is deterministic for that instance.

**The injection is distinct from the creature's own roll.** The loot table runs first (the hook fires after `FillLoot` has processed the template), and the module then adds the worn items on top of it: its own chance (`CreatureGear.Drop.Chance`), its own cap (`CreatureGear.Drop.MaxItemsPerCorpse`), its own eligibility filter, and its own loot rows — `Loot::AddItem` pushes a new `LootItem` per stack and never merges by item id (`LootMgr.cpp:481-536`) [V], so a worn item is always its own entry in the loot window rather than a contribution to a naturally rolled one. Because the two sets can name the same item, the module drops a candidate from its list when the template has already rolled that item for the same corpse (`CreatureGearDrop.cpp`, `FillCorpseWithWornGear`), so everything it adds is an item the player would not otherwise have got from that corpse. For a creature without a loot template the corpse arrives with no loot at all and nothing is filtered [V].

**Bind-on-equip is not an `ItemFlags` bit.** It is `ItemTemplate::Bonding == BIND_WHEN_EQUIPPED` (`item_template.bonding = 2`; `ItemTemplate.h:97,663`), and the module's drop filter reads exactly that field (`CreatureGear.Drop.RequireBindOnEquip`, `CreatureGearMgr.cpp` `IsDropEligible`) so a dropped item is not bound to whoever loots it by accident [V].

Note for this stack: `mod-playerbots` picks grind targets partly on loot and **skips creatures whose `lootid` is 0** (`mod-playerbots/src/Ai/Base/Value/GrindTargetValue.cpp:67,194`, plus `AggressiveTargetValue.cpp:37`; the module reads loot and registers no loot hooks) [V]. Injecting gear drops for template-less mobs while the bots ignore those corpses would be inconsistent — the module should either require a real loot template or also give the creature a `lootid`.

The interesting half is the *selection*, and it is ours: the candidate list is the creature's own worn items — the three weapon-pool entries of its spawn plus the eleven outfit entries its variant carries, deduplicated — and `IsDropEligible` then requires an inventory type that is hand-equippable or a real outfit slot, bind-on-equip under the shipped default, quality no higher than `CreatureGear.Drop.MaxQuality` and RequiredLevel no higher than the creature's level plus `CreatureGear.Drop.LevelMargin` (`CreatureGearMgr.cpp` `GetDropCandidates`/`IsDropEligible`) [V]. Nothing is matched by display id at drop time: the drop is the worn set itself, filtered and then chance-rolled per corpse (`CreatureGearDrop.cpp` `FillCorpseWithWornGear`) [V].

## 6. Stats from worn gear — the new code

Ordering decides where each modifier goes (all in `Creature::UpdateEntry`/`SelectLevel`):

| Step | Site | Consequence |
|---|---|---|
| `SelectLevel()` → `OnCreatureSelectLevel` fires **last** | `Creature.cpp:1572` | equipment is already loaded (`:554`); safe place to read `GetVirtualItemId(0..2)` and item stats |
| core then writes **BASE_VALUE** for armour and the 6 resistances | `Creature.cpp:648-654` | anything written to ARMOR/RESISTANCE BASE_VALUE during `SelectLevel` is overwritten — use `TOTAL_VALUE`, or set armour in `OnCreatureAddWorld` (`Creature.cpp:338`) |
| `SetCanModifyStats(true)` then `UpdateAllStats()` | `Creature.cpp:657-658`, `StatSystem.cpp:1039-1053` | `SetStatFlatModifier(..., TOTAL_VALUE, x)` survives aura/item recalcs, because those re-push from unit mods [V] |
| respawn and summons call `SelectLevel()` again | `Creature.cpp:2108`, `SpellEffects.cpp:2577` | stats re-derive on every respawn for free, no bookkeeping |

Weapon speed and damage need the explicit refresh `ScriptedAI::SetEquipmentSlots()` already performs: `SetBaseWeaponDamage` + `ApplyAttackTimePercentMod` + `UpdateDamagePhysical` (`ScriptedCreature.cpp:557-581`) [V]. Reads are `GetVirtualItemId` / item template, i.e. O(3) per spawn — per-tick hooks (`CreatureScript::OnCreatureUpdate`, `UnitScript::OnUnitUpdate`) must not be used for this.

Useful property for stats: **visibility and stat source are independent.** Armour items in the 11 slots can contribute stats even when we do not render them (no mirror-image path needed), because the server never asked the client anything — we would keep the loadout in our own table and read `item_template` from it.

## 7. Delivery: database edits at build time

The release tarball carries `src/` and the installer points `SourceDirectory` at it, so the worldserver's own DB updater applies SQL on **every** boot (`Updates.EnableDatabases 7`, `Updates.AutoSetup 1`; world paths from `DBUpdater.cpp:137-146`, `UpdateFetcher.cpp:107-187`) [V]. Two slots exist for our data:

| Slot | Path | State | Shipped today? |
|---|---|---|---|
| CUSTOM | `$SRC_DIR/data/sql/custom/db_world` — already an `updates_include` row `('$/data/sql/custom/db_world','CUSTOM')` (`data/sql/base/db_world/updates_include.sql:38`) | `CUSTOM` | yes: `src/core/data/sql` is copied wholesale by the workflow |
| MODULE | `$SRC_DIR/modules/<name>/data/sql/<dir containing "world">` | `MODULE` | **yes** — `server-aarch64.yml` copies `modules/mod-creature-gear/data/sql` into `$PREFIX/src/modules/mod-creature-gear/data/sql`, and `--status` checks that tree (`thor-install.sh`) |

So the smallest wiring for pure data is **one `cp` in the workflow's "SQL sources" step** (into `data/sql/custom/db_world`), plus, optionally, adding that directory to the installer's hard-coded SQL check list (`thor-install.sh:1189`) so `--status` notices a regression. A module instead needs: a `src/` directory (module discovery requires it — `ConfigureModules.cmake:31-46`), a `pins.toml` entry, a clone+`cp` **before** configure so it lands in `AC_MODULES_LIST` (`modules/CMakeLists.txt:340-371`), and the same SQL `cp`. The shipped implementation took the module path: `pins.toml` has `[module.creature_gear]`, `server-aarch64.yml` copies the module tree into `src/core/modules/` before configure and its `data/sql` into `$PREFIX/src/modules/mod-creature-gear/data/sql`, and `thor-install.sh --from-source` fetches this repo's own tree as a tarball (`fetch_self_module`).

Rules the SQL has to follow [V]: updates are ordered by **filename** and names must be globally unique (`UpdateFetcher.cpp:95-99,522-524`); `CUSTOM` files must be re-appliable (`data/sql/custom/README.md`); applies are hashed in the `updates` table; a new release re-applies on the next worldserver start, and re-fetching on an already-provisioned device needs `--force` (`thor-install.sh:674-679`).

**"Translation layer" is the right shape** — and it is what shipped: a curated dataset in the repo (rules in `scripts/creature-gear/gear.toml`) plus a small generator (`scripts/creature-gear/gen_creature_gear.py`) that emits re-appliable SQL into the module's `data/sql/world/base`, exactly as `apps/DatabaseSquash` keeps generated SQL as repo data in the core repo [V]. The generated files use unique names and re-appliable statements (`ON DUPLICATE KEY UPDATE`, an `equipment_id = 0` guard), so the core updater replays them when their content changes. At no point does a client file enter this path.

## 8. Risks and unknowns

- **Balance**: per-spawn gear plus gear-derived stats changes mob difficulty; needs a config toggle (`CreatureGear.Enable`; the shipped module already gates all three implemented halves on it) and a bounded multiplier, and it interacts with native item scaling.
- **Mirror-image armour constraints** [R]: creature must use a player-race display model, `SetDisplayId` morphs drop the outfit, no enchants/gems, portraits can misrender.
- **Bot interaction**: playerbots skip `lootid == 0` creatures as grind targets (§5), and they are real `Player` objects, so their own gear is unrelated to this feature [V].
- Uncertain: whether a client renders a non-hand item placed in one of the 3 virtual slots sensibly (the whitelist only guards the DB load path, not a direct `SetUInt32Value`) [I]. Assume it does not; use the mirror-image path for armour.

## 9. Suggested order of work

| Phase | Content | Acceptance |
|---|---|---|
| A | SQL loadouts: `creature_equip_template` pools + `creature.equipment_id = -1` for chosen humanoids; module per-respawn re-roll (`CreatureGear.Weapons.RerollOnRespawn`); MODULE SQL delivery wiring | the server boots with no `Unknown item (ID=` / `is not equipable in a hand` load errors; the pool varies between spawn points (variety across *respawns* needs the module re-roll — see §10, correction 2); a fresh install applies the SQL on first boot; `--status` clean |
| B | Module: drop what it wears, weapons (and armour displays when a preset/outfit is known), configurable chance (`CreatureGear.Drop.Enable`, `CreatureGear.Drop.Chance`), dedupe, level/quality gates (`CreatureGear.Drop.MaxQuality`, `CreatureGear.Drop.LevelMargin`) | kill in world, observe the worn item dropping; bots unaffected; no loot-window overflow |
| C | Armour appearance for selected humanoids (mirror-image module, own outfit table; `CreatureGear.Appearance.Enable`) | on the device, an NPC shows an 11-slot outfit that matches its stored loadout |
| D | Stats from gear, toggleable (`CreatureGear.Enable`), bounded | mob damage/armour/AP track its loadout; respawn re-derives; difficulty stays inside the configured band |

Each phase ships alone; C and D are optional, and only D is genuinely new code with no reference implementation.

The real config keys are the `CreatureGear.*` options in `modules/mod-creature-gear/conf/mod-creature-gear.conf.dist` (installed as `<prefix>/etc/modules/mod-creature-gear.conf`): `CreatureGear.Enable` (whole module), `CreatureGear.Debug`, `CreatureGear.Weapons.RerollOnRespawn`, `CreatureGear.Drop.{Enable,Chance,MaxQuality,RequireBindOnEquip,IncludeNoLootTemplate,MaxItemsPerCorpse,LevelMargin}`, `CreatureGear.Appearance.{Enable,VaryPerSpawn}`. They are read by the module's `WorldScript::OnAfterConfigLoad`, so `.reload config` applies them without a restart [V].

## 10. Where the implementation diverges from this document

Nine places where the code, written against the pinned core, does something the earlier design text did not say. Each is verified in source at the pin.

| # | What this document said | What the code (and the core) actually does | Evidence |
|---|---|---|---|
| 1 | Implied a template-level equipment slot; §2's mechanism row named only `creature_equip_template` → the 3 virtual slots | `creature_template` has **no `equipment_id` column** on this pin. Equipment is chosen on the **`creature` spawn row** (`creature.equipment_id`, signed `tinyint`), and pool rows in `creature_equip_template` are keyed by the creature's **own entry** (`CreatureID`, contiguous `ID = 1..n`). `0` = no equipment (no fallback), `-1` = random variant, `n` = variant `n` | `ObjectMgr.cpp:1485-1515` (loader; id 0 skipped), `ObjectMgr.cpp:1454-1470` (`GetEquipmentInfo` picks a variant for `-1`) [V] |
| 2 | Phase A acceptance: "the same spawn point shows varied weapons across respawns" | Not true for stock `-1`: the core resolves `-1` **in place once per worldserver start** (`LoadCreatureData` → `GetEquipmentInfo(entry, int8&)`) and stores the concrete index, so `Creature::GetOriginalEquipmentId()` is never observed as `-1` afterwards. Per-respawn variety therefore needs the module's `creature->LoadEquipment(-1)`, scoped to entries covered by `mod_creature_gear_outfit`, with `CreatureGear.Weapons.RerollOnRespawn = 0` disabling it | `ObjectMgr.cpp:2429-2438`, `Creature.cpp:553-554`; module `CreatureGearMgr.cpp` (`ShouldRerollWeapons`; the hooks are correction 7); `conf/mod-creature-gear.conf.dist` [V] |
| 3 | Treated bind-on-equip as an `ItemFlags` bit | It is **not**: it is `ItemTemplate::Bonding == BIND_WHEN_EQUIPPED` (`item_template.bonding = 2`). The module's drop filter reads that field | `ItemTemplate.h:97,663`; module `CreatureGearMgr.cpp` (`IsDropEligible`), `CreatureGear.Drop.RequireBindOnEquip` [V] |
| 4 | §5: `OnUnitDeath` entries "skip threshold semantics" | Not exempt. `Group::GroupLoot` re-scans the **live** `loot->items` list when the corpse is opened, so items injected at `OnUnitDeath` enter group threshold/roll logic normally. The hook must additionally set `UNIT_DYNFLAG_LOOTABLE` (the flag is decided in `Unit::Kill` **before** the hook) and a corpse the core never filled needs a resolvable `loot.lootOwnerGUID`; the loot window is still sent on open, not at death | `Group.cpp:1123`; `Unit.cpp:13819-13830`; module `CreatureGearDrop.cpp` (`CreatureGearDeathScript::OnUnitDeath`) [V] |
| 5 | §1/§2: "mirror-image path: `UNIT_FLAG2_MIRROR_IMAGE` + `SMSG_MIRRORIMAGE_DATA`", as if the core could build it | The only `SMSG_MIRRORIMAGE_DATA` builder in this core is the clone-caster handler (68 bytes, 11 display ids), there is **no helper** for arbitrary units and **no update field** carrying the 11 ids for a creature. The module therefore sets a **player-race model** (`ChrRaces.model_m/model_f`, race derived from the creature's own baked display via `CreatureDisplayInfo` → `CreatureDisplayInfoExtra.DisplayRaceID`), sets `UNIT_FLAG2_MIRROR_IMAGE`, and answers `CMSG_GET_MIRRORIMAGE_DATA` itself from `ServerScript::CanPacketReceive` (returning `false` to suppress the core handler); the packet is rebuilt per requester | `Handlers/SpellHandler.cpp:768-834`; module `CreatureGearAppearance.cpp:32-105` (resolve), `:114-192` (apply + both hooks), `:194-268` (packet) [V] |
| 6 | Phase A spawn SQL was described as an edit against the core's **base** schema (`creature.id1`/`id2`) | The statement targets the schema **after** the update chain — `UPDATE \`creature\` SET \`equipment_id\` = -1 WHERE \`id\` = <entry> AND \`equipment_id\` = 0` — with no `id2` filter, because `2026_06_16_00.sql` renames `id1` → `id` and moves `id2`/`id3` into `creature_multispawn`, and module SQL is applied in the `MODULE` pass *after* the `RELEASED` pass. The column names and the filter are **derived from `data/sql/updates/db_world/*.sql` in filename order**, not hard-coded; the validator fails on base-only column names (naming `id1`/`id2` is a hard failure) | `data/sql/world/base/creature_gear_40_spawn_equipment.sql` header; generator `spawn_update_statement` / `SpawnColumnForm` and the `data/sql/updates/db_world` walker (`scripts/creature-gear/gen_creature_gear.py`); validator `parse_spawn_updates` (`scripts/creature-gear/check_creature_gear.py`) [V] |
| 7 | Phase A re-roll was described as an entry/respawn re-roll only | It is scoped to entries the module's own outfit data covers (`ShouldRerollWeapons` → `GetOutfit(entry) != nullptr`) and is called from **both** `AllCreatureScript::OnCreatureAddWorld` and `OnCreatureSelectLevel`, because the core's default compatibility respawn mode re-runs `SelectLevel()` (`Creature.cpp:2108`) without re-adding the creature to the world; the double roll on first creation is harmless (last one wins) | module `CreatureGearMgr.cpp` (`RerollWeaponsIfNeeded`, both script overrides) and `creature_gear_common.h` (`CreatureGearWeaponScript`); `Creature.cpp:2108` [V] |
| 8 | Phase C was described as setting the model and the flag when the creature enters the world | It is applied from **both** `OnCreatureAddWorld` and `OnCreatureSelectLevel`, because `SelectLevel()` is the core's single funnel for every later appearance change: creation reaches it through `Creature::Create` → `UpdateEntry` (before the creature enters the world, and the display is already the spawn's own model by then), a compatibility-mode respawn re-runs it, and an entry switch runs it from `UpdateEntry` as well. The respawn path's own display reset re-applies `GetNativeDisplayId()`, which is the model this module pinned, and the pinned `creature_model_info` leaves `DisplayID_Other_Gender = 0` on every `ChrRaces` model, so that reset's gender randomiser cannot move the display away from it. The per-entry record, not the hook order, makes the second call a no-op — and the packet path answers only while `GetDisplayId()` equals the recorded model, so an entry switched under the module cannot put this entry's outfit over a foreign body | module `CreatureGearAppearance.cpp` (`ApplyAppearance` + both overrides, `CanPacketReceive` guard), `creature_gear_common.h` (`CreatureGearAppearanceScript`); `Creature.cpp:631,1225-1231,1512,1572,1683,2108,2114-2121`; `ObjectMgr.cpp:1678-1707` [V] |
| 9 | Phase A was described as one `equipment_id = -1` statement per covered entry (and its build-time counts as the whole story) | `-1` is resolved once per spawn, against **one** of the entries that spawn can become, and the resolved id is then looked up for the others too, so an alternate entry without that pool row made the core log `Table 'creature_multispawn' has creature (SpawnId: N) with entry E where equipment_id K not found in 'creature_equip_template'`. Phase A is now three statements: `-1` only for spawns with no alternate entry, pool row 1 where every alternate has an `ID = 1` row, and `0` where one does not (9,869 / 18 / 30 spawns in the shipped dataset). They own `equipment_id` values `0` and `-1`, so re-applying them repairs a database that ran the previous generation. The guard is a `creature_multispawn` subquery, so an update that adds an alternate later cannot reintroduce the mismatch | `ObjectMgr.cpp:2524-2547` (boot check), `ObjectMgr.cpp:2646-2658` (per-spawn load), `ObjectMgr.cpp:1454-1470` (`-1` resolution); generator `spawn_statements`, `creature_gear_40_spawn_equipment.sql` header [V] |

---

## 11. Per-spawn faces and hair

Every outfit row carries five customisation bytes (`Skin`, `Face`, `HairStyle`, `HairColor`, `FacialHair`) and the generated dataset leaves all five at 0, so without more every instance of an entry morphs to the same default face. `CreatureGear.Appearance.VaryPerSpawn` (default 1) samples them per spawn point instead.

| Fact | Detail | Evidence |
|---|---|---|
| The client validates the mirror-image bytes against `CharSections.dbc`, and the core already loads that file | `sCharSectionsStore` (`DBCStores.cpp:287`); section enum `SECTION_TYPE_SKIN/FACE/FACIAL_HAIR/HAIR` (`DBCStructure.h:637-644`); row layout `"diiixxxiii"` = RaceID, SexID, BaseSection, flags, 3 textures, VariationIndex, ColorIndex (`DBCfmt.h:32`) | [V] |
| How much the pinned data offers | 42 race/gender pairs; for the playable races 10-24 hair styles, 10-13 hair colours, 10-30 faces, 4-25 skin colours, and facial-hair rows only where the data has them (none for most female races) | boot log of the derived table, and `CharSections.dbc` parsed directly [V] |
| Only real rows are sent | the sampler draws whole rows, so the (VariationIndex, ColorIndex) pair always exists — section 1 is not a full cross product (249 of 24x15 combinations for human male), which is why face and skin come from the same row | `CharSections.dbc` parse [V] |
| Every pickable hair style has a mesh | `CharSections` section 3's `VariationIndex` set equals `CharHairGeosets.dbc`'s style set for 19 of 20 playable race/gender pairs and is a subset for the 20th (blood-elf female: 0-18 of 0-19), so no pick can be a bald style | both DBCs parsed and compared [V] |
| Deterministic per spawn point | seeded with `Creature::GetSpawnId()` (GUID fallback) and mixed by a pure function: no RNG state, no cache, and the morph path and the packet path derive the same bytes independently | `CreatureGearMgr.cpp` (`MixAppearanceSeed`, `GetCustomisation`) [V] |
| Row columns still win | a non-zero column pins that field for the whole entry, so a named NPC can keep one fixed face while its neighbours vary | `GetCustomisation` [V] |

Observed on the dev workstation, two consecutive boots against the same databases with `Logger.module=5`: the module logged the derived option counts for all 42 pairs; entry 14121's three spawn points were given three different face/hair sets (e.g. `skin 5 face 9 hair 3 colour 0` beside `skin 8 face 0 hair 0 colour 2`); and all five spawn points seen in both boots reproduced byte-identical values, so the look is stable per spawn point and varied between them. The client's rendering of those bytes is still unverified — that needs the device, like the rest of phase C.

Deliberately not varied: **gender**. `Outfit::gender` is 0 = male with no "unset" value to distinguish, so varying sex per spawn would need a sentinel (say 2 = vary) and would move the model choice (`ChrRaces.model_m` vs `model_f`) out of the per-entry record and onto the creature. That plumbing is left out until the data asks for it.

---

## 12. Per-spawn armour variants

§11 varies the face and hair on one outfit; this varies the armour itself. `mod_creature_gear_outfit` stays one row per creature entry holding all eleven slots, and `mod_creature_gear_outfit_variant` overrides five of them — HEAD, CHEST, LEGS, FEET, HANDS (helm, body armour, pants, boots, gloves) — with one row per variant. `armour_variants = 4` in `scripts/creature-gear/gear.toml` is the variant count.

| Fact | Detail | Evidence |
|---|---|---|
| The pick is per spawn point, not per creature | `index = spawnId % variantCount` (GUID fallback for summons), the same seed family as §11, so one spawn point keeps one set across restarts and respawns while neighbouring spawn points of an entry differ | module `CreatureGearMgr.cpp` (`OutfitVariantIndex`, `GetOutfit`) [V] |
| Appearance and drop read that same pick | both halves call `Manager::GetOutfit(creature)` — the appearance when it morphs the creature, the drop when it assembles candidates — so the armour a corpse yields is the armour its spawn point shows | `CreatureGearAppearance.cpp`, `CreatureGearMgr.cpp` (`GetDropCandidates`) [V] |
| The other six slots stay per entry | shoulders, shirt, belt, bracers, cloak and tabard come from the outfit row, so an entry keeps its silhouette across all its spawns | generator `VARIED_OUTFIT_SLOTS` [V] |
| Variants differ by construction | a varying slot's candidate index advances by one per variant (`entry + slot + variant`), so a slot with at least four candidates yields four different items; the validator re-derives every slot's candidate list from the item dump and fails when the picks are less distinct than `min(armour_variants, candidates)` | generator `pick_outfit_slot`, validator `check_outfit_variants` [V] |
| Variant 1 is the outfit row | the generator fills the row's five varying slots from variant 1 and the validator asserts it, so a database carrying the outfit row but no variant rows behaves exactly as before this section | `check_outfit_variants` [V] |
| The rows carry items, not display ids | the module resolves each variant item to its `ItemDisplayInfo` at load, so the shown armour and the droppable armour stay one set of items | `CreatureGearMgr.cpp` (`ResolveOutfitSlot`) [V] |

Generated data, from the validator: 5096 variant rows for 1274 creatures, and every creature reaches full width — a mean of 4.00 distinct items in each of the five varying slots, `full width 1274/1274` for all five. The dataset is uncapped (`max_creatures = 0` in `gear.toml`), so it covers every creature the rules admit; the bind-on-equip floor moved with it, because the whole-set measure is band-limited rather than ordering-limited: 72.29% of the covered creatures wear a droppable item, against the 90.00% the 400-creature slice reached before the uncapping, and `min_boe_coverage` is 0.70 accordingly.

Live on the dev workstation, three boots against the same databases (the 400-creature dataset then shipped; it has since been uncapped, see the status table) — twice with a temporary probe compiled into the worldserver, once with the artifact as shipped — all logged `loaded 400 outfit row(s) and 1600 variant row(s) for 400 creature(s)` and zero ERROR lines, and every spawn point that appeared in more than one boot carried the same variant and the same five items byte for byte. The spawn points whose grid loaded in the shipped run wore:

| Entry | Spawn id | Variant | helm | chest | legs | feet | hands |
|---|---|---|---|---|---|---|---|
| 14121 | 53789 | 2/4 | 2994 | 9774 | 17006 | 2315 | 4318 |
| 14121 | 53790 | 3/4 | 2995 | 3053 | 1934 | 4313 | 9795 |
| 14121 | 53791 | 4/4 | 5625 | 3061 | 2166 | 4788 | 14377 |
| 3182 | 9526 | 3/4 | 3059 | 1753 | 2152 | 2156 | 6605 |
| 1484 | 9569 | 2/4 | 5307 | 2869 | 1934 | 4313 | 16986 |
| 3371 | 4678 | 3/4 | 4393 | 5739 | 16165 | 7916 | 2434 |
| 3372 | 7480 | 1/4 | 3345 | 1535 | 16118 | 16157 | 2422 |
| 6466 | 12325 | 2/4 | 1279 | 846 | 15303 | 26011 | 15401 |

Three spawn points of one entry, three different five-piece sets — and every logged variant number equals `spawnId % 4 + 1`, on all eight samples. That the drop follows the same choice was visible in the same runs: `GetDropCandidates` returned more items than the three worn weapon slots for outfit creatures, and for entry 1484's spawn 9569 the item it drew was `1934`, which is exactly that spawn's variant 2/4 legs item.

Two limits. The client's rendering of those five pieces still needs the device, like the rest of phase C. And because the choice is a function of the spawn id, an entry with a *single* spawn point always shows the same variant; per-respawn variety for those would need the choice stored on the creature instance (the way the core keeps `m_equipmentId`) instead of derived, with a hook to re-pick it.

## 13. Class-appropriate gear

Class blindness was the largest realism gap left: candidate pools were per level band only, `creature_template.unit_class` was read by nothing, and a mage-class caster could draw a two-handed axe and wear plate. Measured on the previous dataset by re-deriving every pick's `item_template.subclass` against the client's proficiency data: **693 of the 1274 creatures (54%) had a weapon in a pool their class cannot use (1146 items), and 1069 (84%) wore at least one armour piece outside their class's chain (2263 pieces)** [V].

The rules now live in `scripts/creature-gear/gear.toml`, one `[classes.N]` table per ChrClasses id, and the generator applies them while it builds a candidate list — nothing new reaches the server, and the module needs no change:

| Rule | Where it comes from | Detail |
|---|---|---|
| Weapon proficiencies | the client's own DBCs | `[classes.N].weapons` lists the `item_template.subclass` ids the class may use, cross-checked against `SkillLine.dbc` + `SkillLineAbility.dbc` (skill line → classMask): mage and warlock swords, staves, daggers, wands; priest maces, staves, daggers, wands; druid maces (1H/2H), staves, daggers, fist, polearm; paladin axes, swords, maces (1H/2H), polearm; hunter axes and swords (1H/2H), polearm, staff, dagger, fist, bow/gun/crossbow; shaman axes and maces (1H/2H), staff, dagger, fist; warrior every melee type plus bow/gun/crossbow/thrown; death knight axes, swords, maces (1H/2H) and polearm [V] |
| `AllowableClass` | `item_template` | an item is kept when its mask is `-1` or carries the class's bit, so class-restricted gear only ever lands on its class [V] |
| Armour chain | class progression | `[classes.N].armour` is a chain of `[subclass, minimum level]`: warrior/paladin/DK plate at 40, mail at 40, leather, cloth; hunter/shaman mail at 40, leather, cloth; rogue/druid leather, cloth; priest/mage/warlock cloth. The generator wears the first chain entry the creature's level reaches that has a candidate in every armour slot; a slot that entry cannot fill falls back along the rest of the chain, which is what keeps a level-10 leather creature's helm (no leather helm exists in that band) off its bare head [V] |
| Shields | a class flag | shields are `AllowableClass = -1` in this data, so `[classes.N].shield` (paladin, shaman, warrior) is the only thing that keeps one out of a mage's `ItemID2` [V] |
| Stat focus | what the class's gear looks like | `melee` drops items carrying a caster-group stat, `caster` the reverse, `either` neither; the groups are the melee- and spell-specific rating ids from the core's `ItemModType` (`ItemTemplate.h`), leaving the generic hit/crit/haste ratings, stamina, resilience and the "taken" family neutral. Candidates then sort bind-on-equip first, focus stat, statless, neutral [V] |
| Class with no table | fallback | a `unit_class` of 0 or an unmodelled id falls back to the warrior row; the generator prints how many did (0 in the shipped dataset) [V] |

Two consequences worth stating plainly.

**Under the shipped rules the caster pools are mostly statless.** The intent is "intellect swords and maces"; the data does not offer that at `quality_max = 2`. For mage-class creatures at level 20 the main-hand pool is 38 items, of which **0** carry intellect, spirit or spell power, 12 carry strength or agility and 18 are statless — random-suffix bases whose stats are rolled in play. Demanding a caster stat would empty the hand, so the rule refuses the contrary items and orders the rest instead. Raising `quality_max` is what brings intellect gear into range (the same pool is 67 items with 4 caster-stat ones at `quality_max = 3`) [V]. Mage-class creatures get swords, staves, daggers and wands: maces are not a mage proficiency in 3.3.5, which the client data confirms (maces go to priest, druid, paladin, rogue, shaman, warrior and DK) [V].

**A role a class cannot fill is not emitted, and its old rows are deleted.** A paladin has no ranged proficiency, so it gets two `creature_equip_template` rows instead of three and `pool_size` is now the maximum number of roles. Because `ObjectMgr::GetEquipmentInfo` resolves `equipment_id = -1` by picking at random among the rows *present* for the entry (`ObjectMgr.cpp:1463-1467`), a replayed database would otherwise keep whatever a previous dataset wrote there and hand the paladin its old bow. The generated file therefore deletes its own unused IDs for those creatures (`WHERE CreatureID = ? AND ID > n AND ID <= pool_size`): never `ID <= n`, which is the row this dataset writes, and never above the module's range, because two of the 1274 creatures (entries 18321 and 27361) carry unrelated rows of their own in `creature_equip_template` [V].

## 14. Spawns that alternate entries

`creature_multispawn` lets one spawn point rotate between creature entries: the update chain moves
the base dumps' `id2`/`id3` into that table (`2026_06_16_00.sql`), so spawn 39353 is entry 11552 on
one respawn and 11553 on the next. The equipment id is **not** per entry — it is one value on the
spawn row, and `ObjectMgr::LoadCreatures` resolves `-1` against the entry it happens to load
(`GetEquipmentInfo(entry, int8&)` picks at random among the rows *present* for that entry) and then
validates the resulting concrete id against every alternate:

```
Table 'creature_multispawn' has creature (SpawnId: 39353) with entry 11553
where equipment_id 2 not found in 'creature_equip_template'.
```

Entry 11552 is covered by the dataset (pool IDs 1..3); 11553 is not, and has no
`creature_equip_template` rows at all. Handing the spawn the whole pool therefore only works when
the spawn cannot change entry, and phase A is split in two:

| Spawn | Statement | Why |
|---|---|---|
| No `creature_multispawn` row | `equipment_id = -1` | the pool is resolved against the only entry the spawn can be |
| Has one, and every alternate entry has an `ID = 1` row | `equipment_id = 1` | one id shared by all of them, and pool row 1 is the row every generated creature has |
| Has one, and an alternate has no `ID = 1` row | set to `0` | no id is valid for the whole alternation; the covered entry still gets the module's rerolled pool and outfit, and an uncovered alternate simply carries no worn weapon |

All three guard with a `creature_multispawn` subquery rather than with a list computed at
build time, so an update that adds an alternate to an already-geared spawn is handled on the
database it runs against. The build-time counts in the file header (9,869 / 18 / 30 for the shipped
dataset) describe a fresh install; the guards decide the rest. They own `equipment_id` values `0` and
`-1` — `0` is unset, `-1` is what the previous generation of this dataset wrote — so re-applying them
repairs a database that already ran it, which is what a `--repair` or a newer artifact lands on. Any
other value is hand-set gear and is never touched.

The pin costs nothing visually. `ShouldRerollWeapons` re-rolls the pool for every entry the module
has outfit data for (`creature->LoadEquipment(-1)` on `OnCreatureAddWorld` and
`OnCreatureSelectLevel`), so a covered creature's weapons are resolved per creature regardless of
the spawn row's value; the pinned id only decides what an *uncovered* alternate shows — its stock
row-1 gear where the dataset gives it one, where the old `-1` resolved to nothing at all and
logged the error.
