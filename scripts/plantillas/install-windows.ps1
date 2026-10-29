# Instala ZFSMgr @VERSION@ en Windows x64: el CLI y, si el paquete la trae, la interfaz.
#
# Con interfaz, lanza el Setup de Inno, que ya instala GUI + CLI + PATH + la tarea de OpenSSH:
# repetir aquí esa lógica sería tener dos instaladores que divergen. Sin interfaz —el paquete
# de solo CLI, o -SoloCli— copia el CLI a las MISMAS rutas que usaría el Setup, para que una
# instalación posterior con interfaz la encuentre y la reemplace.
#
# The local daemon: with a valid OpenZFS it is offered as on Unix, and the installed CLI sets
# it up with `install-daemon-local`, the same scheduled task, firewall rule and TLS that
# `install-daemon` sets up over SSH.
param(
    [switch]$SoloCli,
    [switch]$ConDaemon,
    [switch]$SinDaemon,
    [switch]$Forzar,
    [switch]$Silencioso,
    [switch]$Simular,
    [switch]$Desinstalar,
    [switch]$Ayuda
)
$ErrorActionPreference = 'Stop'
$Version = '@VERSION@'
$Aqui = Split-Path -Parent $MyInvocation.MyCommand.Path
$App = Join-Path $env:ProgramFiles 'ZFSMgr'
$Bin = Join-Path $App 'bin'
$Cli = Join-Path $Bin 'zfsmgr-cli.exe'
# Where install-daemon-local puts the agent (daemonpayload.cpp, windowsBinPath).
$Agente = 'C:\ProgramData\ZFSMgr\agent\zfsmgr-agent.exe'

function Log([string]$m) { Write-Host "[zfsmgr] $m" }
function Muere([string]$m) { Write-Host "[zfsmgr] $m" -ForegroundColor Red; exit 1 }
function Corre([scriptblock]$b, [string]$que) {
    if ($Simular) { Write-Host "  [simulado] $que" } else { & $b }
}
# A question only when someone can answer it: never with -Silencioso or redirected input.
function Pregunta([string]$texto, [bool]$porDefecto) {
    if ($Silencioso -or [Console]::IsInputRedirected) { return $null }
    $r = Read-Host "[zfsmgr] $texto"
    if (-not $r) { return $porDefecto }
    return ($r -match '^[sSyY]')
}

if ($Ayuda) {
    @"
Uso: install-windows.cmd [-SoloCli] [-ConDaemon] [-SinDaemon] [-Forzar] [-Silencioso] [-Simular] [-Desinstalar]

  -SoloCli      Solo la linea de ordenes, aunque el paquete traiga la interfaz
  -ConDaemon    Instala o actualiza el daemon local sin preguntar
  -SinDaemon    No toca el daemon local
  -Forzar       Actualiza el daemon aunque tenga trabajos en curso (se cortan)
  -Silencioso   Sin ventanas del instalador (anade zfsmgr-cli al PATH)
  -Simular      Dice lo que haria, sin tocar nada
  -Desinstalar  Quita ZFSMgr. La configuracion de %APPDATA%\ZFSMgr no se toca
"@
    exit 0
}

if (-not [Environment]::Is64BitOperatingSystem) { Muere 'ZFSMgr necesita Windows de 64 bits.' }

# Administrador: Program Files y el PATH de la maquina lo exigen. Si no lo es, se vuelve a
# lanzar elevado con los mismos argumentos, y se ESPERA a que termine para devolver su codigo.
$esAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $esAdmin -and -not $Simular) {
    Log 'hace falta ser administrador: Windows pedira confirmacion.'
    $args2 = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$($MyInvocation.MyCommand.Path)`"")
    foreach ($p in $PSBoundParameters.Keys) { $args2 += "-$p" }
    $proc = Start-Process powershell.exe -ArgumentList $args2 -Verb RunAs -Wait -PassThru
    exit $proc.ExitCode
}

function Quita-DelPath([string]$dir) {
    $actual = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    $nuevo = ($actual -split ';' | Where-Object { $_ -and ($_.TrimEnd('\') -ne $dir.TrimEnd('\')) }) -join ';'
    if ($nuevo -ne $actual) {
        Corre { [Environment]::SetEnvironmentVariable('Path', $nuevo, 'Machine') } "quitar $dir del PATH"
    }
}

if ($Desinstalar) {
    Log 'desinstalando'
    # The daemon FIRST: the CLI removes it, and the CLI goes next.
    if ((Test-Path $Agente) -and -not $SinDaemon -and (Test-Path $Cli)) {
        $quita = if ($ConDaemon) { $true } else { Pregunta 'Quitar tambien el daemon de esta maquina? [s/N]' $false }
        if ($null -eq $quita) { Log 'el daemon local se deja (sin consola para preguntar; -ConDaemon lo quita)' }
        elseif ($quita) {
            Corre { & $Cli uninstall-daemon-local; if ($LASTEXITCODE -ne 0) { Log 'aviso: el daemon no se pudo quitar del todo' } } "$Cli uninstall-daemon-local"
        }
    }
    # Si lo instalo el Setup, su desinstalador sabe todo lo que puso, accesos directos
    # incluidos. Si fue el CLI suelto, se quita a mano.
    $unins = Get-ChildItem -Path $App -Filter 'unins*.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($unins) {
        Corre { Start-Process $unins.FullName -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait } "$($unins.FullName) /VERYSILENT"
    } elseif (Test-Path $App) {
        Corre { Remove-Item -Recurse -Force $App } "borrar $App"
    }
    Quita-DelPath $Bin
    Log 'hecho. La configuracion de %APPDATA%\ZFSMgr NO se toca.'
    exit 0
}

# Las sumas ANTES de nada. Un ZIP bajado a medias se detiene aqui, no a mitad.
$manifiesto = Join-Path $Aqui 'MANIFIESTO.sha256'
if (-not (Test-Path $manifiesto)) { Muere 'falta MANIFIESTO.sha256: el paquete esta incompleto' }
foreach ($linea in Get-Content $manifiesto) {
    if (-not $linea.Trim()) { continue }
    $suma, $rel = $linea -split '\s+', 2
    $ruta = Join-Path $Aqui ($rel.TrimStart('*') -replace '/', '\')
    if (-not (Test-Path $ruta)) { Muere "el paquete esta incompleto: falta $rel" }
    if ((Get-FileHash -Algorithm SHA256 $ruta).Hash.ToLower() -ne $suma.ToLower()) {
        Muere "el paquete esta danado: no cuadra $rel"
    }
}
Log 'sumas comprobadas'

# La marca de «descargado de Internet». Se hereda del ZIP a todo lo que sale de el, y con
# ella SmartScreen para el Setup y PowerShell se niega a cargar lo que no esta firmado.
if (-not $Simular) { Get-ChildItem -Recurse $Aqui | Unblock-File -ErrorAction SilentlyContinue }

# Jobs in progress: reinstalling restarts the daemon and would cut them. Asked BEFORE copying
# anything, with the new CLI from the package, so nothing is left half updated.
$cliNuevo = Join-Path $Aqui 'cli\zfsmgr-cli-windows-x86_64.exe'
if ((Test-Path $Agente) -and -not $SinDaemon -and -not $Simular -and -not $Forzar) {
    & $cliNuevo install-daemon-local --comprobar
    if ($LASTEXITCODE -eq 3) {
        if ($ConDaemon) { Muere 'no se ha instalado nada. Espere a que terminen o repita con -Forzar.' }
        Log 'el daemon NO se actualiza mientras tenga trabajos en curso (-Forzar lo haria).'
        $SinDaemon = $true
    }
}

$setup = Join-Path $Aqui "gui\windows-x86_64\ZFSMgr-Setup-$Version.exe"
$conGui = (-not $SoloCli) -and (Test-Path $setup)
if ($SoloCli) { Log '-SoloCli: la interfaz no se instala.' }
elseif (-not (Test-Path $setup)) { Log 'este paquete es el de SOLO CLI: no trae la interfaz grafica.' }

if ($conGui) {
    # /TASKS en silencioso: sin ventanas no hay casillas que marcar, y el CLI sin PATH es
    # medio CLI. En modo normal el usuario elige en el propio Setup.
    $argsSetup = if ($Silencioso) { @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/TASKS=addtopath') } else { @() }
    Log "lanzando $setup"
    if ($Simular) {
        Write-Host "  [simulado] $setup $($argsSetup -join ' ')"
    } else {
        $p = Start-Process $setup -ArgumentList $argsSetup -Wait -PassThru
        if ($p.ExitCode -ne 0) { Muere "el Setup termino con codigo $($p.ExitCode)" }
    }
} else {
    $origen = Join-Path $Aqui 'cli\zfsmgr-cli-windows-x86_64.exe'
    if (-not (Test-Path $origen)) { Muere 'el paquete no trae el CLI de Windows' }
    Log "CLI -> $Cli"
    Corre { New-Item -ItemType Directory -Force -Path (Join-Path $Bin 'i18n') | Out-Null } "crear $Bin\i18n"
    Corre { Copy-Item -Force $origen $Cli } "copiar $Cli"
    Corre { Copy-Item -Force (Join-Path $Aqui 'i18n\*.json') (Join-Path $Bin 'i18n') } "copiar los catalogos"
    # Los agentes en {app}\agents, que es bin\..\agents: donde el CLI los busca.
    Corre { Copy-Item -Recurse -Force (Join-Path $Aqui 'agents') $App } "copiar los agentes"
    # Y los CLI de todas las plataformas en {app}\cli\<plataforma>: los que install-daemon
    # manda a otra maquina para ponerle al dia su zfsmgr-cli.
    foreach ($c in Get-ChildItem (Join-Path $Aqui 'cli') -Filter 'zfsmgr-cli-*') {
        $k = $c.BaseName -replace '^zfsmgr-cli-', ''
        $dirK = Join-Path (Join-Path $App 'cli') $k
        Corre { New-Item -ItemType Directory -Force -Path $dirK | Out-Null } "crear $dirK"
        Corre { Copy-Item -Force $c.FullName (Join-Path $dirK ('zfsmgr-cli' + $c.Extension)) } "copiar el CLI de $k"
    }
    $actual = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    if (-not (($actual -split ';') | Where-Object { $_.TrimEnd('\') -eq $Bin.TrimEnd('\') })) {
        Corre { [Environment]::SetEnvironmentVariable('Path', "$actual;$Bin", 'Machine') } "anadir $Bin al PATH"
    }
}

if ($Simular) {
    if (-not $SinDaemon) { Write-Host "  [simulado] $Cli install-daemon-local (si hay OpenZFS)" }
    Log 'simulacro terminado; no se ha tocado nada'; exit 0
}

# Y se COMPRUEBA que arranca: lo unico que demuestra que la instalacion sirve.
if (Test-Path $Cli) {
    $salida = & $Cli version 2>&1
    if ($LASTEXITCODE -ne 0) { Muere "instalado en $Cli, pero NO arranca: $salida" }
    Log "$salida  ->  $Cli"
    Log 'abra una consola NUEVA para que coja el PATH.'
} elseif ($conGui) {
    Log 'el CLI no se instalo: en el Setup no se marco la tarea de linea de ordenes.'
}
# The local daemon, with OpenZFS only: an agent without it answers and sees no pool.
$zfs = Get-Command zfs -ErrorAction SilentlyContinue
if (-not $zfs) {
    foreach ($d in @("$env:ProgramFiles\OpenZFS On Windows\bin", "$env:ProgramFiles\OpenZFS On Windows")) {
        if (Test-Path (Join-Path $d 'zfs.exe')) { $zfs = Join-Path $d 'zfs.exe'; break }
    }
}
if ($SinDaemon) {
    # nothing: asked not to touch it, or it has jobs in progress
} elseif (-not $zfs) {
    if ($ConDaemon) { Log 'aviso: -ConDaemon, pero sin OpenZFS el daemon no se instala.' }
} elseif (-not (Test-Path $Cli)) {
    Log 'el daemon no se instala: no hay CLI instalado que lo haga.'
} else {
    $instala = if ($ConDaemon) { $true }
               elseif (Test-Path $Agente) { Pregunta 'Hay un daemon instalado en esta maquina. Actualizarlo a esta version? [S/n]' $true }
               else { Pregunta 'Esta maquina tiene OpenZFS. Instalar el daemon, para manejarla desde otras? [s/N]' $false }
    if ($null -eq $instala) {
        Log 'para instalar o actualizar el daemon local: -ConDaemon'
    } elseif ($instala) {
        Log 'instalando el daemon local...'
        $argsD = @('install-daemon-local')
        if ($Forzar) { $argsD += '--force' }
        & $Cli @argsD
        if ($LASTEXITCODE -ne 0) { Muere 'el daemon NO se instalo (ver arriba)' }
    }
}
exit 0
