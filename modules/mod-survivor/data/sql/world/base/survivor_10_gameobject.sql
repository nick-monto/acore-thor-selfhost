-- mod-survivor: the lootable container left behind by a Survivor death.
--
-- Entry 5000000 is free in the pinned base dump and is summoned at runtime by
-- Survivor.cpp (Map::SummonGameObject), never spawned from a spawn point.
--
-- Why the values below matter:
--
--   * Data0 (lootId) = 0. Player::SendLoot only clears and regenerates a
--     chest's loot when the template has a non-zero chest.lootId, so a zero
--     lootId is what preserves the loot the module filled in beforehand - the
--     only way to show real player items, because LootItem has no room for
--     enchants, gems or durability.
--   * Data2 (consumable) = 1. A consumed-on-use chest with a summon respawn
--     time is despawned by GameObject::Update once its loot is released, so
--     the module needs no cleanup timer of its own.
--   * Data9 / Data10 (min/max gold) are in gameobject_template_addon, and are
--     left at 0: SendLoot runs generateMoneyLoot for every gameobject no matter
--     what its lootId is, so non-zero gold here would inject money into every
--     single container.
--
-- Data8 (groupLootRules) is 0: this is a solo realm, there is nothing to roll.
-- displayId 4093 is the stock "Mangled Human Remains" model, the corpse pile the
-- game itself uses. faction and flags on the addon row are 0: the container must
-- be visible and clickable, and "owner only" is enforced in code
-- (GlobalScript::OnAllowedToLootContainerCheck), because no gameobject flag can
-- express a player identity.

DELETE FROM `gameobject_template` WHERE `entry` = 5000000;
DELETE FROM `gameobject_template_addon` WHERE `entry` = 5000000;

INSERT INTO `gameobject_template`
(`entry`, `type`, `displayId`, `name`, `IconName`, `castBarCaption`, `unk1`, `size`,
 `Data0`, `Data1`, `Data2`, `Data3`, `Data4`, `Data5`, `Data6`, `Data7`, `Data8`, `Data9`,
 `Data10`, `Data11`, `Data12`, `Data13`, `Data14`, `Data15`, `Data16`, `Data17`, `Data18`,
 `Data19`, `Data20`, `Data21`, `Data22`, `Data23`, `AIName`, `ScriptName`, `VerifiedBuild`)
VALUES
(5000000, 3, 4093, 'Remains', '', '', '', 1,
 0, 0, 1, 0, 0, 0, 0, 0, 0, 0,
 0, 0, 0, 0, 0, 0, 0, 0, 0,
 0, 0, 0, 0, 0, '', '', 12340);

INSERT INTO `gameobject_template_addon`
(`entry`, `faction`, `flags`, `mingold`, `maxgold`, `artkit0`, `artkit1`, `artkit2`, `artkit3`)
VALUES
(5000000, 0, 0, 0, 0, 0, 0, 0, 0);
