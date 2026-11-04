/*
 * mod-skateboard - the module itself.
 *
 * A self-hosted-realm toy, and a deliberate experiment: the character is not
 * moved by the server, it is *allowed* to move faster. Everything below is
 * either bookkeeping around that one number (`Deck::v`) or safety around the
 * one dangerous operation (publishing it as a run speed rate). The deck does
 * carry an idle rider, but through the client's own knockback channel - one
 * horizontal impulse along the facing per Skateboard.Drive.ImpulseMs - never
 * by relocating or spline-moving a character that the client still controls.
 *
 *   - '.board on' refuses a long list of situations first (dead, already
 *     boarded, in combat, mounted, on a vehicle, flying, in water, a
 *     battleground, an arena, mid-teleport, inside an instance, a bot session),
 *     then runs Skateboard.BoardCastMs of boarding: nothing is written until
 *     the clock runs out, so a step off the spot, a hit, a fight, a death or a
 *     teleport drops the request instead of having something to take back. The
 *     bar is the add-on's, fed by an unsolicited 'c' push. Boarding puts the
 *     deck under the rider as a summoned creature (Skateboard.Deck.Render =
 *     unit), re-seated at the feet every Deck.FollowMs: the client draws no
 *     mount model for a unit without UNIT_FLAG_MOUNT, and the flag would sit
 *     the rider, so the deck is an object of its own and the rider stands on
 *     it. Render = mount keeps the older Unit::Mount() path.
 *   - Every PlayerScript update integrates the deck: terrain gradient under the
 *     heading (Map::GetGridHeight, one sample 6 yd ahead, no vmaps, no normals
 *     exist), static plus rolling friction, an optional push impulse, airtime
 *     that ignores both.
 *   - The carry is the movement: one knockback impulse along the facing every
 *     Drive.ImpulseMs, sized so the deck averages its own v and leaves a
 *     standing gap for a jump. The run rate is held at 1.0 while the deck moves
 *     the rider, so the client's own walk is only extra.
 *   - With Drive = 0 the module publishes a *cap* instead:
 *     SetSpeed(MOVE_RUN, max(1, v / 7), true), and the cap only ever lowers.
 *     MovementHandler.cpp kicks a client that reports more speed than the
 *     server holds, so exactly one forced change is in flight at a time and the
 *     gate is opened by CMSG_FORCE_RUN_SPEED_CHANGE_ACK - or by AckTimeoutMs passing,
 *     because a lost ACK must never wedge a deck forever; then a 100 ms throttle
 *     stops a fast RTT from turning into a packet flood. Drift is re-asserted
 *     (an aura refresh calls UpdateSpeed and wipes the rate), which also keeps
 *     Render = mount honest.
 *   - MOVEMENTFLAG_FALLING is the trick button. Take-off queues a clip for the
 *     speed band and plays the band's emote plus one stock visual kit; a
 *     landing before the clip ends crashes the deck (damage, optional root),
 *     a landing at or after it is clean (LandingBonus, EMOTE_ONESHOT_LAND).
 *   - The deck never outlives its session: death, logout, a map change, water,
 *     flying, a battleground or an arena drops it, and so does a hit from
 *     another unit (config: Skateboard.OffOnHit). A fight that merely starts
 *     does not - attacking something while riding is allowed, and the deck only
 *     goes when something hits back. Every drop clears the mount field.
 *
 * Released under GNU GPL v2; redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Chat.h"
#include "ChatCommand.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "GridTerrainData.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "MotionMaster.h"
#include "MoveSplineInit.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "TemporarySummon.h"
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

    // How long after the carry's arc ends a FALLING edge is still read as that arc: a client frame
    // plus the round trip, which on a self-hosted realm is single-digit milliseconds.
    constexpr uint32 CarryArcSlackMs = 40;

    // Display ids behind the three deck names. 26609 is the stock "Log Ride"
    // creature display and needs no client patch; the two wall ids arrive with
    // the step-4 DBC patch and fall back to the log until then.
    constexpr uint32 DeckLog       = 26609;
    constexpr uint32 DeckPlank     = 90001;
    constexpr uint32 DeckCoffinLid = 90002;

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

    // One entry of the trick list by the band its clip belongs to: entry 1 is the slow clip, 2 the
    // medium one, 3 and beyond the long one. A list shorter than three reuses its last entry.
    Skateboard::Trick const* TrickForBand(Skateboard::Config const& cfg, uint32 clip)
    {
        if (cfg.TrickEntries.empty())
            return nullptr;

        size_t index = clip == Skateboard::CLIP_SHORT_MS ? 0 : (clip == Skateboard::CLIP_MEDIUM_MS ? 1 : 2);
        return &cfg.TrickEntries[std::min(index, cfg.TrickEntries.size() - 1)];
    }

    // The clip a list position queues: 1 = short, 2 = medium, everything after = long.
    uint32 ClipForIndex(size_t index)
    {
        if (index == 0)
            return Skateboard::CLIP_SHORT_MS;
        if (index == 1)
            return Skateboard::CLIP_MEDIUM_MS;
        return Skateboard::CLIP_LONG_MS;
    }

    // 'name:emote[:visualkit], name:emote, ...' - the menu's list. A name is lower-cased and cut
    // to [a-z0-9_-]; an entry that has no usable emote is dropped, and a list that yields nothing
    // falls back to the shipped three so tricks never disappear on a typo.
    void ParseTricks(std::string const& raw, uint32 defaultKit, std::vector<Skateboard::Trick>& out)
    {
        out.clear();

        size_t start = 0;
        while (start <= raw.size() && out.size() < 8)
        {
            size_t const comma = raw.find(',', start);
            std::string const entry = raw.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            start = comma == std::string::npos ? raw.size() + 1 : comma + 1;

            std::vector<std::string> parts;
            size_t pos = 0;
            while (pos <= entry.size() && parts.size() < 3)
            {
                size_t const colon = entry.find(':', pos);
                parts.push_back(entry.substr(pos, colon == std::string::npos ? std::string::npos : colon - pos));
                pos = colon == std::string::npos ? entry.size() + 1 : colon + 1;
            }

            if (parts.size() < 2)
                continue;

            std::string name;
            for (char ch : parts[0])
            {
                if (name.size() >= 12)
                    break;
                unsigned char const u = static_cast<unsigned char>(ch);
                if (u == ' ')
                    continue;
                name += char(std::tolower(u));
            }

            if (name.empty())
                continue;

            unsigned long emote = std::strtoul(parts[1].c_str(), nullptr, 10);
            if (emote == 0 || emote > 1000)
                continue;

            uint32 kit = defaultKit;
            if (parts.size() >= 3 && !parts[2].empty())
                kit = uint32(std::min<unsigned long>(std::strtoul(parts[2].c_str(), nullptr, 10), 100000));

            out.push_back(Skateboard::Trick{ name, uint32(emote), kit });
        }

        if (out.empty())
        {
            out.push_back(Skateboard::Trick{ "kickflip", EMOTE_ONESHOT_JUMPSTART, defaultKit });
            out.push_back(Skateboard::Trick{ "ollie", EMOTE_ONESHOT_JUMPLANDRUN, defaultKit });
            out.push_back(Skateboard::Trick{ "grind", EMOTE_ONESHOT_STOMP, defaultKit });
        }
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

    // The summoned deck is an object of its own: it has to leave with the record, or the realm
    // keeps a plank standing wherever the rider got off. It lives on the map it was summoned on,
    // which is the *old* map once the rider has teleported, hearthstoned or queued for a
    // battleground, so it is looked up through Deck::mapId rather than the rider's current map. A
    // grid that has already unloaded took the temp summon with it, which is why a miss is not an
    // error.
    void DespawnDeck(Deck& deck)
    {
        if (deck.deckUnit.IsEmpty())
            return;

        if (Map* map = sMapMgr->FindMap(deck.mapId, 0))
            if (Creature* unit = map->GetCreature(deck.deckUnit))
                unit->DespawnOrUnsummon();

        deck.deckUnit.Clear();
    }

    // Clears the mount display field and the flags around it. The unit render never set
    // UNIT_FLAG_MOUNT, so Dismount() (which returns early when IsMounted() is
    // false) is only the half of the job the mount render needs; the field
    // itself is always written back to 0.
    void ClearVisual(Player* player, Deck& deck)
    {
        DespawnDeck(deck);

        // Always cleared, not just in mount mode: a '.reload config' that switches Render from
        // mount to unit must not leave a mount display on a character that is now on foot.
        player->SetUInt32Value(UNIT_FIELD_MOUNTDISPLAYID, 0);
        player->ForceValuesUpdateAtIndex(UNIT_FIELD_MOUNTDISPLAYID);

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
        deck.driveNextMs = 0;
        deck.impulseArcMs = 0;
        deck.lastImpulseMs = 0;
        deck.pushHeld = false;
        deck.pushNextMs = 0;
        deck.pushHeldSinceMs = 0;
        deck.followNextMs = 0;
    }

    // The ground under a rider, or the rider's own feet when no grid is loaded.
    float GroundUnder(Player* player)
    {
        float const z = player->GetPositionZ();
        float const height = player->GetMap()->GetHeight(player->GetPositionX(), player->GetPositionY(), z);

        return height > INVALID_HEIGHT + 1.0f ? height : z;
    }

    // The summoned deck. A unit's own model comes from its creature_template, and
    // Creature::SetDisplayId writes the deck over it, so the carrier has to be a prop: no AI, no C++
    // script, nothing an idle object would otherwise be doing. A row with either is not harmless -
    // 16386 (Necropolis Relay) looks inert in the display columns but carries ScriptName
    // npc_necropolis_relay, whose AI constructor calls setActive(true), so every rider's deck would
    // be an always-updated object that keeps its grid loaded.
    void SummonDeck(Player* player, Deck& deck)
    {
        Skateboard::Config const& cfg = Skateboard::GetConfig();
        CreatureTemplate const* carrier = sObjectMgr->GetCreatureTemplate(cfg.Carrier);

        if (!carrier || !carrier->AIName.empty() || carrier->ScriptID != 0)
        {
            LOG_ERROR("module", "Skateboard: Skateboard.Deck.Carrier {} is not an inert creature_template row "
                "(missing, or it has an AIName or a ScriptName), so the deck cannot be summoned.",
                cfg.Carrier);
            deck.deckUnit.Clear();
            return;
        }

        float const x = player->GetPositionX();
        float const y = player->GetPositionY();

        TempSummon* unit = player->SummonCreature(cfg.Carrier, x, y, GroundUnder(player),
            player->GetOrientation(), TEMPSUMMON_MANUAL_DESPAWN);

        if (!unit)
        {
            LOG_WARN("module", "Skateboard: could not summon the deck for {}.", player->GetName());
            deck.deckUnit.Clear();
            return;
        }

        unit->SetDisplayId(cfg.DeckDisplayId);
        unit->SetReactState(REACT_PASSIVE);
        unit->SetUnitFlag(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE);
        unit->SetImmuneToPC(true);
        unit->SetImmuneToNPC(true);
        unit->GetMotionMaster()->Clear();

        deck.deckUnit = unit->GetGUID();
        deck.followNextMs = NowMs() + cfg.FollowMs;
    }

    // The mount render: the deck as UNIT_FIELD_MOUNTDISPLAYID, written through Player::Mount so the
    // client is sent the mount flag with it.
    void WriteMount(Player* player, Deck& deck)
    {
        Skateboard::Config const& cfg = Skateboard::GetConfig();

        player->Mount(cfg.DeckDisplayId);
        player->ForceValuesUpdateAtIndex(UNIT_FIELD_MOUNTDISPLAYID);

        deck.displayId = cfg.DeckDisplayId;
    }

    // Whether the live visual still describes the config - '.reload config' can switch Deck.Render
    // or land a Deck name on a different display id, and neither retires a deck that is already
    // drawn. Stateless on purpose: the record stores no copy of what it built.
    bool VisualStale(Player* player, Deck const& deck, Skateboard::Config const& cfg)
    {
        uint32 const field = player->GetUInt32Value(UNIT_FIELD_MOUNTDISPLAYID);

        if (!cfg.RenderUnit)
            return !player->IsMounted() || field != cfg.DeckDisplayId;

        // Only a mount the module drew counts: a real mount the rider climbed onto mid-ride is
        // theirs, and the deck is drawn under them anyway.
        if (field == cfg.DeckDisplayId && player->IsMounted())
            return true;

        if (deck.deckUnit.IsEmpty())
            return false;   // Reseat summons it

        Creature* unit = player->GetMap()->GetCreature(deck.deckUnit);

        return unit && unit->GetDisplayId() != cfg.DeckDisplayId;
    }

    // The deck rides under the rider: every FollowMs the unit is put back at the feet it is
    // under. A spline rather than a relocate: NearTeleportTo sends an uninterpolated teleport, so at
    // 25 object updates a second the client snapped the deck from point to point and it read as the
    // deck chasing the rider. A segment whose velocity covers FollowMs is interpolated by the
    // client instead, so the two move together. The window is armed before the summon, so a carrier
    // that cannot be summoned retries once per FollowMs rather than once per player update.
    void Reseat(Player* player, Deck& deck, uint32 now)
    {
        Skateboard::Config const& cfg = Skateboard::GetConfig();

        if (!Reached(now, deck.followNextMs))
            return;

        deck.followNextMs = now + cfg.FollowMs;

        if (deck.deckUnit.IsEmpty())
        {
            SummonDeck(player, deck);
            return;
        }

        Creature* unit = player->GetMap()->GetCreature(deck.deckUnit);

        // A deck that died or was unloaded comes back under the rider.
        if (!unit)
        {
            deck.deckUnit.Clear();
            SummonDeck(player, deck);
            return;
        }

        float const x = player->GetPositionX();
        float const y = player->GetPositionY();
        float const z = GroundUnder(player);

        // Standing still: nothing to interpolate, and a zero-length spline is a teleport anyway.
        if (unit->GetExactDist2dSq(x, y) < 0.0025f)
        {
            unit->NearTeleportTo(x, y, z, player->GetOrientation());
            return;
        }

        Movement::MoveSplineInit init(unit);
        init.MoveTo(x, y, z, false);
        init.SetVelocity(std::max(unit->GetExactDist2d(x, y) / (float(cfg.FollowMs) / 1000.0f), 1.0f));
        init.SetFacing(player->GetOrientation());
        init.Launch();
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

    // One carry impulse. A player's movement is the client's (MovementHandler.cpp): the server
    // cannot drive a character that is still in control - a relocation is undone by the next
    // movement packet and a server spline is only obeyed by a unit the client has stopped
    // steering (see VerifyMovementInfo, which drops a client's own movement while a spline is
    // unfinalized). The client's own knockback channel is the one exception (Unit::KnockbackFrom
    // -> SMSG_MOVE_KNOCK_BACK), so a rider is pushed along the facing with it, one impulse at a
    // time.
    //
    // KnockbackFrom(x, y, speedXY, speedZ) takes the *source* of the knockback, not a direction:
    // Position::GetSinCos normalizes (thisPosition - source). Passing (cos o, sin o) would push the
    // rider away from a fixed point near the world origin instead of along the facing, so the
    // source is placed one yard behind the rider.
    //
    // The vertical half is what makes it carry at all: the client's knockback is an arc that lasts
    // 2 * zspeed / g, and a zspeed of 0 is a horizontal nudge it drops on the spot. The arc is
    // sized to Drive.ArcMs, or to Drive.ImpulseMs - Drive.GroundMs when that key is 0, and the
    // horizontal speed covers the whole cycle inside that arc, so the deck averages exactly its own
    // v and the rider stands on the ground for Drive.GroundMs between impulses - the only window a
    // jump can start in.
    //
    // The arc is also what OnMove() has to *not* read as a jump: the client reports FALLING for the
    // knockback, with a positive zspeed on the way up, so the record carries the window it opened.
    void CarryImpulse(Player* player, Deck& deck, uint32 now)
    {
        Skateboard::Config const& cfg = Skateboard::GetConfig();

        uint32 const cycleMs = cfg.DriveImpulseMs;
        // At least half the cycle, so a GroundMs an operator set too high cannot turn one impulse
        // into an unbounded jump: the multiplier is capped at 2. Drive.ArcMs overrides the derived
        // length but never exceeds the cycle, or the arc would outlive the impulse that replaces it.
        uint32 const derivedMs = std::max(cycleMs / 2, cycleMs > cfg.DriveGroundMs ? cycleMs - cfg.DriveGroundMs : 0u);
        uint32 const arcMs = cfg.DriveArcMs ? std::min(cfg.DriveArcMs, cycleMs) : derivedMs;
        float const arc = float(arcMs) / 1000.0f;

        float const zspeed = std::max(0.5f, 0.5f * Skateboard::CLIENT_GRAVITY * arc);
        float const speed = deck.v * float(cycleMs) / float(arcMs);

        float const o = player->GetOrientation();
        player->KnockbackFrom(player->GetPositionX() - std::cos(o), player->GetPositionY() - std::sin(o),
            speed, zspeed);

        deck.driveNextMs = now + cycleMs;
        deck.lastImpulseMs = now;
        deck.impulseArcMs = arcMs;
    }

    // The spline carrier (Skateboard.Drive.Mode = 1). The server moves the rider itself, along a
    // polyline it samples, so the path follows the hill - the knockback it replaces is horizontal by
    // construction, which is why a deck under the kick carrier runs straight through a downhill
    // instead of down it. While a player's spline is unfinalized the core drops the client's own
    // movement (VerifyMovementInfo), so the rider has no steering *during* a segment: each new one
    // is aimed along the freshest heading the client managed to report, and Drive.SplineMs is how
    // often that chance comes. The client still animates the spline, so the ride itself is smooth.
    //
    // Nothing here is authoritative beyond the path: the height comes from the map, one sample per
    // sub-segment, and an unloaded grid keeps the previous height rather than inventing a floor.
    void CarrySpline(Player* player, Deck& deck, uint32 now)
    {
        Skateboard::Config const& cfg = Skateboard::GetConfig();

        uint32 const segmentMs = std::max<uint32>(cfg.DriveSplineMs, 50);
        float const seconds = float(segmentMs) / 1000.0f;
        float const distance = std::max(1.0f, deck.v * seconds);
        // A client-controlled player's orientation is the client's: HandleMoverRelocation writes the
        // packet's position *and* facing, so this is the heading the rider last steered to.
        float const o = player->GetOrientation();
        float const x = player->GetPositionX();
        float const y = player->GetPositionY();
        Map* map = player->GetMap();

        Movement::MoveSplineInit init(player);
        Movement::PointsArray& path = init.Path();

        constexpr uint32 Samples = 4;
        float lastZ = player->GetPositionZ();

        // The start vertex is ours to provide: MoveTo() adds it, a hand-built path does not, and a
        // path whose first point is already the destination has nothing to walk (the taxi path does
        // the same thing for the same reason).
        path.push_back(G3D::Vector3(x, y, lastZ));

        for (uint32 i = 1; i <= Samples; ++i)
        {
            float const t = float(i) / float(Samples);
            float const px = x + std::cos(o) * distance * t;
            float const py = y + std::sin(o) * distance * t;

            float height = map->GetHeight(px, py, lastZ);
            if (height <= INVALID_HEIGHT + 1.0f)
                height = lastZ;

            lastZ = height;
            path.push_back(G3D::Vector3(px, py, height));
        }

        init.SetVelocity(std::max(deck.v, 1.0f));
        init.SetFacing(o);
        init.Launch();

        deck.driveNextMs = now + segmentMs;
        deck.lastImpulseMs = 0;
        deck.impulseArcMs = 0;
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

        cfg.Enable = sConfigMgr->GetOption<bool>("Skateboard.Enable", true);
        cfg.DeckName = sConfigMgr->GetOption<std::string>("Skateboard.Deck", "plank");
        cfg.MaxSpeed = std::clamp(sConfigMgr->GetOption<float>("Skateboard.MaxSpeed", 21.0f), 1.0f, 40.0f);
        cfg.Cruise = std::clamp(sConfigMgr->GetOption<float>("Skateboard.Cruise", 4.0f), 0.0f, 20.0f);

        std::string render = sConfigMgr->GetOption<std::string>("Skateboard.Deck.Render", "unit");
        std::transform(render.begin(), render.end(), render.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (render != "unit" && render != "mount")
        {
            LOG_WARN("module", "Skateboard: unknown Skateboard.Deck.Render '{}', expected unit or mount. Using unit.", render);
            render = "unit";
        }
        cfg.RenderUnit = render == "unit";
        cfg.Carrier = sConfigMgr->GetOption<uint32>("Skateboard.Deck.Carrier", 29443);
        cfg.FollowMs = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Skateboard.Deck.FollowMs", 40), 20, 1000);
        cfg.PushImpulse = std::clamp(sConfigMgr->GetOption<float>("Skateboard.PushImpulse", 4.0f), 0.0f, 20.0f);
        cfg.FrictionStatic = std::clamp(sConfigMgr->GetOption<float>("Skateboard.Friction.Static", 1.5f), 0.0f, 20.0f);
        cfg.FrictionRolling = std::clamp(sConfigMgr->GetOption<float>("Skateboard.Friction.Rolling", 0.004f), 0.0f, 1.0f);
        cfg.SlopeGain = std::clamp(sConfigMgr->GetOption<float>("Skateboard.SlopeGain", 30.0f), 0.0f, 90.0f);
        cfg.Drive = sConfigMgr->GetOption<bool>("Skateboard.Drive", true);
        cfg.DriveMode = std::min<uint32>(sConfigMgr->GetOption<uint32>("Skateboard.Drive.Mode", 0), 1);
        cfg.DriveImpulseMs = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Skateboard.Drive.ImpulseMs", 350), 100, 2000);
        cfg.DriveArcMs = std::min<uint32>(sConfigMgr->GetOption<uint32>("Skateboard.Drive.ArcMs", 0), cfg.DriveImpulseMs);
        cfg.DriveSplineMs = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Skateboard.Drive.SplineMs", 150), 50, 1000);
        cfg.DriveGroundMs = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Skateboard.Drive.GroundMs", 100), 0, 1000);
        cfg.DriveMinSpeed = std::clamp(sConfigMgr->GetOption<float>("Skateboard.Drive.MinSpeed", 3.0f), 0.0f, 40.0f);
        cfg.Brake = std::clamp(sConfigMgr->GetOption<float>("Skateboard.Brake", 6.0f), 0.0f, 50.0f);
        cfg.LandingBonus = std::clamp(sConfigMgr->GetOption<float>("Skateboard.LandingBonus", 2.0f), 0.0f, 20.0f);
        cfg.Tricks = sConfigMgr->GetOption<bool>("Skateboard.Tricks", true);
        cfg.PushHold = sConfigMgr->GetOption<uint32>("Skateboard.Push.Mode", 0) != 0;
        cfg.PushIntervalMs = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Skateboard.Push.IntervalMs", 500), 100, 2000);
        cfg.PushMaxHoldMs = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Skateboard.Push.MaxHoldMs", 2000), 100, 5000);
        cfg.TrickVisualKit = std::min<uint32>(sConfigMgr->GetOption<uint32>("Skateboard.Trick.VisualKit", 5036), 100000);
        cfg.CrashEnable = sConfigMgr->GetOption<bool>("Skateboard.Crash.Enable", true);
        cfg.CrashDamagePct = std::min<uint32>(sConfigMgr->GetOption<uint32>("Skateboard.Crash.DamagePct", 8), 99);
        cfg.CrashStaggerMs = std::min<uint32>(sConfigMgr->GetOption<uint32>("Skateboard.Crash.StaggerMs", 1500), 10000);
        cfg.HardcoreSafe = sConfigMgr->GetOption<bool>("Skateboard.HardcoreSafe", true);
        cfg.OffOnHit = sConfigMgr->GetOption<bool>("Skateboard.OffOnHit", true);
        cfg.BoardCastMs = std::min<uint32>(sConfigMgr->GetOption<uint32>("Skateboard.BoardCastMs", 2000), 10000);
        cfg.AckTimeoutMs = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Skateboard.AckTimeoutMs", 500), 50, 5000);
        cfg.Debug = sConfigMgr->GetOption<bool>("Skateboard.Debug", false);
        cfg.TrickList = sConfigMgr->GetOption<std::string>("Skateboard.Trick.List", "kickflip:399:5036, ollie:394:5036, grind:388:5036");
        ParseTricks(cfg.TrickList, cfg.TrickVisualKit, cfg.TrickEntries);

        // The gate the carry opens at can never sit above the floor a deck rolls at, or the config
        // asks for a boarded deck that never moves: Cruise is applied first, then the gate is
        // capped by it.
        if (cfg.Cruise > 0.0f)
            cfg.DriveMinSpeed = std::min(cfg.DriveMinSpeed, cfg.Cruise);

        // The spline carrier *is* the movement, so it cannot be off: a deck with Drive = 0 only
        // publishes a run-speed cap, which needs the forward key and leaves the spline nothing to do.
        if (cfg.DriveMode == 1 && !cfg.Drive)
        {
            LOG_WARN("module", "Skateboard: Skateboard.Drive.Mode = 1 is the carrier itself, so Skateboard.Drive "
                "is forced back on.");
            cfg.Drive = true;
        }

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
        else if (deck == "plank")
            displayId = DeckPlank;
        else if (deck == "engravedwall")   // the old name for 90001, kept for confs written before it
            displayId = DeckPlank;
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
                LOG_WARN("module", "Skateboard: unknown deck '{}', expected log, plank, coffinlid or a display id. Using {}.",
                    cfg.DeckName, DeckLog);
                displayId = DeckLog;
            }
        }

        if (displayId != DeckLog && !sCreatureDisplayInfoStore.LookupEntry(displayId))
        {
            LOG_WARN("module", "Skateboard: display id {} for deck '{}' is not in CreatureDisplayInfo.dbc, falling back to {}. "
                "Run scripts/skateboard/patch_deck_dbc.py on the realm host to install the row.",
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

        // A board in a fight is a free 21 yd/s disengage, so boarding is refused while fighting -
        // always, whatever Skateboard.OffOnHit does to a deck already under the player.
        if (player->IsInCombat())
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

    // An unsolicited add-on message, on the same channel the core's own replies use
    // (ChatHandler::BuildChatPacket with LANG_ADDON, which the client delivers as a CHAT_MSG_ADDON
    // split at the tab). Opcode 'c' is this add-on's own: begin=<ms>, done, cancel=<reason>. It
    // exists so the client's bar starts and stops when the server's cast does, rather than on the
    // next five-second status poll.
    void PushCastState(Player* player, std::string const& payload)
    {
        WorldSession* session = player->GetSession();
        if (!session)
            return;

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player,
            "AzerothCore\tc" + payload);
        session->SendPacket(&data);
    }

    // The boarding itself: the deck written and the gate armed. Called by '.board on' when
    // Skateboard.BoardCastMs is 0, and by the cast when it completes - hence `now` rather than a
    // clock read, so both paths arm the gate at the same instant the caller decided.
    void BoardNow(Player* player, uint32 now)
    {
        Skateboard::Config const& cfg = Skateboard::GetConfig();

        Deck& deck = Decks()[player->GetGUID()];
        deck = Deck();
        deck.displayId = cfg.DeckDisplayId;
        deck.mapId = player->GetMapId();
        deck.gateDeadlineMs = now + PublishThrottleMs;
        deck.debugNextMs = now;

        if (cfg.RenderUnit)
        {
            SummonDeck(player, deck);
        }
        else
        {
            WriteMount(player, deck);
        }

        // A rate of 1.0 is the character's own run speed: SetSpeed() returns without a packet when
        // the server already holds it, so the gate only arms when a change is actually sent - and
        // only downwards, so stepping on while snared does not clear the snare.
        if (player->GetSpeedRate(MOVE_RUN) > 1.0f + 0.01f)
            PublishRate(player, deck, 1.0f, now);
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

        if (cfg.BoardCastMs > 0)
        {
            // Nothing is written yet: the record carries only the cast, so a step, a hit or a
            // fight drops it without anything to take back. Update() boards when the clock runs out.
            Deck& deck = Decks()[player->GetGUID()];
            deck = Deck();
            deck.mapId = player->GetMapId();
            deck.castDeadlineMs = now + cfg.BoardCastMs;
            deck.castX = player->GetPositionX();
            deck.castY = player->GetPositionY();
            PushCastState(player, "begin=" + std::to_string(cfg.BoardCastMs));
            return true;
        }

        BoardNow(player, now);
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

    bool Manager::IsCasting(Player const* player) const
    {
        Deck* deck = FindDeck(player);
        return deck && deck->castDeadlineMs != 0;
    }

    void Manager::CancelCast(Player* player, std::string const& reason)
    {
        if (!player)
            return;

        auto it = Decks().find(player->GetGUID());
        if (it == Decks().end() || !it->second.castDeadlineMs)
            return;

        // Nothing was written for a cast, so there is nothing to take back - and the record cannot
        // be kept to hold the reason, because Update() at displayId 0 is the crash-stagger path.
        Decks().erase(it);
        PushCastState(player, "cancel=" + reason);
    }

    void Manager::OnHit(Player* player)
    {
        if (!player)
            return;

        // A hit stops a cast whatever the config says: the request was made while out of combat,
        // and the player is no longer in the position it was checked in.
        if (IsCasting(player))
        {
            CancelCast(player, "hit");
            return;
        }

        if (!GetConfig().OffOnHit)
            return;

        auto it = Decks().find(player->GetGUID());
        if (it == Decks().end() || it->second.displayId == 0)
            return;

        Cleanup(player);

        if (WorldSession* session = player->GetSession())
            ChatHandler(session).SendSysMessage("The hit knocks you off the board.");
    }

    void Manager::OnEnterCombat(Player* player)
    {
        // Only the cast: a deck already under the player survives the start of a fight, because
        // walking into one and being hit by it are not the same thing (Skateboard.OffOnHit).
        if (IsCasting(player))
            CancelCast(player, "combat");
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

        // A kick is felt now, not at the next slot in the carry's rhythm: the impulse that
        // carries the new speed goes out on this tick.
        deck->driveNextMs = 0;
    }

    void Manager::PushDown(Player* player, ChatHandler* handler)
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

        // Press mode is the older contract: a down edge is one kick and an up edge is nothing, so
        // an add-on can send both without knowing which mode the realm runs.
        if (!cfg.PushHold)
        {
            deck->v = std::min(deck->v + cfg.PushImpulse, cfg.MaxSpeed);
            deck->driveNextMs = 0;
            return;
        }

        // The down edge is a kick in its own right: a rider should feel the press, not the tick
        // that follows it. The rest of the hold follows at Push.IntervalMs.
        if (!deck->pushHeld)
        {
            uint32 const now = NowMs();
            deck->pushHeld = true;
            deck->pushHeldSinceMs = now;
            deck->pushNextMs = now + cfg.PushIntervalMs;
            deck->v = std::min(deck->v + cfg.PushImpulse, cfg.MaxSpeed);
            deck->driveNextMs = 0;
        }
    }

    void Manager::PushUp(Player* player)
    {
        if (Deck* deck = FindDeck(player))
        {
            deck->pushHeld = false;
            deck->pushNextMs = 0;
        }
    }

    void Manager::PlayTrick(Player* player, std::string const& which, ChatHandler* handler)
    {
        Config const& cfg = GetConfig();

        // A name or a 1-based position. Names are lower-cased here, so '.board trick Kickflip'
        // works without the add-on knowing the case the conf was written in.
        std::string wanted;
        for (char ch : which)
        {
            unsigned char const u = static_cast<unsigned char>(ch);
            if (u != ' ')
                wanted += char(std::tolower(u));
        }

        size_t index = cfg.TrickEntries.size();
        for (size_t i = 0; i < cfg.TrickEntries.size(); ++i)
        {
            if (cfg.TrickEntries[i].Name == wanted)
            {
                index = i;
                break;
            }
        }

        if (index == cfg.TrickEntries.size())
        {
            unsigned long const number = std::strtoul(wanted.c_str(), nullptr, 10);
            if (number >= 1 && number <= cfg.TrickEntries.size())
                index = size_t(number - 1);
        }

        if (index == cfg.TrickEntries.size())
        {
            if (handler)
            {
                handler->SendSysMessage("No such trick. '.board tricks' lists them.");
                handler->SetSentErrorMessage(true);
            }
            return;
        }

        if (!cfg.Tricks)
        {
            if (handler)
            {
                handler->SendSysMessage("Skateboard.Tricks is off: no trick list.");
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

        Trick const& trick = cfg.TrickEntries[index];
        uint32 const now = NowMs();

        deck->activeClip = ClipForIndex(index);
        deck->trickEndMs = now + deck->activeClip;

        player->HandleEmoteCommand(trick.Emote);
        if (trick.VisualKit)
            player->SendPlaySpellVisual(trick.VisualKit);
    }

    void Manager::ListTricks(ChatHandler* handler) const
    {
        Config const& cfg = GetConfig();

        if (!cfg.Tricks)
        {
            handler->SendSysMessage("Skateboard.Tricks is off: no trick list.");
            handler->SetSentErrorMessage(true);
            return;
        }

        handler->PSendSysMessage("tricks={}", uint32(cfg.TrickEntries.size()));
        for (size_t i = 0; i < cfg.TrickEntries.size(); ++i)
            handler->PSendSysMessage("{} {} {}", uint32(i) + 1, cfg.TrickEntries[i].Name, cfg.TrickEntries[i].Emote);
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

        // A boarding cast comes first: its record carries displayId 0 exactly like a crash
        // stagger, and the deck is not written until the cast completes.
        if (deck.castDeadlineMs)
        {
            Config const& castCfg = GetConfig();

            if (!castCfg.Enable || !player->IsAlive() || !player->IsInWorld() || player->GetMapId() != deck.mapId)
            {
                CancelCast(player, castCfg.Enable ? "interrupted" : "disabled");
                return;
            }

            if (Reached(now, deck.castDeadlineMs))
            {
                deck.castDeadlineMs = 0;
                BoardNow(player, now);
                PushCastState(player, "done");
                // The instant path says this from the command handler; a cast that finishes has no
                // handler left, and the deck appearing is not the only thing worth saying.
                if (WorldSession* session = player->GetSession())
                    ChatHandler(session).PSendSysMessage("You step onto the board. Display {}.",
                        Skateboard::GetConfig().DeckDisplayId);
            }
            return;
        }

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

        // Combat is not in this list: a fight that starts while the player is riding does not take
        // the deck. Being hit does, in OnHit, which is what Skateboard.OffOnHit means.
        if (!player->IsAlive() || !player->IsInWorld() || player->GetMapId() != deck.mapId ||
            player->IsInWater() || player->IsFlying() || player->InBattleground() || player->InArena() ||
            player->GetVehicle())
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

        // The deck under the rider: re-seated every FollowMs, first thing, so a freshly boarded
        // deck exists before anything can move it. A '.reload config' that switched the render or
        // landed on a different deck id left the live visual describing the old config, so it is
        // rebuilt here - the ride continues, only the drawing changes.
        // A '.reload config' unresolves the deck name. A live rider keeps the deck it boarded, so
        // the name is re-resolved here instead of falling back to the stock log until the next
        // board.
        Skateboard::ResolveDeck();

        if (VisualStale(player, deck, cfg))
        {
            DespawnDeck(deck);

            // Only the display the module itself drew comes off. In unit mode a real mount the rider
            // climbed onto is left alone; in mount mode the field is the deck, so it is always taken
            // back before the new shape is written.
            if (!cfg.RenderUnit || player->GetUInt32Value(UNIT_FIELD_MOUNTDISPLAYID) == cfg.DeckDisplayId)
            {
                player->SetUInt32Value(UNIT_FIELD_MOUNTDISPLAYID, 0);
                player->ForceValuesUpdateAtIndex(UNIT_FIELD_MOUNTDISPLAYID);

                if (player->IsMounted())
                    player->Dismount();
            }

            deck.followNextMs = 0;
            deck.displayId = cfg.DeckDisplayId;

            if (cfg.RenderUnit)
                SummonDeck(player, deck);
            else
                WriteMount(player, deck);
        }

        if (cfg.RenderUnit)
            Reseat(player, deck, now);

        float const dt = std::min(float(diff) / 1000.0f, 0.25f);
        float slope = 0.0f;
        bool const braking = player->HasUnitMovementFlag(MOVEMENTFLAG_BACKWARD);

        if (!deck.airborne)
        {
            float const x = player->GetPositionX();
            float const y = player->GetPositionY();
            float const o = player->GetOrientation();

            float const h0 = player->GetMap()->GetGridHeight(x, y);
            float const h2 = player->GetMap()->GetGridHeight(
                x + std::cos(o) * Skateboard::SLOPE_LOOKAHEAD, y + std::sin(o) * Skateboard::SLOPE_LOOKAHEAD);

            // INVALID_HEIGHT means the grid is not loaded: no gradient, no
            // invented acceleration. The gradient is a rise over the lookahead, so a downhill
            // reads as a positive pull along the heading and an uphill as a drag.
            if (h0 > INVALID_HEIGHT + 1.0f && h2 > INVALID_HEIGHT + 1.0f)
                slope = -cfg.SlopeGain * (h2 - h0) / Skateboard::SLOPE_LOOKAHEAD;

            float acceleration = slope;
            acceleration -= cfg.FrictionStatic + cfg.FrictionRolling * deck.v * deck.v;

            // The brake key. Backward is a foot on the ground: a strong deceleration on top of
            // friction, so the deck stops in a couple of seconds from the top speed and the cap
            // follows it down. The client's own backward walk is what the rider sees while it does.
            if (braking)
                acceleration -= cfg.Brake;

            deck.v += acceleration * dt;
            deck.v = std::clamp(deck.v, 0.0f, cfg.MaxSpeed);

            // A boarded deck always rolls: friction, an uphill and a dozy rider can slow it to
            // the cruise speed but never to a standstill. Braking is the way to stop, and
            // '.board off' is the way off.
            if (!braking && deck.v < cfg.Cruise)
                deck.v = std::min(cfg.Cruise, cfg.MaxSpeed);
        }

        // .board push down: one kick per Push.IntervalMs while the hold lasts. Push.MaxHoldMs ends
        // it whatever the client does, so a lost up edge - a logout, a desync, an add-on that
        // stopped running - cannot leave a deck pumping forever.
        if (deck.pushHeld)
        {
            if (Reached(now, deck.pushHeldSinceMs + cfg.PushMaxHoldMs))
            {
                deck.pushHeld = false;
                deck.pushNextMs = 0;
            }
            else if (Reached(now, deck.pushNextMs))
            {
                deck.v = std::min(deck.v + cfg.PushImpulse, cfg.MaxSpeed);
                deck.pushNextMs = now + cfg.PushIntervalMs;
            }
        }

        // The carry is the movement: the deck is the mover while the rider steers. It runs
        // whether or not a movement key is held, so a boarded character never has to press
        // forward to travel - the client's own walk is only extra if the rider does. Backward is
        // the brake above, and a rooted or stunned rider is not carried: the client would drop
        // every impulse anyway. The gate closes at Drive.MinSpeed, which LoadConfig() caps by
        // Cruise so a config cannot board a deck that never moves.
        if (cfg.Drive && !deck.airborne && deck.v > 0.0f && deck.v >= cfg.DriveMinSpeed &&
            !player->HasUnitState(UNIT_STATE_ROOT) && !player->HasUnitState(UNIT_STATE_STUNNED) &&
            !player->IsBeingTeleported() && !player->GetTransport() && Reached(now, deck.driveNextMs))
        {
            // One segment at a time: a spline that is still running owns the rider, and replacing it
            // mid-path would snap the client to the new one. The knockback carrier has no such
            // state, so it is issued on its own cycle.
            if (cfg.DriveMode != 1)
                CarryImpulse(player, deck, now);
            else if (player->movespline->Finalized())
                CarrySpline(player, deck, now);
        }

        // The published cap is only the mover when the carry is off. With the carry on, the deck
        // is what travels: the client's own rate stays at 1.0, so a held forward key adds its
        // walk on top of the impulse instead of doubling it, and the ACK gate stays idle
        // (SetSpeed() sends nothing for an unchanged rate).
        //
        // The cap only ever lowers the rate: a rider who boards while snared keeps the snare,
        // because a rate the core already holds below the cap is not raised to it. UpdateSpeed()
        // puts the rider back when the aura ends.
        float const target = cfg.Drive ? 1.0f : std::max(1.0f, deck.v / BASE_RUN_RATE);

        if (deck.pendingAcks == 0 && Reached(now, deck.gateDeadlineMs) &&
            player->GetSpeedRate(MOVE_RUN) > target + 0.01f)
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

        if (deck->castDeadlineMs)
        {
            float const dx = movementInfo.pos.m_positionX - deck->castX;
            float const dy = movementInfo.pos.m_positionY - deck->castY;
            // Half a yard: the same position in a heartbeat packet is not a step, a jump is, and a
            // knockback is the hit's business. Nothing is written yet, so nothing is taken back.
            if (dx * dx + dy * dy > 0.25f)
                CancelCast(player, "moved");
            return;
        }

        bool const falling = movementInfo.HasMovementFlag(MOVEMENTFLAG_FALLING);
        if (falling == deck->airborne)
            return;

        Config const& cfg = GetConfig();
        uint32 const now = NowMs();

        if (falling)
        {
            // The carry pushes the rider forward with its own knockback, and the client reports that
            // glide's falling flag on the way - with a *positive* zspeed while the arc rises, so the
            // sign alone cannot tell it from a jump. The only honest discriminator is the window the
            // impulse opened: inside it, nothing is a trick.
            //
            // Reading the arc as a take-off was not cosmetic. Every arc queued a clip and paid the
            // landing bonus, so a cruising deck earned LandingBonus three times a second - faster
            // than friction and slope together could spend it - and v sat at MaxSpeed whichever way
            // the hill went. That is what made a downhill read as flat.
            if (deck->impulseArcMs && deck->lastImpulseMs &&
                !Reached(now, deck->lastImpulseMs + deck->impulseArcMs + CarryArcSlackMs))
                return;

            // A knockback the deck did not send (a spell, a mob) is still not a take-off: its jump
            // carries a negative zspeed, and a client that reports 0 falls back to "an impulse just
            // went out", which is what the carry's own rhythm looks like.
            if (movementInfo.jump.zspeed < 0.0f)
                return;

            if (movementInfo.jump.zspeed == 0.0f && deck->lastImpulseMs &&
                !Reached(now, deck->lastImpulseMs + cfg.DriveImpulseMs * 2))
                return;

            // Rising edge: this is the take-off. Remember when, and which clip
            // the speed band queued.
            deck->airborne = true;
            deck->takeoffMs = now;
            deck->activeClip = ClipForSpeed(deck->v, cfg.MaxSpeed);
            deck->trickEndMs = cfg.Tricks ? now + deck->activeClip : 0;

            if (Trick const* trick = cfg.Tricks ? TrickForBand(cfg, deck->activeClip) : nullptr)
            {
                player->HandleEmoteCommand(trick->Emote);
                if (trick->VisualKit)
                    player->SendPlaySpellVisual(trick->VisualKit);
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
        Skateboard::GetManager().OnEnterCombat(player);
    }
};

// =====================================================================
//  UnitScript: a hit takes the deck, a fall does not
// =====================================================================
class SkateboardUnitScript : public UnitScript
{
public:
    SkateboardUnitScript() : UnitScript("Skateboard_UnitScript") { }

    // Unit::DealDamage runs this for every unit in the world, so the player check is the early
    // out. Fall damage arrives with the victim as its own attacker and a wipeout's damage comes
    // from the crash itself: neither is somebody hitting the player.
    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        if (!damage || !attacker || attacker == victim || !victim->IsPlayer())
            return;

        Skateboard::GetManager().OnHit(victim->ToPlayer());
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
            { "tricks", HandleBoardTricksCommand, SEC_PLAYER, Console::No },
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

        Skateboard::Manager& manager = Skateboard::GetManager();

        if (manager.IsCasting(player))
        {
            manager.CancelCast(player, "cancelled");
            handler->SendSysMessage("The boarding was cancelled.");
            return true;
        }

        if (!manager.IsBoarded(player))
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

        if (manager.IsCasting(player))
        {
            manager.CancelCast(player, "cancelled");
            handler->SendSysMessage("The boarding was cancelled.");
            return true;
        }

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
        uint32 const tricks = cfg.Tricks ? uint32(cfg.TrickEntries.size()) : 0;
        uint32 const push = cfg.PushImpulse > 0.0f ? 1 : 0;

        // The carry's knobs ride at the end, after the fields the add-on parses: tuning
        // Skateboard.Drive.* then needs `.reload config` and one look, not a conf file read.
        handler->PSendSysMessage("boarded={} tricks={} push={} deck={} speed={:.1f} drive={} impulse={} ground={} brake={:.1f}",
            boarded, tricks, push, cfg.DeckDisplayId, manager.Speed(player),
            cfg.Drive ? 1 : 0, cfg.DriveImpulseMs, cfg.DriveGroundMs, cfg.Brake);
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

    // '.board push' is one kick; '.board push down' and '.board push up' are the edges an add-on
    // sends when Skateboard.Push.Mode = 1, so the same binding works in either mode.
    static bool HandleBoardPushCommand(ChatHandler* handler, Optional<std::string> which)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        Skateboard::Manager& manager = Skateboard::GetManager();
        std::string edge = which ? *which : "";

        if (edge == "down")
            manager.PushDown(player, handler);
        else if (edge == "up")
            manager.PushUp(player);
        else if (!edge.empty())
        {
            handler->SendSysMessage("'.board push' is one kick; 'push down' and 'push up' are the hold.");
            handler->SetSentErrorMessage(true);
            return false;
        }
        else
            manager.Push(player, handler);

        return true;
    }

    static bool HandleBoardTrickCommand(ChatHandler* handler, std::string which)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        Skateboard::GetManager().PlayTrick(player, which, handler);
        return true;
    }

    static bool HandleBoardTricksCommand(ChatHandler* handler)
    {
        Skateboard::GetManager().ListTricks(handler);
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
    new SkateboardUnitScript();
    new skateboard_commandscript();
}
