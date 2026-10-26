#!/usr/bin/env bash
#
# thor-install.sh - install, repair, inspect and remove the AzerothCore + mod-playerbots
#                   server for an AYN Thor (Snapdragon 8 Gen 2, Android/Termux, aarch64).
#
# Run it inside Termux as a normal user:
#
#     bash thor-install.sh                 # install (see below)
#     bash thor-install.sh --check         # report what install would do; change nothing
#     bash thor-install.sh --status        # inspect an existing install (exit 1 if broken)
#     bash thor-install.sh --repair        # fix what it can, then re-check
#     bash thor-install.sh --uninstall     # stop, optionally dump the database, remove
#
# Everything is encapsulated: the rootfs is a plain directory, and every file this script
# creates outside it is listed in a manifest, so removal is exact rather than "grep and hope".
#
#   <TERMUX_PREFIX>/var/lib/proot-distro/...   the Unix side (--purge-rootfs). proot-distro keeps that
#                                              filesystem either in containers/ubuntu/rootfs or, on
#                                              older installations, in installed-rootfs/ubuntu, so the
#                                              script probes for it - see THOR_ROOTFS_DIR below
#     /opt/acore           server: bin etc data src logs db run .thor-state   <- --root
#     /opt/thor            this script's copy
#   $HOME/.thor-wow        host state + manifest + database dumps
#   $HOME/start-wow-server.sh, stop-wow-server.sh
#
# MySQL's data directory lives in <root>/db and its socket in <root>/run, so a removal does
# not leave a database behind in /var/lib/mysql.
#
# Description of every step: ../docs/RUNBOOK.md.  Pinned versions: ../pins.toml.
#
# Flags:
#   --check                 report what would happen; change nothing
#   --status                inspect the install and exit non-zero if it needs repair
#   --repair                check every component and fix what is broken
#   --uninstall             remove the install (dumps the database with --keep-db)
#   --purge-rootfs          with --uninstall: also remove the whole Ubuntu rootfs
#   --keep-db               with --uninstall: dump the databases before removing
#   --from-source           build the server in the rootfs instead of downloading the artifact
#   --force                 re-download / overwrite existing server files
#   --bots N                random bots to enable (default 50)
#   --db-user NAME          MySQL user                (default acore)
#   --db-pass PASS          MySQL password            (default: generated)
#   --account NAME          GM account                (default admin)
#   --account-pass PASS     GM password               (default: password)
#   --lan-ip IP             bind to the LAN and point the realm at IP (default: loopback only)
#   --root DIR              install root in the rootfs (default /opt/acore)
#   --release-url URL       prebuilt linux/arm64 tarball
#   --github-token TOKEN    token for a private repository (or set THOR_GH_TOKEN); also used
#                           to fetch the server artifact and the script itself when private
#   --data-zip URL|PATH|none  client data (dbc/maps/vmaps/mmaps)
#   --boot-timeout SECS     first-boot wait for "World initialized" (default 1800)
#   THOR_ROOTFS_DIR DIR     the rootfs directory, if proot-distro stores it somewhere this script
#                           does not probe - a wrong rootfs puts the script's copy where the guest
#                           cannot see it
#   -h, --help              this text
#
# Note: the install root is $ROOT, not $PREFIX, because Termux already uses $PREFIX.
#
set -euo pipefail

SCRIPT_VERSION="2"
CORE_REPO="https://github.com/mod-playerbots/azerothcore-wotlk"
CORE_COMMIT="8a4e76eac8c4ccebf547113afe7b51d6c3e91641"
PB_REPO="https://github.com/mod-playerbots/mod-playerbots"
PB_COMMIT="d750b4b3a50effdf22bedc8032f0e8e5f7cb8091"
# mod-creature-gear, mod-hardcore, mod-e-interact, mod-survivor and mod-pawn live in *this*
# repository
# (modules/), not in repositories of their own, so --from-source has to fetch this one's tarball as
# well when the script is installed from a copy of the repo. --github-token (THOR_GH_TOKEN) is what
# makes that work while private.
SELF_REPO="nick-monto/acore-thor-selfhost"
SELF_REF="${SELF_REF:-main}"
RELEASE_URL="${THOR_RELEASE_URL:-https://github.com/nick-monto/acore-thor-selfhost/releases/latest/download/acore-server-arm64.tar.zst}"
GH_TOKEN_OPT="${THOR_GH_TOKEN:-}"
DATA_URL="https://github.com/wowgaming/client-data/releases/download/v20.0/data.zip"
DATA_SHA256="a3d4df635ae6c2c8f08052c32a79e0f806955150ad36b014a823dd08a32a4610"
ROOT_DEFAULT="/opt/acore"
ROOTFS_NAME="ubuntu"
TERMUX_PREFIX="${TERMUX_PREFIX:-/data/data/com.termux/files/usr}"
POLICY_RC_D="${POLICY_RC_D:-/usr/sbin/policy-rc.d}"     # overridable for tests
HOST_STATE="${THOR_HOST_STATE:-$HOME/.thor-wow}"        # overridable for tests
DB_PROBE_TIMEOUT="${DB_PROBE_TIMEOUT:-30}"              # seconds a write probe may take (tests shorten it)

ROOT="$ROOT_DEFAULT"
BOTS=50
# Solo default: only a handful of the random bots stay in the real player's zone; the rest keep the
# normal world-wide distribution, so the total population is unchanged without crowding the player.
# The zone group also churns: bots visit for 15-40 minutes, then leave for 4-10 and come back.
NEAR_BOTS=10
NEAR_MIN_DISTANCE=100.0
NEAR_VISIT_MIN=900
NEAR_VISIT_MAX=2400
NEAR_AWAY_MIN=240
NEAR_AWAY_MAX=600
# mod-survivor's conf keys with the values its shipped .dist carries, as KEY=DEFAULT. The module
# reads all eleven once at load, so a key that is absent or outside its range is silently replaced
# by the module's own fallback; write_configs() and run_checks() restore them from this one list.
SURVIVOR_CONF_DEFAULTS=(
    "Survivor.Enable=1"
    "Survivor.MinLevelToFlag=10"
    "Survivor.AnnounceDeaths=1"
    "Survivor.DropEquipped=1"
    "Survivor.DropBags=1"
    "Survivor.ContainerLifetimeDays=7"
    "Survivor.MarkOnMap=1"
    "Survivor.ContainerEntry=5000000"
    "Survivor.ApplyToBots=1"
    "Survivor.BotRegenOnDeath=1"
    "Survivor.BotDropContainer=1"
)
# mod-pawn's conf keys with the values its shipped .dist carries, as KEY=DEFAULT. Same contract as
# the survivor list: a key outside its range is silently replaced by the module's own fallback, so
# write_configs() restores it from here.
PAWN_CONF_DEFAULTS=(
    "Pawn.Enable=1"
    "Pawn.MaxPawns=2"
    "Pawn.LevelWindow=2"
    "Pawn.OfferedPerPage=12"
    "Pawn.ObeliskEntry=5000010"
    "Pawn.Debug=0"
)
DB_USER="acore"
DB_PASS=""
ACCOUNT="admin"
ACCOUNT_PASS=""
LAN_IP=""
DATA_SRC="$DATA_URL"
BOOT_TIMEOUT=1800
MODE="host"            # host | inside
ACTION="install"       # install | repair | uninstall | status
CHECK=0
FROM_SOURCE=0
FORCE=0
KEEP_DB=0
PURGE_ROOTFS=0

BIN_DIR=""
CONF_DIR=""
DATA_DIR=""
SRC_DIR=""
ARTIFACT_SHA=""
PROBLEMS=0
FIXES=0
FIRST_BOOT_OK=1

ORIGINAL_ARGS=("$@")
SCRIPT_PATH="$(readlink -f "${BASH_SOURCE[0]:-$0}")"

# ---------------------------------------------------------------- output helpers

log()  { printf '%s\n' "$*"; }
step() { printf '\n=== %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }
note() { if [ "$CHECK" = 1 ]; then printf '  would: %s\n' "$*"; else "$@"; fi; }
ok()   { printf 'ok    %s\n' "$*"; }
bad()  { printf 'FAIL  %s\n' "$*"; PROBLEMS=$((PROBLEMS + 1)); }

usage() { awk 'NR > 2 { if ($0 !~ /^#/) exit; sub(/^# ?/, ""); print }' "$SCRIPT_PATH"; }

# ---------------------------------------------------------------- args

parse_args() {
    while [ $# -gt 0 ]; do
        case "$1" in
            --check)        CHECK=1 ;;
            --status)       ACTION="status" ;;
            --repair)       ACTION="repair" ;;
            --uninstall)    ACTION="uninstall" ;;
            --install)      ACTION="install" ;;
            --purge-rootfs) PURGE_ROOTFS=1 ;;
            --keep-db)      KEEP_DB=1 ;;
            --from-source)  FROM_SOURCE=1 ;;
            --force)        FORCE=1 ;;
            --inside)       MODE="inside" ;;
            --bots)         BOTS="${2:?--bots needs a value}"; shift ;;
            --db-user)      DB_USER="${2:?}"; shift ;;
            --db-pass)      DB_PASS="${2:?}"; shift ;;
            --account)      ACCOUNT="${2:?}"; shift ;;
            --account-pass) ACCOUNT_PASS="${2:?}"; shift ;;
            --lan-ip)       LAN_IP="${2:?}"; shift ;;
            --root)         ROOT="${2:?}"; shift ;;
            --release-url)  RELEASE_URL="${2:?}"; shift ;;
            --github-token) GH_TOKEN_OPT="${2:?}"; shift ;;
            --data-zip)     DATA_SRC="${2:?}"; shift ;;
            --boot-timeout) BOOT_TIMEOUT="${2:?}"; shift ;;
            -h|--help)      usage; exit 0 ;;
            *)              die "unknown argument: $1 (try --help)" ;;
        esac
        shift
    done
    case "$BOTS" in ''|*[!0-9]*) die "--bots must be a number" ;; esac
    case "$BOOT_TIMEOUT" in ''|*[!0-9]*) die "--boot-timeout must be a number" ;; esac
    case "$ROOT" in /*) ;; *) die "--root must be an absolute path" ;; esac
    case "$ROOT" in /|/opt|/usr|/home|/etc|/var) die "--root must be a private directory, not $ROOT" ;; esac
    refresh_paths
}

# ---------------------------------------------------------------- generic helpers

random_password() {
    # head closes this pipe after 16 bytes, so tr dies of SIGPIPE with 141 - and under pipefail that
    # 141 is the substitution's status, which fails the assignment and, with set -e, ends the script
    # with no output whatsoever. "The install exits at Databases and user" was this: generating the
    # passwords is the first thing create_schema does after printing its header.
    ( set +o pipefail; LC_ALL=C tr -dc 'A-Za-z0-9' < /dev/urandom | head -c 16 )
}

# The credentials the install uses when none were given. Called from main() rather than from
# create_schema(), because a resumed install skips that step and first_boot still needs the GM
# password to create the account. The database user gets a generated password (it is only ever
# read by the servers); the GM account defaults to password so the client login is predictable.
default_credentials() {
    if [ -z "$DB_PASS" ]; then DB_PASS="$(random_password)"; fi
    if [ -z "$ACCOUNT_PASS" ]; then ACCOUNT_PASS="password"; fi
}

sha256_file() { sha256sum "$1" | awk '{print $1}'; }

curl_gh() {   # curl_gh ARGS... - adds the token when one is set, i.e. for a private repository
    if [ -n "$GH_TOKEN_OPT" ]; then
        curl -H "Authorization: Bearer $GH_TOKEN_OPT" -H "Accept: application/octet-stream" "$@"
    else
        curl "$@"
    fi
}

human_size() { du -sh "$1" 2>/dev/null | awk '{print $1}' || true; }

baked_conf_dir() { grep -aoE '/[A-Za-z0-9_./-]+/etc' "$1" 2>/dev/null | head -1 || true; }

missing_libs() {   # missing_libs BINARY - runtime libraries the binary cannot find
    have ldd || return 0
    ldd "$1" 2>/dev/null | awk '/not found/ {print $1}' | sort -u
}

set_conf() {   # set_conf FILE KEY VALUE - replace the first live "KEY = ..." line, else append
    local file="$1" key="$2" value="$3" tmp
    [ -f "$file" ] || { warn "no such config file: $file"; return 1; }
    tmp="$(mktemp "${file}.XXXXXX")"
    awk -v k="$key" -v v="$value" '
        { line = $0
          if (!done && line !~ /^[[:space:]]*#/ && line ~ /=/) {
              key = line; sub(/[[:space:]]*=.*/, "", key)
              gsub(/^[[:space:]]+|[[:space:]]+$/, "", key)
              if (key == k) { print key " = " v; done = 1; next }
          }
          print
        }
        END { if (!done) print k " = " v }
    ' "$file" > "$tmp"
    cat "$tmp" > "$file"       # same inode, so owner and permissions survive
    rm -f "$tmp"
}

conf_get() {   # conf_get FILE KEY - print the live value (last uncommented definition)
    awk -v want="$2" '
        $0 !~ /^[[:space:]]*#/ && $0 ~ /=/ {
            key = $0; sub(/[[:space:]]*=.*/, "", key)
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", key)
            if (key == want) { val = $0; sub(/^[^=]*=[[:space:]]*/, "", val); found = val }
        }
        END { if (found != "") print found }
    ' "$1"
}

conf_get_unquoted() { conf_get "$1" "$2" | tr -d '"'; }

pawn_value_ok() {   # pawn_value_ok KEY VALUE - true when VALUE is one mod-pawn accepts
    # Pawn.Enable and Pawn.Debug are 0|1, MaxPawns 0-4 (the gossip page has room for four party
    # rows), LevelWindow 0-10, OfferedPerPage 1-14, and the obelisk is a custom entry in the
    # 5000000 range the custom SQL owns.
    local key="$1" val="$2"
    case "$val" in ''|*[!0-9]*) return 1 ;; esac
    case "$key" in
        Pawn.Enable|Pawn.Debug)
            [ "$val" -eq 0 ] || [ "$val" -eq 1 ] ;;
        Pawn.MaxPawns)
            [ "$val" -ge 0 ] && [ "$val" -le 4 ] ;;
        Pawn.LevelWindow)
            [ "$val" -ge 0 ] && [ "$val" -le 10 ] ;;
        Pawn.OfferedPerPage)
            [ "$val" -ge 1 ] && [ "$val" -le 14 ] ;;
        Pawn.ObeliskEntry)
            [ "$val" -ge 5000000 ] && [ "$val" -le 5099999 ] ;;
        *)
            return 1 ;;
    esac
}

survivor_value_ok() {   # survivor_value_ok KEY VALUE - true when VALUE is one the module accepts
    # The ranges are the ones mod-survivor's own conf comments state: the 0|1 switches, a minimum
    # flag level inside the level cap, a container lifetime in days, and a positive template entry.
    local key="$1" val="$2"
    case "$val" in ''|*[!0-9]*) return 1 ;; esac
    case "$key" in
        Survivor.Enable|Survivor.AnnounceDeaths|Survivor.DropEquipped|Survivor.DropBags|\
        Survivor.MarkOnMap|Survivor.ApplyToBots|Survivor.BotRegenOnDeath|Survivor.BotDropContainer)
            [ "$val" -eq 0 ] || [ "$val" -eq 1 ] ;;
        Survivor.MinLevelToFlag)
            [ "$val" -ge 1 ] && [ "$val" -le 80 ] ;;
        Survivor.ContainerLifetimeDays)
            [ "$val" -ge 1 ] && [ "$val" -le 365 ] ;;
        Survivor.ContainerEntry)
            [ "$val" -gt 0 ] ;;
        *) return 1 ;;
    esac
}

# ---------------------------------------------------------------- state and manifest

INSIDE_SCRIPT_COPY="/opt/thor/thor-install.sh"

# proot-distro has kept the guest filesystem in more than one place. The newer layout is
# containers/<name>/rootfs with a manifest beside it; older installations put it straight in
# installed-rootfs/<name>, and a device that has been through the change can carry both, the old one
# left behind. Guessing is not harmless: a wrong rootfs is still a directory, so the install runs to
# the point where the guest's own bash reports "No such file or directory" for the script's copy.
# So probe for a directory that looks like a filesystem, and let THOR_ROOTFS_DIR settle it if a
# future proot-distro moves things again.
rootfs_candidates() {
    printf '%s\n' \
        "${THOR_ROOTFS_DIR:-}" \
        "$TERMUX_PREFIX/var/lib/proot-distro/containers/$ROOTFS_NAME/rootfs" \
        "$TERMUX_PREFIX/var/lib/proot-distro/installed-rootfs/$ROOTFS_NAME/rootfs" \
        "$TERMUX_PREFIX/var/lib/proot-distro/installed-rootfs/$ROOTFS_NAME"
}

resolve_rootfs() {
    local candidate
    while read -r candidate; do
        [ -n "$candidate" ] || continue
        if [ -d "$candidate" ]; then ROOTFS="$candidate"; return 0; fi
    done < <(rootfs_candidates)
    # Not installed yet: follow the layout this proot-distro uses, so that the directory is found
    # again after "proot-distro install".
    if [ -n "${THOR_ROOTFS_DIR:-}" ]; then
        ROOTFS="$THOR_ROOTFS_DIR"
    elif [ -d "$TERMUX_PREFIX/var/lib/proot-distro/containers" ]; then
        ROOTFS="$TERMUX_PREFIX/var/lib/proot-distro/containers/$ROOTFS_NAME/rootfs"
    else
        ROOTFS="$TERMUX_PREFIX/var/lib/proot-distro/installed-rootfs/$ROOTFS_NAME"
    fi
}

# Paths derived from ROOT. refresh_paths() is called again whenever --root or the state file
# changes ROOT, so --root DIR really moves the database, the socket and the state with it.
refresh_paths() {
    resolve_rootfs
    DB_DIR="$ROOT/db"
    RUN_DIR="$ROOT/run"
    SOCKET="$RUN_DIR/mysqld.sock"
    STATE_DIR="$ROOT/.thor-state"
}
refresh_paths

state_root() {   # where this stage keeps its state and manifest
    if [ "$MODE" = "host" ]; then printf '%s' "$HOST_STATE"; else printf '%s' "$STATE_DIR"; fi
}

state_file() { printf '%s/state.env' "$(state_root)"; }

state_save() {
    local file; file="$(state_file)"
    if [ "$CHECK" = 1 ]; then note "record install state in $file"; return 0; fi
    mkdir -p "$(dirname "$file")"
    umask 077
    cat > "$file" <<EOF
# Written by thor-install.sh $SCRIPT_VERSION on $(date -u +%Y-%m-%dT%H:%M:%SZ)
# Sourced by --status, --repair and --uninstall; edit only if you know what you are doing.
ROOT="$ROOT"
ROOTFS="$ROOTFS"
BIN_DIR="$BIN_DIR"
CONF_DIR="$CONF_DIR"
DATA_DIR="$DATA_DIR"
SRC_DIR="$SRC_DIR"
DB_USER="$DB_USER"
DB_PASS="$DB_PASS"
ACCOUNT="$ACCOUNT"
ACCOUNT_PASS="$ACCOUNT_PASS"
BOTS="$BOTS"
LAN_IP="$LAN_IP"
DATA_SRC="$DATA_SRC"
GH_TOKEN_OPT="$GH_TOKEN_OPT"
ARTIFACT_SHA="$ARTIFACT_SHA"
SCRIPT_VERSION="$SCRIPT_VERSION"
EOF
    umask 022
    chmod 600 "$file"
}

state_load() {
    local file; file="$(state_file)"
    if [ ! -f "$file" ]; then
        if [ "$ACTION" = "install" ]; then return 0; fi
        warn "no state at $file - nothing installed here?"
        return 1
    fi
    # shellcheck disable=SC1090  # generated by state_save above
    . "$file"
    refresh_paths
    return 0
}

state_apply_cli() {   # CLI flags win over the state file
    local saved=("${ORIGINAL_ARGS[@]:-}")
    [ "${#saved[@]}" -eq 0 ] && return 0
    parse_args "${saved[@]}" 2>/dev/null || true
}

manifest_add() {   # paths outside $ROOT created by this script
    if [ "$CHECK" = 1 ]; then note "record $1 in the manifest"; return 0; fi
    mkdir -p "$(state_root)"
    printf '%s\n' "$1" >> "$(state_root)/manifest.txt"
}

manifest_paths() {
    local file; file="$(state_root)/manifest.txt"
    [ -f "$file" ] && sort -u "$file"
}

safe_remove() {   # never remove something that is not clearly ours
    local path="$1" base="${2:-}"
    case "$path" in /*) ;; *) warn "refusing to remove relative path: $path"; return 1 ;; esac
    case "$path" in /|/opt|/usr|/etc|/var|/home|/data|"$TERMUX_PREFIX") warn "refusing to remove $path"; return 1 ;; esac
    if [ -n "$base" ]; then
        case "$path" in "$base"/*) ;; *) warn "refusing to remove $path (outside $base)"; return 1 ;; esac
    fi
    rm -rf "$path"
}

manifest_remove_all() {
    local p removed=0
    while IFS= read -r p; do
        [ -n "$p" ] || continue
        if safe_remove "$p"; then log "removed $p"; removed=$((removed + 1)); fi
    done < <(manifest_paths)
    if [ "$removed" = 0 ]; then log "nothing outside the install root was recorded"; fi
}

proot_login() {   # proot_login ARGS... - never fatal: the caller decides what a failure means
    if [ "$CHECK" = 1 ]; then note proot-distro login "$@"; return 0; fi
    if ! have proot-distro; then warn "proot-distro is not installed; skipping: proot-distro login $*"; return 1; fi
    proot-distro login "$@"
}

guest_has_path() {   # guest_has_path PATH - does the guest that "login" starts actually see PATH?
    if [ "$CHECK" = 1 ]; then note "test -f $1 inside the rootfs"; return 0; fi
    have proot-distro || return 0        # without it there is no guest to ask; the caller already warned
    proot-distro login "$ROOTFS_NAME" -- test -f "$1"
}

die_path_invisible() {   # die_path_invisible PATH - the rootfs on disk is not the one being booted
    die "$1 is not visible inside the '$ROOTFS_NAME' rootfs, so the filesystem proot-distro boots is
       not $ROOTFS. See what is there:
         ls -d $TERMUX_PREFIX/var/lib/proot-distro/*/*/
       then name the right directory:
         THOR_ROOTFS_DIR=<directory> bash $SCRIPT_PATH"
}

proot_remove_rootfs() {   # proot_remove_rootfs NAME
    if [ "$CHECK" = 1 ]; then note proot-distro remove "$1"; return 0; fi
    if ! have proot-distro; then warn "proot-distro is not installed; skipping: proot-distro remove $1"; return 1; fi
    proot-distro remove "$1"
}

# ---------------------------------------------------------------- mysql helpers (inside)

mysql_admin() { mysql --protocol=socket --socket="$SOCKET" -uroot "$@"; }

db_ping() { mysqladmin --protocol=socket --socket="$SOCKET" ping >/dev/null 2>&1; }

# "ping" only proves the server accepts connections. The first statement that writes goes through
# InnoDB's I/O path instead, and under proot that path has no real io_setup/io_submit, so the write
# never returns: the server still answers ping, every later step hangs behind it, and nothing is
# printed. A bounded write probe turns that into a message.
db_writable() {
    # The client is spelled out rather than called through mysql_admin(): timeout is an external
    # binary, so it execs its argument and cannot run a shell function.
    timeout "$DB_PROBE_TIMEOUT" mysql --protocol=socket --socket="$SOCKET" -uroot -e \
        "CREATE DATABASE IF NOT EXISTS _thor_probe; DROP DATABASE _thor_probe;" >/dev/null 2>&1
}

db_start() {
    mkdir -p "$RUN_DIR" "$DB_DIR" "$ROOT/logs"
    chown mysql:mysql "$DB_DIR" "$RUN_DIR"
    chown -R mysql:mysql "$DB_DIR"
    # --innodb-use-native-aio=0 throughout: proot does not implement libaio's io_setup/io_submit, so
    # with native AIO on, InnoDB's first write never completes. The server comes up, answers ping, and
    # then hangs the statement that touches a table.
    if [ ! -d "$DB_DIR/mysql" ]; then
        log "initialising the database directory $DB_DIR (first run)"
        mysqld --initialize-insecure --user=mysql --datadir="$DB_DIR" \
               --innodb-use-native-aio=0 \
               --log-error="$ROOT/logs/mysql.err" >/dev/null 2>&1 \
            || die "mysqld --initialize-insecure failed; see $ROOT/logs/mysql.err"
    fi
    if db_ping; then
        log "mysqld already running"
    else
        log "starting mysqld (datadir $DB_DIR, socket $SOCKET)"
        mysqld --user=mysql --datadir="$DB_DIR" --socket="$SOCKET" \
               --bind-address=127.0.0.1 --port=3306 --skip-name-resolve \
               --innodb-use-native-aio=0 \
               --pid-file="$RUN_DIR/mysqld.pid" --log-error="$ROOT/logs/mysql.err" --daemonize \
            || die "mysqld did not start; see $ROOT/logs/mysql.err"
        local i
        for i in $(seq 1 60); do
            db_ping && break
            if [ "$i" = 60 ]; then die "mysqld did not answer on $SOCKET; see $ROOT/logs/mysql.err"; fi
            sleep 1
        done
    fi
    # A server left over from an earlier run is reused, so check that it can actually write before the
    # install builds on it - otherwise every later step hangs behind the same wedged server.
    db_writable || die "mysqld answers on $SOCKET but a write did not finish in ${DB_PROBE_TIMEOUT}s.
       If an earlier run wedged it, stop it and re-run:
         mysqladmin --protocol=socket --socket=$SOCKET -uroot shutdown    # or: pkill -9 mysqld
       Otherwise read $ROOT/logs/mysql.err and docs/RUNBOOK.md Part 10."
}

db_stop() {
    if db_ping; then
        mysqladmin --protocol=socket --socket="$SOCKET" shutdown 2>/dev/null || true
    fi
}

db_query() { mysql_admin -N -B -e "$1" 2>/dev/null; }

db_schema_exists() { [ "$(db_query "SELECT COUNT(*) FROM information_schema.SCHEMATA WHERE SCHEMA_NAME='$1'")" = "1" ]; }

db_table_count() {
    db_query "SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA='$1'"
}

# ---------------------------------------------------------------- server console

process_gone() {   # process_gone PID - true once it has exited; a zombie counts as gone
    [ -d "/proc/$1" ] || return 0
    [ "$(awk '{print $3}' "/proc/$1/stat" 2>/dev/null)" = Z ]
}

recent_logs() {   # the 3 most recently modified log files, newest first
    # Same trap as random_password: head closes the pipe early, and pipefail would report sort's 141
    # as a failure of the check that is only trying to show log tails.
    ( set +o pipefail
      find "$ROOT/logs" -type f \( -name '*.log' -o -name '*.out' \) -printf '%T@ %p\n' 2>/dev/null \
          | sort -rn | head -3 | cut -d' ' -f2- )
}

world_console() {   # world_console TIMEOUT "console command" ...
    local timeout="$1"
    shift
    local fifo="$ROOT/.console.fifo" wslog="$ROOT/logs/worldserver.out" aslog="$ROOT/logs/authserver.out"
    rm -f "$fifo"
    mkfifo "$fifo"
    : > "$wslog"
    : > "$aslog"
    "$BIN_DIR/authserver" -c "$CONF_DIR/authserver.conf" > "$aslog" 2>&1 &
    local as_pid=$!
    "$BIN_DIR/worldserver" -c "$CONF_DIR/worldserver.conf" < "$fifo" > "$wslog" 2>&1 &
    local ws_pid=$!
    exec 9<> "$fifo"         # read-write, so this cannot block when the server failed to start
                             # (opening for write alone waits for a reader that a dead process never opens)

    local -a logs=("$wslog" "$aslog")
    mapfile -t -O 2 logs < <(recent_logs)
    log "authserver pid $as_pid, worldserver pid $ws_pid: waiting up to ${timeout}s for the world"
    local waited=0 why=""
    while [ "$waited" -lt "$timeout" ]; do
        if grep -qE 'World initialized|ready\.\.\.' "${logs[@]}" 2>/dev/null; then break; fi
        # Waiting on a process that has already died is how a first boot that failed during the SQL
        # import used to burn the whole timeout looking like an import still in progress.
        if process_gone "$ws_pid"; then why="worldserver exited before the world came up"; break; fi
        sleep 1; waited=$((waited + 1))
    done
    if [ -z "$why" ] && ! grep -qE 'World initialized|ready\.\.\.' "${logs[@]}" 2>/dev/null; then
        why="worldserver did not report 'World initialized' within ${timeout}s"
    fi
    if [ -n "$why" ]; then
        warn "$why - last lines of $wslog:"
        tail -n 40 "$wslog" >&2 || true
        if [ -s "$aslog" ]; then warn "last lines of $aslog:"; tail -n 20 "$aslog" >&2 || true; fi
        exec 9>&-
        kill "$ws_pid" "$as_pid" 2>/dev/null || true
        rm -f "$fifo"
        return 1
    fi
    log "world is up"
    local cmd
    for cmd in "$@"; do
        printf '%s\n' "$cmd" >&9
        sleep 3
    done
    printf 'server shutdown 1\n' >&9
    exec 9>&-
    local i
    for i in $(seq 1 60); do
        if ! kill -0 "$ws_pid" 2>/dev/null; then break; fi
        sleep 1
    done
    kill "$ws_pid" "$as_pid" 2>/dev/null || true
    wait "$ws_pid" 2>/dev/null || true
    wait "$as_pid" 2>/dev/null || true
    rm -f "$fifo"
    return 0
}

realm_pin() {   # point realm 1 at the LAN address
    if mysql_admin acore_auth -e "UPDATE realmlist SET address='$LAN_IP', localAddress='$LAN_IP' WHERE id=1;" 2>/dev/null; then
        log "realm 1 pinned to $LAN_IP:8085"
    else
        warn "could not pin realm 1 (is the auth database populated?)"
    fi
}

# ---------------------------------------------------------------- host stage

write_host_wrappers() {
    if [ "$CHECK" = 1 ]; then
        note "write $HOME/start-wow-server.sh and $HOME/stop-wow-server.sh"
        return 0
    fi
    umask 077
    cat > "$HOME/start-wow-server.sh" <<WRAP
#!/data/data/com.termux/files/usr/bin/bash
# Start the realm. --bots N sets the random bot count before the world reads it.
set -uo pipefail
STATE="$HOST_STATE/state.env"
# state.env writes values as KEY="value"; strip the quotes, or the path itself contains them.
state() { [ -f "\$STATE" ] || return 0; sed -n "s/^\$1=//p" "\$STATE" | tail -1 | sed -E 's/^"(.*)"\$/\1/'; }
root="\$(state ROOT)"; [ -n "\$root" ] || root="$ROOT"
conf_dir="\$(state CONF_DIR)"; [ -n "\$conf_dir" ] || conf_dir="$ROOT/etc"
if [ "\${1:-}" = "--bots" ]; then
    [ "\$#" -eq 2 ] || { echo "usage: start-wow-server.sh [--bots N]" >&2; exit 2; }
    n="\$2"
    proot-distro login $ROOTFS_NAME -- "\$root/set-bots.sh" "\$conf_dir/modules/playerbots.conf" "\$n" "\$root/.thor-state/state.env" || exit \$?
    [ -f "\$STATE" ] && sed -i -E "s|^BOTS=.*|BOTS=\$n|" "\$STATE"
    shift 2
elif [ "\$#" -ne 0 ]; then
    echo "usage: start-wow-server.sh [--bots N]" >&2
    exit 2
fi
# The wake lock is what keeps Android from freezing the server; it comes from the Termux:API app.
command -v termux-wake-lock >/dev/null && termux-wake-lock
exec proot-distro login $ROOTFS_NAME -- "\$root/run.sh"
WRAP
    cat > "$HOME/stop-wow-server.sh" <<WRAP
#!/data/data/com.termux/files/usr/bin/bash
proot-distro login $ROOTFS_NAME -- $ROOT/stop.sh
command -v termux-wake-unlock >/dev/null && termux-wake-unlock
WRAP
    chmod +x "$HOME/start-wow-server.sh" "$HOME/stop-wow-server.sh"
    umask 022
    # Older installs got a third script that only forwarded start/stop and changed bots. The start
    # script now owns --bots, so remove that generated copy rather than leave a second control path.
    if [ -f "$HOME/wow-server.sh" ] && grep -q '^# Control for the self-hosted realm' "$HOME/wow-server.sh"; then
        rm -f "$HOME/wow-server.sh"
        log "removed obsolete $HOME/wow-server.sh"
    fi
    log "wrote $HOME/start-wow-server.sh and $HOME/stop-wow-server.sh"
}

host_preflight() {
    [ "$(uname -m)" = "aarch64" ] || die "this script targets aarch64 (got $(uname -m)); run it on the Thor"
    have pkg || die "this does not look like Termux (no 'pkg' command)"
    case "$(uname -o 2>/dev/null)" in
        Android) : ;;
        *) warn "uname -o did not say Android; continuing anyway" ;;
    esac
}

host_install() {
    step "Termux preflight"
    host_preflight
    local free_kb
    free_kb=$(df -Pk "$HOME" | awk 'NR == 2 {print $4}')
    log "free space in \$HOME: $((free_kb / 1048576)) GB (server + database + SQL ~6 GB)"
    if [ "$free_kb" -lt 6000000 ]; then
        warn "less than 6 GB free in \$HOME"
    fi

    step "Termux packages"
    note pkg update
    note pkg install -y proot-distro termux-api curl zstd

    step "Rootfs"
    if [ -d "$ROOTFS" ]; then
        log "already installed: $ROOTFS"
    else
        note proot-distro install "$ROOTFS_NAME"
        if [ "$CHECK" != 1 ]; then
            # "proot-distro install" decides which layout to create, so look again rather than
            # trusting the guess made before it ran.
            resolve_rootfs
            log "rootfs: $ROOTFS"
        fi
    fi

    step "Running the install inside the rootfs"
    local inner="/opt/thor/thor-install.sh"
    local args=(--inside --root "$ROOT" --bots "$BOTS" --db-user "$DB_USER"
                --account "$ACCOUNT" --boot-timeout "$BOOT_TIMEOUT"
                --release-url "$RELEASE_URL" --data-zip "$DATA_SRC")
    if [ -n "$DB_PASS" ]; then args+=(--db-pass "$DB_PASS"); fi
    if [ -n "$ACCOUNT_PASS" ]; then args+=(--account-pass "$ACCOUNT_PASS"); fi
    if [ -n "$LAN_IP" ]; then args+=(--lan-ip "$LAN_IP"); fi
    if [ "$FROM_SOURCE" = 1 ]; then args+=(--from-source); fi
    if [ "$FORCE" = 1 ]; then args+=(--force); fi
    if [ "$CHECK" = 1 ]; then args+=(--check); fi

    # This stage runs for an hour or more: apt, the client-data download, and a first boot that waits
    # up to --boot-timeout. Android freezes a backgrounded Termux, which stops the script and mysqld
    # alike and looks exactly like the install giving up, so take the wake lock before it starts. The
    # command comes from the Termux:API app, so warn instead of failing the install when it is absent.
    if have termux-wake-lock; then
        note termux-wake-lock
    else
        warn "termux-wake-lock is not available (it comes from the Termux:API app); keep Termux in the foreground while this runs"
    fi

    note mkdir -p "$(dirname "$ROOTFS$inner")"
    note cp "$SCRIPT_PATH" "$ROOTFS$inner"
    if [ "$CHECK" != 1 ]; then
        [ -f "$ROOTFS$inner" ] || die "could not put the script into the rootfs: $ROOTFS$inner"
        # The guest sees the copy only if this is the filesystem proot-distro boots. Asking from
        # inside costs one proot start and turns a bare "No such file or directory" from the guest's
        # bash into a statement of what is actually wrong.
        guest_has_path "$inner" || die_path_invisible "$inner"
    fi
    note proot-distro login "$ROOTFS_NAME" -- bash "$inner" "${args[@]}"

    step "Host side"
    note mkdir -p "$HOST_STATE"
    manifest_add "$HOME/start-wow-server.sh"
    manifest_add "$HOME/stop-wow-server.sh"
    manifest_add "$ROOTFS$inner"
    write_host_wrappers

    state_save
    step "Host side finished"
    log "start with: ~/start-wow-server.sh   |   inspect with: bash thor-install.sh --status"
}

host_status() {
    step "Termux side"
    state_load || true
    state_apply_cli
    if [ -f "$(state_file)" ]; then ok "state file: $(state_file)"; else bad "no state file at $(state_file) (installed elsewhere? pass the same flags)"; fi
    if [ -d "$ROOTFS" ]; then
        ok "rootfs: $ROOTFS ($(human_size "$ROOTFS"))"
    else
        bad "rootfs missing: $ROOTFS"
    fi
    if [ -d "$ROOTFS$ROOT" ]; then
        ok "install root: $ROOT ($(human_size "$ROOTFS$ROOT"))"
    else
        bad "install root missing: $ROOT (inside the rootfs)"
    fi
    local w
    for w in "$HOME/start-wow-server.sh" "$HOME/stop-wow-server.sh"; do
        if [ -x "$w" ]; then ok "wrapper: $w"; else bad "wrapper missing: $w"; fi
    done
    log "free space in \$HOME: $(( $(df -Pk "$HOME" | awk 'NR == 2 {print $4}') / 1048576 )) GB"
    if [ -d "$ROOTFS" ] && [ -f "$ROOTFS$INSIDE_SCRIPT_COPY" ]; then
        step "Inside the rootfs"
        if ! proot_login "$ROOTFS_NAME" -- bash "$INSIDE_SCRIPT_COPY" --inside --root "$ROOT" --status; then
            PROBLEMS=$((PROBLEMS + 1))
        fi
    else
        warn "cannot inspect the inside (no script copy at $ROOTFS$INSIDE_SCRIPT_COPY); if the rootfs on disk is not the one this distro boots, name it with THOR_ROOTFS_DIR (see --help)"
    fi
    summary_status
}

host_repair() {
    step "Termux side"
    host_preflight
    state_load || true
    state_apply_cli
    [ -d "$ROOTFS" ] || die "rootfs missing: $ROOTFS (install first)"
    # This copy is the one thing the host stage owns. Refresh it even when it exists: --repair may be
    # run from a newer script, and the inside stage is where missing helpers are detected and written.
    if [ ! -f "$ROOTFS$INSIDE_SCRIPT_COPY" ]; then
        warn "the script's copy is missing; writing it again"
    fi
    mkdir -p "$(dirname "$ROOTFS$INSIDE_SCRIPT_COPY")"
    cp "$SCRIPT_PATH" "$ROOTFS$INSIDE_SCRIPT_COPY"
    manifest_add "$ROOTFS$INSIDE_SCRIPT_COPY"
    guest_has_path "$INSIDE_SCRIPT_COPY" || die_path_invisible "$INSIDE_SCRIPT_COPY"
    local w
    for w in "$HOME/start-wow-server.sh" "$HOME/stop-wow-server.sh"; do
        if [ -x "$w" ]; then ok "wrapper: $w"; else warn "recreating $w"; manifest_add "$w"; fi
    done
    write_host_wrappers
    step "Inside the rootfs"
    # The inside stage exits 0 when everything passes, 1 when the checks still have problems (which
    # is also what die() uses), and anything else when it could not start at all - so the code is
    # reported instead of guessing which of the two happened.
    local rc=0
    proot_login "$ROOTFS_NAME" -- bash "$INSIDE_SCRIPT_COPY" --inside --root "$ROOT" --repair || rc=$?
    if [ "$rc" = 0 ]; then
        ok "the inside repair finished"
    else
        warn "the inside repair exited $rc: the report above says why (it could not start, or problems remain)"
    fi
    log "re-run --status to see the result"
}

host_uninstall() {
    step "Termux side"
    state_load || true
    state_apply_cli
    local purge="$PURGE_ROOTFS" keep="$KEEP_DB"
    local dump
    dump="$HOST_STATE/db-dump-$(date -u +%Y%m%dT%H%M%SZ).sql"

    step "Stop the server"
    if [ -f "$ROOTFS$ROOT/stop.sh" ]; then
        proot_login "$ROOTFS_NAME" -- "$ROOT/stop.sh" || warn "could not run the stop script (already stopped?)"
    else
        log "no stop script at $ROOT/stop.sh (nothing to stop)"
    fi

    step "Inside cleanup"
    if [ -f "$ROOTFS$INSIDE_SCRIPT_COPY" ]; then
        if [ "$keep" = 1 ]; then
            proot_login "$ROOTFS_NAME" -- bash "$INSIDE_SCRIPT_COPY" --inside --root "$ROOT" --uninstall --keep-db || warn "the inside cleanup did not run"
        else
            proot_login "$ROOTFS_NAME" -- bash "$INSIDE_SCRIPT_COPY" --inside --root "$ROOT" --uninstall || warn "the inside cleanup did not run"
        fi
        if [ "$keep" = 1 ] && [ -f "$ROOTFS$ROOT/.thor-state/db-dump.sql" ]; then
            note mkdir -p "$HOST_STATE"
            note mv "$ROOTFS$ROOT/.thor-state/db-dump.sql" "$dump"
            log "database dump kept at $dump (restore: see --help notes in the file header)"
        fi
    else
        log "no script copy inside the rootfs; skipping the inside cleanup"
    fi

    step "Removing the install root"
    if [ "$CHECK" = 1 ]; then
        note rm -rf "$ROOTFS$ROOT"
    elif [ -d "$ROOTFS$ROOT" ]; then
        safe_remove "$ROOTFS$ROOT" "$ROOTFS" && log "removed $ROOTFS$ROOT"
    else
        log "$ROOTFS$ROOT is already gone"
    fi

    step "Removing the files recorded in the host manifest"
    manifest_remove_all
    if [ -d "$ROOTFS/opt/thor" ]; then
        if [ "$CHECK" = 1 ]; then note rm -rf "$ROOTFS/opt/thor"; else safe_remove "$ROOTFS/opt/thor" "$ROOTFS" && log "removed $ROOTFS/opt/thor"; fi
    fi
    note rm -f "$HOST_STATE/manifest.txt"

    if [ "$purge" = 1 ]; then
        step "Removing the whole rootfs"
        proot_remove_rootfs "$ROOTFS_NAME" || warn "could not remove the rootfs; do it by hand: proot-distro remove $ROOTFS_NAME"
    else
        log "the Ubuntu rootfs is still there ($ROOTFS); it holds the apt packages"
        log "remove it completely with: bash thor-install.sh --uninstall --purge-rootfs"
    fi

    step "Done"
    log "state and any dump kept in $HOST_STATE"
    log "if you dumped the database, restore it with:"
    log "  proot-distro login $ROOTFS_NAME -- bash $INSIDE_SCRIPT_COPY --inside --install   # then"
    log "  proot-distro login $ROOTFS_NAME -- mysql --protocol=socket --socket=$SOCKET -uroot < <dump>"
}

# ---------------------------------------------------------------- inside stage: install

stage_inside() {
    step "Rootfs preflight"
    [ "$(id -u)" = "0" ] || die "run the inside stage as root (proot-distro login gives root)"
    [ "$(uname -m)" = "aarch64" ] || die "aarch64 expected, got $(uname -m)"
    have apt-get || die "no apt-get: is this the Ubuntu rootfs?"
    # shellcheck disable=SC1091  # /etc/os-release is dynamic and always present on Ubuntu
    log "os: $(. /etc/os-release 2>/dev/null && printf '%s %s' "$PRETTY_NAME" "$VERSION_CODENAME")"
    if [ "$CHECK" = 1 ]; then log "check mode: nothing will be changed"; fi
    log "install root: $ROOT"

    install_deps
    fetch_server
    refresh_custom_sql
    layout_check
    install_missing_libs || warn "some libraries could not be resolved; --status lists them"   # never aborts an install
    ensure_client_data
    db_start
    create_schema
    write_configs
    first_boot
    write_helpers
    state_save
    summary
}

install_deps() {
    step "Ubuntu packages"
    if [ ! -e "$POLICY_RC_D" ]; then
        note sh -c "printf '#!/bin/sh\nexit 101\n' > $POLICY_RC_D; chmod +x $POLICY_RC_D"
        manifest_add "$POLICY_RC_D"
    fi
    export DEBIAN_FRONTEND=noninteractive
    note apt-get update
    # Only the never-renamed packages are hardcoded; the libraries the prebuilt binaries
    # actually need are resolved from ldd afterwards (install_missing_libs), because Ubuntu
    # renamed several of them in 24.04 (libssl3 -> libssl3t64, libreadline8 -> libreadline8t64).
    note apt-get install -y --no-install-recommends ca-certificates curl unzip zstd \
        mysql-client mysql-server
    if [ "$FROM_SOURCE" = 1 ]; then
        # Same package set as upstream CI (.github/workflows/core-build-playerbots.yml:51-55);
        # libbz2-dev is required by deps/bzip2/CMakeLists.txt:15 on every configure.
        note apt-get install -y --no-install-recommends \
            git cmake ninja-build make gcc g++ clang lld patch \
            libboost-all-dev libssl-dev libbz2-dev libreadline-dev \
            libncurses-dev zlib1g-dev libmysqlclient-dev
    fi
}

fetch_server() {
    if [ -x "$ROOT/bin/worldserver" ] && [ "$FORCE" = 0 ] && [ "$FROM_SOURCE" = 0 ]; then
        step "Server files"
        log "already installed in $ROOT ($(human_size "$ROOT"))"
        return 0
    fi

    if [ "$FROM_SOURCE" = 1 ]; then
        step "Building from source (hours - the prebuilt artifact exists to avoid this)"
        if [ "$CHECK" = 1 ]; then
            log "would clone $CORE_REPO@$CORE_COMMIT + $PB_REPO@$PB_COMMIT into $ROOT/src-build and install into $ROOT"
            log "would fetch $SELF_REPO@$SELF_REF as a tarball for modules/mod-creature-gear, modules/mod-hardcore, modules/mod-e-interact, modules/mod-survivor, modules/mod-pawn, sql/custom/db_world and patches/playerbots-player-centric.patch"
            return 0
        fi
        local src="$ROOT/src-build"
        mkdir -p "$src/modules"
        fetch_git "$CORE_REPO" "$CORE_COMMIT" "$src"
        fetch_git "$PB_REPO" "$PB_COMMIT" "$src/modules/mod-playerbots"
        fetch_self_module "$src/modules" "$src/data/sql"
        apply_playerbots_patch "$src/modules/mod-playerbots" \
            "$src/.thor-patches/playerbots-player-centric.patch"
        # -Wno-psabi: GCC >= 10.1 notes every function that passes or returns a class with a
        # zero-sized subobject (a homogeneous aggregate such as std::pair<float, float>), because
        # the C++17 ABI for those changed to match C++14 and objects from older C++17 builds
        # cannot be mixed with these. Nothing here is mixed - one compiler, one project - so the
        # notes are noise, and this is the same flag the workflow passes.
        cmake -S "$src" -B "$src/build" -G Ninja \
            -DCMAKE_INSTALL_PREFIX="$ROOT" -DCONF_DIR="$ROOT/etc" \
            -DCMAKE_BUILD_TYPE=RelWithDebInfo -DAPPS_BUILD=all -DTOOLS_BUILD=all \
            -DSCRIPTS=static -DMODULES=static -DBoost_USE_STATIC_LIBS=ON \
            -DCMAKE_CXX_FLAGS=-Wno-psabi
        cmake --build "$src/build" --parallel "$(nproc)"
        cmake --install "$src/build"
        SRC_DIR="$src"
        return 0
    fi

    step "Prebuilt linux/arm64 server"
    if [ "$CHECK" = 1 ]; then
        local code
        code=$(curl_gh -sIL -o /dev/null -w '%{http_code}' "$RELEASE_URL" 2>/dev/null || echo "000")
        log "release URL: $RELEASE_URL -> HTTP $code"
        if [ "$code" != "200" ]; then
            if [ -n "$GH_TOKEN_OPT" ]; then
                log "  not available yet: run .github/workflows/server-aarch64.yml from the Actions tab"
            else
                log "  not available yet, or the repository is private: run the workflow once, and if the"
                log "  repository is private pass --github-token TOKEN (or set THOR_GH_TOKEN)"
            fi
        fi
        return 0
    fi
    local tarball=/tmp/acore-server-arm64.tar.zst
    curl_gh -fL --retry 3 -o "$tarball" "$RELEASE_URL" \
        || die "download failed: $RELEASE_URL (run the server-aarch64 workflow once to publish a release; if the repository is private, pass --github-token; or re-run with --from-source)"
    if curl_gh -fsL -o "$tarball.sha256" "$RELEASE_URL.sha256" 2>/dev/null && [ -s "$tarball.sha256" ]; then
        local want got
        want=$(awk '{print $1}' "$tarball.sha256")
        got=$(sha256_file "$tarball")
        [ "$want" = "$got" ] || die "checksum mismatch: expected $want, got $got"
        ARTIFACT_SHA="$got"
        log "checksum ok: $got"
    else
        warn "no .sha256 beside the tarball; skipping verification"
    fi
    mkdir -p "$ROOT"
    tar --zstd -xf "$tarball" -C "$ROOT"
    rm -f "$tarball" "$tarball.sha256"
    log "unpacked into $ROOT ($(human_size "$ROOT"))"
}

fetch_git() {   # fetch_git REPO COMMIT DEST
    local repo="$1" commit="$2" dest="$3"
    if [ -d "$dest/.git" ]; then
        log "source already present: $dest"
    else
        git clone --quiet "$repo" "$dest"
    fi
    git -C "$dest" fetch --quiet --depth 1 origin "$commit" \
        || die "cannot fetch $commit from $repo (is the commit still published?)"
    git -C "$dest" checkout --quiet --detach FETCH_HEAD
    log "source at $dest: $(git -C "$dest" rev-parse --short HEAD)"
}

apply_playerbots_patch() {   # apply_playerbots_patch MODULE-DIR PATCH-FILE
    local dest="$1" patch_file="$2"
    have patch || die "patch is needed to apply the player-centric bot distribution"
    [ -f "$patch_file" ] || die "missing player-centric bot patch: $patch_file"
    patch -p1 --batch -d "$dest" < "$patch_file" >/dev/null \
        || die "cannot apply $patch_file to $dest (upstream pin changed?)"
    log "applied player-centric bot distribution patch"
}

fetch_self_module() {   # fetch_self_module DEST-PARENT SQL-PARENT - this repo's module and custom SQL
    local parent="$1" sql_parent="${2:-}"
    have curl || die "curl is needed to fetch the module source (apt-get install curl)"
    mkdir -p "$parent"
    if [ -n "$GH_TOKEN_OPT" ]; then
        log "fetching $SELF_REPO@$SELF_REF as a tarball (private repository: using the token)"
    else
        log "fetching $SELF_REPO@$SELF_REF as a tarball"
    fi
    # No git here: the module and the custom SQL are two directories inside a repository that
    # carries the installer, the workflow and the design docs too, and a tarball fetch needs no
    # history and no checkout.
    local tarball
    tarball="$(mktemp)"
    curl_gh -fL --retry 3 -o "$tarball" "https://codeload.github.com/$SELF_REPO/tar.gz/$SELF_REF" \
        || die "cannot download https://codeload.github.com/$SELF_REPO/tar.gz/$SELF_REF (private? pass --github-token TOKEN or set THOR_GH_TOKEN)"
    # --strip-components=2 removes "<repo>-<ref>/modules/" so each tree lands as
    # $parent/<module-name>.
    tar -xzf "$tarball" --wildcards --strip-components=2 -C "$parent" \
        '*/modules/mod-creature-gear*' '*/modules/mod-hardcore*' '*/modules/mod-e-interact*' \
        '*/modules/mod-survivor*' '*/modules/mod-pawn*' \
        || { rm -f "$tarball"; die "cannot extract modules/mod-creature-gear, modules/mod-hardcore, modules/mod-e-interact, modules/mod-survivor and modules/mod-pawn out of $SELF_REPO@$SELF_REF"; }
    if [ -n "$sql_parent" ]; then
        # Same strip: "<repo>-<ref>/sql/custom/db_world/..." becomes
        # "<sql_parent>/custom/db_world/...", which is the CUSTOM include the base
        # database already registers.
        mkdir -p "$sql_parent"
        tar -xzf "$tarball" --wildcards --strip-components=2 -C "$sql_parent" '*/sql/custom*' \
            || { rm -f "$tarball"; die "cannot extract sql/custom/db_world out of $SELF_REPO@$SELF_REF"; }
        local n
        n=$(find "$sql_parent/custom/db_world" -maxdepth 1 -name '*.sql' 2>/dev/null | wc -l || true)
        [ -d "$sql_parent/custom/db_world" ] && [ "$n" -gt 0 ] \
            || { rm -f "$tarball"; die "$SELF_REPO@$SELF_REF carries no sql/custom/db_world/*.sql (wrong SELF_REF?)"; }
    fi
    # Keep the source patches outside modules/: the core's build scans modules/* as modules.
    # --strip-components=2 turns "<repo>-<ref>/patches/<name>" into "<patch_parent>/<name>".
    local patch_parent
    patch_parent="$(dirname "$parent")/.thor-patches"
    mkdir -p "$patch_parent"
    tar -xzf "$tarball" --wildcards --strip-components=2 -C "$patch_parent" '*/patches/*' \
        || { rm -f "$tarball"; die "cannot extract patches/ out of $SELF_REPO@$SELF_REF"; }
    [ -f "$patch_parent/playerbots-player-centric.patch" ] \
        || { rm -f "$tarball"; die "$SELF_REPO@$SELF_REF carries no patches/playerbots-player-centric.patch"; }
    rm -f "$tarball"
    [ -d "$parent/mod-creature-gear" ] \
        || die "$SELF_REPO@$SELF_REF carries no modules/mod-creature-gear (wrong SELF_REF?)"
    [ -d "$parent/mod-hardcore" ] \
        || die "$SELF_REPO@$SELF_REF carries no modules/mod-hardcore (wrong SELF_REF?)"
    [ -d "$parent/mod-e-interact" ] \
        || die "$SELF_REPO@$SELF_REF carries no modules/mod-e-interact (wrong SELF_REF?)"
    [ -d "$parent/mod-survivor" ] \
        || die "$SELF_REPO@$SELF_REF carries no modules/mod-survivor (wrong SELF_REF?)"
    [ -d "$parent/mod-pawn" ] \
        || die "$SELF_REPO@$SELF_REF carries no modules/mod-pawn (wrong SELF_REF?)"
    log "source at $parent/mod-creature-gear, $parent/mod-hardcore, $parent/mod-e-interact, $parent/mod-survivor and $parent/mod-pawn"
    [ -n "$sql_parent" ] && log "custom sql at $sql_parent/custom/db_world"
    return 0
}

refresh_custom_sql() {   # fetch only sql/custom from this repo into $SRC_DIR/data/sql
    # The prebuilt artifact carries sql/custom as a build-time snapshot, and fetch_server returns
    # early when the server is already installed, so a re-run would keep an old copy of the custom
    # SQL forever. Refresh it on every install and re-run: it is a few small files, and the DB
    # updater only ever reads them from $SRC_DIR/data/sql/custom.
    local sql_parent="${SRC_DIR:-$ROOT/src}/data/sql" url
    if [ "$FROM_SOURCE" = 1 ]; then
        # fetch_self_module already extracted sql/custom beside the source; a second fetch would
        # just download the same tarball again.
        return 0
    fi
    url="https://codeload.github.com/$SELF_REPO/tar.gz/$SELF_REF"
    if [ "$CHECK" = 1 ]; then
        log "would fetch $SELF_REPO@$SELF_REF sql/custom into $sql_parent"
        return 0
    fi
    if [ ! -d "$sql_parent" ]; then
        warn "no $sql_parent yet; skipping the custom sql refresh"
        return 0
    fi
    if ! have curl; then
        warn "curl is missing; keeping the custom sql already in $sql_parent"
        return 0
    fi
    local tarball
    tarball="$(mktemp)"
    if ! curl_gh -fL --retry 3 -o "$tarball" "$url"; then
        rm -f "$tarball"
        warn "cannot download $url; keeping the custom sql already in $sql_parent (private? pass --github-token TOKEN or set THOR_GH_TOKEN)"
        return 0
    fi
    # Same strip as fetch_self_module: "<repo>-<ref>/sql/custom/db_world/..." becomes
    # "$sql_parent/custom/db_world/...", the CUSTOM include the base database registers.
    if ! tar -xzf "$tarball" --wildcards --strip-components=2 -C "$sql_parent" '*/sql/custom*'; then
        rm -f "$tarball"
        warn "cannot extract sql/custom out of $SELF_REPO@$SELF_REF; keeping the custom sql already in $sql_parent"
        return 0
    fi
    rm -f "$tarball"
    local n=0
    if [ -d "$sql_parent/custom/db_world" ]; then
        n=$(find "$sql_parent/custom/db_world" -maxdepth 1 -name '*.sql' 2>/dev/null | wc -l || true)
    fi
    log "custom sql refreshed: ${n:-0} file(s) in $sql_parent/custom/db_world"
    return 0
}

layout_check() {
    step "Layout"
    if [ "$CHECK" = 1 ]; then
        log "would look for worldserver under $ROOT/{bin,env/dist/bin} and check its libraries"
        return 0
    fi
    local candidate
    BIN_DIR=""
    for candidate in "$ROOT/bin" "$ROOT/env/dist/bin"; do
        if [ -x "$candidate/worldserver" ]; then BIN_DIR="$candidate"; break; fi
    done
    [ -n "$BIN_DIR" ] || die "worldserver not found under $ROOT (expected bin/ or env/dist/bin/)"
    if [ -d "$ROOT/env/dist/etc" ]; then CONF_DIR="$ROOT/env/dist/etc"; else CONF_DIR="$ROOT/etc"; fi
    DATA_DIR="$ROOT/data"
    [ -n "$SRC_DIR" ] || SRC_DIR="$ROOT/src"
    log "binaries    : $BIN_DIR"
    log "configs     : $CONF_DIR"
    log "client data : $DATA_DIR"
    log "sql sources : $SRC_DIR"
    log "database    : $DB_DIR (socket $SOCKET)"

    local bin lib
    for bin in worldserver authserver; do
        lib=$(missing_libs "$BIN_DIR/$bin")
        if [ -n "$lib" ]; then
            warn "$bin is missing libraries: $(printf '%s' "$lib" | tr '\n' ' ')"
            warn "  install them, then re-run --status"
        fi
    done
    ensure_conf_dir_link
}

ensure_conf_dir_link() {   # module configs are read from the compile-time _CONF_DIR
    local baked
    baked=$(baked_conf_dir "$BIN_DIR/worldserver")
    [ -n "$baked" ] || return 0
    [ "$baked" = "$CONF_DIR" ] && return 0
    if [ -e "$baked" ]; then
        warn "the binary reads module configs from $baked, which exists but is not $CONF_DIR"
        return 0
    fi
    log "linking $baked -> $CONF_DIR (module configs come from the build-time path)"
    mkdir -p "$(dirname "$baked")"
    ln -s "$CONF_DIR" "$baked"
    manifest_add "$baked"
}

ensure_client_data() {
    step "Client data (dbc/maps/vmaps/mmaps)"
    if [ "$CHECK" = 1 ]; then
        log "would reuse $DATA_DIR, or fetch $DATA_SRC if dbc/ and maps/ are absent"
        return 0
    fi
    if [ -d "$DATA_DIR/dbc" ] && [ -d "$DATA_DIR/maps" ]; then
        log "already present: $DATA_DIR ($(human_size "$DATA_DIR"))"
        return 0
    fi
    case "$DATA_SRC" in
        none)
            warn "no client data requested; worldserver needs DataDir pointing at a directory with dbc/ and maps/"
            return 0
            ;;
        http*|ftp*)
            log "downloading $DATA_SRC"
            curl -fL --retry 3 -o /tmp/data.zip "$DATA_SRC" || die "client data download failed"
            local got
            got=$(sha256_file /tmp/data.zip)
            [ "$got" = "$DATA_SHA256" ] || die "client data checksum mismatch: expected $DATA_SHA256, got $got"
            log "checksum ok"
            ;;
        *)
            [ -f "$DATA_SRC" ] || die "no such file: $DATA_SRC"
            cp "$DATA_SRC" /tmp/data.zip
            ;;
    esac
    mkdir -p "$DATA_DIR"
    unzip -q -o /tmp/data.zip -d "$DATA_DIR"
    rm -f /tmp/data.zip
    if [ ! -d "$DATA_DIR/dbc" ] || [ ! -d "$DATA_DIR/maps" ]; then
        die "unzipped client data has no dbc/ and maps/ at the top level"
    fi
    log "installed: $(human_size "$DATA_DIR")"
}

create_schema() {
    step "Databases and user"
    # DB_PASS and ACCOUNT_PASS were settled by default_credentials() in main().
    if [ "$CHECK" = 1 ]; then
        log "would create acore_auth, acore_world, acore_characters and the MySQL user '$DB_USER'"
        log "(acore_playerbots is created by the module's own updater on first boot)"
        return 0
    fi
    if ! mysql_admin <<SQL
CREATE DATABASE IF NOT EXISTS acore_auth;
CREATE DATABASE IF NOT EXISTS acore_world;
CREATE DATABASE IF NOT EXISTS acore_characters;
CREATE USER IF NOT EXISTS '$DB_USER'@'127.0.0.1' IDENTIFIED BY '$DB_PASS';
ALTER USER '$DB_USER'@'127.0.0.1' IDENTIFIED BY '$DB_PASS';
GRANT ALL PRIVILEGES ON acore_auth.*       TO '$DB_USER'@'127.0.0.1';
GRANT ALL PRIVILEGES ON acore_world.*      TO '$DB_USER'@'127.0.0.1';
GRANT ALL PRIVILEGES ON acore_characters.* TO '$DB_USER'@'127.0.0.1';
GRANT ALL PRIVILEGES ON acore_playerbots.* TO '$DB_USER'@'127.0.0.1';
FLUSH PRIVILEGES;
SQL
    then
        tail -5 "$ROOT/logs/mysql.err" >&2 || true
        die "mysql rejected the schema setup above; the tail of $ROOT/logs/mysql.err follows it"
    fi
    # This query is the step's last command, so a failure here used to end the install with no output
    # at all - db_query sends the client's error to /dev/null. Say what happened instead.
    local schemas
    if ! schemas="$(db_query "SELECT SCHEMA_NAME FROM information_schema.SCHEMATA WHERE SCHEMA_NAME LIKE 'acore%'")"; then
        db_ping || warn "mysqld is not answering on $SOCKET any more"
        tail -5 "$ROOT/logs/mysql.err" >&2 || true
        die "the databases were created but the check query failed; the log tail is above (docs/RUNBOOK.md Part 10)"
    fi
    log "databases: $(printf '%s' "$schemas" | tr '\n' ' ')"
}

conf_pair() {   # print the live config file, creating it from NAME.conf.dist once
    local dist="$CONF_DIR/$1.conf.dist" live="$CONF_DIR/$1.conf"
    if [ ! -f "$live" ]; then
        [ -f "$dist" ] || die "missing $dist (is the artifact complete?)"
        cp "$dist" "$live"
        printf 'created %s\n' "$live" >&2        # never on stdout: callers capture the path
    fi
    printf '%s' "$live"
}

write_configs() {
    step "Configuration"
    if [ "$CHECK" = 1 ]; then
        log "would write $CONF_DIR/{authserver,worldserver}.conf, $CONF_DIR/modules/{playerbots,mod-creature-gear,mod_hardcore,mod_e_interact,mod-survivor,mod-pawn}.conf and $ROOT/logs"
        return 0
    fi
    [ -n "$BIN_DIR" ] || layout_check
    local bind_ip="$LAN_IP"
    if [ -z "$bind_ip" ]; then bind_ip="127.0.0.1"; fi
    local mysql_bin
    mysql_bin="$(command -v mysql)"

    local ws as pb cg hc ei sv pw
    ws="$(conf_pair worldserver)"
    as="$(conf_pair authserver)"
    set_conf "$ws" DataDir                "\"$DATA_DIR\""
    set_conf "$ws" LogsDir                "\"$ROOT/logs\""
    set_conf "$ws" SourceDirectory        "\"$SRC_DIR\""
    set_conf "$ws" MySQLExecutable        "\"$mysql_bin\""
    set_conf "$ws" BindIP                 "\"$bind_ip\""
    set_conf "$ws" WorldServerPort        8085
    # The realm is started headless by run.sh; the CLI prompt is readline, and at end of input it
    # reprints "AC>" in a loop. GM commands go in-game. Set 1 and start under tmux to get it back.
    set_conf "$ws" Console.Enable         0
    set_conf "$ws" Updates.EnableDatabases 7
    set_conf "$ws" Updates.AutoSetup      1
    set_conf "$ws" PlayerLimit            1000
    # Travel is dead time on a handheld. 2 = instant flight, but each flight master keeps a toggle so
    # the player can still fly normally; 1 = every taxi node known from login; 50.0 = the top of the
    # core's flight-speed clamp (values above 50 are rejected and fall back to the 32 default).
    set_conf "$ws" InstantFlightPaths     2
    set_conf "$ws" AllFlightPaths         1
    set_conf "$ws" TaxiFlightSpeed        50.0
    set_conf "$ws" LoginDatabaseInfo      "\"127.0.0.1;3306;$DB_USER;$DB_PASS;acore_auth\""
    set_conf "$ws" WorldDatabaseInfo      "\"127.0.0.1;3306;$DB_USER;$DB_PASS;acore_world\""
    set_conf "$ws" CharacterDatabaseInfo  "\"127.0.0.1;3306;$DB_USER;$DB_PASS;acore_characters\""
    log "worldserver.conf: InstantFlightPaths=$(conf_get "$ws" InstantFlightPaths) AllFlightPaths=$(conf_get "$ws" AllFlightPaths) TaxiFlightSpeed=$(conf_get "$ws" TaxiFlightSpeed)"
    set_conf "$as" LogsDir                "\"$ROOT/logs\""
    set_conf "$as" SourceDirectory        "\"$SRC_DIR\""
    set_conf "$as" MySQLExecutable        "\"$mysql_bin\""
    set_conf "$as" BindIP                 "\"$bind_ip\""
    set_conf "$as" RealmServerPort        3724
    set_conf "$as" Updates.EnableDatabases 1
    set_conf "$as" Updates.AutoSetup      1
    set_conf "$as" LoginDatabaseInfo      "\"127.0.0.1;3306;$DB_USER;$DB_PASS;acore_auth\""

    mkdir -p "$ROOT/logs"
    if [ -f "$CONF_DIR/modules/playerbots.conf.dist" ] || [ -f "$CONF_DIR/modules/playerbots.conf" ]; then
        pb="$(conf_pair modules/playerbots)"
        set_conf "$pb" Playerbots.Enabled              1
        set_conf "$pb" Playerbots.RandomBotAutologin    1
        set_conf "$pb" Playerbots.MinRandomBots         "$BOTS"
        set_conf "$pb" Playerbots.MaxRandomBots         "$BOTS"
        # Solo-friendly default: keep the bot population level-appropriate for, and inside the zone
        # of, real players instead of spreading it over the whole world. The keys are ignored by an
        # unpatched playerbots build, so an older artifact still starts normally.
        set_conf "$pb" Playerbots.RandomBotNearPlayers              1
        set_conf "$pb" Playerbots.RandomBotNearPlayersLevelRange    5
        set_conf "$pb" Playerbots.RandomBotNearPlayersFollowInterval 120
        set_conf "$pb" Playerbots.RandomBotNearPlayersMinDistance   "$NEAR_MIN_DISTANCE"
        set_conf "$pb" Playerbots.RandomBotNearPlayersSameFaction   1
        set_conf "$pb" Playerbots.RandomBotNearPlayersMaxPerPlayer  "$NEAR_BOTS"
        set_conf "$pb" Playerbots.RandomBotNearPlayersZoneTraffic   1
        set_conf "$pb" Playerbots.RandomBotNearPlayersZoneVisitMinTime "$NEAR_VISIT_MIN"
        set_conf "$pb" Playerbots.RandomBotNearPlayersZoneVisitMaxTime "$NEAR_VISIT_MAX"
        set_conf "$pb" Playerbots.RandomBotNearPlayersZoneAwayMinTime  "$NEAR_AWAY_MIN"
        set_conf "$pb" Playerbots.RandomBotNearPlayersZoneAwayMaxTime  "$NEAR_AWAY_MAX"
        # The twin-ring keys are gone: bots stay in the player's zone instead of being placed in
        # rings around the player. Left in an older config they would look like they still do
        # something, so they are dropped here.
        sed -i -E '/^[[:space:]]*Playerbots\.RandomBotNearPlayers(Radius|InnerCount|OuterRadius|ZoneCount)[[:space:]]*=/d' "$pb"
        # Pawn system (mod-pawn). The obelisk is the only doorway to a pool pawn, so addclass
        # stays a GM command; the pool is cut to five accounts (50 characters, both teams) so the
        # roster reads like a rift rather than a database dump; and pawns pass on every roll and
        # leave free-for-all corpses alone, so the loot-bag economy stays the player's.
        set_conf "$pb" Playerbots.AddClassCommand         0
        set_conf "$pb" Playerbots.AddClassAccountPoolSize 5
        set_conf "$pb" Playerbots.LootNeedRollLevel       0
        set_conf "$pb" Playerbots.LootGreedRollLevel      0
        set_conf "$pb" Playerbots.FreeMethodLoot          0
        set_conf "$pb" Playerbots.KeepAltsInGroup         1
        set_conf "$pb" Playerbots.SummonWhenGroup         1
        set_conf "$pb" PlayerbotsDatabaseInfo           "\"127.0.0.1;3306;$DB_USER;$DB_PASS;acore_playerbots\""
        set_conf "$pb" Playerbots.Updates.EnableDatabases 1
        log "playerbots.conf: enabled=$(conf_get "$pb" Playerbots.Enabled) bots=$(conf_get "$pb" Playerbots.MaxRandomBots)"
    else
        warn "no $CONF_DIR/modules/playerbots.conf.dist - the artifact was built without the module, bots will not load"
    fi
    if [ -f "$CONF_DIR/modules/mod-creature-gear.conf.dist" ] || [ -f "$CONF_DIR/modules/mod-creature-gear.conf" ]; then
        cg="$(conf_pair modules/mod-creature-gear)"
        # The shipped .dist already holds the intended values, so nothing is rewritten here unless
        # a key drifted; the four keys below are what makes all three halves of the module active.
        local cg_key cg_val
        for cg_key in CreatureGear.Enable CreatureGear.Drop.Enable \
                      CreatureGear.Appearance.Enable CreatureGear.Weapons.RerollOnRespawn; do
            cg_val="$(conf_get "$cg" "$cg_key")"
            if [ "$cg_val" != "1" ]; then
                warn "mod-creature-gear.conf: $cg_key is '$cg_val', restoring the shipped default 1"
                set_conf "$cg" "$cg_key" 1
            fi
        done
        log "mod-creature-gear.conf: Enable=$(conf_get "$cg" CreatureGear.Enable) Drop.Enable=$(conf_get "$cg" CreatureGear.Drop.Enable) Appearance.Enable=$(conf_get "$cg" CreatureGear.Appearance.Enable) Weapons.RerollOnRespawn=$(conf_get "$cg" CreatureGear.Weapons.RerollOnRespawn)"
    else
        warn "no $CONF_DIR/modules/mod-creature-gear.conf.dist - the artifact was built without the module, creatures keep their stock gear"
    fi
    if [ -f "$CONF_DIR/modules/mod_e_interact.conf.dist" ] || [ -f "$CONF_DIR/modules/mod_e_interact.conf" ]; then
        ei="$(conf_pair modules/mod_e_interact)"
        # One key for the whole interact loop (loot, gather, talk, chests and nodes). Every option
        # defaults to on in the shipped .dist; a drifted key is put back, since a silent 0 here
        # looks exactly like a broken key in game.
        local ei_key ei_val
        for ei_key in EInteract.Enable EInteract.Loot EInteract.Gather EInteract.Talk EInteract.Use; do
            ei_val="$(conf_get "$ei" "$ei_key")"
            if [ "$ei_val" != "1" ]; then
                warn "mod_e_interact.conf: $ei_key is '$ei_val', restoring the shipped default 1"
                set_conf "$ei" "$ei_key" 1
            fi
        done
        log "mod_e_interact.conf: Enable=$(conf_get "$ei" EInteract.Enable) Loot=$(conf_get "$ei" EInteract.Loot) Gather=$(conf_get "$ei" EInteract.Gather) Talk=$(conf_get "$ei" EInteract.Talk) Use=$(conf_get "$ei" EInteract.Use)"
    else
        warn "no $CONF_DIR/modules/mod_e_interact.conf.dist - the artifact was built without the module, the interact key will not work"
    fi
    if [ -f "$CONF_DIR/modules/mod_hardcore.conf.dist" ] || [ -f "$CONF_DIR/modules/mod_hardcore.conf" ]; then
        hc="$(conf_pair modules/mod_hardcore)"
        local hc_policy
        hc_policy="$(conf_get "$hc" Hardcore.DeathPolicy)"
        case "$hc_policy" in
            ghost|archive|delete) ;;
            *)
                # The module itself falls back to ghost; make the file say so too.
                if [ -n "$hc_policy" ]; then
                    warn "mod_hardcore.conf: unknown DeathPolicy '$hc_policy', restoring the shipped default ghost"
                fi
                set_conf "$hc" Hardcore.DeathPolicy ghost
                hc_policy=ghost
                ;;
        esac
        log "mod_hardcore.conf: Enable=$(conf_get "$hc" Hardcore.Enable) DeathPolicy=$hc_policy"
    else
        warn "no $CONF_DIR/modules/mod_hardcore.conf.dist - the artifact was built without the module, hardcore is unavailable"
    fi
    if [ -f "$CONF_DIR/modules/mod-survivor.conf.dist" ] || [ -f "$CONF_DIR/modules/mod-survivor.conf" ]; then
        sv="$(conf_pair modules/mod-survivor)"
        # The shipped .dist already holds these values; each one is still read back, because a key
        # the module cannot use is silently replaced by the module's own default and a container on
        # the wrong gameobject entry simply never appears. Only a value outside its range is
        # rewritten - a deliberate 0 (ruleset off, no map marker, no bot re-roll) is left alone.
        local sv_default sv_key sv_val
        for sv_default in "${SURVIVOR_CONF_DEFAULTS[@]}"; do
            sv_key="${sv_default%%=*}"
            sv_val="$(conf_get "$sv" "$sv_key")"
            if ! survivor_value_ok "$sv_key" "$sv_val"; then
                warn "mod-survivor.conf: $sv_key is '$sv_val', restoring the shipped default ${sv_default#*=}"
                set_conf "$sv" "$sv_key" "${sv_default#*=}"
            fi
        done
        log "mod-survivor.conf: Enable=$(conf_get "$sv" Survivor.Enable) MinLevelToFlag=$(conf_get "$sv" Survivor.MinLevelToFlag) DropEquipped=$(conf_get "$sv" Survivor.DropEquipped) DropBags=$(conf_get "$sv" Survivor.DropBags) ApplyToBots=$(conf_get "$sv" Survivor.ApplyToBots)"
    else
        warn "no $CONF_DIR/modules/mod-survivor.conf.dist - the artifact was built without the module, survivor mode is unavailable"
    fi
    if [ -f "$CONF_DIR/modules/mod-pawn.conf.dist" ] || [ -f "$CONF_DIR/modules/mod-pawn.conf" ]; then
        pw="$(conf_pair modules/mod-pawn)"
        # The shipped .dist already holds these values; each is read back because a key the module
        # cannot use is silently replaced by its own default. Only a value outside its range is
        # rewritten, so a deliberate Pawn.Enable=0 (obelisk off) survives a repair.
        local pw_default pw_key pw_val
        for pw_default in "${PAWN_CONF_DEFAULTS[@]}"; do
            pw_key="${pw_default%%=*}"
            pw_val="$(conf_get "$pw" "$pw_key")"
            if ! pawn_value_ok "$pw_key" "$pw_val"; then
                warn "mod-pawn.conf: $pw_key is '$pw_val', restoring the shipped default ${pw_default#*=}"
                set_conf "$pw" "$pw_key" "${pw_default#*=}"
            fi
        done
        log "mod-pawn.conf: Enable=$(conf_get "$pw" Pawn.Enable) MaxPawns=$(conf_get "$pw" Pawn.MaxPawns) LevelWindow=$(conf_get "$pw" Pawn.LevelWindow) ObeliskEntry=$(conf_get "$pw" Pawn.ObeliskEntry)"
    else
        warn "no $CONF_DIR/modules/mod-pawn.conf.dist - the artifact was built without the module, the obelisk is unavailable"
    fi
    log "worldserver.conf: DataDir=$(conf_get "$ws" DataDir) BindIP=$(conf_get "$ws" BindIP)"
}

first_boot() {
    step "First boot (populates the databases, then creates the GM account)"
    if [ "$CHECK" = 1 ]; then
        log "would run authserver + worldserver with stdin on a fifo, wait up to ${BOOT_TIMEOUT}s for 'World initialized',"
        log "then send: account create $ACCOUNT *** / account set gmlevel $ACCOUNT 3 -1 / server shutdown 1"
        return 0
    fi
    if ! world_console "$BOOT_TIMEOUT" \
            "account create $ACCOUNT $ACCOUNT_PASS" \
            "account set gmlevel $ACCOUNT 3 -1"; then
        FIRST_BOOT_OK=0
        warn "the first boot did not finish; watch $ROOT/logs/worldserver.out or start the server and try again"
        return 0
    fi
    if grep -qE 'Account created' "$ROOT/logs/worldserver.out"; then
        log "account '$ACCOUNT' created"
    else
        warn "could not confirm account creation; check the account table"
    fi
    [ -n "$LAN_IP" ] && realm_pin
    log "first boot finished"
}

write_helpers() {
    step "Start/stop scripts"
    if [ "$CHECK" = 1 ]; then
        log "would write $ROOT/{run.sh,stop.sh,start-db.sh,stop-db.sh,credentials.txt}"
        return 0
    fi
    umask 077
    cat > "$ROOT/credentials.txt" <<EOF
Created by thor-install.sh $SCRIPT_VERSION on $(date -u +%Y-%m-%dT%H:%M:%SZ)

database user : $DB_USER
database pass : $DB_PASS
gm account    : $ACCOUNT
gm password   : $ACCOUNT_PASS

MySQL root has no password over the unix socket $SOCKET (rootfs-local only).
Everything this install owns lives in $ROOT; see $STATE_DIR/manifest.txt for the rest.
EOF
    umask 022

    cat > "$ROOT/start-db.sh" <<EOF
#!/usr/bin/env bash
set -euo pipefail
mkdir -p "$RUN_DIR" "$DB_DIR" "$ROOT/logs"
chown mysql:mysql "$DB_DIR" "$RUN_DIR"
    if ! mysqladmin --protocol=socket --socket="$SOCKET" ping >/dev/null 2>&1; then
    # --innodb-use-native-aio=0: proot has no libaio, and with native AIO on InnoDB's first write
    # hangs while the server still answers ping (see docs/RUNBOOK.md Part 10).
    mysqld --user=mysql --datadir="$DB_DIR" --socket="$SOCKET" \\
           --bind-address=127.0.0.1 --port=3306 --skip-name-resolve \\
           --innodb-use-native-aio=0 \\
           --pid-file="$RUN_DIR/mysqld.pid" --log-error="$ROOT/logs/mysql.err" --daemonize
fi
for _ in \$(seq 1 60); do
    mysqladmin --protocol=socket --socket="$SOCKET" ping >/dev/null 2>&1 && exit 0
    sleep 1
done
echo "mysqld did not come up; see $ROOT/logs/mysql.err" >&2
exit 1
EOF

    cat > "$ROOT/stop-db.sh" <<EOF
#!/usr/bin/env bash
mysqladmin --protocol=socket --socket="$SOCKET" shutdown 2>/dev/null || true
EOF

    cat > "$ROOT/run.sh" <<EOF
#!/usr/bin/env bash
# Authserver + worldserver, with the world restarting if it dies. Ctrl-C stops everything.
set -uo pipefail
cd "$ROOT" || exit 1
"$ROOT/start-db.sh" || exit 1
"$BIN_DIR/authserver" -c "$CONF_DIR/authserver.conf" > logs/authserver.out 2>&1 &
AS_PID=\$!
WS_PID=""
cleanup() {
    echo "stopping..."
    trap - INT TERM
    [ -n "\$WS_PID" ] && kill \$WS_PID 2>/dev/null
    kill \$AS_PID 2>/dev/null
    wait 2>/dev/null
    "$ROOT/stop-db.sh"
    exit 0
}
trap cleanup INT TERM
while true; do
    # stdin is held open on purpose: a backgrounded job gets /dev/null, and the console then reads
    # EOF in a loop and reprints its "AC>" prompt forever. The pipeline's last element is the
    # server, so $! and wait still refer to it.
    tail -f /dev/null | "$BIN_DIR/worldserver" -c "$CONF_DIR/worldserver.conf" &
    WS_PID=\$!
    wait \$WS_PID
    echo "worldserver exited with \$?; restarting in 10s (Ctrl-C to stop)"
    sleep 10
done
EOF

    cat > "$ROOT/stop.sh" <<EOF
#!/usr/bin/env bash
cd "$ROOT" || exit 1
# run.sh restarts the world when it dies, so the loop has to go first: its INT/TERM trap stops both
# servers and then the database. Killing the binaries on their own just makes the loop bring them
# straight back, which is what "stop" used to do.
if pgrep -f "$ROOT/run.sh" >/dev/null 2>&1; then
    pkill -f "$ROOT/run.sh" 2>/dev/null || true
    for i in \$(seq 1 20); do
        pgrep -f "$ROOT/run.sh" >/dev/null 2>&1 || break
        sleep 1
    done
fi
pkill -f "$BIN_DIR/worldserver" 2>/dev/null || true
pkill -f "$BIN_DIR/authserver" 2>/dev/null || true
sleep 2
"$ROOT/stop-db.sh"
EOF
    cat > "$ROOT/set-bots.sh" <<EOF
#!/usr/bin/env bash
# set-bots.sh CONF [N] [STATE] - read or set the random bot count in a playerbots.conf.
# Min and Max move together: the start script offers one number, and a narrower window would only
# change how the population drifts after startup. STATE is the installer's state file, updated so
# --status and --repair agree with the config; the world reads the config at its next start.
set -euo pipefail
conf="\${1:?usage: set-bots.sh CONF [N] [STATE]}"
n="\${2:-}"
state="\${3:-}"
[ -f "\$conf" ] || { echo "no \$conf: the artifact was built without the module" >&2; exit 1; }
read_key() { grep -E "^[[:space:]]*Playerbots\.\$1[[:space:]]*=" "\$conf" | tail -1 | sed -E 's/^[^=]*=[[:space:]]*//; s/[[:space:]]*#.*\$//'; }
if [ -z "\$n" ]; then
    echo "min=\$(read_key MinRandomBots) max=\$(read_key MaxRandomBots)"
    exit 0
fi
case "\$n" in *[!0-9]*) echo "refusing '\$n': the bot count is a number" >&2; exit 2 ;; esac
[ "\$n" -le 2000 ] || { echo "refusing \$n: the range is 0-2000" >&2; exit 2; }
for key in MinRandomBots MaxRandomBots; do
    if grep -qE "^[[:space:]]*#?[[:space:]]*Playerbots\.\$key[[:space:]]*=" "\$conf"; then
        sed -i -E "s|^[[:space:]]*#?[[:space:]]*(Playerbots\.\$key)[[:space:]]*=.*|\1 = \$n|" "\$conf"
    else
        printf 'Playerbots.%s = %s\n' "\$key" "\$n" >> "\$conf"
    fi
done
if [ -n "\$state" ] && [ -f "\$state" ]; then
    if grep -q '^BOTS=' "\$state"; then
        sed -i -E "s|^BOTS=.*|BOTS=\$n|" "\$state"
    else
        printf 'BOTS=%s\n' "\$n" >> "\$state"
    fi
fi
echo "min=\$(read_key MinRandomBots) max=\$(read_key MaxRandomBots)"
EOF
    chmod +x "$ROOT/run.sh" "$ROOT/stop.sh" "$ROOT/start-db.sh" "$ROOT/stop-db.sh" "$ROOT/set-bots.sh"
    log "wrote $ROOT/run.sh, $ROOT/stop.sh, $ROOT/set-bots.sh (and the two database helpers)"
}

summary() {
    step "Summary"
    if [ "$CHECK" = 1 ]; then
        log "check mode finished; nothing was changed"
        return 0
    fi
    if [ "$FIRST_BOOT_OK" = 0 ]; then
        warn "the first boot did not finish; run $ROOT/run.sh and watch $ROOT/logs/worldserver.out"
    fi
    cat <<EOF
server      : $ROOT   (binaries $BIN_DIR, configs $CONF_DIR)
database    : $DB_DIR, socket $SOCKET
bots        : $BOTS (Playerbots.MinRandomBots / MaxRandomBots)
gm account  : $ACCOUNT / $ACCOUNT_PASS  (also in $ROOT/credentials.txt)
bind        : ${LAN_IP:-127.0.0.1}   (loopback unless --lan-ip was given)

Manage it
  inspect : bash thor-install.sh --status      (exit 1 when something needs repair)
  repair  : bash thor-install.sh --repair
  remove  : bash thor-install.sh --uninstall [--keep-db] [--purge-rootfs]
  start   : ~/start-wow-server.sh [--bots N]   (stop: ~/stop-wow-server.sh)

Next steps
  1. prove it with a PC 3.3.5a client before touching the launcher: re-run with
     --lan-ip <thor-lan-ip>, set "set realmlist <thor-lan-ip>" in the client's
     Data/<locale>/realmlist.wtf, log in, then re-run without --lan-ip.
  2. launcher client half: docs/LAUNCHER-HOSTING-DESIGN.md section 3.
EOF
}

summary_status() {   # the last line of a check run: exit 1 when anything needs repair
    if [ "$PROBLEMS" -gt 0 ]; then
        printf '\nFAIL  %d problem(s) found\n' "$PROBLEMS"
        exit 1
    fi
    if [ "$FIXES" -gt 0 ]; then
        printf '\nok    all checks pass (%d fix(es) applied)\n' "$FIXES"
    else
        printf '\nok    all checks pass\n'
    fi
}

# ---------------------------------------------------------------- inside stage: checks

inside_status() {
    state_load || true
    state_apply_cli
    echo
    run_checks 0
    summary_status
}

inside_repair() {
    state_load || true
    state_apply_cli
    echo
    run_checks 1
    local repaired="$FIXES"
    PROBLEMS=0
    echo
    log "re-checking after $repaired fix(es)"
    run_checks 0
    summary_status
}

check_needs_root() { [ "$(id -u)" = "0" ]; }

run_checks() {   # run_checks FIX(0|1)
    local fix="${1:-0}"
    step "Components"
    if [ -z "$BIN_DIR" ]; then layout_dirs; fi

    # 1. binaries
    local bin missing_any=0
    for bin in worldserver authserver; do
        if [ -x "$BIN_DIR/$bin" ]; then ok "binary: $BIN_DIR/$bin"; else bad "binary missing: $BIN_DIR/$bin"; missing_any=1; fi
    done
    if [ "$missing_any" = 1 ]; then
        if [ "$fix" = 1 ]; then
            warn "re-downloading the server (--force)"
            FIXES=$((FIXES + 1))
            FORCE=1
            fetch_server || true
            layout_dirs
        fi
    else
        local bin2 lib
        for bin2 in worldserver authserver; do
            lib=$(missing_libs "$BIN_DIR/$bin2")
            if [ -n "$lib" ]; then
                bad "$bin2 needs libraries: $(printf '%s' "$lib" | tr '\n' ' ')"
                if [ "$fix" = 1 ]; then
                    FIXES=$((FIXES + 1))
                    install_missing_libs || true
                fi
            else
                ok "$bin2 loads its libraries"
            fi
        done
    fi

    # 2. configs
    local ws="$CONF_DIR/worldserver.conf" as="$CONF_DIR/authserver.conf" pb="$CONF_DIR/modules/playerbots.conf"
    local cg="$CONF_DIR/modules/mod-creature-gear.conf" hc="$CONF_DIR/modules/mod_hardcore.conf"
    local ei="$CONF_DIR/modules/mod_e_interact.conf" sv="$CONF_DIR/modules/mod-survivor.conf"
    local pw="$CONF_DIR/modules/mod-pawn.conf"
    if [ -f "$ws" ] && [ -f "$as" ] && [ -f "$pb" ] && [ -f "$cg" ] && [ -f "$hc" ] && [ -f "$ei" ] && [ -f "$sv" ] && [ -f "$pw" ]; then
        ok "configs present: worldserver.conf, authserver.conf, modules/playerbots.conf, modules/mod-creature-gear.conf, modules/mod_hardcore.conf, modules/mod_e_interact.conf, modules/mod-survivor.conf, modules/mod-pawn.conf"
    else
        bad "configs incomplete (worldserver.conf/authserver.conf/modules/playerbots.conf/modules/mod-creature-gear.conf/modules/mod_hardcore.conf/modules/mod_e_interact.conf/modules/mod-survivor.conf/modules/mod-pawn.conf)"
        if [ "$fix" = 1 ]; then FIXES=$((FIXES + 1)); warn "rewriting the configs"; write_configs || true; fi
    fi
    local key problems=0
    for key in DataDir LogsDir SourceDirectory MySQLExecutable BindIP; do
        if [ -z "$(conf_get_unquoted "$ws" "$key" 2>/dev/null)" ]; then bad "worldserver.conf: $key is empty"; problems=1; fi
    done
    if [ "$problems" = 0 ] && [ -f "$ws" ]; then ok "worldserver.conf: DataDir/SourceDirectory/BindIP set"; fi
    if [ "$fix" = 1 ] && [ "$problems" = 1 ]; then write_configs || true; fi
    if [ -f "$ws" ]; then
        # Travel settings the installer writes. An older config, or one edited by hand, can hold the
        # blizzlike 0/0/32.0, which costs the player travel time on every flight. TaxiFlightSpeed is
        # validated by the core as 0.01 < v <= 50 (WorldConfig.cpp), and 32.0 is the stock value, so
        # only something above it can be the installer's setting.
        local ifp afp tfs travel_wrong=0
        ifp="$(conf_get "$ws" InstantFlightPaths)"
        if [ "$ifp" = 2 ]; then
            ok "worldserver.conf: InstantFlightPaths=$ifp"
        else
            bad "worldserver.conf: InstantFlightPaths is '$ifp', expected 2"
            travel_wrong=1
        fi
        afp="$(conf_get "$ws" AllFlightPaths)"
        if [ "$afp" = 1 ]; then
            ok "worldserver.conf: AllFlightPaths=$afp"
        else
            bad "worldserver.conf: AllFlightPaths is '$afp', expected 1"
            travel_wrong=1
        fi
        tfs="$(conf_get "$ws" TaxiFlightSpeed)"
        if awk -v v="$tfs" 'BEGIN { exit !(v ~ /^[0-9]+(\.[0-9]+)?$/ && v + 0 > 32 && v + 0 <= 50) }'; then
            ok "worldserver.conf: TaxiFlightSpeed=$tfs"
        else
            bad "worldserver.conf: TaxiFlightSpeed is '$tfs', expected a number in (32, 50]"
            travel_wrong=1
        fi
        if [ "$travel_wrong" = 1 ] && [ "$fix" = 1 ]; then
            FIXES=$((FIXES + 1))
            set_conf "$ws" InstantFlightPaths 2
            set_conf "$ws" AllFlightPaths     1
            set_conf "$ws" TaxiFlightSpeed    50.0
            ok "worldserver.conf travel keys set to InstantFlightPaths=2 AllFlightPaths=1 TaxiFlightSpeed=50.0"
        fi
    fi
    if [ -f "$pb" ] && [ "$(conf_get "$pb" Playerbots.MaxRandomBots)" != "$BOTS" ]; then
        bad "playerbots.conf bots is $(conf_get "$pb" Playerbots.MaxRandomBots), state says $BOTS"
        if [ "$fix" = 1 ]; then FIXES=$((FIXES + 1)); set_conf "$pb" Playerbots.MinRandomBots "$BOTS"; set_conf "$pb" Playerbots.MaxRandomBots "$BOTS"; ok "bots set to $BOTS"; fi
    else
        ok "playerbots.conf bots = ${BOTS}"
    fi
    if [ -f "$hc" ]; then
        local hc_policy
        hc_policy="$(conf_get "$hc" Hardcore.DeathPolicy)"
        case "$hc_policy" in
            ghost|archive|delete) ok "mod_hardcore.conf: DeathPolicy=$hc_policy" ;;
            *)
                bad "mod_hardcore.conf: DeathPolicy is '$hc_policy' (expected ghost, archive or delete)"
                if [ "$fix" = 1 ]; then FIXES=$((FIXES + 1)); set_conf "$hc" Hardcore.DeathPolicy ghost; ok "DeathPolicy set to ghost"; fi
                ;;
        esac
    fi
    if [ -f "$sv" ]; then
        # Every key the module reads, with the range it accepts. A missing key is filled with the
        # module's own fallback at load and a bad one is ignored the same way, so a config that
        # still starts the server can be wrong here without saying so.
        local sv_default sv_key sv_val sv_bad=""
        for sv_default in "${SURVIVOR_CONF_DEFAULTS[@]}"; do
            sv_key="${sv_default%%=*}"
            sv_val="$(conf_get "$sv" "$sv_key")"
            if ! survivor_value_ok "$sv_key" "$sv_val"; then sv_bad="$sv_bad $sv_key=$sv_val"; fi
        done
        if [ -z "$sv_bad" ]; then
            ok "mod-survivor.conf: Enable=$(conf_get "$sv" Survivor.Enable) MinLevelToFlag=$(conf_get "$sv" Survivor.MinLevelToFlag) DropEquipped=$(conf_get "$sv" Survivor.DropEquipped) DropBags=$(conf_get "$sv" Survivor.DropBags) ApplyToBots=$(conf_get "$sv" Survivor.ApplyToBots)"
        else
            bad "mod-survivor.conf: unusable value(s):$sv_bad"
            if [ "$fix" = 1 ]; then
                FIXES=$((FIXES + 1))
                for sv_default in "${SURVIVOR_CONF_DEFAULTS[@]}"; do
                    sv_key="${sv_default%%=*}"
                    if ! survivor_value_ok "$sv_key" "$(conf_get "$sv" "$sv_key")"; then
                        set_conf "$sv" "$sv_key" "${sv_default#*=}"
                    fi
                done
                ok "survivor keys restored to their shipped defaults"
            fi
        fi
    fi
    if [ -f "$pw" ]; then
        local pw_default pw_key pw_val pw_bad=""
        for pw_default in "${PAWN_CONF_DEFAULTS[@]}"; do
            pw_key="${pw_default%%=*}"
            pw_val="$(conf_get "$pw" "$pw_key")"
            if ! pawn_value_ok "$pw_key" "$pw_val"; then pw_bad="$pw_bad $pw_key=$pw_val"; fi
        done
        if [ -z "$pw_bad" ]; then
            ok "mod-pawn.conf: Enable=$(conf_get "$pw" Pawn.Enable) MaxPawns=$(conf_get "$pw" Pawn.MaxPawns) LevelWindow=$(conf_get "$pw" Pawn.LevelWindow) ObeliskEntry=$(conf_get "$pw" Pawn.ObeliskEntry)"
        else
            bad "mod-pawn.conf: unusable value(s):$pw_bad"
            if [ "$fix" = 1 ]; then
                FIXES=$((FIXES + 1))
                for pw_default in "${PAWN_CONF_DEFAULTS[@]}"; do
                    pw_key="${pw_default%%=*}"
                    if ! pawn_value_ok "$pw_key" "$(conf_get "$pw" "$pw_key")"; then
                        set_conf "$pw" "$pw_key" "${pw_default#*=}"
                    fi
                done
                ok "mod-pawn.conf: values restored"
            fi
        fi
    fi
    if [ -f "$ei" ]; then
        local ei_off="" ei_key
        for ei_key in Enable Loot Gather Talk Use; do
            if [ "$(conf_get "$ei" "EInteract.$ei_key")" != "1" ]; then ei_off="$ei_off $ei_key"; fi
        done
        if [ -z "$ei_off" ]; then
            ok "mod_e_interact.conf: Enable/Loot/Gather/Talk/Use = 1"
        else
            bad "mod_e_interact.conf: off:$ei_off (the key is on, those steps are not)"
            if [ "$fix" = 1 ]; then
                for ei_key in $ei_off; do set_conf "$ei" "EInteract.$ei_key" 1; done
                ok "interact options set to 1"
            fi
        fi
    fi
    if [ -f "$pb" ]; then
        if grep -qE '^[[:space:]]*Playerbots\.RandomBotNearPlayers[[:space:]]*=' "$pb"; then
            ok "playerbots.conf: near-player distribution = $(conf_get "$pb" Playerbots.RandomBotNearPlayers)"
            # Every key the mode reads, with the value the installer writes. A config from an older
            # install can be missing the newer ones, which the binary then fills with its own
            # defaults - so name them rather than silently accepting the difference.
            local near_pair near_key near_val near_missing=0
            for near_pair in \
                "RandomBotNearPlayersLevelRange=5" \
                "RandomBotNearPlayersFollowInterval=120" \
                "RandomBotNearPlayersMinDistance=$NEAR_MIN_DISTANCE" \
                "RandomBotNearPlayersSameFaction=1" \
                "RandomBotNearPlayersMaxPerPlayer=$NEAR_BOTS" \
                "RandomBotNearPlayersZoneTraffic=1" \
                "RandomBotNearPlayersZoneVisitMinTime=$NEAR_VISIT_MIN" \
                "RandomBotNearPlayersZoneVisitMaxTime=$NEAR_VISIT_MAX" \
                "RandomBotNearPlayersZoneAwayMinTime=$NEAR_AWAY_MIN" \
                "RandomBotNearPlayersZoneAwayMaxTime=$NEAR_AWAY_MAX"
            do
                near_key="${near_pair%%=*}"
                near_val="${near_pair#*=}"
                if grep -qE "^[[:space:]]*Playerbots\\.${near_key}[[:space:]]*=" "$pb"; then
                    continue
                fi

                near_missing=1
                bad "playerbots.conf: $near_key missing (old artifact or config)"
                if [ "$fix" = 1 ]; then
                    FIXES=$((FIXES + 1))
                    set_conf "$pb" "Playerbots.$near_key" "$near_val"
                    ok "$near_key set to $near_val"
                fi
            done
            if [ "$near_missing" = 0 ]; then
                ok "playerbots.conf: zone bots max=$(conf_get "$pb" Playerbots.RandomBotNearPlayersMaxPerPlayer) min-distance=$(conf_get "$pb" Playerbots.RandomBotNearPlayersMinDistance) traffic=$(conf_get "$pb" Playerbots.RandomBotNearPlayersZoneTraffic) visit=$(conf_get "$pb" Playerbots.RandomBotNearPlayersZoneVisitMinTime)-$(conf_get "$pb" Playerbots.RandomBotNearPlayersZoneVisitMaxTime)s away=$(conf_get "$pb" Playerbots.RandomBotNearPlayersZoneAwayMinTime)-$(conf_get "$pb" Playerbots.RandomBotNearPlayersZoneAwayMaxTime)s"
            fi
            if grep -qE '^[[:space:]]*Playerbots\.RandomBotNearPlayers(Radius|InnerCount|OuterRadius|ZoneCount)[[:space:]]*=' "$pb"; then
                warn "playerbots.conf: twin-ring keys are still in the config; the current build ignores them"
                if [ "$fix" = 1 ]; then
                    FIXES=$((FIXES + 1))
                    sed -i -E '/^[[:space:]]*Playerbots\.RandomBotNearPlayers(Radius|InnerCount|OuterRadius|ZoneCount)[[:space:]]*=/d' "$pb"
                    ok "twin-ring keys removed"
                fi
            fi
        else
            bad "playerbots.conf: RandomBotNearPlayers missing (artifact built without the player-centric patch?)"
            if [ "$fix" = 1 ]; then
                FIXES=$((FIXES + 1))
                set_conf "$pb" Playerbots.RandomBotNearPlayers 1
                set_conf "$pb" Playerbots.RandomBotNearPlayersLevelRange 5
                set_conf "$pb" Playerbots.RandomBotNearPlayersFollowInterval 120
                set_conf "$pb" Playerbots.RandomBotNearPlayersMinDistance "$NEAR_MIN_DISTANCE"
                set_conf "$pb" Playerbots.RandomBotNearPlayersSameFaction 1
                set_conf "$pb" Playerbots.RandomBotNearPlayersMaxPerPlayer "$NEAR_BOTS"
                set_conf "$pb" Playerbots.RandomBotNearPlayersZoneTraffic 1
                set_conf "$pb" Playerbots.RandomBotNearPlayersZoneVisitMinTime "$NEAR_VISIT_MIN"
                set_conf "$pb" Playerbots.RandomBotNearPlayersZoneVisitMaxTime "$NEAR_VISIT_MAX"
                set_conf "$pb" Playerbots.RandomBotNearPlayersZoneAwayMinTime "$NEAR_AWAY_MIN"
                set_conf "$pb" Playerbots.RandomBotNearPlayersZoneAwayMaxTime "$NEAR_AWAY_MAX"
                ok "near-player distribution keys added"
            fi
        fi
    fi
    ensure_conf_dir_link

    # 3. helper scripts
    local helper missing_helper=0
    for helper in run.sh stop.sh start-db.sh stop-db.sh set-bots.sh; do
        if [ -x "$ROOT/$helper" ]; then
            ok "helper: $helper"
        else
            bad "helper missing or not executable: $ROOT/$helper"
            missing_helper=1
        fi
    done
    if [ "$missing_helper" = 1 ] && [ "$fix" = 1 ]; then
        FIXES=$((FIXES + 1))
        warn "rewriting the generated helper scripts"
        write_helpers || true
    fi

    # 4. client data and SQL sources
    local d
    for d in dbc maps vmaps mmaps; do
        if [ -d "$DATA_DIR/$d" ]; then ok "client data: $d"; else
            bad "client data missing: $DATA_DIR/$d"
            if [ "$fix" = 1 ]; then FIXES=$((FIXES + 1)); ensure_client_data || true; fi
        fi
    done
    for d in "$SRC_DIR/data/sql" "$SRC_DIR/modules/mod-playerbots/data/sql" "$SRC_DIR/modules/mod-creature-gear/data/sql" "$SRC_DIR/modules/mod-survivor/data/sql" "$SRC_DIR/modules/mod-pawn/data/sql"; do
        if [ -d "$d" ]; then ok "sql sources: $d"; else
            bad "sql sources missing: $d (the DB updater cannot run)"
            if [ "$fix" = 1 ]; then warn "re-run the install to restore them: bash thor-install.sh --force"; fi
        fi
    done
    local sql_name sql_label sql_file
    # Only the families that ship. The graveyard patch was generated, reviewed and reverted
    # (docs/GRAVEYARDS-DESIGN.md), so probing for it would report a permanent failure.
    for sql_name in loot_bags travel_portals; do
        case "$sql_name" in
            loot_bags)      sql_label="loot-bag patch" ;;
            travel_portals) sql_label="travel-portal patch" ;;
        esac
        sql_file="$(find "$SRC_DIR/data/sql/custom/db_world" -maxdepth 1 -name "${sql_name}_*.sql" -print -quit 2>/dev/null || true)"
        if [ -n "$sql_file" ]; then
            ok "sql sources: $sql_label ($(basename "$sql_file"))"
        else
            bad "$sql_label missing: $SRC_DIR/data/sql/custom/db_world/${sql_name}_*.sql"
            if [ "$fix" = 1 ]; then warn "re-run the install to restore it: bash thor-install.sh --force"; fi
        fi
    done

    # 5. database
    if ! have mysql; then
        bad "mysql client not found (is this the rootfs?)"
    else
        if db_ping; then
            ok "mysqld answering on $SOCKET"
        else
            bad "mysqld is not answering on $SOCKET"
            if [ "$fix" = 1 ]; then
                if check_needs_root; then FIXES=$((FIXES + 1)); db_start || true; else warn "starting mysqld needs root"; fi
            fi
        fi
        if db_ping; then
            local s
            for s in acore_auth acore_world acore_characters; do
                if db_schema_exists "$s"; then ok "schema: $s"; else
                    bad "schema missing: $s"
                    if [ "$fix" = 1 ] && check_needs_root; then FIXES=$((FIXES + 1)); create_schema || true; fi
                fi
            done
            local world_tables
            world_tables=$(db_table_count acore_world)
            if [ "${world_tables:-0}" -gt 0 ]; then
                ok "acore_world has $world_tables tables"
            else
                bad "acore_world is empty: the first boot never populated it"
                if [ "$fix" = 1 ]; then
                    FIXES=$((FIXES + 1))
                    warn "running the first boot again to populate the databases"
                    first_boot || true
                fi
            fi
            if [ -n "$ACCOUNT" ]; then
                if [ "$(db_query "SELECT COUNT(*) FROM acore_auth.account WHERE username='$ACCOUNT'")" = "1" ]; then
                    ok "account: $ACCOUNT"
                else
                    bad "account missing: $ACCOUNT"
                    if [ "$fix" = 1 ] && [ "${world_tables:-0}" -gt 0 ]; then
                        FIXES=$((FIXES + 1))
                        if world_console 300 "account create $ACCOUNT $ACCOUNT_PASS" "account set gmlevel $ACCOUNT 3 -1"; then
                            ok "account $ACCOUNT created"
                        else
                            bad "could not create $ACCOUNT"
                        fi
                    fi
                fi
            fi
        fi
    fi

    # 6. processes
    local pattern
    for pattern in worldserver authserver; do
        if pgrep -f "$BIN_DIR/$pattern" >/dev/null 2>&1; then ok "$pattern is running"; else log "--    $pattern is not running"; fi
    done
}

layout_dirs() {   # recover BIN_DIR/CONF_DIR/DATA_DIR/SRC_DIR from what exists on disk
    local candidate
    BIN_DIR=""
    for candidate in "$ROOT/bin" "$ROOT/env/dist/bin"; do
        if [ -d "$candidate" ]; then BIN_DIR="$candidate"; break; fi
    done
    if [ -d "$ROOT/env/dist/etc" ]; then CONF_DIR="$ROOT/env/dist/etc"; else CONF_DIR="$ROOT/etc"; fi
    DATA_DIR="$ROOT/data"
    [ -n "$SRC_DIR" ] || SRC_DIR="$ROOT/src"
}

# The prebuilt server is linked against ubuntu-24.04-arm (see .github/workflows/server-aarch64.yml),
# so a library it needs has to come from that release when the rootfs is a newer one. MySQL is the
# case that bites: 24.04 ships 8.0 with soname libmysqlclient.so.21, 25.04 moved to 8.4, and ICU went
# from .so.74 to .so.76 - same library, different soname, so apt in the newer release cannot satisfy
# a binary built against the older one no matter which package it installs.
REFERENCE_RELEASE="noble"
REFERENCE_VERSION="24.04"
PORTS_MIRROR="${THOR_PORTS_MIRROR:-http://ports.ubuntu.com/ubuntu-ports}"

reference_deb_for_soname() {   # pool path of the deb in $REFERENCE_RELEASE that carries the soname
    case "$1" in
        libmysqlclient.so.21)
            echo "pool/main/m/mysql-8.0/libmysqlclient21_8.0.36-2ubuntu3_arm64.deb" ;;
        libicuuc.so.74|libicui18n.so.74|libicudata.so.74)
            echo "pool/main/i/icu/libicu74_74.2-1ubuntu3.1_arm64.deb" ;;
        *)  echo "" ;;
    esac
}

soname_resolves() {   # soname_resolves SONAME - can the loader find it *now*?
    local soname="$1" dir
    if have ldconfig && ldconfig -p 2>/dev/null | grep -q "[[:space:]]$soname[[:space:]]"; then
        return 0
    fi
    # No ldconfig, or its cache is stale: look where the loader looks.
    for dir in /lib /usr/lib /lib/aarch64-linux-gnu /usr/lib/aarch64-linux-gnu; do
        [ -e "$dir/$soname" ] && return 0
    done
    return 1
}

refresh_ld_cache() {   # dpkg's triggers do this, but they are easily skipped inside proot
    have ldconfig && ldconfig >/dev/null 2>&1 || true
}

install_reference_deb() {   # install_reference_deb SONAME - take that library from $REFERENCE_RELEASE
    local soname="$1" path deb out
    path=$(reference_deb_for_soname "$soname")
    [ -n "$path" ] || return 1
    [ "$(uname -m)" = "aarch64" ] || return 1    # the pool paths above are arm64
    have curl || return 1
    have apt-get || return 1
    deb="/tmp/${path##*/}"
    if [ "$CHECK" = 1 ]; then
        note "curl -fsSL $PORTS_MIRROR/$path -o $deb && apt-get install -y ./$deb   # for $soname"
        return 0
    fi
    log "taking ${path##*/} from Ubuntu $REFERENCE_VERSION, which is what the server was built against"
    rm -f "$deb"
    if ! curl -fsSL --retry 3 -o "$deb" "$PORTS_MIRROR/$path" || [ ! -s "$deb" ]; then
        warn "could not download $PORTS_MIRROR/$path"
        return 1
    fi
    # apt, not dpkg: the deb declares dependencies that a later release may only carry under
    # another name, and only apt fetches them. Its output is kept rather than discarded, because
    # swallowing it is what turns a fixable dependency into a bare "cannot open shared object
    # file" with nothing after it.
    if ! out=$(cd /tmp && apt-get install -y "./${path##*/}" 2>&1); then
        warn "installing ${path##*/} failed:"
        while IFS= read -r line; do warn "  $line"; done <<< "$(printf '%s\n' "$out" | tail -5)"
        rm -f "$deb"
        return 1
    fi
    rm -f "$deb"
    refresh_ld_cache
    return 0
}

packages_for_soname() {   # candidate packages for a soname, best first
    case "$1" in
        libssl.so.*|libcrypto.so.*) echo "libssl3t64 libssl3" ;;
        libreadline.so.*)           echo "libreadline8t64 libreadline8" ;;
        libbz2.so.*)                echo "libbz2-1.0" ;;
        liblzma.so.*)               echo "liblzma5" ;;
        libzstd.so.*)               echo "libzstd1" ;;
        libicuuc.so.*|libicui18n.so.*|libicudata.so.*) echo "libicu74" ;;
        libz.so.*)                  echo "zlib1g" ;;
        libncurses.so.*)            echo "libncurses6" ;;
        libncursesw.so.*)           echo "libncursesw6" ;;
        libtinfo.so.*)              echo "libtinfo6" ;;
        libcrypt.so.*)              echo "libcrypt1" ;;
        libboost_*.so.*)            echo "libboost-all-dev" ;;
        libmysqlclient.so.*)        echo "libmysqlclient21 libmysqlclient-dev mysql-client-core-8.0" ;;
        libstdc++.so.*)             echo "libstdc++6" ;;
        libgcc_s.so.*)              echo "libgcc-s1" ;;
        *)                          echo "" ;;
    esac
}

install_for_soname() {   # first candidate apt accepts *and* which leaves the soname loadable
    local soname="$1" candidate
    for candidate in $(packages_for_soname "$soname"); do
        if [ "$CHECK" = 1 ]; then
            note "apt-get install -y --no-install-recommends $candidate   # for $soname"
            return 0
        fi
        apt-get install -y --no-install-recommends "$candidate" >/dev/null 2>&1 || continue
        refresh_ld_cache
        # apt succeeding is not the answer to the question. On a release that renamed the library
        # the package installs a *different* soname, the binary keeps failing to load, and the
        # install has to keep looking rather than report success.
        if soname_resolves "$soname"; then
            log "installed $candidate for $soname"
            return 0
        fi
        log "$candidate does not provide $soname on this release"
    done
    if install_reference_deb "$soname" && soname_resolves "$soname"; then
        ok "$soname: installed from Ubuntu $REFERENCE_VERSION"
        return 0
    fi
    return 1
}

install_missing_libs() {   # what the binaries cannot load, resolved from ldd not from guesswork
    local bin libs soname unresolved=""
    for bin in worldserver authserver; do
        libs=$(missing_libs "$BIN_DIR/$bin")
        [ -n "$libs" ] || continue
        for soname in $libs; do
            if install_for_soname "$soname"; then
                ok "$bin: package installed for $soname"
            else
                unresolved="$unresolved $soname"
            fi
        done
    done
    if [ -n "$unresolved" ]; then
        warn "no known package for:$unresolved - install them by hand, then re-run --status"
        return 1
    fi
    return 0
}

# ---------------------------------------------------------------- inside stage: uninstall

inside_uninstall() {
    state_load || true
    state_apply_cli
    if [ -z "$BIN_DIR" ]; then layout_dirs; fi

    step "Stopping the server"
    if have pkill; then
        if pkill -f "$BIN_DIR/worldserver" 2>/dev/null; then log "stopped worldserver"; else log "worldserver was not running"; fi
        if pkill -f "$BIN_DIR/authserver" 2>/dev/null; then log "stopped authserver"; else log "authserver was not running"; fi
    fi

    if [ "$KEEP_DB" = 1 ]; then
        step "Dumping the databases"
        if [ "$CHECK" = 1 ]; then
            note "mysqldump the acore_* schemas to $STATE_DIR/db-dump.sql"
        elif have mysqldump; then
            mkdir -p "$STATE_DIR"
            umask 077
            local s schemas=""
            for s in acore_auth acore_world acore_characters acore_playerbots; do
                if db_schema_exists "$s"; then schemas="$schemas $s"; fi
            done
            if [ -n "$schemas" ]; then
                # shellcheck disable=SC2086  # $schemas is a deliberate word list
                if mysqldump --protocol=socket --socket="$SOCKET" -uroot --single-transaction --routines --events \
                        --databases $schemas > "$STATE_DIR/db-dump.sql"; then
                    log "dumped$schemas to $STATE_DIR/db-dump.sql ($(human_size "$STATE_DIR/db-dump.sql"))"
                else
                    warn "mysqldump failed; is the database running?"
                fi
            else
                warn "no acore_* schema found; nothing to dump"
            fi
            umask 022
        else
            warn "mysqldump not available; skipping the dump"
        fi
    fi

    step "Removing files recorded in the manifest"
    if [ "$CHECK" = 1 ]; then
        note "remove the paths in $STATE_DIR/manifest.txt"
    else
        manifest_remove_all
    fi

    step "Stopping the database"
    if [ "$KEEP_DB" = 1 ]; then
        log "keeping $DB_DIR - the host moves the dump out and the install root is removed next"
    fi
    if [ "$CHECK" = 0 ] && db_ping; then db_stop; log "stopped mysqld"; fi

    if [ "$KEEP_DB" = 1 ]; then
        log "database directory $DB_DIR will be removed with the install root; the dump is what survives"
    fi
    log "inside cleanup done; the host stage removes $ROOT and the script copy"
}

# ---------------------------------------------------------------- entry point

main() {
    parse_args "$@"
    default_credentials
    case "$ACTION" in
        install)
            if [ "$MODE" = "host" ]; then host_install; else stage_inside; fi
            ;;
        repair)
            if [ "$MODE" = "host" ]; then host_repair; else inside_repair; fi
            ;;
        uninstall)
            if [ "$MODE" = "host" ]; then host_uninstall; else inside_uninstall; fi
            ;;
        status)
            if [ "$MODE" = "host" ]; then host_status; else inside_status; fi
            ;;
        *) die "unknown action: $ACTION" ;;
    esac
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    main "$@"
fi
