#!/usr/bin/env pwsh
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir = Join-Path $ScriptDir "build-windows"

# Opcional: permitir usar Qt6_DIR/CMAKE_PREFIX_PATH predefinidos.
if (-not $env:Qt6_DIR -and -not $env:CMAKE_PREFIX_PATH) {
  Write-Host "Aviso: define Qt6_DIR o CMAKE_PREFIX_PATH si CMake no encuentra Qt6."
}

cmake -S $ScriptDir -B $BuildDir -DCMAKE_BUILD_TYPE=Release @args
cmake --build $BuildDir --config Release

Write-Host "Build completado: $BuildDir\\Release\\zfsmgr_qt.exe"
