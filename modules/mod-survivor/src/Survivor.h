/*
 * mod-survivor - shared declarations.
 *
 * Opt-in "death is expensive" ruleset, one rung below Hardcore on a one-way
 * ladder (normal -> Survivor -> Hardcore, and normal -> Hardcore). A
 * character flags itself Survivor ('.survivor on') and from then on every death
 * lifts the equipped items and the bag contents off the body and puts them in a
 * lootable container at the death position, which is also announced to the
 * player's addon so it can draw a map pin.
 *
 *   - the equipment, the bags and what is inside them are snapshotted into
 *     survivor_corpse_items (a column-for-column copy of item_instance), so an
 *     instance keeps its enchants, gems, durability, charges and text),
 *   - the live items are destroyed, one lootable gameobject is summoned at the
 *     death spot and filled with one LootItem per saved instance,
 *   - any Survivor-flagged character may open any death container, whoever it
 *     came from; a character that never opted in may open none
 *     (GlobalScript::OnAllowedToLootContainerCheck); looting an entry rebuilds
 *     the *same* item_instance row, so recovery is lossless,
 *   - an unclaimed container re-summons when any flagged character enters its
 *     map, and its remaining items are mailed to the owner when the row expires
 *     - unless the row's no_mail is set (a bot's container, a Hardcore death's
 *     container), in which case they are deleted instead.
 *
 * Bot-controlled (headless) characters follow the same strip: their gear is
 * left in a container unless Survivor.BotDropContainer = 0 destroys it, and it
 * is replaced from playerbots' own factory once they are alive again
 * (PlayerbotFactory::AutoGear) - the bot is re-geared, so the container is loot
 * for the realm rather than something the bot needs back.
 *
 * A Hardcore-flagged death is handled here too: its gear and bags are dropped
 * into a no_mail container and nothing is revived, so Hardcore stays terminal.
 *
 * The bank, the mail, the gold and the quest log are never touched.
 *
 * Released under GNU GPL v2; redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef MOD_SURVIVOR_H
#define MOD_SURVIVOR_H

#include "ObjectGuid.h"

#include <cstdint>
#include <string>

class GameObject;
class Item;
class Player;

namespace Survivor
{
    // Cached config. Written only by WorldScript::OnAfterConfigLoad (world
    // thread, at startup and on '.reload config'); read from any thread after
    // that - the same one-writer/startup assumption the core's own config
    // accessors make.
    struct Config
    {
        bool Enable                = true;     // Survivor.Enable
        uint32_t MinLevelToFlag    = 10;       // Survivor.MinLevelToFlag
        bool AnnounceDeaths        = true;     // Survivor.AnnounceDeaths
        bool DropEquipped          = true;     // Survivor.DropEquipped
        bool DropBags              = true;     // Survivor.DropBags
        uint32_t ContainerLifetimeDays = 7;    // Survivor.ContainerLifetimeDays
        bool MarkOnMap             = true;     // Survivor.MarkOnMap
        uint32_t ContainerEntry    = 5000000;  // Survivor.ContainerEntry
        bool ApplyToBots           = true;     // Survivor.ApplyToBots
        bool BotRegenOnDeath       = true;     // Survivor.BotRegenOnDeath
        bool BotDropContainer      = false;    // Survivor.BotDropContainer
    };

    // Config cache reference. Readable from any thread; only OnAfterConfigLoad
    // writes it, before any character script can run.
    Config& GetConfig();

    // Creates the survivor_flags / survivor_corpses / survivor_corpse_items
    // tables in the characters DB if they do not already exist. World thread.
    void EnsureSchema();

    // (Re)loads the in-memory flagged-guid cache from the characters DB.
    // World thread, once at startup after EnsureSchema().
    void LoadFlags();

    // True if the character (by low guid) opted into Survivor. Reads the
    // in-memory cache only - no DB hit. World or map thread.
    bool IsFlaggedGuid(uint32_t guidLow);
    bool IsFlagged(Player* player);

    // True if the session is a bot-controlled (headless) one. Map thread.
    bool IsBotControlled(Player* player);

    // True when mod-hardcore has this character flagged (read from its table,
    // no link against the module). Cold paths only: a death, a revival, a
    // command.
    bool IsHardcoreFlagged(uint32_t guidLow);

    // Persists the opt-in flag and updates the cache. World thread (command).
    void Flag(Player* player);

    // Death entry point: snapshot + strip + container + marker and a queued
    // revival for a survivor, or the bot strip-and-queue path for a headless
    // session. The revival is queued even when nothing was left to drop.
    // Map thread.
    void HandleDeath(Player* player);

    // Runs the deferred playerbots re-roll for a bot queued by HandleDeath.
    // Map thread (PlayerScript::OnPlayerUpdate).
    void HandleBotUpdate(Player* player);

    // Revives a survivor queued by HandleDeath at the closest graveyard, in
    // place of the ghost the core would leave. No-op for any character with
    // nothing queued. Map thread (PlayerScript::OnPlayerUpdate).
    void HandleReviveUpdate(Player* player);

    // Replaces the template-level item the core stored with the saved
    // instance, or mails it when the bags are full. Map thread.
    void HandleLootItem(Player* player, Item* item, uint32_t count, ObjectGuid lootGuid);

    // Drops the in-memory link and, if the container is empty, its rows.
    // Map thread (AllGameObjectScript).
    void HandleContainerLootState(GameObject* go, uint32_t state);
    void HandleContainerRemoved(GameObject* go);

    // Re-summons the unclaimed containers of this player whose map is the
    // current one, and (re)sends the map marker. Also revives a survivor that
    // died and logged out before its revival tick, so it never appears as a
    // ghost. Map thread.
    void HandleLogin(Player* player);
    void HandleUpdateZone(Player* player);
    // A release that beats the revival tick: revives here instead, for a death
    // this module queued. Map thread.
    void HandleReleasedGhost(Player* player);

    // Addon channel: answers SURV\thello with the newest site. Returns false
    // when the message was consumed. Compared against the private-chat hook's
    // own parameters, so any non-addon whisper is passed through untouched.
    // World thread (chat processing).
    bool HandleAddonChat(Player* player, uint32_t type, uint32_t language, std::string& msg, Player* receiver);

    // GlobalScript::OnAllowedToLootContainerCheck wrapper. Returns true when
    // the container belongs to us and the player is NOT its owner: in this
    // core's inverted wrapper that means "deny". Map thread.
    bool DenyForeignContainerLoot(Player const* player, ObjectGuid source);

    // Mails the remaining items of every row whose expires_ts has passed,
    // then deletes the rows and the live object. World thread (WorldScript).
    void SweepExpired();
}

#endif // MOD_SURVIVOR_H
