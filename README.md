# acore-thor-selfhost

Hosting a stock **AzerothCore WoTLK (3.3.5a) + `mod-playerbots`** realm on the **AYN Thor** (Snapdragon 8 Gen 2, Adreno 740, 16 GB LPDDR5X, Android 13) so it can be played on the device itself — no PC, no Docker.

Scope: upstream AzerothCore as used by the [Dad's MMO Lab WotLK guide](https://github.com/DadsMmoLab/dads-mmo-lab/blob/main/guides/wow-wotlk/WoW-WotLK-HOWTO.md) (`mod-playerbots/azerothcore-wotlk` + `mod-playerbots/mod-playerbots`). Not in scope: the CoA/Ascension fork and its client.

## Status

Design phase. Nothing here has been built or run on a Thor yet; this repo currently holds only findings and design. The two questions that shape everything:

1. **Can the target launcher play a self-hosted 3.3.5a realm?** Yes, with a bounded change list (`docs/LAUNCHER-HOSTING-DESIGN.md` §3) — the 32-bit Windows client path exists in the shipped runtime.
2. **Can the target launcher also host the server?** Not by adapting the guide's Docker flow: this app has no foreground service, one X environment, one shared rootfs, and UID-wide process sweeps. It is a new subsystem (§4), and the pragmatic path is to run the server outside the app (§5).

## Documents

| Doc | What it answers |
|---|---|
| [`docs/LAUNCHER-HOSTING-DESIGN.md`](docs/LAUNCHER-HOSTING-DESIGN.md) | Can `wyattabuntjer/AYN-Thor-WoW-Launcher` support the self-hosted stack, and what exactly must change |
| [`docs/SERVER-STACK.md`](docs/SERVER-STACK.md) | The server half: build, data, database, bots, supervision, and what of the guide transfers |
| [`docs/RUNBOOK.md`](docs/RUNBOOK.md) | The ordered, copy-pasteable device steps (Termux + proot Ubuntu), with the gates to check as you go |
| [`pins.toml`](pins.toml) | Exact commits, artifact hashes and ports every document refers to |
| [`.github/workflows/server-aarch64.yml`](.github/workflows/server-aarch64.yml) | Builds the linux/arm64 server + client data on a native arm64 runner and attaches it to a release |

Confidence labels: **[V]** verified against source or an artifact inspected here, **[R]** reported, **[I]** inference.

## Hard requirements

- A **3.3.5a (build 12340) Windows x86 client** — the game files are supplied by the user; nothing here redistributes them.
- ~10 GB for the client, ~5 GB for the server (binaries + `maps`/`vmaps`/`mmaps` + world/playerbots database).
- 16 GB RAM shared between server and client: bot count has to be measured, not copied from desktop guides.

## Licence

GPL-3.0. No Blizzard client data, no AzerothCore database dumps and no server binaries are committed: the repo holds recipes, pins, hashes and (later) build artifacts published as releases.
