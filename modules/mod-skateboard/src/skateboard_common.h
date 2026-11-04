/*
 * mod-skateboard - shared declarations.
 *
 * A trip to a private realm, not a mount: the character stays on foot, and the
 * module publishes a *run speed cap* instead of moving the character itself.
 * The player is the mover (MovementHandler.cpp), so a boarded character simply
 * keeps pressing forward and the core lets it travel faster.
 *
 *   - '.board on' writes a mount display id into UNIT_FIELD_MOUNTDISPLAYID
 *     without setting UNIT_FLAG_MOUNT (Skateboard.MountedPose = 0), so the deck
 *     is drawn under a standing character; MountedPose = 1 calls Unit::Mount()
 *     for a sitting rider with the flag set.
 *   - A per-tick integrator keeps a deck speed `v` (yd/s): terrain slope pulls
 *     it down or lets it run, static + rolling friction bleed it off, and
 *     '.board push' adds one impulse. `v` is published as a run speed *rate*
 *     (v / 7.0f, the base run speed) through SetSpeed(MOVE_RUN, rate, true).
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

#include <string>

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
    constexpr float MIN_RUN_RATE = 1.0f;

    // Trick clip lengths, by speed band: slow, medium, fast. A landing has to
    // last at least as long as the clip the take-off queued.
    constexpr uint32 CLIP_SHORT_MS  = 500;
    constexpr uint32 CLIP_MEDIUM_MS = 900;
    constexpr uint32 CLIP_LONG_MS   = 1400;

    // Cached config. Written only by LoadConfig() (world thread, at startup and
    // on '.reload config'); read from any thread after that - the same
    // one-writer/startup assumption the core's own config accessors make.
    struct Config
    {
        bool Enable                = false;    // Skateboard.Enable
        std::string DeckName       = "log";    // Skateboard.Deck, as configured
        uint32 DeckDisplayId       = 26609;    // Skateboard.Deck, resolved
        uint32 MountedPose         = 0;        // Skateboard.MountedPose
        float MaxSpeed             = 21.0f;    // Skateboard.MaxSpeed
        float PushImpulse          = 4.0f;     // Skateboard.PushImpulse
        float FrictionStatic       = 1.5f;     // Skateboard.Friction.Static
        float FrictionRolling      = 0.004f;   // Skateboard.Friction.Rolling
        float SlopeGain            = 9.0f;     // Skateboard.SlopeGain
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
        uint32 displayId = 0;           // what was written to the mount field
        uint32 mapId = 0;               // map the deck was boarded on
        uint32 debugNextMs = 0;         // next '.board' debug log line
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

        // '.board trick <1|2|3>': plays clip 1, 2 or 3 by hand. A landing
        // before it ends crashes, exactly like a mistimed jump.
        void PlayTrick(Player* player, uint32 clip, ChatHandler* handler);

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
