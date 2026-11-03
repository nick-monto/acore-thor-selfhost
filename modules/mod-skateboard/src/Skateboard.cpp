/*
 * mod-skateboard - the module itself.
 *
 * A self-hosted-realm toy, and a deliberate experiment: the character is not
 * moved by the server, it is *allowed* to move faster. Everything below is
 * either bookkeeping around that one number (`Deck::v`) or safety around the
 * one dangerous operation (publishing it as a run speed rate).
 *
 *   - '.board on' refuses a long list of situations first (dead, already
 *     boarded, mounted, on a vehicle, flying, in water, a battleground, an
 *     arena, mid-teleport, inside an instance, a bot session), then writes the
 *     deck into UNIT_FIELD_MOUNTDISPLAYID. MountedPose 0 writes the field only,
 *     so the character keeps its standing pose - a skateboard wants a standing
 *     rider; MountedPose 1 goes through Unit::Mount() and gets the mount flag
 *     and the sitting rider the client draws for it.
 *   - Every PlayerScript update integrates the deck: terrain gradient under the
 *     heading (Map::GetGridHeight, two samples 2 yd apart, no vmaps, no normals
 *     exist), static plus rolling friction, an optional push impulse, airtime
 *     that ignores both.
 *   - Publishing is a *cap*, not movement: SetSpeed(MOVE_RUN, max(1, v / 7), true).
 *     MovementHandler.cpp kicks a client that reports more speed than the
 *     server holds, so exactly one forced change is in flight at a time and the
 *     gate is opened by CMSG_FORCE_RUN_SPEED_CHANGE_ACK - or by AckTimeoutMs passing,
 *     because a lost ACK must never wedge a deck forever; then a 100 ms throttle
 *     stops a fast RTT from turning into a packet flood. Drift is re-asserted
 *     (an aura refresh calls UpdateSpeed and wipes the rate), which also keeps
 *     MountedPose 1 honest.
 *   - MOVEMENTFLAG_FALLING is the trick button. Take-off queues a clip for the
 *     speed band and plays the band's emote plus one stock visual kit; a
 *     landing before the clip ends crashes the deck (damage, optional root),
 *     a landing at or after it is clean (LandingBonus, EMOTE_ONESHOT_LAND).
 *   - The deck never outlives its session: death, logout, combat (config),
 *     a map change, water, flying, a battleground or an arena drops it, and
 *     every drop clears the mount field.
 *
 * Released under GNU GPL v2; redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Chat.h"
#include "Config.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "GridTerrainData.h"
#include "Log.h"
#include "Map.h"
#include "ObjectGuid.h"
#include "Opcodes.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "Unit.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include "skateboard_common.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <unordered_map>

namespace
{
    using Skateboard::Deck;

    // One forced speed change is in flight at a time; when the gate is open the
    // next one waits this long after the previous ACK, so a 1-5 ms RTT cannot
    // turn the integrator into a packet flood.
    constexpr uint32 PublishThrottleMs = 100;

    // One diagnostic line per boarded player per five seconds.
    constexpr uint32 DebugIntervalMs = 5000;

    // A hop shorter than this is a step off a kerb, not a trick attempt.
    constexpr uint32 MinAirtimeMs = 250;

    // Display ids behind the three deck names. 26609 is the stock "Log Ride"
    // creature display and needs no client patch; the two wall ids arrive with
    // the step-4 DBC patch and fall back to the log until then.
    constexpr uint32 DeckLog          = 26609;
    constexpr uint32 DeckEngravedWall = 90001;
    constexpr uint32 DeckCoffinLid    = 90002;

    Skateboard::Config& ConfigMut()
    {
        static Skateboard::Config cfg;
        return cfg;
    }

    std::unordered_map<ObjectGuid, Deck>& Decks()
    {
        static std::unordered_map<ObjectGuid, Deck> decks;
        return decks;
    }

    uint32 NowMs()
    {
        return uint32(GameTime::GetGameTimeMS().count());
    }

    // Wrap-safe "the deadline has passed" (the counter rolls over in ~49 days).
    bool Reached(uint32 now, uint32 deadline)
    {
        return int32(now - deadline) >= 0;
    }

    Deck* FindDeck(Player const* player)
    {
        if (!player)
            return nullptr;

        auto it = Decks().find(player->GetGUID());
        return it == Decks().end() ? nullptr : &it->second;
    }

    // The stock clip for the speed band the take-off happened in.
    uint32 ClipForSpeed(float v, float maxSpeed)
    {
        float const span = std::max(maxSpeed, 0.001f);
        if (v < span / 3.0f)
            return Skateboard::CLIP_SHORT_MS;
        if (v < span * 2.0f / 3.0f)
            return Skateboard::CLIP_MEDIUM_MS;
        return Skateboard::CLIP_LONG_MS;
    }

    uint32 EmoteForClip(uint32 clip)
    {
        if (clip == Skateboard::CLIP_SHORT_MS)
            return EMOTE_ONESHOT_JUMPSTART;   // 399
        if (clip == Skateboard::CLIP_MEDIUM_MS)
            return EMOTE_ONESHOT_JUMPLANDRUN; // 394
        return EMOTE_ONESHOT_STOMP;           // 388
    }

    // Clears the mount display field and the flags around it. Mode 0 never set
    // UNIT_FLAG_MOUNT, so Dismount() (which returns early when IsMounted() is
    // false) is only the half of the job the Pose == 1 case needs; the field
    // itself is always written back to 0.
    void ClearVisual(Player* player, Deck& deck)
    {
        player->SetUInt32Value(UNIT_FIELD_MOUNTDISPLAYID, 0);

        if (player->IsMounted())
            player->Dismount();

        deck.displayId = 0;
        deck.v = 0.0f;
        deck.airborne = false;
        deck.takeoffMs = 0;
        deck.trickEndMs = 0;
        deck.activeClip = 0;
        deck.publishedRate = 1.0f;
        deck.pendingAcks = 0;
        deck.gateDeadlineMs = 0;
    }

    // Put the run rate back where the core expects a character on foot to be, and do it from the
    // auras the unit actually has: UpdateSpeed(MOVE_RUN, true) is the core's own recompute and
    // force-send (Unit.cpp:10887). Writing 1.0 directly would flatten a speed aura - Sprint,
    // Aspect of the Cheetah, a mount aura, a snare - until that aura is next applied or removed.
    // SetSpeed() returns without a packet when the value is unchanged, so this is free for a deck
    // that never got to move.
    void RestoreRunRate(Player* player)
    {
        if (std::fabs(player->GetSpeedRate(MOVE_RUN) - 1.0f) > 0.001f)
            player->UpdateSpeed(MOVE_RUN, true);
    }

    // Sends one gated forced speed change: one in flight, ACK deadline armed.
    // SetSpeed(MOVE_RUN, ..., true) is what emits SMSG_FORCE_RUN_SPEED_CHANGE
    // and bumps the core's unacknowledged-change counter; the matching
    // CMSG_FORCE_RUN_SPEED_CHANGE_ACK opens the gate in OnAck().
    void PublishRate(Player* player, Deck& deck, float target, uint32 now)
    {
        player->SetSpeed(MOVE_RUN, target, true);
        deck.publishedRate = target;
        deck.pendingAcks = 1;
        deck.gateDeadlineMs = now + Skateboard::GetConfig().AckTimeoutMs;
    }

    // The wipeout: drop the deck, take the damage, and optionally root the
    // character for a moment. The record is erased first, because the damage can
    // kill the character and OnPlayerJustDied would erase it from under us; a
    // stagger is then served by a fresh, deckless record (displayId 0) that the
    // tick releases.
    void CrashDeck(Player* player)
    {
        Skateboard::Config const& cfg = Skateboard::GetConfig();
        ObjectGuid const guid = player->GetGUID();

        auto it = Decks().find(guid);
        if (it == Decks().end() || it->second.displayId == 0)
            return;

        uint32 const staggerMs = cfg.CrashStaggerMs;

        ClearVisual(player, it->second);
        Decks().erase(it);
        RestoreRunRate(player);

        uint32 damage = std::max<uint32>(1, uint32(float(player->GetMaxHealth()) * float(cfg.CrashDamagePct) / 100.0f));
        if (cfg.HardcoreSafe)
        {
            // Leave at least one point of health - and at exactly one point, take nothing, or the
            // floor of one damage would still kill a Hardcore character.
            uint32 const health = player->GetHealth();
            damage = std::min(damage, health > 1 ? health - 1 : 0);
        }

        if (damage)
            player->EnvironmentalDamage(DAMAGE_FALL, damage);

        if (staggerMs && player->IsAlive())
        {
            Deck& record = Decks()[guid];
            record = Deck();
            record.mapId = player->GetMapId();
            record.staggerDeadlineMs = NowMs() + staggerMs;
            player->SetControlled(true, UNIT_STATE_ROOT);
        }
    }
}

namespace Skateboard
{
    namespace
    {
        // Cleared by LoadConfig(), set by the first ResolveDeck() after it.
        bool deckResolved = false;
    }

    Config const& GetConfig()
    {
        return ConfigMut();
    }

    bool LoadConfig()
    {
        Config& cfg = ConfigMut();

        cfg.Enable = sConfigMgr->GetOption<bool>("Skateboard.Enable", false);
        cfg.DeckName = sConfigMgr->GetOption<std::string>("Skateboard.Deck", "log");
        cfg.MountedPose = std::min<uint32>(sConfigMgr->GetOption<uint32>("Skateboard.MountedPose", 0), 1);
        cfg.MaxSpeed = std::clamp(sConfigMgr->GetOption<float>("Skateboard.MaxSpeed", 21.0f), 1.0f, 40.0f);
        cfg.PushImpulse = std::clamp(sConfigMgr->GetOption<float>("Skateboard.PushImpulse", 4.0f), 0.0f, 20.0f);
        cfg.FrictionStatic = std::clamp(sConfigMgr->GetOption<float>("Skateboard.Friction.Static", 1.5f), 0.0f, 20.0f);
        cfg.FrictionRolling = std::clamp(sConfigMgr->GetOption<float>("Skateboard.Friction.Rolling", 0.004f), 0.0f, 1.0f);
        cfg.SlopeGain = std::clamp(sConfigMgr->GetOption<float>("Skateboard.SlopeGain", 9.0f), 0.0f, 90.0f);
        cfg.LandingBonus = std::clamp(sConfigMgr->GetOption<float>("Skateboard.LandingBonus", 2.0f), 0.0f, 20.0f);
        cfg.Tricks = sConfigMgr->GetOption<bool>("Skateboard.Tricks", true);
        cfg.TrickVisualKit = std::min<uint32>(sConfigMgr->GetOption<uint32>("Skateboard.Trick.VisualKit", 5036), 100000);
        cfg.CrashEnable = sConfigMgr->GetOption<bool>("Skateboard.Crash.Enable", true);
        cfg.CrashDamagePct = std::min<uint32>(sConfigMgr->GetOption<uint32>("Skateboard.Crash.DamagePct", 8), 99);
        cfg.CrashStaggerMs = std::min<uint32>(sConfigMgr->GetOption<uint32>("Skateboard.Crash.StaggerMs", 1500), 10000);
        cfg.HardcoreSafe = sConfigMgr->GetOption<bool>("Skateboard.HardcoreSafe", true);
        cfg.OffInCombat = sConfigMgr->GetOption<bool>("Skateboard.OffInCombat", true);
        cfg.AckTimeoutMs = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Skateboard.AckTimeoutMs", 500), 50, 5000);
        cfg.Debug = sConfigMgr->GetOption<bool>("Skateboard.Debug", false);

        // The deck is only named here. Resolving it needs CreatureDisplayInfo.dbc, and
        // OnAfterConfigLoad runs before LoadDBCStores (World.cpp:323 against :385), so a lookup
        // at this point fails for every id on a cold boot. ResolveDeck() does it on the first
        // board instead, which is after the stores exist.
        deckResolved = false;
        cfg.DeckDisplayId = DeckLog;
        return true;
    }

    // Name or raw display id to a display id the client can draw. Anything that does not resolve
    // to an entry in the client's display table falls back to the stock log, so a missing step-4
    // DBC patch is a warning, never an invisible mount.
    uint32 ResolveDeck()
    {
        Config& cfg = ConfigMut();

        if (deckResolved)
            return cfg.DeckDisplayId;

        deckResolved = true;

        std::string deck = cfg.DeckName;
        std::transform(deck.begin(), deck.end(), deck.begin(), [](unsigned char c) { return char(std::tolower(c)); });

        uint32 displayId = 0;

        if (deck == "log")
            displayId = DeckLog;
        else if (deck == "engravedwall")
            displayId = DeckEngravedWall;
        else if (deck == "coffinlid")
            displayId = DeckCoffinLid;
        else
        {
            char* end = nullptr;
            unsigned long const parsed = std::strtoul(deck.c_str(), &end, 10);
            if (end && *end == '\0' && parsed > 0 && parsed <= 0xFFFFFFFFul)
                displayId = uint32(parsed);
            else
            {
                LOG_WARN("module", "Skateboard: unknown deck '{}', expected log, engravedwall, coffinlid or a display id. Using {}.",
                    cfg.DeckName, DeckLog);
                displayId = DeckLog;
            }
        }

        if (displayId != DeckLog && !sCreatureDisplayInfoStore.LookupEntry(displayId))
        {
            LOG_WARN("module", "Skateboard: display id {} for deck '{}' is not in CreatureDisplayInfo.dbc, falling back to {}.",
                displayId, cfg.DeckName, DeckLog);
            displayId = DeckLog;
        }

        if (!sCreatureDisplayInfoStore.LookupEntry(displayId))
            LOG_WARN("module", "Skateboard: display id {} is not in CreatureDisplayInfo.dbc; the deck will not render.", displayId);

        cfg.DeckDisplayId = displayId;
        return displayId;
    }
}

using namespace Acore::ChatCommands;

namespace
{
    // ---- Manager -----------------------------------------------------------

    bool IsBoardable(Player* player, ChatHandler* handler)
    {
        Skateboard::Config const& cfg = Skateboard::GetConfig();

        if (!cfg.Enable)
        {
            handler->SendSysMessage("The skateboard is disabled on this realm.");
            return false;
        }

        if (!player->IsAlive())
        {
            handler->SendSysMessage("You cannot board while dead.");
            return false;
        }

        // OffInCombat drops the deck when combat starts, so boarding during combat would hand out
        // the same 21 yd/s disengage through the other door and nothing would take it back.
        if (cfg.OffInCombat && player->IsInCombat())
        {
            handler->SendSysMessage("You cannot board in combat.");
            return false;
        }

        Deck* existing = FindDeck(player);
        if (existing && existing->displayId != 0)
        {
            handler->SendSysMessage("You are already on a skateboard.");
            return false;
        }

        if (player->IsMounted())
        {
            handler->SendSysMessage("You cannot board while mounted.");
            return false;
        }

        if (player->GetVehicle())
        {
            handler->SendSysMessage("You cannot board while on a vehicle.");
            return false;
        }

        if (player->IsFlying())
        {
            handler->SendSysMessage("You cannot board while flying.");
            return false;
        }

        if (player->IsInWater())
        {
            handler->SendSysMessage("You cannot board while swimming.");
            return false;
        }

        if (player->InBattleground())
        {
            handler->SendSysMessage("You cannot board in a battleground.");
            return false;
        }

        if (player->InArena())
        {
            handler->SendSysMessage("You cannot board in an arena.");
            return false;
        }

        if (player->IsBeingTeleported())
        {
            handler->SendSysMessage("You cannot board while teleporting.");
            return false;
        }

        if (player->GetMap()->IsDungeon())
        {
            handler->SendSysMessage("You cannot board inside an instance.");
            return false;
        }

        // Random/alt bots share the player session class; the playerbots layer
        // marks them as headless, which is the same gate mod-hardcore uses.
        WorldSession* session = player->GetSession();
        if (!session || session->IsHeadless())
        {
            handler->SendSysMessage("Skateboards are not available for bot-controlled characters.");
            return false;
        }

        return true;
    }
}

namespace Skateboard
{
    Manager& GetManager()
    {
        static Manager manager;
        return manager;
    }

    bool Manager::Board(Player* player, ChatHandler* handler)
    {
        if (!player || !handler)
            return false;

        // After IsBoardable's refusals, and before any display id is read: this is the first
        // point at which the DBC stores are guaranteed to exist.
        ResolveDeck();

        Config const& cfg = GetConfig();

        if (!IsBoardable(player, handler))
        {
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint32 const now = NowMs();

        // A crash may have left a deckless record behind to serve its stagger;
        // boarding releases the root and starts from a clean record.
        auto existing = Decks().find(player->GetGUID());
        if (existing != Decks().end())
        {
            if (existing->second.staggerDeadlineMs)
                player->SetControlled(false, UNIT_STATE_ROOT);
            Decks().erase(existing);
        }

        Deck& deck = Decks()[player->GetGUID()];
        deck = Deck();
        deck.displayId = cfg.DeckDisplayId;
        deck.mapId = player->GetMapId();
        deck.gateDeadlineMs = now + PublishThrottleMs;
        deck.debugNextMs = now;

        if (cfg.MountedPose == 0)
            player->SetUInt32Value(UNIT_FIELD_MOUNTDISPLAYID, cfg.DeckDisplayId);
        else
            player->Mount(cfg.DeckDisplayId);

        // A rate of 1.0 is the character's own run speed: SetSpeed() returns
        // without a packet when the server already holds it, so the gate only
        // arms when a change is actually sent.
        if (std::fabs(player->GetSpeedRate(MOVE_RUN) - 1.0f) > 0.001f)
            PublishRate(player, deck, 1.0f, now);

        handler->PSendSysMessage("You step onto the board. Display {}.", cfg.DeckDisplayId);
        return true;
    }

    void Manager::Leave(Player* player)
    {
        if (!player)
            return;

        auto it = Decks().find(player->GetGUID());
        if (it == Decks().end())
            return;

        bool const wasBoarded = it->second.displayId != 0;

        if (it->second.staggerDeadlineMs)
            player->SetControlled(false, UNIT_STATE_ROOT);

        ClearVisual(player, it->second);
        Decks().erase(it);

        if (wasBoarded)
            RestoreRunRate(player);
    }

    void Manager::Push(Player* player, ChatHandler* handler)
    {
        Deck* deck = FindDeck(player);
        if (!deck || deck->displayId == 0)
        {
            if (handler)
            {
                handler->SendSysMessage("You are not on a skateboard.");
                handler->SetSentErrorMessage(true);
            }
            return;
        }

        Config const& cfg = GetConfig();
        deck->v = std::min(deck->v + cfg.PushImpulse, cfg.MaxSpeed);
    }

    void Manager::PlayTrick(Player* player, uint32 clip, ChatHandler* handler)
    {
        if (!player || clip < 1 || clip > 3)
        {
            if (handler)
            {
                handler->SendSysMessage("Trick clips are 1 (short), 2 (medium) and 3 (long).");
                handler->SetSentErrorMessage(true);
            }
            return;
        }

        Deck* deck = FindDeck(player);
        if (!deck || deck->displayId == 0)
        {
            if (handler)
            {
                handler->SendSysMessage("You are not on a skateboard.");
                handler->SetSentErrorMessage(true);
            }
            return;
        }

        Config const& cfg = GetConfig();
        uint32 const now = NowMs();

        deck->activeClip = clip == 1 ? CLIP_SHORT_MS : (clip == 2 ? CLIP_MEDIUM_MS : CLIP_LONG_MS);
        deck->trickEndMs = now + deck->activeClip;

        player->HandleEmoteCommand(EmoteForClip(deck->activeClip));
        if (cfg.TrickVisualKit)
            player->SendPlaySpellVisual(cfg.TrickVisualKit);
    }

    bool Manager::IsBoarded(Player const* player) const
    {
        Deck* deck = FindDeck(player);
        return deck && deck->displayId != 0;
    }

    float Manager::Speed(Player const* player) const
    {
        Deck* deck = FindDeck(player);
        return deck ? deck->v : 0.0f;
    }

    uint8 Manager::PendingAcks(Player const* player) const
    {
        Deck* deck = FindDeck(player);
        return deck ? deck->pendingAcks : 0;
    }

    void Manager::OnAck(Player* player)
    {
        Deck* deck = FindDeck(player);
        if (!deck)
            return;

        deck->pendingAcks = 0;
        // Open, but throttled: the next send waits 100 ms after this ACK.
        deck->gateDeadlineMs = NowMs() + PublishThrottleMs;
    }

    void Manager::Update(Player* player, uint32 diff)
    {
        if (!player)
            return;

        auto it = Decks().find(player->GetGUID());
        if (it == Decks().end())
            return;

        uint32 const now = NowMs();
        Deck& deck = it->second;

        // A record left behind by a crash: the deck is gone, only the stagger
        // release remains.
        if (deck.displayId == 0)
        {
            if (!deck.staggerDeadlineMs || !player->IsAlive() || !player->IsInWorld())
            {
                if (deck.staggerDeadlineMs)
                    player->SetControlled(false, UNIT_STATE_ROOT);
                Decks().erase(it);
                return;
            }

            if (Reached(now, deck.staggerDeadlineMs))
            {
                player->SetControlled(false, UNIT_STATE_ROOT);
                deck.staggerDeadlineMs = 0;
                Decks().erase(it);
            }
            return;
        }

        Config const& cfg = GetConfig();

        // `.reload config` with Enable back to 0 must take the deck away, not leave a capped
        // player running the old rate for the rest of the session.
        if (!cfg.Enable)
        {
            Cleanup(player);
            return;
        }

        // Combat is in this list as well as in OnPlayerEnterCombat: OffInCombat can be turned on
        // by a reload while a player is already fighting.
        if (!player->IsAlive() || !player->IsInWorld() || player->GetMapId() != deck.mapId ||
            player->IsInWater() || player->IsFlying() || player->InBattleground() || player->InArena() ||
            player->GetVehicle() || (cfg.OffInCombat && player->IsInCombat()))
        {
            Cleanup(player);
            return;
        }

        // The ACK gate. A lost ACK must not wedge the deck: clear it after the
        // timeout and say so once.
        if (deck.pendingAcks > 0 && Reached(now, deck.gateDeadlineMs))
        {
            deck.pendingAcks = 0;

            if (cfg.Debug)
                LOG_INFO("module", "Skateboard: {} sent no run speed ACK within {} ms, gate reopened.", player->GetName(), cfg.AckTimeoutMs);
            else
                LOG_WARN("module", "Skateboard: {} sent no run speed ACK within {} ms, gate reopened.", player->GetName(), cfg.AckTimeoutMs);
        }

        float const dt = std::min(float(diff) / 1000.0f, 0.25f);
        float slope = 0.0f;

        if (!deck.airborne)
        {
            float const x = player->GetPositionX();
            float const y = player->GetPositionY();
            float const o = player->GetOrientation();

            float const h0 = player->GetMap()->GetGridHeight(x, y);
            float const h2 = player->GetMap()->GetGridHeight(x + std::cos(o) * 2.0f, y + std::sin(o) * 2.0f);

            // INVALID_HEIGHT means the grid is not loaded: no gradient, no
            // invented acceleration.
            if (h0 > INVALID_HEIGHT + 1.0f && h2 > INVALID_HEIGHT + 1.0f)
                slope = -cfg.SlopeGain * (h2 - h0) / 2.0f;

            float acceleration = slope;
            acceleration -= cfg.FrictionStatic + cfg.FrictionRolling * deck.v * deck.v;

            deck.v += acceleration * dt;
            deck.v = std::clamp(deck.v, 0.0f, cfg.MaxSpeed);
        }

        float const target = std::max(1.0f, deck.v / BASE_RUN_RATE);

        if (deck.pendingAcks == 0 && Reached(now, deck.gateDeadlineMs) &&
            std::fabs(player->GetSpeedRate(MOVE_RUN) - target) > 0.01f)
        {
            PublishRate(player, deck, target, now);
        }

        if (cfg.Debug && Reached(now, deck.debugNextMs))
        {
            deck.debugNextMs = now + DebugIntervalMs;
            LOG_INFO("module", "Skateboard: {} v={:.2f} rate={:.2f} published={:.2f} pending={} slope={:.2f} airborne={}",
                player->GetName(), deck.v, player->GetSpeedRate(MOVE_RUN), deck.publishedRate,
                uint32(deck.pendingAcks), slope, deck.airborne ? 1 : 0);
        }
    }

    void Manager::OnMove(Player* player, MovementInfo const& movementInfo)
    {
        Deck* deck = FindDeck(player);
        if (!deck || deck->displayId == 0)
            return;

        bool const falling = movementInfo.HasMovementFlag(MOVEMENTFLAG_FALLING);
        if (falling == deck->airborne)
            return;

        Config const& cfg = GetConfig();
        uint32 const now = NowMs();

        if (falling)
        {
            // Rising edge: this is the take-off. Remember when, and which clip
            // the speed band queued.
            deck->airborne = true;
            deck->takeoffMs = now;
            deck->activeClip = ClipForSpeed(deck->v, cfg.MaxSpeed);
            deck->trickEndMs = cfg.Tricks ? now + deck->activeClip : 0;

            if (cfg.Tricks)
            {
                player->HandleEmoteCommand(EmoteForClip(deck->activeClip));
                if (cfg.TrickVisualKit)
                    player->SendPlaySpellVisual(cfg.TrickVisualKit);
            }
            return;
        }

        // Falling edge: the landing. Speed is kept through the arc, slope and
        // friction start again on the next tick.
        deck->airborne = false;

        uint32 const airtime = now - deck->takeoffMs;
        bool const mistimed = deck->trickEndMs != 0 && !Reached(now, deck->trickEndMs);

        deck->trickEndMs = 0;
        deck->activeClip = 0;

        if (airtime < MinAirtimeMs)
            return;

        if (mistimed)
        {
            if (cfg.CrashEnable)
                CrashDeck(player);
            else
                Leave(player);
            return;
        }

        // A clean landing pays whether or not tricks are scored: with Skateboard.Tricks = 0 there
        // is no clip to mistime, so every landing is clean and the bonus is the whole of that
        // system. Both keys are presented as independent, and this is what independence means.
        if (cfg.LandingBonus > 0.0f)
        {
            deck->v = std::min(deck->v + cfg.LandingBonus, cfg.MaxSpeed);
            player->HandleEmoteCommand(EMOTE_ONESHOT_LAND);
        }
    }

    void Manager::Cleanup(Player* player)
    {
        if (!player)
            return;

        auto it = Decks().find(player->GetGUID());
        if (it == Decks().end())
            return;

        bool const wasBoarded = it->second.displayId != 0;

        if (it->second.staggerDeadlineMs)
            player->SetControlled(false, UNIT_STATE_ROOT);

        ClearVisual(player, it->second);
        Decks().erase(it);

        if (wasBoarded)
            RestoreRunRate(player);
    }
}

// =====================================================================
//  WorldScript: config
// =====================================================================
class SkateboardWorldScript : public WorldScript
{
public:
    SkateboardWorldScript() : WorldScript("Skateboard_WorldScript") { }

    void OnAfterConfigLoad(bool reload) override
    {
        Skateboard::LoadConfig();

        // The display id is not resolved here: OnAfterConfigLoad runs before LoadDBCStores, so
        // the lookup would fail for every id. The deck is resolved on the first board.
        LOG_INFO("module", "Skateboard: {} (deck '{}', resolved on the first board).",
            Skateboard::GetConfig().Enable ? "enabled" : "disabled",
            Skateboard::GetConfig().DeckName);

        if (reload)
            LOG_INFO("module", "Skateboard: configuration reloaded.");
    }
};

// =====================================================================
//  PlayerScript: the integrator, and every reason the deck must go
// =====================================================================
class SkateboardPlayerScript : public PlayerScript
{
public:
    SkateboardPlayerScript() : PlayerScript("Skateboard_PlayerScript") { }

    void OnPlayerUpdate(Player* player, uint32 p_time) override
    {
        Skateboard::GetManager().Update(player, p_time);
    }

    void OnPlayerJustDied(Player* player) override
    {
        Skateboard::GetManager().Cleanup(player);
    }

    void OnPlayerBeforeLogout(Player* player) override
    {
        Skateboard::GetManager().Cleanup(player);
    }

    void OnPlayerEnterCombat(Player* player, Unit* /*enemy*/) override
    {
        if (Skateboard::GetConfig().OffInCombat)
            Skateboard::GetManager().Cleanup(player);
    }
};

// =====================================================================
//  MovementHandlerScript: the movement flags are the trick button
// =====================================================================
class SkateboardMovementScript : public MovementHandlerScript
{
public:
    SkateboardMovementScript() : MovementHandlerScript("Skateboard_MovementScript") { }

    void OnPlayerMove(Player* player, MovementInfo movementInfo, uint32 /*opcode*/) override
    {
        Skateboard::GetManager().OnMove(player, movementInfo);
    }
};

// =====================================================================
//  ServerScript: the ACK that opens the speed gate
// =====================================================================
class SkateboardServerScript : public ServerScript
{
public:
    SkateboardServerScript() : ServerScript("Skateboard_ServerScript") { }

    bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
    {
        if (!session || packet.GetOpcode() != CMSG_FORCE_RUN_SPEED_CHANGE_ACK)
            return true;

        if (Player* player = session->GetPlayer())
            Skateboard::GetManager().OnAck(player);

        return true;
    }
};

// =====================================================================
//  CommandScript: `.board on|off|toggle|status|speed|push|trick <n>`
// =====================================================================
class skateboard_commandscript : public CommandScript
{
public:
    skateboard_commandscript() : CommandScript("skateboard_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable boardTable =
        {
            { "on",     HandleBoardOnCommand,     SEC_PLAYER, Console::No },
            { "off",    HandleBoardOffCommand,    SEC_PLAYER, Console::No },
            { "toggle", HandleBoardToggleCommand, SEC_PLAYER, Console::No },
            { "status", HandleBoardStatusCommand, SEC_PLAYER, Console::No },
            { "speed",  HandleBoardSpeedCommand,  SEC_PLAYER, Console::No },
            { "push",   HandleBoardPushCommand,   SEC_PLAYER, Console::No },
            { "trick",  HandleBoardTrickCommand,  SEC_PLAYER, Console::No },
        };

        static ChatCommandTable commandTable =
        {
            { "board", boardTable },
        };

        return commandTable;
    }

    static bool HandleBoardOnCommand(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        return Skateboard::GetManager().Board(player, handler);
    }

    static bool HandleBoardOffCommand(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        if (!Skateboard::GetManager().IsBoarded(player))
        {
            handler->SendSysMessage("You are not on a skateboard.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        Skateboard::GetManager().Leave(player);
        handler->SendSysMessage("You step off the board.");
        return true;
    }

    static bool HandleBoardToggleCommand(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        Skateboard::Manager& manager = Skateboard::GetManager();

        if (manager.IsBoarded(player))
        {
            manager.Leave(player);
            handler->SendSysMessage("You step off the board.");
            return true;
        }

        return manager.Board(player, handler);
    }

    // One parseable line, exactly as the add-on expects it:
    //   boarded=<0|1> tricks=<n> push=<0|1> deck=<displayId> speed=<yd/s>
    // tricks is the number of clips the add-on should offer (three when the
    // module scores them, none when it does not); push is 1 when
    // Skateboard.PushImpulse gives the push command something to do; speed is the
    // cap the module is holding, which GetUnitSpeed cannot report while the
    // player is standing still on a deck.
    static bool HandleBoardStatusCommand(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        Skateboard::Config const& cfg = Skateboard::GetConfig();
        Skateboard::Manager& manager = Skateboard::GetManager();

        uint32 const boarded = manager.IsBoarded(player) ? 1 : 0;
        uint32 const tricks = cfg.Tricks ? 3 : 0;
        uint32 const push = cfg.PushImpulse > 0.0f ? 1 : 0;

        handler->PSendSysMessage("boarded={} tricks={} push={} deck={} speed={:.1f}",
            boarded, tricks, push, cfg.DeckDisplayId, manager.Speed(player));
        return true;
    }

    static bool HandleBoardSpeedCommand(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        Skateboard::Manager& manager = Skateboard::GetManager();

        handler->PSendSysMessage("v={:.2f} rate={:.2f} pending={}",
            manager.Speed(player), player->GetSpeedRate(MOVE_RUN), uint32(manager.PendingAcks(player)));
        return true;
    }

    static bool HandleBoardPushCommand(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        Skateboard::GetManager().Push(player, handler);
        return true;
    }

    static bool HandleBoardTrickCommand(ChatHandler* handler, uint32 clip)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        Skateboard::GetManager().PlayTrick(player, clip, handler);
        return true;
    }
};

// =====================================================================
//  Registration
// =====================================================================
void AddSkateboardScripts()
{
    // The fork's module loader is called once per module; the guard keeps a
    // second call from registering a second copy of every script.
    static bool registered = false;
    if (registered)
        return;

    registered = true;

    new SkateboardWorldScript();
    new SkateboardPlayerScript();
    new SkateboardMovementScript();
    new SkateboardServerScript();
    new skateboard_commandscript();
}
