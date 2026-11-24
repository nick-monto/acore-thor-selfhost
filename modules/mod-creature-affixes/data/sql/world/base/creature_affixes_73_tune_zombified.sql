-- mod-creature-affixes: tune Zombified and attach the miasma scaling script.
--
-- Movement slow: 100200 effect 2 (SPELL_AURA_MOD_DECREASE_SPEED) goes from
-- -20% to -40%. Effect 1 (melee haste) stays -20%.
--
-- Miasma: 100203 moves to SpellRadius row 7 (10 yards). It remains an enemy
-- area aura, but ZombifiedMiasmaAuraScript raises the tick amount from the
-- baseline 2% to 4% at level 10 and 6% at level 20.
--
-- Brutality: 100207 is a hidden +30% physical damage aura with
-- EffectMiscValue 1 (SPELL_SCHOOL_MASK_NORMAL), so it applies to weapon attacks.
-- EquippedItemClass must be -1; the damage-percent handler ignores an aura that
-- carries an equipped-item requirement.

UPDATE `spell_dbc`
SET `EffectBasePoints_2` = -40
WHERE `ID` = 100200;

UPDATE `spell_dbc`
SET `EffectRadiusIndex_1` = 7
WHERE `ID` = 100203;

INSERT INTO `spell_dbc`
(`ID`, `Attributes`, `AttributesEx`, `ProcTypeMask`, `ProcChance`,
 `MaxLevel`, `BaseLevel`, `SpellLevel`, `DurationIndex`,
 `Effect_1`, `EffectDieSides_1`, `EffectRealPointsPerLevel_1`, `EffectBasePoints_1`,
 `EffectMiscValue_1`, `ImplicitTargetA_1`, `ImplicitTargetB_1`, `EffectRadiusIndex_1`,
 `EffectAura_1`, `EffectAuraPeriod_1`, `EffectTriggerSpell_1`,
 `Effect_2`, `EffectDieSides_2`, `EffectRealPointsPerLevel_2`, `EffectBasePoints_2`,
 `ImplicitTargetA_2`, `ImplicitTargetB_2`, `EffectRadiusIndex_2`, `EffectAura_2`,
 `EffectAuraPeriod_2`, `EffectTriggerSpell_2`,
 `SpellVisualID_1`, `SpellIconID`, `SchoolMask`, `Name_Lang_enUS`, `Name_Lang_Mask`,
 `EquippedItemClass`)
VALUES
(100207, 384, 0, 0, 101, 0, 1, 1, 21,
 6, 0, 0, 30, 1, 1, 0, 0, 79, 0, 0,
 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
 0, 0, 1, 'Zombified Brutality', 0, -1)
ON DUPLICATE KEY UPDATE
`Attributes` = VALUES(`Attributes`),
`DurationIndex` = VALUES(`DurationIndex`),
`Effect_1` = VALUES(`Effect_1`),
`EffectDieSides_1` = VALUES(`EffectDieSides_1`),
`EffectRealPointsPerLevel_1` = VALUES(`EffectRealPointsPerLevel_1`),
`EffectBasePoints_1` = VALUES(`EffectBasePoints_1`),
`EffectMiscValue_1` = VALUES(`EffectMiscValue_1`),
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
`Name_Lang_Mask` = VALUES(`Name_Lang_Mask`),
`EquippedItemClass` = VALUES(`EquippedItemClass`);

INSERT INTO `mod_creature_affixes_modifier_effect`
(`modifier_key`, `sort_order`, `role`, `spell_id`)
VALUES
('zombified', 4, 'aura', 100207)
ON DUPLICATE KEY UPDATE
`role` = VALUES(`role`),
`spell_id` = VALUES(`spell_id`);

DELETE FROM `spell_script_names`
WHERE `spell_id` = 100203;

INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`)
VALUES (100203, 'ZombifiedMiasmaAuraScript')
ON DUPLICATE KEY UPDATE
`ScriptName` = VALUES(`ScriptName`);
