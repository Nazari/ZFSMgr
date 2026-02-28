#!/usr/bin/env pwsh
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir = Join-Path $ScriptDir "build-windows"
$NativeArgs = @($args)

# Opcional: permitir usar Qt6_DIR/CMAKE_PREFIX_PATH predefinidos.
if (-not $env:Qt6_DIR -and -not $env:CMAKE_PREFIX_PATH) {
  $qtRoot = "C:\QT"
  if (Test-Path $qtRoot) {
    $qt6Candidates = Get-ChildItem -Path $qtRoot -Directory -ErrorAction SilentlyContinue |
      Sort-Object Name -Descending
    $picked = $null
    foreach ($verDir in $qt6Candidates) {
      $cmakeCandidates = Get-ChildItem -Path $verDir.FullName -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match "msvc|mingw|clang" } |
        ForEach-Object { Join-Path $_.FullName "lib\cmake\Qt6" } |
        Where-Object { Test-Path $_ }
      if ($cmakeCandidates.Count -gt 0) {
        $picked = $cmakeCandidates[0]
        break
      }
    }
    if ($picked) {
      $env:Qt6_DIR = $picked
      Write-Host "Qt6 autodetectado en: $($env:Qt6_DIR)"
    } else {
      Write-Host "Aviso: no se encontró Qt6 en C:\QT (ruta esperada: <version>\\<kit>\\lib\\cmake\\Qt6)."
    }
  } else {
    Write-Host "Aviso: define Qt6_DIR o CMAKE_PREFIX_PATH si CMake no encuentra Qt6."
  }
}

# Si el usuario ya pasó -G/--generator, no forzamos uno.
$hasGenerator = $false
for ($i = 0; $i -lt $NativeArgs.Count; $i++) {
  if ($NativeArgs[$i] -eq "-G" -or $NativeArgs[$i] -eq "--generator") {
    $hasGenerator = $true
    break
  }
}

if ($hasGenerator) {
  cmake -S $ScriptDir -B $BuildDir @NativeArgs
  if ($LASTEXITCODE -ne 0) {
    throw "Fallo en configuracion CMake (exit $LASTEXITCODE)"
  }
} else {
  $candidates = @()
  $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
  if (Test-Path $vswhere) {
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($vsPath)) {
      $candidates += @{ Name = "Visual Studio 17 2022"; Extra = @("-A", "x64") }
    }
  }
  if (Get-Command ninja -ErrorAction SilentlyContinue) {
    $candidates += @{ Name = "Ninja"; Extra = @() }
  }
  if (Get-Command nmake -ErrorAction SilentlyContinue) {
    $candidates += @{ Name = "NMake Makefiles"; Extra = @() }
  }
  if (Get-Command mingw32-make -ErrorAction SilentlyContinue) {
    $candidates += @{ Name = "MinGW Makefiles"; Extra = @() }
  }

  if ($candidates.Count -eq 0) {
    throw "No se encontro generador valido. Instala Visual Studio Build Tools (C++), Ninja o MinGW."
  }

  $configured = $false
  foreach ($cand in $candidates) {
    Write-Host "Intentando generador: $($cand.Name)"
    if (Test-Path $BuildDir) {
      Remove-Item -Recurse -Force $BuildDir
    }
    New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
    $tryArgs = @($NativeArgs) + @("-G", $cand.Name) + $cand.Extra
    cmake -S $ScriptDir -B $BuildDir @tryArgs
    if ($LASTEXITCODE -eq 0) {
      $configured = $true
      break
    }
    Write-Host "Fallo con generador $($cand.Name), probando siguiente..."
  }

  if (-not $configured) {
    throw "No se pudo configurar CMake con ningun generador disponible."
  }
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
