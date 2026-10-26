# mod-hardcore provenance

- Upstream: `https://github.com/maluramichael/mod-hardcore`
- Commit: `e136b45af8097730a058090ec933b1d8849b7478` (2026-10-11)
- License: GNU GPL v2 or later (see `LICENSE`)
- Vendored files: `LICENSE`, `README.md`, `conf/mod_hardcore.conf.dist`, `src/`
- Upstream files intentionally omitted: `.github/`, `.gitignore`, `.editorconfig`,
  `.gitattributes`, `icon.png`, `include.sh` (not used by the build)

## Local changes against the upstream commit

1. `src/Hardcore.cpp`/`src/Hardcore.h`: removed the
   `OnPlayerCanRepopAtGraveyard` override. A fallen ghost may release at a
   graveyard, so under-map/fatigue deaths can reach a spirit healer;
   `OnPlayerCanResurrect` still refuses the resurrection itself.
2. `src/Hardcore.cpp`: added an `IsBotControlled()` guard using
   `WorldSession::IsHeadless()`. Bot sessions cannot run `.hardcore on`, and
   every enforcement hook ignores them.
3. `conf/mod_hardcore.conf.dist` and `README.md`: document the changes.
4. `src/Hardcore.cpp`: `.hardcore on` now treats a character that mod-survivor
   has flagged as a promotion instead of refusing it (a live read of
   `survivor_flags.enabled`, no shared headers, no new build dependency). The
   two rulesets form a one-way ladder - normal -> Survivor -> Hardcore, and
   normal -> Hardcore - so the command accepts a Survivor-flagged character,
   deletes its `survivor_flags` row (`ClearSurvivorFlag`) so exactly one
   ruleset is stored, and reports "Survivor mode is dropped: from here on this
   character permanently falls on death." The reverse is still refused by
   mod-survivor's `.survivor on`: Hardcore is terminal, so a death that is
   already final must not become recoverable. A missing `survivor_flags` table
   (mod-survivor not installed) is treated as "not flagged".
5. Hardcore death container (behaviour contributed by mod-survivor, not a change
   to this module's own hooks): a Hardcore-flagged death is recognised by
   `Survivor::HandleDeath` before the Survivor flag and calls
   `DropGearAtDeath(..., noMail = true)`, leaving the equipped items and bags in
   a lootable container at the death spot, announced when
   `Survivor.AnnounceDeaths = 1`, with no revival and no `Hardcore.DeathPolicy`
   change. The container row carries `no_mail = 1`, so it rots at expiry instead
   of being mailed to a character that can no longer be played; the fallen
   character is no longer Survivor-flagged and cannot open it, but any
   Survivor-flagged character can. Upstream has neither mod-survivor nor a
   container.

The default policy remains `Hardcore.DeathPolicy = ghost`.
