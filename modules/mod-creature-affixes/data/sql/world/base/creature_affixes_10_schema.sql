-- mod-creature-affixes: static modifier definitions.
--
-- This table is the authoring surface for the modifier pool. The worldserver
-- loads it once at startup and keeps per-spawn rolls in memory; no spawn rows
-- are written here. Adding a row makes the modifier rollable only after the
-- matching effect is implemented in CreatureAffixes.cpp.
--
-- `carrier_spell` is a stock client-known permanent dummy aura that is *not*
-- passive and does not carry SPELL_ATTR0_DO_NOT_DISPLAY. The client exposes it
-- through UnitAura, and the add-on both reads its name and hides its target-frame
-- button. Keep the name in CreatureAffixes_Data.lua in sync.

CREATE TABLE IF NOT EXISTS `mod_creature_affixes_modifier` (
  `id` int unsigned NOT NULL AUTO_INCREMENT,
  `modifier_key` varchar(32) NOT NULL,
  `kind` enum('prefix','affix') NOT NULL,
  `label` varchar(64) NOT NULL,
  `carrier_spell` int unsigned NOT NULL,
  `influence` tinyint unsigned NOT NULL DEFAULT '1',
  `effect` varchar(32) NOT NULL,
  `amount` float NOT NULL DEFAULT '0',
  PRIMARY KEY (`id`),
  UNIQUE KEY `modifier_key` (`modifier_key`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
