#!/usr/bin/env pwsh
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir = Join-Path $ScriptDir "build-windows"
$NativeArgs = @args

# Opcional: permitir usar Qt6_DIR/CMAKE_PREFIX_PATH predefinidos.
if (-not $env:Qt6_DIR -and -not $env:CMAKE_PREFIX_PATH) {
  Write-Host "Aviso: define Qt6_DIR o CMAKE_PREFIX_PATH si CMake no encuentra Qt6."
}

# Si el usuario ya pasó -G/--generator, no forzamos uno.
$hasGenerator = $false
for ($i = 0; $i -lt $NativeArgs.Count; $i++) {
  if ($NativeArgs[$i] -eq "-G" -or $NativeArgs[$i] -eq "--generator") {
    $hasGenerator = $true
    break
  }
}

if (-not $hasGenerator) {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
  if (Test-Path $vswhere) {
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($vsPath)) {
      $NativeArgs += @("-G", "Visual Studio 17 2022", "-A", "x64")
      Write-Host "Generador seleccionado: Visual Studio 17 2022 (x64)"
    }
  }

  if (-not $hasGenerator -and -not ($NativeArgs -contains "-G") -and (Get-Command ninja -ErrorAction SilentlyContinue)) {
    $NativeArgs += @("-G", "Ninja")
    Write-Host "Generador seleccionado: Ninja"
  }

  if (-not $hasGenerator -and -not ($NativeArgs -contains "-G") -and (Get-Command nmake -ErrorAction SilentlyContinue)) {
    $NativeArgs += @("-G", "NMake Makefiles")
    Write-Host "Generador seleccionado: NMake Makefiles"
  }
}

if (-not ($NativeArgs -contains "-G")) {
  throw "No se encontró generador válido. Instala Visual Studio Build Tools (C++) o Ninja, o ejecuta desde 'x64 Native Tools Command Prompt'."
}

cmake -S $ScriptDir -B $BuildDir @NativeArgs
if ($LASTEXITCODE -ne 0) {
  throw "Fallo en configuración CMake (exit $LASTEXITCODE)"
}

cmake --build $BuildDir --config Release
if ($LASTEXITCODE -ne 0) {
  throw "Fallo en compilación (exit $LASTEXITCODE)"
}

$exeRelease = Join-Path $BuildDir "Release\zfsmgr_qt.exe"
$exeSingle = Join-Path $BuildDir "zfsmgr_qt.exe"
if (Test-Path $exeRelease) {
  Write-Host "Build completado: $exeRelease"
} elseif (Test-Path $exeSingle) {
  Write-Host "Build completado: $exeSingle"
} else {
  throw "Compilación finalizada, pero no se encontró zfsmgr_qt.exe en $BuildDir."
}
