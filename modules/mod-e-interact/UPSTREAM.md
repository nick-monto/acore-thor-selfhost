# mod-e-interact provenance

- Upstream: `https://github.com/Rainblower/mod-e-interact`
- Commit: `0d17e70622085877929c68d583b18f3b0156c81c` (initial release, 2026-10)
- License: MIT (see `LICENSE`)
- Vendored files: `LICENSE`, `conf/mod_e_interact.conf.dist`, `src/`
- Upstream files intentionally omitted: `.github/`, `.gitignore`, `.editorconfig`,
  `.gitattributes`, `README.md`, `README_RU.md` (upstream's own README is replaced by
  `docs/INTERACT-KEY-DESIGN.md` and the module section of `docs/SERVER-STACK.md`), and
  `addon/EInteract` — the client addon lives in the launcher repository
  (`acore-thor-launcher/app/src/main/assets/einteract/`), because the launcher is what
  installs addons into the client.

## How it works

The 3.3.5a client cannot look for units or objects around the player, and `InteractUnit`
is protected, so the addon sends a **self-addressed addon whisper** and the server does the
work:

```
addon: SendAddonMessage("EINT", "near", "WHISPER", UnitName("player"))
client: CMSG_MESSAGECHAT, LANG_ADDON, whisper, receiver = self
server: PlayerScript::OnPlayerCanUseChat(...) -> false   (swallowed before chat)
server: target -> nearest corpse -> nearest chest/node/quest object -> nearest NPC
addon: whisper "EINT\tloot|gather|talk|use|none" back (LANG_ADDON)
```

The module never invents game state: it calls the same `WorldSession` handlers the client's
own packets reach (`HandleLootOpcode`, `HandleLootMoneyOpcode`,
`HandleAutostoreLootItemOpcode`, `HandleLootReleaseOpcode`, `HandleGameObjectUseOpcode`) or
queues the packet a right click would send (`CMSG_GOSSIP_HELLO`, `CMSG_LIST_INVENTORY`, ...),
so distance, visibility, loot permission and gossip checks all still apply.

## Local changes against the upstream commit

1. `src/e_interact.cpp`: the **gameobject pass**, new in this fork and gated by
   `EInteract.Use` (default on). Upstream only walks creatures, so chests, herb and ore
   nodes and questgiver objects did nothing:
   - `CanOpenObject()` mirrors `Spell::CanOpenLock` without a spell: an item key in the bags
     (`LOCK_KEY_ITEM`), the gathering/lockpicking skill the lock asks for
     (`LOCK_KEY_SKILL`, compared against `LockEntry::Skill`), or a spell the lock names
     (`LOCK_KEY_SPELL`). No lock means open to everyone.
   - `LootGameObject()` reuses `Player::SendLoot(guid, LOOT_CORPSE)`, which generates
     the loot exactly as a right click does, then walks the same
     money → autostore → release chain as `LootCorpse()`. The release is what
     deactivates an emptied node, so respawn and restock timers are the stock ones.
   - `TryGameObject()` opens `GAMEOBJECT_TYPE_CHEST` through `SendLoot`,
     `GAMEOBJECT_TYPE_QUESTGIVER` through `HandleGameObjectUseOpcode`, and
     `GAMEOBJECT_TYPE_MAILBOX` through `HandleGetMailList` (a mailbox has no `Use()` case:
     the client asks for the inbox with the object's GUID, and `CanOpenMailBox` checks the
     distance and the type). Doors, buttons, traps, goobers and battleground flags are
     deliberately not touched: they change world state rather than give the player
     something.
   - `HandleNear()` calls the pass between the corpse stage and the NPC stage, using the
     core's own `WorldObject::FindNearestGameObjectOfType` (spawned, in range, nearest
     first).
2. `conf/mod_e_interact.conf.dist`: documents `EInteract.Use`.

## Known limits of the local change

- Only the **nearest** chest/node and the nearest questgiver object are tried. If the
  nearest node is already looted, that stage reports nothing and the key falls through to
  the NPC stage; step sideways and press again for the next node.
- A node is looted without the gathering cast, so there is no skill-up and no gather
  animation. The lock check above is what keeps this from ignoring tool requirements.
