-- mod-pawn: the obelisk a player uses to pick pawns.
--
-- Entry 5000010 is free in the pinned base dump (mod-survivor owns 5000000) and
-- lives in the custom range on purpose: no core update will ever claim it.
--
-- Why the values below matter:
--
--   * type 10 (GOOBER) with a ScriptName is the supported way to build a gossip
--     menu in code. GameObject::Use calls sScriptMgr->OnGossipHello for every
--     gameobject type before its own type switch, and a GameObjectScript bound
--     to this entry's ScriptName owns the interaction by returning true
--     (src/server/game/Entities/GameObject/GameObject.cpp:1499-1513). The
--     Icecrown Citadel teleporter is the in-tree precedent
--     (icecrown_citadel_teleport.cpp:44-88).
--   * Data0 (gossipID) is 0: the menu is not a DB gossip list, the script
--     builds every row, so there is no npc_text or gossip_menu row to keep in
--     step with the code.
--   * Data1 (consume) is 0: the obelisk is not used up.
--   * displayId 7017 is the stock 'Obelisk' model, the same one entry 183102
--     uses. Size 1.2 makes it read as a landmark rather than scenery.
--   * The addon row carries faction 0 and flags 0: the obelisk must be visible
--     and clickable by every player of both factions. Flags 4 on the ICC
--     transporters is GO_FLAG_INTERACT_COND, which those objects need because
--     their options unlock with boss progress; nothing here is conditional.
--
-- Spawn guids are in the 9800000 band this repository reserves for custom
-- spawns (the travel portals use 9800000-9800015). The base dump's guids run to
-- 5714437, so a 5000000-range guid would sit inside a band a core update may
-- still claim - and a collision on `gameobject`.`guid` fails the updater, which
-- aborts the boot. The template *entry* is separate from the guid and stays
-- 5000010, beside mod-survivor's 5000000.
--
-- Spawns: one in Stormwind's Trade District and one at the Valley of Strength
-- entrance in Orgrimmar, each anchored to an existing object at street level
-- so the Z is the ground the player stands on:
--   * Stormwind: the Food Crate spawn at guid 12759, -8484.0 673.003 102.428
--   * Orgrimmar: the Valley of Strength signpost at guid 10074, 1766.42 -4222.41 44.3204
-- Move them by editing the two rows; the code reads Pawn.ObeliskEntry, not the
-- spawn guids.

DELETE FROM `gameobject_template` WHERE `entry` = 5000010;
DELETE FROM `gameobject_template_addon` WHERE `entry` = 5000010;
DELETE FROM `gameobject` WHERE `guid` IN (9800020, 9800021);

INSERT INTO `gameobject_template`
(`entry`, `type`, `displayId`, `name`, `IconName`, `castBarCaption`, `unk1`, `size`,
 `Data0`, `Data1`, `Data2`, `Data3`, `Data4`, `Data5`, `Data6`, `Data7`, `Data8`, `Data9`,
 `Data10`, `Data11`, `Data12`, `Data13`, `Data14`, `Data15`, `Data16`, `Data17`, `Data18`,
 `Data19`, `Data20`, `Data21`, `Data22`, `Data23`, `AIName`, `ScriptName`, `VerifiedBuild`)
VALUES
(5000010, 10, 7017, 'Pawn Obelisk', '', '', '', 1.2,
 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
 0, 0, 0, 0, 0, 0, 0, 0, 0,
 0, 0, 0, 0, 0, '', 'go_pawn_obelisk', 12340);

INSERT INTO `gameobject_template_addon`
(`entry`, `faction`, `flags`, `mingold`, `maxgold`, `artkit0`, `artkit1`, `artkit2`, `artkit3`)
VALUES
(5000010, 0, 0, 0, 0, 0, 0, 0, 0);

INSERT INTO `gameobject`
(`guid`, `id`, `map`, `zoneId`, `areaId`, `spawnMask`, `phaseMask`,
 `position_x`, `position_y`, `position_z`, `orientation`,
 `rotation0`, `rotation1`, `rotation2`, `rotation3`,
 `spawntimesecs`, `animprogress`, `state`, `ScriptName`, `VerifiedBuild`)
VALUES
(9800020, 5000010, 0, 1519, 1519, 1, 1,
 -8489.0, 673.0, 102.428, 0,
 0, 0, 0, 1,
 300, 0, 1, '', 0),
(9800021, 5000010, 1, 1637, 1637, 1, 1,
 1766.42, -4219.41, 44.3204, 0,
 0, 0, 0, 1,
 300, 0, 1, '', 0);
