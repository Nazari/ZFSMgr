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

function Find-OpenSslRoot {
  if ($env:OPENSSL_ROOT_DIR -and (Test-Path $env:OPENSSL_ROOT_DIR)) {
    return $env:OPENSSL_ROOT_DIR
  }
  $candidates = @(
    "C:\msys64\mingw64",
    "C:\msys64\ucrt64",
    "C:\Qt\Tools\OpenSSLv3\Win_x64",
    "C:\QT\Tools\OpenSSLv3\Win_x64",
    "C:\Program Files\OpenSSL-Win64",
    "C:\OpenSSL-Win64"
  )
  foreach ($root in $candidates) {
    if (-not (Test-Path $root)) {
      continue
    }
    $inc = Join-Path $root "include\openssl\ssl.h"
    $libA = Join-Path $root "lib\libcrypto.a"
    $libDllA = Join-Path $root "lib\libcrypto.dll.a"
    $libVc = Join-Path $root "lib\libcrypto.lib"
    $libVcDeep = Get-ChildItem -Path $root -Filter "libcrypto.lib" -File -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    if ((Test-Path $inc) -and ((Test-Path $libA) -or (Test-Path $libDllA) -or (Test-Path $libVc) -or $libVcDeep)) {
      return $root
    }
  }
  return $null
}

function Test-OpenSslMingwCompatible([string]$root) {
  if ([string]::IsNullOrWhiteSpace($root)) {
    return $false
  }
  $a = Join-Path $root "lib\libcrypto.a"
  $dlla = Join-Path $root "lib\libcrypto.dll.a"
  return (Test-Path $a) -or (Test-Path $dlla)
}

# Resolver Qt6_DIR de forma robusta (aunque existan vars de entorno previas).
$qtFromEnvValid = $false
if ($env:Qt6_DIR) {
  $qtFromEnvValid = Test-Path (Join-Path $env:Qt6_DIR "Qt6Config.cmake")
}
if (-not $qtFromEnvValid) {
  $picked = $null

  # 1) Intentar desde entradas de CMAKE_PREFIX_PATH
  if ($env:CMAKE_PREFIX_PATH) {
    $prefixes = $env:CMAKE_PREFIX_PATH -split ';' | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
    foreach ($p in $prefixes) {
      $pp = $p.Trim()
      if (Test-Path (Join-Path $pp "Qt6Config.cmake")) {
        $picked = $pp
        break
      }
      $cand = Join-Path $pp "lib\cmake\Qt6"
      if (Test-Path (Join-Path $cand "Qt6Config.cmake")) {
        $picked = $cand
        break
      }
    }
  }

  # 2) Buscar en C:\Qt y C:\QT
  if (-not $picked) {
    $qtRoots = @("C:\Qt", "C:\QT") | Where-Object { Test-Path $_ }
    foreach ($qtRoot in $qtRoots) {
      # BÃºsqueda rÃ¡pida por layout tÃ­pico: <root>\<version>\<kit>\lib\cmake\Qt6
      $qt6Candidates = Get-ChildItem -Path $qtRoot -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending
      $orderedKitPatterns = @("mingw", "msvc", "clang")
      foreach ($kitPattern in $orderedKitPatterns) {
        if ($picked) { break }
        foreach ($verDir in $qt6Candidates) {
          $cmakeCandidates = @(
            Get-ChildItem -Path $verDir.FullName -Directory -ErrorAction SilentlyContinue |
              Where-Object { $_.Name -match $kitPattern } |
              ForEach-Object { Join-Path $_.FullName "lib\cmake\Qt6" } |
              Where-Object { Test-Path (Join-Path $_ "Qt6Config.cmake") }
          )
          if ($cmakeCandidates.Count -gt 0) {
            $picked = $cmakeCandidates | Select-Object -First 1
            break
          }
        }
      }
      if ($picked) { break }

      # Fallback recursivo: localizar Qt6Config.cmake y tomar su carpeta padre.
      $found = Get-ChildItem -Path $qtRoot -Filter "Qt6Config.cmake" -File -Recurse -ErrorAction SilentlyContinue |
        Select-Object -First 1
      if ($found) {
        $picked = Split-Path -Parent $found.FullName
        break
      }
    }
  }

  if ($picked) {
    # Normalizar y validar ruta final.
    try {
      $picked = (Resolve-Path -LiteralPath $picked).Path
    } catch {
      $picked = $null
    }
    if ($picked -and (Test-Path (Join-Path $picked "Qt6Config.cmake"))) {
      $env:Qt6_DIR = $picked
      Write-Host "Qt6 autodetectado en: $($env:Qt6_DIR)"
    } else {
      Write-Host "Aviso: ruta Qt6 detectada invÃ¡lida."
    }
  } else {
    Write-Host "Aviso: no se encontrÃ³ Qt6Config.cmake en rutas conocidas."
    Write-Host "Define Qt6_DIR manualmente, ejemplo:"
    Write-Host '$env:Qt6_DIR = "C:\Qt\6.10.2\mingw_64\lib\cmake\Qt6"'
  }
}

if ($env:Qt6_DIR) {
  try { $env:Qt6_DIR = (Resolve-Path -LiteralPath $env:Qt6_DIR).Path } catch {}
  if (-not (Test-Path (Join-Path $env:Qt6_DIR "Qt6Config.cmake"))) {
    $fallbackQt = "C:\Qt\6.10.2\mingw_64\lib\cmake\Qt6"
    if (Test-Path (Join-Path $fallbackQt "Qt6Config.cmake")) {
      $env:Qt6_DIR = $fallbackQt
      Write-Host "Usando fallback Qt6_DIR: $($env:Qt6_DIR)"
    }
  }
  $qtKit = $env:Qt6_DIR.ToLower()
  if (-not (($NativeArgs | Where-Object { $_ -like "-DQt6_DIR=*" }).Count -gt 0)) {
    $NativeArgs += @("-DQt6_DIR=$($env:Qt6_DIR)")
  }
}

# Si el usuario ya pasÃ³ -G/--generator, no forzamos uno.
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
      if ((Test-Path $gcc) -and (Test-Path $gxx)) {
        $NativeArgs += @("-DCMAKE_C_COMPILER=$gcc", "-DCMAKE_CXX_COMPILER=$gxx")
      }
    } else {
      Write-Host "Aviso: Qt MinGW detectado pero no se encontrÃ³ Tools\\mingw*\\bin."
    }

    $opensslRoot = Find-OpenSslRoot
    if ($opensslRoot) {
      if (-not (Test-OpenSslMingwCompatible $opensslRoot)) {
        throw "OpenSSL detectado en '$opensslRoot' pero parece ser MSVC/no-MinGW. Para Qt mingw instala OpenSSL de MSYS2 (p.ej. C:\msys64\mingw64) y/o define OPENSSL_ROOT_DIR."
      }
      $NativeArgs += @("-DOPENSSL_ROOT_DIR=$opensslRoot")
      Write-Host "OpenSSL detectado en: $opensslRoot"
    } else {
      throw "No se encontrÃ³ OpenSSL para MinGW. Instala MSYS2 OpenSSL (p.ej. C:\msys64\mingw64) o define OPENSSL_ROOT_DIR."
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
$exePath = $null
if (Test-Path $exeRelease) {
  $exePath = $exeRelease
} elseif (Test-Path $exeSingle) {
  $exePath = $exeSingle
} else {
  throw "Compilación finalizada, pero no se encontró zfsmgr_qt.exe en $BuildDir."
}

# Despliegue de runtime Qt junto al ejecutable para evitar errores por DLLs faltantes.
$qtBinFromDir = $null
if ($env:Qt6_DIR) {
  try {
    $qtBinFromDir = Join-Path (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $env:Qt6_DIR))) "bin"
  } catch {}
}

$windeployCandidates = @()
if ($qtBinFromDir) {
  $windeployCandidates += (Join-Path $qtBinFromDir "windeployqt.exe")
}
$windeployCandidates += @(
  "C:\Qt\6.10.2\mingw_64\bin\windeployqt.exe",
  "C:\Qt\6.10.2\msvc2022_64\bin\windeployqt.exe",
  "C:\QT\6.10.2\mingw_64\bin\windeployqt.exe",
  "C:\QT\6.10.2\msvc2022_64\bin\windeployqt.exe"
)

$windeployExe = $windeployCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($windeployExe) {
  Write-Host "Ejecutando windeployqt: $windeployExe"
  & $windeployExe --release --compiler-runtime $exePath
  if ($LASTEXITCODE -ne 0) {
    throw "windeployqt falló (exit $LASTEXITCODE)"
  }
  Write-Host "Runtime Qt desplegado en: $(Split-Path -Parent $exePath)"
} else {
  Write-Host "Aviso: no se encontró windeployqt.exe; el ejecutable podría fallar por DLLs Qt faltantes."
}

Write-Host "Build completado: $exePath"
