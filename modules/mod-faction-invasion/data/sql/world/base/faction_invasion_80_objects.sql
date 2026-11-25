-- mod-faction-invasion: the reward chest template.
--
-- The ground carrier is the stock Invisible Stalker (entry 15214); the module
-- overrides its faction (14, hostile to players), display (11686), unit flags
-- and movement at summon time, so no new creature template is needed.
--
-- Entry 5000100 is free (mod-survivor owns 5000000).
--
-- Why the values matter:
--
--   * Data1 (chest.lootId) = 0. Player::SendLoot only clears and regenerates a
--     chest's loot when the template has a non-zero lootId, so a zero lootId is
--     what preserves the loot the module filled in before the chest is opened.
--   * Data3 (chest.consumable) = 1. The core deletes the chest once its loot is
--     released; the module's own expiry sweep only has to cover an unopened
--     chest.
--   * Data15 (chest.groupLootRules) = 0: this is a solo chest, there is
--     nothing to roll.
--   * Data2 (chest.chestRestockTime) = 0: no restock timer.
--   * gameobject_template_addon.mingold/maxgold stay 0: Player::SendLoot calls
--     generateMoneyLoot for every gameobject, so a non-zero value here would
--     overwrite the module's own reward. Gold is paid once by the module.
--
-- displayId 4093 is the stock "Mangled Human Remains" model the Survivor
-- container already uses; a reward chest can be re-skinned by changing only
-- this row.

DELETE FROM `gameobject_template` WHERE `entry` = 5000100;
DELETE FROM `gameobject_template_addon` WHERE `entry` = 5000100;

INSERT INTO `gameobject_template`
(`entry`, `type`, `displayId`, `name`, `IconName`, `castBarCaption`, `unk1`, `size`,
 `Data0`, `Data1`, `Data2`, `Data3`, `Data4`, `Data5`, `Data6`, `Data7`, `Data8`, `Data9`,
 `Data10`, `Data11`, `Data12`, `Data13`, `Data14`, `Data15`, `Data16`, `Data17`, `Data18`,
 `Data19`, `Data20`, `Data21`, `Data22`, `Data23`, `AIName`, `ScriptName`, `VerifiedBuild`)
VALUES
(5000100, 3, 4093, 'Invasion Cache', '', '', '', 1,
 0, 0, 0, 1, 0, 0, 0, 0, 0, 0,
 0, 0, 0, 0, 0, 0, 0, 0, 0,
 0, 0, 0, 0, 0, '', '', 12340);

INSERT INTO `gameobject_template_addon`
(`entry`, `faction`, `flags`, `mingold`, `maxgold`, `artkit0`, `artkit1`, `artkit2`, `artkit3`)
VALUES
(5000100, 0, 0, 0, 0, 0, 0, 0, 0);
