-- mod-creature-affixes: make the Zombified miasma scale with victim health.
--
-- The old design periodically triggered 100204, a flat level-scaled damage
-- spell. At level 80 that was roughly 20 damage every 3 seconds, which natural
-- regeneration could out-heal. 100203 is now a permanent enemy area aura:
-- SPELL_EFFECT_APPLY_AREA_AURA_ENEMY + SPELL_AURA_PERIODIC_DAMAGE_PERCENT,
-- dealing 2% of each victim's maximum health every 3 seconds.
--
-- 100204 is no longer used; delete it. Set EquippedItemClass=-1 on all custom
-- rows: 0 is an item-class requirement, not "none", and breaks .cast with
-- SPELL_FAILED_EQUIPPED_ITEM_CLASS.

UPDATE `spell_dbc`
SET `EquippedItemClass` = -1
WHERE `ID` BETWEEN 100200 AND 100206;

UPDATE `spell_dbc`
SET `Effect_1` = 129,
    `EffectDieSides_1` = 0,
    `EffectRealPointsPerLevel_1` = 0,
    `EffectBasePoints_1` = 2,
    `ImplicitTargetA_1` = 1,
    `ImplicitTargetB_1` = 0,
    `EffectRadiusIndex_1` = 14,
    `EffectAura_1` = 89,
    `EffectAuraPeriod_1` = 3000,
    `EffectTriggerSpell_1` = 0
WHERE `ID` = 100203;

DELETE FROM `spell_dbc`
WHERE `ID` = 100204;
