-- mod-creature-affixes: the initial modifier rows.
--
-- Hidden carriers:
--   52370 "Periodic Despawn Check"  (Vicious)
--   44360 "Racing Crop Aura"        (Gilded)
--   51047 "Have Knotroot"           (of the Bear)
--
-- All three are stock permanent dummy auras with Attributes 0x180
-- (SPELL_ATTR0_DO_NOT_DISPLAY | SPELL_ATTR0_DO_NOT_LOG). The client hides
-- them from the aura bar and combat log, while the add-on can still read the
-- aura name. Do not replace them with a visible dummy aura.

INSERT INTO `mod_creature_affixes_modifier`
(`modifier_key`, `kind`, `label`, `carrier_spell`, `influence`, `effect`, `amount`)
VALUES
('vicious', 'prefix', 'Vicious',     52370, 2, 'vicious', 0.12),
('gilded',  'prefix', 'Gilded',      44360, 1, 'gilded',  1.00),
('bear',    'affix',  'of the Bear', 51047, 2, 'bear',    0.15)
ON DUPLICATE KEY UPDATE
`kind` = VALUES(`kind`),
`label` = VALUES(`label`),
`carrier_spell` = VALUES(`carrier_spell`),
`influence` = VALUES(`influence`),
`effect` = VALUES(`effect`),
`amount` = VALUES(`amount`);
