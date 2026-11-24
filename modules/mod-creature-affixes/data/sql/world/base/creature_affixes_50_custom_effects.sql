-- mod-creature-affixes: server-only custom spells for the candidate modifiers.
--
-- Stat auras (100200, 100201, 100202) are hidden: no visual, no icon, and
-- SPELL_ATTR0_DO_NOT_DISPLAY|DO_NOT_LOG. The server applies them; the client
-- has no aura button to render.
--
-- 100203 is a permanent enemy area aura: 2% of each victim's maximum health
--        every 3 seconds below level 10, 4% at levels 10-19 and 6% at 20+,
--        in a 10-yard radius. ZombifiedMiasmaAuraScript selects the step from
--        the victim's level; the 2 in this row is the fallback amount.
--   100205 is a 40% melee proc that triggers 100206: 4 + 2 per caster level,
--          shadow/fire, direct target damage.
--   100207 gives the carrier +30% physical attack damage, offsetting the
--          movement and melee-haste penalties.
--
-- 100206's EffectRealPointsPerLevel is evaluated against the caster's level
-- (the mob carrying the modifier), with BaseLevel and SpellLevel both 1.
-- All rows set EquippedItemClass=-1 in creature_affixes_72_percent_miasma.sql;
-- the default 0 is an item-class requirement and breaks normal casts.

INSERT INTO `spell_dbc`
(`ID`, `Attributes`, `AttributesEx`, `ProcTypeMask`, `ProcChance`,
 `MaxLevel`, `BaseLevel`, `SpellLevel`, `DurationIndex`,
 `Effect_1`, `EffectDieSides_1`, `EffectRealPointsPerLevel_1`, `EffectBasePoints_1`,
 `ImplicitTargetA_1`, `ImplicitTargetB_1`, `EffectRadiusIndex_1`, `EffectAura_1`,
 `EffectAuraPeriod_1`, `EffectTriggerSpell_1`,
 `Effect_2`, `EffectDieSides_2`, `EffectRealPointsPerLevel_2`, `EffectBasePoints_2`,
 `ImplicitTargetA_2`, `ImplicitTargetB_2`, `EffectRadiusIndex_2`, `EffectAura_2`,
 `EffectAuraPeriod_2`, `EffectTriggerSpell_2`,
 `SpellVisualID_1`, `SpellIconID`, `SchoolMask`, `Name_Lang_enUS`, `Name_Lang_Mask`)
VALUES
(100200, 384, 0, 0, 101, 0, 1, 1, 21,
 6, 0, 0, -20, 1, 0, 0, 138, 0, 0,
 6, 0, 0, -40, 1, 0, 0, 33, 0, 0,
 0, 0, 1, 'Zombified Sluggishness', 0),
(100201, 384, 0, 0, 101, 0, 1, 1, 21,
 6, 0, 0, -20, 1, 0, 0, 133, 0, 0,
 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
 0, 0, 1, 'Zombified Frailty', 0),
(100202, 384, 0, 0, 101, 0, 1, 1, 21,
 6, 0, 0, 20, 1, 0, 0, 138, 0, 0,
 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
 0, 0, 1, 'Corrupted Haste', 0),
(100203, 384, 0, 0, 101, 0, 1, 1, 21,
 129, 0, 0, 2, 1, 0, 7, 89, 3000, 0,
 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
 0, 0, 32, 'Zombified Miasma', 0),
(100205, 384, 0, 4, 40, 0, 1, 1, 21,
 6, 0, 0, 0, 1, 0, 0, 42, 0, 100206,
 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
 0, 0, 40, 'Corrupted Fel Lightning', 0),
(100206, 0, 0, 0, 101, 0, 1, 1, 0,
 2, 0, 2.0, 4, 6, 0, 0, 0, 0, 0,
 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
 9966, 0, 40, 'Corrupted Fel Lightning', 0),
(100207, 384, 0, 0, 101, 0, 1, 1, 21,
 6, 0, 0, 30, 1, 0, 0, 79, 0, 0,
 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
 0, 0, 1, 'Zombified Brutality', 0)
ON DUPLICATE KEY UPDATE
`Attributes` = VALUES(`Attributes`),
`AttributesEx` = VALUES(`AttributesEx`),
`ProcTypeMask` = VALUES(`ProcTypeMask`),
`ProcChance` = VALUES(`ProcChance`),
`MaxLevel` = VALUES(`MaxLevel`),
`BaseLevel` = VALUES(`BaseLevel`),
`SpellLevel` = VALUES(`SpellLevel`),
`DurationIndex` = VALUES(`DurationIndex`),
`Effect_1` = VALUES(`Effect_1`),
`EffectDieSides_1` = VALUES(`EffectDieSides_1`),
`EffectRealPointsPerLevel_1` = VALUES(`EffectRealPointsPerLevel_1`),
`EffectBasePoints_1` = VALUES(`EffectBasePoints_1`),
`ImplicitTargetA_1` = VALUES(`ImplicitTargetA_1`),
`ImplicitTargetB_1` = VALUES(`ImplicitTargetB_1`),
`EffectRadiusIndex_1` = VALUES(`EffectRadiusIndex_1`),
`EffectAura_1` = VALUES(`EffectAura_1`),
`EffectAuraPeriod_1` = VALUES(`EffectAuraPeriod_1`),
`EffectTriggerSpell_1` = VALUES(`EffectTriggerSpell_1`),
`Effect_2` = VALUES(`Effect_2`),
`EffectDieSides_2` = VALUES(`EffectDieSides_2`),
`EffectRealPointsPerLevel_2` = VALUES(`EffectRealPointsPerLevel_2`),
`EffectBasePoints_2` = VALUES(`EffectBasePoints_2`),
`ImplicitTargetA_2` = VALUES(`ImplicitTargetA_2`),
`ImplicitTargetB_2` = VALUES(`ImplicitTargetB_2`),
`EffectRadiusIndex_2` = VALUES(`EffectRadiusIndex_2`),
`EffectAura_2` = VALUES(`EffectAura_2`),
`EffectAuraPeriod_2` = VALUES(`EffectAuraPeriod_2`),
`EffectTriggerSpell_2` = VALUES(`EffectTriggerSpell_2`),
`SpellVisualID_1` = VALUES(`SpellVisualID_1`),
`SpellIconID` = VALUES(`SpellIconID`),
`SchoolMask` = VALUES(`SchoolMask`),
`Name_Lang_enUS` = VALUES(`Name_Lang_enUS`),
`Name_Lang_Mask` = VALUES(`Name_Lang_Mask`);
