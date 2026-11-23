# mod-creature-affixes

The initial server slice for the creature prefix/affix system.

## Initial rows

| Modifier | Kind | Carrier spell | Effect |
| --- | --- | --- | --- |
| `Vicious` | Prefix | `2584` | +12% physical damage |
| `Gilded` | Prefix | `5428` | one extra creature loot roll |
| `of the Bear` | Affix | `5430` | +15% maximum health |

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

## Eligibility

Only world-spawned creatures (`creature.GetSpawnId() != 0`) are considered.
The module skips critters, civilians, quest givers, vendors, trainers, totems,
vehicles, pets, dungeon bosses and world bosses.

## Configuration

`conf/mod-creature-affixes.conf.dist` is installed as
`<prefix>/etc/modules/mod-creature-affixes.conf`. The world script reads it in
`OnAfterConfigLoad`, so `.reload config` applies changes without a restart.

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
