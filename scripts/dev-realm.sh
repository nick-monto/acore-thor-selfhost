#!/usr/bin/env bash
#
# dev-realm.sh - the local x86_64 development loop for this repository's modules.
#
# The arm64 and x86_64 workflows copy modules/ into a fresh core checkout and package the result.
# Doing that on every local edit takes most of an hour, so this script drives the smaller loop: the
# repository's modules are rsynced into an already-configured host core, the host builds just the
# worldserver target, and the result is staged into the running Docker realm. It is deliberately not
# a packager: it never configures a release artifact, never builds an image, and never installs into
# the host's /opt/acore.
#
# Safety is the default: sync previews, stage previews, every mutating command prints itself before
# it runs, and nothing touches the database, the compose project's lifecycle (no down, no up
# --build) or CMake's install target.
#
# Usage: see `scripts/dev-realm.sh --help`.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

REPO_ARG=""
CORE_ARG=""
BUILD_ARG=""
CONTAINER_ARG=""
OUTPUT=""
COMMAND=""
DRY_RUN=0
YES=0
ALLOW_DIRTY=0
RESET_CONF=0
MODULES=()
ARGS=()
TEMP_FILES=()

cleanup() {
    local file
    for file in "${TEMP_FILES[@]}"; do
        [ -n "$file" ] && rm -f -- "$file"
    done
}
trap cleanup EXIT

help() {
    cat <<'EOF'
dev-realm.sh - sync, build and stage this repository's modules into the local Docker realm.

USAGE
  scripts/dev-realm.sh [OPTIONS] COMMAND [MODULE...]

COMMANDS
  status
      Read-only. For every repository module under modules/, compare it to
      $CORE/modules/<name> with diff -qr and report drift. Report the host build's
      CMAKE_BUILD_TYPE, MODULES and install prefix, the newest module source time against the
      built worldserver's mtime, the host binary's size and sha256, and the container status and
      its /opt/acore/bin/worldserver sha256. It also compares the host core and its playerbots
      clone with pins.toml. Print the exact commands that converge the state.
      Exit 0 clean, 1 drift, 2 precondition failure.

  sync [MODULE...]
      rsync -a --delete modules/<name>/ into $CORE/modules/<name>/ for the named modules (all
      repository modules when none are named). Only repository modules are allowed, and
      mod-playerbots is never touched. Dry-run by default; pass --yes to write. A requested
      module with no repository directory is refused. The --delete applies to the module's own
      directory, never to $CORE/modules itself.

  build [MODULE...]
      Verify that the selected modules' core copies match the repository unless --allow-dirty is
      given, and that the host core and its mod-playerbots clone match pins.toml. Re-run
      cmake -S $CORE -B $BUILD only when build.ninja does not reference a source
      file that exists in a synced module (or when there is no build.ninja), then run
      ninja -C $BUILD worldserver. Never passes -k 0 and never runs cmake --install.

  stage
      Strip $BUILD/src/server/apps/worldserver into a temporary file, set its rpath to
      '$ORIGIN/../lib', chmod 0755, docker cp it to /tmp/dev-realm-worldserver and docker exec mv
      it over /opt/acore/bin/worldserver. Refuses a missing, non-ELF or non-executable worldserver
      target. --output PATH writes the staged file locally instead and skips Docker. --dry-run
      prints the commands without running them.

  sql [MODULE...]
      docker cp each selected module's data/sql tree into
      /opt/acore/src/modules/<name>/data/sql, replacing the container's copy. Only modules that
      have a data/sql directory are considered. --dry-run prints the commands.

  conf [MODULE...]
      docker cp each selected module's conf/*.conf.dist into /opt/acore/etc/modules.
      --reset-conf also removes the module's live .conf so the entrypoint recreates it from
      the copied .dist on the next restart. --dry-run prints the commands.

  restart
      docker compose -f docker/docker-compose.yml restart acore, then wait up to 300 seconds for
      both /opt/acore/.state/world-ready and /opt/acore/.state/auth-ready. Never runs down, up or
      up --build.

  ship [MODULE...]
      sync --yes, build, stage, sql, conf, restart, then status. --dry-run prints every command
      (including the status invocation) and executes nothing.

  verify
      Read-only. Compare the running container binary's sha256 with --output PATH when given,
      print the container binary's rpath, check the container's world-ready/auth-ready markers,
      and query acore_world.updates for the selected modules' SQL filenames. Exit 0 when every
      check passes, 1 when a check disagrees, 2 when the container cannot be inspected.

OPTIONS
  --repo PATH        repository root (default: the directory containing this script's parent)
  --core PATH        core checkout (default: $AC_CORE, or $REPO/../azerothcore-wotlk)
  --build-dir PATH   configured core build (default: $AC_BUILD_DIR, or $CORE/build)
  --container NAME   Docker container (default: $ACORE_CONTAINER, or acore-realm-acore-1)
  --module NAME      select a module; repeatable, and equivalent to naming it positionally
  --dry-run          print every command without executing it
  --yes              allow sync to write (sync is a preview without it)
  --allow-dirty      let build proceed even when the core's module copies differ from the repo
  --reset-conf       conf: remove the live module .conf after copying its .dist
  --output PATH      stage: write the staged binary here and skip Docker (verify: compare against it)
  -h, --help         this text

EXIT STATUS
  0  success, or status found no drift
  1  module/core drift, a stale build, or a verify disagreement
  2  precondition failure (missing core, build, container, tool, module or binary)

NOTES
  * Requires bash, rsync, diff, cmake, ninja, strip, patchelf and docker on PATH (each only for the
    commands that use it).
  * The host build is RelWithDebInfo with static modules and prefix /opt/acore; the prefix is never
    installed to on the host. The container receives only the stripped worldserver binary.
  * Environment defaults: AC_CORE, AC_BUILD_DIR, ACORE_CONTAINER.
EOF
}

precondition() {
    printf 'dev-realm: %s\n' "$*" >&2
    exit 2
}

fail() {
    printf 'dev-realm: %s\n' "$*" >&2
    exit 1
}

print_cmd() {
    printf '+ '
    printf '%q ' "$@"
    printf '\n'
}

run() {
    print_cmd "$@"
    if [ "$DRY_RUN" -eq 1 ]; then
        return 0
    fi
    "$@"
}

# Read-only queries echo to stderr so their output on stdout stays parseable.
query() {
    printf '+ ' >&2
    printf '%q ' "$@" >&2
    printf '\n' >&2
    "$@"
}

require_tool() {
    [ "$DRY_RUN" -eq 1 ] && return 0
    command -v "$1" >/dev/null 2>&1 || precondition "no '$1' on PATH"
}

repo_modules() {
    local dir
    for dir in "$REPO"/modules/*/; do
        [ -d "$dir" ] || continue
        printf '%s\n' "$(basename "$dir")"
    done
}

# The host core and its playerbots clone are the two checkouts the build actually compiles. A
# mismatch with pins.toml is not a module-source drift: no amount of sync fixes it, and the
# failure surfaces later as an unrelated core/playerbots header error. Returns 0 when the pins
# agree or cannot be read (a source tarball has no .git), 1 when a checkout differs.
check_core_pins() {
    local pins="$REPO/pins.toml" core_pin pb_pin core_head pb_head rc=0
    [ -f "$pins" ] || return 0
    [ -d "$CORE/.git" ] || return 0
    core_pin="$(awk -F'"' '/^commit[[:space:]]*=/{print $2; exit}' "$pins")"
    pb_pin="$(awk -F'"' '/^\[module\.playerbots\]/{f=1; next} f && /^\[/{exit} f && /^commit[[:space:]]*=/{print $2; exit}' "$pins")"
    core_head="$(git -C "$CORE" rev-parse HEAD 2>/dev/null || true)"
    if [ -n "$core_pin" ] && [ -n "$core_head" ] && [ "$core_pin" != "$core_head" ]; then
        printf '  PIN DRIFT core       %s (pins.toml wants %s)\n' "$core_head" "$core_pin"
        rc=1
    fi
    if [ -n "$pb_pin" ] && [ -d "$CORE/modules/mod-playerbots/.git" ]; then
        pb_head="$(git -C "$CORE/modules/mod-playerbots" rev-parse HEAD 2>/dev/null || true)"
        if [ -n "$pb_head" ] && [ "$pb_pin" != "$pb_head" ]; then
            printf '  PIN DRIFT playerbots %s (pins.toml wants %s)\n' "$pb_head" "$pb_pin"
            rc=1
        fi
    fi
    return "$rc"
}

require_repo_module() {
    local name=$1
    [ -n "$name" ] || precondition "empty module name"
    [ -d "$REPO/modules/$name" ] || precondition "no repository module '$name' under $REPO/modules"
}

select_modules() {
    local -a list=()
    if [ "${#MODULES[@]}" -gt 0 ]; then
        list=("${MODULES[@]}")
    elif [ "${#ARGS[@]}" -gt 0 ]; then
        list=("${ARGS[@]}")
    else
        mapfile -t list < <(repo_modules)
    fi
    [ "${#list[@]}" -gt 0 ] || precondition "no repository modules under $REPO/modules"
    SELECTED=("${list[@]}")
}

require_container() {
    command -v docker >/dev/null 2>&1 || precondition "no docker on PATH"
    docker inspect "$CONTAINER" >/dev/null 2>&1 || precondition "container $CONTAINER does not exist"
}

container_state() {
    docker inspect -f '{{.State.Status}}' "$CONTAINER" 2>/dev/null || true
}

sha256_of() {
    sha256sum -- "$1" | awk '{print $1}'
}

is_elf() {
    local magic
    magic="$(head -c 4 -- "$1" 2>/dev/null | od -An -tx1 | tr -d ' \n')"
    [ "$magic" = "7f454c46" ]
}

human_time() {
    date -d "@$1" '+%Y-%m-%d %H:%M:%S'
}

# sha256 of what `stage` would upload: stripped, rpath rewritten. Sets STAGED_SHA and returns
# nonzero when the tools are absent, in which case status reports the container sha without
# claiming a match. Called in this shell (not a command substitution) so the temp file is cleaned up.
staged_sha() {
    local ws=$1 tmp
    command -v strip >/dev/null 2>&1 || return 1
    command -v patchelf >/dev/null 2>&1 || return 1
    tmp="$(mktemp "${TMPDIR:-/tmp}/dev-realm-status.XXXXXX")"
    TEMP_FILES+=("$tmp")
    strip --strip-unneeded -o "$tmp" "$ws" || return 1
    patchelf --set-rpath '$ORIGIN/../lib' "$tmp" || return 1
    STAGED_SHA="$(sha256_of "$tmp")"
}

# ---------------------------------------------------------------------------
# status
# ---------------------------------------------------------------------------
cmd_status() {
    [ -d "$REPO" ] || precondition "no repository at $REPO"
    [ -d "$REPO/modules" ] || precondition "no modules/ under $REPO"
    [ -d "$CORE" ] || precondition "no core checkout at $CORE (set --core or AC_CORE)"
    [ -d "$CORE/modules" ] || precondition "no $CORE/modules: the core checkout is not set up for modules"
    [ -d "$BUILD" ] || precondition "no build directory at $BUILD (set --build-dir or AC_BUILD_DIR)"
    [ -f "$BUILD/CMakeCache.txt" ] || precondition "no $BUILD/CMakeCache.txt: configure the core first"

    local drift=0 pin_drift=0 stale=0 deploy=0 name out
    local -a drifted=()

    printf '\npins (%s)\n' "$REPO/pins.toml"
    if check_core_pins; then
        printf '  core and playerbots match pins.toml\n'
    else
        pin_drift=1
        drift=1
        printf '  checkout the pinned commits before building; CI builds from pins.toml\n'
    fi

    printf '\nmodules (%s <-> %s)\n' "$REPO/modules" "$CORE/modules"
    while IFS= read -r name; do
        if [ ! -d "$CORE/modules/$name" ]; then
            printf '  missing  %s (repository has it, core does not)\n' "$name"
            drifted+=("$name")
            drift=1
            continue
        fi
        if out="$(diff -qr -- "$REPO/modules/$name" "$CORE/modules/$name" 2>&1)"; then
            printf '  clean    %s\n' "$name"
        else
            printf '  DRIFT    %s\n' "$name"
            printf '%s\n' "$out" | sed 's/^/             /'
            drifted+=("$name")
            drift=1
        fi
    done < <(repo_modules)

    local build_type modules_mode prefix
    build_type="$(sed -n 's/^CMAKE_BUILD_TYPE:[^=]*=//p' "$BUILD/CMakeCache.txt" | head -1)"
    modules_mode="$(sed -n 's/^MODULES:[^=]*=//p' "$BUILD/CMakeCache.txt" | head -1)"
    prefix="$(sed -n 's/^CMAKE_INSTALL_PREFIX:[^=]*=//p' "$BUILD/CMakeCache.txt" | head -1)"
    printf '\nhost build (%s)\n' "$BUILD"
    printf '  CMAKE_BUILD_TYPE     %s\n' "${build_type:-<unset>}"
    printf '  MODULES              %s\n' "${modules_mode:-<unset>}"
    printf '  CMAKE_INSTALL_PREFIX %s\n' "${prefix:-<unset>}"

    local ws="$BUILD/src/server/apps/worldserver"
    if [ ! -f "$ws" ]; then
        precondition "no worldserver target at $ws: run scripts/dev-realm.sh build"
    fi
    local ws_mtime newest_line newest_ts newest_path
    ws_mtime="$(stat -c %Y -- "$ws")"
    newest_line="$(find "$REPO/modules" -type f -printf '%T@ %p\n' | sort -rn | head -1)"
    newest_ts="${newest_line%% *}"
    newest_path="${newest_line#* }"
    newest_ts="${newest_ts%.*}"
    printf '  worldserver          %s, %s bytes, sha256 %s\n' \
        "$(human_time "$ws_mtime")" "$(stat -c %s -- "$ws")" "$(sha256_of "$ws")"
    if [ -n "$newest_path" ] && [ "$newest_ts" -gt "$ws_mtime" ]; then
        printf '  source               %s (%s) is newer than the binary\n' \
            "$newest_path" "$(human_time "$newest_ts")"
        stale=1
        drift=1
    elif [ -n "$newest_path" ]; then
        printf '  source               newest is %s (%s)\n' \
            "$newest_path" "$(human_time "$newest_ts")"
    else
        printf '  source               no module source is newer than the binary\n'
    fi

    printf '\ncontainer (%s)\n' "$CONTAINER"
    local state=""
    if command -v docker >/dev/null 2>&1; then
        state="$(container_state)"
    else
        printf '  docker is not on PATH: container state unknown\n'
    fi
    if [ -z "$state" ]; then
        printf '  not found: %s does not exist (compose project acore-realm)\n' "$CONTAINER"
    else
        printf '  state                %s\n' "$state"
        if [ "$state" = "running" ]; then
            local csha
            if csha="$(query docker exec "$CONTAINER" sha256sum /opt/acore/bin/worldserver 2>/dev/null | awk 'NR == 1 {print $1}')"; then
                printf '  /opt/acore/bin/worldserver sha256 %s\n' "${csha:-<unreadable>}"
                if staged_sha "$ws"; then
                    if [ -n "$csha" ] && [ "$csha" != "$STAGED_SHA" ]; then
                        printf '  differs from a staged copy of the host binary: run stage\n'
                        deploy=1
                        drift=1
                    fi
                else
                    printf '  no strip/patchelf: not comparing with the host binary\n'
                fi
            else
                printf '  could not read /opt/acore/bin/worldserver\n'
            fi
        fi
    fi

    if [ "$drift" -ne 0 ]; then
        printf '\nto converge:\n'
        if [ "$pin_drift" -ne 0 ]; then
            printf '  checkout the core and mod-playerbots commits named in pins.toml\n'
        fi
        if [ "${#drifted[@]}" -gt 0 ]; then
            printf '  scripts/dev-realm.sh sync'
            printf ' %q' "${drifted[@]}"
            printf ' --yes\n'
        fi
        if [ "$pin_drift" -eq 0 ] && { [ "$stale" -ne 0 ] || [ "${#drifted[@]}" -gt 0 ] || [ "$deploy" -ne 0 ]; }; then
            printf '  scripts/dev-realm.sh build\n'
            printf '  scripts/dev-realm.sh stage\n'
            printf '  scripts/dev-realm.sh restart\n'
        fi
        return 1
    fi
    printf '\nclean: repository modules, host build and container agree\n'
    return 0
}

# ---------------------------------------------------------------------------
# sync
# ---------------------------------------------------------------------------
cmd_sync() {
    [ -d "$CORE" ] || precondition "no core checkout at $CORE (set --core or AC_CORE)"
    [ -d "$CORE/modules" ] || precondition "no $CORE/modules: the core checkout is not set up for modules"
    select_modules
    require_tool rsync

    local name
    for name in "${SELECTED[@]}"; do
        require_repo_module "$name"
        [ "$name" != "mod-playerbots" ] || precondition "mod-playerbots is never synced by this script"
    done
    for name in "${SELECTED[@]}"; do
        if [ "$DRY_RUN" -eq 1 ] || [ "$YES" -ne 1 ]; then
            print_cmd rsync -a --delete -- "$REPO/modules/$name/" "$CORE/modules/$name/"
        else
            run rsync -a --delete -- "$REPO/modules/$name/" "$CORE/modules/$name/"
        fi
    done
    if [ "$DRY_RUN" -eq 1 ]; then
        printf 'dry-run: no files changed\n'
    elif [ "$YES" -ne 1 ]; then
        printf 'preview only: pass --yes to write\n'
    fi
    return 0
}

# ---------------------------------------------------------------------------
# build
# ---------------------------------------------------------------------------
module_needs_configure() {
    local name=$1 file rel
    [ -f "$BUILD/build.ninja" ] || return 0
    while IFS= read -r -d '' file; do
        rel="${file#"$CORE"/}"
        grep -Fq -- "$file" "$BUILD/build.ninja" && continue
        grep -Fq -- "$rel" "$BUILD/build.ninja" && continue
        return 0
    done < <(find "$CORE/modules/$name" -type f -name '*.cpp' -print0 2>/dev/null)
    return 1
}

cmd_build() {
    [ -d "$CORE" ] || precondition "no core checkout at $CORE (set --core or AC_CORE)"
    [ -d "$CORE/modules" ] || precondition "no $CORE/modules: the core checkout is not set up for modules"
    if ! check_core_pins; then
        if [ "$DRY_RUN" -eq 1 ]; then
            printf 'dev-realm: pin mismatch would block the real build\n' >&2
        else
            fail "the host core or its mod-playerbots clone does not match pins.toml; check out the pinned commits (the CI workflows do this) or build the artifact there"
        fi
    fi
    select_modules
    require_tool cmake
    require_tool ninja

    local name dirty=0
    for name in "${SELECTED[@]}"; do
        require_repo_module "$name"
        [ "$name" != "mod-playerbots" ] || precondition "mod-playerbots is not built by this script"
        if [ ! -d "$CORE/modules/$name" ]; then
            printf 'dev-realm: no core copy of %s under %s\n' "$name" "$CORE/modules" >&2
            dirty=1
        elif ! diff -qr -- "$REPO/modules/$name" "$CORE/modules/$name" >/dev/null 2>&1; then
            printf 'dev-realm: core copy of %s differs from the repository\n' "$name" >&2
            dirty=1
        fi
    done
    if [ "$dirty" -ne 0 ]; then
        [ "$ALLOW_DIRTY" -eq 1 ] || fail "run scripts/dev-realm.sh sync --yes first, or pass --allow-dirty"
        printf 'dev-realm: building a dirty core copy because --allow-dirty was given\n' >&2
    fi

    local configure=0
    for name in "${SELECTED[@]}"; do
        if module_needs_configure "$name"; then
            configure=1
            break
        fi
    done
    if [ "$configure" -eq 1 ]; then
        run cmake -S "$CORE" -B "$BUILD"
    else
        printf 'build.ninja already references every synced module source; skipping cmake configure\n'
    fi
    run ninja -C "$BUILD" worldserver
    return 0
}

# ---------------------------------------------------------------------------
# stage
# ---------------------------------------------------------------------------
cmd_stage() {
    local ws="$BUILD/src/server/apps/worldserver"
    [ -f "$ws" ] || precondition "no worldserver target at $ws: run scripts/dev-realm.sh build"
    is_elf "$ws" || precondition "$ws is not an ELF binary; refusing to stage it"
    require_tool strip
    require_tool patchelf

    local target tmp=""
    if [ -n "$OUTPUT" ]; then
        target="$OUTPUT"
    elif [ "$DRY_RUN" -eq 1 ]; then
        target="/tmp/dev-realm-worldserver.stage"
    else
        tmp="$(mktemp "${TMPDIR:-/tmp}/dev-realm-worldserver.XXXXXX")"
        TEMP_FILES+=("$tmp")
        target="$tmp"
    fi

    run strip --strip-unneeded -o "$target" "$ws"
    run patchelf --set-rpath '$ORIGIN/../lib' "$target"
    run chmod 0755 "$target"

    if [ -n "$OUTPUT" ]; then
        if [ "$DRY_RUN" -eq 1 ]; then
            printf 'would write the staged binary to %s\n' "$OUTPUT"
        fi
        return 0
    fi

    require_tool docker
    if [ "$DRY_RUN" -eq 0 ]; then
        require_container
    fi
    run docker cp "$target" "$CONTAINER:/tmp/dev-realm-worldserver"
    run docker exec "$CONTAINER" mv -f /tmp/dev-realm-worldserver /opt/acore/bin/worldserver
    return 0
}

# ---------------------------------------------------------------------------
# sql / conf
# ---------------------------------------------------------------------------
cmd_sql() {
    select_modules
    require_tool docker

    local -a with_sql=()
    local name
    for name in "${SELECTED[@]}"; do
        require_repo_module "$name"
        if [ -d "$REPO/modules/$name/data/sql" ]; then
            with_sql+=("$name")
        elif [ "${#MODULES[@]}" -gt 0 ] || [ "${#ARGS[@]}" -gt 0 ]; then
            precondition "module $name has no data/sql directory"
        fi
    done
    [ "${#with_sql[@]}" -gt 0 ] || {
        printf 'no selected module ships a data/sql tree\n'
        return 0
    }
    if [ "$DRY_RUN" -eq 0 ]; then
        require_container
    fi
    for name in "${with_sql[@]}"; do
        run docker exec "$CONTAINER" mkdir -p "/opt/acore/src/modules/$name/data"
        run docker exec "$CONTAINER" rm -rf "/opt/acore/src/modules/$name/data/sql"
        run docker cp "$REPO/modules/$name/data/sql" "$CONTAINER:/opt/acore/src/modules/$name/data/sql"
    done
    return 0
}

cmd_conf() {
    select_modules
    require_tool docker

    local -a with_conf=()
    local name
    for name in "${SELECTED[@]}"; do
        require_repo_module "$name"
        if [ -d "$REPO/modules/$name/conf" ] && \
           find "$REPO/modules/$name/conf" -maxdepth 1 -type f -name '*.conf.dist' -print -quit | grep -q .; then
            with_conf+=("$name")
        elif [ "${#MODULES[@]}" -gt 0 ] || [ "${#ARGS[@]}" -gt 0 ]; then
            precondition "module $name has no conf/*.conf.dist"
        fi
    done
    [ "${#with_conf[@]}" -gt 0 ] || {
        printf 'no selected module ships a conf/*.conf.dist\n'
        return 0
    }
    if [ "$DRY_RUN" -eq 0 ]; then
        require_container
    fi
    run docker exec "$CONTAINER" mkdir -p /opt/acore/etc/modules
    local -a confs=()
    for name in "${with_conf[@]}"; do
        mapfile -t confs < <(find "$REPO/modules/$name/conf" -maxdepth 1 -type f -name '*.conf.dist' | sort)
        run docker cp "${confs[@]}" "$CONTAINER:/opt/acore/etc/modules/"
        if [ "$RESET_CONF" -eq 1 ]; then
            run docker exec "$CONTAINER" rm -f \
                "/opt/acore/etc/modules/$name.conf" \
                "/opt/acore/etc/modules/${name//-/_}.conf"
        fi
    done
    return 0
}

# ---------------------------------------------------------------------------
# restart
# ---------------------------------------------------------------------------
cmd_restart() {
    local compose="$REPO/docker/docker-compose.yml"
    [ -f "$compose" ] || precondition "no compose file at $compose"
    require_tool docker
    if [ "$DRY_RUN" -eq 0 ]; then
        require_container
    fi
    run docker compose -f "$compose" restart acore
    if [ "$DRY_RUN" -eq 1 ]; then
        printf 'would wait up to 300s for /opt/acore/.state/world-ready and /opt/acore/.state/auth-ready\n'
        return 0
    fi
    printf 'waiting up to 300s for the realm to report ready...\n'
    local deadline=$((SECONDS + 300))
    while [ "$SECONDS" -lt "$deadline" ]; do
        if docker exec "$CONTAINER" test -f /opt/acore/.state/world-ready 2>/dev/null && \
           docker exec "$CONTAINER" test -f /opt/acore/.state/auth-ready 2>/dev/null; then
            printf 'realm ready\n'
            return 0
        fi
        sleep 2
    done
    fail "the realm did not report ready within 300s (see docker compose -f $compose logs acore)"
}

# ---------------------------------------------------------------------------
# ship
# ---------------------------------------------------------------------------
cmd_ship() {
    local saved_yes=$YES
    YES=1
    cmd_sync
    YES=$saved_yes
    cmd_build
    cmd_stage
    cmd_sql
    cmd_conf
    cmd_restart
    if [ "$DRY_RUN" -eq 1 ]; then
        print_cmd scripts/dev-realm.sh status
        return 0
    fi
    cmd_status
}

# ---------------------------------------------------------------------------
# verify
# ---------------------------------------------------------------------------
cmd_verify() {
    require_tool docker
    require_container
    [ "$(container_state)" = "running" ] || precondition "container $CONTAINER is not running"
    command -v patchelf >/dev/null 2>&1 || precondition "no patchelf on PATH"
    [ -d "$REPO/modules" ] || precondition "no modules/ under $REPO"

    local staged="${OUTPUT:-${ARGS[0]:-}}"
    local rc=0
    if [ -n "$staged" ]; then
        [ -f "$staged" ] || precondition "no staged file at $staged"
    fi

    local csha
    if ! csha="$(query docker exec "$CONTAINER" sha256sum /opt/acore/bin/worldserver | awk 'NR == 1 {print $1}')"; then
        precondition "could not read /opt/acore/bin/worldserver from $CONTAINER"
    fi
    printf 'container binary sha256   %s\n' "${csha:-<unreadable>}"
    if [ -n "$staged" ]; then
        local hsha
        hsha="$(sha256_of "$staged")"
        printf 'staged binary sha256      %s\n' "$hsha"
        if [ "$csha" = "$hsha" ]; then
            printf 'binary                    matches\n'
        else
            printf 'binary                    DIFFERS\n'
            rc=1
        fi
    fi

    local tmp
    tmp="$(mktemp "${TMPDIR:-/tmp}/dev-realm-verify.XXXXXX")"
    TEMP_FILES+=("$tmp")
    if query docker cp "$CONTAINER:/opt/acore/bin/worldserver" "$tmp" >/dev/null 2>&1; then
        local rpath
        rpath="$(patchelf --print-rpath "$tmp" 2>/dev/null || true)"
        printf 'container binary rpath    %s\n' "${rpath:-<none>}"
        if [ "$rpath" != '$ORIGIN/../lib' ]; then
            printf 'rpath                     expected $ORIGIN/../lib\n'
            rc=1
        fi
    else
        printf 'container binary          could not be copied out of the container\n'
        rc=1
    fi

    local marker missing=0
    for marker in /opt/acore/.state/world-ready /opt/acore/.state/auth-ready; do
        if docker exec "$CONTAINER" test -f "$marker" 2>/dev/null; then
            printf 'marker                    %s present\n' "$marker"
        else
            printf 'marker                    %s MISSING\n' "$marker"
            missing=1
            rc=1
        fi
    done

    local rows=""
    local mysql_cmd='MYSQL_PWD="$DB_PASS" mysql --protocol=TCP -h "$DB_HOST" -P "$DB_PORT" -u "$DB_USER" -N -B -e "SELECT name, state FROM acore_world.updates"'
    if rows="$(query docker exec "$CONTAINER" sh -c "$mysql_cmd" 2>/dev/null)"; then
        local -a bases=()
        mapfile -t bases < <(find "$REPO/modules" -type f -path '*/data/sql/*' -name '*.sql' -printf '%f\n' | sort -u)
        local base state found=0 pending=0 absent=0
        for base in "${bases[@]}"; do
            state="$(printf '%s\n' "$rows" | awk -F $'\t' -v n="$base" '$1 == n { print $2; exit }')"
            if [ -z "$state" ]; then
                absent=$((absent + 1))
            elif [ "$state" != "MODULE" ]; then
                printf 'sql                       %s state %s\n' "$base" "$state"
                pending=$((pending + 1))
            else
                found=$((found + 1))
            fi
        done
        printf 'module sql rows           %d applied, %d not in updates, %d not MODULE\n' \
            "$found" "$absent" "$pending"
        if [ "$absent" -ne 0 ] || [ "$pending" -ne 0 ]; then
            rc=1
        fi
    else
        printf 'module sql rows           could not query acore_world.updates\n'
        rc=1
    fi

    return "$rc"
}

# ---------------------------------------------------------------------------
# argument parsing
# ---------------------------------------------------------------------------
while [ "$#" -gt 0 ]; do
    case "$1" in
        --repo)
            [ "$#" -ge 2 ] || precondition "--repo needs a path"
            REPO_ARG="$2"
            shift 2
            ;;
        --core)
            [ "$#" -ge 2 ] || precondition "--core needs a path"
            CORE_ARG="$2"
            shift 2
            ;;
        --build-dir)
            [ "$#" -ge 2 ] || precondition "--build-dir needs a path"
            BUILD_ARG="$2"
            shift 2
            ;;
        --container)
            [ "$#" -ge 2 ] || precondition "--container needs a name"
            CONTAINER_ARG="$2"
            shift 2
            ;;
        --module)
            [ "$#" -ge 2 ] || precondition "--module needs a name"
            MODULES+=("$2")
            shift 2
            ;;
        --output)
            [ "$#" -ge 2 ] || precondition "--output needs a path"
            OUTPUT="$2"
            shift 2
            ;;
        --dry-run)
            DRY_RUN=1
            shift
            ;;
        --yes)
            YES=1
            shift
            ;;
        --allow-dirty)
            ALLOW_DIRTY=1
            shift
            ;;
        --reset-conf)
            RESET_CONF=1
            shift
            ;;
        -h|--help)
            help
            exit 0
            ;;
        --)
            shift
            while [ "$#" -gt 0 ]; do
                if [ -z "$COMMAND" ]; then
                    COMMAND="$1"
                else
                    ARGS+=("$1")
                fi
                shift
            done
            ;;
        -*)
            precondition "unknown option: $1 (try --help)"
            ;;
        *)
            if [ -z "$COMMAND" ]; then
                COMMAND="$1"
            else
                ARGS+=("$1")
            fi
            shift
            ;;
    esac
done

REPO="${REPO_ARG:-$ROOT}"
CORE="${CORE_ARG:-${AC_CORE:-$REPO/../azerothcore-wotlk}}"
BUILD="${BUILD_ARG:-${AC_BUILD_DIR:-$CORE/build}}"
CONTAINER="${CONTAINER_ARG:-${ACORE_CONTAINER:-acore-realm-acore-1}}"
REPO="$(realpath -m -- "$REPO")"
CORE="$(realpath -m -- "$CORE")"
BUILD="$(realpath -m -- "$BUILD")"

[ -n "$COMMAND" ] || precondition "no command given (try --help)"

case "$COMMAND" in
    status)   cmd_status ;;
    sync)     cmd_sync ;;
    build)    cmd_build ;;
    stage)    cmd_stage ;;
    sql)      cmd_sql ;;
    conf)     cmd_conf ;;
    restart)  cmd_restart ;;
    ship)     cmd_ship ;;
    verify)   cmd_verify ;;
    *)        precondition "unknown command: $COMMAND (try --help)" ;;
esac
