-- mod-creature-affixes: correct the two area-aura radius rows.
--
-- SpellRadius.dbc row 7 is 2 yards, not 10; row 13 is 10 yards. 100203
-- (Zombified Miasma) and 100309 (Creeping Blight) were both created with
-- EffectRadiusIndex_1 = 7, so the "10-yard" comments in 72/73/80 were wrong.
--
-- This file sorts after 80_expanded_pool.sql, where 100309 is created, and
-- both target rows already exist by then. Both statements are idempotent.

UPDATE `spell_dbc`
SET `EffectRadiusIndex_1` = 13
WHERE `ID` = 100203;

UPDATE `spell_dbc`
SET `EffectRadiusIndex_1` = 13
WHERE `ID` = 100309;
