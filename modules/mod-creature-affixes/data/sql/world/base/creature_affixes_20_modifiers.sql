-- mod-creature-affixes: the initial modifier rows.
--
-- Client-visible carriers:
--   69737 "CP Test Spell 2"   (Vicious)
--   67828 "Dummy Riding Aura" (Gilded)
--   71432 "Mote of Anger"     (of the Bear)
--
-- All three are stock permanent dummy auras with Attributes 0, no spell
-- visual and no world-database references. They must stay client-visible:
-- SPELL_ATTR0_DO_NOT_DISPLAY also removes the aura from UnitAura, so the
-- add-on cannot read a hidden carrier. The add-on hides these auras' buttons
-- on the target frame instead.

INSERT INTO `mod_creature_affixes_modifier`
(`modifier_key`, `kind`, `label`, `carrier_spell`, `influence`, `effect`, `amount`)
VALUES
('vicious', 'prefix', 'Vicious',     69737, 2, 'vicious', 0.12),
('gilded',  'prefix', 'Gilded',      67828, 1, 'gilded',  1.00),
('bear',    'affix',  'of the Bear', 71432, 2, 'bear',    0.15)
ON DUPLICATE KEY UPDATE
`kind` = VALUES(`kind`),
`label` = VALUES(`label`),
`carrier_spell` = VALUES(`carrier_spell`),
`influence` = VALUES(`influence`),
`effect` = VALUES(`effect`),
`amount` = VALUES(`amount`);
