-- mod-creature-affixes: rename the Corrupted affix to its noun form.
--
-- The 60 file is the fresh-install definition; this migration updates realms
-- that already applied the earlier label.

UPDATE `mod_creature_affixes_modifier`
SET `kind` = 'affix',
    `label` = 'of the Corruption'
WHERE `modifier_key` = 'corrupted';
