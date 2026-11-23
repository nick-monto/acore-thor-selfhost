-- mod-creature-affixes: move an already-applied slice off the hidden carriers.
--
-- 52370, 44360 and 51047 carry SPELL_ATTR0_DO_NOT_DISPLAY, and the client filters
-- those auras out of UnitAura as well as the aura bar, so the add-on never saw a
-- 0.2.0 marker. A fresh database gets the visible rows from 20_modifiers.sql; this
-- file moves a database that already applied the old seed. The guards leave an
-- operator's own carrier choice alone.

UPDATE `mod_creature_affixes_modifier`
SET `carrier_spell` = 69737
WHERE `modifier_key` = 'vicious' AND `carrier_spell` = 52370;

UPDATE `mod_creature_affixes_modifier`
SET `carrier_spell` = 67828
WHERE `modifier_key` = 'gilded' AND `carrier_spell` = 44360;

UPDATE `mod_creature_affixes_modifier`
SET `carrier_spell` = 71432
WHERE `modifier_key` = 'bear' AND `carrier_spell` = 51047;
