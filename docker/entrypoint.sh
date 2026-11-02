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

ROOT=/opt/acore
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

# The core asks on stdin before creating a database it cannot find (DBUpdater.cpp:228-232). This
# container has no console, so the answer is pre-given: anything missing is created. AC_* is the
# core's own switch for exactly this.
export AC_DISABLE_INTERACTIVE=1

for required in DB_PASS DB_ROOT_PASS ACCOUNT_PASS; do
    [ -n "${!required}" ] || die "$required is empty: set it (docker/docker-compose.yml gives it a default, docker run does not)"
done
case "$BOTS" in ''|*[!0-9]*) die "BOTS must be a number, got '$BOTS'" ;; esac
case "$BOOT_TIMEOUT" in ''|*[!0-9]*) die "BOOT_TIMEOUT must be a number, got '$BOOT_TIMEOUT'" ;; esac

# sql_escape VALUE - VALUE as the body of a MySQL string literal. Credentials are user input; a
# quote in one must not be able to turn the CREATE USER below into different SQL.
sql_escape() { printf '%s' "$1" | sed "s/'/''/g"; }

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
    # acore_playerbots is created here, not left to the playerbots module: the core's DBUpdater
    # opens every database in the conf at startup, and for one that is missing it asks on stdin
    # "Do you want to create it?" (DBUpdater.cpp:228-232). This container's worldserver reads stdin
    # from a fifo, so that question blocks the first boot until the timeout, and the module's own
    # updater never gets to run.
    local user pass
    user="$(sql_escape "$DB_USER")"
    pass="$(sql_escape "$DB_PASS")"
    if ! mysql_root <<SQL
CREATE DATABASE IF NOT EXISTS acore_auth;
CREATE DATABASE IF NOT EXISTS acore_world;
CREATE DATABASE IF NOT EXISTS acore_characters;
CREATE DATABASE IF NOT EXISTS acore_playerbots;
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
    log "schemas acore_auth/acore_world/acore_characters/acore_playerbots and user '$DB_USER'@'%' are in place"
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

# --- 6. the realm ------------------------------------------------------------------------------
WS_PID=0
AS_PID=0
STOPPING=0

on_signal() {
    # Docker sends SIGTERM on `docker compose down`/`stop`, and bash as PID 1 does not pass it on to
    # its children. The core turns SIGTERM into an orderly World::StopNow, so forwarding the signal
    # is the clean shutdown; killing worldserver outright is what leaves half-written character,
    # auction and mail tables behind.
    STOPPING=1
    local pid
    for pid in "$WS_PID" "$AS_PID"; do
        case "$pid" in ''|0) continue ;; esac
        kill -TERM "$pid" 2>/dev/null || true
    done
}

run_realm() {
    "$ROOT/bin/authserver" -c "$CONF/authserver.conf" >> "$AS_LOG" 2>&1 &
    AS_PID=$!
    log "authserver started (pid $AS_PID)"

    local rc waited offset
    while [ "$STOPPING" -eq 0 ]; do
        # Readiness is judged on what THIS start writes. worldserver.out is appended to across
        # restarts, so a "World initialized" line left by the previous run would make a world that
        # is still importing - or one that crashes on start - look healthy.
        offset=0
        if [ -f "$WS_LOG" ]; then offset=$(wc -c < "$WS_LOG"); fi
        rm -f "$READY"
        "$ROOT/bin/worldserver" -c "$CONF/worldserver.conf" >> "$WS_LOG" 2>&1 &
        WS_PID=$!
        log "worldserver started (pid $WS_PID)"
        waited=0
        while [ "$STOPPING" -eq 0 ] && [ "$waited" -lt "$BOOT_TIMEOUT" ]; do
            if tail -c +"$((offset + 1))" "$WS_LOG" 2>/dev/null | grep -qi 'world initialized'; then
                # The compose healthcheck and windows-install.ps1 -Docker read the marker and this
                # line: the world's own output goes to the logs volume, never to the container's
                # stdout, so neither of them can see "World initialized" directly.
                : > "$READY"
                log "the world is up (pid $WS_PID)"
                break
            fi
            kill -0 "$WS_PID" 2>/dev/null || break
            sleep 2 &
            wait $! || true
            waited=$((waited + 2))
        done
        rc=0
        wait "$WS_PID" || rc=$?
        if [ "$STOPPING" -ne 0 ]; then break; fi
        warn "worldserver exited with status $rc; restarting in 5s"
        sleep 5 &
        wait $! || true
    done

    # The trap has already signalled both; wait for them to finish writing before the container ends.
    wait "$WS_PID" 2>/dev/null || true
    wait "$AS_PID" 2>/dev/null || true
    log "realm stopped"
}

main() {
    # Installed before anything long-running: `docker compose down` during the first boot's SQL
    # import must reach worldserver as SIGTERM too, not as the SIGKILL docker sends after its grace
    # period. on_signal only signals processes this script has recorded a pid for.
    trap on_signal TERM INT
    wait_for_db
    init_schemas
    ensure_data
    write_confs
    first_boot_if_needed
    run_realm
}

main "$@"
