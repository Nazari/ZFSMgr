#!/usr/bin/env pwsh
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir = Join-Path $ScriptDir "build-windows"
$NativeArgs = @($args)
$qtKit = ""

function Import-VsDevEnv {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
  if (-not (Test-Path $vswhere)) {
    return $false
  }
  $vsInstall = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($vsInstall)) {
    return $false
  }
  $vsDevCmd = Join-Path $vsInstall "Common7\Tools\VsDevCmd.bat"
  if (-not (Test-Path $vsDevCmd)) {
    return $false
  }
  $dump = & cmd.exe /c "`"$vsDevCmd`" -arch=x64 -host_arch=x64 >nul && set"
  if ($LASTEXITCODE -ne 0) {
    return $false
  }
  foreach ($line in $dump) {
    $eq = $line.IndexOf("=")
    if ($eq -gt 0) {
      $name = $line.Substring(0, $eq)
      $val = $line.Substring($eq + 1)
      Set-Item -Path "Env:$name" -Value $val
    }
  }
  return $true
}

# Opcional: permitir usar Qt6_DIR/CMAKE_PREFIX_PATH predefinidos.
if (-not $env:Qt6_DIR -and -not $env:CMAKE_PREFIX_PATH) {
  $qtRoots = @("C:\Qt", "C:\QT") | Where-Object { Test-Path $_ }
  $picked = $null
  foreach ($qtRoot in $qtRoots) {
    # 1) Búsqueda rápida por layout típico: <root>\<version>\<kit>\lib\cmake\Qt6
    $qt6Candidates = Get-ChildItem -Path $qtRoot -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending
    $orderedKitPatterns = @("msvc", "clang", "mingw")
    foreach ($kitPattern in $orderedKitPatterns) {
      if ($picked) { break }
      foreach ($verDir in $qt6Candidates) {
        $cmakeCandidates = Get-ChildItem -Path $verDir.FullName -Directory -ErrorAction SilentlyContinue |
          Where-Object { $_.Name -match $kitPattern } |
          ForEach-Object { Join-Path $_.FullName "lib\cmake\Qt6" } |
          Where-Object { Test-Path (Join-Path $_ "Qt6Config.cmake") }
        if ($cmakeCandidates.Count -gt 0) {
          $picked = $cmakeCandidates[0]
          break
        }
      }
    }
    if ($picked) { break }

    # 2) Fallback recursivo: localizar Qt6Config.cmake y tomar su carpeta padre.
    $found = Get-ChildItem -Path $qtRoot -Filter "Qt6Config.cmake" -File -Recurse -ErrorAction SilentlyContinue |
      Select-Object -First 1
    if ($found) {
      $picked = Split-Path -Parent $found.FullName
      break
    }
  }

  if ($picked) {
    $env:Qt6_DIR = $picked
    $qtKit = $env:Qt6_DIR.ToLower()
    Write-Host "Qt6 autodetectado en: $($env:Qt6_DIR)"
  } else {
    Write-Host "Aviso: no se encontró Qt6Config.cmake en C:\Qt ni C:\QT."
    Write-Host "Define Qt6_DIR manualmente, ejemplo:"
    Write-Host '$env:Qt6_DIR = "C:\Qt\6.9.0\msvc2022_64\lib\cmake\Qt6"'
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
  if ([string]::IsNullOrWhiteSpace($qtKit) -and $env:Qt6_DIR) {
    $qtKit = $env:Qt6_DIR.ToLower()
  }

  $isMingwQt = ($qtKit -like "*mingw*")
  $isMsvcQt = ($qtKit -like "*msvc*")

  if ($isMingwQt) {
    $qtKitDir = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $env:Qt6_DIR))
    $qtVersionDir = Split-Path -Parent $qtKitDir
    $qtBaseDir = Split-Path -Parent $qtVersionDir
    $toolsRoot = Join-Path $qtBaseDir "Tools"
    $mingwBin = $null
    if (Test-Path $toolsRoot) {
      $cand = Get-ChildItem -Path $toolsRoot -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match "^mingw" } |
        Sort-Object Name -Descending |
        Select-Object -First 1
      if ($cand) {
        $binPath = Join-Path $cand.FullName "bin"
        if (Test-Path $binPath) {
          $mingwBin = $binPath
        }
      }
    }
    if ($mingwBin) {
      $env:Path = "$mingwBin;$env:Path"
      Write-Host "Entorno MinGW detectado: $mingwBin"
      $gxx = Join-Path $mingwBin "g++.exe"
      $gcc = Join-Path $mingwBin "gcc.exe"
      if (Test-Path $gcc -and Test-Path $gxx) {
        $NativeArgs += @("-DCMAKE_C_COMPILER=$gcc", "-DCMAKE_CXX_COMPILER=$gxx")
      }
    } else {
      Write-Host "Aviso: Qt MinGW detectado pero no se encontró Tools\\mingw*\\bin."
    }
  }

  $devEnvLoaded = $false
  if (-not $isMingwQt) {
    $devEnvLoaded = Import-VsDevEnv
    if ($devEnvLoaded) {
      Write-Host "Entorno MSVC cargado desde VsDevCmd.bat"
    }
  }
  $candidates = @()
  if ($isMingwQt) {
    $candidates += @{ Name = "MinGW Makefiles"; Extra = @() }
    if (Get-Command ninja -ErrorAction SilentlyContinue) {
      $candidates += @{ Name = "Ninja"; Extra = @() }
    }
  } else {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
      $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
      if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($vsPath)) {
        $candidates += @{ Name = "Visual Studio 18 2026"; Extra = @("-A", "x64") }
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
