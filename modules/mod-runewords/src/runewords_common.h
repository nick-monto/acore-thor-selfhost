/*
 * mod-runewords - shared declarations.
 *
 * Physical rune items drop from creatures on a level ladder (El at level 1
 * through Zod at 80). A player who holds an exact pattern of runes in one
 * container - the backpack, or one bag - is granted that word's spells, and
 * loses them the moment the pattern breaks.
 *
 *   - the pattern is geometry, not order in a list: a word is a set of
 *     (row, column) cells over the bag grid the client draws, which is four
 *     columns wide and row-major (FrameXML's NUM_CONTAINER_COLUMNS), so a word
 *     may run along a row, down a column, or around a corner;
 *   - a match is scoped to one container: bag windows have no stable order
 *     among themselves, so "in order" can only mean "inside the same grid";
 *   - the longest matching word wins, then the lowest word id, which makes the
 *     outcome independent of bag iteration order;
 *   - each rune instance carries a 1..100 roll (characters table
 *     character_runeword_rune), and a word is as strong as its weakest rune:
 *     the roll scales the borrowed spell's own basepoints, so the same stock
 *     spell serves every roll and no spell is authored per tier;
 *   - nothing is written to equipment, sockets or item enchantments: runes sit
 *     in the bags, so the word stacks on top of whatever gems and enchants the
 *     character already has, and no item slot is spent to make room for it;
 *   - the granted spells are stock auras scaled at cast time, so the client
 *     patch stays at the runes' own Item.dbc records. Their tooltips are the
 *     stock spell text, which is why the add-on, not the tooltip, is the place
 *     that shows the real numbers.
 *
 * Released under GNU GPL v2; redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef MOD_RUNEWORDS_COMMON_H
#define MOD_RUNEWORDS_COMMON_H

#include "ObjectGuid.h"

#include <cstdint>
#include <string>

class Player;

namespace Runewords
{
    // Cached config. Copied in and out under the module's own lock: it is
    // written on the world thread by WorldScript::OnAfterConfigLoad and read
    // from map-update threads, which is the same split every piece of state
    // below lives with.
    struct Config
    {
        bool Enable             = true;   // Runewords.Enable
        uint32 SweepIntervalMs  = 1000;   // Runewords.SweepIntervalMs
        bool ApplyWhenDead      = false;  // Runewords.ApplyWhenDead
        bool Announce           = true;   // Runewords.Announce
    };

    // By value, under the lock: a reference would hand a caller a struct another
    // thread may rewrite underneath it.
    Config GetConfig();
    void SetConfig(Config const& config);

    // characters database: the per-instance roll of every rune that was ever
    // seen in a bag. Created programmatically, not by module SQL: on this fork a
    // failing module SQL file aborts the whole worldserver boot.
    void EnsureSchema();

    // world database: runeword_words, written by
    // scripts/runewords/gen_runewords.py. Safe to call again on '.reload config'.
    void LoadWords();

    // Rescan one player's containers and converge the applied auras on what the
    // bags actually hold. Idempotent: an unchanged pattern costs one scan and
    // no cast.
    void Reconcile(Player* player);

    // The staggered sweep, kept beside the state it belongs to so one lock covers
    // both: true when this player's share of Runewords.SweepIntervalMs has passed.
    bool DueForScan(Player* player, uint32 diff);
    void ForgetScanState(ObjectGuid guid);

    // Remove every spell any word in the table names, whether or not this player
    // is recorded as holding one. Aura rows are only saved for non-passive
    // spells, and a word that broke while its holder was offline left exactly
    // this: an aura nobody's bookkeeping remembers. Cheap - a handful of no-op
    // RemoveAura lookups.
    void ClearWordAuras(Player* player);

    // Drop the per-player bookkeeping (the auras themselves are cleaned by the
    // core when the player leaves the world).
    void ForgetPlayer(ObjectGuid guid);

    // Take back every aura this module granted to one player. Only the reload
    // path needs it, and it needs it: replacing the word table with players
    // online would otherwise leave a spell that the new table no longer names up
    // for the rest of the session, because the reconcile only removes what the
    // word it is about to apply owns.
    void ClearApplied(Player* player);

    // The add-on asks, the module answers: the rolls are the one thing about a
    // word the client cannot work out for itself (the pattern table it ships is
    // generated from the same source). Returns false when the message was this
    // module's, which swallows it so nothing else acts on it.
    //
    //   client -> 'RUNE\tsync'
    //   module -> 'RUNE\tslots <bag>:<slot>:<roll>:<entry> ...'  (one or more)
    //
    // <bag> is the client's own numbering, 0 for the backpack and 1..4 for the
    // equipped bags, because that is what GetContainerItemLink takes; <slot> is
    // the cell index the pattern is written in, counted from zero, which is the
    // client's own container slot minus one.
    bool HandleAddonChat(Player* player, uint32 type, uint32 language, std::string& msg, Player* receiver);

    // Diagnostics, for '.runewords'.
    std::string DescribeWords();
    std::string DescribePlayer(Player* player);
    size_t CountWords();
}

#endif // MOD_RUNEWORDS_COMMON_H
