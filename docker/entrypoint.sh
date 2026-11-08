#!/usr/bin/env bash
# docker/entrypoint.sh - bring this realm up inside a container.
#
# Six steps, each idempotent, in the order the core needs them:
#   1. wait for the db service to answer
#   2. create the schemas and the realm's MySQL user
#   3. install the client data into the data volume
#   4. write the confs with scripts/write-confs.sh
#   5. first boot only: populate the databases and create the GM account
#   6. run authserver and worldserver until the container is stopped
#
# Everything it writes lives under /opt/acore (the data, logs and state volumes) or /tmp.

set -euo pipefail

log() { printf 'entrypoint: %s\n' "$*"; }
warn() { printf 'entrypoint: warning: %s\n' "$*" >&2; }
die() { printf 'entrypoint: %s\n' "$*" >&2; exit 1; }

# --- inputs ------------------------------------------------------------------------------------
# The defaults repeat the ones docker/docker-compose.yml passes, so a `docker run` with only the
# three passwords set behaves like `docker compose up`. The passwords have no default on purpose:
# a realm that silently starts with a known password is worse than one that refuses to.
DB_HOST="${DB_HOST:-db}"
DB_PORT="${DB_PORT:-3306}"
DB_USER="${DB_USER:-acore}"
DB_PASS="${DB_PASS:-}"
DB_ROOT_PASS="${DB_ROOT_PASS:-}"
BOTS="${BOTS:-50}"
# Milliseconds between random-bot manager passes. The whole population is walked each time, so this
# is what BOTS costs: 500 bots at the stock 20 ms is one saturated core.
BOT_TICK_MS="${BOT_TICK_MS:-20}"
HORDE_RATIO="${HORDE_RATIO:-50}"
BIND_IP="${BIND_IP:-0.0.0.0}"
ACCOUNT="${ACCOUNT:-admin}"
ACCOUNT_PASS="${ACCOUNT_PASS:-}"
DATA_URL="${DATA_URL:-https://github.com/wowgaming/client-data/releases/download/v20.0/data.zip}"
DATA_SHA256="${DATA_SHA256:-a3d4df635ae6c2c8f08052c32a79e0f806955150ad36b014a823dd08a32a4610}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-1800}"
NEAR_BOTS="${NEAR_BOTS:-20}"
NEAR_MIN_DISTANCE="${NEAR_MIN_DISTANCE:-100.0}"
NEAR_VISIT_MIN="${NEAR_VISIT_MIN:-900}"
NEAR_VISIT_MAX="${NEAR_VISIT_MAX:-2400}"
NEAR_AWAY_MIN="${NEAR_AWAY_MIN:-240}"
NEAR_AWAY_MAX="${NEAR_AWAY_MAX:-600}"

ROOT="${ROOT:-/opt/acore}"
CONF="$ROOT/etc"
DATA="$ROOT/data"
LOGS="$ROOT/logs"
STATE="$ROOT/.state"
MYSQL=/usr/bin/mysql
MYSQLADMIN=/usr/bin/mysqladmin
WS_LOG="$LOGS/worldserver.out"
AS_LOG="$LOGS/authserver.out"
# Written once the world has reported "World initialized", removed before every start: the
# compose healthcheck for this service and the -Docker readiness probe both read it.
READY="$STATE/world-ready"
# Written once the authserver accepts on its port, removed whenever it is not: the healthcheck needs
# both, because a realm whose world is up but whose login port is dead cannot be played on.
AUTH_READY="$STATE/auth-ready"
# The Item.dbc patcher as the image carries it (Dockerfile: COPY scripts/item-dbc and
# scripts/loot-bags). Overridable so the check can point it at a stub.
DBC_PATCH_DIR="${DBC_PATCH_DIR:-$ROOT/dbc-patch}"

# The core asks on stdin before creating a database it cannot find (DBUpdater.cpp:228-232). This
# container has no console, so the answer is pre-given: anything missing is created. AC_* is the
# core's own switch for exactly this.
export AC_DISABLE_INTERACTIVE=1

for required in DB_PASS DB_ROOT_PASS ACCOUNT_PASS; do
    [ -n "${!required}" ] || die "$required is empty: set it (docker/docker-compose.yml gives it a default, docker run does not)"
done
case "$BOTS" in ''|*[!0-9]*) die "BOTS must be a number, got '$BOTS'" ;; esac
case "$BOT_TICK_MS" in ''|*[!0-9]*) die "BOT_TICK_MS must be a number of milliseconds, got '$BOT_TICK_MS'" ;; esac
case "$HORDE_RATIO" in ''|*[!0-9]*) die "HORDE_RATIO must be a number, got '$HORDE_RATIO'" ;; esac
# 10# so a leading zero stays decimal: write-confs.sh subtracts from this to get the Alliance side,
# where 010 would be ten in the conf and eight to the arithmetic, and 08 would abort the render
# outright. The length bound comes first so a long digit string cannot wrap into range.
[ "${#HORDE_RATIO}" -le 3 ] || die "HORDE_RATIO must be 0-100, got '$HORDE_RATIO'"
HORDE_RATIO=$((10#$HORDE_RATIO))
[ "$HORDE_RATIO" -le 100 ] || die "HORDE_RATIO must be 0-100, got '$HORDE_RATIO'"
case "$BOOT_TIMEOUT" in ''|*[!0-9]*) die "BOOT_TIMEOUT must be a number, got '$BOOT_TIMEOUT'" ;; esac

# sql_escape VALUE - VALUE as the body of a MySQL string literal. Credentials are user input; a
# quote in one must not be able to turn the CREATE USER below into different SQL.
# Backslash first, quote second. MySQL reads a backslash inside a string literal as an escape, so a
# value ending in `\` would otherwise turn the doubled quote into an escaped one and run on past the
# literal - `\'; GRANT ALL ... -- ` was SQL, not a password.
sql_escape() { printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e "s/'/''/g"; }

# MYSQL_PWD rather than -p: -p puts the password in argv, where every process on the host can read
# it, and the client prints a warning about it on every call.
mysql_root() { MYSQL_PWD="$DB_ROOT_PASS" "$MYSQL" --protocol=TCP -h "$DB_HOST" -P "$DB_PORT" -u root "$@"; }
mysql_realm() { MYSQL_PWD="$DB_PASS" "$MYSQL" --protocol=TCP -h "$DB_HOST" -P "$DB_PORT" -u "$DB_USER" "$@"; }

# --- 1. the database ---------------------------------------------------------------------------
wait_for_db() {
    # Without this the schema SQL races the db container's first initialisation, which takes seconds
    # to create its data directory and listens on a socket only until it is done: the realm would
    # die on "Can't connect to MySQL server" on the first `up` and work on the second.
    local waited=0
    until MYSQL_PWD="$DB_ROOT_PASS" "$MYSQLADMIN" --protocol=TCP -h "$DB_HOST" -P "$DB_PORT" \
            -u root ping >/dev/null 2>&1; do
        if [ "$waited" -ge 120 ]; then
            die "the db container at $DB_HOST:$DB_PORT did not answer mysqladmin ping within 120s; see 'docker compose logs db'"
        fi
        sleep 2
        waited=$((waited + 2))
    done
    log "db is answering on $DB_HOST:$DB_PORT"
}

# --- 2. schemas and the realm's user -----------------------------------------------------------
init_schemas() {
    # '%' and not '127.0.0.1': the realm connects from the acore container, whose address on the
    # compose network is never 127.0.0.1, and MySQL picks the account by the client's host. The
    # 127.0.0.1 account scripts/thor-install.sh writes is right for a single host and refused here.
    #
    # CREATE IF NOT EXISTS plus ALTER USER makes this idempotent and keeps the password in step
    # with DB_PASS across restarts, which is what the mysql image's one-shot MYSQL_PASSWORD cannot
    # do.
    #
    # acore_playerbots is deliberately NOT created here. The playerbots module creates its own
    # database and then applies its base SQL (Playerbots.cpp:59-84: ER_BAD_DB_ERROR ->
    # ModuleDBUpdater::Create -> Populate); a database that already exists but is empty skips the
    # base set, so the module's tables are missing and the world aborts at PrepareStatements with
    # "Table 'acore_playerbots.playerbots_weightscales' doesn't exist". What the module needs from
    # this container is only the grant below, which MySQL accepts for a database that does not exist
    # yet, and AC_DISABLE_INTERACTIVE so its create step does not wait on a question.
    local user pass
    user="$(sql_escape "$DB_USER")"
    pass="$(sql_escape "$DB_PASS")"
    if ! mysql_root <<SQL
CREATE DATABASE IF NOT EXISTS acore_auth;
CREATE DATABASE IF NOT EXISTS acore_world;
CREATE DATABASE IF NOT EXISTS acore_characters;
CREATE USER IF NOT EXISTS '$user'@'%' IDENTIFIED BY '$pass';
ALTER USER '$user'@'%' IDENTIFIED BY '$pass';
GRANT ALL PRIVILEGES ON acore_auth.*       TO '$user'@'%';
GRANT ALL PRIVILEGES ON acore_world.*      TO '$user'@'%';
GRANT ALL PRIVILEGES ON acore_characters.* TO '$user'@'%';
GRANT ALL PRIVILEGES ON acore_playerbots.* TO '$user'@'%';
FLUSH PRIVILEGES;
SQL
    then
        die "mysql rejected the schema setup as root@$DB_HOST (is DB_ROOT_PASS the same one the db service was initialised with?)"
    fi
    log "schemas acore_auth/acore_world/acore_characters and user '$DB_USER'@'%' are in place (acore_playerbots is the module's)"
}

# --- 3. client data ----------------------------------------------------------------------------
ensure_data() {
    if [ -d "$DATA/dbc" ] && [ -d "$DATA/maps" ]; then
        log "client data is already installed in $DATA"
        return 0
    fi

    log "downloading client data from $DATA_URL"
    # /tmp, not the data volume: a truncated download must not be left where the server reads from,
    # and a half-unpacked tree must not look like an installed one - the dbc/ maps/ test above is
    # the only thing standing between a failed unzip and a worldserver that starts with no maps.
    local zip=/tmp/client-data.zip got wrapper
    curl -fL --retry 3 -o "$zip" "$DATA_URL" || die "client data download failed: $DATA_URL"
    got="$(sha256sum "$zip" | awk '{ print $1 }')"
    if [ "$got" != "$DATA_SHA256" ]; then
        rm -f "$zip"
        die "client data checksum mismatch: expected $DATA_SHA256, got $got (a truncated download, or DATA_URL is not the v20.0 archive)"
    fi

    mkdir -p "$DATA"
    unzip -q -o "$zip" -d "$DATA" || { rm -f "$zip"; die "unzip failed on $zip"; }
    # data.zip keeps dbc/ and maps/ at the top. An archive that wraps everything in one directory
    # would unpack to data/<wrapper>/dbc, where the server finds neither, so that wrapper is lifted.
    if [ ! -d "$DATA/dbc" ]; then
        for wrapper in "$DATA"/*/; do
            if [ -d "$wrapper/dbc" ] && [ -d "$wrapper/maps" ]; then
                mv "$wrapper"* "$DATA"/
                rmdir "$wrapper" 2>/dev/null || true
                break
            fi
        done
    fi
    rm -f "$zip"
    if [ ! -d "$DATA/dbc" ] || [ ! -d "$DATA/maps" ]; then
        die "$DATA has no dbc/ and maps/ at the top level after unpacking $DATA_URL"
    fi
    log "client data installed in $DATA"
}

# --- 3b. the loot-bag records in Item.dbc ------------------------------------------------------
# The worldserver loads Item.dbc once, at startup, and the same start imports the bag items into
# item_template, so the file has to be patched before run_realm - not after the realm is up, which is
# what a host-side patch costs: a second boot, and a first boot in which the DB and the DBC disagree.
# The tool drops its own records before appending them (extend_records), so running it on every start
# is a no-op rewrite of a 1.4 MiB file rather than a file that grows.
patch_item_dbc() { # patch_item_dbc - put the 64 bag records into the server's Item.dbc
    local tool="$DBC_PATCH_DIR/item-dbc/patch_item_dbc.py"
    if [ ! -f "$tool" ]; then
        warn "no $tool in this image, so the bag records are not in Item.dbc"
        return 0
    fi
    if [ ! -f "$DATA/dbc/Item.dbc" ]; then
        warn "no $DATA/dbc/Item.dbc, so the bag records were not added"
        return 0
    fi
    if python3 "$tool" --dbc "$DATA/dbc/Item.dbc" --server-dbc "$DATA/dbc" --out /tmp/item-dbc >/dev/null 2>&1; then
        log "Item.dbc: the 64 bag records are in $DATA/dbc/Item.dbc"
    else
        # Not fatal: a realm without them runs, and the bags only lose their DBC half.
        warn "Item.dbc: the bag records could not be added (python3 or the tool failed)"
    fi
}

# --- 4. confs ----------------------------------------------------------------------------------
write_confs() {
    mkdir -p "$LOGS" "$STATE" "$DATA"
    [ -f "$ROOT/write-confs.sh" ] || die "$ROOT/write-confs.sh is missing: the image was built without it"
    # scripts/write-confs.sh is the single source of this realm's conf opinions: the Thor installer
    # and this entrypoint both call it, so a realm started from a container gets the same confs. It
    # defines functions only and sourcing it twice is harmless.
    . "$ROOT/write-confs.sh"

    WC_CONF_DIR="$CONF"
    WC_ROOT="$ROOT"
    WC_SRC_DIR="$ROOT/src"
    WC_DATA_DIR="$DATA"
    WC_MYSQL_EXEC="$MYSQL"
    WC_DB_HOST="$DB_HOST"
    WC_DB_PORT="$DB_PORT"
    WC_DB_USER="$DB_USER"
    WC_DB_PASS="$DB_PASS"
    WC_BOTS="$BOTS"
    WC_BOT_TICK_MS="$BOT_TICK_MS"
    WC_HORDE_RATIO="$HORDE_RATIO"
    WC_BIND_IP="$BIND_IP"
    WC_NEAR_BOTS="$NEAR_BOTS"
    WC_NEAR_MIN_DISTANCE="$NEAR_MIN_DISTANCE"
    WC_NEAR_VISIT_MIN="$NEAR_VISIT_MIN"
    WC_NEAR_VISIT_MAX="$NEAR_VISIT_MAX"
    WC_NEAR_AWAY_MIN="$NEAR_AWAY_MIN"
    WC_NEAR_AWAY_MAX="$NEAR_AWAY_MAX"
    wc_write_all || die "the confs could not be written (the reason is above)"
}

# --- 5. first boot -----------------------------------------------------------------------------
# account_exists: 0 the account is there, 1 it is not, 2 the query could not run at all (an empty
# auth database before the first boot, or a database that is not answering).
account_exists() {
    local n
    n="$(mysql_realm -N -B -e \
        "SELECT COUNT(*) FROM acore_auth.account WHERE username = '$(sql_escape "$ACCOUNT")'" 2>/dev/null)" || return 2
    [ "${n:-0}" -gt 0 ]
}

first_boot() {
    log "first boot: starting worldserver to populate the databases (up to ${BOOT_TIMEOUT}s)"
    local fifo=/tmp/worldserver.fifo wslog="$WS_LOG" ws pid rc=0 waited=0 cmd
    local bootconf=/tmp/worldserver.first-boot.conf
    rm -f "$fifo"
    mkfifo "$fifo"
    : > "$wslog"

    # The account commands below are read by the core's CLI thread (CliRunnable), and
    # scripts/write-confs.sh turns that thread off with Console.Enable=0 because a headless realm
    # does not want readline on its stdin. The first boot does: with the thread off nothing reads
    # the fifo and the account is never created. A copy of the conf carries the one changed key
    # rather than flipping the live file, because a first boot that dies half way must not leave
    # Console.Enable=1 behind - the run phase's stdin is /dev/null, the CLI reads EOF, and the world
    # would stop the moment it started.
    sed -E 's/^([[:space:]]*Console\.Enable[[:space:]]*=[[:space:]]*).*$/\1 1/' \
        "$CONF/worldserver.conf" > "$bootconf"
    grep -qE '^[[:space:]]*Console\.Enable[[:space:]]*=[[:space:]]*1$' "$bootconf" \
        || die "could not turn the console on for the first boot: $CONF/worldserver.conf has no Console.Enable line"

    ( cd "$ROOT" && exec ./bin/worldserver -c "$bootconf" ) < "$fifo" > "$wslog" 2>&1 &
    ws=$!
    # So a SIGTERM during the import reaches this worldserver through on_signal instead of being
    # left to docker's grace-period SIGKILL.
    WS_PID=$ws
    # Read-write, so opening the fifo cannot block when worldserver died before opening its end: a
    # plain write-open waits for a reader that a dead process never provides, and the entrypoint
    # would hang until the container was killed instead of reporting the failure.
    exec 9<> "$fifo"

    while :; do
        # The line is "WORLD: World Initialized In ..." on the server.loading logger, which the
        # Console and Server appenders both take; grep both sinks so a conf with the console
        # appender turned off still works.
        if grep -qi 'world initialized' "$wslog" "$LOGS/Server.log" 2>/dev/null; then break; fi
        if [ "$STOPPING" -ne 0 ]; then
            exec 9>&-
            wait "$ws" 2>/dev/null || true
            rm -f "$fifo"
            die "stopped during the first boot; start the container again and it will finish it"
        fi
        if ! kill -0 "$ws" 2>/dev/null; then
            exec 9>&-
            rm -f "$fifo"
            printf 'entrypoint: worldserver exited before the world came up; last lines of %s:\n' "$wslog" >&2
            tail -n 40 "$wslog" >&2 || true
            die "the first boot did not finish; the SQL import is the usual cause (see the log above)"
        fi
        if [ "$waited" -ge "$BOOT_TIMEOUT" ]; then
            exec 9>&-
            kill -TERM "$ws" 2>/dev/null || true
            wait "$ws" 2>/dev/null || true
            rm -f "$fifo"
            printf 'entrypoint: worldserver did not report "World initialized" within %ss; last lines of %s:\n' \
                "$BOOT_TIMEOUT" "$wslog" >&2
            tail -n 40 "$wslog" >&2 || true
            die "the first boot timed out; raise BOOT_TIMEOUT if the host is just slow"
        fi
        sleep 1
        waited=$((waited + 1))
    done
    log "the world is up; creating the GM account '$ACCOUNT'"

    # The console is readline over the fifo. Give each command a moment to be read and answered
    # before the next one, and let the server's own shutdown finish rather than killing it.
    for cmd in "account create $ACCOUNT $ACCOUNT_PASS" "account set gmlevel $ACCOUNT 3 -1"; do
        printf '%s\n' "$cmd" >&9
        sleep 3
    done
    printf 'server shutdown 1\n' >&9
    exec 9>&-

    for pid in $(seq 1 60); do
        kill -0 "$ws" 2>/dev/null || break
        sleep 1
    done
    if kill -0 "$ws" 2>/dev/null; then
        warn "worldserver was still running 60s after 'server shutdown 1'; stopping it"
        kill -TERM "$ws" 2>/dev/null || true
    fi
    wait "$ws" 2>/dev/null || rc=$?
    rm -f "$fifo" "$bootconf"
    if [ "$rc" -ne 0 ] && [ "$rc" -ne 143 ]; then
        warn "the first boot's worldserver exited with status $rc"
    fi
    log "first boot finished"
}

first_boot_if_needed() {
    local marker="$STATE/first-boot.done" status=0
    account_exists || status=$?
    if [ "$status" -eq 0 ]; then
        log "account '$ACCOUNT' already exists; skipping the first boot"
        return 0
    fi
    if [ "$status" -eq 2 ] && [ -f "$marker" ]; then
        # The account query could not run but this realm has booted before: re-running the first
        # boot would import every SQL source again for nothing. The query stays authoritative, so a
        # `down -v` of the db volume without the state volume still gets a fresh first boot.
        log "first boot already ran ($marker); the account table is not readable, continuing"
        return 0
    fi
    first_boot
    printf 'first boot %s, account %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$ACCOUNT" > "$marker"
    if ! account_exists; then
        warn "could not confirm that account '$ACCOUNT' was created; check acore_auth.account"
    fi
}

WS_PID=''
AS_PID=''
WS_OFFSET=0
STOPPING=0

on_signal() {
    # Docker sends SIGTERM on `docker compose down`/`stop`, and bash as PID 1 does not pass it on to
    # its children. The core turns SIGTERM into an orderly World::StopNow, so forwarding the signal
    # is the clean shutdown; killing worldserver outright is what leaves half-written character,
    # auction and mail tables behind.
    STOPPING=1
    local pid
    for pid in "${WS_PID:-}" "${AS_PID:-}"; do
        case "$pid" in ''|0) continue ;; esac
        kill -TERM "$pid" 2>/dev/null || true
    done
}

# --- 6. the realm ------------------------------------------------------------------------------
# The authserver is the only process that answers the login port, and it is not part of the world:
# when it exited here, the container stayed "healthy" on the world marker alone and the client was
# disconnected at the login screen with a running realm reported. It is supervised for the whole
# container lifetime below, and it is started only after the world is up - see start_auth.

auth_port() { # auth_port - the port the authserver binds, as its own conf has it
    local port
    port="$(sed -n 's/^RealmServerPort *= *\([0-9]\{1,\}\).*/\1/p' "$CONF/authserver.conf" 2>/dev/null | head -1)"
    printf '%s\n' "${port:-3724}"
}

port_is_open() { # port_is_open PORT - true when a listener accepts on the loopback
    (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null
}

realm_id() { # realm_id - the realm this worldserver owns, as its own conf has it
    local id
    id="$(sed -n 's/^RealmID *= *\([0-9]\{1,\}\).*/\1/p' "$CONF/worldserver.conf" 2>/dev/null | head -1)"
    printf '%s\n' "${id:-1}"
}

# The authserver exits at startup when no realm passes its query, and the query is `WHERE flag <> 3`
# (LoginDatabase.cpp:51): flag 3 is VERSION_MISMATCH|OFFLINE. A worldserver that dies between marking
# its realm VERSION_MISMATCH (worldserver/Main.cpp:294) and clearing it once the world is ready
# (:383) - a crash, or `docker stop` during the boot - leaves flag 1 behind, and the authserver's own
# "every realm offline" UPDATE (:135) turns that into 3. Nothing then clears the bit before the
# authserver reads it on the next boot, so the realm stays unloggable while the restarted
# worldserver quietly clears the bit and makes the database look healthy again. Clear it first, for
# this realm only and only that bit: VERSION_MISMATCH is what the worldserver owns.
clear_realm_version_mismatch() {
    local id
    id="$(realm_id)"
    mysql_realm acore_auth -e "UPDATE realmlist SET flag = flag & 254 WHERE id = $id;" >/dev/null 2>&1 ||
        warn "could not clear realmlist.flag for realm $id (the authserver may refuse to start)"
}

# Started only after the world reports "World initialized", which is also when the worldserver
# clears VERSION_MISMATCH from its realm row. An authserver that starts inside the world's startup
# window ORs OFFLINE onto that bit, sees flag 3, and exits with "No valid realms specified" - the
# failure this ordering removes rather than retries inside.
start_auth() {
    clear_realm_version_mismatch
    "$ROOT/bin/authserver" -c "$CONF/authserver.conf" >> "$AS_LOG" 2>&1 &
    AS_PID=$!
    log "authserver started (pid $AS_PID)"
}

# Both markers live in the state volume, so they survive a container restart: a container that is
# still booting must not look ready to the compose healthcheck or to windows-install.ps1 -Docker,
# which reads the health. Removed before anything slow runs, not when the worldserver starts - a
# stale world-ready from the previous boot is what made -Docker believe a fresh start was already
# up and restart the container 7 s into the world's boot.
clear_readiness() {
    rm -f "$READY" "$AUTH_READY"
}

start_world() {
    WS_OFFSET=0
    if [ -f "$WS_LOG" ]; then WS_OFFSET=$(wc -c < "$WS_LOG"); fi
    rm -f "$READY"
    "$ROOT/bin/worldserver" -c "$CONF/worldserver.conf" >> "$WS_LOG" 2>&1 &
    WS_PID=$!
    log "worldserver started (pid $WS_PID)"
}

# Docker Desktop leaves no core in this container - the host's core_pattern pipes to a handler that
# does not exist here - so a segfault shows up only as status 139. Keep what the process wrote last,
# beside the markers, so a crash can be read after the log has scrolled past it.
preserve_crash_tail() { # preserve_crash_tail STATUS
    local out="$STATE/worldserver-exit-$1.log"
    tail -n 200 "$WS_LOG" > "$out" 2>/dev/null || return 0
    warn "kept the last 200 lines of $WS_LOG in $out"
}

run_realm() {
    local rc done_pid tick waited world_up auth_up
    world_up=0
    auth_up=0
    waited=0
    clear_readiness
    start_world

    while [ "$STOPPING" -eq 0 ]; do
        # Readiness is judged on what THIS start writes. worldserver.out is appended to across
        # restarts, so a "World initialized" line left by the previous run would make a world that
        # is still importing - or one that crashes on start - look healthy.
        if [ "$world_up" -eq 0 ]; then
            if tail -c +"$((WS_OFFSET + 1))" "$WS_LOG" 2>/dev/null | grep -qi 'world initialized'; then
                world_up=1
                : > "$READY"
                log "the world is up (pid $WS_PID)"
            elif [ "$waited" -ge "$BOOT_TIMEOUT" ]; then
                warn "worldserver has not reported 'World initialized' after ${BOOT_TIMEOUT}s; still watching"
                waited=$((BOOT_TIMEOUT + 1))
            else
                waited=$((waited + 2))
            fi
        fi

        # The authserver is started once the world is up and restarted whenever it is gone - not
        # restarted alongside the worldserver, whose restarts leave it serving: a second authserver
        # would collide on the port and orphan the first.
        if [ "$world_up" -eq 1 ]; then
            if [ -z "${AS_PID:-}" ]; then
                start_auth
            elif [ "$auth_up" -eq 0 ] && port_is_open "$(auth_port)"; then
                auth_up=1
                : > "$AUTH_READY"
                log "authserver is answering on port $(auth_port) (pid $AS_PID)"
            fi
        fi

        # wait -n -p, not wait "$WS_PID": blocking on the worldserver would leave a dead authserver
        # unnoticed for as long as the world runs, which is days. The background sleep bounds the
        # wait, and wait -n -p names which child ended so the right one is restarted.
        done_pid=''
        rc=0
        sleep 2 & tick=$!
        wait -n -p done_pid 2>/dev/null || rc=$?
        kill "$tick" 2>/dev/null || true
        wait "$tick" 2>/dev/null || true
        if [ "$STOPPING" -ne 0 ]; then break; fi

        if [ "$done_pid" = "$WS_PID" ]; then
            warn "worldserver exited with status $rc; restarting in 5s"
            preserve_crash_tail "$rc"
            world_up=0
            waited=0
            rm -f "$READY"
            sleep 5 & wait $! || true
            if [ "$STOPPING" -ne 0 ]; then break; fi
            start_world
        elif [ -n "${AS_PID:-}" ] && [ "$done_pid" = "$AS_PID" ]; then
            warn "authserver exited with status $rc; restarting in 5s"
            auth_up=0
            AS_PID=''
            rm -f "$AUTH_READY"
            sleep 5 & wait $! || true
            if [ "$STOPPING" -ne 0 ]; then break; fi
        fi
    done

    # The trap has already signalled both; wait for them to finish writing before the container ends.
    if [ -n "${WS_PID:-}" ]; then wait "$WS_PID" 2>/dev/null || true; fi
    if [ -n "${AS_PID:-}" ]; then wait "$AS_PID" 2>/dev/null || true; fi
    rm -f "$READY" "$AUTH_READY"
    log "realm stopped"
}

main() {
    # wait -n -p, which lets one loop supervise both servers, needs bash 5.1+; the image is
    # ubuntu:24.04 (bash 5.2). Saying so beats supervising the authserver's death silently.
    if (( BASH_VERSINFO[0] < 5 || (BASH_VERSINFO[0] == 5 && BASH_VERSINFO[1] < 1) )); then
        die "bash 5.1 or newer is required for wait -n -p; this is $BASH_VERSION"
    fi

    # Installed before anything long-running: `docker compose down` during the first boot's SQL
    # import must reach worldserver as SIGTERM too, not as the SIGKILL docker sends after its grace
    # period. on_signal only signals processes this script has recorded a pid for.
    trap on_signal TERM INT
    # Before the slow steps, so nothing that asks "is this realm ready" during a restart can be
    # answered by the previous boot's markers.
    clear_readiness
    wait_for_db
    init_schemas
    ensure_data
    patch_item_dbc
    write_confs
    first_boot_if_needed
    run_realm
}

# Only when executed: a smoke test sources this file to call the functions above without starting a
# realm, the same guard scripts/thor-install.sh uses.
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    main "$@"
fi
