param(
    [string]$InstallDir = "$env:LOCALAPPDATA\ZFSMgr"
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$exe = Join-Path $root "dist\ZFSMgr.exe"

if (!(Test-Path $exe)) {
    Write-Error "No se encuentra ejecutable en $exe. Ejecuta primero: py packaging/build.py"
}

New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
$target = Join-Path $InstallDir "ZFSMgr.exe"
Copy-Item -Force $exe $target
Write-Host "Instalado en $target"
Write-Host "Opcional: agrega $InstallDir al PATH de usuario."
