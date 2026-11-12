# mod-runewords

Rune items drop from creatures on a level ladder; hold a word's pattern of them in one
container and the word's auras are granted. This module is the carrier, not the content: the
runes, the words and their drops are generated SQL, and the design — including why the auras
come from stock spells and why nothing touches a socket — is in
[`docs/RUNEWORDS-DESIGN.md`](../../docs/RUNEWORDS-DESIGN.md).

## What it reads

| Source | Contents |
|---|---|
| `sql/custom/db_world/runewords_10_items.sql` | the 33 rune items (93000–93032) |
| `sql/custom/db_world/runewords_20_words.sql` | `runeword_words`: the shapes and their spells |
| `sql/custom/db_world/runewords_30_drop_tables.sql`, `runewords_40_creature_drops.sql` | the level-banded world drops |
| `character_runeword_rune` (characters) | each rune instance's 1..100 roll |

The four SQL files are written by `scripts/runewords/gen_runewords.py`; edit the generator's
tables and regenerate rather than editing them by hand. The module ships no SQL of its own,
because on this fork a failing module SQL file aborts the whole worldserver boot, so its one
characters table is created programmatically at startup.

## What it does

A word is a pattern of `(row, column)` cells over the grid the client draws, which is four
columns wide and filled row-major, so a word may run along a row, down a column, or around a
corner. The shipped table holds 24 words, 2 to 6 cells each, and every rune in the ladder is
named by at least one of them; the generator refuses a table with a rune nothing uses. A match is scoped to one container — the backpack or one bag — because bag windows
keep no order among themselves. The longest matching word wins, then the lowest word id.

Each rune rolls 1..100 the first time it is seen, and a word is worth the average of its rune
rolls, rounded half up and floored by the word's own minimum. The average scales the borrowed
spell's own basepoints at cast time, so one stock spell serves every roll.

The table borrows stock `Spell.dbc` auras only — stats, attack power, per-school spell damage,
armour, resistances, combat ratings, mana and health regeneration, a damage shield and the stock
proc auras — and `scripts/runewords/check_spells.py` re-checks every borrowed id against a real
`Spell.dbc` (permanent, deterministic, caster-targeted).

Runes do not stack: the roll belongs to the item instance. Their quality colour and vendor price
follow the ladder - white at the bottom through legendary at the top - and the roll pays up to
that price again when the rune is sold (`OnPlayerCanSellItem` pays `SellPrice * roll / 100`, and
the core pays the template price after it).

The applied state is re-derived from the bags on an interval
(`Runewords.SweepIntervalMs`, default 1000 ms) rather than from inventory events, so a missed
event can never leave a stale aura behind.

## Configuration

`conf/mod-runewords.conf.dist`, installed as `etc/modules/mod-runewords.conf`:
`Runewords.Enable`, `Runewords.SweepIntervalMs`, `Runewords.ApplyWhenDead`,
`Runewords.Announce`. Every key is documented in that file.

## Commands

| Command | Security | Effect |
|---|---|---|
| `.runewords status` | player | the active word, and every rune cell in the bags with its roll |
| `.runewords list` | gamemaster | the loaded words: runes in order, level, scale, spells |
| `.runewords reload` | gamemaster | re-read `runeword_words` without a restart |

## Checks

`scripts/syntax-check.sh modules/mod-runewords/src/*.cpp` compile-checks the sources against the
pinned core's own flags — the same gate CI runs. `scripts/runewords/gen_runewords.py --core
../azerothcore-wotlk --check` fails when the committed SQL stops matching a fresh run, and
`scripts/item-dbc/patch_item_dbc.py --check` does the same for the client's `Item.dbc`.

## Local changes

`SendNotification` does not exist on this fork's `WorldSession`, so the announce line is a
`ChatHandler` system message; the command table is declared with the same file-scope
`using namespace Acore::ChatCommands` that `mod-survivor` uses, because the return type of
`GetCommands()` is resolved before the function body opens.
