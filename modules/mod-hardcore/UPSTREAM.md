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
3. `conf/mod_hardcore.conf.dist` and `README.md`: document the two changes.

The default policy remains `Hardcore.DeathPolicy = ghost`.
