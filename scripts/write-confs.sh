# write-confs.sh - this realm's conf opinions, in one place.
#
# Sourced, never executed. It is plain bash (not POSIX sh) and defines only functions and
# variables: no `set -e` of its own, no work at load time, so sourcing it twice is harmless and a
# caller keeps its own error handling.
#
# Callers:
#   - scripts/thor-install.sh, whose write_configs() maps the installer's variables to the WC_*
#     ones below and calls wc_write_all (it also uses the three conf-defaults lists and the five
#     helpers from run_checks);
#   - the Docker entrypoint, which has no installer helpers, so logging falls back to plain
#     stdout/stderr.
#
# The conf content is what matters here: the same keys, the same values and the same comments the
# installer has always written, so a realm started from the tarball and one started from the
# container get byte-identical configs.

if [ -n "${WC_WRITE_CONFS_LOADED:-}" ]; then return 0; fi
WC_WRITE_CONFS_LOADED=1

# Each of these calls the caller's function when it has one (the installer's log/warn/ok, which
# know about --check and formatting) and prints the same text itself when it does not.
wc_log()  { if [ "$(type -t log)"  = function ]; then log  "$*"; else printf '%s\n' "$*"; fi; }
wc_warn() { if [ "$(type -t warn)" = function ]; then warn "$*"; else printf 'warning: %s\n' "$*" >&2; fi; }
wc_ok()   { if [ "$(type -t ok)"   = function ]; then ok   "$*"; else printf 'ok    %s\n' "$*"; fi; }

# mod-survivor's conf keys with the values its shipped .dist carries, as KEY=DEFAULT. The module
# reads all eleven once at load, so a key that is absent or outside its range is silently replaced
# by the module's own fallback; wc_write_all() and the installer's run_checks() restore them from
# this one list.
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
# mod-mercenaries's conf keys with the values its shipped .dist carries, as KEY=DEFAULT. Same
# contract as the survivor list: a key outside its range is silently replaced by the module's own
# fallback, so wc_write_all() restores it from here.
MERCENARY_CONF_DEFAULTS=(
    "Mercenary.Enable=1"
    "Mercenary.MaxMercenaries=2"
    "Mercenary.LevelWindow=2"
    "Mercenary.GainXP=0"
    "Mercenary.DismissGraceSeconds=300"
    "Mercenary.Voice=Talkative: +emote | Quiet: -emote"
    "Mercenary.VoiceKillChancePercent=10"
    "Mercenary.VoiceCooldownSeconds=120"
    "Mercenary.HireFeePercent=100"
    "Mercenary.HireFeeHeldPercent=100"
    "Mercenary.GearFloorPercent=85"
    "Mercenary.GearCapPercent=150"
    "Mercenary.ResupplyFeePercent=100"
    "Mercenary.AffinityEnabled=1"
    "Mercenary.AffinityKillPoints=10"
    "Mercenary.AffinityQuestPoints=25"
    "Mercenary.AffinityStep=100"
    "Mercenary.AffinityGearPercent=5"
    "Mercenary.AffinityMaxSteps=2"
    "Mercenary.OfferedPerPage=12"
    "Mercenary.FeaturedMin=10"
    "Mercenary.FeaturedMax=15"
    "Mercenary.PoolRefreshMinMinutes=40"
    "Mercenary.PoolRefreshMaxMinutes=60"
    "Mercenary.Debug=0"
)
# mod-skateboard's conf keys with the values its shipped .dist carries, as KEY=DEFAULT - the whole
# set the module reads, the same nineteen the contract and the checkers hold. Same contract as the
# survivor list: a key absent from the file or outside its range is silently replaced by the
# module's own fallback, so wc_write_all() and the installer's run_checks() restore it from here.
SKATEBOARD_CONF_DEFAULTS=(
    "Skateboard.Enable=1"
    "Skateboard.Deck=engravedwall"
    "Skateboard.Deck.Render=unit"
    "Skateboard.Deck.Carrier=29443"
    "Skateboard.Deck.FollowMs=40"
    "Skateboard.MaxSpeed=21.0"
    "Skateboard.Cruise=4.0"
    "Skateboard.PushImpulse=4.0"
    "Skateboard.Push.Mode=0"
    "Skateboard.Push.IntervalMs=500"
    "Skateboard.Push.MaxHoldMs=2000"
    "Skateboard.Friction.Static=1.5"
    "Skateboard.Friction.Rolling=0.004"
    "Skateboard.SlopeGain=30.0"
    "Skateboard.Drive=1"
    "Skateboard.Drive.ImpulseMs=350"
    "Skateboard.Drive.GroundMs=100"
    "Skateboard.Drive.MinSpeed=3.0"
    "Skateboard.Brake=6.0"
    "Skateboard.LandingBonus=2.0"
    "Skateboard.Tricks=1"
    "Skateboard.Trick.VisualKit=5036"
    "Skateboard.Trick.List=kickflip:399:5036, ollie:394:5036, grind:388:5036"
    "Skateboard.Crash.Enable=1"
    "Skateboard.Crash.DamagePct=8"
    "Skateboard.Crash.StaggerMs=1500"
    "Skateboard.HardcoreSafe=1"
    "Skateboard.OffOnHit=1"
    "Skateboard.BoardCastMs=2000"
    "Skateboard.AckTimeoutMs=500"
    "Skateboard.Debug=0"
)

wc_conf_pair() {   # wc_conf_pair CONF_DIR NAME - print the live config file, creating it from NAME.conf.dist once
    local dist="$1/$2.conf.dist" live="$1/$2.conf"
    if [ ! -f "$live" ]; then
        [ -f "$dist" ] || { wc_warn "missing $dist (is the artifact complete?)"; return 1; }
        cp "$dist" "$live"
        printf 'created %s\n' "$live" >&2        # never on stdout: callers capture the path
    fi
    printf '%s' "$live"
}

wc_set_conf() {   # wc_set_conf FILE KEY VALUE - replace the first live "KEY = ..." line, else append
    local file="$1" key="$2" value="$3" tmp
    [ -f "$file" ] || { wc_warn "no such config file: $file"; return 1; }
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

wc_get_conf() {   # wc_get_conf FILE KEY - print the live value (last uncommented definition)
    awk -v want="$2" '
        $0 !~ /^[[:space:]]*#/ && $0 ~ /=/ {
            key = $0; sub(/[[:space:]]*=.*/, "", key)
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", key)
            if (key == want) { val = $0; sub(/^[^=]*=[[:space:]]*/, "", val); found = val }
        }
        END { if (found != "") print found }
    ' "$1"
}

# The five helpers below keep their unprefixed names: scripts/mercenaries/check_mercenaries.py
# reads mercenary_value_ok and MERCENARY_CONF_DEFAULTS out of this file with regexes, and
# scripts/skateboard/check_skateboard.py does the same for skateboard_value_ok and
# SKATEBOARD_CONF_DEFAULTS.

mercenary_value_ok() {   # mercenary_value_ok KEY VALUE - true when VALUE is one mod-mercenaries accepts
    # Mercenary.Enable and Mercenary.Debug are 0|1, MaxMercenaries 0-4 (the gossip page has room for four party
    # rows), LevelWindow 0-10, OfferedPerPage 1-14, the featured count 1-50 each, the pool
    # refresh 1-1440 minutes each, the two fee percentages 0-10000 and 0-1000, and the gear
    # band 25-100 for the floor and 100-300 for the cap.
    local key="$1" val="$2"
    # A voice pool is a profile list, not a number - labels, strategies and `|` separators -
    # so it is settled here, before the numeric guard below can see it. A pool is any
    # non-empty value carrying a `:`; the module skips the profiles it cannot parse.
    if [ "$key" = Mercenary.Voice ]; then
        case "$val" in
            ?*:*) return 0 ;;
            *) return 1 ;;
        esac
    fi
    case "$val" in ''|*[!0-9]*) return 1 ;; esac
    case "$key" in
        Mercenary.Enable|Mercenary.Debug)
            [ "$val" -eq 0 ] || [ "$val" -eq 1 ] ;;
        Mercenary.MaxMercenaries)
            [ "$val" -ge 0 ] && [ "$val" -le 4 ] ;;
        Mercenary.LevelWindow)
            [ "$val" -ge 0 ] && [ "$val" -le 10 ] ;;
        Mercenary.GainXP)
            [ "$val" -eq 0 ] || [ "$val" -eq 1 ] ;;
        Mercenary.OfferedPerPage)
            [ "$val" -ge 1 ] && [ "$val" -le 14 ] ;;
        Mercenary.HireFeePercent)
            [ "$val" -ge 0 ] && [ "$val" -le 10000 ] ;;
        Mercenary.HireFeeHeldPercent)
            [ "$val" -ge 0 ] && [ "$val" -le 1000 ] ;;
        Mercenary.GearFloorPercent)
            [ "$val" -ge 25 ] && [ "$val" -le 100 ] ;;
        Mercenary.GearCapPercent)
            [ "$val" -ge 100 ] && [ "$val" -le 300 ] ;;
        Mercenary.ResupplyFeePercent)
            [ "$val" -ge 0 ] && [ "$val" -le 10000 ] ;;
        Mercenary.VoiceKillChancePercent)
            [ "$val" -ge 0 ] && [ "$val" -le 100 ] ;;
        Mercenary.VoiceCooldownSeconds)
            [ "$val" -ge 0 ] && [ "$val" -le 3600 ] ;;
        Mercenary.DismissGraceSeconds)
            # MercenaryMgr.cpp:170 clamps it to 0-3600; without this arm the shipped 300 fell to
            # the catch-all, so every install reported a drift it did not have and rewrote 300.
            [ "$val" -ge 0 ] && [ "$val" -le 3600 ] ;;
        Mercenary.AffinityEnabled)
            [ "$val" -eq 0 ] || [ "$val" -eq 1 ] ;;
        Mercenary.AffinityKillPoints|Mercenary.AffinityQuestPoints)
            [ "$val" -ge 0 ] && [ "$val" -le 1000 ] ;;
        Mercenary.AffinityStep)
            [ "$val" -ge 1 ] && [ "$val" -le 100000 ] ;;
        Mercenary.AffinityGearPercent)
            [ "$val" -ge 0 ] && [ "$val" -le 100 ] ;;
        Mercenary.AffinityMaxSteps)
            [ "$val" -ge 0 ] && [ "$val" -le 20 ] ;;
        Mercenary.FeaturedMin|Mercenary.FeaturedMax)
            [ "$val" -ge 1 ] && [ "$val" -le 50 ] ;;
        Mercenary.PoolRefreshMinMinutes|Mercenary.PoolRefreshMaxMinutes)
            [ "$val" -ge 1 ] && [ "$val" -le 1440 ] ;;
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

skateboard_value_ok() {   # skateboard_value_ok KEY VALUE - true when VALUE is one the module accepts
    # The ranges are the ones the contract states: the 0|1 switches, MaxSpeed 1-40 yd/s, PushImpulse
    # and LandingBonus 0-20 yd/s, the static/rolling friction 0-20 and 0-1, SlopeGain 0-90, the
    # hold-to-pump mode 0|1, its interval 100-2000 ms and cap 100-5000 ms, the carry
    # impulse 100-2000 ms and its ground gap 0-1000 ms, its floor 0-40 yd/s and the brake
    # 0-50 yd/s^2, the visual kit
    # 0-100000 (0 = no flair), crash damage 0-99 percent, the stagger 0-10000 ms and the ACK
    # timeout 50-5000 ms. Deck is a name or a raw display id, not a range, so it is settled
    # before the numeric guard below can see it. The momentum keys carry decimals, so the
    # comparison is awk's rather than a shell integer test.
    local key="$1" val="$2" lo hi
    if [ "$key" = Skateboard.Trick.List ]; then
        # A list, not a number: `name:emote[:kit]` entries separated by commas. Settled here so
        # the numeric guard below never sees it; an entry the module cannot use is dropped by the
        # module itself, which is why anything with a colon is accepted.
        case "$val" in ?*:*) return 0 ;; *) return 1 ;; esac
    fi
    if [ "$key" = Skateboard.Deck.Render ]; then
        # unit or mount, nothing else: the module's own read falls back to unit with a warning,
        # so a typo here would be a silent mode switch on the next boot.
        case "$val" in unit|mount) return 0 ;; *) return 1 ;; esac
    fi
    if [ "$key" = Skateboard.Deck ]; then
        case "$val" in
            log|engravedwall|coffinlid) return 0 ;;
            ''|*[!0-9]*) return 1 ;;
            *) awk -v v="$val" 'BEGIN { exit !(v ~ /^[0-9]+$/ && v + 0 > 0) }'; return ;;
        esac
    fi
    case "$key" in
        Skateboard.Enable|Skateboard.Tricks|Skateboard.Crash.Enable|\
        Skateboard.HardcoreSafe|Skateboard.OffOnHit|Skateboard.Debug|Skateboard.Drive)
            case "$val" in 0|1) return 0 ;; *) return 1 ;; esac ;;
        Skateboard.BoardCastMs)      lo=0;  hi=10000 ;;
        Skateboard.MaxSpeed)         lo=1;  hi=40 ;;
        Skateboard.Cruise)           lo=0;  hi=20 ;;
        Skateboard.Deck.Carrier)     lo=1;  hi=4294967295 ;;
        Skateboard.Deck.FollowMs)    lo=20; hi=1000 ;;
        Skateboard.PushImpulse)      lo=0;  hi=20 ;;
        Skateboard.Push.Mode)        lo=0;  hi=1 ;;
        Skateboard.Push.IntervalMs)  lo=100; hi=2000 ;;
        Skateboard.Push.MaxHoldMs)   lo=100; hi=5000 ;;
        Skateboard.Friction.Static)  lo=0;  hi=20 ;;
        Skateboard.Friction.Rolling) lo=0;  hi=1 ;;
        Skateboard.SlopeGain)        lo=0;  hi=90 ;;
        Skateboard.Drive.ImpulseMs)  lo=100; hi=2000 ;;
        Skateboard.Drive.GroundMs)   lo=0;  hi=1000 ;;
        Skateboard.Drive.MinSpeed)   lo=0;  hi=40 ;;
        Skateboard.Brake)            lo=0;  hi=50 ;;
        Skateboard.LandingBonus)     lo=0;  hi=20 ;;
        Skateboard.Trick.VisualKit)  lo=0;  hi=100000 ;;
        Skateboard.Crash.DamagePct)  lo=0;  hi=99 ;;
        Skateboard.Crash.StaggerMs)  lo=0;  hi=10000 ;;
        Skateboard.AckTimeoutMs)     lo=50; hi=5000 ;;
        *) return 1 ;;
    esac
    awk -v v="$val" -v lo="$lo" -v hi="$hi" \
        'BEGIN { exit !(v ~ /^[0-9]+(\.[0-9]+)?$/ && v + 0 >= lo && v + 0 <= hi) }'
}

mercenary_append_missing_conf() {   # mercenary_append_missing_conf LIVE DIST - append the shipped keys LIVE lacks
    # The pool keys are lists, not scalars, so they stay out of MERCENARY_CONF_DEFAULTS: the checker
    # asserts that array and the conf agree key for key, and wc_set_conf would overwrite a pool the
    # admin had trimmed. But wc_conf_pair() only copies the .dist when the live conf is missing, so
    # an install made before the pools shipped keeps a conf with no Mercenary.Temperament.* block at
    # all, and MercenaryPersonality::Load then has nothing to roll from. Appending the missing keys
    # is the only repair that leaves every existing value alone. The module looks keys up by name,
    # so the additions can go at the end of a hand-edited file without disturbing it.
    local live="$1" dist="$2" line key added=0 pending
    [ -f "$live" ] && [ -f "$dist" ] || return 0
    pending="$(mktemp)"
    while IFS= read -r line || [ -n "$line" ]; do
        key="${line%%=*}"
        key="${key#"${key%%[![:space:]]*}"}"        # trim leading whitespace
        key="${key%"${key##*[![:space:]]}"}"        # and trailing
        case "$key" in
            Mercenary.Voice) ;;
            Mercenary.VoiceLine.*) ;;
            Mercenary.Temperament.[0-9]*.[0-9]*) ;;
            *) continue ;;                          # comments, blanks and every scalar key
        esac
        # An empty value counts as absent here, unlike for a scalar: a pool is never legitimately
        # empty (the module drops an empty one), and wc_get_conf() prints nothing for it.
        [ -z "$(wc_get_conf "$live" "$key")" ] || continue
        printf '%s\n' "$line" >> "$pending"
        added=$((added + 1))
    done < "$dist"
    if [ "$added" -gt 0 ]; then
        {
            printf '\n# Pool keys this file did not have: appended by the installer, values as shipped.\n'
            cat "$pending"
        } >> "$live"
        wc_ok "mod-mercenaries.conf: appended $added shipped pool keys (voice/temperament/blacklist/variant)"
    fi
    rm -f "$pending"
}

# conf_defaults FILE CHECK LABEL DEFAULT... - hold every scalar key this script knows to its shipped
# value, and keep the repair honest about why it was needed.
#
# A key the file does not have at all is not a bad value: the live conf was written by an older
# install and wc_conf_pair() deliberately never overwrites one, so an upgrade leaves the new keys
# absent. They are appended with the values the shipped .dist carries - which is what the module
# would have used anyway - and reported once, as a count, because ten "restoring the shipped
# default" lines for an upgrade that worked read like ten failures.
#
# A key that is present and out of range is a different matter and stays a warning per key: the
# module silently replaces an unusable value with its own default, so the file and the running
# server disagree until the file is fixed.
conf_defaults() {
    local file=$1 check=$2 label=$3; shift 3
    local default key val added=0 fixed=0
    for default in "$@"; do
        key="${default%%=*}"
        val="$(wc_get_conf "$file" "$key")"
        if [ -z "$val" ]; then
            wc_set_conf "$file" "$key" "${default#*=}"
            added=$((added + 1))
        elif ! "$check" "$key" "$val"; then
            wc_warn "$label: $key is '$val', restoring the shipped default ${default#*=}"
            wc_set_conf "$file" "$key" "${default#*=}"
            fixed=$((fixed + 1))
        fi
    done
    if [ "$added" -gt 0 ]; then
        wc_ok "$label: appended $added key(s) the live file did not have yet, values as shipped"
    fi
    if [ "$fixed" -gt 0 ]; then
        wc_ok "$label: restored $fixed drifted key(s) to their shipped defaults"
    fi
    return 0
}

# wc_write_all - write every conf for the realm. The caller sets, before calling:
#   WC_CONF_DIR   <root>/etc, where the .conf/.conf.dist files live (required)
#   WC_ROOT       <root>, for LogsDir and the log directory (required)
#   WC_SRC_DIR    the source checkout worldserver points SourceDirectory at
#   WC_DATA_DIR   the client data directory (default <root>/data)
#   WC_MYSQL_EXEC the mysql client the server runs SQL with
#   WC_DB_USER    / WC_DB_PASS   credentials in the three *DatabaseInfo strings
#   WC_BOTS       playerbots population (Min and Max)
#   WC_BOT_TICK_MS Playerbots.RandomBotUpdateInterval: how often the random-bot manager runs, and
#                   therefore what a large population costs in CPU. Unset leaves the dist value.
#   WC_HORDE_RATIO  percentage of random bots on the Horde side; Alliance is 100 minus it. Empty
#                   leaves the pair alone, which is what the Thor's set-ratio.sh relies on.
#   WC_BIND_IP    the worldserver/authserver bind address
#   WC_NEAR_BOTS, WC_NEAR_MIN_DISTANCE, WC_NEAR_VISIT_MIN, WC_NEAR_VISIT_MAX,
#   WC_NEAR_AWAY_MIN, WC_NEAR_AWAY_MAX  the playerbots near-player window
# It prints the same status lines the installer has always printed, and returns non-zero only on a
# real failure: a missing module conf is a warning, because an artifact built without that module is
# still a usable realm.
wc_write_all() {
    local ws as pb cg hc ei sv pw sb horde
    [ -n "${WC_DATA_DIR:-}" ] || WC_DATA_DIR="$WC_ROOT/data"
    # The database is local to the install on the Thor and on Windows, but a container reaches it
    # through the compose network, so the host and port are inputs like the credentials are.
    [ -n "${WC_DB_HOST:-}" ] || WC_DB_HOST=127.0.0.1
    [ -n "${WC_DB_PORT:-}" ] || WC_DB_PORT=3306

    ws="$(wc_conf_pair "$WC_CONF_DIR" worldserver)" || return 1
    as="$(wc_conf_pair "$WC_CONF_DIR" authserver)" || return 1
    wc_set_conf "$ws" DataDir                "\"$WC_DATA_DIR\""
    wc_set_conf "$ws" LogsDir                "\"$WC_ROOT/logs\""
    wc_set_conf "$ws" SourceDirectory        "\"$WC_SRC_DIR\""
    wc_set_conf "$ws" MySQLExecutable        "\"$WC_MYSQL_EXEC\""
    wc_set_conf "$ws" BindIP                 "\"$WC_BIND_IP\""
    wc_set_conf "$ws" WorldServerPort        8085
    # The realm is started headless by run.sh; the CLI prompt is readline, and at end of input it
    # reprints "AC>" in a loop. GM commands go in-game. Set 1 and start under tmux to get it back.
    wc_set_conf "$ws" Console.Enable         0
    wc_set_conf "$ws" Updates.EnableDatabases 7
    wc_set_conf "$ws" Updates.AutoSetup      1
    wc_set_conf "$ws" PlayerLimit            1000
    # Travel is dead time on a handheld. 2 = instant flight, but each flight master keeps a toggle so
    # the player can still fly normally; 1 = every taxi node known from login; 50.0 = the top of the
    # core's flight-speed clamp (values above 50 are rejected and fall back to the 32 default).
    wc_set_conf "$ws" InstantFlightPaths     2
    wc_set_conf "$ws" AllFlightPaths         1
    wc_set_conf "$ws" TaxiFlightSpeed        50.0
    wc_set_conf "$ws" LoginDatabaseInfo      "\"$WC_DB_HOST;$WC_DB_PORT;$WC_DB_USER;$WC_DB_PASS;acore_auth\""
    wc_set_conf "$ws" WorldDatabaseInfo      "\"$WC_DB_HOST;$WC_DB_PORT;$WC_DB_USER;$WC_DB_PASS;acore_world\""
    wc_set_conf "$ws" CharacterDatabaseInfo  "\"$WC_DB_HOST;$WC_DB_PORT;$WC_DB_USER;$WC_DB_PASS;acore_characters\""
    wc_log "worldserver.conf: InstantFlightPaths=$(wc_get_conf "$ws" InstantFlightPaths) AllFlightPaths=$(wc_get_conf "$ws" AllFlightPaths) TaxiFlightSpeed=$(wc_get_conf "$ws" TaxiFlightSpeed)"
    wc_set_conf "$as" LogsDir                "\"$WC_ROOT/logs\""
    wc_set_conf "$as" SourceDirectory        "\"$WC_SRC_DIR\""
    wc_set_conf "$as" MySQLExecutable        "\"$WC_MYSQL_EXEC\""
    wc_set_conf "$as" BindIP                 "\"$WC_BIND_IP\""
    wc_set_conf "$as" RealmServerPort        3724
    wc_set_conf "$as" Updates.EnableDatabases 1
    wc_set_conf "$as" Updates.AutoSetup      1
    wc_set_conf "$as" LoginDatabaseInfo      "\"$WC_DB_HOST;$WC_DB_PORT;$WC_DB_USER;$WC_DB_PASS;acore_auth\""

    if [ -f "$WC_CONF_DIR/modules/playerbots.conf.dist" ] || [ -f "$WC_CONF_DIR/modules/playerbots.conf" ]; then
        pb="$(wc_conf_pair "$WC_CONF_DIR" modules/playerbots)"
        wc_set_conf "$pb" Playerbots.Enabled              1
        wc_set_conf "$pb" Playerbots.RandomBotAutologin    1
        wc_set_conf "$pb" Playerbots.MinRandomBots         "$WC_BOTS"
        wc_set_conf "$pb" Playerbots.MaxRandomBots         "$WC_BOTS"
        # The manager's tick: every pass walks the whole population, so this is the knob that
        # decides what N bots cost. 20 ms is the dist value and what the Thor runs; 500 bots at
        # 20 ms saturates a core on its own.
        if [ -n "${WC_BOT_TICK_MS:-}" ]; then
            case "${WC_BOT_TICK_MS}" in
                ''|*[!0-9]*) wc_warn "WC_BOT_TICK_MS must be a number of milliseconds, got '${WC_BOT_TICK_MS}'" ;;
                *) wc_set_conf "$pb" Playerbots.RandomBotUpdateInterval "$WC_BOT_TICK_MS" ;;
            esac
        fi
        # Faction split. Alliance is the remainder, so the pair always sums to 100 - the same rule
        # the Thor's set-ratio.sh applies, kept here so a container render and a device render
        # cannot disagree. WC_HORDE_RATIO is unset on the Thor, where set-ratio.sh owns the pair, so
        # the reference must stay nounset-safe: thor-install.sh runs under `set -u` and never defines
        # it. Unset (or empty) leaves whatever the file already holds.
        if [ -n "${WC_HORDE_RATIO:-}" ]; then
            # 10# so a leading zero stays decimal - 010 is ten, and 08 is eight rather than a bash
            # "value too great for base" abort in the middle of the render - and the digit test plus
            # the three-character bound keep a long or non-numeric value out of the arithmetic.
            case "${WC_HORDE_RATIO}" in
            *[!0-9]* | ????*) horde=-1 ;;
            *) horde=$((10#${WC_HORDE_RATIO})) ;;
            esac
            if [ "$horde" -lt 0 ] || [ "$horde" -gt 100 ]; then
                wc_warn "WC_HORDE_RATIO must be a percentage 0-100, got '${WC_HORDE_RATIO}'"
                return 1
            fi
            wc_set_conf "$pb" Playerbots.RandomBotHordeRatio    "$horde"
            wc_set_conf "$pb" Playerbots.RandomBotAllianceRatio "$((100 - horde))"
        fi
        # Solo-friendly default: keep the bot population level-appropriate for, and inside the zone
        # of, real players instead of spreading it over the whole world. The keys are ignored by an
        # unpatched playerbots build, so an older artifact still starts normally.
        wc_set_conf "$pb" Playerbots.RandomBotNearPlayers              1
        wc_set_conf "$pb" Playerbots.RandomBotNearPlayersLevelRange    3
        wc_set_conf "$pb" Playerbots.RandomBotNearPlayersFollowInterval 120
        wc_set_conf "$pb" Playerbots.RandomBotNearPlayersMinDistance   "$WC_NEAR_MIN_DISTANCE"
        wc_set_conf "$pb" Playerbots.RandomBotNearPlayersSameFaction   1
        wc_set_conf "$pb" Playerbots.RandomBotNearPlayersMaxPerPlayer  "$WC_NEAR_BOTS"
        wc_set_conf "$pb" Playerbots.RandomBotNearPlayersZoneTraffic   1
        wc_set_conf "$pb" Playerbots.RandomBotNearPlayersZoneVisitMinTime "$WC_NEAR_VISIT_MIN"
        wc_set_conf "$pb" Playerbots.RandomBotNearPlayersZoneVisitMaxTime "$WC_NEAR_VISIT_MAX"
        wc_set_conf "$pb" Playerbots.RandomBotNearPlayersZoneAwayMinTime  "$WC_NEAR_AWAY_MIN"
        wc_set_conf "$pb" Playerbots.RandomBotNearPlayersZoneAwayMaxTime  "$WC_NEAR_AWAY_MAX"
        # The twin-ring keys are gone: bots stay in the player's zone instead of being placed in
        # rings around the player. Left in an older config they would look like they still do
        # something, so they are dropped here.
        sed -i -E '/^[[:space:]]*Playerbots\.RandomBotNearPlayers(Radius|InnerCount|OuterRadius|ZoneCount)[[:space:]]*=/d' "$pb"
        # Mercenary system (mod-mercenaries). The innkeeper picker is the only doorway to a pool mercenary, so
        # addclass stays a GM command; the pool is widened to fifteen accounts (about 150
        # characters across both teams and every class) because the picker shows a random 10-15
        # of them per visit, so a small pool would repeat; and mercenaries pass on every roll and leave
        # free-for-all corpses alone, so the loot-bag economy stays the player's.
        wc_set_conf "$pb" Playerbots.AddClassCommand         0
        wc_set_conf "$pb" Playerbots.AddClassAccountPoolSize 15
        wc_set_conf "$pb" Playerbots.LootNeedRollLevel       0
        wc_set_conf "$pb" Playerbots.LootGreedRollLevel      0
        wc_set_conf "$pb" Playerbots.FreeMethodLoot          0
        wc_set_conf "$pb" Playerbots.KeepAltsInGroup         1
        wc_set_conf "$pb" Playerbots.SummonWhenGroup         1
        wc_set_conf "$pb" PlayerbotsDatabaseInfo           "\"$WC_DB_HOST;$WC_DB_PORT;$WC_DB_USER;$WC_DB_PASS;acore_playerbots\""
        wc_set_conf "$pb" Playerbots.Updates.EnableDatabases 1
        wc_log "playerbots.conf: enabled=$(wc_get_conf "$pb" Playerbots.Enabled) bots=$(wc_get_conf "$pb" Playerbots.MaxRandomBots)"
    else
        wc_warn "no $WC_CONF_DIR/modules/playerbots.conf.dist - the artifact was built without the module, bots will not load"
    fi
    if [ -f "$WC_CONF_DIR/modules/mod-creature-gear.conf.dist" ] || [ -f "$WC_CONF_DIR/modules/mod-creature-gear.conf" ]; then
        cg="$(wc_conf_pair "$WC_CONF_DIR" modules/mod-creature-gear)"
        # The shipped .dist already holds the intended values, so nothing is rewritten here unless
        # a key drifted; the four keys below are what makes all three halves of the module active.
        local cg_key cg_val
        for cg_key in CreatureGear.Enable CreatureGear.Drop.Enable \
                      CreatureGear.Appearance.Enable CreatureGear.Weapons.RerollOnRespawn; do
            cg_val="$(wc_get_conf "$cg" "$cg_key")"
            if [ "$cg_val" != "1" ]; then
                wc_warn "mod-creature-gear.conf: $cg_key is '$cg_val', restoring the shipped default 1"
                wc_set_conf "$cg" "$cg_key" 1
            fi
        done
        wc_log "mod-creature-gear.conf: Enable=$(wc_get_conf "$cg" CreatureGear.Enable) Drop.Enable=$(wc_get_conf "$cg" CreatureGear.Drop.Enable) Appearance.Enable=$(wc_get_conf "$cg" CreatureGear.Appearance.Enable) Weapons.RerollOnRespawn=$(wc_get_conf "$cg" CreatureGear.Weapons.RerollOnRespawn)"
    else
        wc_warn "no $WC_CONF_DIR/modules/mod-creature-gear.conf.dist - the artifact was built without the module, creatures keep their stock gear"
    fi
    if [ -f "$WC_CONF_DIR/modules/mod_e_interact.conf.dist" ] || [ -f "$WC_CONF_DIR/modules/mod_e_interact.conf" ]; then
        ei="$(wc_conf_pair "$WC_CONF_DIR" modules/mod_e_interact)"
        # One key for the whole interact loop (loot, gather, talk, chests and nodes). Every option
        # defaults to on in the shipped .dist; a drifted key is put back, since a silent 0 here
        # looks exactly like a broken key in game.
        local ei_key ei_val
        for ei_key in EInteract.Enable EInteract.Loot EInteract.Gather EInteract.Talk EInteract.Use; do
            ei_val="$(wc_get_conf "$ei" "$ei_key")"
            if [ "$ei_val" != "1" ]; then
                wc_warn "mod_e_interact.conf: $ei_key is '$ei_val', restoring the shipped default 1"
                wc_set_conf "$ei" "$ei_key" 1
            fi
        done
        wc_log "mod_e_interact.conf: Enable=$(wc_get_conf "$ei" EInteract.Enable) Loot=$(wc_get_conf "$ei" EInteract.Loot) Gather=$(wc_get_conf "$ei" EInteract.Gather) Talk=$(wc_get_conf "$ei" EInteract.Talk) Use=$(wc_get_conf "$ei" EInteract.Use)"
    else
        wc_warn "no $WC_CONF_DIR/modules/mod_e_interact.conf.dist - the artifact was built without the module, the interact key will not work"
    fi
    if [ -f "$WC_CONF_DIR/modules/mod_hardcore.conf.dist" ] || [ -f "$WC_CONF_DIR/modules/mod_hardcore.conf" ]; then
        hc="$(wc_conf_pair "$WC_CONF_DIR" modules/mod_hardcore)"
        local hc_policy
        hc_policy="$(wc_get_conf "$hc" Hardcore.DeathPolicy)"
        case "$hc_policy" in
            ghost|archive|delete) ;;
            *)
                # The module itself falls back to ghost; make the file say so too.
                if [ -n "$hc_policy" ]; then
                    wc_warn "mod_hardcore.conf: unknown DeathPolicy '$hc_policy', restoring the shipped default ghost"
                fi
                wc_set_conf "$hc" Hardcore.DeathPolicy ghost
                hc_policy=ghost
                ;;
        esac
        wc_log "mod_hardcore.conf: Enable=$(wc_get_conf "$hc" Hardcore.Enable) DeathPolicy=$hc_policy"
    else
        wc_warn "no $WC_CONF_DIR/modules/mod_hardcore.conf.dist - the artifact was built without the module, hardcore is unavailable"
    fi
    if [ -f "$WC_CONF_DIR/modules/mod-survivor.conf.dist" ] || [ -f "$WC_CONF_DIR/modules/mod-survivor.conf" ]; then
        sv="$(wc_conf_pair "$WC_CONF_DIR" modules/mod-survivor)"
        # The shipped .dist already holds these values; each one is still read back, because a key
        # the module cannot use is silently replaced by the module's own default and a container on
        # the wrong gameobject entry simply never appears. Only a value outside its range is
        # rewritten - a deliberate 0 (ruleset off, no map marker, no bot re-roll) is left alone.
        conf_defaults "$sv" survivor_value_ok mod-survivor.conf "${SURVIVOR_CONF_DEFAULTS[@]}"
        wc_log "mod-survivor.conf: Enable=$(wc_get_conf "$sv" Survivor.Enable) MinLevelToFlag=$(wc_get_conf "$sv" Survivor.MinLevelToFlag) DropEquipped=$(wc_get_conf "$sv" Survivor.DropEquipped) DropBags=$(wc_get_conf "$sv" Survivor.DropBags) ApplyToBots=$(wc_get_conf "$sv" Survivor.ApplyToBots)"
    else
        wc_warn "no $WC_CONF_DIR/modules/mod-survivor.conf.dist - the artifact was built without the module, survivor mode is unavailable"
    fi
    if [ -f "$WC_CONF_DIR/modules/mod-mercenaries.conf.dist" ] || [ -f "$WC_CONF_DIR/modules/mod-mercenaries.conf" ]; then
        pw="$(wc_conf_pair "$WC_CONF_DIR" modules/mod-mercenaries)"
        # The shipped .dist already holds these values; each is read back because a key the module
        # cannot use is silently replaced by its own default. Only a value outside its range is
        # rewritten, so a deliberate Mercenary.Enable=0 (picker off) survives a repair. A key that no
        # longer exists is dropped: the obelisk gameobject is gone, and a leftover entry would
        # look like it still selected something.
        sed -i -E '/^[[:space:]]*Mercenary\.ObeliskEntry[[:space:]]*=/d' "$pw"
        # Same for the spellbook restriction: the blacklist and variant blocks and their two scalars
        # are no longer read by the module, so a conf written before the removal would keep a page of
        # blocks that do nothing. Only the live file is edited - the .dist is the shipped one.
        sed -i -E '/^[[:space:]]*Mercenary\.(Blacklist|Variant)\.[0-9]+\.[0-9]+[[:space:]]*=/d; /^[[:space:]]*Mercenary\.VariantWithhold(Min|Max)[[:space:]]*=/d' "$pw"
        conf_defaults "$pw" mercenary_value_ok mod-mercenaries.conf "${MERCENARY_CONF_DEFAULTS[@]}"
        mercenary_append_missing_conf "$pw" "$WC_CONF_DIR/modules/mod-mercenaries.conf.dist"
        # The module was mod-pawn before it was mod-mercenaries and its conf file is renamed with it.
        # The core loads every modules/*.conf, so a leftover mod-pawn.conf keeps being read while
        # nothing looks at its Pawn.* keys; it goes now that the new file is written. run_checks
        # reports and fixes the same thing for an install that never reaches this function.
        rm -f "$WC_CONF_DIR/modules/mod-pawn.conf" "$WC_CONF_DIR/modules/mod-pawn.conf.dist"
        wc_log "mod-mercenaries.conf: Enable=$(wc_get_conf "$pw" Mercenary.Enable) MaxMercenaries=$(wc_get_conf "$pw" Mercenary.MaxMercenaries) LevelWindow=$(wc_get_conf "$pw" Mercenary.LevelWindow) Featured=$(wc_get_conf "$pw" Mercenary.FeaturedMin)-$(wc_get_conf "$pw" Mercenary.FeaturedMax) Refresh=$(wc_get_conf "$pw" Mercenary.PoolRefreshMinMinutes)-$(wc_get_conf "$pw" Mercenary.PoolRefreshMaxMinutes)min Fee=$(wc_get_conf "$pw" Mercenary.HireFeePercent)%+$(wc_get_conf "$pw" Mercenary.HireFeeHeldPercent)% Gear=$(wc_get_conf "$pw" Mercenary.GearFloorPercent)-$(wc_get_conf "$pw" Mercenary.GearCapPercent)%"
    else
        wc_warn "no $WC_CONF_DIR/modules/mod-mercenaries.conf.dist - the artifact was built without the module, the innkeeper picker is unavailable"
    fi
    if [ -f "$WC_CONF_DIR/modules/mod-skateboard.conf.dist" ] || [ -f "$WC_CONF_DIR/modules/mod-skateboard.conf" ]; then
        sb="$(wc_conf_pair "$WC_CONF_DIR" modules/mod-skateboard)"
        # The shipped .dist already holds these values, and Skateboard.Enable ships as 1: the board
        # is available from the first boot. Each key is still read back, because a key the module
        # cannot use is silently replaced by its own fallback and a deck id the DBC does not carry
        # falls back to the stock display. Only a value outside its range is rewritten - a
        # deliberate Enable=0 is left alone.
        # The combat rule used to be Skateboard.OffInCombat, which nothing reads now: the start of a
        # fight leaves the deck alone and a hit takes it (OffOnHit). A leftover key would sit in the
        # live file looking like it still did something.
        sed -i -E '/^[[:space:]]*Skateboard\.OffInCombat[[:space:]]*=/d' "$sb"
        # Drive.Mode went with the spline carrier: a spline on the player's own character makes the
        # core drop their movement packets while it runs (VerifyMovementInfo), which is the opposite
        # of what the carry needs. The key would sit in the live file looking like a choice.
        sed -i -E '/^[[:space:]]*Skateboard\.Drive\.Mode[[:space:]]*=/d' "$sb"
        # MountedPose was the mount render's "seat the rider" switch, folded into Render = mount: a
        # conf that still sets it to 1 looks like it draws the sitting deck and does not.
        sed -i -E '/^[[:space:]]*Skateboard\.MountedPose[[:space:]]*=/d' "$sb"
        # 16386 was the shipped carrier until it turned out to carry ScriptName npc_necropolis_relay,
        # whose constructor marks every summoned deck active. The module refuses a scripted row, so
        # an upgraded conf that still names it is migrated - otherwise the deck stops appearing and
        # only the log says why.
        if [ "$(wc_get_conf "$sb" Skateboard.Deck.Carrier)" = 16386 ]; then
            wc_warn "mod-skateboard.conf: Skateboard.Deck.Carrier 16386 is scripted (npc_necropolis_relay), using 29443"
            wc_set_conf "$sb" Skateboard.Deck.Carrier 29443
        fi
        conf_defaults "$sb" skateboard_value_ok mod-skateboard.conf "${SKATEBOARD_CONF_DEFAULTS[@]}"
        # WC_SKATEBOARD is how a container or the Thor asks for the board on a given boot. The conf
        # is rendered from the .dist every start, and the container's /opt/acore/etc is part of the
        # image layer rather than a volume, so a hand edit there is gone at the next rebuild: the
        # dial is what survives. Unset leaves the shipped default (on).
        case "${WC_SKATEBOARD:-}" in
            0|1) wc_set_conf "$sb" Skateboard.Enable "$WC_SKATEBOARD" ;;
            '') ;;
            *) wc_warn "mod-skateboard.conf: WC_SKATEBOARD must be 0 or 1, keeping $(wc_get_conf "$sb" Skateboard.Enable)" ;;
        esac
        wc_log "mod-skateboard.conf: Enable=$(wc_get_conf "$sb" Skateboard.Enable) Deck=$(wc_get_conf "$sb" Skateboard.Deck) MaxSpeed=$(wc_get_conf "$sb" Skateboard.MaxSpeed) BoardCastMs=$(wc_get_conf "$sb" Skateboard.BoardCastMs) OffOnHit=$(wc_get_conf "$sb" Skateboard.OffOnHit) Crash.Enable=$(wc_get_conf "$sb" Skateboard.Crash.Enable)"
    else
        wc_warn "no $WC_CONF_DIR/modules/mod-skateboard.conf.dist - the artifact was built without the module, the skateboard is unavailable"
    fi
    wc_log "worldserver.conf: DataDir=$(wc_get_conf "$ws" DataDir) BindIP=$(wc_get_conf "$ws" BindIP)"
}
