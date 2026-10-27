#!/usr/bin/env bash
#
# Compile-check every module's translation units against the core's own flags.
#
# The arm64 build is the only compiler this repository has otherwise: it is a
# manual dispatch that takes most of an hour, and it caught two errors in
# mod-pawn that were both a misspelt core symbol - GetMSTime for getMSTime, and
# OnLogin for OnPlayerLogin. A -fsyntax-only pass over the module sources finds
# both in seconds, which is the difference between a typo caught at the keyboard
# and a typo caught the next morning.
#
# Nothing is built here. The flags are lifted from one recorded compile command
# in the core's compile_commands.json, so the include set, the defines and the
# C++ standard are the real build's, not a guess. A syntax pass cannot prove the
# module links or behaves, but it does prove every core symbol it names exists.
#
# Usage:
#   scripts/syntax-check.sh                 # every module under modules/
#   scripts/syntax-check.sh modules/mod-pawn/src/PawnMgr.cpp   # named files
#
# Environment:
#   AC_CORE       core checkout            (default: ../azerothcore-wotlk)
#   AC_BUILD_DIR  configured core build    (default: $AC_CORE/build)
#   AC_DEPS_ROOT  core dependency headers  (default: ~/.cache/acore-deps/root)
#
# Exits 0 when it checked everything and everything compiled, or when it could
# not check (no build, no dependency headers) - it prints why and how to fix it
# rather than failing a checkout that was never meant to have a build.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CORE="${AC_CORE:-$ROOT/../azerothcore-wotlk}"
BUILD="${AC_BUILD_DIR:-$CORE/build}"
DEPS="${AC_DEPS_ROOT:-$HOME/.cache/acore-deps/root}"
COMPILE_COMMANDS="$BUILD/compile_commands.json"

skip() {
    printf 'skip  %s\n' "$*"
    exit 0
}

fail() {
    printf 'FAIL  %s\n' "$*" >&2
    exit 1
}

[ -f "$COMPILE_COMMANDS" ] || skip "no $COMPILE_COMMANDS: configure the core first (cmake -B build -G Ninja), or set AC_BUILD_DIR"
[ -d "$DEPS/usr/include/boost" ] || skip "no core dependency headers in $DEPS: see the recipe at the bottom of this file"

# The flags of one recorded module compile, with the source and the object
# output stripped, and the dependency root pointed at this host's cache. NUL
# separated, so a define that carries quotes survives intact.
mapfile -d '' -t FLAGS < <(
    python3 - "$COMPILE_COMMANDS" "$DEPS" <<'PY'
import json, shlex, sys

path, deps = sys.argv[1], sys.argv[2]
commands = json.load(open(path))

# A module translation unit carries the module include set, which is what these
# sources need; fall back to any entry so the script still works on a build that
# was configured before the modules were added.
entries = [e for e in commands if '/modules/' in e['file']] or commands
if not entries:
    sys.exit('no compile commands in the database')

entry = entries[0]
parts = shlex.split(entry['command'])
out, skip_next = [], False
for part in parts:
    if skip_next:
        skip_next = False
        continue
    if part == '-o':
        skip_next = True
        continue
    if part == '-c':
        continue
    if part == entry['file']:
        continue
    out.append(part.replace('/tmp/acdeps/root', deps))

# -fsyntax-only ignores the object output, so the compile is a pure check.
out.append('-fsyntax-only')
sys.stdout.write('\0'.join(out))
PY
) || fail "could not derive the compile flags from $COMPILE_COMMANDS"

[ "${#FLAGS[@]}" -gt 0 ] || fail "no compile flags came out of $COMPILE_COMMANDS"

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

    # The module's own headers are included as the real build includes them, per
    # module, so two modules may each have a PawnMgr.h and never see each other's.
    if output=$("${FLAGS[@]}" -I"$(dirname "$source")" "$source" 2>&1); then
        printf 'ok    %s\n' "${source#"$ROOT"/}"
    else
        printf 'FAIL  %s\n' "${source#"$ROOT"/}" >&2
        printf '%s\n' "$output" | grep -E 'error:|warning:' | head -20 >&2 || printf '%s\n' "$output" | head -20 >&2
        failed+=("$source")
    fi
done

if [ "${#failed[@]}" -gt 0 ]; then
    printf '\n%d of %d translation units did not compile\n' "${#failed[@]}" "$checked" >&2
    exit 1
fi

printf '\n%d translation units compile against the core\n' "$checked"

# The dependency headers the core's build downloads are the only thing this
# script needs that a checkout does not have. Fetch them without root:
#
#   mkdir -p /tmp/acore-debs && cd /tmp/acore-debs
#   apt-get download libboost1.83-dev libmysqlclient-dev libargon2-dev
#   mkdir -p ~/.cache/acore-deps/root
#   for d in *.deb; do dpkg-deb -x "$d" ~/.cache/acore-deps/root; done
#
# Then point AC_DEPS_ROOT at ~/.cache/acore-deps/root (the default) or wherever
# they were extracted.
