/*
 * mod-pawn loader.
 *
 * The playerbots fork auto-globs every module's sources into one library and
 * looks up a loader symbol derived from the folder name: for folder "mod-pawn"
 * that symbol is exactly "Addmod_pawnScripts" (the dash becomes an underscore).
 * It must exist and call our real registration function, the same way
 * mod-survivor's loader does.
 *
 * Released under GNU GPL v2 or (at your option) any later version.
 */

void AddPawnScripts();

void Addmod_pawnScripts()
{
    AddPawnScripts();
}
