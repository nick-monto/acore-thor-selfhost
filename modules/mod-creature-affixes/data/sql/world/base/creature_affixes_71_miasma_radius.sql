-- mod-creature-affixes: pin the Zombified miasma to its final 10-yard radius.
--
-- The 50 file defines 100203 with EffectRadiusIndex_1=7 (SpellRadius row 7 is
-- 10 yards). This idempotent update repairs realms that applied the earlier
-- 8-yard state; creature_affixes_72_percent_miasma.sql and
-- creature_affixes_73_tune_zombified.sql supersede that historical state and
-- re-assert radius 7. (100204 was the obsolete flat-damage spell; 72 deletes
-- it, and no file creates it.)

UPDATE `spell_dbc`
SET `EffectRadiusIndex_1` = 7
WHERE `ID` = 100203;
