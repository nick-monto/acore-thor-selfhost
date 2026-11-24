-- mod-creature-affixes: the expanded 15-prefix / 15-affix pool.
--
-- Three proof rows were in 20_modifiers.sql and two complex rows in
-- 60_candidates.sql. This migration adds the remaining simple and complex
-- rows without changing the existing rows. The four new complex prefixes are
-- Feral, Blazing, Frostbound and Venomous; the four new complex affixes are
-- of the Inferno, of the Glacier, of the Plague and of the Storm. Together
-- with Zombified and of the Corruption that is five complex rows per kind.
--
-- Every new row uses a stock, client-visible permanent dummy carrier. The
-- add-on reads the carrier name through UnitAura, then hides its target-frame
-- button. The names below are intentionally obscure game/DND auras so a
-- natural creature aura is unlikely to be mistaken for a modifier marker.
--
-- Simple rows use one hidden stat aura plus one stock visual cue. Complex rows
-- use two or three hidden stat/area auras plus one stock visual effect. Visual
-- ids are stock client spells, so their effects are visible without a client
-- Spell.dbc patch.
-- The server-only custom auras use DurationIndex 21 and EquippedItemClass=-1;
-- their attributes are DO_NOT_DISPLAY|DO_NOT_LOG so the client sees no aura
-- button for them.

INSERT INTO `mod_creature_affixes_modifier`
(`modifier_key`, `kind`, `label`, `carrier_spell`, `influence`, `effect`, `amount`)
VALUES
-- Prefixes
('feral',       'prefix', 'Feral',          22805, 3, 'none', 0),
('blazing',     'prefix', 'Blazing',        25430, 3, 'none', 0),
('frostbound',  'prefix', 'Frostbound',     26563, 3, 'none', 0),
('venomous',    'prefix', 'Venomous',       41033, 3, 'none', 0),
('cruel',       'prefix', 'Cruel',          41522, 1, 'none', 0),
('sturdy',      'prefix', 'Sturdy',         58167, 2, 'none', 0),
('frenzied',    'prefix', 'Frenzied',       58169, 2, 'none', 0),
('sluggish',    'prefix', 'Sluggish',       43300, 2, 'none', 0),
('arcane',      'prefix', 'Arcane',         43685, 2, 'none', 0),
('warded',      'prefix', 'Warded',         58174, 2, 'none', 0),
('swift',       'prefix', 'Swift',          43827, 1, 'none', 0),
('blessed',     'prefix', 'Blessed',        58175, 1, 'none', 0),
-- Affixes
('inferno',     'affix',  'of the Inferno',  58176, 3, 'none', 0),
('glacier',     'affix',  'of the Glacier',  45062, 3, 'none', 0),
('plague',      'affix',  'of the Plague',   58177, 3, 'none', 0),
('storm',       'affix',  'of the Storm',    46340, 3, 'none', 0),
('ox',          'affix',  'of the Ox',       49444, 2, 'none', 0),
('wolf',        'affix',  'of the Wolf',     50797, 1, 'none', 0),
('falcon',      'affix',  'of the Falcon',   51176, 2, 'none', 0),
('turtle',      'affix',  'of the Turtle',   51217, 2, 'none', 0),
('serpent',     'affix',  'of the Serpent',  51624, 2, 'none', 0),
('moon',        'affix',  'of the Moon',     61793, 1, 'none', 0),
('boar',        'affix',  'of the Boar',     61794, 1, 'none', 0),
('wraith',      'affix',  'of the Wraith',   61795, 2, 'none', 0),
('colossus',    'affix',  'of the Colossus', 61796, 2, 'none', 0)
ON DUPLICATE KEY UPDATE
`kind` = VALUES(`kind`),
`label` = VALUES(`label`),
`carrier_spell` = VALUES(`carrier_spell`),
`influence` = VALUES(`influence`),
`effect` = VALUES(`effect`),
`amount` = VALUES(`amount`);

-- Gilded is the existing loot prefix; it had no visual. Harpoon Loot Sparkles
-- is a stock permanent dummy visual, so the server can apply it without a
-- client DBC row.
INSERT INTO `mod_creature_affixes_modifier_effect`
(`modifier_key`, `sort_order`, `role`, `spell_id`)
VALUES
('gilded', 0, 'visual', 57718),
-- Feral: fast, frail, physically dangerous, insect swarm.
('feral', 0, 'aura', 100300),
('feral', 1, 'aura', 100303),
('feral', 2, 'aura', 100304),
('feral', 3, 'visual', 35471),
-- Blazing: fast, magically dangerous, frail, fire spiral.
('blazing', 0, 'aura', 100300),
('blazing', 1, 'aura', 100305),
('blazing', 2, 'aura', 100303),
('blazing', 3, 'visual', 45421),
-- Frostbound: armored, slowed, magically dangerous, frost shell.
('frostbound', 0, 'aura', 100306),
('frostbound', 1, 'aura', 100301),
('frostbound', 2, 'aura', 100305),
('frostbound', 3, 'visual', 40158),
-- Venomous: area blight, slowed, physical damage, entangling growth.
('venomous', 0, 'aura', 100309),
('venomous', 1, 'aura', 100301),
('venomous', 2, 'aura', 100304),
('venomous', 3, 'visual', 35468),
-- Simple prefixes.
('cruel',    0, 'aura', 100304),
('sturdy',   0, 'aura', 100302),
('frenzied', 0, 'aura', 100300),
('sluggish', 0, 'aura', 100301),
('arcane',   0, 'aura', 100305),
('warded',   0, 'aura', 100306),
('swift',    0, 'aura', 100307),
('blessed',  0, 'aura', 100312),
-- Inferno: fast, magically dangerous, frail, flame sphere.
('inferno', 0, 'aura', 100305),
('inferno', 1, 'aura', 100300),
('inferno', 2, 'aura', 100303),
('inferno', 3, 'visual', 55928),
-- Glacier: armored, slowed, magically dangerous, frost visual.
('glacier', 0, 'aura', 100306),
('glacier', 1, 'aura', 100301),
('glacier', 2, 'aura', 100305),
('glacier', 3, 'visual', 47706),
-- Plague: area blight, frail, physical damage, infected visual.
('plague', 0, 'aura', 100309),
('plague', 1, 'aura', 100303),
('plague', 2, 'aura', 100304),
('plague', 3, 'visual', 49702),
-- Storm: fast casting, magically dangerous, fast movement, storm cloud.
('storm', 0, 'aura', 100308),
('storm', 1, 'aura', 100305),
('storm', 2, 'aura', 100300),
('storm', 3, 'visual', 45213),
-- Simple affixes.
('ox',       0, 'aura', 100302),
('wolf',     0, 'aura', 100304),
('falcon',   0, 'aura', 100300),
('turtle',   0, 'aura', 100306),
('serpent',  0, 'aura', 100305),
('moon',     0, 'aura', 100308),
('boar',     0, 'aura', 100312),
('wraith',   0, 'aura', 100311),
('colossus', 0, 'aura', 100302),
-- Simple visual cues. Each is a stock, permanent dummy visual; the matching
-- names are marked in CreatureAffixes_Data.lua so their aura buttons stay out
-- of the target/focus frames.
('vicious',  0, 'visual', 72100), -- Blood Orb State Visual
('cruel',    1, 'visual', 45727), -- Display Blood Globe
('sturdy',   1, 'visual', 70733), -- Stoneform
('frenzied', 1, 'visual', 45111), -- Enrage
('sluggish', 1, 'visual', 40714), -- Dazed and Confused
('arcane',   1, 'visual', 44347), -- Arcane State
('warded',   1, 'visual', 46933), -- Cosmetic - Arcane Force Shield (Blue)
('swift',    1, 'visual', 42901), -- Cosmetic - Combat Whirlwind
('blessed',  1, 'visual', 65712), -- Light Essence Aura
('bear',     0, 'visual', 58837), -- Icebound Fortitude
('ox',       1, 'visual', 55845), -- Power Spark
('wolf',     1, 'visual', 71949), -- Frenzied Bloodthirst Visual
('falcon',   1, 'visual', 71348), -- Gusty Prison
('turtle',   1, 'visual', 70692), -- Shield of the Lich King
('serpent',  1, 'visual', 52822), -- Blight Spill
('moon',     1, 'visual', 30888), -- Star Beam
('boar',     1, 'visual', 25824), -- Spotlight
('wraith',   1, 'visual', 47840), -- Cosmetic - Purple Ghost Aura
('colossus', 1, 'visual', 52948) -- Wild Growth
ON DUPLICATE KEY UPDATE
`role` = VALUES(`role`),
`spell_id` = VALUES(`spell_id`);

-- Server-only stat and area auras. No client Spell.dbc rows exist for these;
-- they are deliberately invisible. The name, school mask, and base points are
-- server-side data only.
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
(100300, 384, 0, 0, 101, 0, 1, 1, 21,
 6, 0, 0, 20, 0, 1, 0, 0, 138, 0, 0,
 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
 0, 0, 1, 'Feral Haste', 0, -1),
(100301, 384, 0, 0, 101, 0, 1, 1, 21,
 6, 0, 0, -25, 0, 1, 0, 0, 33, 0, 0,
 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
 0, 0, 1, 'Crippling Slowness', 0, -1),
(100302, 384, 0, 0, 101, 0, 1, 1, 21,
 6, 0, 0, 15, 0, 1, 0, 0, 133, 0, 0,
 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
 0, 0, 1, 'Vitality Surge', 0, -1),
(100303, 384, 0, 0, 101, 0, 1, 1, 21,
 6, 0, 0, -15, 0, 1, 0, 0, 133, 0, 0,
 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
 0, 0, 1, 'Frailty', 0, -1),
(100304, 384, 0, 0, 101, 0, 1, 1, 21,
 6, 0, 0, 10, 1, 1, 0, 0, 79, 0, 0,
 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
 0, 0, 1, 'Savage Strikes', 0, -1),
(100305, 384, 0, 0, 101, 0, 1, 1, 21,
 6, 0, 0, 15, 126, 1, 0, 0, 79, 0, 0,
 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
 0, 0, 126, 'Elemental Fury', 0, -1),
(100306, 384, 0, 0, 101, 0, 1, 1, 21,
 6, 0, 0, 20, 1, 1, 0, 0, 101, 0, 0,
 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
 0, 0, 1, 'Iron Hide', 0, -1),
(100307, 384, 0, 0, 101, 0, 1, 1, 21,
 6, 0, 0, 20, 0, 1, 0, 0, 31, 0, 0,
 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
 0, 0, 1, 'Windborne', 0, -1),
(100308, 384, 0, 0, 101, 0, 1, 1, 21,
 6, 0, 0, 20, 0, 1, 0, 0, 65, 0, 0,
 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
 0, 0, 1, 'Quick Casting', 0, -1),
(100309, 384, 0, 0, 101, 0, 1, 1, 21,
 129, 0, 0, 3, 0, 1, 0, 7, 89, 3000, 0,
 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
 0, 0, 8, 'Creeping Blight', 0, -1),
(100311, 384, 0, 0, 101, 0, 1, 1, 21,
 6, 0, 0, 10, 0, 1, 0, 0, 57, 0, 0,
 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
 0, 0, 1, 'Spell Precision', 0, -1),
(100312, 384, 0, 0, 101, 0, 1, 1, 21,
 6, 0, 0, 15, 0, 1, 0, 0, 136, 0, 0,
 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
 0, 0, 1, 'Blessing of the Boar', 0, -1)
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
