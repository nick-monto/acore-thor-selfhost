/*
 * mod-faction-invasion loader.
 *
 * The playerbots fork auto-globs every module's sources into one library and
 * looks up a loader symbol derived from the folder name: for folder
 * "mod-faction-invasion" that symbol is exactly
 * "Addmod_faction_invasionScripts" (the dash becomes an underscore). It must
 * exist and call our real registration function.
 */

void AddFactionInvasionScripts();

void Addmod_faction_invasionScripts()
{
    AddFactionInvasionScripts();
}
