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
`effect` value is implemented in `CreatureAffixes.cpp`.

## Initial rows

| Modifier | Kind | Client-visible carrier | Effect |
| --- | --- | --- | --- |
| `Vicious` | Prefix | `69737` CP Test Spell 2 | +12% physical damage |
| `Gilded` | Prefix | `67828` Dummy Riding Aura | one extra creature loot roll |
| `of the Bear` | Affix | `71432` Mote of Anger | +15% maximum health |

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

The server writes the chosen labels into the creature name as
`%prefix %name %affix`. That is the create-block name the client uses for the
target frame, TidyPlates and the stock nameplates, so every nameplate carries
the same text. The add-on does not compose names.

The add-on hides the carriers' target-frame buttons and has no aura tooltip
hooks. Marker auras still exist so that suppression has a stable, client-visible
name to match; they are not the displayed labels.

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
.affix set none none none
```

`show` prints the selected creature's prefix, affix, size factor, maximum health,
native/current scale, bounding radius and combat reach. `set` forces a state on
the selected creature; it is for testing and is not the spawn roll.

## Loader

Folder `mod-creature-affixes` -> loader symbol
`Addmod_creature_affixesScripts` in `src/creature_affixes_loader.cpp`.
