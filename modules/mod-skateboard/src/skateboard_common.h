/*
 * mod-skateboard - shared declarations.
 *
 * A trip to a private realm, not a mount: the character stays on foot, and the
 * module moves it with the client's own knockback channel (Skateboard.Drive) -
 * the one channel that moves a character the client still controls. The carry
 * runs every Drive.ImpulseMs whatever the client is doing, so a boarded
 * character needs no key at all: forward only adds the client's own walk on top,
 * and backward is the brake (Skateboard.Brake). The impulse's arc is tilted by
 * the terrain gradient along the heading, because the client ends a knockback
 * where the arc meets the ground: a level arc is cut to nothing by a rise (the
 * rider stands still while the deck reports Cruise) and outlives the cycle on a
 * drop. With Drive = 0 the module publishes a run-speed cap instead, which is
 * the older path and needs the forward key.
 *
 *   - '.board on' puts a deck *under* the character. Skateboard.Deck.Render =
 *     unit (the default) summons a creature carrying the deck display, so the
 *     rider stands on a real object; under the spline carrier the deck is
 *     launched on the rider's own path in the same tick. Render = mount is
 *     Unit::Mount(): the deck is the player's own mount display, which the
 *     client draws at the rider's position - and seats them on.
 *   - A per-tick integrator keeps a deck speed `v` (yd/s): terrain slope pulls
 *     it down or lets it run, rolling friction bleeds it off, '.board push'
 *     adds one impulse, and Skateboard.Cruise is the floor the deck never
 *     drops below while it is rolling - a boarded deck is always moving.
 *     `v` is published as a run speed *rate* (v / 7.0f, the base run speed)
 *     through SetSpeed(MOVE_RUN, rate, true).
 *   - Publishing is gated: the core kicks a client that reports a higher speed
 *     than the server holds (MovementHandler.cpp), and it counts the
 *     unacknowledged changes in a uint8. One change is in flight at a time; the
 *     gate opens when the matching CMSG_FORCE_RUN_SPEED_CHANGE_ACK arrives, or
 *     when AckTimeoutMs passes without it, so a lost ACK can never wedge a
 *     deck. Between sends there is a 100 ms throttle, and the rate is
 *     re-asserted whenever an aura refresh wipes it.
 *   - Falling (MOVEMENTFLAG_FALLING) is the trick button: take-off queues a
 *     clip by the speed band, a landing that beats the clip crashes the deck
 *     (damage, optional stagger), a landing at or after it is clean
 *     (LandingBonus, EMOTE_ONESHOT_LAND).
 *
 * Nothing is persisted: a deck lives in one in-memory record per boarded
 * player, and every hook returns early for anyone not in that table.
 *
 * Released under GNU GPL v2; redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef MOD_SKATEBOARD_COMMON_H
#define MOD_SKATEBOARD_COMMON_H

#include "Define.h"
#include "ObjectGuid.h"

#include <string>
#include <vector>

class ChatHandler;
class Player;
struct MovementInfo;

namespace Skateboard
{
    // Base run speed in yd/s (Unit.cpp: playerBaseMoveSpeed[MOVE_RUN]). A deck
    // speed is published as v / BASE_RUN_RATE, a rate SetSpeed understands, with
    // a floor of 1.0: below 7 yd/s the cap is normal run speed rather than a
    // crawl, and the rate never goes under the value a character on foot has.
    constexpr float BASE_RUN_RATE = 7.0f;

    // Yards ahead of the rider the terrain gradient is sampled over. Two points at the rider's
    // feet read a single grid cell and twitch; a longer baseline reads the hill the deck is on.
    constexpr float SLOPE_LOOKAHEAD = 6.0f;

    // Gravity in the 3.3.5a client, yd/s^2. The carry sizes the vertical half of its knockback
    // from this so one impulse lasts exactly Skateboard.Drive.ImpulseMs.
    constexpr float CLIENT_GRAVITY = 19.2911f;

    // Trick clip lengths, by speed band: slow, medium, fast. A landing has to
    // last at least as long as the clip the take-off queued.
    constexpr uint32 CLIP_SHORT_MS  = 500;
    constexpr uint32 CLIP_MEDIUM_MS = 900;
    constexpr uint32 CLIP_LONG_MS   = 1400;

    // One entry of Skateboard.Trick.List: the name the add-on's menu shows, the emote it plays and
    // the stock SpellVisualKit sent with it (0 = no flair). The list's order is the speed band:
    // entry 1 is the slow clip, 2 the medium one, 3 and beyond the long one, and a manual
    // '.board trick <name>' plays any of them by hand.
    struct Trick
    {
        std::string Name;
        uint32 Emote = 0;
        uint32 VisualKit = 0;
    };

    // Cached config. Written only by LoadConfig() (world thread, at startup and
    // on '.reload config'); read from any thread after that - the same
    // one-writer/startup assumption the core's own config accessors make.
    struct Config
    {
        bool Enable                = false;    // Skateboard.Enable (shipped off: development paused)
        std::string DeckName       = "plank";          // Skateboard.Deck, as configured
        uint32 DeckDisplayId       = 90001;    // Skateboard.Deck, resolved
        bool RenderUnit            = true;     // Skateboard.Deck.Render (unit, not mount)
        uint32 Carrier             = 29443;    // Skateboard.Deck.Carrier
        uint32 FollowMs            = 40;       // Skateboard.Deck.FollowMs
        float MaxSpeed             = 21.0f;    // Skateboard.MaxSpeed
        float Cruise               = 4.0f;     // Skateboard.Cruise
        float PushImpulse          = 4.0f;     // Skateboard.PushImpulse
        float FrictionStatic       = 1.5f;     // Skateboard.Friction.Static
        float FrictionRolling      = 0.004f;   // Skateboard.Friction.Rolling
        float SlopeGain            = 30.0f;    // Skateboard.SlopeGain
        bool PushHold              = false;    // Skateboard.Push.Mode (1 = hold to pump)
        uint32 PushIntervalMs      = 500;      // Skateboard.Push.IntervalMs
        uint32 PushMaxHoldMs       = 2000;     // Skateboard.Push.MaxHoldMs
        bool Drive                 = true;     // Skateboard.Drive
        uint32 DriveMode           = 1;        // Skateboard.Drive.Mode (0 = knockback kicks, 1 = server spline)
        uint32 DriveImpulseMs      = 350;      // Skateboard.Drive.ImpulseMs
        uint32 DriveArcMs          = 0;        // Skateboard.Drive.ArcMs (0 = derive it from the cycle)
        uint32 DriveSplineMs       = 400;      // Skateboard.Drive.SplineMs (one spline segment)
        float DriveMinSpeed        = 3.0f;     // Skateboard.Drive.MinSpeed
        float Brake                = 6.0f;     // Skateboard.Brake
        uint32 DriveGroundMs       = 100;      // Skateboard.Drive.GroundMs (the gap between impulses)
        std::string TrickList;                 // Skateboard.Trick.List, as configured
        std::vector<Trick> TrickEntries;       // Skateboard.Trick.List, parsed (never empty)
        float LandingBonus         = 2.0f;     // Skateboard.LandingBonus
        bool Tricks                = true;     // Skateboard.Tricks
        uint32 TrickVisualKit      = 5036;     // Skateboard.Trick.VisualKit
        bool CrashEnable           = true;     // Skateboard.Crash.Enable
        uint32 CrashDamagePct      = 8;        // Skateboard.Crash.DamagePct
        uint32 CrashStaggerMs      = 1500;     // Skateboard.Crash.StaggerMs
        bool HardcoreSafe          = true;     // Skateboard.HardcoreSafe
        bool OffOnHit              = true;     // Skateboard.OffOnHit
        uint32 BoardCastMs         = 2000;     // Skateboard.BoardCastMs
        uint32 AckTimeoutMs        = 500;      // Skateboard.AckTimeoutMs
        bool Debug                 = false;    // Skateboard.Debug
    };

    // Config cache reference. Readable from any thread; only LoadConfig writes
    // it, before any character script can run.
    Config const& GetConfig();

    // Reads every Skateboard.* key, clamps the numerics to their documented
    // ranges and resolves the deck name to a display id that exists in
    // sCreatureDisplayInfoStore (falling back to the stock 'log' 26609 with one
    // warning). World thread. Returns false when the deck had to fall back.
    bool LoadConfig();

    // Resolves Skateboard.Deck to a display id the client can draw, once per load. Deferred out of
    // LoadConfig() because the DBC stores do not exist when OnAfterConfigLoad runs.
    std::uint32_t ResolveDeck();

    // Skateboard.Vehicle.Probe: world start only. Lists every Vehicle.dbc row that has a seat flagged
    // VEHICLE_SEAT_FLAG_CAN_CONTROL, with the seat's attachment offset, its ride animation and a
    // creature the world DB spawns for it - the candidates a vehicle-directed deck would borrow.
    void LogVehicleProbe();

    // One boarded character. A record also outlives the boarding while a crash
    // stagger is being served: then displayId is 0 (the deck is gone) and
    // staggerDeadlineMs is the moment the root is released.
    struct Deck
    {
        float v = 0.0f;                 // deck speed, yd/s, 0..MaxSpeed
        bool airborne = false;          // MOVEMENTFLAG_FALLING on the last packet
        uint32 takeoffMs = 0;           // when the current fall started
        uint32 trickEndMs = 0;          // when the queued clip ends (0 = none)
        uint32 activeClip = 0;          // clip queued for this fall, in ms
        float publishedRate = 1.0f;     // the run rate the server currently holds
        uint8 pendingAcks = 0;          // SMSG_FORCE_RUN_SPEED_CHANGE not ACKed
        uint32 gateDeadlineMs = 0;      // ACK deadline, or the 100 ms send throttle
        uint32 staggerDeadlineMs = 0;   // root release after a crash (0 = none)
        uint32 displayId = 0;           // the deck id this ride draws (0 = not boarded)
        uint32 mapId = 0;               // map the deck was boarded on
        uint32 debugNextMs = 0;         // next '.board' debug log line
        bool pushHeld = false;          // '.board push down' is in effect (Skateboard.Push.Mode = 1)
        uint32 pushNextMs = 0;          // next kick while held
        uint32 pushHeldSinceMs = 0;     // when the hold started, for Push.MaxHoldMs
        uint32 driveNextMs = 0;         // next carry impulse (Skateboard.Drive)
        uint32 impulseArcMs = 0;        // how long the last impulse keeps the rider off their feet
        uint32 followNextMs = 0;        // next probe of the deck follow (FollowMs/4, or FollowMs after a failed summon)
        ObjectGuid deckUnit;            // the summoned deck (Skateboard.Deck.Render = unit)
        float velX = 0.0f;              // the rider's measured ground velocity, yd/s
        float velY = 0.0f;
        float lastX = 0.0f;             // the position of the last measured change
        float lastY = 0.0f;
        uint32 sampleMs = 0;            // when the last measured position change happened
        uint32 lastPacketMs = 0;        // when the client last told us where it is (Manager::OnMove)
        bool havePos = false;           // lastX/lastY hold a sample worth differencing
        float grade = 0.0f;             // terrain gradient along the heading, uphill positive
        uint32 lastImpulseMs = 0;       // the last carry impulse, to tell a glide's fall from a jump
        float splineHeading = 0.0f;     // the heading the last spline segment walked (Drive.Mode = 1)
        bool haveHeading = false;       // splineHeading holds a value worth holding to
        uint32 castDeadlineMs = 0;      // boarding cast in flight (0 = none, nothing written yet)
        float castX = 0.0f;             // where the cast started, to catch a step
        float castY = 0.0f;
    };

    // One table, one hash lookup per hook for everyone who is not boarded.
    class Manager
    {
    public:
        // PlayerScript::OnPlayerUpdate: stagger release, then the integrator and
        // the gated rate publish for a boarded player. Map thread.
        void Update(Player* player, uint32 diff);

        // '.board on': refuses with a reason through the handler, otherwise
        // starts the boarding cast (Skateboard.BoardCastMs) or, with 0, writes
        // the deck and puts the run rate back at 1.0 straight away. Returns true
        // when the request was accepted, which includes a cast that is now
        // running - IsBoarded() is false until it completes.
        bool Board(Player* player, ChatHandler* handler);

        // A boarding cast in flight: the record exists, the deck does not.
        bool IsCasting(Player const* player) const;

        // Stops a boarding cast and pushes the reason to the client, so its bar
        // stops on the same second the server does. Nothing is taken back.
        void CancelCast(Player* player, std::string const& reason);

        // UnitScript::OnDamage: another unit hit the player. A cast stops; a
        // deck goes when Skateboard.OffOnHit says so. Map thread.
        void OnHit(Player* player);

        // PlayerScript::OnPlayerEnterCombat: a cast that started before the
        // fight is dropped. A deck already under the player is not.
        void OnEnterCombat(Player* player);

        // '.board off' and every forced drop: clears the visual, releases a
        // stagger, puts the run rate back at 1.0 and forgets the record.
        void Leave(Player* player);

        // '.board push': one impulse, clamped to MaxSpeed.
        void Push(Player* player, ChatHandler* handler);

        // '.board push down' / '.board push up': with Skateboard.Push.Mode = 1 the deck pumps one
        // impulse every Push.IntervalMs while the hold is in effect, for at most Push.MaxHoldMs.
        void PushDown(Player* player, ChatHandler* handler);
        void PushUp(Player* player);

        // '.board trick <name|index>': plays one entry of Skateboard.Trick.List by hand. A landing
        // before its clip ends crashes, exactly like a mistimed jump.
        void PlayTrick(Player* player, std::string const& which, ChatHandler* handler);

        // '.board tricks': one line per entry, `1 kickflip 399`, which is what the add-on's menu
        // and its bar slots are built from.
        void ListTricks(ChatHandler* handler) const;

        // True while the deck record exists and the mount field is written.
        bool IsBoarded(Player const* player) const;

        // Deck speed in yd/s (0 when not boarded).
        float Speed(Player const* player) const;

        // Unacknowledged forced-speed changes for this deck (0 when unboarded).
        uint8 PendingAcks(Player const* player) const;

        // ServerScript::CanPacketReceive: the client's ACK opened the gate.
        void OnAck(Player* player);

        // MovementHandlerScript::OnPlayerMove: falling edges queue the clip,
        // landing edges score it. Map thread.
        void OnMove(Player* player, MovementInfo const& movementInfo);

        // Drop the deck whatever the reason: death, logout, combat, a map
        // change, water, flying, a battleground. Always clears the mount field.
        void Cleanup(Player* player);
    };

    // The single manager instance behind every hook.
    Manager& GetManager();
}

#endif // MOD_SKATEBOARD_COMMON_H
