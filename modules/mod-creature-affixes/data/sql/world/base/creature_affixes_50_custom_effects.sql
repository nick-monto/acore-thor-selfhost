-- mod-creature-affixes: server-only hidden auras for the candidate modifiers.
--
-- These IDs are intentionally above the client's stock Spell.dbc range. They
-- have SPELL_ATTR0_DO_NOT_DISPLAY|DO_NOT_LOG, no visual and no icon, so the
-- client has no aura button to render; the server still applies the stat aura.
-- The carrier/marker, proc and visual spells are stock client-known spells.

INSERT INTO `spell_dbc`
(`ID`, `Attributes`, `DurationIndex`,
 `Effect_1`, `EffectDieSides_1`, `EffectBasePoints_1`, `EffectAura_1`,
 `Effect_2`, `EffectDieSides_2`, `EffectBasePoints_2`, `EffectAura_2`,
 `SpellIconID`, `SpellVisualID_1`, `SchoolMask`, `Name_Lang_enUS`, `Name_Lang_Mask`)
VALUES
(100200, 384, 21,
 6, 0, -20, 138,
 6, 0, -20, 33,
 0, 0, 1, 'Zombified Sluggishness', 0),
(100201, 384, 21,
 6, 0, -20, 133,
 0, 0, 0, 0,
 0, 0, 1, 'Zombified Frailty', 0),
(100202, 384, 21,
 6, 0, 20, 138,
 0, 0, 0, 0,
 0, 0, 1, 'Corrupted Haste', 0)
ON DUPLICATE KEY UPDATE
`Attributes` = VALUES(`Attributes`),
`DurationIndex` = VALUES(`DurationIndex`),
`Effect_1` = VALUES(`Effect_1`),
`EffectDieSides_1` = VALUES(`EffectDieSides_1`),
`EffectBasePoints_1` = VALUES(`EffectBasePoints_1`),
`EffectAura_1` = VALUES(`EffectAura_1`),
`Effect_2` = VALUES(`Effect_2`),
`EffectDieSides_2` = VALUES(`EffectDieSides_2`),
`EffectBasePoints_2` = VALUES(`EffectBasePoints_2`),
`EffectAura_2` = VALUES(`EffectAura_2`),
`SpellIconID` = VALUES(`SpellIconID`),
`SpellVisualID_1` = VALUES(`SpellVisualID_1`),
`SchoolMask` = VALUES(`SchoolMask`),
`Name_Lang_enUS` = VALUES(`Name_Lang_enUS`);
