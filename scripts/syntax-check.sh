#!/usr/bin/env bash
#
# Compile-check every module's translation units against the core's own flags.
#
# The arm64 build is the only compiler this repository has otherwise: it is a
# manual dispatch that packages a release and takes most of an hour, and it
# caught two errors in mod-mercenaries that were both a misspelt core symbol -
# GetMSTime for getMSTime, and OnLogin for OnPlayerLogin. A -fsyntax-only pass
# over the module sources finds both in seconds, which is the difference between
# a typo caught at the keyboard and a typo caught the next morning.
#
# Nothing is built or linked. The flags come from a configured core build's
# compile_commands.json when there is one (a development host), and otherwise
# from the checked-in scripts/module-flags.txt, which was generated from such a
# build and templated so it works wherever the checkouts are (CI). Either way the
# include set, the defines and the C++ standard are the real build's.
#
# Usage:
#   scripts/syntax-check.sh                                   # every module source
#   scripts/syntax-check.sh modules/mod-mercenaries/src/MercenaryMgr.cpp   # named files
#   scripts/syntax-check.sh --dump-flags                       # regenerate the flags file
#
# Environment:
#   AC_CORE        core checkout             (default: ../azerothcore-wotlk)
#   AC_PLAYERBOTS  mod-playerbots checkout   (default: ../mod-playerbots)
#   AC_BUILD_DIR   configured core build     (default: $AC_CORE/build)
#   AC_DEPS_ROOT   core dependency headers   (default: ~/.cache/acore-deps/root,
#                                              or / when they are installed system-wide)
#
# Exits 0 when it checked everything and everything compiled, or when it could
# not check (no core checkout, no flags, no dependency headers) - it prints why
# and how to fix it rather than failing a checkout that was never meant to have
# a build.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CORE="${AC_CORE:-$ROOT/../azerothcore-wotlk}"
PLAYERBOTS="${AC_PLAYERBOTS:-$ROOT/../mod-playerbots}"
BUILD="${AC_BUILD_DIR:-$CORE/build}"
DEPS="${AC_DEPS_ROOT:-$HOME/.cache/acore-deps/root}"
COMPILE_COMMANDS="$BUILD/compile_commands.json"
FLAGS_FILE="$ROOT/scripts/module-flags.txt"

# The recorded flags carry absolute paths, so the roots are normalised before they
# are matched: "$ROOT/../core" would never equal the "/abs/core" in a command line.
CORE="$(realpath -m "$CORE")"
PLAYERBOTS="$(realpath -m "$PLAYERBOTS")"
DEPS="$(realpath -m "$DEPS")"
# AC_DEPS_ROOT=/ means "the dependency headers are installed system-wide", which is
# how a CI runner has them; the empty prefix keeps "$DEPS/usr/include" = /usr/include.
[ "$DEPS" = "/" ] && DEPS=""

skip() {
    printf 'skip  %s\n' "$*"
    exit 0
}

fail() {
    printf 'FAIL  %s\n' "$*" >&2
    exit 1
}

# The flags for one module translation unit, NUL separated, with the three checkout
# roots resolved for this machine. Works from a configured build or from the file.
templated_flags() {
    python3 - "$1" "$CORE" "$PLAYERBOTS" "$DEPS" "${2:-resolved}" <<'PY'
import json, os, shlex, sys

source, core, playerbots, deps, mode = sys.argv[1:6]

# The core's build records playerbots inside its own modules/, takes its dependency
# headers from wherever they were unpacked on the machine that configured it, and
# writes absolute paths throughout. All three become placeholders, so flags taken on
# one machine work on another.
RECORDED_PLAYERBOTS = os.path.join(core, 'modules', 'mod-playerbots')
RECORDED_DEPS = ('/tmp/acdeps/root',)
PLACEHOLDERS = {'@CORE@': core, '@PLAYERBOTS@': playerbots, '@DEPS@': deps}


def templatise(token):
    token = token.replace(RECORDED_PLAYERBOTS, '@PLAYERBOTS@')
    token = token.replace(playerbots, '@PLAYERBOTS@')
    token = token.replace(core, '@CORE@')
    for recorded in RECORDED_DEPS:
        token = token.replace(recorded, '@DEPS@')
    # An empty root is the system case (AC_DEPS_ROOT=/, so the dependency headers are
    # installed): replacing '' would insert @DEPS@ between every character of every
    # token and shred the other placeholders before they are resolved.
    if deps:
        token = token.replace(deps, '@DEPS@')
    return token


def resolve(token):
    for placeholder, value in PLACEHOLDERS.items():
        token = token.replace(placeholder, value)
    return token


if source.endswith('compile_commands.json'):
    commands = json.load(open(source))
    # A module translation unit carries the module include set, which is what these
    # sources need; fall back to any entry so the check works on a build that was
    # configured before the modules were added.
    entries = [e for e in commands if '/modules/' in e['file']] or commands
    if not entries:
        sys.exit('no compile commands in the database')

    entry = entries[0]
    out, skip_next = [], False
    for part in shlex.split(entry['command']):
        if skip_next:
            skip_next = False
            continue
        if part == '-o':
            skip_next = True
            continue
        if part in ('-c', '-MD', '-MF', '-MT'):
            continue
        if part == entry['file']:
            continue
        out.append(part)
else:
    out = []
    for line in open(source):
        line = line.rstrip('\n')
        if line.strip() and not line.startswith('#'):
            out.append(line)

# One pass for both sources: the derived tokens were just templated, the file's are
# already templated, and resolving is idempotent for anything with no placeholder.
# The dump keeps the placeholders, which is the whole point of the file.
out = [templatise(token) for token in out]
if mode != 'keep-placeholders':
    out = [resolve(token) for token in out]
    # A dependency root of / is the system case: the packages installed the headers in
    # /usr/include, which the compiler already searches. Passing it again as -isystem
    # puts it ahead of libstdc++'s own directory in the search order, and then
    # #include_next <stdlib.h> in <cstdlib> has nothing left to find: every translation
    # unit dies on its first standard header.
    cleaned, i = [], 0
    while i < len(out):
        if out[i] == '-isystem' and i + 1 < len(out) and out[i + 1].rstrip('/') == '/usr/include':
            i += 2
            continue
        cleaned.append(out[i])
        i += 1
    out = cleaned
if '-fsyntax-only' not in out:
    out.append('-fsyntax-only')
sys.stdout.write('\0'.join(out))
PY
}

if [ "${1:-}" = "--dump-flags" ]; then
    [ -f "$COMPILE_COMMANDS" ] || fail "no $COMPILE_COMMANDS to derive the flags from; configure the core first"
    commit="$(git -C "$CORE" rev-parse HEAD 2>/dev/null || printf 'unknown')"
    {
        printf '# Generated by scripts/syntax-check.sh --dump-flags; do not edit by hand.\n'
        printf '#\n'
        printf '# The flags one module translation unit compiles with in the real build, one per\n'
        printf '# line, with the checkout roots left as placeholders:\n'
        printf '#   @CORE@        the core checkout             (AC_CORE)\n'
        printf '#   @PLAYERBOTS@  the mod-playerbots checkout   (AC_PLAYERBOTS)\n'
        printf '#   @DEPS@        the core dependency headers   (AC_DEPS_ROOT)\n'
        printf '#\n'
        printf '# The commit below is the core these flags were taken from; scripts/mercenaries/check_mercenaries.py\n'
        printf '# fails when pins.toml moves to another core commit without this file being regenerated.\n'
        printf '# core: %s\n' "$commit"
        templated_flags "$COMPILE_COMMANDS" keep-placeholders | tr '\0' '\n'
    } > "$FLAGS_FILE"
    printf 'wrote %s (%s)\n' "${FLAGS_FILE#"$ROOT"/}" "$commit"
    exit 0
fi

[ -d "$CORE" ] || skip "no core checkout in $CORE: set AC_CORE, or clone the pinned core beside this repository"
[ -d "$PLAYERBOTS" ] || skip "no mod-playerbots checkout in $PLAYERBOTS: set AC_PLAYERBOTS"
[ -d "$DEPS/usr/include/boost" ] || [ -d "$DEPS/boost" ] || skip "no core dependency headers in $DEPS: set AC_DEPS_ROOT (see the recipe at the bottom of this file)"

if [ -f "$COMPILE_COMMANDS" ]; then
    FLAGS_SOURCE="$COMPILE_COMMANDS"
    FLAGS_NOTE="from ${COMPILE_COMMANDS#"$ROOT"/}"
else
    [ -f "$FLAGS_FILE" ] || skip "no $COMPILE_COMMANDS and no ${FLAGS_FILE#"$ROOT"/}: run scripts/syntax-check.sh --dump-flags on a configured core"
    FLAGS_SOURCE="$FLAGS_FILE"
    FLAGS_NOTE="from ${FLAGS_FILE#"$ROOT"/}"
fi

mapfile -d '' -t FLAGS < <(templated_flags "$FLAGS_SOURCE") || fail "could not read the flags from $FLAGS_SOURCE"
[ "${#FLAGS[@]}" -gt 0 ] || fail "no compile flags came out of $FLAGS_SOURCE"
# A placeholder that survives resolution would reach the compiler as a literal
# include path, and every translation unit would fail on the first core header.
for token in "${FLAGS[@]}"; do
    case "$token" in
        *@CORE@*|*@PLAYERBOTS@*|*@DEPS@*)
            fail "unresolved placeholder in the flags from $FLAGS_SOURCE: $token" ;;
    esac
done

# Named files, or every module source in the repository.
if [ "$#" -gt 0 ]; then
    SOURCES=("$@")
else
    mapfile -t SOURCES < <(find "$ROOT/modules" -mindepth 3 -maxdepth 3 -path '*/src/*.cpp' | sort)
fi

[ "${#SOURCES[@]}" -gt 0 ] || fail "no module sources under $ROOT/modules/*/src"

checked=0
failed=()
for source in "${SOURCES[@]}"; do
    [ -f "$source" ] || fail "no such source: $source"
    checked=$((checked + 1))

    # The module's own headers are added per file, so two modules may each have a
    # MercenaryMgr.h and never see each other's.
    if output=$("${FLAGS[@]}" -I"$(dirname "$source")" "$source" 2>&1); then
        printf 'ok    %s\n' "${source#"$ROOT"/}"
    else
        printf 'FAIL  %s\n' "${source#"$ROOT"/}" >&2
        printf '%s\n' "$output" | grep -E 'error:|warning:' | head -20 >&2 || printf '%s\n' "$output" | head -20 >&2
        failed+=("$source")
    fi
done

if [ "${#failed[@]}" -gt 0 ]; then
    printf '\n%d of %d translation units did not compile (%s)\n' "${#failed[@]}" "$checked" "$FLAGS_NOTE" >&2
    exit 1
fi

printf '\n%d translation units compile against the core (%s)\n' "$checked" "$FLAGS_NOTE"

# The dependency headers the core's build downloads are the only thing this
# script needs that a checkout does not have. Fetch them without root:
#
#   mkdir -p /tmp/acore-debs && cd /tmp/acore-debs
#   apt-get download libboost1.83-dev libmysqlclient-dev libargon2-dev
#   mkdir -p ~/.cache/acore-deps/root
#   for d in *.deb; do dpkg-deb -x "$d" ~/.cache/acore-deps/root; done
#
# With root, apt-get install does the same job and AC_DEPS_ROOT=/ points at it.
