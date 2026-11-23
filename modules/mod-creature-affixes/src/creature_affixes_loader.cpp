/*
 * mod-creature-affixes loader.
 *
 * Folder name "mod-creature-affixes" makes the playerbots core look up exactly
 * Addmod_creature_affixesScripts (the dashes become underscores). It must exist
 * and call our real registration function, the same way mod-survivor's loader does.
 *
 * Released under GNU GPL v2 or (at your option) any later version.
 */

void AddCreatureAffixesScripts();

void Addmod_creature_affixesScripts()
{
    AddCreatureAffixesScripts();
}
