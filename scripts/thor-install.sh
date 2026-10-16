#!/usr/bin/env bash
#
# thor-install.sh - installs and first-boots the AzerothCore + mod-playerbots server
#                   on an AYN Thor (Snapdragon 8 Gen 2, Android/Termux, aarch64).
#
# Run it inside Termux as a normal user:
#
#     bash thor-install.sh
#
# It installs an Ubuntu arm64 rootfs (proot-distro), then re-runs itself inside that
# rootfs ("--inside") to install the server, initialise MySQL, write the configs,
# populate the databases on first boot, create a GM account and generate the
# start/stop scripts. Re-running is safe: every step checks its own state, and
# --check reports without changing anything.
#
# Step by step description: ../docs/RUNBOOK.md.  Pinned versions: ../pins.toml.
#
# Flags:
#   --check                 report what would happen; change nothing
#   --from-source           build the server in the rootfs instead of downloading the artifact
#   --force                 re-download / overwrite existing server files
#   --bots N                random bots to enable (default 50)
#   --db-user NAME          MySQL user to create      (default acore)
#   --db-pass PASS          MySQL password            (default: generated)
#   --account NAME          GM account to create      (default admin)
#   --account-pass PASS     GM password               (default: generated)
#   --lan-ip IP             bind to the LAN and point the realm at IP (default: loopback only)
#   --root DIR              install root in the rootfs (default /opt/acore)
#   --release-url URL       prebuilt linux/arm64 tarball
#   --data-zip URL|PATH|none  client data (dbc/maps/vmaps/mmaps)
#   --boot-timeout SECS     first-boot wait for "World initialized" (default 1800)
#   -h, --help              this text
#
# Note: the variable holding the install root is ROOT, not PREFIX, because Termux
# already uses $PREFIX for its own usr tree.
#
set -euo pipefail

CORE_REPO="https://github.com/mod-playerbots/azerothcore-wotlk"
CORE_COMMIT="8a4e76eac8c4ccebf547113afe7b51d6c3e91641"
PB_REPO="https://github.com/mod-playerbots/mod-playerbots"
PB_COMMIT="d750b4b3a50effdf22bedc8032f0e8e5f7cb8091"
RELEASE_URL="${THOR_RELEASE_URL:-https://github.com/nick-monto/acore-thor-selfhost/releases/latest/download/acore-server-arm64.tar.zst}"
DATA_URL="https://github.com/wowgaming/client-data/releases/download/v20.0/data.zip"
DATA_SHA256="a3d4df635ae6c2c8f08052c32a79e0f806955150ad36b014a823dd08a32a4610"
ROOT_DEFAULT="/opt/acore"

ROOT="$ROOT_DEFAULT"
BOTS=50
DB_USER="acore"
DB_PASS=""
ACCOUNT="admin"
ACCOUNT_PASS=""
LAN_IP=""
DATA_SRC="$DATA_URL"
BOOT_TIMEOUT=1800
MODE="host"          # host | inside
CHECK=0
FROM_SOURCE=0
FORCE=0

BIN_DIR=""
CONF_DIR=""
DATA_DIR=""
SRC_DIR=""
FIRST_BOOT_OK=1

SCRIPT_PATH="$(readlink -f "${BASH_SOURCE[0]:-$0}")"

# ---------------------------------------------------------------- output helpers

log()  { printf '%s\n' "$*"; }
step() { printf '\n=== %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }
note() { if [ "$CHECK" = 1 ]; then printf '  would: %s\n' "$*"; else "$@"; fi; }

usage() { sed -n '3,32p' "$SCRIPT_PATH" | sed 's/^# \{0,1\}//'; }

# ---------------------------------------------------------------- args

parse_args() {
    while [ $# -gt 0 ]; do
        case "$1" in
            --check)        CHECK=1 ;;
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
}

# ---------------------------------------------------------------- small helpers

random_password() { LC_ALL=C tr -dc 'A-Za-z0-9' < /dev/urandom | head -c 16; }

sha256_file() { sha256sum "$1" | awk '{print $1}'; }

human_size() { du -sh "$1" 2>/dev/null | awk '{print $1}' || true; }

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
        { }
        END { if (found != "") print found }
    ' "$1"
}

baked_conf_dir() { grep -aoE '/[A-Za-z0-9_./-]+/etc' "$1" 2>/dev/null | head -1 || true; }

missing_libs() {   # missing_libs BINARY - runtime libraries the binary cannot find
    have ldd || return 0
    ldd "$1" 2>/dev/null | awk '/not found/ {print $1}' | sort -u
}

# ---------------------------------------------------------------- host stage (Termux)

stage_host() {
    step "Termux preflight"
    [ "$(uname -m)" = "aarch64" ] || die "this script targets aarch64 (got $(uname -m)); run it on the Thor"
    have pkg || die "this does not look like Termux (no 'pkg' command)"
    case "$(uname -o 2>/dev/null)" in
        Android) : ;;
        *) warn "uname -o did not say Android; continuing anyway" ;;
    esac

    local free_kb
    free_kb=$(df -Pk "$HOME" | awk 'NR == 2 {print $4}')
    log "free space in \$HOME: $((free_kb / 1048576)) GB (server + SQL ~4 GB, client data ~1 GB, in-rootfs view)"
    if [ "$free_kb" -lt 6000000 ]; then
        warn "less than 6 GB free in \$HOME; the server files alone are ~4 GB"
    fi

    step "Termux packages"
    note pkg update
    note pkg install -y proot-distro termux-api curl zstd

    step "Keep the CPU awake while the server runs"
    note termux-wake-lock

    local rootfs="${TERMUX_PREFIX:-/data/data/com.termux/files/usr}/var/lib/proot-distro/installed-rootfs/ubuntu"
    step "Ubuntu arm64 rootfs"
    if [ -d "$rootfs" ]; then
        log "already installed: $rootfs"
    else
        note proot-distro install ubuntu
    fi

    step "Running the setup inside the rootfs"
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

    note mkdir -p "$rootfs/opt/thor"
    note cp "$SCRIPT_PATH" "$rootfs$inner"
    note proot-distro login ubuntu -- bash "$inner" "${args[@]}"

    step "Host side finished"
    log "start the server from Termux with:  ~/start-wow-server.sh"
    log "stop it with:                       ~/stop-wow-server.sh"
}

# ---------------------------------------------------------------- inside stage (Ubuntu rootfs)

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
    layout_check
    ensure_client_data
    init_db
    create_schema
    write_configs
    first_boot
    write_helpers
    summary
}

install_deps() {
    step "Ubuntu packages"
    if [ ! -e /usr/sbin/policy-rc.d ]; then
        note sh -c 'printf "#!/bin/sh\nexit 101\n" > /usr/sbin/policy-rc.d; chmod +x /usr/sbin/policy-rc.d'
    fi
    export DEBIAN_FRONTEND=noninteractive
    note apt-get update
    note apt-get install -y --no-install-recommends ca-certificates curl unzip zstd \
        libssl3 libreadline8 zlib1g mysql-client mysql-server
    if [ "$FROM_SOURCE" = 1 ]; then
        note apt-get install -y --no-install-recommends git cmake ninja-build make clang lld \
            libboost-all-dev libssl-dev libreadline-dev zlib1g-dev libncurses-dev libmysqlclient-dev
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
            log "would clone $CORE_REPO@$CORE_COMMIT + $PB_REPO@$PB_COMMIT, cmake+install into $ROOT"
            return 0
        fi
        local src="/opt/acore-src"
        mkdir -p "$src/modules"
        fetch_git "$CORE_REPO" "$CORE_COMMIT" "$src"
        fetch_git "$PB_REPO" "$PB_COMMIT" "$src/modules/mod-playerbots"
        cmake -S "$src" -B "$src/build" -G Ninja \
            -DCMAKE_INSTALL_PREFIX="$ROOT" -DCONF_DIR="$ROOT/etc" \
            -DCMAKE_BUILD_TYPE=RelWithDebInfo -DAPPS_BUILD=all -DTOOLS_BUILD=all \
            -DSCRIPTS=static -DMODULES=static -DBoost_USE_STATIC_LIBS=ON
        cmake --build "$src/build" --parallel "$(nproc)"
        cmake --install "$src/build"
        SRC_DIR="$src"
        return 0
    fi

    step "Prebuilt linux/arm64 server"
    if [ "$CHECK" = 1 ]; then
        local code
        code=$(curl -sIL -o /dev/null -w '%{http_code}' "$RELEASE_URL" 2>/dev/null || echo "000")
        log "release URL: $RELEASE_URL -> HTTP $code"
        if [ "$code" != "200" ]; then
            log "  no release asset yet: push the repo and run .github/workflows/server-aarch64.yml, or use --from-source"
        fi
        return 0
    fi
    local tarball=/tmp/acore-server-arm64.tar.zst
    curl -fL --retry 3 -o "$tarball" "$RELEASE_URL" \
        || die "download failed: $RELEASE_URL (push the repo and let the server-aarch64 workflow publish a release, or re-run with --from-source)"
    if curl -fsL -o "$tarball.sha256" "$RELEASE_URL.sha256" 2>/dev/null; then
        local want got
        want=$(awk '{print $1}' "$tarball.sha256")
        got=$(sha256_file "$tarball")
        [ "$want" = "$got" ] || die "checksum mismatch: expected $want, got $got"
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

layout_check() {
    step "Layout"
    if [ "$CHECK" = 1 ]; then
        log "would look for worldserver under $ROOT/{bin,env/dist/bin} and check its libraries"
        return 0
    fi
    local candidate
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

    local bin lib
    for bin in worldserver authserver; do
        lib=$(missing_libs "$BIN_DIR/$bin")
        if [ -n "$lib" ]; then
            warn "$bin is missing libraries: $(printf '%s' "$lib" | tr '\n' ' ')"
            warn "  install them with: apt-get install -y <package>, then re-run --check"
        fi
    done

    if [ ! -d "$SRC_DIR/data/sql" ]; then
        warn "$SRC_DIR/data/sql is missing: the first boot cannot populate the databases"
    fi
    if [ ! -d "$SRC_DIR/modules/mod-playerbots/data/sql" ]; then
        warn "$SRC_DIR/modules/mod-playerbots/data/sql is missing: the playerbots database cannot be created"
    fi

    local baked
    baked=$(baked_conf_dir "$BIN_DIR/worldserver")
    if [ -n "$baked" ] && [ "$baked" != "$CONF_DIR" ]; then
        # module configs are read from the compile-time _CONF_DIR, never from -c
        if [ -e "$baked" ]; then
            warn "the binary reads module configs from $baked, which exists but is not $CONF_DIR"
        else
            log "linking $baked -> $CONF_DIR (module configs come from the build-time path)"
            mkdir -p "$(dirname "$baked")"
            ln -s "$CONF_DIR" "$baked"
        fi
    fi
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
            warn "no client data requested; worldserver will need DataDir pointing at a directory with dbc/ and maps/"
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

init_db() {
    step "MySQL"
    if [ "$CHECK" = 1 ]; then
        log "would initialise /var/lib/mysql if empty, start mysqld on 127.0.0.1:3306 (socket /run/mysqld/mysqld.sock)"
        return 0
    fi
    mkdir -p /run/mysqld
    chown mysql:mysql /run/mysqld
    if [ ! -d /var/lib/mysql/mysql ]; then
        log "initialising the data directory (first run)"
        mysqld --initialize-insecure --user=mysql --datadir=/var/lib/mysql
    fi
    if mysqladmin --protocol=socket ping >/dev/null 2>&1; then
        log "mysqld already running"
    else
        log "starting mysqld"
        mysqld --user=mysql --datadir=/var/lib/mysql --socket=/run/mysqld/mysqld.sock \
               --bind-address=127.0.0.1 --port=3306 --skip-name-resolve --daemonize
    fi
    local i
    for i in $(seq 1 60); do
        if mysqladmin --protocol=socket ping >/dev/null 2>&1; then break; fi
        sleep 1
        if [ "$i" = 60 ]; then die "mysqld did not come up; see /var/log/mysql/error.log"; fi
    done
    log "server version: $(mysql --protocol=socket -N -B -e 'SELECT VERSION()')"
}

create_schema() {
    step "Databases and user"
    if [ -z "$DB_PASS" ]; then DB_PASS="$(random_password)"; fi
    if [ -z "$ACCOUNT_PASS" ]; then ACCOUNT_PASS="$(random_password)"; fi
    if [ "$CHECK" = 1 ]; then
        log "would create acore_auth, acore_world, acore_characters and the MySQL user '$DB_USER'"
        log "(acore_playerbots is created by the module's own updater on first boot)"
        return 0
    fi
    mysql --protocol=socket -uroot <<SQL
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
    mysql --protocol=socket -uroot -N -B -e \
        "SELECT SCHEMA_NAME FROM information_schema.SCHEMATA WHERE SCHEMA_NAME LIKE 'acore%'"
}

conf_pair() {   # conf_pair NAME - print the live config file, creating it from NAME.conf.dist once
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
        log "would write $ROOT/etc/{authserver,worldserver}.conf, $ROOT/etc/modules/playerbots.conf and $ROOT/logs"
        return 0
    fi
    local bind_ip="$LAN_IP"
    if [ -z "$bind_ip" ]; then bind_ip="127.0.0.1"; fi
    local mysql_bin
    mysql_bin="$(command -v mysql)"

    local ws as pb
    ws="$(conf_pair worldserver)"
    as="$(conf_pair authserver)"
    set_conf "$ws" DataDir                "\"$DATA_DIR\""
    set_conf "$ws" LogsDir                "\"$ROOT/logs\""
    set_conf "$ws" SourceDirectory        "\"$SRC_DIR\""
    set_conf "$ws" MySQLExecutable        "\"$mysql_bin\""
    set_conf "$ws" BindIP                 "\"$bind_ip\""
    set_conf "$ws" WorldServerPort        8085
    set_conf "$ws" Updates.EnableDatabases 7
    set_conf "$ws" Updates.AutoSetup      1
    set_conf "$ws" PlayerLimit            1000
    set_conf "$ws" LoginDatabaseInfo      "\"127.0.0.1;3306;$DB_USER;$DB_PASS;acore_auth\""
    set_conf "$ws" WorldDatabaseInfo      "\"127.0.0.1;3306;$DB_USER;$DB_PASS;acore_world\""
    set_conf "$ws" CharacterDatabaseInfo  "\"127.0.0.1;3306;$DB_USER;$DB_PASS;acore_characters\""
    set_conf "$as" BindIP                 "\"$bind_ip\""
    set_conf "$as" RealmServerPort        3724
    set_conf "$as" Updates.EnableDatabases 1
    set_conf "$as" Updates.AutoSetup      1
    set_conf "$as" LoginDatabaseInfo      "\"127.0.0.1;3306;$DB_USER;$DB_PASS;acore_auth\""

    mkdir -p "$ROOT/logs"
    if [ -f "$CONF_DIR/modules/playerbots.conf.dist" ]; then
        pb="$(conf_pair modules/playerbots)"
        set_conf "$pb" Playerbots.Enabled              1
        set_conf "$pb" Playerbots.RandomBotAutologin    1
        set_conf "$pb" Playerbots.MinRandomBots         "$BOTS"
        set_conf "$pb" Playerbots.MaxRandomBots         "$BOTS"
        set_conf "$pb" PlayerbotsDatabaseInfo           "\"127.0.0.1;3306;$DB_USER;$DB_PASS;acore_playerbots\""
        set_conf "$pb" Playerbots.Updates.EnableDatabases 1
        log "playerbots.conf: enabled=$(conf_get "$pb" Playerbots.Enabled) bots=$(conf_get "$pb" Playerbots.MaxRandomBots)"
    else
        warn "no $CONF_DIR/modules/playerbots.conf.dist - the artifact was built without the module, bots will not load"
    fi
    log "worldserver.conf: DataDir=$(conf_get "$ws" DataDir) BindIP=$(conf_get "$ws" BindIP)"
}

wait_for_log() {   # wait_for_log TIMEOUT PATTERN FILE...
    local timeout="$1" pattern="$2"
    shift 2
    local i f
    for i in $(seq 1 "$timeout"); do
        for f in "$@"; do
            if [ -n "$f" ] && [ -f "$f" ] && grep -qE "$pattern" "$f"; then return 0; fi
        done
        sleep 1
    done
    return 1
}

recent_logs() {   # recent_logs DIR - the 3 most recently modified *.log/*.out files, newest first
    find "$1" -type f \( -name '*.log' -o -name '*.out' \) -printf '%T@ %p\n' 2>/dev/null \
        | sort -rn | head -3 | cut -d' ' -f2-
}

first_boot() {
    step "First boot (populates the databases, then creates the GM account)"
    if [ "$CHECK" = 1 ]; then
        log "would run authserver + worldserver with stdin on a fifo, wait up to ${BOOT_TIMEOUT}s for 'World initialized',"
        log "then send: account create $ACCOUNT *** / account set gmlevel $ACCOUNT 3 -1 / server shutdown 1"
        return 0
    fi
    local fifo="$ROOT/.console.fifo" wslog="$ROOT/logs/worldserver.out" aslog="$ROOT/logs/authserver.out"
    rm -f "$fifo"
    mkfifo "$fifo"
    : > "$wslog"
    : > "$aslog"

    "$BIN_DIR/authserver" -c "$CONF_DIR/authserver.conf" > "$aslog" 2>&1 &
    local as_pid=$!
    "$BIN_DIR/worldserver" -c "$CONF_DIR/worldserver.conf" < "$fifo" > "$wslog" 2>&1 &
    local ws_pid=$!
    exec 9> "$fifo"     # completes the child's open-for-read, and keeps the console open
    log "authserver pid $as_pid, worldserver pid $ws_pid: waiting up to ${BOOT_TIMEOUT}s for the world"

    local -a logs=("$wslog" "$aslog")
    mapfile -t -O 2 logs < <(recent_logs "$ROOT/logs")
    if ! wait_for_log "$BOOT_TIMEOUT" 'World initialized|ready\.\.\.' "${logs[@]}"; then
        warn "worldserver did not report 'World initialized' within ${BOOT_TIMEOUT}s - last lines of $wslog:"
        tail -n 20 "$wslog" >&2 || true
        warn "both processes are still running; leave this window open and watch $wslog, or kill them and re-run"
        FIRST_BOOT_OK=0
        return 0
    fi
    log "world is up"

    printf 'account create %s %s\n' "$ACCOUNT" "$ACCOUNT_PASS" >&9
    sleep 3
    printf 'account set gmlevel %s 3 -1\n' "$ACCOUNT" >&9
    sleep 3
    if [ -n "$LAN_IP" ]; then
        mysql --protocol=socket -uroot acore_auth -e \
            "UPDATE realmlist SET address='$LAN_IP', localAddress='$LAN_IP' WHERE id=1;"
        log "realm 1 pinned to $LAN_IP:8085"
    fi
    if grep -qE 'Account created' "$wslog"; then
        log "account '$ACCOUNT' created"
    else
        warn "could not confirm account creation; check $wslog and the account table"
    fi

    printf 'server shutdown 1\n' >&9
    exec 9>&-
    local i
    for i in $(seq 1 60); do
        if ! kill -0 "$ws_pid" 2>/dev/null; then break; fi
        sleep 1
    done
    kill "$ws_pid" 2>/dev/null || true
    kill "$as_pid" 2>/dev/null || true
    wait "$ws_pid" 2>/dev/null || true
    wait "$as_pid" 2>/dev/null || true
    rm -f "$fifo"
    log "first boot finished"
}

write_helpers() {
    step "Start/stop scripts"
    if [ "$CHECK" = 1 ]; then
        log "would write $ROOT/{run.sh,stop.sh,start-db.sh,stop-db.sh,credentials.txt} and ~/start-wow-server.sh"
        return 0
    fi
    umask 077
    cat > "$ROOT/credentials.txt" <<EOF
Created by thor-install.sh on $(date -u +%Y-%m-%dT%H:%M:%SZ)

database user : $DB_USER
database pass : $DB_PASS
gm account    : $ACCOUNT
gm password   : $ACCOUNT_PASS

MySQL root has no password over the unix socket (reachable from inside the rootfs only).
EOF
    umask 022

    cat > "$ROOT/start-db.sh" <<EOF
#!/usr/bin/env bash
set -euo pipefail
mkdir -p /run/mysqld && chown mysql:mysql /run/mysqld
if ! mysqladmin --protocol=socket ping >/dev/null 2>&1; then
    mysqld --user=mysql --datadir=/var/lib/mysql --socket=/run/mysqld/mysqld.sock \\
           --bind-address=127.0.0.1 --port=3306 --skip-name-resolve --daemonize
fi
for _ in \$(seq 1 60); do
    mysqladmin --protocol=socket ping >/dev/null 2>&1 && exit 0
    sleep 1
done
echo "mysqld did not come up" >&2
exit 1
EOF

    cat > "$ROOT/stop-db.sh" <<'EOF'
#!/usr/bin/env bash
mysqladmin --protocol=socket shutdown 2>/dev/null || true
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
    "$BIN_DIR/worldserver" -c "$CONF_DIR/worldserver.conf" &
    WS_PID=\$!
    wait \$WS_PID
    echo "worldserver exited with \$?; restarting in 10s (Ctrl-C to stop)"
    sleep 10
done
EOF

    cat > "$ROOT/stop.sh" <<'EOF'
#!/usr/bin/env bash
pkill -f 'bin/worldserver' 2>/dev/null || true
pkill -f 'bin/authserver' 2>/dev/null || true
mysqladmin --protocol=socket shutdown 2>/dev/null || true
EOF
    chmod +x "$ROOT/run.sh" "$ROOT/stop.sh" "$ROOT/start-db.sh" "$ROOT/stop-db.sh"
    log "wrote $ROOT/run.sh, $ROOT/stop.sh (and the two db helpers)"

    local home="${TERMUX_HOME:-/data/data/com.termux/files/home}"
    if [ -d "$home" ]; then
        cat > "$home/start-wow-server.sh" <<EOF
#!/data/data/com.termux/files/usr/bin/bash
termux-wake-lock
exec proot-distro login ubuntu -- $ROOT/run.sh
EOF
        cat > "$home/stop-wow-server.sh" <<EOF
#!/data/data/com.termux/files/usr/bin/bash
proot-distro login ubuntu -- $ROOT/stop.sh
termux-wake-unlock
EOF
        chmod +x "$home/start-wow-server.sh" "$home/stop-wow-server.sh"
        log "wrote $home/start-wow-server.sh and $home/stop-wow-server.sh"
    else
        log "not running inside Termux: skipping the ~/start-wow-server.sh wrappers"
    fi
}

summary() {
    step "Summary"
    if [ "$CHECK" = 1 ]; then
        log "check mode finished; nothing was changed"
        return 0
    fi
    if [ "$FIRST_BOOT_OK" = 0 ]; then
        warn "the first boot did not finish; start it again with $ROOT/run.sh and watch $ROOT/logs/worldserver.out"
    fi
    cat <<EOF
server      : $ROOT   (binaries $BIN_DIR, configs $CONF_DIR)
databases   : acore_auth, acore_world, acore_characters, acore_playerbots
bots        : $BOTS (Playerbots.MinRandomBots / MaxRandomBots)
gm account  : $ACCOUNT / $ACCOUNT_PASS  (also in $ROOT/credentials.txt)
bind        : ${LAN_IP:-127.0.0.1}   (loopback unless --lan-ip was given)

Next steps
  1. start it from Termux:  ~/start-wow-server.sh
  2. prove it with a PC 3.3.5a client before touching the launcher: re-run this
     script with --lan-ip <thor-lan-ip>, set "set realmlist <thor-lan-ip>" in the
     client's Data/<locale>/realmlist.wtf, log in, then re-run without --lan-ip.
  3. launcher client half: docs/LAUNCHER-HOSTING-DESIGN.md section 3.
EOF
}

# ---------------------------------------------------------------- entry point

main() {
    parse_args "$@"
    case "$MODE" in
        host)   stage_host ;;
        inside) stage_inside ;;
        *)      die "unknown mode: $MODE" ;;
    esac
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    main "$@"
fi
