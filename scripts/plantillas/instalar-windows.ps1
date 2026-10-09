# Instala ZFSMgr @VERSION@ en Windows x64: el CLI y, si el paquete la trae, la interfaz.
#
# Con interfaz, lanza el Setup de Inno, que ya instala GUI + CLI + PATH + la tarea de OpenSSH:
# repetir aquí esa lógica sería tener dos instaladores que divergen. Sin interfaz —el paquete
# de solo CLI, o -SoloCli— copia el CLI a las MISMAS rutas que usaría el Setup, para que una
# instalación posterior con interfaz la encuentre y la reemplace.
#
# El daemon local no se ofrece: en Windows no hay instalación local de servicio, y el agente
# se instala con «install-daemon» desde otra máquina, por SSH.
param(
    [switch]$SoloCli,
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

function Log([string]$m) { Write-Host "[zfsmgr] $m" }
function Muere([string]$m) { Write-Host "[zfsmgr] $m" -ForegroundColor Red; exit 1 }
function Corre([scriptblock]$b, [string]$que) {
    if ($Simular) { Write-Host "  [simulado] $que" } else { & $b }
}

if ($Ayuda) {
    @"
Uso: instalar-windows.cmd [-SoloCli] [-Silencioso] [-Simular] [-Desinstalar]

  -SoloCli      Solo la linea de ordenes, aunque el paquete traiga la interfaz
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
    $actual = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    if (-not (($actual -split ';') | Where-Object { $_.TrimEnd('\') -eq $Bin.TrimEnd('\') })) {
        Corre { [Environment]::SetEnvironmentVariable('Path', "$actual;$Bin", 'Machine') } "anadir $Bin al PATH"
    }
}

if ($Simular) { Log 'simulacro terminado; no se ha tocado nada'; exit 0 }

# Y se COMPRUEBA que arranca: lo unico que demuestra que la instalacion sirve.
if (Test-Path $Cli) {
    $salida = & $Cli version 2>&1
    if ($LASTEXITCODE -ne 0) { Muere "instalado en $Cli, pero NO arranca: $salida" }
    Log "$salida  ->  $Cli"
    Log 'abra una consola NUEVA para que coja el PATH.'
} elseif ($conGui) {
    Log 'el CLI no se instalo: en el Setup no se marco la tarea de linea de ordenes.'
}
# Comillas rectas: las «» dependen de la página de códigos de la consola y por SSH
# salían como «�».
Log 'el daemon de esta maquina se instala desde otra, con "install-daemon" (necesita OpenSSH).'
exit 0
