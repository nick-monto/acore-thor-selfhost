# Module development on the local realm

This is the human loop for changing a module in the local x86_64 Docker realm.
The device and CI paths are in [`RUNBOOK.md`](RUNBOOK.md) and
[`module-checks.yml`](../.github/workflows/module-checks.yml); the agent traps
and delivery-surface rules are in the `azerothcore-custom-module` skill.

## 1. Source layout

```text
modules/mod-<name>/
  src/                          C++ and the loader symbol Addmod_<name>Scripts
  data/sql/world/base/*.sql     module SQL, applied as MODULE updates
  conf/mod-<name>.conf.dist     installed to /opt/acore/etc/modules/
sql/custom/db_world/*.sql       CUSTOM slot, applied without a module rebuild
scripts/dev-realm.sh            the loop below
scripts/sql/check_module_sql.py SQL structural and spell-row checks
scripts/dbc/check_spell_dbc.py  spell_dbc <-> client Spell.dbc checks
scripts/check-module-wiring.py  every module on every build/install surface
```

The build and `dev-realm.sh sync` copy `modules/<name>` into
`$CORE/modules/<name>` before configure. The running container's
`/opt/acore/bin` is an image layer; `/opt/acore/src` is the SQL tree the
worldserver updater reads; `/opt/acore/etc` holds the confs. Editing the
repository does not change any of them until the matching step runs.

## 2. The loop: `scripts/dev-realm.sh`

```sh
bash scripts/dev-realm.sh sync mod-creature-affixes --dry-run
bash scripts/dev-realm.sh sync mod-creature-affixes --yes
bash scripts/dev-realm.sh build mod-creature-affixes
bash scripts/dev-realm.sh ship mod-creature-affixes --dry-run
bash scripts/dev-realm.sh verify
```

| Command | What it does |
| --- | --- |
| `status` | Compares every module tree with `$CORE/modules`, the build type/flags, the built binary and the container binary hash; prints the converging commands. Exit 1 on drift |
| `sync [MODULE...]` | `rsync -a --delete` the selected repository module trees into `$CORE/modules`; a preview until `--yes` |
| `build [MODULE...]` | Refuses a stale core copy unless `--allow-dirty`; re-runs `cmake` only when needed, then `ninja -C $BUILD worldserver`. Never `cmake --install` |
| `stage` | Strips the built worldserver, sets rpath `$ORIGIN/../lib`, and `docker cp`s it over `/opt/acore/bin/worldserver`; `--output PATH` writes it locally instead |
| `sql [MODULE...]` | `docker cp`s each selected module's `data/sql` tree into `/opt/acore/src/modules/<name>/data/sql`; the updater applies it on the next restart |
| `conf [MODULE...]` | `docker cp`s `conf/*.conf.dist` into `/opt/acore/etc/modules/`; `--reset-conf` also removes the live `.conf` so the entrypoint recreates it |
| `restart` | `docker compose restart acore`, then waits up to 300s for the `world-ready` and `auth-ready` markers; never `down`/`up` |
| `ship [MODULE...]` | `sync --yes`, `build`, `stage`, `sql`, `conf`, `restart`, then `status` |
| `verify` | Read-only: compares the container binary to `--output`, prints its rpath, checks the readiness markers, and queries `acore_world.updates` for the selected modules' SQL filenames |

The host core and its `mod-playerbots` clone must be the commits in `pins.toml`.
`status` reports a mismatch and `build` refuses it. A checkout at the wrong core
commit fails later as an unrelated playerbots header error, such as
`PlayerbotsDatabasePool::CreateConnection` not overriding the core's signature;
that is a pin problem, not a module problem.

Options: `--repo PATH`, `--core PATH`, `--build-dir PATH`, `--container NAME`,
`--module NAME`, `--dry-run`, `--yes`, `--allow-dirty`, `--reset-conf`,
`--output PATH`.
`sync` is a preview until `--yes`; `stage`, `sql` and `conf` preview with
`--dry-run`; every mutating command prints itself before running. `--yes` does
not apply to `build` or `restart` — they always run when named.

Rules for this host:

- Never `cmake --install` here: it installs into the host's `/opt/acore`.
- Never copy a host-built worldserver in by hand without stripping it and
  setting `patchelf --set-rpath '$ORIGIN/../lib'`; the container has no
  `/usr/local` dependency tree.
- Never restart the development stack with `docker compose down` or
  `docker compose up --build`; `restart` is the iteration step. `down -v`
  deletes the realm.

## 3. SQL delivery rules

| Source | State in `updates` | Applied |
| --- | --- | --- |
| `modules/<name>/data/sql/world/base/*.sql` | `MODULE` | Every boot; a changed file is re-applied |
| `sql/custom/db_world/*.sql` | `CUSTOM` | Every boot; a changed file is re-applied |
| `<SourceDirectory>/data/sql/base/db_world/` | none | Only by `PopulateDatabase`, when the schema is empty |

The worldserver walks module `data/sql/` recursively and keeps directories whose
name contains `world`. It records each `.sql` by filename with a SHA-1 of its
content, so editing a file re-applies the whole file, not just the line you
changed. Filenames are global keys: two files with the same basename abort boot
with `Duplicate filename`.

Consequences:

- Treat `data/sql/world/base` as an update tree even though the directory is
  called `base`. It is not a fresh-install dump; only the core's own
  `data/sql/base/db_world` is.
- Make every statement safe to run twice (`ON DUPLICATE KEY UPDATE`, guarded
  `UPDATE`) and add a new numbered file to change data a live realm already has.
- Characters tables are created by module code with `CREATE TABLE IF NOT
  EXISTS` at startup here. A failing module SQL file aborts boot on this fork,
  which is why several modules keep their schema in C++.
- Inspect the truth rather than the file:

```sh
docker exec -it acore-realm-acore-1 \
  mysql -h db -uacore -pacore acore_world \
  -e "SELECT name, hash, state FROM updates WHERE name LIKE 'creature_affixes%';"
```

The defaults `acore`/`acore` come from `docker/docker-compose.yml`.

## 4. Spell and DBC rules

Server-side spell rules:

- Name every column in an `INSERT` into `spell_dbc`.
- `EquippedItemClass = -1` on every custom row. The default `0` is an item-class
  requirement and normal casts fail with `SPELL_FAILED_EQUIPPED_ITEM_CLASS`.
- Aura `79` (percent damage done) needs `EffectMiscValue` = school mask; `1` is
  physical.
- `DurationIndex = 21` is permanent.
- `EffectRadiusIndex` is a `SpellRadius.dbc` row number, not yards. Here row `7`
  is 10 yd, row `14` is 8 yd and row `8` is 5 yd.
- A proc needs the triggered spell's row to exist; `100205` triggers `100206`.
- `spell_script_names.ScriptName` must equal the `RegisterSpellScript` class
  name, and the binding should use `ON DUPLICATE KEY UPDATE` rather than
  `INSERT IGNORE`, which hides a stale row.
- `spell_dbc` and `spell_script_names` load at boot. `.reload config` does not
  rebuild `SpellInfo`; restart after changing either table.

Client boundary:

| Change | Server | Client |
| --- | --- | --- |
| Reuse a stock spell id | full spell | name, icon, visual, log line |
| Custom `spell_dbc` row | full spell | nothing: no client row |
| Custom row with stock `SpellVisualID`/`SpellIconID` | row is read | still nothing; ids inside a server-only row do not exist client-side |
| Item id added by `scripts/item-dbc/patch_item_dbc.py` | server `data/dbc/Item.dbc` | icon if the patch archive is the one the client loads |
| Custom item without an `Item.dbc` record | attribute checks skipped | red question mark in bags |

The server loads the client's `Spell.dbc` and then overlays `spell_dbc` rows
from the world database (`LOAD_DBC(sSpellStore, "Spell.dbc", "spell_dbc")`).
The client reads only the DBC files in its own `Data/*.MPQ`. Custom text, icon,
visual or combat-log text therefore needs a client `Spell.dbc` row in a patch
MPQ and a client test. The item patcher writes the highest free archive name,
`patch-9` down to `patch-4`, and `--verify --client-data <Data>` reports which
archive the client actually reads.

## 5. Checks

Run from the repository root. None of these replaces a client test.

| Command | Proves | Does not prove |
| --- | --- | --- |
| `bash scripts/syntax-check.sh` | every repository module TU compiles with the core's own flags; refuses a core/playerbots checkout that differs from `pins.toml` | linking, the `$CORE` copy, runtime behaviour |
| `python3 scripts/sql/check_module_sql.py --repo .` | SQL is idempotent, spell rows have explicit `spell_dbc` columns and `EquippedItemClass=-1`, and script/trigger references resolve | that the live database applied it, or that every column matches the core schema |
| `python3 scripts/check-module-wiring.py` | every module is named by `pins.toml`, the three server workflows and `thor-install.sh` | that any of them compiles |
| `bash scripts/dev-realm.sh verify` | local container state, staged files and stale-artefact checks | anything client-side |
| `python3 scripts/runewords/gen_runewords.py --core <core> --check` | generated CUSTOM SQL matches its generator | database state |
| `python3 scripts/item-dbc/patch_item_dbc.py ... --check` | patched `Item.dbc` matches the generator | that the icon draws |
| `python3 scripts/item-dbc/patch_item_dbc.py --verify --client-data <Data>` | which archive the client loads and whether it carries the ids | that the game renders them |

The spell check needs game data to say anything about the client:

```sh
python3 scripts/dbc/check_spell_dbc.py \
  --sql modules/mod-creature-affixes/data/sql/world/base/creature_affixes_50_custom_effects.sql \
  --spell-dbc /path/to/Spell.dbc \
  --allow-server-only 100200-100203,100205-100207
```

Without `--spell-dbc` it skips cleanly; `--strict` makes the custom-visual
warning an error. The allow-list is the set of ids this repository deliberately
keeps server-only.

## 6. Evidence matrix

| Claim | Preflight | Runtime (server) | Client-only |
| --- | --- | --- | --- |
| Module code loads | syntax-check + wiring check | boot log has no loader error; script responds | n/a |
| Module SQL landed | `check_module_sql.py` | `updates` row has the new hash with state `MODULE`; select the row | n/a |
| Custom spell exists | `check_spell_dbc.py` | `.affix set ...`, then `SpellInfo`/cast behaviour | name, icon, tooltip |
| Aura applies | row and attributes reviewed | server aura/caster state, health or damage change | buff-bar button |
| Aura is readable by the add-on | attributes are not `DO_NOT_DISPLAY` | `UnitAura` returns it (add-on probe) | target/focus header |
| Visual plays | n/a for custom rows | nothing to observe server-side | client frame, combat log |
| Custom item shows an icon | item patcher `--check`/`--verify` | loot window slot display id | bag icon; red `?` if absent |
| Conf key takes effect | installer/conf file | `.reload config` and the changed behaviour | n/a |

A completed boot only proves the updater did not abort. Query the table, probe
in game, then test anything the client draws in a 3.3.5a client; record it as
unproven until then.

