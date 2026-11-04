#!/usr/bin/env bash
# scripts/docker/check_entrypoint.sh - what the container's supervisor has to do, checked without
# a container.
#
# docker/entrypoint.sh runs two independent processes, and the worldserver's own status flags on the
# realm row decide whether the second one can start at all (LoginDatabase.cpp:51 excludes flag 3,
# which the worldserver holds for the whole of its startup). A realm whose supervisor gets that
# order wrong starts, reports healthy on the world alone, and cannot be logged into - which is
# exactly how the login port went dead while `docker compose ps` said `running (healthy)`.
#
# So the cases here are the ones that failure consisted of:
#   - the authserver starts after "World initialized", not before, and its realm flag is cleared first
#   - auth-ready appears only once the port actually accepts
#   - an exited authserver is restarted, and auth-ready is re-established
#   - a worldserver that segfaults is restarted, its last output kept, and the authserver is *not*
#     started a second time (two listeners on one port, one orphaned)
#   - SIGTERM stops both and takes the markers with it
#
# The binaries are stubs and no database is touched: mysql_realm is replaced, so the SQL is asserted
# rather than executed. Runs in well under a minute and needs only bash and python3.

set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# ENTRYPOINT is overridable so the check can be pointed at an older revision of the file,
# which is how it was shown to fail on the supervisor that had this bug.
ENTRYPOINT="${ENTRYPOINT:-$REPO/docker/entrypoint.sh}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
FAILURES=0

fail() { printf 'FAIL %s\n' "$*"; FAILURES=$((FAILURES + 1)); }
pass() { printf 'PASS %s\n' "$*"; }
check() { # check DESCRIPTION COMMAND...
    local description="$1"
    shift
    if "$@"; then pass "$description"; else fail "$description"; fi
}

# --- a fake realm -------------------------------------------------------------------------------
# The port is not 3724: the check must not collide with a realm someone is running on this machine.
PORT=3924
export PORT          # the stubs read it from the environment
REALM_ID=7

mkdir -p "$WORK/bin" "$WORK/etc" "$WORK/logs" "$WORK/.state"

# worldserver: first start reports "World initialized" and then segfaults (139, the status Docker
# reports for a segfaulted child); later starts stay up.
cat > "$WORK/bin/worldserver" <<'STUB'
#!/usr/bin/env bash
n=$(( $(cat "$WORK_RUNS" 2>/dev/null || echo 0) + 1 ))
echo "$n" > "$WORK_RUNS"
sleep 1
echo "World initialized"
if [ "$n" -le 1 ]; then sleep 2; exit 139; fi
exec sleep 600
STUB

# authserver: first start listens for 6 s and then exits 7 (a startup failure such as
# "No valid realms specified"); later starts listen until they are stopped.
cat > "$WORK/bin/authserver" <<'STUB'
#!/usr/bin/env bash
n=$(( $(cat "$WORK_RUNS_AUTH" 2>/dev/null || echo 0) + 1 ))
echo "$n" > "$WORK_RUNS_AUTH"
if [ "$n" -le 1 ]; then
    exec python3 -c '
import socket, sys, time
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", int(sys.argv[1])))
s.listen(8)
s.settimeout(0.2)
end = time.time() + 6
while time.time() < end:
    try:
        c, _ = s.accept()
        c.close()
    except socket.timeout:
        pass
sys.exit(7)
' "$PORT"
fi
exec python3 -c '
import socket, sys
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", int(sys.argv[1])))
s.listen(8)
while True:
    c, _ = s.accept()
    c.close()
' "$PORT"
STUB
chmod +x "$WORK/bin/worldserver" "$WORK/bin/authserver"

printf 'RealmID = %s\n' "$REALM_ID" > "$WORK/etc/worldserver.conf"
printf 'RealmServerPort = %s\n' "$PORT" > "$WORK/etc/authserver.conf"

# --- drive the supervisor -----------------------------------------------------------------------
# ROOT is overridable in the entrypoint for exactly this; the rest of its inputs are the ones a
# container gets, with the passwords set so its argument checks pass.
export ROOT="$WORK"
export WORK_RUNS="$WORK/world.runs" WORK_RUNS_AUTH="$WORK/auth.runs"
export DB_PASS=x DB_ROOT_PASS=x ACCOUNT_PASS=x
export CONF="$WORK/etc" LOGS="$WORK/logs" STATE="$WORK/.state" AS_LOG="$WORK/logs/authserver.out"
export WS_LOG="$WORK/logs/worldserver.out" READY="$WORK/.state/world-ready"
export AUTH_READY="$WORK/.state/auth-ready"

# shellcheck source=../../docker/entrypoint.sh
. "$ENTRYPOINT"
set +euo pipefail

# The SQL helper is the one thing that would need a database. Keep the calls instead of running them.
SQL_LOG="$WORK/sql.log"
mysql_realm() { printf '%s\n' "$*" >> "$SQL_LOG"; return 0; }

# A stale marker from the previous boot must not survive into this one: -Docker reads the health to
# decide the realm is up, and a marker left in the state volume made it restart the container seven
# seconds into the world's boot.
mkdir -p "$STATE"
: > "$READY"
: > "$AUTH_READY"
clear_readiness
check "a stale readiness marker does not survive the start of a boot" test ! -e "$READY" -a ! -e "$AUTH_READY"

trap on_signal TERM INT
( sleep 20; kill -TERM $$ ) &
KILLER=$!
run_realm > "$WORK/supervisor.log" 2>&1
kill "$KILLER" 2>/dev/null

LOG="$WORK/supervisor.log"
order() { # order PATTERN - the line number of the first match, or 0
    grep -n -m1 "$1" "$LOG" 2>/dev/null | cut -d: -f1
}

# --- assertions ---------------------------------------------------------------------------------
world_up=$(order 'the world is up')
auth_start=$(order 'authserver started')
check "the authserver starts only after the world is up" test "$world_up" -gt 0 -a "$auth_start" -gt "$world_up"
check "the realm flag is cleared before the authserver starts" grep -q "acore_auth -e UPDATE realmlist SET flag = flag & 254 WHERE id = $REALM_ID;" "$SQL_LOG"
check "auth-ready is written once the port answers" grep -q "authserver is answering on port $PORT" "$LOG"
check "an exited authserver is restarted" test "$(grep -c 'authserver exited with status 7' "$LOG")" -eq 1
check "the authserver was started twice, not more" test "$(grep -c 'authserver started' "$LOG")" -eq 2
# Not a count of both: while the supervisor is inside a worldserver restart it is not probing, so a
# short-lived first listener can be missed. What must hold is that the authserver which is running at
# the end is the one the port answered for.
pid_of() { grep "$1" "$LOG" | tail -1 | sed 's/.*pid \([0-9][0-9]*\)).*/\1/'; }
check "the port answers for the authserver that ends up running" test "$(pid_of 'authserver started')" = "$(pid_of 'authserver is answering')"

check "a segfaulted worldserver is restarted" grep -q 'worldserver exited with status 139; restarting' "$LOG"
check "the crash keeps the tail of its log" test -s "$WORK/.state/worldserver-exit-139.log"
check "the worldserver restart does not start a second authserver" test "$(grep -c 'authserver started' "$LOG")" -eq 2
check "SIGTERM ends the supervisor" grep -q 'realm stopped' "$LOG"
check "neither marker outlives the realm" test ! -e "$READY" -a ! -e "$AUTH_READY"
check "no stub process is left behind" test -z "$(pgrep -f "$WORK/bin/" || true)"

# --- the boot order in main, with every slow step stubbed ---------------------------------------
# The patch has to run before the worldserver reads Item.dbc, which is a property of main's order -
# so main runs here with the db, the data and the realm replaced by log lines.
ORDER="$WORK/order.log"
: > "$ORDER"
export ORDER
wait_for_db() { printf 'wait_for_db\n' >> "$ORDER"; }
init_schemas() { printf 'init_schemas\n' >> "$ORDER"; }
ensure_data() { printf 'ensure_data\n' >> "$ORDER"; }
write_confs() { printf 'write_confs\n' >> "$ORDER"; }
first_boot_if_needed() { printf 'first_boot_if_needed\n' >> "$ORDER"; }
run_realm() { printf 'run_realm\n' >> "$ORDER"; }

mkdir -p "$WORK/dbc-patch/item-dbc" "$DATA/dbc"
printf 'not really a dbc' > "$DATA/dbc/Item.dbc"
cat > "$WORK/dbc-patch/item-dbc/patch_item_dbc.py" <<'TOOL'
#!/usr/bin/env python3
import os, sys
args = " ".join(sys.argv[1:])
open(os.environ["ORDER"], "a").write("patch_item_dbc " + args + "\n")
TOOL
chmod +x "$WORK/dbc-patch/item-dbc/patch_item_dbc.py"

main
order_line() { grep -n "$1" "$ORDER" | head -1 | cut -d: -f1; }
check "the DBC patch runs in main" test -n "$(order_line '^patch_item_dbc ')"
check "the DBC patch writes the server data directory" grep -q -- "--server-dbc $DATA/dbc" "$ORDER"
check "the DBC patch runs after the client data is unpacked" test "$(order_line '^patch_item_dbc ')" -gt "$(order_line '^ensure_data$')"
check "the DBC patch runs before the realm starts" test "$(order_line '^patch_item_dbc ')" -lt "$(order_line '^run_realm$')"

# A missing tool is a warning, not a failed boot: the bags lose their DBC half, the realm still runs.
mv "$WORK/dbc-patch/item-dbc/patch_item_dbc.py" "$WORK/dbc-patch/item-dbc/patch_item_dbc.py.gone"
if patch_item_dbc 2>/dev/null; then pass "a missing DBC patch tool does not fail the boot"
else fail "a missing DBC patch tool does not fail the boot"; fi
mv "$WORK/dbc-patch/item-dbc/patch_item_dbc.py.gone" "$WORK/dbc-patch/item-dbc/patch_item_dbc.py"

if [ "$FAILURES" -ne 0 ]; then
    printf '\n%d check(s) failed; supervisor log:\n' "$FAILURES"
    sed 's/^/  | /' "$LOG"
    printf '\nauthserver output:\n'
    sed 's/^/  | /' "$WORK/logs/authserver.out" 2>/dev/null || true
    printf '\nworldserver output:\n'
    sed 's/^/  | /' "$WORK/logs/worldserver.out" 2>/dev/null || true
    exit 1
fi
printf '\nentrypoint supervisor checks passed\n'
