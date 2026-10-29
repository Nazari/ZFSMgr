# Installs ZFSMgr @VERSION@ on Windows x64: the CLI, the GUI if the package has it, and the
# local daemon when OpenZFS is there.
#
# With the GUI, it runs the Inno Setup, which already installs GUI + CLI + PATH + the OpenSSH
# task: repeating that logic here would make two installers that drift apart. Without it —the
# CLI-only package, or -CliOnly— it copies the CLI to the SAME paths the Setup would use, so a
# later installation with the GUI finds it and replaces it.
#
# The local daemon is offered as on Unix, and the installed CLI sets it up with
# `install-daemon-local`: the same scheduled task, firewall rule and TLS that `install-daemon`
# sets up over SSH.
#
# Messages in the system's language: Spanish when the UI culture is, English otherwise. Plain
# quotes and no accents: «» and accents depend on the console code page and came out as «?»
# over SSH.
param(
    [switch]$CliOnly,
    [switch]$WithDaemon,
    [switch]$NoDaemon,
    [switch]$Force,
    [switch]$Quiet,
    [switch]$DryRun,
    [switch]$Uninstall,
    [switch]$Help
)
$ErrorActionPreference = 'Stop'
$Version = '@VERSION@'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$App = Join-Path $env:ProgramFiles 'ZFSMgr'
$Bin = Join-Path $App 'bin'
$Cli = Join-Path $Bin 'zfsmgr-cli.exe'
# Where install-daemon-local puts the agent (daemonpayload.cpp, windowsBinPath).
$Agent = 'C:\ProgramData\ZFSMgr\agent\zfsmgr-agent.exe'
$Lang = if ((Get-UICulture).TwoLetterISOLanguageName -eq 'es') { 'es' } else { 'en' }

# T <spanish> <english>: the text in the system's language.
function T([string]$es, [string]$en) { if ($Lang -eq 'es') { $es } else { $en } }
function Log([string]$m) { Write-Host "[zfsmgr] $m" }
function Die([string]$m) { Write-Host "[zfsmgr] $m" -ForegroundColor Red; exit 1 }
function Run([scriptblock]$b, [string]$what) {
    if ($DryRun) { Write-Host "  [dry-run] $what" } else { & $b }
}
# A question only when someone can answer it: never with -Quiet or redirected input. $null
# means it could not be asked.
function Ask([string]$es, [string]$en, [bool]$default) {
    if ($Quiet -or [Console]::IsInputRedirected) { return $null }
    $hint = if ($default) { T '[S/n]' '[Y/n]' } else { T '[s/N]' '[y/N]' }
    $r = Read-Host "[zfsmgr] $(T $es $en) $hint"
    if (-not $r) { return $default }
    return ($r -match '^[sSyY]')
}

if ($Help) {
    if ($Lang -eq 'es') {
        @"
Uso: install-windows.cmd [-CliOnly] [-WithDaemon] [-NoDaemon] [-Force] [-Quiet] [-DryRun] [-Uninstall]

  -CliOnly      Solo la linea de ordenes, aunque el paquete traiga la interfaz
  -WithDaemon   Instala o actualiza el daemon local sin preguntar
  -NoDaemon     No toca el daemon local
  -Force        Actualiza el daemon aunque tenga trabajos en curso (se cortan)
  -Quiet        Sin preguntas ni ventanas del instalador (anade zfsmgr-cli al PATH)
  -DryRun       Dice lo que haria, sin tocar nada
  -Uninstall    Quita ZFSMgr. La configuracion de %APPDATA%\ZFSMgr no se toca
"@
    } else {
        @"
Usage: install-windows.cmd [-CliOnly] [-WithDaemon] [-NoDaemon] [-Force] [-Quiet] [-DryRun] [-Uninstall]

  -CliOnly      Only the command line, even if the package has the GUI
  -WithDaemon   Install or update the local daemon without asking
  -NoDaemon     Leave the local daemon alone
  -Force        Update the daemon even with jobs in progress (they are cut)
  -Quiet        No questions and no installer windows (adds zfsmgr-cli to the PATH)
  -DryRun       Say what would be done, touching nothing
  -Uninstall    Remove ZFSMgr. The configuration in %APPDATA%\ZFSMgr is not touched
"@
    }
    exit 0
}

if (-not [Environment]::Is64BitOperatingSystem) { Die (T 'ZFSMgr necesita Windows de 64 bits.' 'ZFSMgr needs 64-bit Windows.') }

# Administrator: Program Files, the machine PATH and the daemon's task demand it. If not, it
# relaunches itself elevated with the same arguments, and WAITS for it to return its code.
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin -and -not $DryRun) {
    Log (T 'hace falta ser administrador: Windows pedira confirmacion.' 'administrator rights are needed: Windows will ask for confirmation.')
    $args2 = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$($MyInvocation.MyCommand.Path)`"")
    foreach ($p in $PSBoundParameters.Keys) { $args2 += "-$p" }
    $proc = Start-Process powershell.exe -ArgumentList $args2 -Verb RunAs -Wait -PassThru
    exit $proc.ExitCode
}

function Remove-FromPath([string]$dir) {
    $current = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    $new = ($current -split ';' | Where-Object { $_ -and ($_.TrimEnd('\') -ne $dir.TrimEnd('\')) }) -join ';'
    if ($new -ne $current) {
        Run { [Environment]::SetEnvironmentVariable('Path', $new, 'Machine') } "remove $dir from the PATH"
    }
}

if ($Uninstall) {
    Log (T 'desinstalando' 'uninstalling')
    # The daemon FIRST: the CLI removes it, and the CLI goes next.
    if ((Test-Path $Agent) -and -not $NoDaemon -and (Test-Path $Cli)) {
        $remove = if ($WithDaemon) { $true } else { Ask 'Quitar tambien el daemon de esta maquina?' "Remove this machine's daemon too?" $false }
        if ($null -eq $remove) {
            Log (T 'el daemon local se deja (sin consola para preguntar; -WithDaemon lo quita)' 'the local daemon is left (no console to ask; -WithDaemon removes it)')
        } elseif ($remove) {
            Run {
                & $Cli --lang $Lang uninstall-daemon-local
                if ($LASTEXITCODE -ne 0) { Log (T 'aviso: el daemon no se pudo quitar del todo' 'warning: the daemon could not be fully removed') }
            } "$Cli uninstall-daemon-local"
        }
    }
    # Installed by the Setup, its uninstaller knows all it put there, shortcuts included. The
    # standalone CLI is removed by hand.
    $unins = Get-ChildItem -Path $App -Filter 'unins*.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($unins) {
        Run { Start-Process $unins.FullName -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait } "$($unins.FullName) /VERYSILENT"
    } elseif (Test-Path $App) {
        Run { Remove-Item -Recurse -Force $App } "delete $App"
    }
    Remove-FromPath $Bin
    Log (T 'hecho. La configuracion de %APPDATA%\ZFSMgr NO se toca.' 'done. The configuration in %APPDATA%\ZFSMgr is NOT touched.')
    exit 0
}

# Checksums BEFORE anything. A ZIP downloaded halfway stops here, not halfway through.
$manifest = Join-Path $Here 'MANIFIESTO.sha256'
if (-not (Test-Path $manifest)) { Die (T 'falta MANIFIESTO.sha256: el paquete esta incompleto' 'MANIFIESTO.sha256 is missing: the package is incomplete') }
foreach ($line in Get-Content $manifest) {
    if (-not $line.Trim()) { continue }
    $sum, $rel = $line -split '\s+', 2
    $path = Join-Path $Here ($rel.TrimStart('*') -replace '/', '\')
    if (-not (Test-Path $path)) { Die (T "el paquete esta incompleto: falta $rel" "the package is incomplete: $rel is missing") }
    if ((Get-FileHash -Algorithm SHA256 $path).Hash.ToLower() -ne $sum.ToLower()) {
        Die (T "el paquete esta danado: no cuadra $rel" "the package is damaged: $rel does not match")
    }
}
Log (T 'sumas comprobadas' 'checksums verified')

# The «downloaded from the Internet» mark. Everything taken out of the ZIP inherits it, and
# with it SmartScreen stops the Setup and PowerShell refuses to load what is not signed.
if (-not $DryRun) { Get-ChildItem -Recurse $Here | Unblock-File -ErrorAction SilentlyContinue }

# Jobs in progress: reinstalling restarts the daemon and would cut them. Asked BEFORE copying
# anything, with the new CLI from the package, so nothing is left half updated.
$newCli = Join-Path $Here 'cli\zfsmgr-cli-windows-x86_64.exe'
if ((Test-Path $Agent) -and -not $NoDaemon -and -not $DryRun -and -not $Force) {
    & $newCli --lang $Lang install-daemon-local --check-jobs
    if ($LASTEXITCODE -eq 3) {
        if ($WithDaemon) { Die (T 'no se ha instalado nada. Espere a que terminen o repita con -Force.' 'nothing was installed. Wait for them to end, or repeat with -Force.') }
        Log (T 'el daemon NO se actualiza mientras tenga trabajos en curso (-Force lo haria).' 'the daemon is NOT updated while it has jobs in progress (-Force would).')
        $NoDaemon = $true
    }
}

$setup = Join-Path $Here "gui\windows-x86_64\ZFSMgr-Setup-$Version.exe"
$withGui = (-not $CliOnly) -and (Test-Path $setup)
if ($CliOnly) { Log (T '-CliOnly: la interfaz no se instala.' '-CliOnly: the GUI is not installed.') }
elseif (-not (Test-Path $setup)) { Log (T 'este paquete es el de SOLO CLI: no trae la interfaz grafica.' 'this is the CLI-ONLY package: it has no graphical interface.') }

if ($withGui) {
    # /TASKS when quiet: without windows there are no boxes to tick, and a CLI without PATH is
    # half a CLI. Otherwise the user picks in the Setup itself.
    $setupArgs = if ($Quiet) { @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/TASKS=addtopath') } else { @() }
    Log "$(T 'lanzando' 'running') $setup"
    if ($DryRun) {
        Write-Host "  [dry-run] $setup $($setupArgs -join ' ')"
    } else {
        $p = Start-Process $setup -ArgumentList $setupArgs -Wait -PassThru
        if ($p.ExitCode -ne 0) { Die (T "el Setup termino con codigo $($p.ExitCode)" "the Setup ended with code $($p.ExitCode)") }
    }
} else {
    $source = Join-Path $Here 'cli\zfsmgr-cli-windows-x86_64.exe'
    if (-not (Test-Path $source)) { Die (T 'el paquete no trae el CLI de Windows' 'the package has no Windows CLI') }
    Log "CLI -> $Cli"
    Run { New-Item -ItemType Directory -Force -Path (Join-Path $Bin 'i18n') | Out-Null } "mkdir $Bin\i18n"
    Run { Copy-Item -Force $source $Cli } "copy $Cli"
    Run { Copy-Item -Force (Join-Path $Here 'i18n\*.json') (Join-Path $Bin 'i18n') } "copy the catalogues"
    # The agents in {app}\agents, which is bin\..\agents: where the CLI looks for them.
    Run { Copy-Item -Recurse -Force (Join-Path $Here 'agents') $App } "copy the agents"
    # And the CLIs of every platform in {app}\cli\<platform>: the ones install-daemon sends to
    # another machine to update its zfsmgr-cli.
    foreach ($c in Get-ChildItem (Join-Path $Here 'cli') -Filter 'zfsmgr-cli-*') {
        $k = $c.BaseName -replace '^zfsmgr-cli-', ''
        $dirK = Join-Path (Join-Path $App 'cli') $k
        Run { New-Item -ItemType Directory -Force -Path $dirK | Out-Null } "mkdir $dirK"
        Run { Copy-Item -Force $c.FullName (Join-Path $dirK ('zfsmgr-cli' + $c.Extension)) } "copy the $k CLI"
    }
    $current = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    if (-not (($current -split ';') | Where-Object { $_.TrimEnd('\') -eq $Bin.TrimEnd('\') })) {
        Run { [Environment]::SetEnvironmentVariable('Path', "$current;$Bin", 'Machine') } "add $Bin to the PATH"
    }
}

if ($DryRun) {
    if (-not $NoDaemon) { Write-Host "  [dry-run] $Cli install-daemon-local ($(T 'si hay OpenZFS' 'if OpenZFS is there'))" }
    Log (T 'simulacro terminado; no se ha tocado nada' 'dry run finished; nothing was touched')
    exit 0
}

# And it is CHECKED that it starts: the only proof that the installation works.
if (Test-Path $Cli) {
    $out = & $Cli version 2>&1
    if ($LASTEXITCODE -ne 0) { Die (T "instalado en $Cli, pero NO arranca: $out" "installed at $Cli, but it does NOT start: $out") }
    Log "$out  ->  $Cli"
    Log (T 'abra una consola NUEVA para que coja el PATH.' 'open a NEW console for it to pick up the PATH.')
} elseif ($withGui) {
    Log (T 'el CLI no se instalo: en el Setup no se marco la tarea de linea de ordenes.' 'the CLI was not installed: the command line task was not ticked in the Setup.')
}

# The local daemon, with OpenZFS only: an agent without it answers and sees no pool.
$zfs = Get-Command zfs -ErrorAction SilentlyContinue
if (-not $zfs) {
    foreach ($d in @("$env:ProgramFiles\OpenZFS On Windows\bin", "$env:ProgramFiles\OpenZFS On Windows")) {
        if (Test-Path (Join-Path $d 'zfs.exe')) { $zfs = Join-Path $d 'zfs.exe'; break }
    }
}
if ($NoDaemon) {
    # nothing: asked not to touch it, or it has jobs in progress
} elseif (-not $zfs) {
    if ($WithDaemon) { Log (T 'aviso: -WithDaemon, pero sin OpenZFS el daemon no se instala.' 'warning: -WithDaemon, but without OpenZFS the daemon is not installed.') }
} elseif (-not (Test-Path $Cli)) {
    Log (T 'el daemon no se instala: no hay CLI instalado que lo haga.' 'the daemon is not installed: there is no installed CLI to do it.')
} else {
    $install = if ($WithDaemon) { $true }
               elseif (Test-Path $Agent) { Ask 'Hay un daemon instalado en esta maquina. Actualizarlo a esta version?' 'A daemon is installed on this machine. Update it to this version?' $true }
               else { Ask 'Esta maquina tiene OpenZFS. Instalar el daemon, para manejarla desde otras?' 'This machine has OpenZFS. Install the daemon, to manage it from others?' $false }
    if ($null -eq $install) {
        Log (T 'para instalar o actualizar el daemon local: -WithDaemon' 'to install or update the local daemon: -WithDaemon')
    } elseif ($install) {
        Log (T 'instalando el daemon local...' 'installing the local daemon...')
        $argsD = @('--lang', $Lang, 'install-daemon-local')
        if ($Force) { $argsD += '--force' }
        & $Cli @argsD
        if ($LASTEXITCODE -ne 0) { Die (T 'el daemon NO se instalo (ver arriba)' 'the daemon was NOT installed (see above)') }
    }
}
exit 0
