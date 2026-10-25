/*
 * mod-e-interact: server half of the EInteract addon (one key to loot, gather and talk).
 *
 * The 3.3.5 client cannot look for units around the player and InteractUnit is protected,
 * so the addon only sends an addon whisper to itself and the server does the work:
 *   "EINT\thello" -> reply "EINT\tready" (lets the addon know the module is installed)
 *   "EINT\tnear"  -> use the current target, or the nearest corpse / chest / NPC in interaction
 *                    range: loot it (see LootCorpse), start skinning/herbing/mining it with
 *                    CastSpell, open a chest, herb or ore node (see TryGameObject), or queue the
 *                    packet the client would send on right click (CMSG_GOSSIP_HELLO,
 *                    CMSG_LIST_INVENTORY, CMSG_GAMEOBJ_USE, ...).
 * Every reply is an addon whisper "EINT\t<result>": loot, gather, talk, use, none.
 *
 * Local change against upstream (Rainblower/mod-e-interact 0d17e70): the gameobject pass, so one
 * key also opens chests, herb and ore nodes and questgiver objects, not only creatures. See
 * UPSTREAM.md at the module root.
 */

#include "Chat.h"
#include "ConfigValueCache.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "Group.h"
#include "Log.h"
#include "LootMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace
{
constexpr char const* kPrefix = "EINT\t";
constexpr char const* kHello = "EINT\thello";
constexpr char const* kNear = "EINT\tnear";

enum class EInteractConfig
{
    ENABLED,
    LOOT,
    GATHER,
    TALK,
    USE,

    NUM_CONFIGS,
};

class EInteractConfigData : public ConfigValueCache<EInteractConfig>
{
public:
    EInteractConfigData() : ConfigValueCache(EInteractConfig::NUM_CONFIGS) { }

    void BuildConfigCache() override
    {
        SetConfigValue<bool>(EInteractConfig::ENABLED, "EInteract.Enable", true);
        SetConfigValue<bool>(EInteractConfig::LOOT, "EInteract.Loot", true);
        SetConfigValue<bool>(EInteractConfig::GATHER, "EInteract.Gather", true);
        SetConfigValue<bool>(EInteractConfig::TALK, "EInteract.Talk", true);
        SetConfigValue<bool>(EInteractConfig::USE, "EInteract.Use", true);
    }
};

EInteractConfigData config;

bool Enabled(EInteractConfig option)
{
    return config.GetConfigValue<bool>(option);
}

bool CanLootCorpse(Player* player, Creature* c)
{
    if (c->IsAlive() || !c->HasDynamicFlag(UNIT_DYNFLAG_LOOTABLE))
        return false;

    if (player->isAllowedToLoot(c))
        return true;

    // isAllowedToLoot misses corpses that only have money left
    if (c->loot.gold == 0)
        return false;

    if (Group* group = player->GetGroup())
        return group == c->GetLootRecipientGroup();

    return c->GetLootRecipient() == player;
}

bool CanTalkTo(Player* player, Creature* c)
{
    uint32 const flags = uint32(c->GetNpcFlags()) & ~uint32(UNIT_NPC_FLAG_SPELLCLICK | UNIT_NPC_FLAG_PLAYER_VEHICLE);
    return flags != 0 && player->GetNPCIfCanInteractWith(c->GetGUID(), UNIT_NPC_FLAG_NONE) == c;
}

Opcodes TalkOpcode(Creature* c)
{
    if (c->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP))
        return CMSG_GOSSIP_HELLO;
    if (c->HasNpcFlag(UNIT_NPC_FLAG_QUESTGIVER))
        return CMSG_QUESTGIVER_HELLO;
    if (c->HasNpcFlag(UNIT_NPC_FLAG_VENDOR))
        return CMSG_LIST_INVENTORY;
    if (c->HasNpcFlag(UNIT_NPC_FLAG_TRAINER))
        return CMSG_TRAINER_LIST;
    if (c->HasNpcFlag(UNIT_NPC_FLAG_BANKER))
        return CMSG_BANKER_ACTIVATE;
    if (c->HasNpcFlag(UNIT_NPC_FLAG_AUCTIONEER))
        return MSG_AUCTION_HELLO;
    return CMSG_GOSSIP_HELLO; // flight master, innkeeper and the rest are handled by PrepareGossipMenu
}

// Result for the addon: drives its autoloot, its messages and /ei debug
void Reply(Player* player, std::string const& what)
{
    LOG_DEBUG("module", "EInteract {}: {}", player->GetName(), what);
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, kPrefix + what);
    player->SendDirectMessage(&data);
}

// Gathering spell for a corpse: skinning, herbalism, mining, engineering (all SPELL_EFFECT_SKINNING)
uint32 GatherSpell(Player* player, Creature* c)
{
    if (c->IsAlive() || !c->HasUnitFlag(UNIT_FLAG_SKINNABLE))
        return 0;
    if (!c->IsCritter() && !c->loot.isLooted())
        return 0;

    SkillType const skill = c->GetCreatureTemplate()->GetRequiredLootSkill();
    int32 const skillValue = player->GetSkillValue(skill);
    if (skillValue <= 0)
        return 0;

    // same formula as Spell::CheckCast, so we do not get stuck on a corpse above our skill
    int32 const level = c->GetLevel();
    if ((skillValue < 100 ? (level - 10) * 10 : level * 5) > skillValue)
        return 0;

    switch (skill)
    {
        case SKILL_HERBALISM:   return 32605;
        case SKILL_MINING:      return 32606;
        case SKILL_ENGINEERING: return 49383;
        default:
            for (uint32 id : { 50305u, 32678u, 10768u, 8618u, 8617u })
                if (player->HasSpell(id))
                    return id;
            return 8613;
    }
}

void Queue(Player* player, Opcodes opcode, ObjectGuid guid)
{
    WorldPacket* packet = new WorldPacket(opcode, 8);
    *packet << guid;
    player->GetSession()->QueuePacket(packet);
}

WorldPacket GuidPacket(Opcodes opcode, ObjectGuid guid)
{
    WorldPacket packet(opcode, 8);
    packet << guid;
    return packet;
}

bool CanTakeSlot(Player* player, Loot* loot, uint8 slot)
{
    QuestItem* qitem = nullptr;
    QuestItem* ffaitem = nullptr;
    QuestItem* conditem = nullptr;
    LootItem* item = loot->LootItemInSlot(slot, player, &qitem, &ffaitem, &conditem);
    if (!item)
        return false;

    bool const looted = qitem ? qitem->is_looted : ffaitem ? ffaitem->is_looted : conditem ? conditem->is_looted : item->is_looted;
    if (looted || !item->AllowedForPlayer(player, loot->sourceWorldObjectGUID))
        return false;

    // leave items that are being rolled for, under master loot or won by someone else
    if (!qitem && item->is_blocked)
        return false;
    if (item->rollWinnerGUID && item->rollWinnerGUID != player->GetGUID())
        return false;
    if (Group* group = player->GetGroup())
        if (!item->is_underthreshold && loot->roundRobinPlayer && group->GetLootMethod() == MASTER_LOOT
            && group->GetMasterLooterGuid() != player->GetGUID() && !qitem && !ffaitem && !conditem)
            return false;

    return true;
}

// The 3.3.5 client silently ignores a corpse loot window it did not request itself, so the server
// walks the whole chain for it: CMSG_LOOT -> LOOT_MONEY -> AUTOSTORE for every slot -> LOOT_RELEASE.
// CMSG_LOOT goes through the CanPacketReceive hooks first, so mod-aoe-loot can merge nearby corpses.
void LootCorpse(Player* player, Creature* c)
{
    WorldSession* session = player->GetSession();
    ObjectGuid const guid = c->GetGUID();

    WorldPacket open = GuidPacket(CMSG_LOOT, guid);
    if (sScriptMgr->CanPacketReceive(session, open))
        session->HandleLootOpcode(open);

    if (player->GetLootGUID() != guid)
        return;

    Loot* loot = &c->loot;
    if (loot->gold)
    {
        WorldPacket money(CMSG_LOOT_MONEY, 0);
        session->HandleLootMoneyOpcode(money);
    }

    std::size_t const slots = loot->items.size() + loot->quest_items.size();
    for (std::size_t slot = 0; slot < slots && slot < 255; ++slot)
    {
        if (!CanTakeSlot(player, loot, uint8(slot)))
            continue;

        WorldPacket take(CMSG_AUTOSTORE_LOOT_ITEM, 1);
        take << uint8(slot);
        session->HandleAutostoreLootItemOpcode(take);
    }

    // whatever is left (full bags, rolls) stays on the corpse and can be picked up with another press
    if (player->GetLootGUID() == guid)
    {
        WorldPacket release = GuidPacket(CMSG_LOOT_RELEASE, guid);
        session->HandleLootReleaseOpcode(release);
    }
}

/**
 * Whether the player may open this object, mirroring Spell::CanOpenLock without a spell to read the
 * lock type off: an item key in the bags, the gathering or lockpicking skill the lock asks for, or a
 * spell the lock names. An object with no lock is open to everyone, which is the common case for
 * quest chests.
 */
bool CanOpenObject(Player* player, GameObject* go)
{
    GameObjectTemplate const* info = go->GetGOInfo();
    if (!info)
        return false;

    uint32 const lockId = info->GetLockId();
    if (!lockId)
        return true;

    LockEntry const* lockInfo = sLockStore.LookupEntry(lockId);
    if (!lockInfo)
        return false;

    // Any one case is enough, the same way the spell's own check takes the first that fits.
    for (int j = 0; j < MAX_LOCK_CASE; ++j)
    {
        switch (lockInfo->Type[j])
        {
            case LOCK_KEY_ITEM:
                if (lockInfo->Index[j] && player->HasItemCount(lockInfo->Index[j], 1, true))
                    return true;
                break;
            case LOCK_KEY_SKILL:
            {
                // The tool requirement and the skill are one number here: a node above the player's
                // skill stays shut, and the player can still open it by hand.
                SkillType const skill = SkillByLockType(LockType(lockInfo->Index[j]));
                if (skill != SKILL_NONE && player->GetSkillValue(skill) >= int32(lockInfo->Skill[j]))
                    return true;
                break;
            }
            case LOCK_KEY_SPELL:
                if (lockInfo->Index[j] && player->HasSpell(lockInfo->Index[j]))
                    return true;
                break;
            default:
                break;
        }
    }

    return false;
}

// A chest, herb or ore node: the same chain as LootCorpse, with SendLoot generating the loot the
// way a right click would. The 3.3.5 client ignores a loot window it did not ask for, so the server
// takes the money and every item the player may take and then releases the object itself.
void LootGameObject(Player* player, GameObject* go)
{
    WorldSession* session = player->GetSession();
    ObjectGuid const guid = go->GetGUID();

    player->SendLoot(guid, LOOT_CORPSE);
    if (player->GetLootGUID() != guid)
        return;

    Loot* loot = &go->loot;
    if (loot->gold)
    {
        WorldPacket money(CMSG_LOOT_MONEY, 0);
        session->HandleLootMoneyOpcode(money);
    }

    std::size_t const slots = loot->items.size() + loot->quest_items.size();
    for (std::size_t slot = 0; slot < slots && slot < 255; ++slot)
    {
        if (!CanTakeSlot(player, loot, uint8(slot)))
            continue;

        WorldPacket take(CMSG_AUTOSTORE_LOOT_ITEM, 1);
        take << uint8(slot);
        session->HandleAutostoreLootItemOpcode(take);
    }

    // The release is what marks an emptied chest or node deactivated, so the respawn and restock
    // timers run exactly as they do after a manual loot.
    if (player->GetLootGUID() == guid)
    {
        WorldPacket release = GuidPacket(CMSG_LOOT_RELEASE, guid);
        session->HandleLootReleaseOpcode(release);
    }
}

bool TryGameObject(Player* player, GameObject* go)
{
    if (!go || !go->isSpawned() || !player->CanSeeOrDetect(go))
        return false;

    if (go->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE))
        return false;

    if (!player->IsWithinDistInMap(go, go->GetInteractionDistance()))
        return false;

    switch (go->GetGoType())
    {
        case GAMEOBJECT_TYPE_CHEST:
            // Nothing left to take, or someone is already looting it.
            if (go->getLootState() != GO_READY || go->loot.isLooted())
                return false;
            if (!CanOpenObject(player, go))
                return false;

            Reply(player, "loot");
            LootGameObject(player, go);
            return true;
        case GAMEOBJECT_TYPE_QUESTGIVER:
        {
            // The gossip, quest, vendor or trainer window, through the same handler a right click uses.
            WorldPacket use(CMSG_GAMEOBJ_USE, 8);
            use << go->GetGUID();
            if (!sScriptMgr->CanPacketReceive(player->GetSession(), use))
                return false;

            Reply(player, "talk");
            player->GetSession()->HandleGameObjectUseOpcode(use);
            return true;
        }
        case GAMEOBJECT_TYPE_MAILBOX:
        {
            // The mailbox has no Use() case: the client asks for the inbox by sending the object's
            // GUID, and CanOpenMailBox checks the distance and the type.
            WorldPacket list(CMSG_GET_MAIL_LIST, 8);
            list << go->GetGUID();
            if (!sScriptMgr->CanPacketReceive(player->GetSession(), list))
                return false;

            Reply(player, "use");
            player->GetSession()->HandleGetMailList(list);
            return true;
        }
        default:
            break;
    }

    return false;
}

bool TryInteract(Player* player, Creature* c)
{
    if (!c || !player->IsWithinDistInMap(c, INTERACTION_DISTANCE) || !player->CanSeeOrDetect(c))
        return false;

    if (Enabled(EInteractConfig::LOOT) && CanLootCorpse(player, c))
    {
        Reply(player, "loot");
        LootCorpse(player, c);
        return true;
    }

    if (Enabled(EInteractConfig::GATHER))
    {
        if (uint32 spell = GatherSpell(player, c))
        {
            if (player->IsNonMeleeSpellCast(false))
                return true; // already gathering
            Reply(player, "gather");
            player->CastSpell(c, spell, false);
            return true;
        }
    }

    if (Enabled(EInteractConfig::TALK) && c->IsAlive() && CanTalkTo(player, c))
    {
        Reply(player, "talk");
        Queue(player, TalkOpcode(c), c->GetGUID());
        return true;
    }

    return false;
}

void HandleNear(Player* player)
{
    if (!player->IsAlive() || player->IsInFlight())
        return;

    if (Unit* sel = player->GetSelectedUnit())
        if (TryInteract(player, sel->ToCreature()))
            return;

    std::list<Creature*> nearby;
    player->GetDeadCreatureListInGrid(nearby, INTERACTION_DISTANCE + 5.0f, false);
    nearby.sort([player](Creature* a, Creature* b) { return player->GetDistance(a) < player->GetDistance(b); });

    for (Creature* c : nearby)
        if (!c->IsAlive())
            LOG_DEBUG("module", "EInteract {}: corpse {} '{}' dist={:.1f} lootable={} allowed={} looted={} gold={} items={} lootType={} recipient={} group={} skinnable={}",
                player->GetName(), c->GetEntry(), c->GetName(), player->GetDistance(c),
                c->HasDynamicFlag(UNIT_DYNFLAG_LOOTABLE), player->isAllowedToLoot(c), c->loot.isLooted(),
                c->loot.gold, c->loot.items.size(), uint32(c->loot.loot_type),
                c->GetLootRecipient() ? c->GetLootRecipient()->GetName() : "-",
                c->GetLootRecipientGroup() ? 1 : 0, c->HasUnitFlag(UNIT_FLAG_SKINNABLE));

    // corpses first, then chests and questgiver objects, then NPCs
    for (Creature* c : nearby)
        if (!c->IsAlive() && TryInteract(player, c))
            return;

    if (Enabled(EInteractConfig::USE))
    {
        // The nearest chest or node, then the nearest questgiver object. FindNearestGameObjectOfType
        // is the core's own search: spawned, in range, nearest first.
        float const search = INTERACTION_DISTANCE + 5.0f;
        if (GameObject* chest = player->FindNearestGameObjectOfType(GAMEOBJECT_TYPE_CHEST, search))
            if (TryGameObject(player, chest))
                return;
        if (GameObject* questObject = player->FindNearestGameObjectOfType(GAMEOBJECT_TYPE_QUESTGIVER, search))
            if (TryGameObject(player, questObject))
                return;
        if (GameObject* mailbox = player->FindNearestGameObjectOfType(GAMEOBJECT_TYPE_MAILBOX, search))
            if (TryGameObject(player, mailbox))
                return;
    }

    for (Creature* c : nearby)
        if (c->IsAlive() && TryInteract(player, c))
            return;

    Reply(player, "none");
}
} // namespace

class EInteractPlayerScript : public PlayerScript
{
public:
    EInteractPlayerScript() : PlayerScript("EInteractPlayerScript", { PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT }) { }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Player* receiver) override
    {
        if (lang != LANG_ADDON || type != CHAT_MSG_WHISPER || receiver != player || !Enabled(EInteractConfig::ENABLED))
            return true;

        if (msg == kHello)
            Reply(player, "ready");
        else if (msg == kNear)
            HandleNear(player);
        else
            return true;

        return false;
    }
};

class EInteractWorldScript : public WorldScript
{
public:
    EInteractWorldScript() : WorldScript("EInteractWorldScript", { WORLDHOOK_ON_BEFORE_CONFIG_LOAD }) { }

    void OnBeforeConfigLoad(bool reload) override
    {
        config.Initialize(reload);
    }
};

void AddSC_e_interact()
{
    new EInteractPlayerScript();
    new EInteractWorldScript();
}
