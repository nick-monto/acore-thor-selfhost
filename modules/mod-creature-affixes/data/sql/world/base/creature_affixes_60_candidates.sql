-- mod-creature-affixes: candidate Zombified (prefix) and Corrupted (affix).
--
-- Zombified:
--   health -20%          custom hidden aura 100201
--   attack/move -20%     custom hidden aura 100200 (two aura effects)
--   gaseous shadow tick  100203 periodically triggers 100204 (shadow school,
--                         8-yard enemy area), scaled from the mob's level.
--   gas visual           25039 Green Ghost Visual; stock client data.
--
-- Corrupted:
--   attack speed +20%    custom hidden aura 100202
--   shadow proc          100205 (40% on hit) triggers 100206, scaled from the
--                         mob's level; hidden proc aura.
--   corrupted visual     60451 Corruption of Time (purple shadow visual); stock
--                         client data.
--
-- Carrier spells are the existing visible dummy mechanism; the add-on hides
-- their target-frame buttons. The custom stat and trigger auras are server-only
-- and hidden by SPELL_ATTR0_DO_NOT_DISPLAY|DO_NOT_LOG.

INSERT INTO `mod_creature_affixes_modifier`
(`modifier_key`, `kind`, `label`, `carrier_spell`, `influence`, `effect`, `amount`)
VALUES
('zombified', 'prefix', 'Zombified', 34463, 3, 'none', 0),
('corrupted', 'affix',  'Corrupted', 29479, 3, 'none', 0)
ON DUPLICATE KEY UPDATE
`kind` = VALUES(`kind`),
`label` = VALUES(`label`),
`carrier_spell` = VALUES(`carrier_spell`),
`influence` = VALUES(`influence`),
`effect` = VALUES(`effect`),
`amount` = VALUES(`amount`);

INSERT INTO `mod_creature_affixes_modifier_effect`
(`modifier_key`, `sort_order`, `role`, `spell_id`)
VALUES
('zombified', 0, 'aura', 100200),
('zombified', 1, 'aura', 100201),
('zombified', 2, 'aura', 100203),
('zombified', 3, 'visual', 25039),
('corrupted', 0, 'aura', 100202),
('corrupted', 1, 'aura', 100205),
('corrupted', 2, 'visual', 60451)
ON DUPLICATE KEY UPDATE
`role` = VALUES(`role`),
`spell_id` = VALUES(`spell_id`);
