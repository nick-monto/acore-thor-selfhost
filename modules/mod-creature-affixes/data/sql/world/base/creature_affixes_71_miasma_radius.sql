-- mod-creature-affixes: widen the Zombified miasma to its documented 8 yards.
--
-- SpellRadius.dbc row 8 is 5 yards, not 8. Row 14 is the 8-yard radius.
-- The 50 file is the fresh-install definition; this migration fixes realms
-- that already applied the earlier row.

UPDATE `spell_dbc`
SET `EffectRadiusIndex_1` = 14
WHERE `ID` = 100204;
