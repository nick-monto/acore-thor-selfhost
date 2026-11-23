-- mod-creature-affixes: effect spells attached to a modifier.
--
-- A modifier still has one carrier_spell (the visible dummy aura the add-on
-- hides). This table holds the spells that implement the modifier's actual
-- behavior: stat auras, periodic damage, procs, transforms and visuals.
--
-- role = 'aura'   apply a permanent aura with AddAura and remove it by caster.
-- role = 'visual' same lifecycle, but the spell exists for its visual only.
--
-- The module rejects passive or non-permanent rows at startup. Stock spells
-- are preferred; a server-only custom spell must be hidden in the client.

CREATE TABLE IF NOT EXISTS `mod_creature_affixes_modifier_effect` (
  `id` int unsigned NOT NULL AUTO_INCREMENT,
  `modifier_key` varchar(32) NOT NULL,
  `sort_order` tinyint unsigned NOT NULL DEFAULT '0',
  `role` enum('aura','visual') NOT NULL,
  `spell_id` int unsigned NOT NULL,
  PRIMARY KEY (`id`),
  UNIQUE KEY `modifier_sort` (`modifier_key`, `sort_order`),
  KEY `modifier_key` (`modifier_key`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
