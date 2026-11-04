<#
.SYNOPSIS
    Installs, repairs, inspects and removes the AzerothCore + mod-playerbots realm on a
    Windows x64 host.

.DESCRIPTION
    The Windows counterpart of scripts/thor-install.sh: the same realm - the same AzerothCore
    and mod-playerbots pins, and the five modules that live in this repository - installed
    from acore-server-win64.zip, the artifact .github/workflows/server-win64.yml publishes.
    The client data is fetched and sha256-verified, a portable MySQL 8.4 is unpacked beside it,
    and the configs, the three schemas and the GM account are written exactly the way the Thor
    installer writes them, so the two installers can be read side by side.

    Run it from an elevated PowerShell (install, repair and uninstall need local admin: they
    create an inbound firewall rule and write to Program Files-free locations but still stop
    services and touch the firewall):

        .\windows-install.ps1                 # install (see the phases below)
        .\windows-install.ps1 -Check          # report what install would do; change nothing
        .\windows-install.ps1 -Status         # inspect an existing install (exit 1 if broken)
        .\windows-install.ps1 -Repair         # fix what it can, then re-check
        .\windows-install.ps1 -Uninstall      # stop, optionally dump the databases, remove

    Everything is encapsulated: the install root is a plain directory, and every file this
    script creates outside it is listed in a manifest, so removal is exact rather than
    "grep and hope".

      C:\acore                     server: worldserver.exe authserver.exe configs src data
                                          db mysql logs .win-state
      %USERPROFILE%\.win-wow       state, manifest, database dumps
      %USERPROFILE%\start-wow-server.cmd, stop-wow-server.cmd

    MySQL's data directory lives in <root>\db and its logs in <root>\logs, so a removal does
    not leave a database behind in another program's data directory.

    Phases, in order: preflight, server files, layout check, client data, MySQL, schemas,
    configs, first boot, helpers, firewall, state and manifest, summary. -Check walks the same
    phases and prints what each one would do.

.PARAMETER Check
    Report what the install would do and change nothing: no downloads, no directories, no
    registry and no firewall changes. Combined with -Status or -Repair it is not allowed.

.PARAMETER Status
    Inspect an existing install and print one "ok" or "FAIL" line per component (binaries,
    configs, module keys, client data, MySQL, schemas, the GM account, the wrappers and the
    firewall rule), then exit 1 when anything is red. Reads only: nothing is downloaded,
    written or started.

.PARAMETER Repair
    Check every component, fix the ones this script knows how to fix (conf writing and drift
    repairs, a missing client-data tree, the generated helper scripts, a stopped MySQL, an
    empty schema) and then re-check, which is the same relationship --repair has to --check
    in thor-install.sh.

.PARAMETER Uninstall
    Stop the realm and the database, remove every path recorded in
    %USERPROFILE%\.win-wow\manifest.txt and the install root itself. With -KeepDb the four
    acore_* schemas are dumped to %USERPROFILE%\.win-wow\dumps first, because removing the
    root removes <root>\db with it.

.PARAMETER KeepDb
    With -Uninstall: dump acore_auth, acore_world, acore_characters and acore_playerbots to
    %USERPROFILE%\.win-wow\dumps\<schema>.sql before anything is removed.

.PARAMETER Force
    Re-download and re-unpack the server artifact even though <root>\worldserver.exe exists.
    Without it an install reuses the build it finds, so re-running the installer never
    overwrites a working server with a different one.

.PARAMETER Bots
    Random bots to enable (default 50). Written to Playerbots.MinRandomBots and
    Playerbots.MaxRandomBots; set-bots.ps1 changes it later without a full re-run.

.PARAMETER HordeRatio
    Percentage of the random bots on the Horde side, 0-100 (default 50). Alliance is the
    remainder, so the pair always sums to 100 - the same rule the Thor's start-wow-server.sh
    --horde-ratio applies. In Docker mode it is the container's HORDE_RATIO and is rendered into
    playerbots.conf at every start; in native mode it is written into the conf directly.

.PARAMETER DbUser
    MySQL user the servers connect as (default acore). Its password is generated unless
    -DbPass says otherwise.

.PARAMETER DbPass
    MySQL password for -DbUser. Default: 16 random alphanumerics, recorded in
    <root>\credentials.txt and in the state file the wrappers read.

.PARAMETER Account
    The GM account created on the first boot (default admin).

.PARAMETER AccountPass
    The GM account's password (default password, so the client login is predictable).

.PARAMETER LanIp
    Bind the servers to this address and point realm 1 at it, and open the inbound firewall
    rules for 3724 and 8085. Without it everything listens on 127.0.0.1 only.

.PARAMETER Root
    Install root (default C:\acore). Must be an absolute path outside Program Files; a path
    with spaces is accepted when it is given explicitly and is quoted in the generated .cmd
    files, but the default is used unquoted everywhere.

.PARAMETER ReleaseUrl
    The server artifact. Default: the acore-server-win64.zip asset of the server-build-win64
    release. A .sha256 published beside it is verified when it exists.

.PARAMETER DataZip
    Client data (dbc/maps/vmaps/mmaps): an http(s) URL, a local file, or "none" to skip it.
    Default: the pinned wowgaming client-data 20.0 zip, whose sha256 is checked.

.PARAMETER ClientData
    The 3.3.5a client folder - the World of Warcraft folder itself, the one holding
    Data\common.MPQ - or "none". Its Data folder is accepted instead of the parent, a quoted path
    is accepted as-is (Explorer's Copy as path adds the quotes), and the value is stored absolute.
    The loot bags are
    custom items, and a client draws an inventory item from its own Item.dbc, so this folder
    gets a patch archive with the 64 bag records and the server's data\dbc\Item.dbc gets the
    same records. The client lives outside the install root and outside the container, so this
    half always runs on the host, for both modes. Default: the remembered folder, or one
    question when the install starts; the answer is kept in %USERPROFILE%\.win-wow\client-data.

.PARAMETER BootTimeout
    Seconds the first boot may take to report that the world is up (default 1800): the
    native path waits for "World initialized" in the log, the -Docker path for the
    entrypoint's readiness line. Importing the world database takes the longest part of it.

.PARAMETER MysqlUrl
    The portable MySQL zip. Default: MySQL 8.4.9 winx64 from the MySQL CDN archives. It is
    unpacked into <root>\mysql and never installed system-wide, so the host's own MySQL is
    left alone.

.PARAMETER Docker
    Install, inspect, repair and remove the realm as two containers (an image built from
    docker/Dockerfile plus mysql:8.4) instead of a native install. Docker Desktop has to be
    running. The mode is recorded in <root>\.win-state\mode and -Status, -Repair and -Uninstall
    follow it, so a container install is never torn down by the native path and the other way
    round. -LanIp, -Bots, -Account, -AccountPass, -DbPass, -BootTimeout and -Root mean the same
    thing here; -Force, -DataZip and -MysqlUrl do not apply.

.PARAMETER DockerDir
    Where the container files are staged, laid out the way this repository lays them out
    because docker/docker-compose.yml builds with the repository root as its context
    (default <root>\docker-src).

.PARAMETER SelfRef
    The git ref the container files (docker/Dockerfile, docker/docker-compose.yml,
    docker/entrypoint.sh and scripts/write-confs.sh) are fetched at from raw.githubusercontent.com
    (default main). Point it at a tag or a branch to install the files from there.

.PARAMETER Help
    Print this text and exit.

.EXAMPLE
    .\windows-install.ps1
    Install into C:\acore with 50 bots on loopback only.

.EXAMPLE
    .\windows-install.ps1 -LanIp 192.168.1.20 -Bots 25
    Install, listen on the LAN, point realm 1 at 192.168.1.20 and open 3724/8085.

.EXAMPLE
    .\windows-install.ps1 -Status
    Inspect the install; exit code 1 means something needs -Repair.

.EXAMPLE
    .\windows-install.ps1 -Uninstall -KeepDb
    Dump the databases to %USERPROFILE%\.win-wow\dumps and remove everything else.

.EXAMPLE
    .\windows-install.ps1 -Docker
    Install the realm as two containers staged in C:\acore\docker-src; -Docker -Uninstall is the
    one command that takes it away again, volumes and all.
#>
[CmdletBinding()]
param(
    [switch]$Check,
    [switch]$Status,
    [switch]$Repair,
    [switch]$Uninstall,
    [switch]$KeepDb,
    [switch]$Force,
    [int]$Bots = 50,
    [int]$HordeRatio = 50,
    [string]$DbUser = 'acore',
    [string]$DbPass = '',
    [string]$Account = 'admin',
    [string]$AccountPass = 'password',
    [string]$LanIp = '',
    [string]$Root = 'C:\acore',
    [string]$ReleaseUrl = 'https://github.com/nick-monto/acore-thor-selfhost/releases/download/server-build-win64/acore-server-win64.zip',
    [string]$DataZip = 'https://github.com/wowgaming/client-data/releases/download/v20.0/data.zip',
    [string]$ClientData = '',
    [int]$BootTimeout = 1800,
    [string]$MysqlUrl = 'https://cdn.mysql.com/archives/mysql-8.4/mysql-8.4.9-winx64.zip',
    [switch]$Docker,
    [string]$DockerDir = '',
    [string]$SelfRef = 'main',
    [switch]$Help
)

# Windows PowerShell 5.1 only: no ternary operator, no null-coalescing operator, no
# ConvertFrom-Json -AsHashtable. The file is ASCII on purpose - a BOM or a smart quote in a
# .cmd that this script writes is enough to break cmd.exe's parser.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------- constants

$ScriptVersion = '1'
$RootDefault = 'C:\acore'
$ReleaseUrlDefault = 'https://github.com/nick-monto/acore-thor-selfhost/releases/download/server-build-win64/acore-server-win64.zip'
$DataUrl = 'https://github.com/wowgaming/client-data/releases/download/v20.0/data.zip'
# Pinned like thor-install.sh's DATA_SHA256: the same bytes that reach the Thor reach the PC.
$DataSha256 = 'a3d4df635ae6c2c8f08052c32a79e0f806955150ad36b014a823dd08a32a4610'
$MysqlUrlDefault = 'https://cdn.mysql.com/archives/mysql-8.4/mysql-8.4.9-winx64.zip'
$MysqlVersion = '8.4.9'
$DbPort = 3306
$AuthPort = 3724
$WorldPort = 8085
# The core runs "<MySQLExecutable> ... --default-character-set=utf8" once per SQL file it
# applies, so the bundled client is the only one that has to exist; nothing is added to PATH.

# Solo default: only a handful of the random bots stay in the real player's zone; the rest keep
# the normal world-wide distribution, so the total population is unchanged without crowding the
# player. The zone group also churns: bots visit for 15-40 minutes, then leave for 4-10 and
# come back. These are the same numbers thor-install.sh writes.
$NearBots = 20
$NearMinDistance = '100.0'
$NearVisitMin = 900
$NearVisitMax = 2400
$NearAwayMin = 240
$NearAwayMax = 600

# The container mode's staged files, in the layout of this repository on purpose:
# docker/docker-compose.yml builds with the repository root as its context, so the staging directory
# has to hold docker\ and scripts\ at its top level. It is every file the Dockerfile COPYs, because a
# file missing here fails the build at its COPY line, long after the download that was supposed to
# have staged it.
$DockerFiles = @(
    'docker/Dockerfile'
    'docker/docker-compose.yml'
    'docker/entrypoint.sh'
    'scripts/write-confs.sh'
    'scripts/item-dbc/patch_item_dbc.py'
    'scripts/loot-bags/gen_loot_bags.py'
    'scripts/lib/dbc_mpq.py'
    'scripts/skateboard/patch_deck_dbc.py'
)
$SelfRefDefault = 'main'
$DockerRepoBase = 'https://raw.githubusercontent.com/nick-monto/acore-thor-selfhost/'

# mod-survivor's conf keys with the values its shipped .dist carries, as KEY=DEFAULT. The module
# reads all eleven once at load, so a key that is absent or outside its range is silently replaced
# by the module's own fallback; Write-Configs() and Invoke-Checks() restore them from this list.
$SurvivorConfDefaults = @(
    'Survivor.Enable=1'
    'Survivor.MinLevelToFlag=10'
    'Survivor.AnnounceDeaths=1'
    'Survivor.DropEquipped=1'
    'Survivor.DropBags=1'
    'Survivor.ContainerLifetimeDays=7'
    'Survivor.MarkOnMap=1'
    'Survivor.ContainerEntry=5000000'
    'Survivor.ApplyToBots=1'
    'Survivor.BotRegenOnDeath=1'
    'Survivor.BotDropContainer=1'
)

# mod-mercenaries's conf keys with the values its shipped .dist carries. Same contract as the
# survivor list: a key outside its range is silently replaced by the module's own fallback, so
# Write-Configs() restores it from here. The pool lists (Mercenary.Voice, Mercenary.VoiceLine.*,
# Mercenary.Temperament.*) stay out on purpose: they are lists, and Set-Conf would overwrite a
# pool the player had trimmed.
$MercenaryConfDefaults = @(
    'Mercenary.Enable=1'
    'Mercenary.MaxMercenaries=2'
    'Mercenary.LevelWindow=2'
    'Mercenary.GainXP=0'
    'Mercenary.DismissGraceSeconds=300'
    'Mercenary.Voice=Talkative: +emote | Quiet: -emote'
    'Mercenary.VoiceKillChancePercent=10'
    'Mercenary.VoiceCooldownSeconds=120'
    'Mercenary.HireFeePercent=100'
    'Mercenary.HireFeeHeldPercent=100'
    'Mercenary.GearFloorPercent=85'
    'Mercenary.GearCapPercent=150'
    'Mercenary.ResupplyFeePercent=100'
    'Mercenary.AffinityEnabled=1'
    'Mercenary.AffinityKillPoints=10'
    'Mercenary.AffinityQuestPoints=25'
    'Mercenary.AffinityStep=100'
    'Mercenary.AffinityGearPercent=5'
    'Mercenary.AffinityMaxSteps=2'
    'Mercenary.OfferedPerPage=12'
    'Mercenary.FeaturedMin=10'
    'Mercenary.FeaturedMax=15'
    'Mercenary.PoolRefreshMinMinutes=40'
    'Mercenary.PoolRefreshMaxMinutes=60'
    'Mercenary.Debug=0'
)

# mod-skateboard's conf keys with the values its shipped .dist carries. Same contract as the
# survivor list: a key absent from the file or outside its range is silently replaced by the
# module's own fallback, so Write-Configs() and Invoke-Checks() restore it from here. These are the
# module's whole conf - nineteen scalars, no lists - and Skateboard.Enable ships as 1.
$SkateboardConfDefaults = @(
    'Skateboard.Enable=1'
    'Skateboard.Deck=engravedwall'
    'Skateboard.Deck.Render=unit'
    'Skateboard.Deck.Carrier=29443'
    'Skateboard.Deck.FollowMs=40'
    'Skateboard.MaxSpeed=21.0'
    'Skateboard.Cruise=4.0'
    'Skateboard.PushImpulse=4.0'
    'Skateboard.Push.Mode=0'
    'Skateboard.Push.IntervalMs=500'
    'Skateboard.Push.MaxHoldMs=2000'
    'Skateboard.Friction.Static=1.5'
    'Skateboard.Friction.Rolling=0.004'
    'Skateboard.SlopeGain=30.0'
    'Skateboard.Drive=1'
    'Skateboard.Drive.ImpulseMs=350'
    'Skateboard.Drive.GroundMs=100'
    'Skateboard.Drive.MinSpeed=3.0'
    'Skateboard.Brake=6.0'
    'Skateboard.LandingBonus=2.0'
    'Skateboard.Tricks=1'
    'Skateboard.Trick.VisualKit=5036'
    'Skateboard.Trick.List=kickflip:399:5036, ollie:394:5036, grind:388:5036'
    'Skateboard.Crash.Enable=1'
    'Skateboard.Crash.DamagePct=8'
    'Skateboard.Crash.StaggerMs=1500'
    'Skateboard.HardcoreSafe=1'
    'Skateboard.OffOnHit=1'
    'Skateboard.BoardCastMs=2000'
    'Skateboard.AckTimeoutMs=500'
    'Skateboard.Debug=0'
)

# ---------------------------------------------------------------- mutable state

$script:Action = 'install'      # install | check | status | repair | uninstall
$script:DryRun = $false         # true for -Check: every phase prints instead of acting
$script:RootGiven = $false
$script:Root = $Root
$script:Bots = $Bots
$script:DbUser = $DbUser
$script:DbPass = $DbPass
$script:Account = $Account
$script:AccountPass = $AccountPass
$script:LanIp = $LanIp
$script:ReleaseUrl = $ReleaseUrl
$script:DataZip = $DataZip
$script:ClientData = $ClientData
$script:PythonProblem = ''
$script:BootTimeout = $BootTimeout
$script:MysqlUrl = $MysqlUrl
$script:Docker = [bool]$Docker
$script:DockerDir = $DockerDir
$script:HordeRatio = $HordeRatio
$script:SelfRef = $SelfRef
$script:SelfRefSha = ''   # resolved once by Resolve-SelfRef; every download uses it
# Root password of the db container. Generated at a container install and read back from the state
# file afterwards: the mysql image initialises its volume with the password it is given once, so a
# second, different one would leave the entrypoint unable to log in as root.
$script:DockerRootPass = ''
# Why the last daemon probe said no, as one line: -Docker -Status reports it next to the FAIL.
$script:DockerDaemonReason = ''
$script:Force = [bool]$Force
$script:KeepDb = [bool]$KeepDb
$script:ArtifactSha = ''
$script:AdminHost = '127.0.0.1'
$script:Problems = 0
$script:Fixes = 0
$script:FirstBootOk = $true
# True unless a repair discovered there is no state file: Resolve-SelfRef saves when it pins a ref,
# which would otherwise create <root>\.win-state\state.env on a root that was never installed.
$script:HadState = $true
# What the state file recorded for the ref, kept even when -SelfRef was passed (Import-State then
# leaves SelfRefSha empty so this run fetches the ref itself). Save-State writes the pin back when the
# ref did not change, so a repair whose resolution failed cannot erase it.
$script:RecordedSelfRef = ''
$script:RecordedSelfRefSha = ''
$script:BinDir = ''
$script:ConfDir = ''
$script:DataDir = ''
$script:SrcDir = ''
$script:DbDir = ''
$script:MysqlDir = ''
$script:LogsDir = ''
$script:StateDir = ''
$script:StateFile = ''
$script:ModeFile = ''
$script:HostState = Join-Path $env:USERPROFILE '.win-wow'
$script:ManifestFile = Join-Path $env:USERPROFILE '.win-wow\manifest.txt'

# ---------------------------------------------------------------- output helpers

function Log {
    param([string]$Message)
    Write-Host $Message
}

function Step {
    param([string]$Message)
    Write-Host ''
    Write-Host "=== $Message"
}

function Warn {
    param([string]$Message)
    [Console]::Error.WriteLine("warning: $Message")
}

# Die is the only exit path for a broken install: a partial install that exits 0 reads like a
# finished one to a wrapper or a scheduled task.
function Die {
    param([string]$Message)
    [Console]::Error.WriteLine("error: $Message")
    exit 1
}

# Note prints what -Check would have done. Call sites guard with $script:DryRun and return, the
# way thor-install.sh guards its write steps with [ "$CHECK" = 1 ].
function Note {
    # -Check is the only mode where nothing happens, so it is the only one where "would" is true:
    # -Status reports the same lines about an install that already exists.
    param([string]$Message)
    if ($script:DryRun) {
        Write-Host "  would: $Message"
    } else {
        Write-Host "  note : $Message"
    }
}

function Ok {
    param([string]$Message)
    Write-Host "ok    $Message"
}

function Bad {
    param([string]$Message)
    Write-Host "FAIL  $Message"
    $script:Problems = $script:Problems + 1
}

function Have {
    param([string]$Name)
    $null -ne (Get-Command -Name $Name -ErrorAction SilentlyContinue)
}

# ---------------------------------------------------------------- native commands

function Invoke-Native {
    <# Invoke-Native -FilePath EXE [-Arguments ARGS] [-Display TEXT] [-InputText TEXT] [-Echo]
       [-StdOutOnly] - run a program and return its text; $LASTEXITCODE is left for the caller. #>
    param(
        [string]$FilePath,
        [string[]]$Arguments = @(),
        [string]$Display = '',
        [string]$InputText = '',
        [switch]$Echo,
        [switch]$StdOutOnly
    )
    # Windows PowerShell 5.1 turns a native program's stderr into an ErrorRecord, and this file runs
    # with $ErrorActionPreference = 'Stop': the first line a program writes to stderr therefore ends
    # the script - "docker version" with Docker Desktop closed would abort -Status with a
    # NativeCommandError block instead of a FAIL line - and the 2>$null at the call sites does not
    # prevent it, because the record is an error before the redirection can drop it. The preference
    # is 'Continue' for the length of the call, where the same stderr is just a message in the
    # pipeline, and it is restored in finally. Success is decided from $LASTEXITCODE alone, never
    # from whether an exception was thrown, and the caller gets the merged text, so a real failure
    # is still reported by the exit code and by the words the program itself printed.
    #
    # -StdOutOnly drops the error records from the returned text: the callers that parse one value
    # out of stdout (a ping, one field of digest) must not have a warning mixed into the answer.
    # -Echo writes each line to this console as it arrives and collects nothing: a build that prints
    # nothing for minutes looks hung, and streaming is the only reason compose up is not captured.
    $argvList = @()
    foreach ($item in $Arguments) { $argvList += $item }
    if ($Display) { Log $Display }
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $text = ''
    try {
        if ($Echo) {
            if ($InputText) {
                $InputText | & $FilePath @argvList 2>&1 | ForEach-Object { Log ([string]$_) }
            } else {
                & $FilePath @argvList 2>&1 | ForEach-Object { Log ([string]$_) }
            }
        } else {
            $pieces = @()
            if ($InputText) {
                $pieces = @($InputText | & $FilePath @argvList 2>&1)
            } else {
                $pieces = @(& $FilePath @argvList 2>&1)
            }
            $kept = @()
            foreach ($piece in $pieces) {
                if ($StdOutOnly -and ($piece -is [System.Management.Automation.ErrorRecord])) { continue }
                $kept += [string]$piece
            }
            $text = ($kept -join "`n")
        }
    } finally {
        $ErrorActionPreference = $saved
    }
    return $text
}

# ---------------------------------------------------------------- generic helpers

function Test-Elevated {
    # WindowsPrincipal.IsInRole on the built-in Administrator role is the only elevation test that
    # does not depend on the UAC registry or on a policy that grants local admin to the user.
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object -TypeName Security.Principal.WindowsPrincipal -ArgumentList $identity
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-ProcessorArchitecture {
    # 32-bit PowerShell on 64-bit Windows reports x86 in PROCESSOR_ARCHITECTURE and the real one in
    # PROCESSOR_ARCHITEW6432; reading only the first would refuse to install on a correct host.
    if ($env:PROCESSOR_ARCHITEW6432) { return $env:PROCESSOR_ARCHITEW6432 }
    return $env:PROCESSOR_ARCHITECTURE
}

function Get-FreeSpaceGb {
    # -1 means "could not ask": a drive that does not exist yet is not a reason to stop, it is a
    # reason to let the download report the real failure.
    try {
        $drive = New-Object -TypeName System.IO.DriveInfo -ArgumentList ([System.IO.Path]::GetPathRoot($script:Root))
        return [math]::Round($drive.AvailableFreeSpace / 1GB, 1)
    } catch {
        return -1
    }
}

function Get-FetchRef {
    <# Get-FetchRef - the ref every download uses: the resolved commit, or the ref as given. #>
    if ($script:SelfRefSha) { return $script:SelfRefSha }
    return $script:SelfRef
}

function Resolve-SelfRef {
    <# Resolve-SelfRef - pin -SelfRef to a commit, once, and fetch from that commit from then on. #>
    # -SelfRef defaults to a branch, and a branch is a moving pointer: the entrypoint.sh this installer
    # fetches and runs as root inside the acore container, the compose file, and the two python files
    # it runs as the user are whatever main holds at that moment. A commit is content-addressed -
    # GitHub cannot serve different bytes for the same commit - so the first run resolves the ref once
    # and records the commit in the state file; every later -Repair fetches from the recorded commit,
    # and a branch or tag that moves afterwards cannot change what an installed realm runs.
    # Passing -SelfRef explicitly asks for that ref, so it is resolved again.
    if ($script:SelfRef -match '^[0-9a-fA-F]{40}$') {
        $script:SelfRefSha = $script:SelfRef.ToLowerInvariant()
        return
    }
    if ($script:SelfRefSha -and -not $script:Bound.ContainsKey('SelfRef')) {
        Note ("-SelfRef '{0}' is pinned to {1} (recorded at install time)" -f $script:SelfRef, $script:SelfRefSha.Substring(0, 12))
        # The pin is why a repair after a push keeps running the old files. Say what moves it, where
        # the confusion happens, rather than only in the runbook.
        Note ("  a newer {0} does not apply by itself: pass -SelfRef {0} to resolve and record it" -f $script:SelfRef)
        return
    }
    if ($script:DryRun) {
        Note ("would resolve -SelfRef '{0}' to a commit and fetch from that" -f $script:SelfRef)
        return
    }
    $sha = ''
    try {
        $api = 'https://api.github.com/repos/nick-monto/acore-thor-selfhost/commits/' + $script:SelfRef
        $response = Invoke-WebRequest -UseBasicParsing -Uri $api -Headers @{
            'Accept' = 'application/vnd.github+json'
            'User-Agent' = 'acore-thor-installer'
        }
        $json = $response.Content | ConvertFrom-Json
        if ($json -and ([string]$json.sha) -match '^[0-9a-fA-F]{40}$') { $sha = ([string]$json.sha).ToLowerInvariant() }
    } catch {
        $sha = ''
    }
    if ($sha) {
        $script:SelfRefSha = $sha
        if ($script:Action -ne 'repair' -or $script:HadState) { Save-State }
        Note ("-SelfRef '{0}' pinned to commit {1}" -f $script:SelfRef, $sha.Substring(0, 12))
        return
    }
    Warn ("-SelfRef '{0}' is not a commit and could not be resolved, so the files fetched below are whatever that ref holds when they are fetched" -f $script:SelfRef)
    Warn '  pass -SelfRef <40-character commit> to pin them'
}

function Reset-SelfRefPin {
    <# Reset-SelfRefPin - forget a recorded pin and resolve -SelfRef again. #>
    # A recorded pin is content-addressed, not immortal: a file a newer artifact ships (for example
    # scripts/lib/dbc_mpq.py, which came after the deck tool that imports it) does not exist at the
    # commit an older install pinned. Dropping both the pin and the state copy is what stops
    # Save-State from writing the stale commit back.
    $script:SelfRefSha = ''
    $script:RecordedSelfRefSha = ''
    Resolve-SelfRef
}

function Get-RemoteFile {
    <# Get-RemoteFile URI DESTINATION - download with retries. #>
    param(
        [string]$Uri,
        [string]$Destination,
        [int]$Attempts = 3
    )
    # The progress bar is what makes Invoke-WebRequest slow on 5.1 - it renders per chunk - and
    # -UseBasicParsing keeps the result out of the Internet Explorer engine, which throws when IE
    # has never been started on a fresh profile.
    $ProgressPreference = 'SilentlyContinue'
    for ($attempt = 1; $attempt -le $Attempts; $attempt++) {
        try {
            Invoke-WebRequest -UseBasicParsing -Uri $Uri -OutFile $Destination
            return $true
        } catch {
            if ($attempt -ge $Attempts) {
                Warn "download failed: $($_.Exception.Message)"
                return $false
            }
            Warn "download failed ($($_.Exception.Message)); retrying in 5s"
            Start-Sleep -Seconds 5
        }
    }
    return $false
}

function Get-Sha256 {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return '' }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-FileTailText {
    <# Get-FileTailText PATH [BYTES] - the last BYTES of a file, '' when it cannot be read. #>
    param(
        [string]$Path,
        [int]$Bytes = 65536
    )
    if (-not (Test-Path -LiteralPath $Path)) { return '' }
    try {
        # FileShare.ReadWrite: the servers hold their logs open for writing, and the default share
        # mode of .NET would fail this read with a sharing violation.
        $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    } catch {
        return ''
    }
    try {
        $length = $stream.Length
        if ($length -gt $Bytes) {
            [void]$stream.Seek($length - $Bytes, [System.IO.SeekOrigin]::Begin)
        }
        $buffer = New-Object byte[] $Bytes
        $read = $stream.Read($buffer, 0, $Bytes)
        if ($read -le 0) { return '' }
        return [System.Text.Encoding]::UTF8.GetString($buffer, 0, $read)
    } catch {
        return ''
    } finally {
        $stream.Dispose()
    }
}

function Show-Usage {
    # The help is the comment-based block at the top of this file, the way thor-install.sh's
    # usage() prints its own header: one copy of the text, so -Help cannot drift from Get-Help.
    $path = $PSCommandPath
    if (-not $path) { $path = $MyInvocation.MyCommand.Path }
    if (-not $path -or -not (Test-Path -LiteralPath $path)) {
        Log 'windows-install.ps1 - see the header of the script for the parameters'
        return
    }
    $inside = $false
    foreach ($line in [System.IO.File]::ReadAllLines($path)) {
        if (-not $inside) {
            if ($line -match '^\s*<#') { $inside = $true }
            continue
        }
        if ($line -match '^\s*#>') { break }
        Write-Host $line
    }
}

# ---------------------------------------------------------------- arguments and paths

# Which parameters were actually given on the command line. -Status and -Repair read the state
# file and must not undo a flag the caller passed this time, which is what state_apply_cli does
# on the Thor and this reproduces without re-parsing.
$script:Bound = @{}
foreach ($boundKey in $PSBoundParameters.Keys) {
    $script:Bound[$boundKey] = $PSBoundParameters[$boundKey]
}

function Resolve-Action {
    $actions = @()
    if ($Check) { $actions += 'check' }
    if ($Status) { $actions += 'status' }
    if ($Repair) { $actions += 'repair' }
    if ($Uninstall) { $actions += 'uninstall' }
    if ($actions.Count -gt 1) {
        Die ("pick one of -Check, -Status, -Repair and -Uninstall (got {0})" -f ($actions -join ', '))
    }
    if ($actions.Count -eq 1) {
        $script:Action = $actions[0]
    } else {
        $script:Action = 'install'
    }
    # -Check is install's dry run: every phase runs, every write is replaced by a "would:" line.
    if ($script:Action -eq 'check') { $script:DryRun = $true }
}

function Test-Quoted {
    param([string]$Value)
    return ($Value.Contains("'") -or $Value.Contains('"'))
}

function Confirm-Arguments {
    # Values that end up inside SQL text or inside a cmd.exe line are rejected here rather than
    # half-escaped later: a quote in a password breaks the CREATE USER statement, and a quote in
    # anything the generated .cmd files interpolate breaks the wrapper.
    if ($script:HordeRatio -lt 0 -or $script:HordeRatio -gt 100) {
        Die "-HordeRatio must be 0-100 (got $($script:HordeRatio))"
    }
    if ($script:Bots -lt 0 -or $script:Bots -gt 2000) {
        Die "-Bots must be 0-2000 (got $($script:Bots))"
    }
    if ($script:BootTimeout -lt 1) {
        Die "-BootTimeout must be a positive number of seconds (got $($script:BootTimeout))"
    }
    if ($script:DbUser -notmatch '^[A-Za-z0-9_]+$') {
        Die "-DbUser must be letters, digits and underscores (got '$($script:DbUser)')"
    }
    if ($script:Account -notmatch '^[A-Za-z0-9]+$') {
        Die "-Account must be letters and digits (got '$($script:Account)')"
    }
    if (Test-Quoted $script:DbPass) {
        Die "-DbPass must not contain a quote: it is written into a MySQL CREATE USER statement"
    }
    if (Test-Quoted $script:AccountPass) {
        Die "-AccountPass must not contain a quote: the GM account is created by a console command"
    }
    if ($script:DbPass -eq '') {
        $script:DbPass = New-RandomPassword
    }
    if ($script:AccountPass -eq '') {
        $script:AccountPass = 'password'
    }
    if (-not [System.IO.Path]::IsPathRooted($script:Root)) {
        Die ("-Root must be an absolute path (got '{0}')" -f $script:Root)
    }
    # The generated .cmd files interpolate the root into quoted paths, and the generated .ps1 files
    # into single-quoted ones; cmd.exe still treats & | < > ^ % inside a quoted string in some
    # positions as operators or variable references, and a root that breaks its own start script is
    # worse than a refusal here.
    if ($script:Root -match "[&|<>^%!'`"]") {
        Die ("-Root must not contain & | < > ^ % ! or a quote (got '{0}')" -f $script:Root)
    }
    $script:Root = [System.IO.Path]::GetFullPath($script:Root)
    while ($script:Root.Length -gt 3 -and $script:Root.EndsWith('\')) {
        $script:Root = $script:Root.Substring(0, $script:Root.Length - 1)
    }
    if ($script:Root -match '^[A-Za-z]:\\?$') {
        Die ("-Root must be a private directory, not the drive root {0}" -f $script:Root)
    }
    # The staging directory takes the same rules as the root: absolute, no trailing separator, and
    # nothing cmd.exe would act on, because every compose call quotes the paths inside it.
    if ($script:DockerDir -eq '') {
        $script:DockerDir = Join-Path $script:Root 'docker-src'
    } else {
        if (-not [System.IO.Path]::IsPathRooted($script:DockerDir)) {
            Die ("-DockerDir must be an absolute path (got '{0}')" -f $script:DockerDir)
        }
        if ($script:DockerDir -match "[&|<>^%!'`"]") {
            Die ("-DockerDir must not contain & | < > ^ % ! or a quote (got '{0}')" -f $script:DockerDir)
        }
        $script:DockerDir = [System.IO.Path]::GetFullPath($script:DockerDir)
        while ($script:DockerDir.Length -gt 3 -and $script:DockerDir.EndsWith('\')) {
            $script:DockerDir = $script:DockerDir.Substring(0, $script:DockerDir.Length - 1)
        }
        if ($script:DockerDir -match '^[A-Za-z]:\\?$') {
            Die ("-DockerDir must be a private directory, not the drive root {0}" -f $script:DockerDir)
        }
    }
    # -ClientData is the player's own folder, so it is not created or removed here - but it is
    # resolved once, up front, so a typo is an error at the start rather than a warning at the end.
    if ($script:ClientData -and $script:ClientData -ne 'none') {
        $script:ClientData = ConvertTo-ClientPath $script:ClientData
        $rooted = $false
        try { $rooted = [System.IO.Path]::IsPathRooted($script:ClientData) } catch { $rooted = $false }
        if (-not $rooted) {
            Die ("-ClientData must be an absolute path or 'none' (got '{0}')" -f $script:ClientData)
        }
    }
    # -SelfRef becomes part of a URL and of nothing else, but a space or a quote in it would break
    # both the request and the path it is written to.
    if ($script:SelfRef -notmatch '^[A-Za-z0-9._/-]+$') {
        Die ("-SelfRef must be a git ref: letters, digits, dot, dash and slash (got '{0}')" -f $script:SelfRef)
    }
    # A root that is one of Windows' own directories would be deleted wholesale by -Uninstall.
    $forbidden = @(
        $env:WINDIR
        $env:ProgramFiles
        ${env:ProgramFiles(x86)}
        $env:ProgramData
        $env:USERPROFILE
        (Join-Path $env:SystemDrive 'Users')
    )
    foreach ($path in $forbidden) {
        if (-not $path) { continue }
        if ($script:Root -eq $path.TrimEnd('\')) {
            Die ("-Root must be a private directory, not {0}" -f $path)
        }
    }
}

function Reset-Paths {
    $script:BinDir = $script:Root
    $script:ConfDir = Join-Path $script:Root 'configs'
    $script:DataDir = Join-Path $script:Root 'data'
    $script:SrcDir = Join-Path $script:Root 'src'
    $script:DbDir = Join-Path $script:Root 'db'
    $script:MysqlDir = Join-Path $script:Root 'mysql'
    $script:LogsDir = Join-Path $script:Root 'logs'
    $script:StateDir = Join-Path $script:Root '.win-state'
    $script:StateFile = Join-Path $script:StateDir 'state.env'
    $script:ModeFile = Join-Path $script:StateDir 'mode'
    # -DockerDir defaults to <root>\docker-src; Confirm-Arguments fills it in as soon as the root is
    # final, and a value read back from the state file survives this.
    if ($script:DockerDir -eq '') { $script:DockerDir = Join-Path $script:Root 'docker-src' }
}

function New-RandomPassword {
    # RandomNumberGenerator, not Get-Random: Get-Random is seeded from the clock and is not meant
    # for secrets. A-Za-z0-9 only, so the password survives a conf file, a .cmd file and SQL.
    $alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789'
    $bytes = New-Object byte[] 16
    $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    try {
        $rng.GetBytes($bytes)
    } finally {
        $rng.Dispose()
    }
    $chars = New-Object System.Text.StringBuilder
    foreach ($b in $bytes) {
        [void]$chars.Append($alphabet[$b % $alphabet.Length])
    }
    return $chars.ToString()
}

# ---------------------------------------------------------------- state and manifest

function Protect-SecretFile {
    <# Protect-SecretFile PATH - drop inherited ACLs and leave the current user alone on it. #>
    # These files hold the db passwords and the gm password in plain text. They live under a -Root the
    # user chooses, so they inherit whatever that directory has - C:\acore inherits from C:\, where
    # Users may read. The Thor side already does this with umask 077.
    param([string]$Path)
    if ($script:DryRun) { return }
    try {
        $acl = Get-Acl -LiteralPath $Path
        $acl.SetAccessRuleProtection($true, $false)
        foreach ($rule in @($acl.Access)) { [void]$acl.RemoveAccessRule($rule) }
        $me = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
        $acl.AddAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule($me, 'FullControl', 'Allow')))
        Set-Acl -LiteralPath $Path -AclObject $acl
    } catch {
        Warn ("could not restrict {0} to the current user: {1}" -f $Path, $_.Exception.Message)
    }
}

function Save-State {
    if ($script:DryRun) {
        Note "record the install state in $script:StateFile"
        return
    }
    [void][System.IO.Directory]::CreateDirectory($script:StateDir)
    $stamp = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    # A run that asked for another ref leaves this empty until that ref resolves; a repair that names
    # the ref already recorded must not erase the pin just because the lookup failed or was skipped.
    $selfRefSha = $script:SelfRefSha
    if (-not $selfRefSha -and $script:SelfRef -eq $script:RecordedSelfRef) { $selfRefSha = $script:RecordedSelfRefSha }
    $lines = @(
        ('# Written by windows-install.ps1 {0} on {1}' -f $ScriptVersion, $stamp)
        '# Read by -Status, -Repair, -Uninstall and the generated wrappers; edit only if you know what you are doing.'
        ('ROOT="{0}"' -f $script:Root)
        ('CONF_DIR="{0}"' -f $script:ConfDir)
        ('BIN_DIR="{0}"' -f $script:BinDir)
        ('DATA_DIR="{0}"' -f $script:DataDir)
        ('SRC_DIR="{0}"' -f $script:SrcDir)
        ('DB_DIR="{0}"' -f $script:DbDir)
        ('MYSQL_DIR="{0}"' -f $script:MysqlDir)
        ('DB_USER="{0}"' -f $script:DbUser)
        ('DB_PASS="{0}"' -f $script:DbPass)
        ('ACCOUNT="{0}"' -f $script:Account)
        ('ACCOUNT_PASS="{0}"' -f $script:AccountPass)
        ('BOTS="{0}"' -f $script:Bots)
        ('HORDE_RATIO="{0}"' -f $script:HordeRatio)
        ('LAN_IP="{0}"' -f $script:LanIp)
        ('ARTIFACT_SHA="{0}"' -f $script:ArtifactSha)
        ('DOCKER_DIR="{0}"' -f $script:DockerDir)
        ('DB_ROOT_PASS="{0}"' -f $script:DockerRootPass)
        ('SELF_REF="{0}"' -f $script:SelfRef)
        ('SELF_REF_SHA="{0}"' -f $selfRefSha)
        ('VERSION="{0}"' -f $ScriptVersion)
        ('INSTALLED_AT="{0}"' -f $stamp)
    )
    [System.IO.File]::WriteAllLines($script:StateFile, $lines)
    Protect-SecretFile -Path $script:StateFile
}

function Import-State {
    <# Import-State [-Quiet] - load <root>\.win-state\state.env; returns $false when there is none. #>
    # -Quiet is for the modes that read the state before they know whether there is one: a -Check or
    # a status of a fresh directory is not worth a warning about a missing install.
    param([switch]$Quiet)
    if (-not (Test-Path -LiteralPath $script:StateFile)) {
        if (-not $Quiet -and $script:Action -ne 'install') {
            Warn "no state at $script:StateFile - nothing installed here?"
        }
        return $false
    }
    foreach ($line in [System.IO.File]::ReadAllLines($script:StateFile)) {
        if ($line -match '^\s*#') { continue }
        $eq = $line.IndexOf('=')
        if ($eq -lt 1) { continue }
        $key = $line.Substring(0, $eq).Trim()
        $value = $line.Substring($eq + 1).Trim()
        if ($value.Length -ge 2 -and $value.StartsWith('"') -and $value.EndsWith('"')) {
            $value = $value.Substring(1, $value.Length - 2)
        }
        # The command line wins over the state file, exactly like state_apply_cli.
        switch ($key) {
            # A root read from the state file was given explicitly when the install ran, so it is
            # not the default and the Program Files / space checks must not refuse it now.
            'ROOT' {
                if (-not $script:Bound.ContainsKey('Root')) {
                    $script:Root = $value
                    $script:RootGiven = $true
                }
            }
            'DB_USER' { if (-not $script:Bound.ContainsKey('DbUser')) { $script:DbUser = $value } }
            'DB_PASS' { if (-not $script:Bound.ContainsKey('DbPass')) { $script:DbPass = $value } }
            'ACCOUNT' { if (-not $script:Bound.ContainsKey('Account')) { $script:Account = $value } }
            'ACCOUNT_PASS' { if (-not $script:Bound.ContainsKey('AccountPass')) { $script:AccountPass = $value } }
            'BOTS' { if (-not $script:Bound.ContainsKey('Bots')) { $script:Bots = [int]$value } }
            'HORDE_RATIO' { if (-not $script:Bound.ContainsKey('HordeRatio')) { $script:HordeRatio = [int]$value } }
            'LAN_IP' { if (-not $script:Bound.ContainsKey('LanIp')) { $script:LanIp = $value } }
            'ARTIFACT_SHA' { $script:ArtifactSha = $value }
            'DOCKER_DIR' { if ($value -and -not $script:Bound.ContainsKey('DockerDir')) { $script:DockerDir = $value } }
            'DB_ROOT_PASS' { $script:DockerRootPass = $value }
            'SELF_REF' {
                if ($value) { $script:RecordedSelfRef = $value }
                if ($value -and -not $script:Bound.ContainsKey('SelfRef')) { $script:SelfRef = $value }
            }
            # Recorded once, at the run that resolved the ref; the command line can still override it.
            'SELF_REF_SHA' {
                $script:RecordedSelfRefSha = $value
                if (-not $script:Bound.ContainsKey('SelfRef')) { $script:SelfRefSha = $value }
            }
        }
    }
    Reset-Paths
    return $true
}

function Get-ModeText {
    <# Get-ModeText - 'native' or 'docker' as recorded in <root>\.win-state\mode; '' when absent. #>
    if (-not $script:ModeFile) { return '' }
    if (-not (Test-Path -LiteralPath $script:ModeFile)) { return '' }
    $text = ([System.IO.File]::ReadAllText($script:ModeFile)).Trim().ToLowerInvariant()
    if ($text -eq 'docker') { return 'docker' }
    if ($text -eq 'native') { return 'native' }
    return ''
}

function Save-Mode {
    <# Save-Mode MODE - record which sort of install lives in this root. #>
    param([string]$Mode)
    if ($script:DryRun) {
        Note ("record the install mode '{0}' in {1}" -f $Mode, $script:ModeFile)
        return
    }
    [void][System.IO.Directory]::CreateDirectory($script:StateDir)
    [System.IO.File]::WriteAllText($script:ModeFile, ($Mode + "`r`n"), (New-Object -TypeName System.Text.ASCIIEncoding))
}

function Assert-ModeFree {
    <# Assert-ModeFree MODE - refuse to install one mode over the other in the same root. #>
    param([string]$Mode)
    $recorded = Get-ModeText
    if ($recorded -and $recorded -ne $Mode) {
        Die ("{0} already holds a {1} install (see {2}): remove it with -Uninstall first. The two modes share no state, and the {1} path must not tear down what the other one built" -f $script:Root, $recorded, $script:ModeFile)
    }
    # An install from before the mode file existed has no record of which path built it. Guessing
    # wrong would orphan it: the compose project would take the root over and -Uninstall would stop
    # addressing the native realm.
    if (-not $recorded -and $Mode -eq 'docker' -and (Test-Path -LiteralPath $script:StateFile)) {
        Die ("{0} already holds an install with no recorded mode ({1}): remove it with -Uninstall first, then run -Docker" -f $script:Root, $script:StateFile)
    }
}

function Resolve-Mode {
    # -Docker wins; otherwise the mode recorded when this root was installed decides. That is what
    # keeps -Status, -Repair and -Uninstall on the same path as the install: a container realm is
    # inspected with docker compose, never by looking for <root>\worldserver.exe.
    if ($script:Docker) { return 'docker' }
    $recorded = Get-ModeText
    if ($recorded) { return $recorded }
    return 'native'
}

function Add-Manifest {
    <# Add-Manifest ENTRY - record a path (or firewall:PORT) this install created outside the root. #>
    param([string]$Entry)
    if ($script:DryRun) {
        Note "record $Entry in the manifest"
        return
    }
    [void][System.IO.Directory]::CreateDirectory($script:HostState)
    Add-Content -LiteralPath $script:ManifestFile -Value $Entry -Encoding ASCII
}

function Get-ManifestEntries {
    if (-not (Test-Path -LiteralPath $script:ManifestFile)) { return @() }
    $entries = @()
    foreach ($line in [System.IO.File]::ReadAllLines($script:ManifestFile)) {
        $entry = $line.Trim()
        if ($entry -and ($entries -notcontains $entry)) { $entries += $entry }
    }
    return $entries
}

function Test-SafeRemovePath {
    <# Test-SafeRemovePath PATH - is this path clearly owned by this install? #>
    param([string]$Path)
    if (-not $Path) { return $false }
    if (-not [System.IO.Path]::IsPathRooted($Path)) {
        Warn "refusing to remove relative path: $Path"
        return $false
    }
    $full = [System.IO.Path]::GetFullPath($Path)
    if ($full -match '^[A-Za-z]:\\?$') {
        Warn "refusing to remove the drive root $full"
        return $false
    }
    $guarded = @($env:WINDIR, $env:ProgramFiles, ${env:ProgramFiles(x86)}, $env:ProgramData, $env:USERPROFILE, $script:Root)
    foreach ($keep in $guarded) {
        if (-not $keep) { continue }
        if ($full.TrimEnd('\') -eq $keep.TrimEnd('\')) {
            # The root is removed deliberately by Invoke-Uninstall, not through the manifest walk.
            if ($full.TrimEnd('\') -eq $script:Root.TrimEnd('\')) { return $true }
            Warn "refusing to remove $full"
            return $false
        }
    }
    # Everything inside the install root is this install's own, including a manifest entry such as
    # <root>\docker-src (the container staging directory): the root was chosen by the caller and is
    # removed on purpose by Invoke-Uninstall anyway.
    $rootPrefix = $script:Root.TrimEnd('\') + '\'
    if ($full.StartsWith($rootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) { return $true }
    # Everything this install owns outside the root is under the user profile; anything else is
    # either a path this script does not know about or a mistake in the manifest.
    $profileRoot = $env:USERPROFILE.TrimEnd('\') + '\'
    if (-not $full.StartsWith($profileRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        Warn "refusing to remove $full (not under $env:USERPROFILE)"
        return $false
    }
    return $true
}

# ---------------------------------------------------------------- config files

function Set-Conf {
    <# Set-Conf FILE KEY VALUE - replace the first live "KEY = ..." line, else append. #>
    param(
        [string]$File,
        [string]$Key,
        [string]$Value
    )
    if (-not (Test-Path -LiteralPath $File)) {
        Warn "no such config file: $File"
        return
    }
    $result = New-Object System.Collections.Generic.List[string]
    $done = $false
    foreach ($line in [System.IO.File]::ReadAllLines($File)) {
        if (-not $done -and -not ($line -match '^\s*#')) {
            $eq = $line.IndexOf('=')
            if ($eq -ge 0) {
                # -ceq: conf keys are case-sensitive to ConfigMgr and "datadir" is not "DataDir".
                if ($line.Substring(0, $eq).Trim() -ceq $Key) {
                    $result.Add(('{0} = {1}' -f $Key, $Value))
                    $done = $true
                    continue
                }
            }
        }
        $result.Add($line)
    }
    if (-not $done) { $result.Add(('{0} = {1}' -f $Key, $Value)) }
    # WriteAllLines: CRLF, UTF-8 without BOM, and one final newline - the core reads the file with
    # std::getline and trims, so a BOM in front of the first key would become part of its name.
    [System.IO.File]::WriteAllLines($File, $result.ToArray())
}

function Get-Conf {
    <# Get-Conf FILE KEY - the live value (the last uncommented definition), '' when absent. #>
    param(
        [string]$File,
        [string]$Key
    )
    if (-not (Test-Path -LiteralPath $File)) { return '' }
    $found = ''
    foreach ($line in [System.IO.File]::ReadAllLines($File)) {
        if ($line -match '^\s*#') { continue }
        $eq = $line.IndexOf('=')
        if ($eq -lt 0) { continue }
        if ($line.Substring(0, $eq).Trim() -ceq $Key) {
            $found = $line.Substring($eq + 1).Trim()
        }
    }
    return $found
}

function Get-ConfUnquoted {
    param(
        [string]$File,
        [string]$Key
    )
    return ((Get-Conf -File $File -Key $Key) -replace '"', '')
}

function Remove-ConfMatch {
    <# Remove-ConfMatch FILE REGEX - delete every live line whose key matches REGEX. #>
    param(
        [string]$File,
        [string]$KeyPattern
    )
    if (-not (Test-Path -LiteralPath $File)) { return }
    $keep = New-Object System.Collections.Generic.List[string]
    foreach ($line in [System.IO.File]::ReadAllLines($File)) {
        $drop = $false
        if (-not ($line -match '^\s*#')) {
            $eq = $line.IndexOf('=')
            if ($eq -ge 0) {
                $name = $line.Substring(0, $eq).Trim()
                if ($name -cmatch $KeyPattern) { $drop = $true }
            }
        }
        if (-not $drop) { $keep.Add($line) }
    }
    [System.IO.File]::WriteAllLines($File, $keep.ToArray())
}

function Get-ConfPair {
    <# Get-ConfPair RELATIVE - the live conf, created from its .dist on first use. #>
    param([string]$Relative)
    $dist = Join-Path $script:ConfDir ($Relative + '.conf.dist')
    $live = Join-Path $script:ConfDir ($Relative + '.conf')
    if (-not (Test-Path -LiteralPath $live)) {
        if (-not (Test-Path -LiteralPath $dist)) {
            Die "missing $dist (is the artifact complete?)"
        }
        Copy-Item -LiteralPath $dist -Destination $live -Force
        Log "created $live"
    }
    return $live
}

function Set-ConfDefaults {
    <# Set-ConfDefaults -File F -Check {..} -Label L -Defaults @('KEY=VALUE', ...) #>
    # A key the file does not have at all is not a bad value: the live conf was written by an older
    # install and Get-ConfPair deliberately never overwrites one, so an upgrade leaves the new keys
    # absent. They are appended with the shipped value and reported once, as a count.
    # A key that is present and out of range is different: the module silently replaces an unusable
    # value with its own default, so the file and the running server disagree until it is fixed.
    param(
        [string]$File,
        [scriptblock]$Check,
        [string]$Label,
        [string[]]$Defaults
    )
    $added = 0
    $fixed = 0
    foreach ($default in $Defaults) {
        $eq = $default.IndexOf('=')
        $key = $default.Substring(0, $eq)
        $want = $default.Substring($eq + 1)
        $value = Get-Conf -File $File -Key $key
        if ($value -eq '') {
            Set-Conf -File $File -Key $key -Value $want
            $added++
        } elseif (-not (& $Check $key $value)) {
            Warn "${Label}: $key is '$value', restoring the shipped default $want"
            Set-Conf -File $File -Key $key -Value $want
            $fixed++
        }
    }
    if ($added -gt 0) {
        Ok "${Label}: appended $added key(s) the live file did not have yet, values as shipped"
    }
    if ($fixed -gt 0) {
        Ok "${Label}: restored $fixed drifted key(s) to their shipped defaults"
    }
}

function Test-MercenaryValueOk {
    <# Test-MercenaryValueOk KEY VALUE - true when mod-mercenaries accepts VALUE. #>
    param(
        [string]$Key,
        [string]$Value
    )
    # Mercenary.Enable and Mercenary.Debug are 0|1, MaxMercenaries 0-4 (the gossip page has room
    # for four party rows), LevelWindow 0-10, OfferedPerPage 1-14, the featured count 1-50 each,
    # the pool refresh 1-1440 minutes each, the two fee percentages 0-10000 and 0-1000, and the
    # gear band 25-100 for the floor and 100-300 for the cap.
    if ($Key -ceq 'Mercenary.Voice') {
        # A voice pool is a profile list, not a number - labels, strategies and | separators - so
        # it is settled before the numeric guard can see it.
        return ($Value.Length -gt 1 -and $Value.Contains(':'))
    }
    if ($Value -notmatch '^[0-9]+$') { return $false }
    $n = [int64]$Value
    switch -CaseSensitive ($Key) {
        'Mercenary.Enable' { return ($n -eq 0 -or $n -eq 1) }
        'Mercenary.Debug' { return ($n -eq 0 -or $n -eq 1) }
        'Mercenary.MaxMercenaries' { return ($n -ge 0 -and $n -le 4) }
        'Mercenary.LevelWindow' { return ($n -ge 0 -and $n -le 10) }
        'Mercenary.GainXP' { return ($n -eq 0 -or $n -eq 1) }
        # MercenaryMgr.cpp:170 clamps it to 0-3600, so a larger value is accepted here and then
        # silently run at 3600: the file and the server would disagree, which is what the repair
        # path exists to prevent. check_mercenaries.py holds the two shells to the same range.
        'Mercenary.DismissGraceSeconds' { return ($n -ge 0 -and $n -le 3600) }
        'Mercenary.OfferedPerPage' { return ($n -ge 1 -and $n -le 14) }
        'Mercenary.HireFeePercent' { return ($n -ge 0 -and $n -le 10000) }
        'Mercenary.HireFeeHeldPercent' { return ($n -ge 0 -and $n -le 1000) }
        'Mercenary.GearFloorPercent' { return ($n -ge 25 -and $n -le 100) }
        'Mercenary.GearCapPercent' { return ($n -ge 100 -and $n -le 300) }
        'Mercenary.ResupplyFeePercent' { return ($n -ge 0 -and $n -le 10000) }
        'Mercenary.VoiceKillChancePercent' { return ($n -ge 0 -and $n -le 100) }
        'Mercenary.VoiceCooldownSeconds' { return ($n -ge 0 -and $n -le 3600) }
        'Mercenary.AffinityEnabled' { return ($n -eq 0 -or $n -eq 1) }
        'Mercenary.AffinityKillPoints' { return ($n -ge 0 -and $n -le 1000) }
        'Mercenary.AffinityQuestPoints' { return ($n -ge 0 -and $n -le 1000) }
        'Mercenary.AffinityStep' { return ($n -ge 1 -and $n -le 100000) }
        'Mercenary.AffinityGearPercent' { return ($n -ge 0 -and $n -le 100) }
        'Mercenary.AffinityMaxSteps' { return ($n -ge 0 -and $n -le 20) }
        'Mercenary.FeaturedMin' { return ($n -ge 1 -and $n -le 50) }
        'Mercenary.FeaturedMax' { return ($n -ge 1 -and $n -le 50) }
        'Mercenary.PoolRefreshMinMinutes' { return ($n -ge 1 -and $n -le 1440) }
        'Mercenary.PoolRefreshMaxMinutes' { return ($n -ge 1 -and $n -le 1440) }
        default { return $false }
    }
}

function Test-SurvivorValueOk {
    <# Test-SurvivorValueOk KEY VALUE - true when mod-survivor accepts VALUE. #>
    param(
        [string]$Key,
        [string]$Value
    )
    # The ranges are the ones mod-survivor's own conf comments state: the 0|1 switches, a minimum
    # flag level inside the level cap, a container lifetime in days, and a positive template entry.
    if ($Value -notmatch '^[0-9]+$') { return $false }
    $n = [int64]$Value
    switch -CaseSensitive ($Key) {
        'Survivor.Enable' { return ($n -eq 0 -or $n -eq 1) }
        'Survivor.AnnounceDeaths' { return ($n -eq 0 -or $n -eq 1) }
        'Survivor.DropEquipped' { return ($n -eq 0 -or $n -eq 1) }
        'Survivor.DropBags' { return ($n -eq 0 -or $n -eq 1) }
        'Survivor.MarkOnMap' { return ($n -eq 0 -or $n -eq 1) }
        'Survivor.ApplyToBots' { return ($n -eq 0 -or $n -eq 1) }
        'Survivor.BotRegenOnDeath' { return ($n -eq 0 -or $n -eq 1) }
        'Survivor.BotDropContainer' { return ($n -eq 0 -or $n -eq 1) }
        'Survivor.MinLevelToFlag' { return ($n -ge 1 -and $n -le 80) }
        'Survivor.ContainerLifetimeDays' { return ($n -ge 1 -and $n -le 365) }
        'Survivor.ContainerEntry' { return ($n -gt 0) }
        default { return $false }
    }
}

function Test-SkateboardValueOk {
    <# Test-SkateboardValueOk KEY VALUE - true when mod-skateboard accepts VALUE. #>
    param(
        [string]$Key,
        [string]$Value
    )
    # The ranges are the ones the contract states: the 0|1 switches, MaxSpeed 1-40 yd/s,
    # PushImpulse and LandingBonus 0-20 yd/s, the static/rolling friction 0-20 and 0-1, SlopeGain
    # 0-90, the visual kit 0-100000 (0 = no flair), crash damage 0-99 percent, the stagger 0-10000
    # ms and the ACK timeout 50-5000 ms. Deck and Deck.Render are names, not ranges, so both are
    # settled before the numeric guard can see them; the momentum keys carry decimals, so the
    # comparison is a double, not an int64.
    if ($Key -ceq 'Skateboard.Deck') {
        if ($Value -ceq 'log' -or $Value -ceq 'engravedwall' -or $Value -ceq 'coffinlid') { return $true }
        if ($Value -notmatch '^[0-9]{1,12}$') { return $false }
        return ([int64]$Value -gt 0)
    }
    if ($Key -ceq 'Skateboard.Deck.Render') { return ($Value -ceq 'unit' -or $Value -ceq 'mount') }
    if ($Key -ceq 'Skateboard.Trick.List') {
        # A list, not a number: `name:emote[:kit]` entries separated by commas, settled here so the
        # numeric guard below never sees it. The module drops an entry it cannot use, so anything
        # with a colon is accepted - without this branch the shipped list was "unusable" and was
        # rewritten on every -Repair.
        return ($Value -cmatch ':')
    }
    # The switches are checked as text first: an integer-only guard would accept 1.0, which the
    # core's sConfigMgr does not read as a switch.
    if (@('Skateboard.Enable', 'Skateboard.Tricks', 'Skateboard.Crash.Enable',
          'Skateboard.HardcoreSafe', 'Skateboard.OffOnHit', 'Skateboard.Debug',
          'Skateboard.Drive') -ccontains $Key) {
        return ($Value -ceq '0' -or $Value -ceq '1')
    }
    # Bounded digits, so a value pasted out of a log cannot hand an unbounded string to Parse (and
    # to the $ErrorActionPreference = 'Stop' abort that follows a throw).
    if ($Value -notmatch '^[0-9]{1,12}(\.[0-9]{1,6})?$') { return $false }
    $n = [double]::Parse($Value, [System.Globalization.CultureInfo]::InvariantCulture)
    if ($Key -ceq 'Skateboard.Deck.Carrier') { return ($n -ge 1 -and $n -le 4294967295) }
    if ($Key -ceq 'Skateboard.Push.Mode') { return ($n -eq 0 -or $n -eq 1) }
    switch -CaseSensitive ($Key) {
        'Skateboard.BoardCastMs' { return ($n -ge 0 -and $n -le 10000) }
        'Skateboard.MaxSpeed' { return ($n -ge 1 -and $n -le 40) }
        'Skateboard.Cruise' { return ($n -ge 0 -and $n -le 20) }
        'Skateboard.Deck.FollowMs' { return ($n -ge 20 -and $n -le 1000) }
        'Skateboard.PushImpulse' { return ($n -ge 0 -and $n -le 20) }
        'Skateboard.Push.IntervalMs' { return ($n -ge 100 -and $n -le 2000) }
        'Skateboard.Push.MaxHoldMs' { return ($n -ge 100 -and $n -le 5000) }
        'Skateboard.Friction.Static' { return ($n -ge 0 -and $n -le 20) }
        'Skateboard.Friction.Rolling' { return ($n -ge 0 -and $n -le 1) }
        'Skateboard.SlopeGain' { return ($n -ge 0 -and $n -le 90) }
        'Skateboard.Drive.ImpulseMs' { return ($n -ge 100 -and $n -le 2000) }
        'Skateboard.Drive.GroundMs' { return ($n -ge 0 -and $n -le 1000) }
        'Skateboard.Drive.MinSpeed' { return ($n -ge 0 -and $n -le 40) }
        'Skateboard.Brake' { return ($n -ge 0 -and $n -le 50) }
        'Skateboard.LandingBonus' { return ($n -ge 0 -and $n -le 20) }
        'Skateboard.Trick.VisualKit' { return ($n -ge 0 -and $n -le 100000) }
        'Skateboard.Crash.DamagePct' { return ($n -ge 0 -and $n -le 99) }
        'Skateboard.Crash.StaggerMs' { return ($n -ge 0 -and $n -le 10000) }
        'Skateboard.AckTimeoutMs' { return ($n -ge 50 -and $n -le 5000) }
        default { return $false }
    }
}

function Add-MercenaryPoolKeys {
    <# Add-MercenaryPoolKeys -Live F -Dist F - append the shipped pool keys the live file lacks. #>
    # The pool keys are lists, not scalars, so they stay out of the defaults list: Set-Conf would
    # overwrite a pool the player had trimmed. But Get-ConfPair only copies the .dist when the live
    # conf is missing, so an install made before the pools shipped keeps a conf with no
    # Mercenary.Temperament.* block at all, and MercenaryPersonality::Load has nothing to roll from.
    param(
        [string]$Live,
        [string]$Dist
    )
    if (-not (Test-Path -LiteralPath $Live) -or -not (Test-Path -LiteralPath $Dist)) { return }
    $pending = New-Object System.Collections.Generic.List[string]
    foreach ($line in [System.IO.File]::ReadAllLines($Dist)) {
        $eq = $line.IndexOf('=')
        if ($eq -lt 1) { continue }
        $key = $line.Substring(0, $eq).Trim()
        $isPool = ($key -ceq 'Mercenary.Voice') -or
                  ($key -cmatch '^Mercenary\.VoiceLine\.') -or
                  ($key -cmatch '^Mercenary\.Temperament\.[0-9]+\.[0-9]+')
        if (-not $isPool) { continue }
        # An empty value counts as absent here, unlike for a scalar: a pool is never legitimately
        # empty, and Get-Conf prints nothing for it.
        if ((Get-Conf -File $Live -Key $key) -ne '') { continue }
        $pending.Add($line)
    }
    if ($pending.Count -eq 0) { return }
    $append = New-Object System.Collections.Generic.List[string]
    $append.Add('')
    $append.Add('# Pool keys this file did not have: appended by the installer, values as shipped.')
    foreach ($line in $pending) { $append.Add($line) }
    [System.IO.File]::AppendAllLines($Live, $append.ToArray())
    Ok ("mod-mercenaries.conf: appended {0} shipped pool keys (voice/temperament)" -f $pending.Count)
}

function Get-StaleNearValue {
    <# Get-StaleNearValue KEY - old defaults this installer wrote, so a repair can replace one. #>
    param([string]$Key)
    switch -CaseSensitive ($Key) {
        'RandomBotNearPlayersLevelRange' { return @('5') }
        'RandomBotNearPlayersMaxPerPlayer' { return @('10') }
        default { return @() }
    }
}

# ---------------------------------------------------------------- database

function Get-MysqlPath {
    param([string]$Exe)
    return (Join-Path (Join-Path $script:MysqlDir 'bin') $Exe)
}

function Get-MysqlHostArgs {
    # root@localhost is what --initialize-insecure creates, and a loopback TCP client is matched as
    # localhost by MySQL 8.4; the explicit host is tried first because the account the servers use
    # is created as user@127.0.0.1 and a build that resolves hosts differently must not lock the
    # installer out of its own database.
    if ($script:AdminHost) {
        return @('--protocol=tcp', ('-h{0}' -f $script:AdminHost), ('-P{0}' -f $DbPort), '-uroot')
    }
    return @('--protocol=tcp', ('-P{0}' -f $DbPort), '-uroot')
}

function Invoke-MysqlAdmin {
    <# Invoke-MysqlAdmin -Arguments ARGS - run the bundled mysql client as root. #>
    # The caller reads $LASTEXITCODE; the output is returned so a query can use it, and the client's
    # own error text is dropped because the failure paths print the tail of logs\mysql.err instead.
    param([string[]]$Arguments)
    $exe = Get-MysqlPath 'mysql.exe'
    if (-not (Test-Path -LiteralPath $exe)) { Die "no bundled mysql client at $exe (the MySQL step did not run)" }
    $argvList = @(Get-MysqlHostArgs) + @($Arguments)
    # Through Invoke-Native: a client that writes its complaint to stderr would otherwise be a
    # terminating error under $ErrorActionPreference = 'Stop' (see Invoke-Native). Its error text is
    # dropped (-StdOutOnly) because the failure paths print the tail of logs\mysql.err instead.
    $out = Invoke-Native -FilePath $exe -Arguments $argvList -StdOutOnly
    return $out
}

function Invoke-Sql {
    <# Invoke-Sql -Query QUERY [-Database DB] - tab-separated rows, or $null when it failed. #>
    param(
        [string]$Query,
        [string]$Database = ''
    )
    if (-not (Test-Path -LiteralPath (Get-MysqlPath 'mysql.exe'))) { return $null }
    $argvList = @('-N', '-B', '-e', $Query)
    if ($Database) { $argvList += $Database }
    $out = Invoke-MysqlAdmin -Arguments $argvList
    if ($LASTEXITCODE -ne 0) { return $null }
    return @($out)
}

function Test-DbHost {
    <# Test-DbHost HOST - does mysqladmin ping answer as root against one connection form? #>
    param([string]$HostName)
    $exe = Get-MysqlPath 'mysqladmin.exe'
    if (-not (Test-Path -LiteralPath $exe)) { return $false }
    $argvList = @()
    if ($HostName) {
        $argvList += @('--protocol=tcp', ('-h{0}' -f $HostName), ('-P{0}' -f $DbPort), '-uroot')
    } else {
        $argvList += @('--protocol=tcp', ('-P{0}' -f $DbPort), '-uroot')
    }
    $argvList += 'ping'
    # Invoke-Native, not a bare '&': mysqladmin prints "mysqld is alive" or an error, and either can
    # arrive on stderr, which $ErrorActionPreference = 'Stop' would turn into a fatal record.
    $null = Invoke-Native -FilePath $exe -Arguments $argvList -StdOutOnly
    return ($LASTEXITCODE -eq 0)
}

function Test-Db {
    <# Test-Db - is mysqld answering ping? Also settles which host string can log in as root. #>
    # root@localhost is what --initialize-insecure creates, and a loopback TCP client is matched as
    # localhost by MySQL 8.4; a build that resolves hosts differently matches the literal
    # 127.0.0.1 instead. Both are tried here, and whichever answers is remembered, so every later
    # statement uses a connection form that is known to work.
    foreach ($candidate in @('127.0.0.1', '')) {
        if (Test-DbHost -HostName $candidate) {
            $script:AdminHost = $candidate
            return $true
        }
    }
    return $false
}

function Resolve-AdminHost {
    if (Test-Db) { return }
    Die "cannot talk to MySQL as root over 127.0.0.1:$DbPort (see $script:LogsDir\mysql.err)"
}

function Test-SchemaExists {
    param([string]$Name)
    $rows = Invoke-Sql ("SELECT COUNT(*) FROM information_schema.SCHEMATA WHERE SCHEMA_NAME='{0}'" -f $Name)
    if ($null -eq $rows) { return $false }
    return (($rows -join '').Trim() -eq '1')
}

function Get-SchemaTableCount {
    param([string]$Name)
    $rows = Invoke-Sql ("SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA='{0}'" -f $Name)
    if ($null -eq $rows) { return 0 }
    $text = ($rows -join '').Trim()
    # An aggregate always returns a row; an empty answer means the client failed, and a cast of ''
    # would throw instead of reporting zero tables.
    if ($text -notmatch '^[0-9]+$') { return 0 }
    return [int]$text
}

function Wait-DbUp {
    param([int]$Seconds = 60)
    for ($i = 0; $i -lt $Seconds; $i++) {
        if (Test-Db) { return $true }
        Start-Sleep -Seconds 1
    }
    return (Test-Db)
}

function Wait-DbDown {
    param([int]$Seconds = 60)
    for ($i = 0; $i -lt $Seconds; $i++) {
        if (-not (Test-Db)) { return $true }
        Start-Sleep -Seconds 1
    }
    return (-not (Test-Db))
}

function Start-Db {
    if (Test-Db) { return }
    if ($script:DryRun) {
        Note "start mysqld from $script:MysqlDir on 127.0.0.1:$DbPort"
        return
    }
    $mysqld = Get-MysqlPath 'mysqld.exe'
    if (-not (Test-Path -LiteralPath $mysqld)) { Die "no bundled mysqld at $mysqld (the MySQL step did not run)" }
    # A mysqld that is still shutting down holds the datadir: starting a second one over it fails
    # to lock it, or leaves both flushing the same pages.
    if (@(Get-Process -Name 'mysqld' -ErrorAction SilentlyContinue).Count -gt 0) {
        if (Wait-DbUp 120) { return }
        if (@(Get-Process -Name 'mysqld' -ErrorAction SilentlyContinue).Count -gt 0) {
            Die "a mysqld is running but not answering on 127.0.0.1:$DbPort; stop it and re-run (see $script:LogsDir\mysql.err)"
        }
    }
    [void][System.IO.Directory]::CreateDirectory($script:LogsDir)
    $mysqldArgs = @(
        ('--basedir="{0}"' -f $script:MysqlDir)
        ('--datadir="{0}"' -f $script:DbDir)
        ('--port={0}' -f $DbPort)
        '--bind-address=127.0.0.1'
        # --console sends the error log to stderr, which is the file this script and -Status name in
        # their failures; without it Windows would put it in the datadir as a .err file.
        '--console'
        # Every account is created as user@127.0.0.1 and the servers connect by IP, so a DNS lookup
        # on each connection buys nothing and can hang on a host with no resolver.
        '--skip-name-resolve'
        '--mysqlx=0'
    )
    Start-Process -FilePath $mysqld -ArgumentList $mysqldArgs -WorkingDirectory $script:MysqlDir `
        -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $script:LogsDir 'mysql.out') `
        -RedirectStandardError (Join-Path $script:LogsDir 'mysql.err') | Out-Null
    if (-not (Wait-DbUp 60)) {
        Die "mysqld did not come up within 60s; see $script:LogsDir\mysql.err"
    }
}

function Stop-Db {
    if (-not (Test-Db)) { return }
    if ($script:DryRun) {
        Note 'stop mysqld'
        return
    }
    $exe = Get-MysqlPath 'mysqladmin.exe'
    $argvList = @(Get-MysqlHostArgs) + @('shutdown')
    $null = Invoke-Native -FilePath $exe -Arguments $argvList -StdOutOnly
    # mysqladmin returns as soon as the shutdown is accepted, while InnoDB is still writing out and
    # the datadir is still locked: a start that follows immediately would race the shutdown for it.
    if (-not (Wait-DbDown 60)) {
        Warn "mysqld is still running after 60s; see $script:LogsDir\mysql.err"
    }
}

# ---------------------------------------------------------------- install phases

function Get-DockerRootExecBase {
    <# Get-DockerRootExecBase - argv prefix for a root client inside the db container. #>
    # 'MYSQL_PWD' by name, not 'MYSQL_PWD=secret': the value form puts the root password in the
    # docker CLI's own argv, where /proc/<pid>/cmdline shows it to every local user for as long as
    # the call runs - seconds to minutes for a mysqldump. The name-only form takes the value from
    # this process's environment, which other users cannot read. Set it with Use-DockerRootPassword.
    return @('exec', '-T', '-e', 'MYSQL_PWD', 'db')
}

function Use-DockerRootPassword {
    <# Use-DockerRootPassword SCRIPTBLOCK - run it with MYSQL_PWD set to the root password. #>
    param([scriptblock]$Action)
    $previous = $env:MYSQL_PWD
    $env:MYSQL_PWD = $script:DockerRootPass
    try {
        return (& $Action)
    } finally {
        if ($null -eq $previous) {
            Remove-Item Env:MYSQL_PWD -ErrorAction SilentlyContinue
        } else {
            $env:MYSQL_PWD = $previous
        }
    }
}

function Test-DockerRootPassword {
    <# Test-DockerRootPassword -Password P - true when the running db accepts P as root. #>
    param([string]$Password)
    if (-not $Password) { return $false }
    if ($script:DryRun) { return $false }
    $id = ([string](Invoke-Compose -Arguments @('ps', '-q', 'db') -Capture -AllowFailure)).Trim()
    if (-not $id) { return $false }
    if ((Get-DockerInspectField -Id $id -Format '{{.State.Status}}') -ne 'running') { return $false }
    $probe = @(Get-DockerRootExecBase) + @('mysql', '--protocol=TCP', '-h', '127.0.0.1', '-uroot', '-N', '-e', 'select 1')
    $previous = $script:DockerRootPass
    $script:DockerRootPass = $Password
    try {
        $null = Use-DockerRootPassword { Invoke-Compose -Arguments $probe -Capture -AllowFailure }
        return ($LASTEXITCODE -eq 0)
    } finally {
        $script:DockerRootPass = $previous
    }
}

function Resolve-DockerRootPassword {
    <# Resolve-DockerRootPassword - adopt the compose default when the volume answers to it. #>
    # The recorded password is authoritative; this only covers a volume created before the state
    # existed (or by hand), where the password is the compose file's own default. Adopting it is the
    # difference between a -Repair that continues and one that stops and tells the user to wipe the
    # realm for a password that is already correct.
    if ($script:DockerRootPass) { return }
    if ($script:DryRun) { return }
    $id = ([string](Invoke-Compose -Arguments @('ps', '-q', 'db') -Capture -AllowFailure)).Trim()
    if (-not $id) { return }
    if (Test-DockerRootPassword -Password 'acore') {
        $script:DockerRootPass = 'acore'
        Save-State
        Note "the db volume answers to the compose default root password; recording it in $script:StateFile"
        # Adopted, not rotated: rotating needs the ALTER USER this password cannot run yet, and
        # replacing a working credential behind the user's back is worse than saying so. It is still
        # the password published in docker/docker-compose.yml, so say so.
        Warn '  that default is published in docker/docker-compose.yml: anything that can reach the db container can log in as root'
        Warn '  to change it, set DB_ROOT_PASS and DB_PASS in a .env beside the compose file and reinstall, or ALTER USER inside the volume'
    }
}

function Assert-DockerRootPassword {
    <# Assert-DockerRootPassword - the recorded root password has to be the volume's, or stop now. #>
    # The db volume keeps the root password it was initialised with and nothing can read it back out.
    # Without this probe the mismatch surfaces as the entrypoint's
    # "ERROR 1045 (28000): Access denied for user 'root'@'172.18.0.3'" after the image build and the
    # data download, and the remedy is a sentence in a log nobody is watching.
    if (-not $script:DockerRootPass) { return }
    if ($script:DryRun) { return }
    $id = ([string](Invoke-Compose -Arguments @('ps', '-q', 'db') -Capture -AllowFailure)).Trim()
    if (-not $id) { return }   # a fresh volume: the db service has not been created yet
    # A stopped db cannot be probed, and the start that follows is the right place for its failure.
    if ((Get-DockerInspectField -Id $id -Format '{{.State.Status}}') -ne 'running') { return }
    if (Test-DockerRootPassword -Password $script:DockerRootPass) { return }
    $compose = Get-DockerComposeFile
    Die ("the db volume does not accept the root password recorded in {0}, so the entrypoint cannot create the schemas or the realm user. Nothing below this point can succeed.`n" +
        "  the volume keeps the password it was first initialised with, and it cannot be read back; either`n" +
        ("    docker compose -f {0} down -v   and install again (this drops the realm), or`n" -f $compose) +
        "  reset root inside the volume - skip-grant-tables lets it in without a password:`n" +
        "    docker run -d --rm --name acore-reset -v acore-realm_db:/var/lib/mysql mysql:8.4 --skip-grant-tables --skip-networking`n" +
        "    docker exec acore-reset mysql -uroot -e `"FLUSH PRIVILEGES; ALTER USER 'root'@'localhost' IDENTIFIED BY 'acore'; ALTER USER 'root'@'%' IDENTIFIED BY 'acore';`"`n" +
        "    docker stop acore-reset   (then run this again)")
}

function Invoke-Preflight {
    Step 'Preflight'
    $arch = Get-ProcessorArchitecture
    if (-not [Environment]::Is64BitOperatingSystem) {
        Die "this script targets Windows x64 (this is a 32-bit Windows)"
    }
    if ($arch -ne 'AMD64') {
        Die ("this script targets Windows x64 (got {0}); the artifact is built for x64" -f $arch)
    }
    if ($script:Action -eq 'install' -or $script:Action -eq 'repair' -or $script:Action -eq 'uninstall') {
        if (-not (Test-Elevated)) {
            Die 'run this from an elevated PowerShell (Run as Administrator): the install writes to C:\ and manages the inbound firewall rules'
        }
    } elseif (-not (Test-Elevated)) {
        # -Status only reads, so a normal session is enough - except for the firewall rules, which
        # Windows only lets an administrator enumerate.
        Warn 'not elevated: -Status can read the install, but the firewall rules may not be readable'
    }
    if ($script:DryRun) { Log 'check mode: nothing will be changed' }
    Log "install root: $script:Root"

    $drive = [System.IO.Path]::GetPathRoot($script:Root)
    $free = Get-FreeSpaceGb
    if ($free -ge 0) {
        Log ("free space on {0}: {1} GB" -f $drive, $free)
        if ($free -lt 6) {
            Warn "only $free GB free on $drive; the server, its client data and MySQL need about 6 GB"
        }
    }

    # Program Files needs elevation for every write and is the first place an updater walks over;
    # spaces are fine for the core (its conf values are quoted and boost::process passes the exe
    # path directly) but every generated .cmd needs quoting, so both are accepted only when -Root
    # was given on purpose.
    if ($script:Root -match '(?i)\\program files') {
        if (-not $script:RootGiven) {
            Die ("refusing to install into {0}: pass -Root explicitly if that is really where it should live" -f $script:Root)
        }
        Warn "installing into $script:Root, inside Program Files"
    }
    if ($script:Root.Contains(' ')) {
        if (-not $script:RootGiven) {
            Die ("refusing the default root {0}: it contains a space - pass -Root explicitly to confirm" -f $script:Root)
        }
        Log "root contains a space: the generated .cmd files quote every path that uses it"
    } else {
        Log 'root has no spaces: the generated .cmd files still quote their paths'
    }
    Log ("powershell: {0}" -f $PSVersionTable.PSVersion.ToString())
}

function Expand-ZipFile {
    <# Expand-ZipFile ZIP DESTINATION [-Overwrite] - the fastest extraction 5.1 can do. #>
    param(
        [string]$Zip,
        [string]$Destination,
        [switch]$Overwrite
    )
    [void][System.IO.Directory]::CreateDirectory($Destination)
    try {
        Add-Type -AssemblyName System.IO.Compression.FileSystem -ErrorAction Stop
        # ZipFile is the library Expand-Archive wraps; the cmdlet pushes every entry through the
        # pipeline, which is minutes on the 200k-file client-data archive.
        if ($Overwrite) {
            [System.IO.Compression.ZipFile]::ExtractToDirectory($Zip, $Destination, $true)
        } else {
            [System.IO.Compression.ZipFile]::ExtractToDirectory($Zip, $Destination)
        }
        return
    } catch {
        # No overwrite overload before .NET 4.7.2, and a zip with an entry that collides throws
        # here too; the cmdlet is the documented path and reports the same failure with a path.
        Warn "extracting $Zip failed ($($_.Exception.Message)); retrying with Expand-Archive"
    }
    if ($Overwrite) {
        Expand-Archive -LiteralPath $Zip -DestinationPath $Destination -Force
    } else {
        Expand-Archive -LiteralPath $Zip -DestinationPath $Destination
    }
}

function Install-ServerFiles {
    Step 'Server files'
    $worldserver = Join-Path $script:Root 'worldserver.exe'
    if ((Test-Path -LiteralPath $worldserver) -and -not $script:Force) {
        # Same rule as fetch_server: a re-run of the installer must not replace a build that works
        # with whatever the release tag points at today.
        Log "already installed in $script:Root - the installed build is left alone (pass -Force to replace it)"
        return
    }
    if ($script:DryRun) {
        Note "download $script:ReleaseUrl"
        Note "verify the .sha256 beside it when the release publishes one"
        Note "unpack it into $script:Root"
        Note 'require worldserver.exe, configs\worldserver.conf.dist and configs\modules\playerbots.conf.dist'
        return
    }
    $zip = Join-Path $env:TEMP 'acore-server-win64.zip'
    Log "downloading $script:ReleaseUrl"
    if (-not (Get-RemoteFile -Uri $script:ReleaseUrl -Destination $zip)) {
        Die "download failed: $script:ReleaseUrl (run the server-win64 workflow once to publish the release, or pass -ReleaseUrl)"
    }
    $shaFile = $zip + '.sha256'
    $want = ''
    if (Get-RemoteFile -Uri ($script:ReleaseUrl + '.sha256') -Destination $shaFile -Attempts 1) {
        $want = ([System.IO.File]::ReadAllText($shaFile)).Trim()
        if ($want) { $want = ($want -split '\s+')[0].ToLowerInvariant() }
    }
    if (-not $want -and $script:ArtifactSha) {
        # No published hash. The hash this install recorded is the stronger pin anyway: it travels
        # with the install, not with whatever the release URL points at today.
        $want = $script:ArtifactSha.ToLowerInvariant()
        Log 'no .sha256 beside the artifact; verifying against the hash recorded at install time'
    }
    if (-not $want) {
        Remove-Item -LiteralPath $zip, $shaFile -Force -ErrorAction SilentlyContinue
        Die ("no .sha256 beside {0} and no recorded artifact hash: refusing to unpack an unverified artifact" -f $script:ReleaseUrl)
    }
    $got = Get-Sha256 $zip
    if ($want -ne $got) {
        Remove-Item -LiteralPath $zip, $shaFile -Force -ErrorAction SilentlyContinue
        Die "checksum mismatch: expected $want, got $got"
    }
    $script:ArtifactSha = $got
    Log "checksum ok: $got"
    [void][System.IO.Directory]::CreateDirectory($script:Root)
    Expand-ZipFile -Zip $zip -Destination $script:Root -Overwrite
    Remove-Item -LiteralPath $zip, $shaFile -Force -ErrorAction SilentlyContinue

    $missing = @()
    $required = @(
        (Join-Path $script:Root 'worldserver.exe')
        (Join-Path $script:ConfDir 'worldserver.conf.dist')
        (Join-Path $script:ConfDir 'modules\playerbots.conf.dist')
    )
    foreach ($path in $required) {
        if (-not (Test-Path -LiteralPath $path)) { $missing += $path }
    }
    if ($missing.Count -gt 0) {
        Die ("the artifact is incomplete, missing: {0}" -f ($missing -join ', '))
    }
    Log "unpacked into $script:Root"
}

function Test-Layout {
    Step 'Layout'
    $script:BinDir = $script:Root
    $script:ConfDir = Join-Path $script:Root 'configs'
    $script:DataDir = Join-Path $script:Root 'data'
    $script:SrcDir = Join-Path $script:Root 'src'
    $script:DbDir = Join-Path $script:Root 'db'
    $script:MysqlDir = Join-Path $script:Root 'mysql'
    $script:LogsDir = Join-Path $script:Root 'logs'
    $script:StateDir = Join-Path $script:Root '.win-state'
    $script:StateFile = Join-Path $script:StateDir 'state.env'
    if ($script:DryRun) {
        Note "look for worldserver.exe and authserver.exe in $script:BinDir"
        Note "require $script:ConfDir\{worldserver,authserver}.conf.dist"
        return
    }
    foreach ($name in @('worldserver', 'authserver')) {
        $exe = Join-Path $script:BinDir ($name + '.exe')
        if (-not (Test-Path -LiteralPath $exe)) {
            Die "missing $exe (is the artifact complete?)"
        }
        $dist = Join-Path $script:ConfDir ($name + '.conf.dist')
        if (-not (Test-Path -LiteralPath $dist)) {
            Die "missing $dist (is the artifact complete?)"
        }
    }
    Log "binaries    : $script:BinDir (worldserver.exe, authserver.exe and their DLLs)"
    Log "configs     : $script:ConfDir (and $script:ConfDir\modules)"
    Log "client data : $script:DataDir"
    Log "sql sources : $script:SrcDir"
    Log "database    : $script:DbDir (mysqld from $script:MysqlDir)"
}

function Install-ClientData {
    Step 'Client data (dbc/maps/vmaps/mmaps)'
    if ((Test-Path -LiteralPath (Join-Path $script:DataDir 'dbc')) -and
        (Test-Path -LiteralPath (Join-Path $script:DataDir 'maps'))) {
        Log "already present: $script:DataDir"
        return
    }
    if ($script:DataZip -eq 'none') {
        Warn "no client data requested; worldserver needs DataDir pointing at a directory with dbc\ and maps\"
        return
    }
    if ($script:DryRun) {
        Note "reuse $script:DataDir, or fetch $script:DataZip if dbc\ and maps\ are absent"
        return
    }
    $zip = Join-Path $env:TEMP 'acore-client-data.zip'
    if ($script:DataZip -match '^(https?|ftp)://') {
        Log "downloading $script:DataZip"
        if (-not (Get-RemoteFile -Uri $script:DataZip -Destination $zip)) {
            Die "client data download failed: $script:DataZip"
        }
        $got = Get-Sha256 $zip
        if ($got -ne $DataSha256) {
            Remove-Item -LiteralPath $zip -Force -ErrorAction SilentlyContinue
            Die "client data checksum mismatch: expected $DataSha256, got $got"
        }
        Log 'checksum ok'
    } else {
        if (-not (Test-Path -LiteralPath $script:DataZip)) {
            Die "no such file: $script:DataZip"
        }
        Log "using the local archive $script:DataZip"
        Copy-Item -LiteralPath $script:DataZip -Destination $zip -Force
    }
    [void][System.IO.Directory]::CreateDirectory($script:DataDir)
    Expand-ZipFile -Zip $zip -Destination $script:DataDir -Overwrite
    Remove-Item -LiteralPath $zip -Force -ErrorAction SilentlyContinue
    if (-not (Test-Path -LiteralPath (Join-Path $script:DataDir 'dbc')) -or
        -not (Test-Path -LiteralPath (Join-Path $script:DataDir 'maps'))) {
        Die "the unzipped client data has no dbc\ and maps\ at the top level"
    }
    Log "installed into $script:DataDir"
}

function Install-MySql {
    Step ("Portable MySQL {0}" -f $MysqlVersion)
    $mysqld = Join-Path (Join-Path $script:MysqlDir 'bin') 'mysqld.exe'
    if (Test-Path -LiteralPath $mysqld) {
        Log "already present: $script:MysqlDir"
    } else {
        if ($script:DryRun) {
            Note "download $script:MysqlUrl"
            Note "unpack it into $script:MysqlDir, dropping the versioned top directory"
            return
        }
        $zip = Join-Path $env:TEMP 'mysql-winx64.zip'
        Log "downloading $script:MysqlUrl"
        if (-not (Get-RemoteFile -Uri $script:MysqlUrl -Destination $zip)) {
            Die "download failed: $script:MysqlUrl"
        }
        $stage = Join-Path $env:TEMP ('acore-mysql-' + [Guid]::NewGuid().ToString('N'))
        Expand-ZipFile -Zip $zip -Destination $stage
        # The zip carries one directory named after the version (mysql-8.4.9-winx64); the layout
        # this install promises is <root>\mysql\bin\mysqld.exe, so that directory is stripped here
        # rather than left for every later path lookup to guess.
        $source = $stage
        $children = @(Get-ChildItem -LiteralPath $stage -Directory)
        if ($children.Count -eq 1 -and (Test-Path -LiteralPath (Join-Path $children[0].FullName 'bin'))) {
            $source = $children[0].FullName
        }
        [void][System.IO.Directory]::CreateDirectory($script:MysqlDir)
        Copy-Item -Path (Join-Path $source '*') -Destination $script:MysqlDir -Recurse -Force
        Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $zip -Force -ErrorAction SilentlyContinue
        if (-not (Test-Path -LiteralPath $mysqld)) {
            Die "the MySQL archive has no bin\mysqld.exe (expected it under $MysqlDir)"
        }
        Log "unpacked into $script:MysqlDir"
    }
    if ($script:DryRun) {
        Note "run mysqld --initialize-insecure with --datadir=$script:DbDir when that directory is empty"
        return
    }
    [void][System.IO.Directory]::CreateDirectory($script:LogsDir)
    $entries = @()
    if (Test-Path -LiteralPath $script:DbDir) {
        $entries = @(Get-ChildItem -LiteralPath $script:DbDir -Force)
    }
    if ($entries.Count -gt 0) {
        Log "data directory already initialised: $script:DbDir"
    } else {
        [void][System.IO.Directory]::CreateDirectory($script:DbDir)
        Log "initialising $script:DbDir (root gets no password: this server only listens on 127.0.0.1)"
        $initArgs = @(
            '--initialize-insecure'
            ('--basedir="{0}"' -f $script:MysqlDir)
            ('--datadir="{0}"' -f $script:DbDir)
        )
        $proc = Start-Process -FilePath $mysqld -ArgumentList $initArgs -WorkingDirectory $script:MysqlDir `
            -WindowStyle Hidden -PassThru -Wait `
            -RedirectStandardOutput (Join-Path $script:LogsDir 'mysql-init.out') `
            -RedirectStandardError (Join-Path $script:LogsDir 'mysql.err')
        if ($proc.ExitCode -ne 0) {
            $tail = Get-FileTailText -Path (Join-Path $script:LogsDir 'mysql.err') -Bytes 8192
            [Console]::Error.WriteLine($tail)
            Die ("mysqld --initialize-insecure exited with {0}; the tail of logs\mysql.err is above" -f $proc.ExitCode)
        }
        Log 'data directory initialised'
    }
    Start-Db
    Resolve-AdminHost
    Log "mysqld answering on 127.0.0.1:$DbPort"
}

function New-Schema {
    Step 'Databases and user'
    if ($script:DryRun) {
        Log ("would create acore_auth, acore_world, acore_characters and the MySQL user '{0}'" -f $script:DbUser)
        Log '(acore_playerbots is created by the module itself, which also applies its base SQL)'
        return
    }
    # The same statements create_schema runs on the Thor. The user is created for 127.0.0.1 only:
    # the server connects over TCP, and an account for '%' would be reachable from the LAN the
    # moment -LanIp opens the firewall.
    $sql = @"
CREATE DATABASE IF NOT EXISTS acore_auth;
CREATE DATABASE IF NOT EXISTS acore_world;
CREATE DATABASE IF NOT EXISTS acore_characters;
-- acore_playerbots is NOT created here: the module creates its own database and then applies its
-- base SQL, and a database that already exists but is empty skips that base set - the world then
-- aborts at PrepareStatements on the module's missing tables. The grant below is what it needs,
-- and MySQL accepts it for a database that does not exist yet.
CREATE USER IF NOT EXISTS '$($script:DbUser)'@'127.0.0.1' IDENTIFIED BY '$($script:DbPass)';
ALTER USER '$($script:DbUser)'@'127.0.0.1' IDENTIFIED BY '$($script:DbPass)';
GRANT ALL PRIVILEGES ON acore_auth.*       TO '$($script:DbUser)'@'127.0.0.1';
GRANT ALL PRIVILEGES ON acore_world.*      TO '$($script:DbUser)'@'127.0.0.1';
GRANT ALL PRIVILEGES ON acore_characters.* TO '$($script:DbUser)'@'127.0.0.1';
GRANT ALL PRIVILEGES ON acore_playerbots.* TO '$($script:DbUser)'@'127.0.0.1';
FLUSH PRIVILEGES;
"@
    $exe = Get-MysqlPath 'mysql.exe'
    $argvList = @(Get-MysqlHostArgs)
    # -InputText, not a pipeline into '&': the client's error text (a rejected statement) arrives on
    # stderr, which $ErrorActionPreference = 'Stop' would turn into a terminating error before the
    # exit code below could report it.
    $null = Invoke-Native -FilePath $exe -Arguments $argvList -InputText $sql -StdOutOnly
    if ($LASTEXITCODE -ne 0) {
        $tail = Get-FileTailText -Path (Join-Path $script:LogsDir 'mysql.err') -Bytes 8192
        [Console]::Error.WriteLine($tail)
        Die "mysql rejected the schema setup above; the tail of logs\mysql.err follows it"
    }
    $schemas = Invoke-Sql "SELECT SCHEMA_NAME FROM information_schema.SCHEMATA WHERE SCHEMA_NAME LIKE 'acore%'"
    if ($null -eq $schemas) {
        if (-not (Test-Db)) { Warn "mysqld is not answering on 127.0.0.1:$DbPort any more" }
        Die 'the databases were created but the check query failed (see logs\mysql.err)'
    }
    Log ("databases: {0}" -f (($schemas -join ' ').Trim()))
}

function Write-Configs {
    Step 'Configuration'
    if ($script:DryRun) {
        Log "would write $script:ConfDir\{authserver,worldserver}.conf"
        Log "would write $script:ConfDir\modules\{playerbots,mod-creature-gear,mod_hardcore,mod_e_interact,mod-survivor,mod-mercenaries,mod-skateboard}.conf"
        Log "would create $script:LogsDir"
        return
    }
    [void][System.IO.Directory]::CreateDirectory($script:LogsDir)

    $bindIp = '127.0.0.1'
    if ($script:LanIp) { $bindIp = $script:LanIp }
    $mysqlExec = Get-MysqlPath 'mysql.exe'
    $loginInfo = '{0};{1};{2};{3};acore_auth' -f '127.0.0.1', $DbPort, $script:DbUser, $script:DbPass
    $worldInfo = '{0};{1};{2};{3};acore_world' -f '127.0.0.1', $DbPort, $script:DbUser, $script:DbPass
    $charInfo = '{0};{1};{2};{3};acore_characters' -f '127.0.0.1', $DbPort, $script:DbUser, $script:DbPass
    $botsInfo = '{0};{1};{2};{3};acore_playerbots' -f '127.0.0.1', $DbPort, $script:DbUser, $script:DbPass

    $ws = Get-ConfPair 'worldserver'
    $as = Get-ConfPair 'authserver'
    # Assigned below only when the artifact carries the module; declared here because the conf ACL
    # pass at the end of this function reads it under Set-StrictMode.
    $pb = $null
    # DataDir and friends are quoted: the value may contain a space, and the core strips the
    # quotes and nothing else. The database values use the host;port;user;pass;db form, quoted,
    # exactly as the Thor writes them.
    Set-Conf -File $ws -Key 'DataDir' -Value ('"{0}"' -f $script:DataDir)
    Set-Conf -File $ws -Key 'LogsDir' -Value ('"{0}"' -f $script:LogsDir)
    Set-Conf -File $ws -Key 'SourceDirectory' -Value ('"{0}"' -f $script:SrcDir)
    # The core shells out to this exact path for every SQL file its updater applies, so it points
    # at the bundled client and never at whatever mysql.exe happens to be on PATH.
    Set-Conf -File $ws -Key 'MySQLExecutable' -Value ('"{0}"' -f $mysqlExec)
    Set-Conf -File $ws -Key 'BindIP' -Value ('"{0}"' -f $bindIp)
    Set-Conf -File $ws -Key 'WorldServerPort' -Value $WorldPort
    # Console.Enable = 0 in the live config, and it has to be: the only reader of stdin is the CLI
    # thread, and Main.cpp starts it only when this is 1. With 1 and no console attached - a
    # start.ps1 whose stdin is at EOF, a scheduled task, a service - CliRunnable reads EOF and
    # treats it as a shutdown request, so the world stops the moment it comes up. The one place
    # that needs the console is the first boot, and it runs with a COPY of this file that has 1
    # (see Invoke-FirstBoot), so "account create ..." is read and "server shutdown 1" is honoured.
    Set-Conf -File $ws -Key 'Console.Enable' -Value 0
    Set-Conf -File $ws -Key 'Updates.EnableDatabases' -Value 7
    Set-Conf -File $ws -Key 'Updates.AutoSetup' -Value 1
    Set-Conf -File $ws -Key 'PlayerLimit' -Value 1000
    # Travel is dead time on a handheld. 2 = instant flight, but each flight master keeps a toggle
    # so the player can still fly normally; 1 = every taxi node known from login; 50.0 = the top of
    # the core's flight-speed clamp (values above 50 are rejected and fall back to 32).
    Set-Conf -File $ws -Key 'InstantFlightPaths' -Value 2
    Set-Conf -File $ws -Key 'AllFlightPaths' -Value 1
    Set-Conf -File $ws -Key 'TaxiFlightSpeed' -Value '50.0'
    Set-Conf -File $ws -Key 'LoginDatabaseInfo' -Value ('"{0}"' -f $loginInfo)
    Set-Conf -File $ws -Key 'WorldDatabaseInfo' -Value ('"{0}"' -f $worldInfo)
    Set-Conf -File $ws -Key 'CharacterDatabaseInfo' -Value ('"{0}"' -f $charInfo)
    Log ("worldserver.conf: InstantFlightPaths={0} AllFlightPaths={1} TaxiFlightSpeed={2}" -f `
        (Get-Conf $ws 'InstantFlightPaths'), (Get-Conf $ws 'AllFlightPaths'), (Get-Conf $ws 'TaxiFlightSpeed'))

    Set-Conf -File $as -Key 'LogsDir' -Value ('"{0}"' -f $script:LogsDir)
    Set-Conf -File $as -Key 'SourceDirectory' -Value ('"{0}"' -f $script:SrcDir)
    Set-Conf -File $as -Key 'MySQLExecutable' -Value ('"{0}"' -f $mysqlExec)
    Set-Conf -File $as -Key 'BindIP' -Value ('"{0}"' -f $bindIp)
    Set-Conf -File $as -Key 'RealmServerPort' -Value $AuthPort
    Set-Conf -File $as -Key 'Updates.EnableDatabases' -Value 1
    Set-Conf -File $as -Key 'Updates.AutoSetup' -Value 1
    Set-Conf -File $as -Key 'LoginDatabaseInfo' -Value ('"{0}"' -f $loginInfo)

    $moduleConf = Join-Path $script:ConfDir 'modules'
    if ((Test-Path -LiteralPath (Join-Path $moduleConf 'playerbots.conf.dist')) -or
        (Test-Path -LiteralPath (Join-Path $moduleConf 'playerbots.conf'))) {
        $pb = Get-ConfPair 'modules\playerbots'
        Set-Conf -File $pb -Key 'Playerbots.Enabled' -Value 1
        Set-Conf -File $pb -Key 'Playerbots.RandomBotAutologin' -Value 1
        Set-Conf -File $pb -Key 'Playerbots.MinRandomBots' -Value $script:Bots
        Set-Conf -File $pb -Key 'Playerbots.MaxRandomBots' -Value $script:Bots
        # Alliance is the remainder, so the pair always sums to 100: the same rule the Thor's
        # set-ratio.sh applies, and the one the container's write-confs.sh applies.
        Set-Conf -File $pb -Key 'Playerbots.RandomBotHordeRatio' -Value $script:HordeRatio
        Set-Conf -File $pb -Key 'Playerbots.RandomBotAllianceRatio' -Value (100 - $script:HordeRatio)
        # Solo-friendly default: keep the bot population level-appropriate for, and inside the zone
        # of, real players instead of spreading it over the whole world. The keys are ignored by an
        # unpatched playerbots build, so an older artifact still starts normally.
        Set-Conf -File $pb -Key 'Playerbots.RandomBotNearPlayers' -Value 1
        Set-Conf -File $pb -Key 'Playerbots.RandomBotNearPlayersLevelRange' -Value 3
        Set-Conf -File $pb -Key 'Playerbots.RandomBotNearPlayersFollowInterval' -Value 120
        Set-Conf -File $pb -Key 'Playerbots.RandomBotNearPlayersMinDistance' -Value $NearMinDistance
        Set-Conf -File $pb -Key 'Playerbots.RandomBotNearPlayersSameFaction' -Value 1
        Set-Conf -File $pb -Key 'Playerbots.RandomBotNearPlayersMaxPerPlayer' -Value $NearBots
        Set-Conf -File $pb -Key 'Playerbots.RandomBotNearPlayersZoneTraffic' -Value 1
        Set-Conf -File $pb -Key 'Playerbots.RandomBotNearPlayersZoneVisitMinTime' -Value $NearVisitMin
        Set-Conf -File $pb -Key 'Playerbots.RandomBotNearPlayersZoneVisitMaxTime' -Value $NearVisitMax
        Set-Conf -File $pb -Key 'Playerbots.RandomBotNearPlayersZoneAwayMinTime' -Value $NearAwayMin
        Set-Conf -File $pb -Key 'Playerbots.RandomBotNearPlayersZoneAwayMaxTime' -Value $NearAwayMax
        # The twin-ring keys are gone: bots stay in the player's zone instead of being placed in
        # rings around the player. Left in an older config they would look like they still do
        # something, so they are dropped here.
        Remove-ConfMatch -File $pb -KeyPattern '^Playerbots\.RandomBotNearPlayers(Radius|InnerCount|OuterRadius|ZoneCount)$'
        # Mercenary system (mod-mercenaries). The innkeeper picker is the only doorway to a pool
        # mercenary, so addclass stays a GM command; the pool is widened to fifteen accounts
        # because the picker shows a random 10-15 per visit; and mercenaries pass on every roll and
        # leave free-for-all corpses alone, so the loot-bag economy stays the player's.
        Set-Conf -File $pb -Key 'Playerbots.AddClassCommand' -Value 0
        Set-Conf -File $pb -Key 'Playerbots.AddClassAccountPoolSize' -Value 15
        Set-Conf -File $pb -Key 'Playerbots.LootNeedRollLevel' -Value 0
        Set-Conf -File $pb -Key 'Playerbots.LootGreedRollLevel' -Value 0
        Set-Conf -File $pb -Key 'Playerbots.FreeMethodLoot' -Value 0
        Set-Conf -File $pb -Key 'Playerbots.KeepAltsInGroup' -Value 1
        Set-Conf -File $pb -Key 'Playerbots.SummonWhenGroup' -Value 1
        Set-Conf -File $pb -Key 'PlayerbotsDatabaseInfo' -Value ('"{0}"' -f $botsInfo)
        Set-Conf -File $pb -Key 'Playerbots.Updates.EnableDatabases' -Value 1
        Log ("playerbots.conf: enabled={0} bots={1}" -f (Get-Conf $pb 'Playerbots.Enabled'), (Get-Conf $pb 'Playerbots.MaxRandomBots'))
    } else {
        Warn "no $moduleConf\playerbots.conf.dist - the artifact was built without the module, bots will not load"
    }

    if ((Test-Path -LiteralPath (Join-Path $moduleConf 'mod-creature-gear.conf.dist')) -or
        (Test-Path -LiteralPath (Join-Path $moduleConf 'mod-creature-gear.conf'))) {
        $cg = Get-ConfPair 'modules\mod-creature-gear'
        # The shipped .dist already holds the intended values, so nothing is rewritten unless a key
        # drifted; these four are what makes all three halves of the module active.
        $cgKeys = @('CreatureGear.Enable', 'CreatureGear.Drop.Enable', 'CreatureGear.Appearance.Enable', 'CreatureGear.Weapons.RerollOnRespawn')
        foreach ($key in $cgKeys) {
            $value = Get-Conf $cg $key
            if ($value -ne '1') {
                Warn "mod-creature-gear.conf: $key is '$value', restoring the shipped default 1"
                Set-Conf -File $cg -Key $key -Value 1
            }
        }
        Log ("mod-creature-gear.conf: Enable={0} Drop.Enable={1} Appearance.Enable={2} Weapons.RerollOnRespawn={3}" -f `
            (Get-Conf $cg 'CreatureGear.Enable'), (Get-Conf $cg 'CreatureGear.Drop.Enable'), `
            (Get-Conf $cg 'CreatureGear.Appearance.Enable'), (Get-Conf $cg 'CreatureGear.Weapons.RerollOnRespawn'))
    } else {
        Warn "no $moduleConf\mod-creature-gear.conf.dist - the artifact was built without the module, creatures keep their stock gear"
    }

    if ((Test-Path -LiteralPath (Join-Path $moduleConf 'mod_e_interact.conf.dist')) -or
        (Test-Path -LiteralPath (Join-Path $moduleConf 'mod_e_interact.conf'))) {
        $ei = Get-ConfPair 'modules\mod_e_interact'
        # One key for the whole interact loop (loot, gather, talk, chests and nodes). Every option
        # defaults to on in the shipped .dist; a drifted key is put back, since a silent 0 here
        # looks exactly like a broken key in game. EInteract.Roll is deliberately NOT in this list:
        # it is an opt-out the player may have taken on purpose.
        foreach ($key in @('EInteract.Enable', 'EInteract.Loot', 'EInteract.Gather', 'EInteract.Talk', 'EInteract.Use')) {
            $value = Get-Conf $ei $key
            if ($value -ne '1') {
                Warn "mod_e_interact.conf: $key is '$value', restoring the shipped default 1"
                Set-Conf -File $ei -Key $key -Value 1
            }
        }
        Log ("mod_e_interact.conf: Enable={0} Loot={1} Gather={2} Talk={3} Use={4}" -f `
            (Get-Conf $ei 'EInteract.Enable'), (Get-Conf $ei 'EInteract.Loot'), (Get-Conf $ei 'EInteract.Gather'), `
            (Get-Conf $ei 'EInteract.Talk'), (Get-Conf $ei 'EInteract.Use'))
    } else {
        Warn "no $moduleConf\mod_e_interact.conf.dist - the artifact was built without the module, the interact key will not work"
    }

    if ((Test-Path -LiteralPath (Join-Path $moduleConf 'mod_hardcore.conf.dist')) -or
        (Test-Path -LiteralPath (Join-Path $moduleConf 'mod_hardcore.conf'))) {
        $hc = Get-ConfPair 'modules\mod_hardcore'
        $policy = Get-Conf $hc 'Hardcore.DeathPolicy'
        if ($policy -ne 'ghost' -and $policy -ne 'archive' -and $policy -ne 'delete') {
            # The module itself falls back to ghost; make the file say so too.
            if ($policy -ne '') {
                Warn "mod_hardcore.conf: unknown DeathPolicy '$policy', restoring the shipped default ghost"
            }
            Set-Conf -File $hc -Key 'Hardcore.DeathPolicy' -Value 'ghost'
            $policy = 'ghost'
        }
        Log ("mod_hardcore.conf: Enable={0} DeathPolicy={1}" -f (Get-Conf $hc 'Hardcore.Enable'), $policy)
    } else {
        Warn "no $moduleConf\mod_hardcore.conf.dist - the artifact was built without the module, hardcore is unavailable"
    }

    if ((Test-Path -LiteralPath (Join-Path $moduleConf 'mod-survivor.conf.dist')) -or
        (Test-Path -LiteralPath (Join-Path $moduleConf 'mod-survivor.conf'))) {
        $sv = Get-ConfPair 'modules\mod-survivor'
        # The shipped .dist already holds these values; each one is still read back, because a key
        # the module cannot use is silently replaced by the module's own default and a container on
        # the wrong gameobject entry simply never appears. Only a value outside its range is
        # rewritten - a deliberate 0 (ruleset off, no map marker, no bot re-roll) is left alone.
        Set-ConfDefaults -File $sv -Check { param($k, $v) Test-SurvivorValueOk -Key $k -Value $v } `
            -Label 'mod-survivor.conf' -Defaults $SurvivorConfDefaults
        Log ("mod-survivor.conf: Enable={0} MinLevelToFlag={1} DropEquipped={2} DropBags={3} ApplyToBots={4}" -f `
            (Get-Conf $sv 'Survivor.Enable'), (Get-Conf $sv 'Survivor.MinLevelToFlag'), (Get-Conf $sv 'Survivor.DropEquipped'), `
            (Get-Conf $sv 'Survivor.DropBags'), (Get-Conf $sv 'Survivor.ApplyToBots'))
    } else {
        Warn "no $moduleConf\mod-survivor.conf.dist - the artifact was built without the module, survivor mode is unavailable"
    }

    if ((Test-Path -LiteralPath (Join-Path $moduleConf 'mod-mercenaries.conf.dist')) -or
        (Test-Path -LiteralPath (Join-Path $moduleConf 'mod-mercenaries.conf'))) {
        $pw = Get-ConfPair 'modules\mod-mercenaries'
        # The obelisk gameobject is gone, and a leftover entry would look like it still selected
        # something; the blacklist and variant blocks and their two scalars are no longer read, so
        # a conf written before the removal would keep a page of blocks that do nothing. Only the
        # live file is edited - the .dist is the shipped one.
        Remove-ConfMatch -File $pw -KeyPattern '^Mercenary\.ObeliskEntry$'
        Remove-ConfMatch -File $pw -KeyPattern '^Mercenary\.(Blacklist|Variant)\.[0-9]+\.[0-9]+$'
        Remove-ConfMatch -File $pw -KeyPattern '^Mercenary\.VariantWithhold(Min|Max)$'
        Set-ConfDefaults -File $pw -Check { param($k, $v) Test-MercenaryValueOk -Key $k -Value $v } `
            -Label 'mod-mercenaries.conf' -Defaults $MercenaryConfDefaults
        Add-MercenaryPoolKeys -Live $pw -Dist (Join-Path $moduleConf 'mod-mercenaries.conf.dist')
        # The module was mod-pawn before it was mod-mercenaries and its conf file is renamed with
        # it. The core loads every modules\*.conf, so a leftover mod-pawn.conf keeps being read
        # while nothing looks at its Pawn.* keys; it goes now that the new file is written.
        Remove-Item -LiteralPath (Join-Path $moduleConf 'mod-pawn.conf') -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath (Join-Path $moduleConf 'mod-pawn.conf.dist') -Force -ErrorAction SilentlyContinue
        Log ("mod-mercenaries.conf: Enable={0} MaxMercenaries={1} LevelWindow={2} Featured={3}-{4} Refresh={5}-{6}min Fee={7}%+{8}% Gear={9}-{10}%" -f `
            (Get-Conf $pw 'Mercenary.Enable'), (Get-Conf $pw 'Mercenary.MaxMercenaries'), (Get-Conf $pw 'Mercenary.LevelWindow'), `
            (Get-Conf $pw 'Mercenary.FeaturedMin'), (Get-Conf $pw 'Mercenary.FeaturedMax'), `
            (Get-Conf $pw 'Mercenary.PoolRefreshMinMinutes'), (Get-Conf $pw 'Mercenary.PoolRefreshMaxMinutes'), `
            (Get-Conf $pw 'Mercenary.HireFeePercent'), (Get-Conf $pw 'Mercenary.HireFeeHeldPercent'), `
            (Get-Conf $pw 'Mercenary.GearFloorPercent'), (Get-Conf $pw 'Mercenary.GearCapPercent'))
    } else {
        Warn "no $moduleConf\mod-mercenaries.conf.dist - the artifact was built without the module, the innkeeper picker is unavailable"
    }

    if ((Test-Path -LiteralPath (Join-Path $moduleConf 'mod-skateboard.conf.dist')) -or
        (Test-Path -LiteralPath (Join-Path $moduleConf 'mod-skateboard.conf'))) {
        $sb = Get-ConfPair 'modules\mod-skateboard'
        # The shipped .dist already holds these values, and Skateboard.Enable ships as 1: the board
        # is available, and nobody is on it until the player boards. Each key is still read back,
        # because a key the module cannot use is silently replaced by its own fallback and a deck
        # id the DBC does not carry falls back to the stock display. Only a value outside its range
        # is rewritten - a deliberate Enable=0 is left alone.
        # Skateboard.OffInCombat is gone: the start of a fight leaves the deck alone and a hit takes
        # it (Skateboard.OffOnHit). Nothing reads the old key, so it is deleted rather than left
        # looking live.
        Remove-ConfMatch -File $sb -KeyPattern '^Skateboard\.OffInCombat$'
        Remove-ConfMatch -File $sb -KeyPattern '^Skateboard\.Drive\.Mode$'
        # MountedPose was the mount render's seat switch, folded into Render = mount.
        Remove-ConfMatch -File $sb -KeyPattern '^Skateboard\.MountedPose$'
        # 16386 was the shipped carrier until it turned out to carry ScriptName npc_necropolis_relay,
        # which marks every summoned deck active. The module refuses a scripted row, so an upgraded
        # conf that still names it is migrated instead of boarding an invisible deck.
        if ((Get-Conf $sb 'Skateboard.Deck.Carrier') -eq '16386') {
            Warn 'mod-skateboard.conf: Skateboard.Deck.Carrier 16386 is scripted (npc_necropolis_relay), using 29443'
            Set-Conf -File $sb -Key 'Skateboard.Deck.Carrier' -Value '29443'
        }
        Set-ConfDefaults -File $sb -Check { param($k, $v) Test-SkateboardValueOk -Key $k -Value $v } `
            -Label 'mod-skateboard.conf' -Defaults $SkateboardConfDefaults
        Log ("mod-skateboard.conf: Enable={0} Deck={1} MaxSpeed={2} Crash.Enable={3}" -f `
            (Get-Conf $sb 'Skateboard.Enable'), (Get-Conf $sb 'Skateboard.Deck'), `
            (Get-Conf $sb 'Skateboard.MaxSpeed'), (Get-Conf $sb 'Skateboard.Crash.Enable'))
    } else {
        Warn "no $moduleConf\mod-skateboard.conf.dist - the artifact was built without the module, the skateboard is unavailable"
    }
    Log ("worldserver.conf: DataDir={0} BindIP={1}" -f (Get-Conf $ws 'DataDir'), (Get-Conf $ws 'BindIP'))

    # These three carry the database password in LoginDatabaseInfo / WorldDatabaseInfo, and they
    # inherit the root's ACL - on the default C:\acore that is C:\, where Users may read. Restrict
    # them the way the state file and credentials.txt already are.
    foreach ($secretConf in @($ws, $as, $pb)) {
        if ($secretConf -and (Test-Path -LiteralPath $secretConf)) { Protect-SecretFile -Path $secretConf }
    }
}

# ---------------------------------------------------------------- first boot

function Test-WorldReady {
    # Server.log is written by the file appender, which flushes every line, so it is the reliable
    # place to look; worldserver.out carries the same text through the console appender and is
    # checked too in case the log config was changed. -match is case-insensitive, so the core's
    # "WORLD: World Initialized In ..." line matches the same pattern as "ready...".
    foreach ($file in @((Join-Path $script:LogsDir 'worldserver.out'), (Join-Path $script:LogsDir 'Server.log'))) {
        $text = Get-FileTailText -Path $file -Bytes 262144
        if ($text -match 'World initialized|ready\.\.\.') { return $true }
    }
    return $false
}

function Stop-ConsoleProcess {
    <# Stop-ConsoleProcess PROC - stop a server that did not stop on its own. #>
    param($Process)
    if (-not $Process) { return }
    try {
        if ($Process.HasExited) { return }
    } catch {
        return
    }
    # taskkill /T: the worldserver runs under the cmd.exe started for its output redirection, so
    # killing cmd alone would leave the server running with nobody holding its stdin.
    $taskkill = Join-Path (Join-Path $env:SystemRoot 'System32') 'taskkill.exe'
    if (Test-Path -LiteralPath $taskkill) {
        # taskkill writes "not found"/"access denied" to stderr; through Invoke-Native that is text
        # instead of the terminating error $ErrorActionPreference = 'Stop' would make of it.
        $null = Invoke-Native -FilePath $taskkill -Arguments @('/PID', [string]$Process.Id, '/T', '/F') -StdOutOnly
        if ($LASTEXITCODE -eq 0) { return }
    }
    Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
}

function Invoke-WorldConsole {
    <# Invoke-WorldConsole -TimeoutSeconds N -Commands @('account create ...', ...) [-ConfPath P] #>
    # The Windows equivalent of world_console in thor-install.sh. Three things matter:
    #
    #  * The commands are written into a stdin pipe this process holds open. On Windows the CLI
    #    thread (CliRunnable.cpp) treats EOF on a redirected stdin as "stop the world", so a stdin
    #    file would queue the commands and then shut the world down before they ran.
    #  * That thread only exists when Console.Enable is 1 (Main.cpp starts it conditionally), and
    #    the live config has it at 0, so every caller passes a copy of the config with 1 in it -
    #    see New-ConsoleConf. With 0 the commands below would be written into a pipe nothing reads.
    #  * stdout has to land in a file, because the reply to a console command is printed to stdout
    #    and the "Account created" line is what confirms the account exists. Start-Process can
    #    redirect stdin from a file or stdout to a file, never both, so cmd.exe does the output
    #    redirection and leaves worldserver's stdin as our pipe.
    param(
        [int]$TimeoutSeconds,
        [string[]]$Commands,
        [string]$ConfPath = ''
    )
    $wsLog = Join-Path $script:LogsDir 'worldserver.out'
    $wsErr = Join-Path $script:LogsDir 'worldserver.err'
    $asLog = Join-Path $script:LogsDir 'authserver.out'
    $asErr = Join-Path $script:LogsDir 'authserver.err'
    foreach ($file in @($wsLog, $wsErr, $asLog, $asErr)) {
        Remove-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue
    }
    $asProc = Start-Process -FilePath (Join-Path $script:BinDir 'authserver.exe') `
        -ArgumentList @('-c', 'configs\authserver.conf') `
        -WorkingDirectory $script:Root -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $asLog -RedirectStandardError $asErr

    # Relative paths throughout: the working directory is the root, and every path the command
    # line mentions is one this script created there, so a root with spaces needs no quoting. The
    # one exception is $ConfPath, which is the console-enabled copy in .win-state and is therefore
    # passed as a full path, quoted - the core resolves a relative -c against the working directory,
    # so "configs\worldserver.conf" and a full path are not interchangeable.
    $confArg = 'configs\worldserver.conf'
    if ($ConfPath) { $confArg = '"' + $ConfPath + '"' }
    # The core asks on stdin before it creates a database it cannot find (DBUpdater.cpp:228-232).
    # With no console attached that question can only wait, so every worldserver this installer
    # starts is told to answer it: anything missing is created.
    $env:AC_DISABLE_INTERACTIVE = '1'
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $env:ComSpec
    if (-not $psi.FileName -or -not (Test-Path -LiteralPath $psi.FileName)) {
        $psi.FileName = Join-Path (Join-Path $env:SystemRoot 'System32') 'cmd.exe'
    }
    $psi.Arguments = '/c worldserver.exe -c ' + $confArg + ' > logs\worldserver.out 2> logs\worldserver.err'
    $psi.WorkingDirectory = $script:Root
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardInput = $true
    $wsProc = [System.Diagnostics.Process]::Start($psi)

    Log ("authserver pid {0}, worldserver pid {1}: waiting up to {2}s for the world" -f $asProc.Id, $wsProc.Id, $TimeoutSeconds)
    $waited = 0
    $why = ''
    while ($waited -lt $TimeoutSeconds) {
        if (Test-WorldReady) { break }
        # Waiting on a process that has already died is how a first boot that failed during the SQL
        # import used to burn the whole timeout looking like an import still in progress.
        if ($wsProc.HasExited) { $why = 'worldserver exited before the world came up'; break }
        Start-Sleep -Seconds 1
        $waited++
    }
    if (-not $why -and -not (Test-WorldReady)) {
        $why = ("worldserver did not report 'World initialized' within {0}s" -f $TimeoutSeconds)
    }
    if ($why) {
        Warn ("{0} - last lines of {1}:" -f $why, $wsLog)
        [Console]::Error.WriteLine((Get-FileTailText -Path $wsLog -Bytes 16384))
        $asTail = Get-FileTailText -Path $asLog -Bytes 8192
        if ($asTail) {
            Warn ("last lines of {0}:" -f $asLog)
            [Console]::Error.WriteLine($asTail)
        }
        Stop-ConsoleProcess $wsProc
        Stop-ConsoleProcess $asProc
        return $false
    }
    Log 'world is up'
    foreach ($command in $Commands) {
        try {
            $wsProc.StandardInput.WriteLine($command)
            $wsProc.StandardInput.Flush()
        } catch {
            Warn ("could not type '{0}' into the worldserver console: {1}" -f $command, $_.Exception.Message)
        }
        # Three seconds between commands: the console queues each one and the world executes it on
        # its next tick, so the next command must not overtake it.
        Start-Sleep -Seconds 3
    }
    try {
        $wsProc.StandardInput.WriteLine('server shutdown 1')
        $wsProc.StandardInput.Flush()
    } catch {
        Warn 'could not ask the worldserver to shut down'
    }
    for ($i = 0; $i -lt 60; $i++) {
        if ($wsProc.HasExited) { break }
        Start-Sleep -Seconds 1
    }
    if (-not $wsProc.HasExited) {
        Warn 'worldserver did not exit within 60s of "server shutdown 1"'
        Stop-ConsoleProcess $wsProc
    }
    Stop-ConsoleProcess $asProc
    return $true
}

function Set-RealmPin {
    if (-not $script:LanIp) { return }
    $sql = "UPDATE realmlist SET address='{0}', localAddress='{0}' WHERE id=1;" -f $script:LanIp
    $null = Invoke-Sql -Query $sql -Database 'acore_auth'
    # UPDATE prints nothing, so the row is read back rather than the exit code trusted.
    $rows = Invoke-Sql -Query ("SELECT COUNT(*) FROM realmlist WHERE id=1 AND address='{0}'" -f $script:LanIp) -Database 'acore_auth'
    if (($null -ne $rows) -and ((($rows -join '').Trim()) -eq '1')) {
        Log ("realm 1 pinned to {0}:{1}" -f $script:LanIp, $WorldPort)
    } else {
        Warn 'could not pin realm 1 (is the auth database populated?)'
    }
}

function New-ConsoleConf {
    <# New-ConsoleConf DEST - a copy of worldserver.conf with Console.Enable = 1; '' when there is none. #>
    # The live config sets Console.Enable 0 (see Write-Configs), so the world started from it never
    # reads stdin. The first boot has to type into the console, and the core resolves -c against the
    # working directory, so it gets a full-path copy with the key flipped instead of an edit to the
    # file the realm runs from afterwards.
    param([string]$Destination)
    $live = Join-Path $script:ConfDir 'worldserver.conf'
    if (-not (Test-Path -LiteralPath $live)) { return '' }
    $dir = [System.IO.Path]::GetDirectoryName($Destination)
    if ($dir) { [void][System.IO.Directory]::CreateDirectory($dir) }
    Copy-Item -LiteralPath $live -Destination $Destination -Force
    Set-Conf -File $Destination -Key 'Console.Enable' -Value 1
    return $Destination
}

function Remove-ConsoleConf {
    <# Remove-ConsoleConf PATH - drop a console-enabled copy, on the success path and on failure. #>
    param([string]$Path)
    if (-not $Path) { return }
    Remove-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
}

function Invoke-FirstBoot {
    Step 'First boot (populates the databases, then creates the GM account)'
    $bootConf = Join-Path $script:StateDir 'first-boot.conf'
    if ($script:DryRun) {
        Log ("would copy $script:ConfDir\worldserver.conf to $bootConf with Console.Enable = 1")
        Log ("would run authserver + worldserver -c $bootConf with the working directory at {0}, wait up to {1}s for 'World initialized'," -f $script:Root, $script:BootTimeout)
        Log ("then send: account create {0} *** / account set gmlevel {0} 3 -1 / server shutdown 1" -f $script:Account)
        Note ("delete $bootConf when the step ends")
        if ($script:LanIp) { Note ("point realm 1 at {0}" -f $script:LanIp) }
        return
    }
    $commands = @(
        ("account create {0} {1}" -f $script:Account, $script:AccountPass)
        ("account set gmlevel {0} 3 -1" -f $script:Account)
    )
    try {
        if (-not (New-ConsoleConf -Destination $bootConf)) {
            $script:FirstBootOk = $false
            Warn "no $script:ConfDir\worldserver.conf to copy; the first boot cannot reach the console"
            return
        }
        Log ("first boot config: {0} (Console.Enable = 1)" -f $bootConf)
        if (-not (Invoke-WorldConsole -TimeoutSeconds $script:BootTimeout -Commands $commands -ConfPath $bootConf)) {
            $script:FirstBootOk = $false
            Warn "the first boot did not finish; watch $script:LogsDir\worldserver.out or start the server and try again"
            return
        }
    } finally {
        # Removed on both paths: the copy has a bug-fixing key in it and must never be the config a
        # later start picks up by accident.
        Remove-ConsoleConf $bootConf
    }
    $console = Get-FileTailText -Path (Join-Path $script:LogsDir 'worldserver.out') -Bytes 524288
    $serverLog = Get-FileTailText -Path (Join-Path $script:LogsDir 'Server.log') -Bytes 524288
    if (($console -match 'Account created') -or ($serverLog -match 'Account created')) {
        Log ("account '{0}' created" -f $script:Account)
    } else {
        Warn 'could not confirm account creation; check the account table'
    }
    Set-RealmPin
    Log 'first boot finished'
}

# ---------------------------------------------------------------- generated helpers

function Get-FirewallRuleName {
    param([int]$Port)
    # Deterministic, so -Status and -Uninstall can find the rule again from the port alone.
    return ('AzerothCore {0} (acore-thor-selfhost)' -f $Port)
}

function Write-TextFile {
    <# Write-TextFile PATH TEXT - UTF-8 without BOM, line endings as given. #>
    param(
        [string]$Path,
        [string]$Text
    )
    [System.IO.File]::WriteAllText($Path, $Text, (New-Object -TypeName System.Text.UTF8Encoding -ArgumentList $false))
}

function Write-Helpers {
    Step 'Start/stop scripts'
    if ($script:DryRun) {
        Log ("would write {0}\{{start,stop,set-bots}}.ps1 and {{start,stop}}-db.cmd" -f $script:Root)
        Log ("would write {0}\credentials.txt" -f $script:Root)
        return
    }
    $root = $script:Root
    $nl = "`r`n"

    # Every .cmd is written with CRLF. A .cmd whose labels and gotos are separated by LF is parsed
    # as one long line by cmd.exe, which turns "goto :run" into part of the previous command.
    $startDb = @'
@echo off
rem Start the bundled MySQL if it is not already answering. Generated by windows-install.ps1.
set "MYSQLADMIN=@@ROOT@@\mysql\bin\mysqladmin.exe"
set "MYSQLD=@@ROOT@@\mysql\bin\mysqld.exe"
rem -uroot with no password: this database only listens on 127.0.0.1, see credentials.txt.
"%MYSQLADMIN%" --protocol=tcp -h127.0.0.1 -P3306 -uroot ping >nul 2>&1
if not errorlevel 1 exit /b 0
rem --console sends the error log to stderr, which is redirected into logs\mysql.err here.
start "acore-mysql" /min "%MYSQLD%" --basedir="@@ROOT@@\mysql" --datadir="@@ROOT@@\db" --port=3306 --bind-address=127.0.0.1 --console --skip-name-resolve --mysqlx=0 >> "@@ROOT@@\logs\mysql.err" 2>&1
for /l %%i in (1,1,60) do (
    "%MYSQLADMIN%" --protocol=tcp -h127.0.0.1 -P3306 -uroot ping >nul 2>&1 && exit /b 0
    timeout /t 1 /nobreak >nul
)
echo mysqld did not come up; see "@@ROOT@@\logs\mysql.err" 1>&2
exit /b 1
'@
    $stopDb = @'
@echo off
rem Stop the bundled MySQL and wait until it stops answering. Generated by windows-install.ps1.
set "MYSQLADMIN=@@ROOT@@\mysql\bin\mysqladmin.exe"
"%MYSQLADMIN%" --protocol=tcp -h127.0.0.1 -P3306 -uroot shutdown >nul 2>&1
rem mysqladmin returns as soon as the shutdown is accepted, while InnoDB is still writing out and
rem the data directory is still locked: a start that follows immediately would race it.
for /l %%i in (1,1,60) do (
    "%MYSQLADMIN%" --protocol=tcp -h127.0.0.1 -P3306 -uroot ping >nul 2>&1 || exit /b 0
    timeout /t 1 /nobreak >nul
)
echo mysqld is still running after 60s; see "@@ROOT@@\logs\mysql.err" 1>&2
exit /b 0
'@
    $startPs = @'
# Generated by windows-install.ps1: start the realm and keep it running.
#
# Ctrl-C in this window stops the world (the core's console handler runs the graceful shutdown),
# then the auth server, then the database. stop.ps1 in another window asks for the same thing
# through .win-state\stop-request; see the shutdown block below for what that can and cannot do.
#
# The world runs from configs\worldserver.conf, whose Console.Enable is 0: the CLI thread - the
# only reader of stdin - is not started, so nothing in the worldserver ever looks at this process'
# stdin. That is deliberate. A console thread with no console behind it reads EOF, and the core
# treats EOF as a shutdown request, so a realm started from a shortcut, a scheduled task or a
# hidden window would stop the moment it came up.
$ErrorActionPreference = 'Stop'
$root = '@@ROOT@@'
$logs = Join-Path $root 'logs'
$state = Join-Path $root '.win-state'
$request = Join-Path $state 'stop-request'
# Windows PowerShell 5.1 turns a native program's stderr into an error record, and 'Stop' above
# makes the first one fatal: the .cmd helpers write their failures to stderr themselves, so they
# run with 'Continue' (local to this function, the caller keeps 'Stop') and are judged by their
# exit code. 2>&1 merges the two streams so the message is still shown.
function Invoke-Child {
    param([string]$Path, [string[]]$Arguments = @())
    $argv = @()
    foreach ($item in $Arguments) { $argv += $item }
    $ErrorActionPreference = 'Continue'
    & $Path @argv 2>&1 | ForEach-Object { Write-Host ([string]$_) }
}
foreach ($dir in @($logs, $state)) {
    if (-not (Test-Path -LiteralPath $dir)) { [void][System.IO.Directory]::CreateDirectory($dir) }
}
Remove-Item -LiteralPath $request -ErrorAction SilentlyContinue
# Nothing here may wait on a question: the core asks on stdin before creating a missing database
# (DBUpdater.cpp:228-232), and this process never answers.
$env:AC_DISABLE_INTERACTIVE = '1'
Invoke-Child (Join-Path $root 'start-db.cmd')
if ($LASTEXITCODE -ne 0) { Write-Host 'the database did not start'; exit 1 }

$auth = Start-Process -FilePath (Join-Path $root 'authserver.exe') -ArgumentList @('-c', 'configs\authserver.conf') `
    -WorkingDirectory $root -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput (Join-Path $logs 'authserver.out') -RedirectStandardError (Join-Path $logs 'authserver.err')

$stop = $false
try {
    while (-not $stop) {
        # cmd.exe does the output redirection and leaves stdin alone. The child's stdin is a pipe
        # this process never writes to: it only has to exist so worldserver does not inherit a
        # stdin that is already closed (the core reads that as EOF, i.e. "stop the world"). With
        # Console.Enable 0 nothing reads it in any case.
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $env:ComSpec
        $psi.Arguments = '/c worldserver.exe -c configs\worldserver.conf > logs\worldserver.out 2> logs\worldserver.err'
        $psi.WorkingDirectory = $root
        $psi.UseShellExecute = $false
        $psi.CreateNoWindow = $true
        $psi.RedirectStandardInput = $true
        $world = [System.Diagnostics.Process]::Start($psi)
        Write-Host ("worldserver pid {0} - Ctrl-C stops the realm" -f $world.Id)
        while (-not $world.HasExited) {
            if (Test-Path -LiteralPath $request) { $stop = $true; break }
            Start-Sleep -Milliseconds 500
        }
        if ($stop) {
            Write-Host 'shutdown requested'
            # "server shutdown 1" is a console command, and Console.Enable is 0, so there is no
            # console thread left to read it: writing it into the pipe would queue it forever and
            # the wait below would burn its whole timeout before killing the tree anyway. The stop
            # request therefore ends the process tree directly; Ctrl-C in this window is the
            # graceful path (the core's signal handler runs whatever Console.Enable says).
            # /T: the worldserver runs under the cmd.exe started for its output redirection, so
            # killing cmd alone would leave the server holding the ports and the database.
            Invoke-Child "$env:SystemRoot\System32\taskkill.exe" @('/PID', $world.Id, '/T', '/F')
            [void]$world.WaitForExit(30000)
            if (-not $world.HasExited) {
                Write-Host 'worldserver did not stop in 30s; it may still be writing'
            }
        } else {
            Write-Host ("worldserver exited ({0}); restarting in 10 seconds - Ctrl-C to stop" -f $world.ExitCode)
            $deadline = (Get-Date).AddSeconds(10)
            while ((Get-Date) -lt $deadline -and -not (Test-Path -LiteralPath $request)) { Start-Sleep -Milliseconds 500 }
            if (Test-Path -LiteralPath $request) { $stop = $true }
        }
    }
} finally {
    Remove-Item -LiteralPath $request -ErrorAction SilentlyContinue
    if ($auth -and -not $auth.HasExited) { Stop-Process -Id $auth.Id -Force -ErrorAction SilentlyContinue }
    Invoke-Child (Join-Path $root 'stop-db.cmd')
}
'@
    $stopPs = @'
# Generated by windows-install.ps1: stop the realm, then the database.
#
# The stop request is a file: start.ps1 owns the worldserver process and acts on it, because this
# script cannot type into a console the live config does not read (Console.Enable is 0) and cannot
# signal another process' window. The wait below is for start.ps1 to finish what it started; only
# a worldserver that outlives it is killed here, and a forced stop loses whatever the character
# saves have not written yet, which is up to one autosave interval.
$ErrorActionPreference = 'Continue'
$root = '@@ROOT@@'
$state = Join-Path $root '.win-state'
$request = Join-Path $state 'stop-request'
if (Get-Process -Name 'worldserver' -ErrorAction SilentlyContinue) {
    if (-not (Test-Path -LiteralPath $state)) { [void][System.IO.Directory]::CreateDirectory($state) }
    Set-Content -LiteralPath $request -Value 'stop' -Encoding ASCII
    $deadline = (Get-Date).AddSeconds(150)
    while ((Get-Date) -lt $deadline -and (Get-Process -Name 'worldserver' -ErrorAction SilentlyContinue)) {
        Start-Sleep -Seconds 1
    }
}
if (Get-Process -Name 'worldserver' -ErrorAction SilentlyContinue) {
    Write-Host 'worldserver did not stop; stopping it forcibly (unsaved progress is lost)'
    Get-Process -Name 'worldserver' -ErrorAction SilentlyContinue | Stop-Process -Force
}
if (Get-Process -Name 'authserver' -ErrorAction SilentlyContinue) {
    Get-Process -Name 'authserver' -ErrorAction SilentlyContinue | Stop-Process -Force
    Write-Host 'stopped authserver'
}
& (Join-Path $root 'stop-db.cmd')
Remove-Item -LiteralPath $request -ErrorAction SilentlyContinue
Write-Host 'realm stopped'
'@
    $setBots = @'
# Generated by windows-install.ps1: read or set the random bot population and faction split.
#
# Min and Max move together: the wrapper offers one number, and a narrower window would only change
# how the population drifts after startup. The split takes one percentage, horde, with alliance as
# the remainder, so the pair always sums to 100 - the same rule the Thor's start-wow-server.sh
# --horde-ratio applies. The state file is updated with each, so -Status and -Repair agree with the
# config; the world reads the config at its next start.
param([string]$Count, [string]$HordeRatio)
$ErrorActionPreference = 'Stop'
$conf = '@@CONF@@'
$state = '@@STATE@@'
if (-not (Test-Path -LiteralPath $conf)) {
    Write-Host ("no {0}: the artifact was built without the module" -f $conf)
    exit 1
}
function Read-Key {
    param([string]$Name)
    $found = ''
    foreach ($line in [System.IO.File]::ReadAllLines($conf)) {
        if ($line -match '^\s*#') { continue }
        $eq = $line.IndexOf('=')
        if ($eq -lt 0) { continue }
        if ($line.Substring(0, $eq).Trim() -eq ('Playerbots.' + $Name)) { $found = $line.Substring($eq + 1).Trim() }
    }
    return $found
}
function Set-Key {
    param([string]$Name, [string]$Value)
    $full = 'Playerbots.' + $Name
    $lines = New-Object System.Collections.Generic.List[string]
    $done = $false
    foreach ($line in [System.IO.File]::ReadAllLines($conf)) {
        # A commented definition is replaced as well: two definitions of one key are a
        # "Duplicate key name" error in the core's parser, not a later value that wins.
        if (-not $done -and $line -match ('^\s*#?\s*' + [regex]::Escape($full) + '\s*=')) {
            $lines.Add(('{0} = {1}' -f $full, $Value))
            $done = $true
            continue
        }
        $lines.Add($line)
    }
    if (-not $done) { $lines.Add(('{0} = {1}' -f $full, $Value)) }
    [System.IO.File]::WriteAllLines($conf, $lines.ToArray())
}
function Set-State {
    param([string]$Key, [string]$Value)
    if (-not (Test-Path -LiteralPath $state)) { return }
    $stateLines = New-Object System.Collections.Generic.List[string]
    $done = $false
    foreach ($line in [System.IO.File]::ReadAllLines($state)) {
        if ($line -match ('^' + [regex]::Escape($Key) + '=')) {
            $stateLines.Add(('{0}="{1}"' -f $Key, $Value))
            $done = $true
            continue
        }
        $stateLines.Add($line)
    }
    if (-not $done) { $stateLines.Add(('{0}="{1}"' -f $Key, $Value)) }
    [System.IO.File]::WriteAllLines($state, $stateLines.ToArray())
}
function Show {
    Write-Host ("min={0} max={1}" -f (Read-Key 'MinRandomBots'), (Read-Key 'MaxRandomBots'))
    Write-Host ("alliance={0} horde={1}" -f (Read-Key 'RandomBotAllianceRatio'), (Read-Key 'RandomBotHordeRatio'))
}
if (-not $Count -and -not $HordeRatio) { Show; exit 0 }
if ($Count) {
    if ($Count -notmatch '^[0-9]+$') { Write-Host ("refusing '{0}': the bot count is a number" -f $Count); exit 2 }
    if ([int]$Count -gt 2000) { Write-Host ("refusing {0}: the range is 0-2000" -f $Count); exit 2 }
    foreach ($key in @('MinRandomBots', 'MaxRandomBots')) { Set-Key -Name $key -Value $Count }
    Set-State -Key 'BOTS' -Value $Count
}
if ($HordeRatio) {
    if ($HordeRatio -notmatch '^[0-9]+$') { Write-Host ("refusing '{0}': the horde ratio is a number" -f $HordeRatio); exit 2 }
    if ([int]$HordeRatio -gt 100) { Write-Host ("refusing {0}: the range is 0-100" -f $HordeRatio); exit 2 }
    # Written as the number, not the text: '08' would otherwise sit in the conf and in state while
    # the installer compares its own integer against it and reports the split as drifted.
    $ratio = [string]([int]$HordeRatio)
    Set-Key -Name 'RandomBotHordeRatio' -Value $ratio
    Set-Key -Name 'RandomBotAllianceRatio' -Value ([string](100 - [int]$HordeRatio))
    Set-State -Key 'HORDE_RATIO' -Value $ratio
}
Show
'@

    Write-TextFile -Path (Join-Path $root 'start-db.cmd') -Text ($startDb.Replace('@@ROOT@@', $root) -replace "`r?`n", $nl)
    Write-TextFile -Path (Join-Path $root 'stop-db.cmd') -Text ($stopDb.Replace('@@ROOT@@', $root) -replace "`r?`n", $nl)
    Write-TextFile -Path (Join-Path $root 'start.ps1') -Text ($startPs.Replace('@@ROOT@@', $root) -replace "`r?`n", $nl)
    Write-TextFile -Path (Join-Path $root 'stop.ps1') -Text ($stopPs.Replace('@@ROOT@@', $root) -replace "`r?`n", $nl)
    $setBotsPath = Join-Path (Join-Path $script:ConfDir 'modules') 'playerbots.conf'
    $setBotsText = $setBots.Replace('@@CONF@@', $setBotsPath).Replace('@@STATE@@', (Join-Path $root '.win-state\state.env'))
    Write-TextFile -Path (Join-Path $root 'set-bots.ps1') -Text ($setBotsText -replace "`r?`n", $nl)

    $stamp = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    $credentials = @(
        ("Created by windows-install.ps1 {0} on {1}" -f $ScriptVersion, $stamp)
        ''
        ("database user : {0}" -f $script:DbUser)
        ("database pass : {0}" -f $script:DbPass)
        ("gm account    : {0}" -f $script:Account)
        ("gm password   : {0}" -f $script:AccountPass)
        ("world port    : {0}   auth port: {1}" -f $WorldPort, $AuthPort)
        ''
        ("MySQL root has no password over 127.0.0.1:{0} - the server binds to loopback only, and" -f $DbPort)
        'this file is the only place the servers'' credentials are written down.'
        ("Everything this install owns lives in {0}; see {1} for the rest." -f $root, $script:ManifestFile)
    ) -join $nl
    $credentialsPath = Join-Path $root 'credentials.txt'
    Write-TextFile -Path $credentialsPath -Text ($credentials + $nl)
    Protect-SecretFile -Path $credentialsPath
    Log ("wrote {0}\start.ps1, stop.ps1, set-bots.ps1 (and the two database helpers)" -f $root)
}

function Write-HostWrappers {
    Step 'Host wrappers'
    $start = Join-Path $env:USERPROFILE 'start-wow-server.cmd'
    $stop = Join-Path $env:USERPROFILE 'stop-wow-server.cmd'
    if ($script:DryRun) {
        Note ("write {0} and {1}" -f $start, $stop)
        Note ("record them, and {0} itself, in {1}" -f $script:HostState, $script:ManifestFile)
        return
    }
    $nl = "`r`n"
    # Both wrappers take the same -Bots N: the start wrapper applies it before the world reads the
    # conf, the stop wrapper records it for the next start. Anything else is a usage error rather
    # than something silently ignored.
    $forwarder = @'
@echo off
rem Generated by windows-install.ps1 - @@PURPOSE@@
setlocal
:parse
if "%~1"=="" goto run
if /i "%~1"=="-bots" goto bots
if /i "%~1"=="-h" goto usage
if /i "%~1"=="--help" goto usage
goto usage
:bots
if "%~2"=="" goto usage
powershell -NoProfile -ExecutionPolicy Bypass -File "@@ROOT@@\set-bots.ps1" "%~2"
if errorlevel 1 exit /b 1
shift
shift
goto parse
:usage
echo usage: %~nx0 [-Bots N] 1>&2
exit /b 2
:run
powershell -NoProfile -ExecutionPolicy Bypass -File "@@ROOT@@\@@SCRIPT@@.ps1"
exit /b %errorlevel%
'@
    $startText = $forwarder.Replace('@@PURPOSE@@', 'start the realm, or change the bot count first').Replace('@@ROOT@@', $script:Root).Replace('@@SCRIPT@@', 'start')
    $stopText = $forwarder.Replace('@@PURPOSE@@', 'stop the realm, or change the bot count for the next start').Replace('@@ROOT@@', $script:Root).Replace('@@SCRIPT@@', 'stop')
    Write-TextFile -Path $start -Text (($startText -replace "`r?`n", $nl).TrimStart() + $nl)
    Write-TextFile -Path $stop -Text (($stopText -replace "`r?`n", $nl).TrimStart() + $nl)
    Add-Manifest $start
    Add-Manifest $stop
    Add-Manifest $script:HostState
    Log "wrote $start and $stop"
}

function Add-FirewallRuleForPort {
    <# Add-FirewallRuleForPort PORT - one idempotent inbound TCP rule, recorded in the manifest. #>
    param([int]$Port)
    $name = Get-FirewallRuleName -Port $Port
    # Remove-then-add: New-NetFirewallRule fails on a duplicate name, and a re-run after a changed
    # LAN address must end up with one rule, not two.
    Get-NetFirewallRule -DisplayName $name -ErrorAction SilentlyContinue | Remove-NetFirewallRule -ErrorAction SilentlyContinue
    New-NetFirewallRule -DisplayName $name -Direction Inbound -Action Allow -Protocol TCP `
        -LocalPort $Port -Profile Any -Description 'AzerothCore realm, added by windows-install.ps1' | Out-Null
    Add-Manifest ('firewall:{0}' -f $Port)
    Log ("inbound TCP {0} allowed ({1})" -f $Port, $name)
}

function Enable-Firewall {
    Step 'Firewall'
    if (-not $script:LanIp) {
        Log 'loopback only: no inbound rule is opened (pass -LanIp to listen on the LAN)'
        return
    }
    if ($script:DryRun) {
        Note ("add inbound TCP rules for {0} and {1} named 'AzerothCore <port> (acore-thor-selfhost)'" -f $AuthPort, $WorldPort)
        return
    }
    if (-not (Have 'New-NetFirewallRule')) {
        Warn 'New-NetFirewallRule is unavailable; open TCP 3724 and 8085 by hand'
        return
    }
    foreach ($port in @($AuthPort, $WorldPort)) {
        Add-FirewallRuleForPort -Port $port
    }
}

function Disable-FirewallRule {
    param([int]$Port)
    $name = Get-FirewallRuleName -Port $Port
    if (-not (Have 'Get-NetFirewallRule')) { return }
    $rule = Get-NetFirewallRule -DisplayName $name -ErrorAction SilentlyContinue
    if ($rule) {
        Remove-NetFirewallRule -DisplayName $name -ErrorAction SilentlyContinue
        Log ("removed firewall rule {0}" -f $name)
    }
}

# ---------------------------------------------------------------- summary

# ---------------------------------------------------------------- the client's Item.dbc
#
# The loot bags are custom items (91000-91063), and a 3.3.5a client draws an inventory item from its
# own Item.dbc: with no record there the bag shows the red question mark once it is in the player's
# bags, even though the loot window draws it from the packet. Two halves close that - the server's
# data\dbc\Item.dbc, so sItemStore agrees with item_template and the DBC attribute checks run, and
# the client's, shipped as a patch archive under its Data directory.
#
# The client is part of neither install: it sits wherever the player put it, outside the root and
# outside the container, which is why this phase runs on the host for both modes. Under -Docker the
# server half is reached with "docker compose cp" against the data volume.

function Get-ClientDataFile {
    <# Get-ClientDataFile - the file the chosen client folder is remembered in. #>
    return (Join-Path $script:HostState 'client-data')
}

function Read-ClientDataChoice {
    <# Read-ClientDataChoice - the remembered folder, '' when nothing has been recorded. #>
    $file = Get-ClientDataFile
    if (-not (Test-Path -LiteralPath $file)) { return '' }
    $lines = @(Get-Content -LiteralPath $file -TotalCount 1 -ErrorAction SilentlyContinue)
    if ($lines.Count -eq 0 -or -not $lines[0]) { return '' }
    return ([string]$lines[0]).Trim()
}

function Save-ClientDataChoice {
    param([string]$Value)
    if ($script:DryRun) {
        Note ("record the client folder '{0}' in {1}" -f $Value, (Get-ClientDataFile))
        return
    }
    [void][System.IO.Directory]::CreateDirectory($script:HostState)
    [System.IO.File]::WriteAllText((Get-ClientDataFile), ($Value + [Environment]::NewLine))
}

function Test-ClientDataDir {
    <# Test-ClientDataDir PATH - true when PATH is a 3.3.5a client folder. #>
    # common.MPQ is what every 3.3.5a install has and nothing else does; the loot-bag patch goes into
    # <client>\Data, and a folder without it is either the wrong game or the wrong directory.
    param([string]$Path)
    if (-not $Path) { return $false }
    if (-not (Test-Path -LiteralPath $Path)) { return $false }
    $data = Join-Path $Path 'Data'
    if (-not (Test-Path -LiteralPath $data)) { return $false }
    return (Test-Path -LiteralPath (Join-Path $data 'common.MPQ'))
}

function ConvertTo-ClientPath {
    <# ConvertTo-ClientPath TEXT - quotes stripped, absolute, no trailing separator; '' when empty. #>
    # Explorer's "Copy as path" and a folder dragged onto the prompt both hand over the folder wrapped
    # in double quotes, and [System.IO.Path]::IsPathRooted throws "Illegal characters in path" on one
    # rather than answering false. Strip first, then ask.
    param([string]$Text)
    if (-not $Text) { return '' }
    $value = $Text.Trim().Trim('"').Trim()
    if (-not $value) { return '' }
    try {
        if ([System.IO.Path]::IsPathRooted($value)) {
            $value = [System.IO.Path]::GetFullPath($value)
        }
    } catch {
        # Not a usable path at all; return it and let the caller's rooted check report it.
        return $value
    }
    if ($value.Length -gt 3) { $value = $value.TrimEnd('\', '/') }
    return $value
}

function Resolve-ClientRoot {
    <# Resolve-ClientRoot PATH - the client folder, accepting the Data directory itself too. #>
    param([string]$Path)
    if (-not $Path) { return '' }
    # Quotes stripped and made absolute, so the value recorded in %USERPROFILE%\.win-wow\client-data
    # does not depend on the directory the next -Repair is run from.
    $Path = ConvertTo-ClientPath $Path
    if (-not $Path) { return '' }
    if (Test-ClientDataDir $Path) { return $Path }
    if ((Split-Path -Leaf $Path) -ieq 'Data') {
        $parent = Split-Path -Parent $Path
        if ($parent -and (Test-Path -LiteralPath (Join-Path $Path 'common.MPQ'))) { return $parent }
    }
    return ''
}

function Get-ClientRoot {
    <# Get-ClientRoot - -ClientData, else the remembered folder, else one question. '' means skip. #>
    if ($script:ClientData) {
        if ($script:ClientData -eq 'none') { return '' }
        $root = Resolve-ClientRoot $script:ClientData
        if (-not $root) {
            Die ("-ClientData '{0}' holds no Data\common.MPQ, so it is not a 3.3.5a client folder (pass 'none' to skip the client half)" -f $script:ClientData)
        }
        Save-ClientDataChoice $root
        return $root
    }
    $remembered = Read-ClientDataChoice
    if ($remembered) {
        if ($remembered -eq 'none') { return '' }
        if (Test-ClientDataDir $remembered) { return $remembered }
        Warn ("the remembered client folder '{0}' is gone or is not a 3.3.5a client; asking again" -f $remembered)
    }
    if ($script:DryRun -or -not [Environment]::UserInteractive) { return '' }
    Log 'the loot bags are custom items, so the client needs a patch archive to draw their icons.'
    Log 'Point this at the folder that holds Data\common.MPQ (the World of Warcraft folder), or press Enter to skip.'
    $answer = [string](Read-Host 'Client folder')
    if (-not $answer) {
        Note 'skipped; pass -ClientData DIR (or -Repair) to do it later'
        return ''
    }
    if ($answer -eq 'none') { Save-ClientDataChoice 'none'; return '' }
    $root = Resolve-ClientRoot $answer
    if (-not $root) {
        Warn ("'{0}' holds no Data\common.MPQ, so the client half is left alone (pass -ClientData DIR next time)" -f $answer)
        return ''
    }
    Save-ClientDataChoice $root
    return $root
}

function Get-PythonCommand {
    <# Get-PythonCommand - the argv prefix that runs Python 3, or $null. #>
    # python.exe on Windows may be the Microsoft Store stub, which opens the Store instead of
    # running anything and exits non-zero: the version probe is what tells them apart.
    $candidates = @(
        @('python.exe'),
        @('python3.exe'),
        @('py.exe', '-3')
    )
    $seen = @()
    foreach ($candidate in $candidates) {
        $exe = $candidate[0]
        $cmd = Get-Command $exe -ErrorAction SilentlyContinue
        if (-not $cmd) { continue }
        $prefix = @()
        for ($i = 1; $i -lt $candidate.Count; $i++) { $prefix += $candidate[$i] }
        # -V, not -c with a quoted payload: Windows PowerShell 5.1 drops the embedded double quotes
        # when it passes an argument to a native program, so 'import sys; print("%d.%d" ...)' reaches
        # python as print(%d.%d ...) and fails with a SyntaxError - a working interpreter reported as
        # missing. -V is one token and prints "Python 3.12.10" on stdout.
        $argv = @($prefix) + @('-V')
        $out = [string](Invoke-Native -FilePath $cmd.Source -Arguments $argv -StdOutOnly)
        if ($LASTEXITCODE -eq 0 -and $out.Trim() -match '^Python 3\.') {
            # The comma keeps the shape: a bare `return @('C:\...\python.exe')` is enumerated by the
            # pipeline and arrives as a String, which then has no .Count under Set-StrictMode and
            # makes the caller's argv slice throw.
            return ,(@($cmd.Source) + $prefix)
        }
        # The Microsoft Store ships an app-execution alias under WindowsApps that opens the Store
        # instead of running anything, and it is what a fresh Windows resolves python.exe to.
        if ($cmd.Source -match '\\WindowsApps\\') {
            $seen += ("{0} is the Microsoft Store alias, not a Python install" -f $exe)
        } else {
            $seen += ("{0} at {1} did not answer as Python 3" -f $exe, $cmd.Source)
        }
    }
    $seen = @($seen | Select-Object -Unique)
    if ($seen.Count -gt 0) {
        $script:PythonProblem = ($seen -join '; ')
    } else {
        $script:PythonProblem = 'no python.exe, python3.exe or py.exe on PATH'
    }
    return $null
}

function Get-ItemDbcTool {
    <# Get-ItemDbcTool - fetch the two Item.dbc tools at -SelfRef; '' when they cannot be fetched. #>
    # Both, and in the repository's own layout: patch_item_dbc.py loads scripts/loot-bags/
    # gen_loot_bags.py from its parent directory (GENERATOR = HERE.parent / 'loot-bags' / ...) to get
    # the bag ids and display ids. Fetching only the patch script fails at run time with
    # "FileNotFoundError: ...\loot-bags\gen_loot_bags.py".
    $dir = Join-Path $script:HostState 'tools'
    $tool = Join-Path (Join-Path $dir 'item-dbc') 'patch_item_dbc.py'
    # Objects, not nested arrays: @(@('a','b'), @('c','d')) is flattened by PowerShell into one
    # four-element array, and then $entry[0] is a character rather than a path.
    $files = @(
        [pscustomobject]@{ Rel = 'scripts/item-dbc/patch_item_dbc.py'; Path = $tool }
        [pscustomobject]@{ Rel = 'scripts/loot-bags/gen_loot_bags.py'; Path = (Join-Path (Join-Path $dir 'loot-bags') 'gen_loot_bags.py') }
        [pscustomobject]@{ Rel = 'scripts/lib/dbc_mpq.py'; Path = (Join-Path (Join-Path $dir 'lib') 'dbc_mpq.py') }
        [pscustomobject]@{ Rel = 'scripts/skateboard/patch_deck_dbc.py'; Path = (Join-Path (Join-Path $dir 'skateboard') 'patch_deck_dbc.py') }
    )
    $missing = @($files | Where-Object { -not (Test-Path -LiteralPath $_.Path) })
    if ($script:DryRun) {
        foreach ($entry in $missing) {
            Note ("download {0}{1}/{2} to {3}" -f $DockerRepoBase, (Get-FetchRef), $entry.Rel, $entry.Path)
        }
        return $tool
    }
    # Two passes at most, like Install-DockerAssets: a recorded pin that predates scripts/lib/dbc_mpq.py
    # cannot serve it, and a 404 there used to skip the DBC patch with "could not be fetched".
    for ($attempt = 1; $attempt -le 2; $attempt++) {
        $failed = ''
        foreach ($entry in $missing) {
            [void][System.IO.Directory]::CreateDirectory((Split-Path -Parent $entry.Path))
            $uri = $DockerRepoBase + (Get-FetchRef) + '/' + $entry.Rel
            if (-not (Get-RemoteFile -Uri $uri -Destination $entry.Path)) { $failed = $entry.Rel; break }
        }
        if (-not $failed) { return $tool }

        $refreshable = ($attempt -eq 1) -and (-not $script:Bound.ContainsKey('SelfRef')) -and $script:SelfRefSha
        if (-not $refreshable) { return '' }
        Warn ("{0} is not in commit {1} - the pin this realm recorded predates it, resolving -SelfRef '{2}' again" -f `
            $failed, (Get-FetchRef), $script:SelfRef)
        Reset-SelfRefPin
        Save-State
    }
    return ''
}

function Get-ServerDbcDir {
    <# Get-ServerDbcDir - a host directory holding the server's Item.dbc, or '' when there is none. #>
    # Native: the tree the installer unpacked. Docker: the data volume, reached through the running
    # container, because the volume is not a host path on either engine.
    if ((Resolve-Mode) -ne 'docker') {
        $dir = Join-Path $script:DataDir 'dbc'
        if (Test-Path -LiteralPath (Join-Path $dir 'Item.dbc')) { return $dir }
        return ''
    }
    $dir = Join-Path $env:TEMP ('acore-itemdbc-' + [guid]::NewGuid().ToString('N'))
    [void][System.IO.Directory]::CreateDirectory($dir)
    # Item.dbc for the bag records, and the two the deck rows need: the deck tool clones the stock
    # log's rows out of CreatureModelData/CreatureDisplayInfo, so it cannot run from Item.dbc alone.
    foreach ($name in @('Item.dbc', 'CreatureModelData.dbc', 'CreatureDisplayInfo.dbc')) {
        $null = Invoke-Compose -Arguments @('cp', ('acore:/opt/acore/data/dbc/' + $name), $dir) -AllowFailure
    }
    if (-not (Test-Path -LiteralPath (Join-Path $dir 'Item.dbc'))) { return '' }
    return $dir
}

function Test-ServerDbc {
    <# Test-ServerDbc - $true when the server's data\dbc\Item.dbc exists, $false when it is missing,
       $null when the container could not be asked at all. #>
    # Copies nothing, unlike Get-ServerDbcDir: a status line should not pull a file out of the volume
    # to find out whether it is there.
    if ((Resolve-Mode) -ne 'docker') {
        return (Test-Path -LiteralPath (Join-Path (Join-Path $script:DataDir 'dbc') 'Item.dbc'))
    }
    if ($script:DryRun) { return $true }
    # The volume is not a host path, so ask the container the worldserver reads the file from. A
    # stopped container fails this the same way a missing file does - both exit 1 - so the answer is
    # read from what compose said: the file is only reported missing when the probe actually ran.
    $probe = Invoke-Compose -Arguments @('exec', '-T', 'acore', 'test', '-f', '/opt/acore/data/dbc/Item.dbc') -Capture -AllowFailure
    if ($LASTEXITCODE -eq 0) { return $true }
    if ($probe -match 'is not running|no such service|no configuration file|Cannot connect|error during connect|is not a docker command') { return $null }
    return $false
}

function Show-ClientPatchStatus {
    <# Show-ClientPatchStatus - report the loot-bag client half; shared by -Status and -Docker -Status. #>
    # Reported, not fixed: the client folder is the player's, so the only automatic fix is -Repair,
    # which runs Install-ClientPatch.
    $clientRoot = Read-ClientDataChoice
    if (-not $clientRoot) {
        Note 'client folder: none recorded (pass -ClientData DIR to write the loot-bag patch into it)'
    } elseif ($clientRoot -eq 'none') {
        Note 'client folder: skipped on purpose (the bags keep the red question mark in the bags)'
    } elseif (-not (Test-ClientDataDir $clientRoot)) {
        Bad ("client folder: {0} is gone or holds no Data\common.MPQ" -f $clientRoot)
    } else {
        $clientData = Join-Path $clientRoot 'Data'
        $patches = @(Get-ChildItem -LiteralPath $clientData -Filter 'patch-*.MPQ' -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^patch-[A-Za-z0-9]+\.MPQ$' })
        if ($patches.Count -gt 0) {
            Ok ("client folder: {0} ({1} patch archive(s))" -f $clientRoot, $patches.Count)
        } else {
            Bad ("client folder: {0} holds no patch-*.MPQ, so the bags keep the red question mark" -f $clientRoot)
        }
        # The docker path is a volume, so the host path is not where the file lives and not what to
        # report: a container realm used to FAIL here for a file it had all along.
        $serverDbc = Join-Path $script:DataDir 'dbc\Item.dbc'
        if ((Resolve-Mode) -eq 'docker') { $serverDbc = '/opt/acore/data/dbc/Item.dbc in the data volume' }
        $serverState = Test-ServerDbc
        if ($null -eq $serverState) {
            Warn ("server Item.dbc: could not ask the container about {0} (is it running?); neither confirmed nor reported missing" -f $serverDbc)
        } elseif (-not $serverState) {
            Bad ("server Item.dbc: {0} is missing (the client data is not installed)" -f $serverDbc)
        }
    }
}

function Install-ClientPatch {
    <# Install-ClientPatch - the two client-data patches: Item.dbc (bags) then the deck DBCs. #>
    # Two steps, one entry point: every caller (-Install, -Repair, -Docker and -Status's fix arm)
    # wants both or neither, and the deck rows are the second thing a fresh client is missing.
    Install-ItemDbcPatch
    Install-DeckPatch
}

function Install-ItemDbcPatch {
    Step 'Loot-bag items in Item.dbc'
    if ($script:DryRun) {
        $where = $script:DataDir + '\dbc\Item.dbc'
        if ((Resolve-Mode) -eq 'docker') { $where = '/opt/acore/data/dbc/Item.dbc in the data volume' }
        Note ("add the 64 bag records to {0}" -f $where)
        $root = Get-ClientRoot
        if ($root) { Note ("write a patch archive into {0}" -f (Join-Path $root 'Data')) }
        $py = Get-PythonCommand
        if (-not $py) { Note ("python 3 is missing ({0}), so the patch would be skipped" -f $script:PythonProblem) }
        [void](Get-ItemDbcTool)
        return
    }
    # The client folder is asked for first, and recorded, even when the server half cannot run: the
    # question is the only thing here that cannot be repeated unattended, and a realm whose container
    # failed must not also lose the answer.
    $clientRoot = Get-ClientRoot
    $clientData = ''
    if ($clientRoot) { $clientData = Join-Path $clientRoot 'Data' }

    $dbcDir = Get-ServerDbcDir
    if (-not $dbcDir) {
        # The dry run returned above, so this branch only ever runs for real.
        $where = Join-Path $script:DataDir 'dbc\Item.dbc'
        if ((Resolve-Mode) -eq 'docker') { $where = '/opt/acore/data/dbc/Item.dbc in the data volume' }
        Warn "no $where yet, so the bag records were not added (install the client data first)"
        if ($clientData) {
            Warn ("  the client folder {0} is recorded, so -Repair picks it up once the server data exists" -f $clientRoot)
        }
        return
    }

    $py = Get-PythonCommand
    $tool = Get-ItemDbcTool
    if (-not $py -or -not $tool) {
        if (-not $py) {
            Warn ("python 3 is missing ({0}), so the loot-bag Item.dbc patch is skipped" -f $script:PythonProblem)
            Warn '  install it with: winget install --id Python.Python.3.12 -e, then re-run -Repair'
        } else {
            Warn 'the Item.dbc tool could not be fetched, so the bags keep the red question mark'
        }
        return
    }
    $out = Join-Path $script:StateDir 'item-dbc'
    # Normalised only now: @($null) is a one-element array and would pass the guard above.
    $py = @($py)
    # The argv prefix is the exe plus the arguments that must precede the script: 'py.exe -3' has one,
    # a plain 'python.exe' has none, and 1..($py.Count - 1) on a single-entry list yields index 0 and
    # 1 back, so the exe would be passed to itself as its first argument.
    $pyPrefix = @()
    if ($py.Count -gt 1) { $pyPrefix = @($py[1..($py.Count - 1)]) }
    $argv = $pyPrefix + @($tool, '--dbc', (Join-Path $dbcDir 'Item.dbc'), '--server-dbc', $dbcDir, '--out', $out)
    if ($clientData) { $argv += @('--client-data', $clientData) }
    # No dry-run arm here: the function returns at the top when -DryRun, and that block already says
    # what it would write.
    Log ("running the Item.dbc patch ({0} of 64 bag records)" -f 'patch_item_dbc.py')
    $null = Invoke-Native -FilePath $py[0] -Arguments $argv
    if ($LASTEXITCODE -ne 0) {
        Warn ("the Item.dbc patch failed (exit {0}); the bags keep the red question mark until it runs" -f $LASTEXITCODE)
        return
    }
    if ((Resolve-Mode) -eq 'docker') {
        # The server half belongs to the container: entrypoint.sh runs this same tool against
        # /opt/acore/data/dbc/Item.dbc on every start, before worldserver reads it (patch_item_dbc).
        # So the copy this step took from the volume was only needed to build the client archive
        # below. Writing it back and restarting the realm, which this used to do, cost a second boot
        # of the world - and restarted the container mid-boot whenever the readiness marker had
        # survived the previous one, which is how a realm ended up with the authserver dead.
        Ok 'server Item.dbc: the container adds the 64 bag records on every start, before the world reads it'
    } else {
        Ok ("server Item.dbc: the 64 bag records are in {0}" -f (Join-Path $dbcDir 'Item.dbc'))
    }

    if (-not $clientData) {
        Warn 'no client folder was chosen, so the client half was not written: pass -ClientData DIR, or re-run -Repair to be asked'
        Warn ("  (or copy {0}\item-dbc.MPQ into the client's Data as patch-9.MPQ)" -f $out)
        return
    }
    # --verify reads the client's Data directory back in patch-load order and says which archive the
    # client will actually use, which is the only thing that decides whether the icon appears.
    $verifyArgv = $pyPrefix + @($tool, '--verify', '--client-data', $clientData)
    $null = Invoke-Native -FilePath $py[0] -Arguments $verifyArgv
    if ($LASTEXITCODE -eq 0) {
        Ok ("client Item.dbc: {0} reads the patched file" -f $clientData)
    } else {
        Bad ("client Item.dbc: {0} does not read the patched file (run the patch by hand to see why)" -f $clientData)
    }
}

function Install-DeckPatch {
    <# Install-DeckPatch - put the two shield deck rows in CreatureDisplayInfo/CreatureModelData. #>
    # Skateboard.Deck ships as `engravedwall` (display 90001), a shield item model registered as a
    # creature display - rows the stock DBCs do not have. The module warns once and draws the stock
    # log (26609) until the server's copy carries them, so this is a warning, not a failure. In
    # docker mode the container patches its own copy on every start (entrypoint.sh,
    # patch_deck_dbc), exactly like the bag records: the copy taken here is what builds the client
    # archive.
    Step 'Shield deck rows in CreatureDisplayInfo.dbc'
    if ($script:DryRun) {
        $where = $script:DataDir + '\dbc'
        if ((Resolve-Mode) -eq 'docker') { $where = '/opt/acore/data/dbc in the data volume' }
        Note ("add the two shield deck rows (90001, 90002) to {0}" -f $where)
        $root = Get-ClientRoot
        if ($root) { Note ("write a patch archive into {0}" -f (Join-Path $root 'Data')) }
        $py = Get-PythonCommand
        if (-not $py) { Note ("python 3 is missing ({0}), so the patch would be skipped" -f $script:PythonProblem) }
        [void](Get-ItemDbcTool)
        return
    }
    $clientRoot = Read-ClientDataChoice
    $clientData = ''
    if ($clientRoot -and $clientRoot -ne 'none' -and (Test-ClientDataDir $clientRoot)) {
        $clientData = Join-Path $clientRoot 'Data'
    }
    $dbcDir = Get-ServerDbcDir
    if (-not $dbcDir) {
        Warn 'no server dbc directory yet, so the shield deck rows were not added (install the client data first)'
        return
    }
    $surface = Join-Path $dbcDir 'CreatureDisplayInfo.dbc'
    $model = Join-Path $dbcDir 'CreatureModelData.dbc'
    if (-not (Test-Path -LiteralPath $surface) -or -not (Test-Path -LiteralPath $model)) {
        Warn ("no CreatureDisplayInfo.dbc/CreatureModelData.dbc beside {0}, so the shield deck rows were not added" -f (Join-Path $dbcDir 'Item.dbc'))
        return
    }
    $py = Get-PythonCommand
    $tool = Get-ItemDbcTool
    if (-not $py -or -not $tool) {
        if (-not $py) {
            Warn ("python 3 is missing ({0}), so the shield deck rows are skipped" -f $script:PythonProblem)
        } else {
            Warn 'the DBC tool could not be fetched, so the board keeps the stock log'
        }
        return
    }
    $deckTool = Join-Path (Join-Path (Join-Path $script:HostState 'tools') 'skateboard') 'patch_deck_dbc.py'
    $py = @($py)
    $pyPrefix = @()
    if ($py.Count -gt 1) { $pyPrefix = @($py[1..($py.Count - 1)]) }
    $out = Join-Path $script:StateDir 'deck-dbc'
    $argv = $pyPrefix + @($deckTool, '--dbcs', $dbcDir, '--out', $out)
    if ((Resolve-Mode) -ne 'docker') { $argv += @('--server-dbc', $dbcDir) }
    if ($clientData) { $argv += @('--client-data', $clientData) }
    Log 'running the deck DBC patch (patch_deck_dbc.py)'
    $null = Invoke-Native -FilePath $py[0] -Arguments $argv
    if ($LASTEXITCODE -ne 0) {
        Warn ("the deck DBC patch failed (exit {0}); the board keeps the stock log until it runs" -f $LASTEXITCODE)
        return
    }
    if ((Resolve-Mode) -eq 'docker') {
        Ok 'server DBCs: the container adds the two shield deck rows on every start, before the world reads them'
    } else {
        Ok ("server DBCs: the shield decks (90001, 90002) are in {0}" -f $dbcDir)
    }
    if (-not $clientData) {
        Warn 'no client folder was chosen, so the client half was not written: pass -ClientData DIR, or re-run -Repair to be asked'
        Warn ("  (or copy {0}\deck-dbc.MPQ into the client's Data as patch-9.MPQ)" -f $out)
        return
    }
    $verifyArgv = $pyPrefix + @($deckTool, '--verify', '--client-data', $clientData)
    $null = Invoke-Native -FilePath $py[0] -Arguments $verifyArgv
    if ($LASTEXITCODE -eq 0) {
        Ok ("client CreatureDisplayInfo.dbc: {0} reads the patched file" -f $clientData)
    } else {
        Bad ("client CreatureDisplayInfo.dbc: {0} does not read the patched file (run the patch by hand to see why)" -f $clientData)
    }
}

function Invoke-Summary {
    Step 'Summary'
    if ($script:DryRun) {
        Log 'check mode finished; nothing was changed'
        return
    }
    if (-not $script:FirstBootOk) {
        Warn "the first boot did not finish; run $script:Root\start.ps1 and watch $script:LogsDir\worldserver.out"
    }
    $bind = '127.0.0.1'
    if ($script:LanIp) { $bind = $script:LanIp }
    Log ("server      : {0}   (binaries {1}, configs {2})" -f $script:Root, $script:BinDir, $script:ConfDir)
    Log ("database    : {0}, port {1} on 127.0.0.1, mysqld in {2}" -f $script:DbDir, $DbPort, $script:MysqlDir)
    Log ("bots        : {0} (Playerbots.MinRandomBots / MaxRandomBots)" -f $script:Bots)
    Log ("faction     : {0}/{1} (alliance/horde, Playerbots.RandomBotAllianceRatio / RandomBotHordeRatio)" -f (100 - $script:HordeRatio), $script:HordeRatio)
    Log ("gm account  : {0} / {1}  (also in {2}\credentials.txt)" -f $script:Account, $script:AccountPass, $script:Root)
    Log ("bind        : {0}   (loopback unless -LanIp was given)" -f $bind)
    Log ''
    Log 'Manage it'
    Log '  inspect : powershell -File windows-install.ps1 -Status      (exit 1 when something needs repair)'
    Log '  repair  : powershell -File windows-install.ps1 -Repair'
    Log '  remove  : powershell -File windows-install.ps1 -Uninstall [-KeepDb]'
    Log ('  start   : {0}\start-wow-server.cmd [-Bots N]   (stop: stop-wow-server.cmd)' -f $env:USERPROFILE)
    Log ''
    Log 'Next steps'
    Log '  1. prove it with a PC 3.3.5a client before touching the launcher: start the realm with'
    Log ('     {0}\start-wow-server.cmd and log in with {1} / {2}.' -f $env:USERPROFILE, $script:Account, $script:AccountPass)
    Log '  2. launcher client half: docs/LAUNCHER-HOSTING-DESIGN.md section 3.'
}

# ---------------------------------------------------------------- checks

function Test-HelperFile {
    param([string]$Name)
    return (Test-Path -LiteralPath (Join-Path $script:Root $Name))
}

function Invoke-Checks {
    <# Invoke-Checks [-Fix] - the -Status / -Repair body; never writes unless -Fix. #>
    param([switch]$Fix)
    Step 'Components'
    if (-not $script:BinDir) { Reset-Paths }

    # 1. binaries
    $missingBinary = $false
    foreach ($name in @('worldserver', 'authserver')) {
        $exe = Join-Path $script:BinDir ($name + '.exe')
        if (Test-Path -LiteralPath $exe) {
            Ok "binary: $exe"
        } else {
            Bad "binary missing: $exe"
            $missingBinary = $true
        }
    }
    if ($missingBinary -and $Fix) {
        Warn 're-downloading the server (-Force)'
        $script:Fixes = $script:Fixes + 1
        $script:Force = $true
        Install-ServerFiles
    }

    # 2. configs
    $ws = Join-Path $script:ConfDir 'worldserver.conf'
    $as = Join-Path $script:ConfDir 'authserver.conf'
    $moduleConf = Join-Path $script:ConfDir 'modules'
    $pb = Join-Path $moduleConf 'playerbots.conf'
    $cg = Join-Path $moduleConf 'mod-creature-gear.conf'
    $hc = Join-Path $moduleConf 'mod_hardcore.conf'
    $ei = Join-Path $moduleConf 'mod_e_interact.conf'
    $sv = Join-Path $moduleConf 'mod-survivor.conf'
    $pw = Join-Path $moduleConf 'mod-mercenaries.conf'
    $sb = Join-Path $moduleConf 'mod-skateboard.conf'
    $confFiles = @($ws, $as, $pb, $cg, $hc, $ei, $sv, $pw, $sb)
    $confMissing = @()
    foreach ($file in $confFiles) {
        if (-not (Test-Path -LiteralPath $file)) { $confMissing += $file }
    }
    if ($confMissing.Count -eq 0) {
        Ok 'configs present: worldserver.conf, authserver.conf, modules\playerbots.conf, modules\mod-creature-gear.conf, modules\mod_hardcore.conf, modules\mod_e_interact.conf, modules\mod-survivor.conf, modules\mod-mercenaries.conf, modules\mod-skateboard.conf'
    } else {
        Bad ("configs incomplete, missing: {0}" -f ($confMissing -join ', '))
        if ($Fix) {
            $script:Fixes = $script:Fixes + 1
            Warn 'rewriting the configs'
            Write-Configs
        }
    }

    $emptyKeys = @()
    if (Test-Path -LiteralPath $ws) {
        foreach ($key in @('DataDir', 'LogsDir', 'SourceDirectory', 'MySQLExecutable', 'BindIP')) {
            if ((Get-ConfUnquoted -File $ws -Key $key) -eq '') { $emptyKeys += $key }
        }
        if ($emptyKeys.Count -eq 0) {
            Ok 'worldserver.conf: DataDir/LogsDir/SourceDirectory/MySQLExecutable/BindIP set'
        } else {
            Bad ("worldserver.conf: empty or missing: {0}" -f ($emptyKeys -join ', '))
            if ($Fix) {
                $script:Fixes = $script:Fixes + 1
                Write-Configs
            }
        }
        # The live value is 0 on purpose (see Write-Configs): only the first boot runs with a copy
        # that has 1. A 1 here means a start with no console attached stops the world on stdin EOF.
        $consoleKey = Get-Conf $ws 'Console.Enable'
        if ($consoleKey -eq '0') {
            Ok 'worldserver.conf: Console.Enable=0 (console off; the first boot ran a copy with 1)'
        } else {
            Bad ("worldserver.conf: Console.Enable is '{0}', expected 0 (with no console attached a world reading stdin stops on EOF); -Repair restores it" -f $consoleKey)
        }
        # Travel settings the installer writes. An older config, or one edited by hand, can hold
        # the blizzlike 0/0/32.0, which costs the player travel time on every flight.
        $ifp = Get-Conf $ws 'InstantFlightPaths'
        $afp = Get-Conf $ws 'AllFlightPaths'
        $tfs = Get-Conf $ws 'TaxiFlightSpeed'
        $travelWrong = $false
        if ($ifp -eq '2') { Ok "worldserver.conf: InstantFlightPaths=$ifp" } else { Bad "worldserver.conf: InstantFlightPaths is '$ifp', expected 2"; $travelWrong = $true }
        if ($afp -eq '1') { Ok "worldserver.conf: AllFlightPaths=$afp" } else { Bad "worldserver.conf: AllFlightPaths is '$afp', expected 1"; $travelWrong = $true }
        # The core validates TaxiFlightSpeed as 0.01 < v <= 50 (WorldConfig.cpp) and 32.0 is the
        # stock value, so only something above it can be the installer's setting.
        $speed = 0.0
        $speedOk = [double]::TryParse($tfs, [System.Globalization.NumberStyles]::Float, [System.Globalization.CultureInfo]::InvariantCulture, [ref]$speed)
        if ($speedOk -and $speed -gt 32 -and $speed -le 50) {
            Ok "worldserver.conf: TaxiFlightSpeed=$tfs"
        } else {
            Bad "worldserver.conf: TaxiFlightSpeed is '$tfs', expected a number in (32, 50]"
            $travelWrong = $true
        }
        if ($travelWrong -and $Fix) {
            $script:Fixes = $script:Fixes + 1
            Set-Conf -File $ws -Key 'InstantFlightPaths' -Value 2
            Set-Conf -File $ws -Key 'AllFlightPaths' -Value 1
            Set-Conf -File $ws -Key 'TaxiFlightSpeed' -Value '50.0'
            Ok 'worldserver.conf travel keys set to InstantFlightPaths=2 AllFlightPaths=1 TaxiFlightSpeed=50.0'
        }
    }

    if (Test-Path -LiteralPath $pb) {
        $maxBots = Get-Conf $pb 'Playerbots.MaxRandomBots'
        if ($maxBots -ne [string]$script:Bots) {
            Bad ("playerbots.conf bots is {0}, state says {1}" -f $maxBots, $script:Bots)
            if ($Fix) {
                $script:Fixes = $script:Fixes + 1
                Set-Conf -File $pb -Key 'Playerbots.MinRandomBots' -Value $script:Bots
                Set-Conf -File $pb -Key 'Playerbots.MaxRandomBots' -Value $script:Bots
                Ok ("bots set to {0}" -f $script:Bots)
            }
        } else {
            Ok ("playerbots.conf bots = {0}" -f $script:Bots)
        }
        $horde = Get-Conf $pb 'Playerbots.RandomBotHordeRatio'
        $alliance = Get-Conf $pb 'Playerbots.RandomBotAllianceRatio'
        $wantAlliance = [string](100 - $script:HordeRatio)
        if ($horde -ne [string]$script:HordeRatio -or $alliance -ne $wantAlliance) {
            Bad ("playerbots.conf faction split is {0}/{1} (alliance/horde), state says {2}/{3}" -f $alliance, $horde, $wantAlliance, $script:HordeRatio)
            if ($Fix) {
                $script:Fixes = $script:Fixes + 1
                Set-Conf -File $pb -Key 'Playerbots.RandomBotHordeRatio' -Value $script:HordeRatio
                Set-Conf -File $pb -Key 'Playerbots.RandomBotAllianceRatio' -Value $wantAlliance
                Ok ("faction split set to {0}/{1} (alliance/horde)" -f $wantAlliance, $script:HordeRatio)
            }
        } else {
            Ok ("playerbots.conf faction split = {0}/{1} (alliance/horde)" -f $alliance, $horde)
        }
    }

    if (Test-Path -LiteralPath $hc) {
        $policy = Get-Conf $hc 'Hardcore.DeathPolicy'
        if ($policy -eq 'ghost' -or $policy -eq 'archive' -or $policy -eq 'delete') {
            Ok "mod_hardcore.conf: DeathPolicy=$policy"
        } else {
            Bad "mod_hardcore.conf: DeathPolicy is '$policy' (expected ghost, archive or delete)"
            if ($Fix) {
                $script:Fixes = $script:Fixes + 1
                Set-Conf -File $hc -Key 'Hardcore.DeathPolicy' -Value 'ghost'
                Ok 'DeathPolicy set to ghost'
            }
        }
    }

    if (Test-Path -LiteralPath $sv) {
        # A missing key is filled with the module's own fallback at load and a bad one is ignored
        # the same way, so a config that still starts the server can be wrong here without saying
        # so. The two are reported apart: absent means the live conf is older than the artifact,
        # bad means it says something the module will not take.
        $svAbsent = @()
        $svBad = @()
        foreach ($default in $SurvivorConfDefaults) {
            $eq = $default.IndexOf('=')
            $key = $default.Substring(0, $eq)
            $value = Get-Conf $sv $key
            if ($value -eq '') { $svAbsent += $key }
            elseif (-not (Test-SurvivorValueOk -Key $key -Value $value)) { $svBad += ("{0}={1}" -f $key, $value) }
        }
        if ($svAbsent.Count -eq 0 -and $svBad.Count -eq 0) {
            Ok ("mod-survivor.conf: Enable={0} MinLevelToFlag={1} DropEquipped={2} DropBags={3} ApplyToBots={4}" -f `
                (Get-Conf $sv 'Survivor.Enable'), (Get-Conf $sv 'Survivor.MinLevelToFlag'), (Get-Conf $sv 'Survivor.DropEquipped'), `
                (Get-Conf $sv 'Survivor.DropBags'), (Get-Conf $sv 'Survivor.ApplyToBots'))
        } else {
            if ($svBad.Count -gt 0) { Bad ("mod-survivor.conf: unusable value(s): {0}" -f ($svBad -join ' ')) }
            if ($svAbsent.Count -gt 0) { Bad ("mod-survivor.conf: key(s) the live file predates: {0} (--Repair appends the shipped defaults)" -f ($svAbsent -join ' ')) }
            if ($Fix) {
                $script:Fixes = $script:Fixes + 1
                Set-ConfDefaults -File $sv -Check { param($k, $v) Test-SurvivorValueOk -Key $k -Value $v } `
                    -Label 'mod-survivor.conf' -Defaults $SurvivorConfDefaults
            }
        }
    }

    if ((Test-Path -LiteralPath $pw) -and ((Test-Path -LiteralPath (Join-Path $moduleConf 'mod-pawn.conf')) -or
        (Test-Path -LiteralPath (Join-Path $moduleConf 'mod-pawn.conf.dist')))) {
        # The core loads every modules\*.conf, so the old file from the module's previous name
        # keeps being read while nothing looks at its Pawn.* keys.
        Bad 'mod-mercenaries.conf: the stale mod-pawn.conf from the module''s old name is still here'
        if ($Fix) {
            Remove-Item -LiteralPath (Join-Path $moduleConf 'mod-pawn.conf') -Force -ErrorAction SilentlyContinue
            Remove-Item -LiteralPath (Join-Path $moduleConf 'mod-pawn.conf.dist') -Force -ErrorAction SilentlyContinue
            $script:Fixes = $script:Fixes + 1
            Ok 'mod-mercenaries.conf: removed the stale mod-pawn.conf'
        }
    }

    if (Test-Path -LiteralPath $pw) {
        # Keys the module stopped reading. They are deleted by Write-Configs, and reported here so
        # an install that is never reinstalled loses them on its first -Repair instead.
        $retired = @(Get-ConfKeysMatching -File $pw -KeyPattern '^Mercenary\.ObeliskEntry$')
        $retired += @(Get-ConfKeysMatching -File $pw -KeyPattern '^Mercenary\.(Blacklist|Variant)\.[0-9]+\.[0-9]+$')
        $retired += @(Get-ConfKeysMatching -File $pw -KeyPattern '^Mercenary\.VariantWithhold(Min|Max)$')
        if ($retired.Count -gt 0) {
            Bad ("mod-mercenaries.conf: retired key(s) still present: {0} (nothing reads them)" -f ($retired -join ', '))
            if ($Fix) {
                $script:Fixes = $script:Fixes + 1
                Remove-ConfMatch -File $pw -KeyPattern '^Mercenary\.ObeliskEntry$'
                Remove-ConfMatch -File $pw -KeyPattern '^Mercenary\.(Blacklist|Variant)\.[0-9]+\.[0-9]+$'
                Remove-ConfMatch -File $pw -KeyPattern '^Mercenary\.VariantWithhold(Min|Max)$'
                Ok 'retired mercenary keys removed'
            }
        }
        $pwAbsent = @()
        $pwBad = @()
        foreach ($default in $MercenaryConfDefaults) {
            $eq = $default.IndexOf('=')
            $key = $default.Substring(0, $eq)
            $value = Get-Conf $pw $key
            if ($value -eq '') { $pwAbsent += $key }
            elseif (-not (Test-MercenaryValueOk -Key $key -Value $value)) { $pwBad += ("{0}={1}" -f $key, $value) }
        }
        # Outside the "something is wrong" branch on purpose: a conf whose scalars are all fine is
        # exactly the one an older install has, and it still has no temperament block to append.
        if ($Fix) {
            Add-MercenaryPoolKeys -Live $pw -Dist (Join-Path $moduleConf 'mod-mercenaries.conf.dist')
        }
        if ($pwAbsent.Count -eq 0 -and $pwBad.Count -eq 0) {
            Ok ("mod-mercenaries.conf: Enable={0} MaxMercenaries={1} LevelWindow={2} Featured={3}-{4} Refresh={5}-{6}min" -f `
                (Get-Conf $pw 'Mercenary.Enable'), (Get-Conf $pw 'Mercenary.MaxMercenaries'), (Get-Conf $pw 'Mercenary.LevelWindow'), `
                (Get-Conf $pw 'Mercenary.FeaturedMin'), (Get-Conf $pw 'Mercenary.FeaturedMax'), `
                (Get-Conf $pw 'Mercenary.PoolRefreshMinMinutes'), (Get-Conf $pw 'Mercenary.PoolRefreshMaxMinutes'))
        } else {
            if ($pwBad.Count -gt 0) { Bad ("mod-mercenaries.conf: unusable value(s): {0}" -f ($pwBad -join ' ')) }
            if ($pwAbsent.Count -gt 0) { Bad ("mod-mercenaries.conf: key(s) the live file predates: {0} (--Repair appends the shipped defaults)" -f ($pwAbsent -join ' ')) }
            if ($Fix) {
                $script:Fixes = $script:Fixes + 1
                Set-ConfDefaults -File $pw -Check { param($k, $v) Test-MercenaryValueOk -Key $k -Value $v } `
                    -Label 'mod-mercenaries.conf' -Defaults $MercenaryConfDefaults
            }
        }
    }

    if (Test-Path -LiteralPath $sb) {
        # A missing key is filled with the module's own fallback at load and a bad one is ignored
        # the same way, so a config that still starts the server can be wrong here without saying
        # so. The two are reported apart: absent means the live conf is older than the artifact,
        # bad means it says something the module will not take.
        # Keys the module stopped reading: deleted by Write-Configs, reported here so an install
        # that is never reinstalled loses them on its first -Repair instead.
        $sbRetired = @(Get-ConfKeysMatching -File $sb -KeyPattern '^Skateboard\.(OffInCombat|Drive\.Mode|MountedPose)$')
        if ($sbRetired.Count -gt 0) {
            Bad ("mod-skateboard.conf: retired key(s) still present: {0} (the hit rule and the spline carrier replaced them)" -f ($sbRetired -join ', '))
            if ($Fix) {
                $script:Fixes = $script:Fixes + 1
                Remove-ConfMatch -File $sb -KeyPattern '^Skateboard\.(OffInCombat|Drive\.Mode|MountedPose)$'
                Ok 'retired skateboard key(s) removed'
            }
        }
        $sbAbsent = @()
        $sbBad = @()
        foreach ($default in $SkateboardConfDefaults) {
            $eq = $default.IndexOf('=')
            $key = $default.Substring(0, $eq)
            $value = Get-Conf $sb $key
            if ($value -eq '') { $sbAbsent += $key }
            elseif (-not (Test-SkateboardValueOk -Key $key -Value $value)) { $sbBad += ("{0}={1}" -f $key, $value) }
        }
        if ($sbAbsent.Count -eq 0 -and $sbBad.Count -eq 0) {
            Ok ("mod-skateboard.conf: Enable={0} Deck={1} MaxSpeed={2} Crash.Enable={3}" -f `
                (Get-Conf $sb 'Skateboard.Enable'), (Get-Conf $sb 'Skateboard.Deck'), `
                (Get-Conf $sb 'Skateboard.MaxSpeed'), (Get-Conf $sb 'Skateboard.Crash.Enable'))
        } else {
            if ($sbBad.Count -gt 0) { Bad ("mod-skateboard.conf: unusable value(s): {0}" -f ($sbBad -join ' ')) }
            if ($sbAbsent.Count -gt 0) { Bad ("mod-skateboard.conf: key(s) the live file predates: {0} (--Repair appends the shipped defaults)" -f ($sbAbsent -join ' ')) }
            if ($Fix) {
                $script:Fixes = $script:Fixes + 1
                Set-ConfDefaults -File $sb -Check { param($k, $v) Test-SkateboardValueOk -Key $k -Value $v } `
                    -Label 'mod-skateboard.conf' -Defaults $SkateboardConfDefaults
            }
        }
    }

    if (Test-Path -LiteralPath $ei) {
        $off = @()
        foreach ($key in @('Enable', 'Loot', 'Gather', 'Talk', 'Use')) {
            # EInteract.Roll is not in this list: it is the one option a player may switch off on
            # purpose, and restoring it would be the repair fighting the player.
            if ((Get-Conf $ei ('EInteract.' + $key)) -ne '1') { $off += $key }
        }
        if ($off.Count -eq 0) {
            Ok 'mod_e_interact.conf: Enable/Loot/Gather/Talk/Use = 1'
        } else {
            Bad ("mod_e_interact.conf: off: {0} (the key is on, those steps are not)" -f ($off -join ' '))
            if ($Fix) {
                foreach ($key in $off) { Set-Conf -File $ei -Key ('EInteract.' + $key) -Value 1 }
                $script:Fixes = $script:Fixes + 1
                Ok 'interact options set to 1'
            }
        }
    }

    if (Test-Path -LiteralPath $pb) {
        if ((Get-Conf $pb 'Playerbots.RandomBotNearPlayers') -ne '') {
            Ok ("playerbots.conf: near-player distribution = {0}" -f (Get-Conf $pb 'Playerbots.RandomBotNearPlayers'))
            # Every key the mode reads, with the value the installer writes. A config from an older
            # install can be missing the newer ones, which the binary then fills with its own
            # defaults - so name them rather than silently accepting the difference.
            $nearPairs = @(
                'RandomBotNearPlayersLevelRange=3'
                'RandomBotNearPlayersFollowInterval=120'
                ('RandomBotNearPlayersMinDistance={0}' -f $NearMinDistance)
                'RandomBotNearPlayersSameFaction=1'
                ('RandomBotNearPlayersMaxPerPlayer={0}' -f $NearBots)
                'RandomBotNearPlayersZoneTraffic=1'
                ('RandomBotNearPlayersZoneVisitMinTime={0}' -f $NearVisitMin)
                ('RandomBotNearPlayersZoneVisitMaxTime={0}' -f $NearVisitMax)
                ('RandomBotNearPlayersZoneAwayMinTime={0}' -f $NearAwayMin)
                ('RandomBotNearPlayersZoneAwayMaxTime={0}' -f $NearAwayMax)
            )
            $nearMissing = $false
            foreach ($pair in $nearPairs) {
                $eq = $pair.IndexOf('=')
                $shortKey = $pair.Substring(0, $eq)
                $want = $pair.Substring($eq + 1)
                $fullKey = 'Playerbots.' + $shortKey
                $now = Get-Conf $pb $fullKey
                if ($now -eq '') {
                    $nearMissing = $true
                    Bad "playerbots.conf: $shortKey missing (old artifact or config)"
                    if ($Fix) {
                        $script:Fixes = $script:Fixes + 1
                        Set-Conf -File $pb -Key $fullKey -Value $want
                        Ok "$shortKey set to $want"
                    }
                    continue
                }
                if ($now -eq $want) { continue }
                # Present is not the same as right, but only a superseded installer default is
                # replaced - a value that was never written by the installer is somebody's tuning.
                $stale = @(Get-StaleNearValue -Key $shortKey)
                if ($stale -contains $now) {
                    Bad "playerbots.conf: $shortKey is the old default $now, the installer now writes $want"
                    if ($Fix) {
                        $script:Fixes = $script:Fixes + 1
                        Set-Conf -File $pb -Key $fullKey -Value $want
                        Ok "$shortKey set to $want"
                    }
                } else {
                    Log "      playerbots.conf: $shortKey is $now (set by hand; the installer default is $want)"
                }
            }
            if (-not $nearMissing) {
                Ok ("playerbots.conf: zone bots max={0} min-distance={1} traffic={2} visit={3}-{4}s away={5}-{6}s" -f `
                    (Get-Conf $pb 'Playerbots.RandomBotNearPlayersMaxPerPlayer'), (Get-Conf $pb 'Playerbots.RandomBotNearPlayersMinDistance'), `
                    (Get-Conf $pb 'Playerbots.RandomBotNearPlayersZoneTraffic'), (Get-Conf $pb 'Playerbots.RandomBotNearPlayersZoneVisitMinTime'), `
                    (Get-Conf $pb 'Playerbots.RandomBotNearPlayersZoneVisitMaxTime'), (Get-Conf $pb 'Playerbots.RandomBotNearPlayersZoneAwayMinTime'), `
                    (Get-Conf $pb 'Playerbots.RandomBotNearPlayersZoneAwayMaxTime'))
            }
            $twin = @(Get-ConfKeysMatching -File $pb -KeyPattern '^Playerbots\.RandomBotNearPlayers(Radius|InnerCount|OuterRadius|ZoneCount)$')
            if ($twin.Count -gt 0) {
                Warn ("playerbots.conf: twin-ring keys are still in the config ({0}); the current build ignores them" -f ($twin -join ', '))
                if ($Fix) {
                    $script:Fixes = $script:Fixes + 1
                    Remove-ConfMatch -File $pb -KeyPattern '^Playerbots\.RandomBotNearPlayers(Radius|InnerCount|OuterRadius|ZoneCount)$'
                    Ok 'twin-ring keys removed'
                }
            }
        } else {
            Bad 'playerbots.conf: RandomBotNearPlayers missing (artifact built without the player-centric patch?)'
            if ($Fix) {
                $script:Fixes = $script:Fixes + 1
                Set-Conf -File $pb -Key 'Playerbots.RandomBotNearPlayers' -Value 1
                Set-Conf -File $pb -Key 'Playerbots.RandomBotNearPlayersLevelRange' -Value 3
                Set-Conf -File $pb -Key 'Playerbots.RandomBotNearPlayersFollowInterval' -Value 120
                Set-Conf -File $pb -Key 'Playerbots.RandomBotNearPlayersMinDistance' -Value $NearMinDistance
                Set-Conf -File $pb -Key 'Playerbots.RandomBotNearPlayersSameFaction' -Value 1
                Set-Conf -File $pb -Key 'Playerbots.RandomBotNearPlayersMaxPerPlayer' -Value $NearBots
                Set-Conf -File $pb -Key 'Playerbots.RandomBotNearPlayersZoneTraffic' -Value 1
                Set-Conf -File $pb -Key 'Playerbots.RandomBotNearPlayersZoneVisitMinTime' -Value $NearVisitMin
                Set-Conf -File $pb -Key 'Playerbots.RandomBotNearPlayersZoneVisitMaxTime' -Value $NearVisitMax
                Set-Conf -File $pb -Key 'Playerbots.RandomBotNearPlayersZoneAwayMinTime' -Value $NearAwayMin
                Set-Conf -File $pb -Key 'Playerbots.RandomBotNearPlayersZoneAwayMaxTime' -Value $NearAwayMax
                Ok 'near-player distribution keys added'
            }
        }
    }

    # 3. generated helpers
    $helpers = @('start.ps1', 'stop.ps1', 'set-bots.ps1', 'start-db.cmd', 'stop-db.cmd', 'credentials.txt')
    $missingHelper = $false
    foreach ($helper in $helpers) {
        if (Test-HelperFile $helper) {
            Ok "helper: $script:Root\$helper"
        } else {
            Bad "helper missing: $script:Root\$helper"
            $missingHelper = $true
        }
    }
    if ($missingHelper -and $Fix) {
        $script:Fixes = $script:Fixes + 1
        Warn 'rewriting the generated helper scripts'
        Write-Helpers
    }

    # 4. client data and SQL sources
    foreach ($dir in @('dbc', 'maps', 'vmaps', 'mmaps')) {
        $path = Join-Path $script:DataDir $dir
        if (Test-Path -LiteralPath $path) {
            Ok "client data: $dir"
        } else {
            Bad "client data missing: $path"
            if ($Fix) {
                $script:Fixes = $script:Fixes + 1
                Install-ClientData
            }
        }
    }
    Show-ClientPatchStatus
    $sqlSources = @(
        (Join-Path $script:SrcDir 'data\sql')
        (Join-Path $script:SrcDir 'modules\mod-playerbots\data\sql')
        (Join-Path $script:SrcDir 'modules\mod-creature-gear\data\sql')
        (Join-Path $script:SrcDir 'modules\mod-survivor\data\sql')
    )
    foreach ($path in $sqlSources) {
        if (Test-Path -LiteralPath $path) {
            Ok "sql sources: $path"
        } else {
            Bad "sql sources missing: $path (the DB updater cannot run)"
            if ($Fix) {
                Warn 're-run the install to restore them: .\windows-install.ps1 -Force'
            }
        }
    }
    # Only the families that ship: the graveyard patch was reverted, so probing for it would report
    # a permanent failure.
    $customSql = Join-Path $script:SrcDir 'data\sql\custom\db_world'
    foreach ($family in @(@('loot_bags', 'loot-bag patch'), @('travel_portals', 'travel-portal patch'))) {
        $name = $family[0]
        $label = $family[1]
        $found = @()
        if (Test-Path -LiteralPath $customSql) {
            $found = @(Get-ChildItem -LiteralPath $customSql -Filter ($name + '_*.sql') -File)
        }
        if ($found.Count -gt 0) {
            Ok ("sql sources: {0} ({1})" -f $label, $found[0].Name)
        } else {
            Bad ("{0} missing: {1}\{2}_*.sql" -f $label, $customSql, $name)
            if ($Fix) {
                Warn 're-run the install to restore it: .\windows-install.ps1 -Force'
            }
        }
    }

    # 5. database
    if (-not (Test-Path -LiteralPath (Get-MysqlPath 'mysql.exe'))) {
        Bad "the bundled mysql client is missing ($script:MysqlDir\bin\mysql.exe)"
    } else {
        if (Test-Db) {
            Ok "mysqld answering on 127.0.0.1:$DbPort"
        } else {
            Bad "mysqld is not answering on 127.0.0.1:$DbPort"
            if ($Fix) {
                $script:Fixes = $script:Fixes + 1
                Start-Db
            }
        }
        if (Test-Db) {
            Resolve-AdminHost
            $schemaMissing = $false
            foreach ($schema in @('acore_auth', 'acore_world', 'acore_characters')) {
                if (Test-SchemaExists $schema) {
                    Ok "schema: $schema"
                } else {
                    Bad "schema missing: $schema"
                    $schemaMissing = $true
                }
            }
            if ($schemaMissing -and $Fix) {
                $script:Fixes = $script:Fixes + 1
                New-Schema
            }
            $worldTables = Get-SchemaTableCount 'acore_world'
            if ($worldTables -gt 0) {
                Ok ("acore_world has {0} tables" -f $worldTables)
            } else {
                Bad 'acore_world is empty: the first boot never populated it'
                if ($Fix) {
                    $script:Fixes = $script:Fixes + 1
                    Warn 'running the first boot again to populate the databases'
                    Invoke-FirstBoot
                }
            }
            if ($script:Account) {
                $rows = Invoke-Sql ("SELECT COUNT(*) FROM acore_auth.account WHERE username='{0}'" -f $script:Account)
                $count = ''
                if ($null -ne $rows) { $count = ($rows -join '').Trim() }
                if ($count -eq '1') {
                    Ok ("account: {0}" -f $script:Account)
                } else {
                    Bad ("account missing: {0}" -f $script:Account)
                    if ($Fix -and $worldTables -gt 0) {
                        $script:Fixes = $script:Fixes + 1
                        $commands = @(
                            ("account create {0} {1}" -f $script:Account, $script:AccountPass)
                            ("account set gmlevel {0} 3 -1" -f $script:Account)
                        )
                        # Same reason as the first boot: the live config has Console.Enable 0, so the
                        # console commands only reach the world through a copy that has 1.
                        $repairConf = Join-Path $script:StateDir 'repair-console.conf'
                        try {
                            if (New-ConsoleConf -Destination $repairConf) {
                                if (Invoke-WorldConsole -TimeoutSeconds 300 -Commands $commands -ConfPath $repairConf) {
                                    Ok ("account {0} created" -f $script:Account)
                                } else {
                                    Bad ("could not create {0}" -f $script:Account)
                                }
                            } else {
                                Bad ("could not create {0}: no worldserver.conf to copy" -f $script:Account)
                            }
                        } finally {
                            Remove-ConsoleConf $repairConf
                        }
                    }
                }
            }
        }
    }

    # 6. host wrappers and firewall
    foreach ($name in @('start-wow-server.cmd', 'stop-wow-server.cmd')) {
        $path = Join-Path $env:USERPROFILE $name
        if (Test-Path -LiteralPath $path) {
            Ok "wrapper: $path"
        } else {
            Bad "wrapper missing: $path"
            if ($Fix) {
                $script:Fixes = $script:Fixes + 1
                Write-HostWrappers
            }
        }
    }
    if ($script:LanIp) {
        if (Have 'Get-NetFirewallRule') {
            foreach ($port in @($AuthPort, $WorldPort)) {
                $name = Get-FirewallRuleName -Port $port
                $rule = $null
                try {
                    $rule = Get-NetFirewallRule -DisplayName $name -ErrorAction SilentlyContinue
                } catch {
                    # Reading the firewall needs elevation; not having it is not a broken install.
                    Log "      firewall: cannot read the rules from this session ($($_.Exception.Message))"
                    continue
                }
                if ($rule) {
                    Ok ("firewall: inbound TCP {0} allowed" -f $port)
                } else {
                    Bad ("firewall: no inbound rule for {0} (the state records LAN IP {1})" -f $port, $script:LanIp)
                    if ($Fix) {
                        $script:Fixes = $script:Fixes + 1
                        Add-FirewallRuleForPort -Port $port
                    }
                }
            }
        } else {
            Log '--    firewall: Get-NetFirewallRule is unavailable on this build'
        }
    }

    # 7. processes
    foreach ($name in @('worldserver', 'authserver', 'mysqld')) {
        if (@(Get-Process -Name $name -ErrorAction SilentlyContinue).Count -gt 0) {
            Ok "$name is running"
        } else {
            Log "--    $name is not running"
        }
    }
}

function Get-ConfKeysMatching {
    <# Get-ConfKeysMatching -File F -KeyPattern REGEX - live keys that match. #>
    param(
        [string]$File,
        [string]$KeyPattern
    )
    $keys = @()
    if (-not (Test-Path -LiteralPath $File)) { return $keys }
    foreach ($line in [System.IO.File]::ReadAllLines($File)) {
        if ($line -match '^\s*#') { continue }
        $eq = $line.IndexOf('=')
        if ($eq -lt 0) { continue }
        $name = $line.Substring(0, $eq).Trim()
        if ($name -cmatch $KeyPattern) { $keys += $name }
    }
    return $keys
}

function Complete-Checks {
    if ($script:Problems -gt 0) {
        Log ''
        Log ("FAIL  {0} problem(s) found" -f $script:Problems)
        exit 1
    }
    Log ''
    if ($script:Fixes -gt 0) {
        Log ("ok    all checks pass ({0} fix(es) applied)" -f $script:Fixes)
    } else {
        Log 'ok    all checks pass'
    }
}

# ---------------------------------------------------------------- actions

function Invoke-Status {
    Invoke-Preflight
    if (-not (Import-State)) {
        Log ("no install state in {0}: this looks like an empty or foreign directory" -f $script:StateDir)
    }
    if ((Resolve-Mode) -eq 'docker') {
        Invoke-DockerStatus
        return
    }
    Log ''
    Invoke-Checks
    Complete-Checks
}

function Invoke-Repair {
    Invoke-Preflight
    $hadState = Import-State
    $script:HadState = $hadState
    if (-not $hadState) {
        Warn 'no install state found; repairing what can be found under the given -Root'
    }
    if ((Resolve-Mode) -eq 'docker') {
        Invoke-DockerRepair
        return
    }
    Log ''
    # Write-Configs is idempotent: it rewrites the keys the installer owns, restores drifted
    # defaults and deletes the keys the modules stopped reading, and leaves every hand-set value
    # alone. Running it up front is what makes -Repair fix an old conf that still carries them,
    # rather than only checking it.
    if (Test-Path -LiteralPath (Join-Path $script:ConfDir 'worldserver.conf.dist')) {
        Write-Configs
    } else {
        Warn "no $script:ConfDir\worldserver.conf.dist; skipping the conf rewrite (re-run the install with -Force to restore it)"
    }
    # Same for the two wrappers: rewriting them is how a moved root or a deleted %USERPROFILE% file
    # comes back without a full reinstall.
    Write-HostWrappers
    Invoke-Checks -Fix
    # The fix pass rewrites the conf from state, and a value given on this command line wins over the
    # recorded one (`-Repair -Bots 120`, `-Repair -HordeRatio 70`), so without this the repair applied
    # the value to the conf and left state behind it: the re-check below would report the conf as
    # drifted from state, -Status would report the same, and the next -Repair would revert it. The
    # container repair already saves (Invoke-DockerRepair); save only when a state file was actually
    # loaded, so a stateless repair cannot invent one with default passwords in it.
    if ($hadState) { Save-State }
    # The client half is not a conf key or a file this script owns: it is a Python run against the
    # player's client folder and the server's Item.dbc, so -Repair re-applies it rather than checking
    # it, and the re-check below reports what it produced. The tool it runs comes from the commit the
    # install is pinned to, so the ref is resolved here too when this mode did not do it already.
    Resolve-SelfRef
    Install-ClientPatch
    $repaired = $script:Fixes
    $script:Problems = 0
    Log ''
    Log ("re-checking after {0} fix(es)" -f $repaired)
    Invoke-Checks
    Complete-Checks
}

function Stop-ServerProcesses {
    <# Stop-ServerProcesses - stop worldserver (politely first), then authserver. #>
    # The database is left running on purpose: -Uninstall -KeepDb dumps it afterwards, and the
    # dump is the only thing that survives the removal of the root.
    if ((@(Get-Process -Name 'worldserver' -ErrorAction SilentlyContinue).Count -eq 0) -and
        (@(Get-Process -Name 'authserver' -ErrorAction SilentlyContinue).Count -eq 0)) {
        Log 'the servers were not running'
        return
    }
    # Same handshake as stop.ps1: start.ps1 holds the world's console, so it is the only process
    # that can type "server shutdown 1"; if it is not running there is nothing to ask.
    $request = Join-Path $script:StateDir 'stop-request'
    if (@(Get-Process -Name 'worldserver' -ErrorAction SilentlyContinue).Count -gt 0) {
        [void][System.IO.Directory]::CreateDirectory($script:StateDir)
        Set-Content -LiteralPath $request -Value 'stop' -Encoding ASCII
        $deadline = (Get-Date).AddSeconds(150)
        while ((Get-Date) -lt $deadline -and (@(Get-Process -Name 'worldserver' -ErrorAction SilentlyContinue).Count -gt 0)) {
            Start-Sleep -Seconds 1
        }
    }
    if (@(Get-Process -Name 'worldserver' -ErrorAction SilentlyContinue).Count -gt 0) {
        Warn 'worldserver did not stop; stopping it forcibly (unsaved progress is lost)'
        Get-Process -Name 'worldserver' -ErrorAction SilentlyContinue | Stop-Process -Force
    }
    if (@(Get-Process -Name 'authserver' -ErrorAction SilentlyContinue).Count -gt 0) {
        Get-Process -Name 'authserver' -ErrorAction SilentlyContinue | Stop-Process -Force
        Log 'stopped authserver'
    }
    Remove-Item -LiteralPath $request -Force -ErrorAction SilentlyContinue
}

function Invoke-Uninstall {
    Invoke-Preflight
    if (-not (Import-State)) {
        Warn ("no install state in {0}; removing what the manifest in {1} records" -f $script:StateDir, $script:ManifestFile)
    }
    if ((Resolve-Mode) -eq 'docker') {
        Invoke-DockerUninstall
        return
    }
    Step 'Stopping the realm'
    if ($script:DryRun) {
        Note 'stop worldserver, authserver and mysqld'
        Note ("remove the paths in {0} and {1}" -f $script:ManifestFile, $script:Root)
        return
    }
    Stop-ServerProcesses

    if ($script:KeepDb) {
        Step 'Dumping the databases'
        $dumpDir = Join-Path $script:HostState 'dumps'
        [void][System.IO.Directory]::CreateDirectory($dumpDir)
        if (-not (Test-Db)) {
            # Dying here is deliberate: nothing has been removed yet, and an empty "dump" that is
            # actually a failure is worse than an uninstall that stops and says why.
            Warn 'the database is not running; starting it so it can be dumped'
            Start-Db
        }
        $existing = @()
        foreach ($schema in @('acore_auth', 'acore_world', 'acore_characters', 'acore_playerbots')) {
            if (Test-SchemaExists $schema) { $existing += $schema }
        }
        if ($existing.Count -eq 0) {
            Warn 'no acore_* schema found; nothing to dump'
        } else {
            $stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss')
            foreach ($schema in $existing) {
                $file = Join-Path $dumpDir ('{0}-{1}.sql' -f $stamp, $schema)
                $errFile = $file + '.err'
                $dumpArgs = @('--protocol=tcp', ('-h{0}' -f $script:AdminHost), ('-P{0}' -f $DbPort), '-uroot',
                    '--single-transaction', '--routines', '--events', '--databases', $schema)
                $proc = Start-Process -FilePath (Get-MysqlPath 'mysqldump.exe') -ArgumentList $dumpArgs `
                    -RedirectStandardOutput $file -RedirectStandardError $errFile -NoNewWindow -Wait -PassThru
                if ($proc.ExitCode -eq 0 -and (Test-Path -LiteralPath $file)) {
                    Log ("dumped {0} to {1}" -f $schema, $file)
                    Remove-Item -LiteralPath $errFile -Force -ErrorAction SilentlyContinue
                } else {
                    Warn ("mysqldump failed for {0}; see {1}" -f $schema, $errFile)
                }
            }
        }
        Log ("the data directory {0} goes with the root; the dumps above are what survives" -f $script:DbDir)
    }

    Step 'Stopping the database'
    # After the dump, before the root (and <root>\db) is removed: a mysqld still running out of a
    # directory that is being deleted is how a half-flushed InnoDB file is left behind.
    if (Test-Db) {
        Stop-Db
        Log 'stopped mysqld'
    } else {
        Log 'mysqld was not running'
    }

    Step 'Removing files recorded in the manifest'
    $entries = @(Get-ManifestEntries | Sort-Object -Unique)
    # The manifest lives in %USERPROFILE%\.win-wow, so that directory has to go last.
    $deferred = @()
    $removed = 0
    foreach ($entry in $entries) {
        if ($entry -like 'firewall:*') {
            $port = 0
            if ([int]::TryParse($entry.Substring(9), [ref]$port)) {
                Disable-FirewallRule -Port $port
            }
            continue
        }
        if ($entry.TrimEnd('\') -eq $script:HostState.TrimEnd('\')) {
            $deferred += $entry
            continue
        }
        if (-not (Test-Path -LiteralPath $entry)) { continue }
        if (-not (Test-SafeRemovePath $entry)) { continue }
        Remove-Item -LiteralPath $entry -Recurse -Force -ErrorAction SilentlyContinue
        if (-not (Test-Path -LiteralPath $entry)) {
            Log "removed $entry"
            $removed = $removed + 1
        } else {
            Warn "could not remove $entry"
        }
    }
    foreach ($entry in $deferred) {
        if (-not (Test-Path -LiteralPath $entry)) { continue }
        if (-not (Test-SafeRemovePath $entry)) { continue }
        if ($script:KeepDb -and $entry.TrimEnd('\') -eq $script:HostState.TrimEnd('\')) {
            # -KeepDb promised a dump that survives: empty this directory, keep dumps\.
            foreach ($child in @(Get-ChildItem -LiteralPath $entry -Force)) {
                if ($child.Name -eq 'dumps') { continue }
                Remove-Item -LiteralPath $child.FullName -Recurse -Force -ErrorAction SilentlyContinue
            }
            Log ("kept {0}\dumps (-KeepDb)" -f $entry)
            continue
        }
        Remove-Item -LiteralPath $entry -Recurse -Force -ErrorAction SilentlyContinue
        if (-not (Test-Path -LiteralPath $entry)) { Log "removed $entry" }
    }
    if ($removed -eq 0 -and $deferred.Count -eq 0) {
        Log 'nothing outside the install root was recorded'
    }

    Step 'Removing the install root'
    if (-not (Test-Path -LiteralPath $script:Root)) {
        Log ("{0} is already gone" -f $script:Root)
        return
    }
    if (-not (Test-SafeRemovePath $script:Root)) {
        Warn ("refusing to remove {0}; remove it by hand" -f $script:Root)
        return
    }
    Remove-Item -LiteralPath $script:Root -Recurse -Force -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $script:Root) {
        Warn ("{0} could not be removed completely; something is still using it" -f $script:Root)
    } else {
        Log ("removed {0}" -f $script:Root)
    }
}

function Invoke-Install {
    Invoke-Preflight
    Install-ServerFiles
    Test-Layout
    Install-ClientData
    Install-MySql
    New-Schema
    Write-Configs
    Invoke-FirstBoot
    Resolve-SelfRef
    Install-ClientPatch
    Write-Helpers
    Write-HostWrappers
    Enable-Firewall
    # The mode file is what -Status, -Repair and -Uninstall read: a native root and a container root
    # are different realms, and the recorded mode is what keeps the two removal paths apart.
    Save-Mode -Mode 'native'
    Save-State
    Invoke-Summary
}

function Install-ForMode {
    # The requested mode, or the one this root already holds: a re-run of the installer and a -Check
    # both stay on the path the root was installed with, so a container realm is never "repaired"
    # into a native one (or the reverse) by a missing -Docker.
    $mode = Resolve-Mode
    Assert-ModeFree -Mode $mode
    if ($mode -eq 'docker') {
        Invoke-DockerInstall
    } else {
        Invoke-Install
    }
}

# ---------------------------------------------------------------- docker mode

# The realm can also run as two containers: an image built from docker/Dockerfile (a headless
# ubuntu server) and mysql:8.4, driven by docker/docker-compose.yml. That is the "easy to serve
# and delete" path - every piece of state is a named volume, so "docker compose down -v" puts the
# host back the way it was. This mode stages the container files from this repository at
# -SelfRef into <root>\docker-src (the layout docker/docker-compose.yml expects, since it builds
# with the repository root as its context) and drives compose from there.
#
# It is exclusive with a native install in the same root: the mode recorded in
# <root>\.win-state\mode is what -Status, -Repair and -Uninstall dispatch on, so a container realm
# is never torn down by the native path (which would walk the manifest, find no <root>\db, and
# leave the volumes running) and the other way round.

function Get-DockerComposeFile {
    return (Join-Path (Join-Path $script:DockerDir 'docker') 'docker-compose.yml')
}

function Invoke-Docker {
    <# Invoke-Docker -Arguments @(...) [-WorkingDirectory DIR] [-Capture] [-AllowFailure] #>
    # -Capture collects the output and returns it as text; without it the command streams to this
    # console, which is what a build has to do (it takes minutes and a silent one looks hung).
    param(
        [string[]]$Arguments,
        [string]$WorkingDirectory = '',
        [switch]$Capture,
        [switch]$AllowFailure
    )
    # Every docker command is echoed: -Check prints them and runs none, and a failed compose call is
    # only diagnosable when the exact argv is on screen. The database password rides in MYSQL_PWD
    # (so it never shows in a process list by itself) and is masked here for the same reason.
    $shown = ($Arguments -join ' ') -replace 'MYSQL_PWD=\S+', 'MYSQL_PWD=***'
    Log ("  > docker {0}" -f $shown)
    if ($script:DryRun) { return '' }
    $cwd = $WorkingDirectory
    if (-not $cwd) { $cwd = $script:Root }
    # compose resolves relative paths (and a .env) from the directory it is run in, so it runs in
    # the staging directory - but a status of a directory that was never staged still has to be
    # able to ask the daemon, and Push-Location to a path that is not there is a terminating error.
    $pushed = $false
    if (Test-Path -LiteralPath $cwd) {
        Push-Location -LiteralPath $cwd
        $pushed = $true
    }
    $text = ''
    $code = 0
    try {
        if ($Capture) {
            # Invoke-Native merges stderr into the text and decides success from $LASTEXITCODE: a
            # daemon that is down writes its complaint to stderr, which a bare '&' would turn into
            # a fatal error record under $ErrorActionPreference = 'Stop' (see Invoke-Native).
            # Joined per line, not piped through Out-String: a mysqldump line is one string and
            # Out-String would reformat it through the console width.
            $text = Invoke-Native -FilePath 'docker' -Arguments $Arguments
            $code = $LASTEXITCODE
        } else {
            # -Echo streams both streams to this console as they arrive, so docker's own progress
            # and warnings reach the user while a build runs - and an error record from a failed
            # pull still cannot abort the script.
            [void](Invoke-Native -FilePath 'docker' -Arguments $Arguments -Echo)
            $code = $LASTEXITCODE
        }
    } finally {
        if ($pushed) { Pop-Location }
    }
    if ($code -ne 0 -and -not $AllowFailure) {
        if ($text) { [Console]::Error.WriteLine($text) }
        Die ("docker {0} exited {1}" -f $Arguments[0], $code)
    }
    return $text
}

function Invoke-Compose {
    <# Invoke-Compose -Arguments @('ps') [-Capture] [-AllowFailure] - against the staged file. #>
    param(
        [string[]]$Arguments,
        [switch]$Capture,
        [switch]$AllowFailure
    )
    $argv = @('compose', '-f', (Get-DockerComposeFile))
    foreach ($item in $Arguments) { $argv += $item }
    return (Invoke-Docker -Arguments $argv -WorkingDirectory $script:DockerDir -Capture:$Capture -AllowFailure:$AllowFailure)
}

function Get-BriefText {
    <# Get-BriefText TEXT - one line, clipped from the front, for a status line. #>
    param([string]$Text)
    $line = ''
    foreach ($piece in @($Text -split "`r?`n")) {
        if ($piece.Trim()) { $line = $piece.Trim(); break }
    }
    $line = ($line -replace '\s+', ' ')
    # The CLI spells its reason as 'Get "http://%2F%2F.%2Fpipe%2F.../v1.50/version": open //./pipe/
    # ...', and the escaped URL says nothing the pipe path after the colon does not; dropping it is
    # what keeps this a status line instead of a log excerpt.
    $line = ($line -replace 'Get "[^"]*": ', '')
    # The end of a docker failure is the actionable part ("open //./pipe/...: The system cannot find
    # the file specified"), so an over-long line is clipped from the front, not from the back.
    if ($line.Length -gt 150) { $line = '...' + $line.Substring($line.Length - 147) }
    return $line
}

function Test-DockerDaemon {
    <# Test-DockerDaemon - true when the Docker CLI and a reachable daemon are both present. #>
    # Why it said no is kept in $script:DockerDaemonReason, one line: -Status prints it beside the
    # FAIL so a closed Docker Desktop is diagnosable from the status alone.
    $script:DockerDaemonReason = 'docker.exe is not on PATH (install Docker Desktop)'
    if (-not (Have 'docker')) { return $false }
    if ($script:DryRun) { return $true }
    # The CLI alone proves nothing: Docker Desktop installs it whether or not the engine behind it
    # is up, and every command below fails with "cannot connect to the Docker daemon" until it is.
    # Through Invoke-Native: the failure arrives on stderr, which a bare '&' would turn into a fatal
    # error record and end the status with a stack trace instead of this line (see Invoke-Native).
    $server = Invoke-Native -FilePath 'docker' -Arguments @('version', '--format', '{{.Server.Version}}')
    if ($LASTEXITCODE -ne 0) {
        $brief = Get-BriefText $server
        if (-not $brief) { $brief = ('docker version exited {0} without a message' -f $LASTEXITCODE) }
        $script:DockerDaemonReason = $brief
        return $false
    }
    $text = $server.Trim()
    if (-not $text) {
        $script:DockerDaemonReason = 'docker version reported no server version'
        return $false
    }
    $script:DockerDaemonReason = ''
    return $true
}

function Test-DockerPrerequisites {
    Step 'Docker'
    if ($script:DryRun) {
        Note 'require docker.exe on PATH, a reachable daemon and the compose plugin'
        Log '  > docker --version'
        Log '  > docker version --format "{{.Server.Version}}"'
        Log '  > docker compose version'
        return
    }
    if (-not (Have 'docker')) {
        Die 'the Docker CLI is not on PATH: install Docker Desktop (https://www.docker.com/products/docker-desktop/), open it once, and re-run'
    }
    # -StdOutOnly: the CLI answers on stdout, and a warning on stderr must not be mixed into the
    # version line this prints (both calls go through Invoke-Native so neither can abort the run).
    $cli = (Invoke-Native -FilePath 'docker' -Arguments @('--version') -StdOutOnly).Trim()
    Log ("docker CLI: {0}" -f $cli)
    if (-not (Test-DockerDaemon)) {
        Die ("the Docker daemon is not reachable ({0}): start Docker Desktop and wait until it reports that it is running, then re-run (nothing has been changed)" -f $script:DockerDaemonReason)
    }
    $compose = (Invoke-Native -FilePath 'docker' -Arguments @('compose', 'version') -StdOutOnly).Trim()
    if ($LASTEXITCODE -ne 0 -or -not $compose) {
        Die 'the docker compose plugin is missing: it ships with Docker Desktop, so update or reinstall it'
    }
    Log ("compose: {0}" -f $compose)
}

function Install-DockerAssets {
    <# Install-DockerAssets - fetch the container files into <root>\docker-src at -SelfRef. #>
    Step 'Container files'
    if ($script:DryRun) {
        $base = $DockerRepoBase + (Get-FetchRef) + '/'
        foreach ($rel in $DockerFiles) {
            Note ("download {0}{1} to {2}" -f $base, $rel, (Join-Path $script:DockerDir ($rel.Replace('/', '\'))))
        }
        return
    }

    # The whole list is fetched twice at most. A recorded pin cannot serve a file that shipped after
    # it (the deck tool predates scripts/lib/dbc_mpq.py, so a realm installed before the lib existed
    # 404s on it), and the first attempt's answer to that used to be "check the network". The second
    # attempt resolves the ref again and starts over, because the files fetched before the failure
    # came from the stale commit too.
    for ($attempt = 1; $attempt -le 2; $attempt++) {
        $base = $DockerRepoBase + (Get-FetchRef) + '/'
        $failed = ''
        foreach ($rel in $DockerFiles) {
            $dest = Join-Path $script:DockerDir ($rel.Replace('/', '\'))
            $dir = [System.IO.Path]::GetDirectoryName($dest)
            if ($dir) { [void][System.IO.Directory]::CreateDirectory($dir) }
            Log ("fetching {0}{1}" -f $base, $rel)
            if (-not (Get-RemoteFile -Uri ($base + $rel) -Destination $dest)) { $failed = $rel; break }
        }
        if (-not $failed) { break }

        $refreshable = ($attempt -eq 1) -and (-not $script:Bound.ContainsKey('SelfRef')) -and $script:SelfRefSha
        if (-not $refreshable) {
            Die ("could not fetch {0}{1}: check -SelfRef (currently '{2}') and the network" -f $base, $failed, (Get-FetchRef))
        }
        Warn ("{0} is not in commit {1} - the pin this realm recorded predates it, resolving -SelfRef '{2}' again" -f `
            $failed, (Get-FetchRef), $script:SelfRef)
        Reset-SelfRefPin
        Save-State
    }
    # A 404 from raw.githubusercontent.com lands as a file with the error text in it on some
    # proxies, and a compose file of four bytes fails much later with a message that says nothing
    # about the download, so both halves are checked here.
    $missing = @()
    foreach ($rel in $DockerFiles) {
        $dest = Join-Path $script:DockerDir ($rel.Replace('/', '\'))
        if (-not (Test-Path -LiteralPath $dest)) { $missing += $rel; continue }
        if ((Get-Item -LiteralPath $dest).Length -le 0) { $missing += $rel }
    }
    if ($missing.Count -gt 0) {
        Die ("the fetched container files are missing or empty: {0}" -f ($missing -join ', '))
    }
    Log ("staged {0} files in {1}" -f $DockerFiles.Count, $script:DockerDir)
}

function Get-DockerArtifactPin {
    <# Get-DockerArtifactPin - the artifact URL and its published sha256, or $null. #>
    # Docker decides what to rebuild by hashing each instruction and the build args in scope. The
    # artifact step is one RUN that downloads a release asset from a moving URL
    # (releases/download/<tag>/...), and its sha256 is fetched inside that same layer - so with no
    # args the layer's key is identical from one release to the next and Docker replays the old
    # tarball, whatever was published since. Handing the sha in as a build arg changes the key with
    # every release, so the download re-runs and the verification is against the same hash.
    #
    # The URL is read out of the staged Dockerfile rather than repeated here: that file is the one
    # the build reads, so the two cannot drift.
    param()
    $arch = if ($env:PROCESSOR_ARCHITECTURE -match '^ARM64$') { 'arm64' } else { 'amd64' }
    $dockerfile = Join-Path $script:DockerDir 'docker\Dockerfile'
    if (-not (Test-Path -LiteralPath $dockerfile)) { return $null }
    $text = [System.IO.File]::ReadAllText($dockerfile)
    $m = [regex]::Match($text, '(?m)^\s*' + [regex]::Escape($arch) + '\)\s*default_url="([^"]+)"')
    if (-not $m.Success) { return $null }
    $url = $m.Groups[1].Value
    $shaFile = Join-Path $env:TEMP 'acore-artifact.sha256'
    if (-not (Get-RemoteFile -Uri ($url + '.sha256') -Destination $shaFile -Attempts 2)) { return $null }
    $sha = ([System.IO.File]::ReadAllText($shaFile)).Trim()
    Remove-Item -LiteralPath $shaFile -Force -ErrorAction SilentlyContinue
    if ($sha) { $sha = ($sha -split '\s+')[0].ToLowerInvariant() }
    if ($sha -notmatch '^[0-9a-f]{64}$') { return $null }
    return @{ Url = $url; Sha = $sha }
}

function Set-DockerEnvironment {
    <# Set-DockerEnvironment - the values docker/docker-compose.yml reads, for this process only. #>
    # The compose file gives every variable a default, so only the ones that must not default are
    # set here. DATA_URL and DATA_SHA256 are left alone on purpose: the compose defaults are the
    # pinned v20.0 archive and its sha256, the same bytes the native path verifies.
    if ($script:DataZip -ne $DataUrl) {
        if ($script:DataZip -match '^https?://') {
            Die ("Docker mode uses the pinned client-data archive (docker/docker-compose.yml); -DataZip '{0}' is not it. Change DATA_URL and DATA_SHA256 in the compose file to use another archive" -f $script:DataZip)
        }
        # 'none' and local paths are native-only: the container downloads the archive into its data
        # volume on the first boot, so there is nothing on this host to point it at.
        Warn ("-DataZip '{0}' does not apply in Docker mode: the container downloads the pinned v20.0 archive on its first boot" -f $script:DataZip)
    }
    if ($script:DryRun) {
        Note ("would start the acore container with BOTS={0} HORDE_RATIO={1} BIND_IP=0.0.0.0 (the entrypoint renders both into playerbots.conf at every start)" -f $script:Bots, $script:HordeRatio)
        Note 'would pin the published artifact sha256 as a build arg, so a new release re-runs the download layer instead of reusing its cache'
        return
    }
    $pin = Get-DockerArtifactPin
    if ($pin) {
        $env:ARTIFACT_URL = $pin.Url
        $env:ARTIFACT_SHA256 = $pin.Sha
        Log ("artifact pin: {0} {1}" -f $pin.Sha.Substring(0, 12), $pin.Url)
    }
    else {
        # Falls back to what the Dockerfile does by itself: fetch the .sha256 inside the layer. That
        # still verifies the bytes it downloads, but it cannot invalidate an earlier layer, so a
        # release published since the last build stays invisible until the cache is busted.
        Remove-Item Env:\ARTIFACT_URL, Env:\ARTIFACT_SHA256 -ErrorAction SilentlyContinue
        Warn 'could not read the published artifact sha256: the image build will not re-fetch a cached artifact'
        Warn '  if the realm is older than the release, rebuild with: docker compose -f <compose> build --no-cache acore'
    }
    $env:DB_PASS = $script:DbPass
    $env:DB_ROOT_PASS = $script:DockerRootPass
    $env:ACCOUNT = $script:Account
    $env:ACCOUNT_PASS = $script:AccountPass
    $env:BOTS = [string]$script:Bots
    $env:HORDE_RATIO = [string]$script:HordeRatio
    # 0.0.0.0 inside the container: the published ports are what decides whether the realm answers
    # on the LAN, and the firewall rule -LanIp adds is the only thing in front of them.
    $env:BIND_IP = '0.0.0.0'
    $env:BOOT_TIMEOUT = [string]$script:BootTimeout
}

function Get-DockerInspectField {
    <# Get-DockerInspectField -Id ID -Format TEMPLATE - one field of a container, '' on failure. #>
    # A status line reads this, so a failed inspect is an empty answer, not a crash: the text is
    # 'docker inspect' through Invoke-Native (its complaint is on stderr, which must not abort the
    # run) and an exit code of non-zero is reported as "no answer" rather than as a value.
    param(
        [string]$Id,
        [string]$Format
    )
    $text = (Invoke-Native -FilePath 'docker' -Arguments @('inspect', '--format', $Format, $Id)).Trim()
    if ($LASTEXITCODE -ne 0) { return '' }
    return $text
}

function Test-DockerWorldReady {
    <# Test-DockerWorldReady - 'ok', 'down' or 'waiting' for the acore container. #>
    $id = ([string](Invoke-Compose -Arguments @('ps', '-q', 'acore') -Capture -AllowFailure)).Trim()
    if (-not $id) { return 'waiting' }
    $status = Get-DockerInspectField -Id $id -Format '{{.State.Status}}'
    # A container that is still being created or has just been restarted is "waiting", not "down":
    # the entrypoint has not had its say yet.
    if ($status -eq 'exited' -or $status -eq 'dead') { return 'down' }
    $health = Get-DockerInspectField -Id $id -Format '{{if .State.Health}}{{.State.Health.Status}}{{end}}'
    if ($health -eq 'healthy') { return 'ok' }
    # The acore healthcheck is the entrypoint's markers (world up *and* the login port answering),
    # so it is the answer whenever it is there. An image built before that healthcheck has none, and the world's own "World
    # initialized" line never reaches the container's stdout - it goes to the logs volume - so the
    # entrypoint's readiness line is the signal to fall back to.
    $log = Invoke-Compose -Arguments @('logs', '--no-color', '--tail', '400', 'acore') -Capture -AllowFailure
    if ($log -match 'authserver is answering|the world is up|World initialized') { return 'ok' }
    return 'waiting'
}

function Wait-DockerWorld {
    <# Wait-DockerWorld - wait up to -BootTimeout for the world; $false when it did not get there. #>
    param([int]$TimeoutSeconds)
    if ($script:DryRun) { return $true }
    $waited = 0
    while ($waited -lt $TimeoutSeconds) {
        $state = Test-DockerWorldReady
        if ($state -eq 'ok') {
            Log 'the container reports that the world is up'
            return $true
        }
        if ($state -eq 'down') {
            $tail = Invoke-Compose -Arguments @('logs', '--no-color', '--tail', '60', 'acore') -Capture -AllowFailure
            if ($tail) { [Console]::Error.WriteLine($tail) }
            Warn 'the acore container exited before the world came up (its last log lines are above); the volumes are kept, so docker compose up -d retries'
            return $false
        }
        Start-Sleep -Seconds 5
        $waited = $waited + 5
    }
    $tail = Invoke-Compose -Arguments @('logs', '--no-color', '--tail', '60', 'acore') -Capture -AllowFailure
    if ($tail) { [Console]::Error.WriteLine($tail) }
    Warn ("the acore container did not report 'World initialized' within {0}s" -f $TimeoutSeconds)
    return $false
}

function Set-DockerRealmPin {
    <# Set-DockerRealmPin - point realm 1 at -LanIp, through the db container's own client. #>
    if (-not $script:LanIp) { return }
    if ($script:DryRun) {
        Note ("run UPDATE realmlist SET address='{0}' WHERE id=1 inside the db container" -f $script:LanIp)
        return
    }
    $execBase = @(Get-DockerRootExecBase) + @('mysql', '-uroot')
    $sql = "UPDATE realmlist SET address='{0}', localAddress='{0}' WHERE id=1;" -f $script:LanIp
    $argv = @()
    foreach ($item in $execBase) { $argv += $item }
    $argv += @('acore_auth', '-e', $sql)
    [void](Use-DockerRootPassword { Invoke-Compose -Arguments $argv })
    # UPDATE prints nothing, so the row is read back rather than the exit code trusted.
    $argv = @()
    foreach ($item in $execBase) { $argv += $item }
    $argv += @('-N', '-B', 'acore_auth', '-e', ("SELECT COUNT(*) FROM realmlist WHERE id=1 AND address='{0}'" -f $script:LanIp))
    $rows = ([string](Use-DockerRootPassword { Invoke-Compose -Arguments $argv -Capture -AllowFailure })).Trim()
    if ($rows -eq '1') {
        Log ("realm 1 pinned to {0}:{1}" -f $script:LanIp, $WorldPort)
    } else {
        Warn 'could not pin realm 1 (is the auth database populated yet?)'
    }
}

function Invoke-DockerInstall {
    # The state is read first, and quietly: a re-run over an existing container install has to reuse
    # the root password the db volume was initialised with, or the entrypoint cannot log in as root.
    [void](Import-State -Quiet)
    Invoke-Preflight
    Test-DockerPrerequisites
    Resolve-DockerRootPassword
    if (-not $script:DockerRootPass -and -not $script:DryRun) {
        $script:DockerRootPass = New-RandomPassword
    }
    # Written before anything starts, not after the install finishes: the db volume keeps the root
    # password it was initialised with forever, and an install that dies in the first boot used to
    # leave no record of it - every later -Repair then generated a different one, could not log in
    # as root, and reported "Access denied for user 'root'@'172.18.0.3'".
    Save-State
    Resolve-SelfRef
    Install-DockerAssets
    Set-DockerEnvironment
    # The staging directory is this mode's own tree. Recorded like any other path outside the root,
    # so -Uninstall removes it from the manifest and not just because it happens to sit under -Root.
    Add-Manifest $script:DockerDir
    if ($script:DryRun) {
        Note 'set DB_PASS, DB_ROOT_PASS, ACCOUNT, ACCOUNT_PASS, BOTS, HORDE_RATIO, BIND_IP=0.0.0.0 and BOOT_TIMEOUT for the compose project'
    }
    # Written before the containers start, not after: an install that dies in the middle has to be
    # removable by -Uninstall, and the mode file is what makes -Uninstall run compose down instead
    # of the native path (which would find no <root>\db and leave the containers running).
    Save-Mode -Mode 'docker'
    Assert-DockerRootPassword
    Step 'Containers (build and start)'
    [void](Invoke-Compose -Arguments @('up', '-d', '--build'))
    if (-not (Wait-DockerWorld -TimeoutSeconds $script:BootTimeout)) {
        $script:FirstBootOk = $false
    }
    Set-DockerRealmPin
    # After the first boot, because the file the client archive is built from - the server's
    # data/dbc/Item.dbc - only exists once the entrypoint has unpacked the client data. The server
    # half needs nothing from here: the container patches its own copy before the world starts.
    Install-ClientPatch
    # -LanIp opens the same two rules as the native path, through the same helper and the same
    # manifest entries, so -Uninstall removes them whichever mode installed them.
    Enable-Firewall
    Save-State
    Invoke-DockerSummary
}

function Invoke-DockerSummary {
    Step 'Summary'
    if ($script:DryRun) {
        Log 'check mode finished; nothing was changed'
        return
    }
    if (-not $script:FirstBootOk) {
        Warn ("the entrypoint has not reported the world is up yet; watch docker compose -f {0} logs -f acore, or the world's own log at <root>\logs\Server.log" -f (Get-DockerComposeFile))
    }
    $bind = '0.0.0.0 in the container, published on 3724 and 8085'
    if ($script:LanIp) { $bind = ('{0} (realm 1 pinned there; inbound 3724/8085 opened)' -f $script:LanIp) }
    Log ("staged in   : {0}   (docker\ + scripts\, ref {1})" -f $script:DockerDir, $script:SelfRef)
    Log ("compose     : docker compose -f {0}" -f (Get-DockerComposeFile))
    Log 'containers  : acore (world + auth) and db (mysql:8.4); data, logs and db are named volumes'
    Log ("ports       : {0} (authserver) and {1} (worldserver), published by the acore container" -f $AuthPort, $WorldPort)
    Log ("bots        : {0} (BOTS for the entrypoint, Playerbots.Min/MaxRandomBots in the conf it writes)" -f $script:Bots)
    Log ("faction     : {0}/{1} (HORDE_RATIO for the entrypoint, Playerbots.RandomBotAllianceRatio / RandomBotHordeRatio)" -f (100 - $script:HordeRatio), $script:HordeRatio)
    Log ("gm account  : {0} / {1}" -f $script:Account, $script:AccountPass)
    Log ("bind        : {0}" -f $bind)
    Log ''
    Log 'Manage it'
    Log '  inspect : powershell -File windows-install.ps1 -Status      (exit 1 when something needs repair)'
    Log '  repair  : powershell -File windows-install.ps1 -Repair      (re-fetches docker\ and scripts\, then rebuilds)'
    Log '  remove  : powershell -File windows-install.ps1 -Uninstall [-KeepDb]'
    Log '            (this is "down -v": the database, the client data and the logs are volumes, and they go)'
    Log ''
    Log 'Next steps'
    Log '  1. prove it with a PC 3.3.5a client: connect to 127.0.0.1 and log in with the account above.'
    Log '  2. launcher client half: docs/LAUNCHER-HOSTING-DESIGN.md section 3.'
}

function Invoke-DockerStatus {
    Step 'Components'
    # The mode line reports what is on disk, not the mode the arguments asked for: no state file
    # means nothing was installed in this root, and naming a mode there is how a clean machine read
    # as an installed one (the rest of the table then reads like a broken install instead of none).
    if (-not (Test-Path -LiteralPath $script:StateFile)) {
        Log ("mode        : nothing installed in {0} (no {1})" -f $script:Root, $script:StateFile)
    } else {
        $recorded = Get-ModeText
        if (-not $recorded) { $recorded = 'not recorded' }
        Log ("mode        : {0}   (staged in {1}, recorded in {2})" -f $recorded, $script:DockerDir, $script:ModeFile)
    }
    $daemonUp = Test-DockerDaemon
    if ($daemonUp) {
        Ok 'the Docker daemon is reachable'
    } else {
        Bad ("the Docker daemon is not reachable ({0}); start Docker Desktop and re-check" -f $script:DockerDaemonReason)
    }
    # One line for the four staged files: on a clean machine they are all absent, and four "missing"
    # lines say nothing the one line does not. A partly staged directory names what is missing.
    $missing = @()
    foreach ($rel in $DockerFiles) {
        $dest = Join-Path $script:DockerDir ($rel.Replace('/', '\'))
        if (-not (Test-Path -LiteralPath $dest)) { $missing += $rel; continue }
        if ((Get-Item -LiteralPath $dest).Length -le 0) { $missing += $rel }
    }
    if ($missing.Count -eq 0) {
        Ok ("staged files: {0} present in {1}" -f $DockerFiles.Count, $script:DockerDir)
    } else {
        Bad ("staged files: absent ({0} of {1} missing from {2}: {3})" -f $missing.Count, $DockerFiles.Count, $script:DockerDir, ($missing -join ', '))
    }
    # Nothing below can be read without the daemon: compose ps, the container states and the
    # published ports all come from it. The services and the ports are still reported, as down, so
    # the table says which components are missing instead of stopping at the daemon.
    if (-not $daemonUp) {
        Bad 'services: not running (the daemon is unreachable, so compose cannot address them)'
        Bad ("ports: {0} and {1} not published (nothing is listening)" -f $AuthPort, $WorldPort)
        Complete-Checks
        return
    }

    Step 'Containers'
    $table = Invoke-Compose -Arguments @('ps') -Capture -AllowFailure
    if ($table) { Log $table } else { Bad 'docker compose ps returned nothing (the project has not been started)' }
    foreach ($service in @('db', 'acore')) {
        $id = ([string](Invoke-Compose -Arguments @('ps', '-q', $service) -Capture -AllowFailure)).Trim()
        if (-not $id) {
            Bad ("{0}: no container (docker compose up has not run, or removed it)" -f $service)
            continue
        }
        $status = Get-DockerInspectField -Id $id -Format '{{.State.Status}}'
        if ($status -eq 'running') {
            Ok ("{0}: running" -f $service)
        } else {
            Bad ("{0}: {1}" -f $service, $status)
        }
    }

    # "Healthy recently" for the acore service is its own healthcheck - the entrypoint's world and
    # auth markers, removed as each goes away - or, for an image built before it, the entrypoint's
    # readiness lines. The servers' own "World initialized" goes to the logs volume, never to the
    # container's stdout, so it is not what compose logs can show.
    $acoreId = ([string](Invoke-Compose -Arguments @('ps', '-q', 'acore') -Capture -AllowFailure)).Trim()
    if ($acoreId) {
        $health = Get-DockerInspectField -Id $acoreId -Format '{{if .State.Health}}{{.State.Health.Status}}{{end}}'
        $log = Invoke-Compose -Arguments @('logs', '--no-color', '--tail', '400', 'acore') -Capture -AllowFailure
        $worldUp = ($log -match 'authserver is answering|the world is up|World initialized')
        if ($health -eq 'healthy') {
            Ok 'acore: healthy'
        } elseif ($health -eq 'unhealthy') {
            Bad 'acore: unhealthy (docker compose logs acore)'
        } elseif ($worldUp) {
            Ok 'acore: running, and the entrypoint reports the world is up'
        } else {
            Bad 'acore: running, but the entrypoint has not reported the world is up (the first boot may still be importing)'
        }
    }

    foreach ($port in @($AuthPort, $WorldPort)) {
        $mapped = ([string](Invoke-Compose -Arguments @('port', 'acore', ([string]$port)) -Capture -AllowFailure)).Trim()
        if ($mapped -match ('^.+:' + $port + '$')) {
            Ok ("port: {0} published on {1}" -f $port, $mapped)
        } else {
            Bad ("port: {0} is not published by the acore container" -f $port)
        }
    }

    # -Status writes nothing, so the default is used for this one query rather than recorded: without
    # it the read below is a refused login and the status says the realm address is broken.
    if (-not $script:DockerRootPass -and (Test-DockerRootPassword -Password 'acore')) {
        $script:DockerRootPass = 'acore'
        Note 'no recorded db root password; using the compose default for this read-only check'
    }
    $pinArgv = @(Get-DockerRootExecBase) + @('mysql', '-uroot', '-N', '-B', 'acore_auth', '-e', 'SELECT address FROM realmlist WHERE id=1')
    $address = ([string](Use-DockerRootPassword { Invoke-Compose -Arguments $pinArgv -Capture -AllowFailure })).Trim()
    # A refused login is text too, and 'ok realm 1 address: ERROR 1045 ...' reads as a pass.
    if ($address -match '^ERROR \d+') {
        Bad ("realm 1 address: the db refused the login ({0})" -f (Get-BriefText -Text $address))
    } elseif ($address) {
        if ($script:LanIp -and $address -ne $script:LanIp) {
            Bad ("realm 1 address is {0}, state records LAN IP {1}" -f $address, $script:LanIp)
        } else {
            Ok ("realm 1 address: {0}" -f $address)
        }
    } elseif ($script:LanIp) {
        Bad ("realm 1 address is unreadable (state records LAN IP {0})" -f $script:LanIp)
    } else {
        Log '--    realm 1 address: not readable (the db container is not up)'
    }

    Show-ClientPatchStatus

    if ($script:LanIp -and (Have 'Get-NetFirewallRule')) {
        foreach ($port in @($AuthPort, $WorldPort)) {
            $name = Get-FirewallRuleName -Port $port
            $rule = $null
            try {
                $rule = Get-NetFirewallRule -DisplayName $name -ErrorAction SilentlyContinue
            } catch {
                Log "      firewall: cannot read the rules from this session ($($_.Exception.Message))"
                continue
            }
            if ($rule) { Ok ("firewall: inbound TCP {0} allowed" -f $port) } else { Bad ("firewall: no inbound rule for {0} (state records LAN IP {1})" -f $port, $script:LanIp) }
        }
    }
    Complete-Checks
}

function Invoke-DockerRepair {
    [void](Import-State -Quiet)
    Test-DockerPrerequisites
    Resolve-DockerRootPassword
    if (-not $script:DockerRootPass) {
        # Without the recorded password the entrypoint cannot log in as root on the next start, and
        # there is no way to read it back out of the volume. The volumes are still there, so this is
        # a warning rather than a refusal - but it is the one failure a repair cannot fix.
        Warn "no recorded db root password in $script:StateFile; the containers keep the one their volume was initialised with"
        Warn '  if the entrypoint then fails with "Access denied for user ''root''", the two ways out are:'
        Warn ("    docker compose -f {0} down -v   then install again (drops the realm), or" -f (Get-DockerComposeFile))
        Warn '    reset root inside the volume: docker run -d --rm --name acore-reset -v acore-realm_db:/var/lib/mysql mysql:8.4 --skip-grant-tables --skip-networking'
        Warn "    docker exec acore-reset mysql -uroot -e `"FLUSH PRIVILEGES; ALTER USER 'root'@'localhost' IDENTIFIED BY 'acore'; ALTER USER 'root'@'%' IDENTIFIED BY 'acore';`""
        Warn '    docker stop acore-reset   (then run this again)'
    }
    # Re-fetching is cheap (four small text files) and it is how a drifted compose file or a fixed
    # entrypoint comes back without touching the volumes - from the commit the install was pinned to,
    # not from whatever the branch holds now.
    Resolve-SelfRef
    Install-DockerAssets
    Set-DockerEnvironment
    Assert-DockerRootPassword
    Step 'Containers (rebuild and restart)'
    [void](Invoke-Compose -Arguments @('up', '-d', '--build'))
    if (-not (Wait-DockerWorld -TimeoutSeconds $script:BootTimeout)) {
        $script:FirstBootOk = $false
    }
    Set-DockerRealmPin
    Install-ClientPatch
    Save-Mode -Mode 'docker'
    Save-State
    Log ''
    Invoke-DockerStatus
}

function Invoke-DockerUninstall {
    Step 'Removing the containers'
    if ($script:DryRun) {
        Note ("docker compose -f {0} down -v --remove-orphans" -f (Get-DockerComposeFile))
        Note ("remove {0}, its manifest entries, and {1}" -f $script:DockerDir, $script:Root)
        return
    }
    if (-not (Test-DockerDaemon)) {
        # Without the daemon compose cannot stop anything, and deleting the staging directory would
        # leave the containers running with no compose file to stop them by.
        Die 'the Docker daemon is not reachable: start Docker Desktop, then run -Uninstall again (nothing has been removed)'
    }
    if (-not (Test-Path -LiteralPath (Get-DockerComposeFile))) {
        Die ("no {0}: the staged files are gone, so the stack cannot be addressed from here. Re-run the install to stage them again, or remove it with 'docker compose down -v' from a copy of docker/docker-compose.yml (nothing has been removed)" -f (Get-DockerComposeFile))
    }

    if ($script:KeepDb) {
        Step 'Dumping the databases'
        $dumpDir = Join-Path $script:HostState 'dumps'
        [void][System.IO.Directory]::CreateDirectory($dumpDir)
        # mysqldump emits utf8mb4, and PowerShell decodes a native command's stdout with the console
        # output encoding, whose default on Windows is a legacy code page - which would mangle every
        # accented name in the world database. UTF-8 for the length of the dump, then back.
        $savedEncoding = [Console]::OutputEncoding
        [Console]::OutputEncoding = New-Object -TypeName System.Text.UTF8Encoding -ArgumentList $false
        $execBase = @(Get-DockerRootExecBase) + @('mysql', '-uroot')
        $argv = @()
        foreach ($item in $execBase) { $argv += $item }
        $argv += @('-N', '-B', '-e', "SELECT SCHEMA_NAME FROM information_schema.SCHEMATA WHERE SCHEMA_NAME LIKE 'acore%'")
        $schemaText = Use-DockerRootPassword { Invoke-Compose -Arguments $argv -Capture -AllowFailure }
        $schemas = @()
        foreach ($line in @($schemaText -split "`r?`n")) {
            if ($line.Trim()) { $schemas += $line.Trim() }
        }
        if ($schemas.Count -eq 0) {
            Warn 'no acore_* schema could be read from the db container; nothing to dump'
        }
        $stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss')
        foreach ($schema in $schemas) {
            $file = Join-Path $dumpDir ('{0}-{1}.sql' -f $stamp, $schema)
            $argv = @(Get-DockerRootExecBase) + @('mysqldump', '-uroot',
                '--single-transaction', '--routines', '--events', '--databases', $schema)
            $dump = Use-DockerRootPassword { Invoke-Compose -Arguments $argv -Capture -AllowFailure }
            if ($dump) {
                # WriteAllText, not Out-File: the latter goes through PowerShell's encoding and adds
                # a BOM to the first line, which mysql chokes on when the dump is restored.
                [System.IO.File]::WriteAllText($file, ($dump.Replace("`r`n", "`n") -replace "`n", "`r`n") + "`r`n",
                    (New-Object -TypeName System.Text.UTF8Encoding -ArgumentList $false))
                Log ("dumped {0} to {1}" -f $schema, $file)
            } else {
                Warn ("mysqldump produced nothing for {0}; the dump in {1} is not usable" -f $schema, $file)
            }
        }
        Log ("the volumes go with the containers; the dumps above are what survives")
        [Console]::OutputEncoding = $savedEncoding
    }

    Step 'Stopping and deleting the containers and their volumes'
    # down -v is the point of this mode: the database, the client data and the logs are named
    # volumes, and -KeepDb above is the only copy that outlives them. --remove-orphans also takes
    # containers a renamed service left behind.
    [void](Invoke-Compose -Arguments @('down', '-v', '--remove-orphans'))

    Step 'Removing the staged files and the rules'
    foreach ($port in @($AuthPort, $WorldPort)) {
        Disable-FirewallRule -Port $port
    }
    # The manifest is filtered first - the firewall entries above are gone, and the staging line is
    # about to be - so a crash between here and the removal below leaves no stale path behind.
    $remaining = @()
    foreach ($entry in @(Get-ManifestEntries)) {
        if ($entry -like 'firewall:*') { continue }
        if ($entry.TrimEnd('\') -eq $script:DockerDir.TrimEnd('\')) { continue }
        $remaining += $entry
    }
    if (Test-Path -LiteralPath $script:ManifestFile) {
        if ($remaining.Count -eq 0) {
            Remove-Item -LiteralPath $script:ManifestFile -Force -ErrorAction SilentlyContinue
        } else {
            [System.IO.File]::WriteAllLines($script:ManifestFile, $remaining)
        }
    }
    # The staging directory itself, whether or not it was recorded: it is four fetched files and
    # nothing else, and leaving them behind would make the next -Docker install believe they are
    # current.
    if (Test-Path -LiteralPath $script:DockerDir) {
        Remove-Item -LiteralPath $script:DockerDir -Recurse -Force -ErrorAction SilentlyContinue
        if (Test-Path -LiteralPath $script:DockerDir) {
            Warn ("could not remove {0}" -f $script:DockerDir)
        } else {
            Log ("removed {0}" -f $script:DockerDir)
        }
    }

    Step 'Removing the install root'
    if (-not (Test-Path -LiteralPath $script:Root)) {
        Log ("{0} is already gone" -f $script:Root)
        return
    }
    if (-not (Test-SafeRemovePath $script:Root)) {
        Warn ("refusing to remove {0}; remove it by hand" -f $script:Root)
        return
    }
    Remove-Item -LiteralPath $script:Root -Recurse -Force -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $script:Root) {
        Warn ("{0} could not be removed completely; something is still using it" -f $script:Root)
    } else {
        Log ("removed {0}" -f $script:Root)
        if ($script:KeepDb) { Log ("the dumps are in {0}\dumps" -f $script:HostState) }
    }
}

function Invoke-Main {
    if ($Help) {
        Show-Usage
        return
    }
    Resolve-Action
    $script:RootGiven = $script:Bound.ContainsKey('Root')
    Confirm-Arguments
    Reset-Paths
    switch ($script:Action) {
        # -Check is install's dry run: the same phases, each write replaced by a "would:" line. It
        # follows the mode the same way -Status does, so a dry run in a container root prints the
        # compose commands instead of planning a native install.
        'install' { Install-ForMode }
        'check' { Install-ForMode }
        'status' { Invoke-Status }
        'repair' { Invoke-Repair }
        'uninstall' { Invoke-Uninstall }
        default { Die "unknown action: $script:Action" }
    }
}

# Dot-sourcing the file (tests, or a wrapper that wants the helpers) must not install anything;
# the same guard thor-install.sh uses with [ "${BASH_SOURCE[0]}" = "$0" ].
if ($MyInvocation.InvocationName -ne '.') {
    Invoke-Main
}
