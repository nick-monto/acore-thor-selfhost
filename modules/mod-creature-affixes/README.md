# mod-creature-affixes

The initial server slice for the creature prefix/affix system.

## Definitions

`mod_creature_affixes_modifier` is the static pool. The table and initial rows
ship in:

- `data/sql/world/base/creature_affixes_10_schema.sql`
- `data/sql/world/base/creature_affixes_20_modifiers.sql`

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
applies the effect. Damage spells `100204` and `100206` are server-only too,
but use stock visuals so the client still shows the gas and Fel Lightning
impacts.

## Initial rows

| Modifier | Kind | Client-visible carrier | Effect |
| --- | --- | --- | --- |
| `Vicious` | Prefix | `69737` CP Test Spell 2 | +12% physical damage |
| `Gilded` | Prefix | `67828` Dummy Riding Aura | one extra creature loot roll |
| `of the Bear` | Affix | `71432` Mote of Anger | +15% maximum health |

## Candidate rows

| Modifier | Kind | Carrier | Effect spells |
| --- | --- | --- | --- |
| `Zombified` | Prefix | `34463` Debug Debuff | `100200` attack/move −20%, `100201` health −20%, `100203` miasma trigger, `25039` gas visual |
| `of the Corruption` | Affix | `29479` Unscathed (DND) | `100202` attack speed +20%, `100205` Fel Lightning proc, `60451` Corruption of Time visual |

`100203` ticks every 3 seconds and triggers `100204`: `1 + 0.25 × (mob level − 1)`
shadow damage in an 8-yard enemy area. `100205` is a 40% melee proc that
triggers `100206`: `4 + 2 × (mob level − 1)` shadow/fire direct damage. Both
use `EffectRealPointsPerLevel`, so a level 1 mob deals 1 (gas) and 4 (proc),
while a level 80 mob deals 20 and 162. The old stock `12187`/`44537` damage
spells are not used.

The custom effect spell ids are server-only: the client has no `Spell.dbc`
rows for `100200`–`100206`, so the stock combat log has no name or tooltip for
those ticks. Verify the miasma on the player health bar; a client `Spell.dbc`
patch is needed for combat-log text. A stock-spell substitute would put the
effect in client data at the cost of changing the spell ids.

The carriers are stock permanent dummy auras with Attributes `0`, no spell
visual and no world-database references. They must stay client-visible:
`SPELL_ATTR0_DO_NOT_DISPLAY` removes the aura from `UnitAura` as well as the
UI, so the add-on cannot read it. The add-on reads the names through
`UnitAura` and hides the carriers' target-frame buttons; the data file must
use the same names.

`creature_affixes_30_visible_carriers.sql` moves a database that already applied
the old hidden seed; its guards leave an operator's own carrier choice alone.

The other design rows are intentionally absent until the first slice is proven.

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
carrier auras themselves stay hidden. Stock nameplates still use the client's
entry-keyed name and are not changed by this add-on.

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
