/*
 * mod-survivor loader.
 *
 * The playerbots fork auto-globs every module's sources into one library and
 * looks up a loader symbol derived from the folder name: for folder
 * "mod-survivor" that symbol is exactly "Addmod_survivorScripts" (the dash
 * becomes an underscore). It must exist and call our real registration
 * function, the same way mod-hardcore's loader does.
 *
 * Released under GNU GPL v2 or (at your option) any later version.
 */

void AddSurvivorScripts();

void Addmod_survivorScripts()
{
    AddSurvivorScripts();
}
