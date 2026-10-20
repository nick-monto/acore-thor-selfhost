# Player-centric random bot distribution

## Problem

With a small bot population (the default install is 50) the stock distribution spreads
randombots over the whole level range and over every enabled map. A solo player therefore
sees very few bots even though 50 are online: most are in another level band, another
continent, or both. Lowering the bot count makes the world emptier rather than quieter.

## What the patch adds

`patches/playerbots-player-centric.patch` adds an opt-in mode to the pinned
`mod-playerbots` source. When `Playerbots.RandomBotNearPlayers = 1`:

1. Each online random bot is assigned to a real, non-GM player in the open world. Assignments
   are distributed evenly (or capped by `RandomBotNearPlayersMaxPerPlayer`).
2. The bot's level is kept inside `[player level - range, player level + range]`, clamped to
   the normal randombot level bounds. Death Knights are only assigned to players whose window
   can include the heroic starting level.
3. If the bot is more than twice `RandomBotNearPlayersRadius` yards away, or on another map,
   it is placed at a safe reachable point roughly 35-100% of the radius from the assigned
   player (28-80 yards at the installer's 80-yard radius), after a short cooldown
   (`RandomBotNearPlayersFollowInterval`).
4. A periodic pass re-levels bots that drift outside the window and moves bots that drifted
   away. Re-levelling uses the normal full randomize, so that bot's gear and talents are
   regenerated. At most 5 bots are touched per manager tick (2 while the bot pool is
   initialising), so a player login does not produce a teleport spike.

The pass deliberately skips bots that are dead, in combat, rooted, in flight, in an instance,
battleground, arena, LFG queue, grouped (including bot-only groups), or controlled by a real
player. It never anchors to a GM and does not move bots into instances.

## Configuration

| Key | Module default | Installer default | Meaning |
|---|---|---|---|
| `Playerbots.RandomBotNearPlayers` | `0` | `1` | Master switch |
| `Playerbots.RandomBotNearPlayersLevelRange` | `5` | `5` | Allowed level distance from the assigned player (max 40) |
| `Playerbots.RandomBotNearPlayersFollowInterval` | `120` | `120` | Seconds between follow attempts per bot (min 30) |
| `Playerbots.RandomBotNearPlayersRadius` | `60.0` | `80.0` | Preferred distance from the player in yards (5-300) |
| `Playerbots.RandomBotNearPlayersSameFaction` | `1` | `1` | Only assign same-faction players |
| `Playerbots.RandomBotNearPlayersMaxPerPlayer` | `0` | `8` | Cap per player; `0` = even distribution, no cap |

The self-host installer enables the mode, keeps the same-faction restriction, caps the
near-player population at 8 bots and places them 28-80 yards away; the remaining bots keep the
normal world-wide distribution, so a 50-bot install still has 50 bots online. To fill the world
with both factions around a solo player, set `RandomBotNearPlayersSameFaction = 0`; the bot AI
may then treat the player as a PvP target. Re-running the installer rewrites these six keys to
those defaults, so make later tuning changes after it has finished.

## Interaction with existing randombot features

- `SyncLevelWithPlayers` only caps the upper level of newly randomised bots. This mode
  replaces that level selection while a player is assigned; the old behaviour is the fallback.
- `RandomBotConcentrateInPlayerZone` filters the normal teleport locations to zones that hold
  a player. This mode teleports relative to the assigned player directly, so it takes priority
  and has no zone/hub requirement.
- `DowngradeMaxLevelBot` is bypassed while this mode is on; the near-player range owns level
  selection. Explicit fixed-level settings (`DisableRandomLevels`, `RandomBotFixedLevel`) are
  respected by the periodic pass, which then only moves bots closer without changing their level.
- `LevelBrackets.Enabled` is skipped while this mode is on; the bracket pass returns before it
  can pull bots back out of the player's level window.

## Delivery

The self-host build pins upstream `mod-playerbots` at `PB_COMMIT`; the patch is applied to
that exact checkout by the GitHub workflow before compilation, and by
`scripts/thor-install.sh --from-source`. No fork is created or required: the module is
checked out from upstream, patched in the build tree, and compiled there. The prebuilt
artifact therefore already contains the code, the `playerbots.conf.dist` documentation, and
the installer defaults.

## Verification

- The patched module compiled into the local x86_64 worldserver from the pinned source
  (`ninja -C build worldserver`); the resulting binary exposes
  `RandomPlayerbotMgr::UpdatePlayerCentricBots`, `GetPlayerCentricTarget`,
  `IsPlayerCentricTargetValid`, `IsPlayerCentricBotEligible` and `AdjustBotLevel`.
- The patch applies cleanly to a pristine export of the pinned `mod-playerbots` commit:
  `patch --dry-run -p1` reports only the six expected files.
- The CI workflow applies the same patch to the pinned arm64 checkout before configuring; that
  arm64 build has not been run here.
- The source paths were reviewed against the existing randombot lifecycle: `RandomizeFirst`
  (initial level), `RandomTeleportForLevel` (initial placement), `UpdateAIInternal` (periodic
  level/follow pass), `Remove` (target cleanup).
- The mode is intentionally source-level only: no client data, SQL or DBC change is involved.
- An in-game session is still required to validate how visible the bots feel at a given bot
  count, radius and follow interval.
