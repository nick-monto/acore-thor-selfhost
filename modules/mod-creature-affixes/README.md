# mod-creature-affixes

The server-side creature prefix/affix system.

## Definitions

`mod_creature_affixes_modifier` is the static pool. The table, the three proof
rows and the expanded pool ship in:

- `data/sql/world/base/creature_affixes_10_schema.sql`
- `data/sql/world/base/creature_affixes_20_modifiers.sql`
- `data/sql/world/base/creature_affixes_80_expanded_pool.sql`

The worldserver loads the table once at startup (`Manager::LoadModifiers`) and
keeps per-spawn rolls in memory. The table is not written on spawn, death or
respawn; editing a row requires a restart. A row is only rollable when its
`effect` value is implemented in `CreatureAffixes.cpp`, or when it uses
`effect = 'none'` and gets its behavior from the effect-spell table below.

## Effect spells

`mod_creature_affixes_modifier_effect` attaches permanent spells to a modifier:

- `role = 'aura'`: a stat, periodic damage, proc or transform aura.
- `role = 'visual'`: a spell used only for its visual.

`carrier_spell` remains the one visible dummy marker the add-on hides. Effect
spells are applied with `AddAura` and removed by the creature's caster GUID,
so a natural aura with the same spell id survives. The loader rejects passive
or non-permanent effect rows at startup.

Stock spells are preferred where they are already level-scaled. The candidate
stat and trigger auras (`100200`, `100201`, `100202`, `100203`, `100205`) are
server-only `spell_dbc` rows: no visual, no icon, and
`DO_NOT_DISPLAY|DO_NOT_LOG`, so the client has nothing to draw while the server
applies the effect. The miasma is a server-only area aura. `100206` is a
server-only damage spell too: its row carries the stock `SpellVisualID` `9966`,
but a visual id inside a custom `spell_dbc` row is server-side data only. The
client has no `Spell.dbc` row for `100206`, so no gas and no Fel Lightning
impact is proven or expected to draw; a client `Spell.dbc` row is required
before either can be claimed. The visible gas comes from the separate stock
spell `25039`, which the client already knows, in the modifier's `visual` row.

## Initial rows

| Modifier | Kind | Client-visible carrier | Effect |
| --- | --- | --- | --- |
| `Vicious` | Prefix | `69737` CP Test Spell 2 | +12% physical damage |
| `Gilded` | Prefix | `67828` Dummy Riding Aura | one extra creature loot roll |
| `of the Bear` | Affix | `71432` Mote of Anger | +15% maximum health |

## Complex proof rows

| Modifier | Kind | Carrier | Effect spells |
| --- | --- | --- | --- |
| `Zombified` | Prefix | `34463` Debug Debuff | `100200` melee haste −20%, movement −40%; `100201` health −20%; `100203` miasma area aura; `100207` attack damage +30%; `25039` gas visual |
| `of the Corruption` | Affix | `29479` Unscathed (DND) | `100202` attack speed +20%, `100205` Fel Lightning proc, `60451` Corruption of Time visual |

`100203` is a permanent 10-yard enemy area aura. It deals 2% of each victim's
maximum health every 3 seconds below level 10, 4% at levels 10–19, and 6% at
level 20 and above. `ZombifiedMiasmaAuraScript` sets that step from the victim's
level before each tick; the row's base amount is the 2% fallback. `100200` slows
movement by 40% and melee haste by 20%; `100207` adds 30% physical attack
damage to offset those penalties. `100205` is a 40% melee proc that triggers
`100206`: `4 + 2 × (mob level − 1)` shadow/fire direct damage. A level 1 mob
deals 4 (proc) and a level 80 mob deals 162. The old stock `12187`/`44537`
damage spells are not used.

All custom `spell_dbc` rows set `EquippedItemClass = -1`; the column default
`0` is an item-class requirement and breaks normal casts with
`SPELL_FAILED_EQUIPPED_ITEM_CLASS`.

## Expanded pool

The pool now has fifteen prefixes and fifteen affixes. Five of each are complex
rows made from two or three stat/area auras plus a stock visual:

| Kind | Simple | Complex |
| --- | --- | --- |
| Prefix | Vicious, Gilded, Cruel, Sturdy, Frenzied, Sluggish, Arcane, Warded, Swift, Blessed | Zombified, Feral, Blazing, Frostbound, Venomous |
| Affix | of the Bear, of the Ox, of the Wolf, of the Falcon, of the Turtle, of the Serpent, of the Moon, of the Boar, of the Wraith, of the Colossus | of the Corruption, of the Inferno, of the Glacier, of the Plague, of the Storm |

Gilded gains the stock `57718` Harpoon Loot Sparkles visual. The other complex
rows use `35471`, `45421`, `40158`, `35468`, `55928`, `47706`, `49702` and
`45213`; all are stock permanent visuals, so the client needs no Spell.dbc
patch. The data-driven simple rows use one hidden custom aura (100300–100312)
plus one stock visual cue; Vicious, Gilded and of the Bear keep their native
effects. No new database schema or client DBC rows are introduced.

| Simple modifier | Stock visual cue |
| --- | --- |
| Vicious | Blood Orb State Visual |
| Cruel | Display Blood Globe |
| Sturdy | Stoneform |
| Frenzied | Enrage |
| Sluggish | Dazed and Confused |
| Arcane | Arcane State |
| Warded | Cosmetic - Arcane Force Shield (Blue) |
| Swift | Cosmetic - Combat Whirlwind |
| Blessed | Holy Zone Visual |
| of the Bear | Icebound Fortitude |
| of the Ox | Power Spark |
| of the Wolf | Frenzied Bloodthirst Visual |
| of the Falcon | Gusty Prison |
| of the Turtle | Shield of the Lich King |
| of the Serpent | Blight Spill |
| of the Moon | Star Beam |
| of the Boar | Spotlight |
| of the Wraith | Cosmetic - Purple Ghost Aura |
| of the Colossus | Wild Growth |

The custom effect spell ids are server-only: the client has no `Spell.dbc`
rows for `100200`–`100203` or `100205`–`100207`, so the stock combat log has
no name or tooltip for those ticks. Verify the miasma on the player health
bar; a client `Spell.dbc` patch is needed for combat-log text. A stock-spell
substitute would put the effect in client data at the cost of changing the
spell ids.

The carriers are stock permanent dummy auras with Attributes `0`, no spell
visual and no world-database references. They must stay client-visible:
`SPELL_ATTR0_DO_NOT_DISPLAY` removes the aura from `UnitAura` as well as the
UI, so the add-on cannot read it. The add-on reads the names through
`UnitAura` and hides the carriers' target-frame buttons; the data file must
use the same names.

`creature_affixes_30_visible_carriers.sql` moves a database that already applied
the old hidden seed; its guards leave an operator's own carrier choice alone.

The carrier rows added by `creature_affixes_80_expanded_pool.sql` follow the
same visibility rule. The add-on marker table contains all thirty carrier
names and every stock visual name.

## Hidden size layer

The size layer rolls independently of prefix/affix; a creature may have both.
It never receives a marker aura, so the add-on does not show it.

| Layer | Scale |
| --- | --- |
| Tiny | `0.70`–`0.85` |
| Giant | `1.15`–`1.30` |

`Creature::SetObjectScale` updates `OBJECT_FIELD_SCALE_X`,
`UNIT_FIELD_BOUNDINGRADIUS` and `UNIT_FIELD_COMBATREACH` together
(`Creature.cpp:3591-3607`), so the visual size, hit-box and melee reach stay in
sync. `Creature::Respawn` can reset the display after `OnCreatureSelectLevel`;
the module re-applies the stored size factor from `OnDisplayIdChange` and
`OnCreatureAddWorld`.

Size rolls only on outdoor maps by default.

## Client add-on

`Creature::SetName` changes the server-side name used by logs and scripts, but
the 3.3.5a client resolves creature names from entry-keyed query data, so a
live `SetName` does not update the target frame. The add-on composes the
target/focus header from the carrier auras as `%prefix %name %affix`; the
carrier auras themselves stay hidden. For nameplates, the native adapter
publishes each `CGNamePlateFrame` GUID and the add-on asks this module over a
`CAFF labels` add-on whisper for the labels belonging to that GUID. Stock
clients without the adapter keep the target/mouseover fallback.

The add-on hides the carriers' target-frame buttons and has no aura tooltip
hooks. Marker auras still exist so that suppression and the header labels have
a stable, client-visible name to match; the carrier name maps to a display
label in `CreatureAffixes_Data.lua`.

## Eligibility

Only world-spawned creatures (`creature.GetSpawnId() != 0`) are considered.
The module skips critters, civilians, quest givers, vendors, trainers, totems,
vehicles, pets, dungeon bosses and world bosses.

## Configuration

`conf/mod-creature-affixes.conf.dist` is installed as
`<prefix>/etc/modules/mod-creature-affixes.conf`. The world script reads it in
`OnAfterConfigLoad`, so `.reload config` applies chance/size settings without a
restart. Modifier rows are loaded once at world startup; restart after editing
`mod_creature_affixes_modifier`.

## GM test command

```text
.affix show
.affix set vicious bear giant
.affix set gilded none tiny
.affix set zombified corrupted
.affix set none none none
```

`show` prints the selected creature's current and base names, prefix, affix,
size factor, maximum health, native/current scale, bounding radius and combat
reach. `set` forces a state on the selected creature; the size argument is
optional and defaults to `none`.

## Loader

Folder `mod-creature-affixes` -> loader symbol
`Addmod_creature_affixesScripts` in `src/creature_affixes_loader.cpp`.
