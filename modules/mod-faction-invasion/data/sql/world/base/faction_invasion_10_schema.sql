-- mod-faction-invasion: definition tables.
--
-- All module SQL under data/sql/world/base is applied by the DB updater in
-- filename order with state MODULE. The schema and data files are re-runnable:
-- the data files DELETE their scope and INSERT, and every table is created
-- with IF NOT EXISTS.
--
-- The generated half of this directory (themes, creature roster, areas,
-- ground spells, loot pools, elite clones) is written by
-- scripts/faction-invasion/gen_invasion.py from the pinned core's base dumps.
-- Do not hand-edit those files; edit the generator's THEMES/AREAS tables.

CREATE TABLE IF NOT EXISTS `mod_faction_invasion_theme`
(
    `theme_key`       VARCHAR(32)  NOT NULL,
    `name`            VARCHAR(64)  NOT NULL,
    `faction`         VARCHAR(64)  NOT NULL,
    `marker_spell`    INT UNSIGNED NOT NULL DEFAULT 0,
    `modifier_stat`   ENUM('health','damage','spell','armor') NOT NULL DEFAULT 'damage',
    `modifier_amount` FLOAT        NOT NULL DEFAULT 0,
    `weight`          INT UNSIGNED NOT NULL DEFAULT 1,
    PRIMARY KEY (`theme_key`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS `mod_faction_invasion_creature`
(
    `theme_key`   VARCHAR(32)  NOT NULL,
    `base_entry`  INT UNSIGNED NOT NULL,
    `elite_entry` INT UNSIGNED NOT NULL DEFAULT 0,
    `min_level`   TINYINT UNSIGNED NOT NULL,
    `max_level`   TINYINT UNSIGNED NOT NULL,
    `weight`      INT UNSIGNED NOT NULL DEFAULT 1,
    PRIMARY KEY (`theme_key`, `base_entry`),
    KEY `level_band` (`theme_key`, `min_level`, `max_level`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS `mod_faction_invasion_area`
(
    `id`         INT UNSIGNED NOT NULL,
    `zone_id`    INT UNSIGNED NOT NULL,
    `map_id`     INT UNSIGNED NOT NULL,
    `name`       VARCHAR(64)  NOT NULL,
    `x`          FLOAT        NOT NULL,
    `y`          FLOAT        NOT NULL,
    `z`          FLOAT        NOT NULL DEFAULT 0,
    `radius`     FLOAT        NOT NULL DEFAULT 120,
    `min_level`  TINYINT UNSIGNED NOT NULL DEFAULT 1,
    `max_level`  TINYINT UNSIGNED NOT NULL DEFAULT 80,
    `weight`     INT UNSIGNED NOT NULL DEFAULT 1,
    PRIMARY KEY (`id`),
    KEY `zone` (`zone_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS `mod_faction_invasion_ground`
(
    `id`          INT UNSIGNED NOT NULL AUTO_INCREMENT,
    `theme_key`   VARCHAR(32)  NOT NULL,
    `spell_id`    INT UNSIGNED NOT NULL,
    `pattern`     ENUM('single','cluster') NOT NULL DEFAULT 'single',
    `patch_count` TINYINT UNSIGNED NOT NULL DEFAULT 1,
    `radius`      FLOAT        NOT NULL DEFAULT 10,
    `weight`      INT UNSIGNED NOT NULL DEFAULT 1,
    PRIMARY KEY (`id`),
    KEY `theme` (`theme_key`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS `mod_faction_invasion_pool`
(
    `bracket` TINYINT UNSIGNED NOT NULL,
    `quality` ENUM('common','uncommon','rare','epic') NOT NULL,
    `item_id` INT UNSIGNED NOT NULL,
    PRIMARY KEY (`bracket`, `quality`, `item_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS `mod_faction_invasion_loot`
(
    `bracket`    TINYINT UNSIGNED NOT NULL,
    `difficulty` TINYINT UNSIGNED NOT NULL,
    `kind`       ENUM('mob','chest') NOT NULL,
    `roll_order` TINYINT UNSIGNED NOT NULL,
    `quality`    ENUM('common','uncommon','rare','epic','fixed') NOT NULL,
    `item_id`    INT UNSIGNED NOT NULL DEFAULT 0,
    `rolls`      TINYINT UNSIGNED NOT NULL DEFAULT 1,
    `chance`     FLOAT        NOT NULL DEFAULT 100,
    PRIMARY KEY (`bracket`, `difficulty`, `kind`, `roll_order`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS `mod_faction_invasion_money`
(
    `bracket`     TINYINT UNSIGNED NOT NULL,
    `difficulty`  TINYINT UNSIGNED NOT NULL,
    `mob_copper`  INT UNSIGNED NOT NULL DEFAULT 0,
    `chest_copper` INT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`bracket`, `difficulty`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
