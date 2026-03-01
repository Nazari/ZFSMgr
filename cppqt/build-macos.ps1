#!/usr/bin/env pwsh
param(
  [switch]$Bundle,
  [Parameter(ValueFromRemainingArguments = $true)]
  [string[]]$CMakeArgs
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir = Join-Path $ScriptDir "build-macos"

if (Test-Path "/opt/homebrew/opt/qt@6") {
  $qtPrefix = "/opt/homebrew/opt/qt@6"
} elseif (Test-Path "/usr/local/opt/qt@6") {
  $qtPrefix = "/usr/local/opt/qt@6"
} else {
  $qtPrefix = $null
}

if ($qtPrefix) {
  $env:PATH = "$qtPrefix/bin:$env:PATH"
  if ($env:CMAKE_PREFIX_PATH) {
    $env:CMAKE_PREFIX_PATH = "$qtPrefix:$env:CMAKE_PREFIX_PATH"
  } else {
    $env:CMAKE_PREFIX_PATH = $qtPrefix
  }
}

if (Test-Path "/opt/homebrew/opt/openssl@3") {
  $opensslPrefix = "/opt/homebrew/opt/openssl@3"
} elseif (Test-Path "/usr/local/opt/openssl@3") {
  $opensslPrefix = "/usr/local/opt/openssl@3"
} else {
  $opensslPrefix = $null
}

if ($opensslPrefix) {
  if ($env:CMAKE_PREFIX_PATH) {
    $env:CMAKE_PREFIX_PATH = "$opensslPrefix:$env:CMAKE_PREFIX_PATH"
  } else {
    $env:CMAKE_PREFIX_PATH = $opensslPrefix
  }
}

& cmake -S $ScriptDir -B $BuildDir -DCMAKE_BUILD_TYPE=Release @CMakeArgs
if ($LASTEXITCODE -ne 0) { throw "Fallo en configuración CMake (exit $LASTEXITCODE)" }

$jobs = 4
try {
  $cpuOut = & sysctl -n hw.ncpu
  if ($LASTEXITCODE -eq 0 -and $cpuOut) {
    $parsed = 0
    if ([int]::TryParse(($cpuOut | Select-Object -First 1), [ref]$parsed) -and $parsed -gt 0) {
      $jobs = $parsed
    }
  }
} catch { }

& cmake --build $BuildDir -j$jobs
if ($LASTEXITCODE -ne 0) { throw "Fallo en compilación (exit $LASTEXITCODE)" }

Write-Host "Build completado: $BuildDir/zfsmgr_qt"

if ($Bundle) {
  $appBundle = Join-Path $BuildDir "zfsmgr_qt.app"
  if (!(Test-Path $appBundle)) {
    throw "No se ha generado $appBundle"
  }
  $macdeployqt = Get-Command macdeployqt -ErrorAction SilentlyContinue
  if ($macdeployqt) {
    & $macdeployqt.Source $appBundle -always-overwrite
    if ($LASTEXITCODE -ne 0) { throw "Fallo macdeployqt (exit $LASTEXITCODE)" }
  } else {
    Write-Warning "macdeployqt no encontrado; el .app puede no ser portable."
  }
  & /usr/bin/codesign --remove-signature $appBundle *> $null
  Write-Host "App macOS creada (sin firmar): $appBundle"
} else {
  Write-Host "Empaquetado .app omitido (usa -Bundle para generarlo)."
}
