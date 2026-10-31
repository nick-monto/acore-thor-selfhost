# Player-centric random bot distribution

## Problem

With a small bot population (the default install is 50) the stock distribution spreads
randombots over the whole level range and over every enabled map. A solo player therefore
sees very few bots even though 50 are online: most are in another level band, another
continent, or both. Lowering the bot count makes the world emptier rather than quieter.

Placing bots in rings around the player fixes the emptiness but creates a worse problem: bots
appear and are pulled back with visible teleports right next to the player, so the world reads
as a gadget rather than as a server with people on it. The mode therefore keeps bots inside the
player's **zone** and never places one within a minimum distance of the player.

## What the patch adds

`patches/playerbots-player-centric.patch` adds an opt-in mode to the pinned
`mod-playerbots` source. When `Playerbots.RandomBotNearPlayers = 1`:

1. Each online random bot is assigned to a real, non-GM player in the open world. Assignments
   are distributed evenly (or capped by `RandomBotNearPlayersMaxPerPlayer`, which is also the
   most that can be in one player's zone at once).
2. The bot's level is kept inside `[player level - range, player level + range]`, clamped to
   the normal randombot level bounds. Death Knights are only assigned to players whose window
   can include the heroic starting level.
3. A bot is placed at a safe, level-appropriate travel location inside the assigned player's
   current zone: roads, hubs and quest areas, never inside an instance. Locations closer than
   `RandomBotNearPlayersMinDistance` yards to the player are skipped, so an arrival is not a
   bot landing on top of them. There is no ring: nothing is ever placed *at* the player.
4. A bot that drifts out of the player's zone (wandered across a border, was re-levelled
   elsewhere, died far away) is returned to the zone, but only after
   `RandomBotNearPlayersFollowInterval` seconds since its last move.
5. **Zone traffic.** A bot that has been in the zone for its whole visit leaves for another
   zone, stays away for a randomised while, then comes back: the zone population changes over
   time instead of standing still. See below.

A periodic pass re-levels bots that drift outside the window and moves bots that drifted away.
Re-levelling uses the normal full randomize, so that bot's gear and talents are regenerated.
At most 5 bots are touched per manager tick (2 while the bot pool is initialising), so a player
login does not produce a teleport spike.

The pass deliberately skips bots that are dead, in combat, rooted, in flight, in an instance,
battleground, arena, LFG queue, grouped (including bot-only groups), or controlled by a real
player. It never anchors to a GM and does not move bots into instances.

## Zone traffic

Zone traffic is what makes the zone feel alive rather than curated. Each assigned bot runs a
visit/away cycle:

| Phase | Default | What happens |
|---|---|---|
| Visit | 15-40 minutes, randomised per visit | The bot is in the player's zone, doing whatever its AI does: questing, grinding, travelling roads. |
| Leave | on the visit timer, verified first | The bot is teleported to a level-appropriate location **outside** the player's zone. This only happens once the bot is more than 100 yards from the player, so nobody is watching when it goes. |
| Away | 4-10 minutes, randomised per trip | The bot is marked away. Neither the periodic pass nor the bots' own teleport event pulls it back early, and it is never counted as present. |
| Return | after the away timer | The bot is placed in the player's zone again, at a travel location at least `MinDistance` away from the player, and a new visit starts. |

With the defaults, a 10-bot zone shows roughly seven or eight bots at any moment: the count
rises and falls, and the faces change, without any of it happening on screen. `RandomBotNearPlayersZoneTraffic = 0`
disables the cycle and keeps every assigned bot in the zone permanently (the pre-traffic
behaviour).

The cycle is deliberately not simulated as logins and logouts. `mod-playerbots` can churn bot
sessions (`EnablePeriodicOnlineOffline`), but that changes account allocation at the module
level and is visible to the whole world, not just this zone; moving a bot to another zone is
the same thing from the player's point of view and costs nothing.

## Configuration

| Key | Module default | Installer default | Meaning |
|---|---|---|---|
| `Playerbots.RandomBotNearPlayers` | `0` | `1` | Master switch |
| `Playerbots.RandomBotNearPlayersLevelRange` | `5` | `3` | Allowed level distance from the assigned player (max 40) |
| `Playerbots.RandomBotNearPlayersFollowInterval` | `120` | `120` | Seconds before a bot that drifted out of the zone is moved back (min 30) |
| `Playerbots.RandomBotNearPlayersMinDistance` | `100.0` | `100.0` | Yards of clearance from the player when placing a bot in the zone (about where the client stops drawing other players) |
| `Playerbots.RandomBotNearPlayersSameFaction` | `1` | `1` | Only assign same-faction players |
| `Playerbots.RandomBotNearPlayersMaxPerPlayer` | `10` | `20` | Bots per player, i.e. the most that can be in one zone; `0` = even distribution, no cap |
| `Playerbots.RandomBotNearPlayersZoneTraffic` | `1` | `1` | Visit/away cycle for zone bots |
| `Playerbots.RandomBotNearPlayersZoneVisitMinTime` | `900` | `900` | Shortest stay in the zone, in seconds |
| `Playerbots.RandomBotNearPlayersZoneVisitMaxTime` | `2400` | `2400` | Longest stay in the zone, in seconds |
| `Playerbots.RandomBotNearPlayersZoneAwayMinTime` | `240` | `240` | Shortest time away, in seconds |
| `Playerbots.RandomBotNearPlayersZoneAwayMaxTime` | `600` | `600` | Longest time away, in seconds |

The self-host installer enables the mode, keeps the same-faction restriction, keeps at most 20
bots in the player's zone and turns the traffic cycle on with the defaults above; the remaining
bots keep the normal world-wide distribution, so a 50-bot install still has 50 bots online. To
fill the world with both factions around a solo player, set
`RandomBotNearPlayersSameFaction = 0`; the bot AI may then treat the player as a PvP target.
Re-running the installer rewrites these keys to those defaults, and `--status`/`--repair` compare
the values rather than just their presence - so make later tuning changes after the last repair; it also drops the old `RandomBotNearPlayersRadius`,
`RandomBotNearPlayersInnerCount`, `RandomBotNearPlayersOuterRadius` and
`RandomBotNearPlayersZoneCount` keys, which the current build does not read.

Lowering `MaxPerPlayer` under the number of bots already assigned leaves the extra bots on the
normal distribution until they are reassigned, which happens when their assigned player logs
out.

## Interaction with existing randombot features

- `SyncLevelWithPlayers` only caps the upper level of newly randomised bots. This mode
  replaces that level selection while a player is assigned; the old behaviour is the fallback.
- `RandomBotConcentrateInPlayerZone` filters the normal teleport locations to zones that hold
  a player. This mode picks locations in the assigned player's zone directly, so it takes
  priority and needs no hub in the zone.
- `DowngradeMaxLevelBot` is bypassed while this mode is on; the near-player range owns level
  selection. Explicit fixed-level settings (`DisableRandomLevels`, `RandomBotFixedLevel`) are
  respected by the periodic pass, which then only moves bots without changing their level.
- `LevelBrackets.Enabled` is skipped while this mode is on; the bracket pass returns before it
  can pull bots back out of the player's level window.
- The bots' own periodic teleport event (`RandomTeleportForLevel`) is redirected into the
  player's zone, and is skipped entirely for a bot that is away on a traffic trip. That event
  would otherwise teleport away-bots home early and flatten the cycle.

## Delivery

The self-host build pins upstream `mod-playerbots` at `PB_COMMIT`; the patch is applied to
that exact checkout by the GitHub workflow before compilation, and by
`scripts/thor-install.sh --from-source`. No fork is created or required: the module is
checked out from upstream, patched in the build tree, and compiled there. The prebuilt
artifact therefore already contains the code, the `playerbots.conf.dist` documentation, and
the installer defaults.

## Verification

- The zone-plus-traffic revision (`IsBotInPlayerZone`, `TeleportBotToTargetZone` with the
  in-zone/elsewhere switch, `PlaceBotInPlayerZone`, `SendBotAwayFromPlayerZone`,
  `UpdatePlayerCentricBots`) compiles against the pinned core and playerbots headers: the four
  translation units that carry the change (`RandomPlayerbotMgr.cpp`, `PlayerbotAIConfig.cpp`,
  `RandomBotLevelMgr.cpp` and `PlayerbotAI.cpp`, which pulls the changed header) were built
  with the project's own compile flags and no errors. The full worldserver was not relinked in
  this workspace (its CMake re-run needs the original dependency prefix), and an in-game run is
  still required before claiming runtime verification.
- The patch applies cleanly to a pristine export of the pinned `mod-playerbots` commit:
  `patch -p1` reports only the six expected files, and the result is byte-identical to the
  tree that was compiled.
- The CI workflow applies the same patch to the pinned arm64 checkout before configuring; that
  arm64 build has not been run here.
- The source paths were reviewed against the existing randombot lifecycle: `RandomizeFirst`
  (initial level), `RandomTeleportForLevel` (periodic placement), `UpdateAIInternal` (periodic
  level/follow/traffic pass), `Remove` (state cleanup).
- The mode is intentionally source-level only: no client data, SQL or DBC change is involved.
- An in-game session is still required to validate how a zone feels at a given bot count,
  visit/away timers and minimum distance.
