#!/usr/bin/env pwsh
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir
$BuildDir = Join-Path $ProjectRoot "build-windows"
$SourceDir = Join-Path $ProjectRoot "resources"
$NativeArgs = @()
$GenerateInnoInstaller = $false
$InstallerPreferenceExplicit = $false
$InnoScriptPath = $null
$InnoOutputDir = Join-Path $BuildDir "installer"
$SftpTarget = if ($env:ZFSMGR_SFTP_TARGET) { $env:ZFSMGR_SFTP_TARGET } else { "sftp://linarese@fc16:Descargas/z" }
$DownloadsDir = if ($env:DOWNLOADS_DIR) { $env:DOWNLOADS_DIR } else { Join-Path $env:USERPROFILE "Downloads\z" }
$UploadSftp = $false

function Show-Usage {
  @"
Uso:
  build-windows.ps1 [opciones] [-- <args extra de CMake>]

Opciones:
  --inno, --installer       Genera instalador con Inno Setup
  --no-inno, --no-installer No genera instalador
  --inno-script <ruta>      Usa un script .iss concreto
  --inno-output <dir>       Directorio de salida para el instalador
  --sftpfc16                Sube el instalador .exe generado con --inno (Inno Setup) al destino SFTP configurado
  -h, --help                Muestra esta ayuda

Variables opcionales:
  ZFSMGR_SFTP_TARGET        Destino SFTP para --sftpfc16
  INNO_SETUP_COMPILER       Ruta explícita de ISCC.exe

Ejemplos:
  ./scripts/build-windows.ps1
  ./scripts/build-windows.ps1 --inno
"@
}

for ($i = 0; $i -lt $args.Count; $i++) {
  $arg = $args[$i]
  switch -Regex ($arg) {
    '^(--help|-h)$' {
      Show-Usage
      exit 0
    }
    '^(--inno|--inno-setup|-inno|-inno-setup|--installer|-installer)$' {
      $GenerateInnoInstaller = $true
      $InstallerPreferenceExplicit = $true
      continue
    }
    '^(--sftpfc16|-sftpfc16)$' {
      $UploadSftp = $true
      continue
    }
    '^(--no-inno|-no-inno|--no-installer|-no-installer)$' {
      $GenerateInnoInstaller = $false
      $InstallerPreferenceExplicit = $true
      continue
    }
    '^(--inno-script|-inno-script)$' {
      if ($i + 1 -ge $args.Count) {
        throw "Falta valor para $arg."
      }
      $GenerateInnoInstaller = $true
      $InstallerPreferenceExplicit = $true
      $InnoScriptPath = $args[$i + 1]
      $i++
      continue
    }
    '^(--inno-output|-inno-output)$' {
      if ($i + 1 -ge $args.Count) {
        throw "Falta valor para $arg."
      }
      $GenerateInnoInstaller = $true
      $InstallerPreferenceExplicit = $true
      $InnoOutputDir = $args[$i + 1]
      $i++
      continue
    }
    default {
      $NativeArgs += $arg
      continue
    }
  }
}

if (($env:GITHUB_ACTIONS -eq "true") -and -not $InstallerPreferenceExplicit) {
  $GenerateInnoInstaller = $false
  Write-Host "GitHub Actions detectado: se desactiva la generación de instalador Inno Setup por defecto."
}

if ($UploadSftp -and -not $GenerateInnoInstaller) {
  throw "--sftpfc16 requiere generar el instalador con --inno/--installer."
}

function Resolve-SftpTarget([string]$target) {
  if ([string]::IsNullOrWhiteSpace($target)) {
    throw "Destino SFTP vacío."
  }
  $t = $target.Trim()
  if ($t -match '^sftp://|^sft://') {
    if ($t.StartsWith("sftp://")) {
      $body = $t.Substring(7)
    } else {
      $body = $t.Substring(6)
    }
    $slash = $body.IndexOf('/')
    $authority = $body
    $path = ""
    if ($slash -ge 0) {
      $authority = $body.Substring(0, $slash)
      $path = "/" + $body.Substring($slash + 1)
    }
    $user = $null
    $remoteHost = $null
    if ($authority.Contains("@")) {
      $parts = $authority.Split("@", 2)
      $user = $parts[0]
      $hostPart = $parts[1]
      if ($hostPart.Contains(":")) {
        $hp = $hostPart.Split(":", 2)
        $remoteHost = $hp[0]
        if (-not [string]::IsNullOrWhiteSpace($hp[1])) {
          if ([string]::IsNullOrWhiteSpace($path)) {
            $path = $hp[1]
          } else {
            $pathPart = $path.TrimStart("/")
            $path = "$($hp[1])/$pathPart"
          }
        }
      } else {
        $remoteHost = $hostPart
      }
    } else {
      $user = $env:USERNAME
      if ($authority.Contains(":")) {
        $hp = $authority.Split(":", 2)
        $remoteHost = $hp[0]
        if (-not [string]::IsNullOrWhiteSpace($hp[1])) {
          if ([string]::IsNullOrWhiteSpace($path)) {
            $path = $hp[1]
          } else {
            $pathPart = $path.TrimStart("/")
            $path = "$($hp[1])/$pathPart"
          }
        }
      } else {
        $remoteHost = $authority
      }
    }
    if ([string]::IsNullOrWhiteSpace($path)) {
      throw "Destino SFTP inválido: $target"
    }
    return [PSCustomObject]@{
      Remote = "$user@$remoteHost"
      Path   = $path
      HomeRelative = (-not $path.StartsWith("/"))
    }
  }

  if ($t -match '^(?<remote>[^:]+):(?<path>.+)$') {
    $remote = $Matches['remote']
    if (-not $remote.Contains("@")) {
      $remote = "$($env:USERNAME)@$remote"
    }
    $path = $Matches['path']
    if (-not $path.StartsWith("/")) {
      $path = "/" + $path
    }
    return [PSCustomObject]@{
      Remote = $remote
      Path   = $path
      HomeRelative = (-not $path.StartsWith("/"))
    }
  }

  throw "Destino SFTP inválido: $target"
}

function Upload-ArtifactSftp([string]$artifactPath) {
  if (-not (Test-Path $artifactPath)) {
    throw "No se encontró artefacto para subir: $artifactPath"
  }
  $dst = Resolve-SftpTarget $SftpTarget
  Write-Host "Subiendo artefacto a $($dst.Remote):$($dst.Path)"
  if ($dst.HomeRelative) {
    & ssh -o BatchMode=yes $dst.Remote "mkdir -p `"`$HOME/$($dst.Path)`""
  } else {
    & ssh -o BatchMode=yes $dst.Remote "mkdir -p '$($dst.Path)'"
  }
  if ($LASTEXITCODE -ne 0) {
    throw "No se pudo crear el directorio remoto SFTP."
  }
  if ($dst.HomeRelative) {
    & scp $artifactPath "$($dst.Remote):~/$($dst.Path)/"
  } else {
    & scp $artifactPath "$($dst.Remote):$($dst.Path)/"
  }
  if ($LASTEXITCODE -ne 0) {
    throw "Falló la subida SFTP del artefacto."
  }
}

function Upload-DownloadsDaemons() {
  $daemonRoot = Join-Path $DownloadsDir "daemons"
  if (-not (Test-Path $daemonRoot)) {
    Write-Host "No se encontró ${daemonRoot}; no hay daemons para subir."
    return
  }
  Get-ChildItem -Path $daemonRoot -Recurse -File -Filter 'zfsmgr_daemon*' | ForEach-Object {
    Upload-ArtifactSftp $_.FullName
  }
}

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

function Get-Msys2RootPrefixes {
  $prefixes = [System.Collections.Generic.List[string]]::new()
  if ($env:MSYS2_ROOT -and (Test-Path $env:MSYS2_ROOT)) {
    $prefixes.AddRange(@(
      (Join-Path $env:MSYS2_ROOT "mingw64"),
      (Join-Path $env:MSYS2_ROOT "ucrt64"),
      (Join-Path $env:MSYS2_ROOT "clang64"),
      (Join-Path $env:MSYS2_ROOT "clangarm64"),
      (Join-Path $env:MSYS2_ROOT "clang32")
    ))
  }
  $prefixes.AddRange(@(
    "C:\msys64\mingw64",
    "C:\msys64\ucrt64",
    "C:\msys64\clang64",
    "C:\msys64\clangarm64",
    "C:\msys64\clang32"
  ))
  $msysRoot = "C:\msys64"
  if (Test-Path $msysRoot) {
    $msysCandidates = Get-ChildItem -Path $msysRoot -Directory -ErrorAction SilentlyContinue |
      Where-Object { $_.Name -match '^(mingw|ucrt|clang)' } |
      ForEach-Object { $_.FullName }
    $prefixes.AddRange($msysCandidates)
  }
  return $prefixes | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Select-Object -Unique
}

function Prepend-CmakePrefixPath([string]$prefix) {
  if ([string]::IsNullOrWhiteSpace($prefix) -or -not (Test-Path $prefix)) {
    return
  }
  try {
    $canonical = (Resolve-Path -LiteralPath $prefix).Path
  } catch {
    $canonical = $prefix
  }
  $normalize = { param($value)
    if ([string]::IsNullOrWhiteSpace($value)) { return $null }
    $trimmed = $value.Trim()
    if ($trimmed.EndsWith('\') -or $trimmed.EndsWith('/')) {
      $trimmed = $trimmed.Substring(0, $trimmed.Length - 1)
    }
    return $trimmed.ToLowerInvariant()
  }
  $existing = @()
  if (-not [string]::IsNullOrWhiteSpace($env:CMAKE_PREFIX_PATH)) {
    $existing = $env:CMAKE_PREFIX_PATH -split ';'
  }
  $normalizedEntries = $existing | ForEach-Object { & $normalize $_ } | Where-Object { $_ -ne $null }
  $candidateNormalized = & $normalize $canonical
  if ($candidateNormalized -and ($normalizedEntries -contains $candidateNormalized)) {
    return
  }
  if ([string]::IsNullOrWhiteSpace($env:CMAKE_PREFIX_PATH)) {
    $env:CMAKE_PREFIX_PATH = $canonical
  } else {
    $env:CMAKE_PREFIX_PATH = "${canonical};${env:CMAKE_PREFIX_PATH}"
  }
}

function Find-OpenSslRoot {
  if ($env:OPENSSL_ROOT_DIR -and (Test-Path $env:OPENSSL_ROOT_DIR)) {
    return $env:OPENSSL_ROOT_DIR
  }

  $candidates = Get-Msys2RootPrefixes
  $candidates += @(
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

function Find-LibsshRoot {
  if ($env:LIBSSH_ROOT -and (Test-Path $env:LIBSSH_ROOT)) {
    return $env:LIBSSH_ROOT
  }

  $candidates = Get-Msys2RootPrefixes
  foreach ($root in $candidates) {
    if (-not (Test-Path $root)) {
      continue
    }
    $cmakeDir = Join-Path $root "lib\cmake\libssh"
    $configA = Join-Path $cmakeDir "libsshConfig.cmake"
    $configB = Join-Path $cmakeDir "libssh-config.cmake"
    if (Test-Path $configA -or Test-Path $configB) {
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

function Test-OpenSslHeaderPresent([string]$root) {
  if ([string]::IsNullOrWhiteSpace($root)) {
    return $false
  }
  $inc = Join-Path $root "include\openssl\ssl.h"
  return (Test-Path $inc)
}

function Copy-OpenSslRuntimeDlls([string]$root, [string]$destDir) {
  if ([string]::IsNullOrWhiteSpace($root) -or -not (Test-Path $root)) {
    Write-Host "Aviso: no se pudo copiar OpenSSL runtime; prefijo no válido."
    return
  }
  if ([string]::IsNullOrWhiteSpace($destDir) -or -not (Test-Path $destDir)) {
    throw "Destino inválido para desplegar DLLs OpenSSL: $destDir"
  }

  $searchDirs = @(
    (Join-Path $root "bin"),
    (Join-Path $root "lib"),
    $root
  ) | Select-Object -Unique

  $patterns = @(
    "libcrypto-3-x64.dll",
    "libssl-3-x64.dll",
    "libcrypto-*.dll",
    "libssl-*.dll",
    "libcrypto*.dll",
    "libssl*.dll"
  )

  $copied = New-Object System.Collections.Generic.HashSet[string]
  foreach ($dir in $searchDirs) {
    if (-not (Test-Path $dir)) {
      continue
    }
    foreach ($pattern in $patterns) {
      Get-ChildItem -Path $dir -File -Filter $pattern -ErrorAction SilentlyContinue |
        Sort-Object Name |
        ForEach-Object {
          if ($copied.Add($_.Name)) {
            Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $destDir $_.Name) -Force
            Write-Host "OpenSSL runtime desplegado: $($_.Name)"
          }
        }
    }
  }

  if (-not ($copied.Contains("libcrypto-3-x64.dll")) -and -not ($copied | Where-Object { $_ -like "libcrypto*.dll" })) {
    Write-Host "Aviso: no se encontró ninguna DLL runtime de libcrypto en '$root'."
  }
  if (-not ($copied.Contains("libssl-3-x64.dll")) -and -not ($copied | Where-Object { $_ -like "libssl*.dll" })) {
    Write-Host "Aviso: no se encontró ninguna DLL runtime de libssl en '$root'."
  }
}

function Find-LibsshRoot {
  if ($env:libssh_DIR -and (Test-Path (Join-Path $env:libssh_DIR "libsshConfig.cmake"))) {
    return Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $env:libssh_DIR))
  }

  if ($env:LIBSSH_ROOT_DIR -and (Test-Path $env:LIBSSH_ROOT_DIR)) {
    $cfg = Join-Path $env:LIBSSH_ROOT_DIR "lib\cmake\libssh\libsshConfig.cmake"
    if (Test-Path $cfg) {
      return $env:LIBSSH_ROOT_DIR
    }
  }

  $candidates = @()
  if ($env:MSYS2_ROOT -and (Test-Path $env:MSYS2_ROOT)) {
    $candidates += @(
      (Join-Path $env:MSYS2_ROOT "mingw64"),
      (Join-Path $env:MSYS2_ROOT "ucrt64"),
      (Join-Path $env:MSYS2_ROOT "clang64"),
      (Join-Path $env:MSYS2_ROOT "clangarm64"),
      (Join-Path $env:MSYS2_ROOT "clang32")
    )
  }
  $candidates += @(
    "C:\msys64\mingw64",
    "C:\msys64\ucrt64",
    "C:\msys64\clang64",
    "C:\msys64\clangarm64",
    "C:\msys64\clang32",
    "C:\Program Files\libssh",
    "C:\libssh"
  )

  foreach ($root in ($candidates | Select-Object -Unique)) {
    if (-not (Test-Path $root)) {
      continue
    }
    $cfg = Join-Path $root "lib\cmake\libssh\libsshConfig.cmake"
    $header = Join-Path $root "include\libssh\libssh.h"
    $importLibs = @(
      (Join-Path $root "lib\libssh.dll.a"),
      (Join-Path $root "lib\libssh.a"),
      (Join-Path $root "lib\libssh.lib")
    )
    if ((Test-Path $cfg) -and (Test-Path $header) -and (($importLibs | Where-Object { Test-Path $_ }).Count -gt 0)) {
      return $root
    }
  }

  return $null
}

function Test-LibsshMingwCompatible([string]$root) {
  if ([string]::IsNullOrWhiteSpace($root)) {
    return $false
  }
  $cfg = Join-Path $root "lib\cmake\libssh\libsshConfig.cmake"
  $header = Join-Path $root "include\libssh\libssh.h"
  $a = Join-Path $root "lib\libssh.a"
  $dlla = Join-Path $root "lib\libssh.dll.a"
  return (Test-Path $cfg) -and (Test-Path $header) -and ((Test-Path $a) -or (Test-Path $dlla))
}

function Copy-LibsshRuntimeDlls([string]$root, [string]$destDir) {
  if ([string]::IsNullOrWhiteSpace($root) -or -not (Test-Path $root)) {
    Write-Host "Aviso: no se pudo copiar libssh runtime; prefijo no válido."
    return
  }
  if ([string]::IsNullOrWhiteSpace($destDir) -or -not (Test-Path $destDir)) {
    throw "Destino inválido para desplegar DLLs libssh: $destDir"
  }

  $searchDirs = @(
    (Join-Path $root "bin"),
    (Join-Path $root "lib"),
    $root
  ) | Select-Object -Unique

  $copied = New-Object System.Collections.Generic.HashSet[string]
  foreach ($dir in $searchDirs) {
    if (-not (Test-Path $dir)) {
      continue
    }
    Get-ChildItem -Path $dir -File -Filter "libssh*.dll" -ErrorAction SilentlyContinue |
      Sort-Object Name |
      ForEach-Object {
        if ($copied.Add($_.Name)) {
          Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $destDir $_.Name) -Force
          Write-Host "libssh runtime desplegado: $($_.Name)"
        }
      }
  }

  if (-not ($copied | Where-Object { $_ -like "libssh*.dll" })) {
    Write-Host "Aviso: no se encontró ninguna DLL runtime de libssh en '$root'."
  }
}

function Get-ProjectVersion {
  $cmakeFile = Join-Path $SourceDir "CMakeLists.txt"
  if (-not (Test-Path $cmakeFile)) {
    return "0.0.0"
  }
  $content = Get-Content -Raw $cmakeFile
  $appMatch = [regex]::Match($content, 'set\s*\(\s*ZFSMGR_APP_VERSION_STRING\s+"([^"]+)"')
  if ($appMatch.Success) {
    return $appMatch.Groups[1].Value
  }
  $m = [regex]::Match($content, 'project\s*\(\s*[^)]*VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)')
  if ($m.Success) {
    return $m.Groups[1].Value
  }
  return "0.0.0"
}

function Find-InnoSetupCompiler {
  if ($env:INNO_SETUP_COMPILER -and (Test-Path $env:INNO_SETUP_COMPILER)) {
    return $env:INNO_SETUP_COMPILER
  }

  $onPath = Get-Command "ISCC.exe" -ErrorAction SilentlyContinue
  if ($onPath) {
    return $onPath.Source
  }

  $candidates = @(
    "C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
    "C:\Program Files\Inno Setup 6\ISCC.exe"
  )
  foreach ($candidate in $candidates) {
    if (Test-Path $candidate) {
      return $candidate
    }
  }
  return $null
}

function New-DefaultInnoScript([string]$path, [string]$version, [string]$sourceDir, [string]$outputDir) {
  $iss = @"
[Setup]
AppId={{9A34D91D-B01A-4D0B-9CD9-3DF295C8DDB8}
AppName=ZFSMgr
AppVersion=$version
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog
DefaultDirName={autopf}\ZFSMgr
DefaultGroupName=ZFSMgr
OutputDir=$outputDir
OutputBaseFilename=ZFSMgr-Setup-$version
Compression=lzma
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\zfsmgr_qt.exe

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop icon"; GroupDescription: "Additional icons:"

[Files]
Source: "$sourceDir\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; Excludes: "connections.ini,*.pdb,*.obj,*.o,*.a,*.lib,*.exp,*.ilk,*.idb,*.tmp,*.log,*.tlog,CMakeFiles\*,.qt\*,Testing\*,zfsmgr_*_test.exe,*_autogen\*,*.cpp,*.c,*.h,*.hpp,*.md,*.txt"

[Icons]
Name: "{autoprograms}\ZFSMgr"; Filename: "{app}\zfsmgr_qt.exe"
Name: "{autodesktop}\ZFSMgr"; Filename: "{app}\zfsmgr_qt.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\zfsmgr_qt.exe"; Description: "Launch ZFSMgr"; Flags: nowait postinstall skipifsilent
"@
  Set-Content -Path $path -Value $iss -Encoding ascii
}

function New-InstallerPayload([string]$sourceDir, [string]$payloadDir) {
  if (-not (Test-Path $sourceDir)) {
    throw "No se encontro el directorio de runtime para empaquetar: $sourceDir"
  }

  if (Test-Path $payloadDir) {
    Remove-Item -Recurse -Force $payloadDir
  }
  New-Item -ItemType Directory -Force -Path $payloadDir | Out-Null

  $mainExe = Join-Path $sourceDir "zfsmgr_qt.exe"
  if (-not (Test-Path $mainExe)) {
    throw "No se encontro zfsmgr_qt.exe en: $sourceDir"
  }
  Copy-Item -LiteralPath $mainExe -Destination (Join-Path $payloadDir "zfsmgr_qt.exe") -Force

  $qtConf = Join-Path $sourceDir "qt.conf"
  if (Test-Path $qtConf) {
    Copy-Item -LiteralPath $qtConf -Destination (Join-Path $payloadDir "qt.conf") -Force
  }

  Get-ChildItem -Path $sourceDir -File -Filter "*.dll" -ErrorAction SilentlyContinue |
    ForEach-Object {
      Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $payloadDir $_.Name) -Force
    }

  $runtimeDirs = @(
    "platforms",
    "styles",
    "imageformats",
    "iconengines",
    "generic",
    "networkinformation",
    "tls",
    "translations",
    "sqldrivers",
    "bearer",
    "i18n",
    "help"
  )
  foreach ($dirName in $runtimeDirs) {
    $srcDir = Join-Path $sourceDir $dirName
    if (Test-Path $srcDir) {
      Copy-Item -Path $srcDir -Destination (Join-Path $payloadDir $dirName) -Recurse -Force
    }
  }

  $forbiddenPatterns = @(
    "connections.ini",
    "*.pdb",
    "*.obj",
    "*.o",
    "*.a",
    "*.lib",
    "*.exp",
    "*.ilk",
    "*.idb",
    "*.tmp",
    "*.log",
    "*.tlog",
    "*.cpp",
    "*.c",
    "*.h",
    "*.hpp",
    "*.md",
    "*.txt"
  )
  foreach ($pattern in $forbiddenPatterns) {
    Get-ChildItem -Path $payloadDir -Recurse -File -Filter $pattern -ErrorAction SilentlyContinue |
      Remove-Item -Force
  }
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

# Agregar prefijos de libssh para CMake cuando sea posible.
$libsshDirValid = $false
$existingLibsshDir = $null
if ($env:libssh_DIR) {
  $existingLibsshDir = $env:libssh_DIR
  if (Test-Path $existingLibsshDir) {
    $libsshDirValid = $true
  } else {
    Write-Host "Aviso: libssh_DIR definido en '$existingLibsshDir' no existe."
  }
}

if (-not $libsshDirValid) {
  $libsshRoot = Find-LibsshRoot
  if ($libsshRoot) {
    Prepend-CmakePrefixPath $libsshRoot
    $libsshCmakeDir = Join-Path $libsshRoot "lib\cmake\libssh"
    if (Test-Path $libsshCmakeDir) {
      $env:libssh_DIR = $libsshCmakeDir
      if (-not $env:LIBSSH_ROOT) {
        $env:LIBSSH_ROOT = $libsshRoot
      }
      Write-Host "libssh detectado en: $libsshRoot"
    } else {
      Write-Host "Aviso: libssh encontrado en $libsshRoot pero no se localizó '$libsshCmakeDir'."
    }
  } else {
    Write-Host "Aviso: no se encontró libssh. Instala 'mingw-w64-x86_64-libssh' en MSYS2 o define LIBSSH_ROOT/libssh_DIR."
  }
} else {
  Write-Host "Usando libssh_DIR preexistente: $existingLibsshDir"
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
  cmake -S $SourceDir -B $BuildDir @NativeArgs
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
      $hasHeader = Test-OpenSslHeaderPresent $opensslRoot
      $isMingwOpenSsl = Test-OpenSslMingwCompatible $opensslRoot
      if (-not $hasHeader -or -not $isMingwOpenSsl) {
        throw "OpenSSL detectado en '$opensslRoot' pero no parece un prefijo MinGW valido (esperado: include\openssl\ssl.h y lib\libcrypto.a o lib\libcrypto.dll.a). En MSYS2 instala mingw-w64-x86_64-openssl (o equivalente ucrt/clang), no el paquete base 'openssl'. Tambien puedes fijar OPENSSL_ROOT_DIR al prefijo correcto."
      }
      $NativeArgs += @("-DOPENSSL_ROOT_DIR=$opensslRoot")
      Write-Host "OpenSSL detectado en: $opensslRoot"
    } else {
      throw "No se encontrÃ³ OpenSSL para MinGW. Instala el paquete de toolchain correcto en MSYS2 (por ejemplo mingw-w64-x86_64-openssl en C:\msys64\mingw64) o define OPENSSL_ROOT_DIR al prefijo que contiene include\openssl\ssl.h y lib\libcrypto*.a."
    }

    $libsshRoot = Find-LibsshRoot
    if ($libsshRoot) {
      if (-not (Test-LibsshMingwCompatible $libsshRoot)) {
        throw "libssh detectado en '$libsshRoot' pero no parece un prefijo MinGW valido (esperado: lib\cmake\libssh\libsshConfig.cmake, include\libssh\libssh.h y lib\libssh.a o lib\libssh.dll.a). En MSYS2 instala mingw-w64-x86_64-libssh (o equivalente ucrt/clang) y evita mezclar binarios MSVC con Qt MinGW."
      }
      $NativeArgs += @("-Dlibssh_DIR=$(Join-Path $libsshRoot 'lib\cmake\libssh')")
      Write-Host "libssh detectado en: $libsshRoot"
    } else {
      throw "No se encontrÃ³ libssh para MinGW. Instala el paquete de toolchain correcto en MSYS2 (por ejemplo mingw-w64-x86_64-libssh en C:\msys64\mingw64) o define libssh_DIR al directorio que contiene libsshConfig.cmake."
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
    cmake -S $SourceDir -B $BuildDir @tryArgs
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

$opensslRuntimeRoot = Find-OpenSslRoot
if ($opensslRuntimeRoot) {
  Copy-OpenSslRuntimeDlls -root $opensslRuntimeRoot -destDir (Split-Path -Parent $exePath)
} else {
  Write-Host "Aviso: no se encontró OpenSSL para desplegar sus DLLs runtime."
}

$libsshRuntimeRoot = Find-LibsshRoot
if ($libsshRuntimeRoot) {
  Copy-LibsshRuntimeDlls -root $libsshRuntimeRoot -destDir (Split-Path -Parent $exePath)
} else {
  Write-Host "Aviso: no se encontró libssh para desplegar su DLL runtime."
}

# Safety: never ship local connection secrets in Windows build artifacts.
$exeDir = Split-Path -Parent $exePath
$candidateIni = @(
  (Join-Path $BuildDir "connections.ini"),
  (Join-Path $exeDir "connections.ini")
)
foreach ($ini in $candidateIni) {
  if (Test-Path $ini) {
    Remove-Item -Force $ini
    Write-Host "Excluido del artefacto: $ini"
  }
}

if ($GenerateInnoInstaller) {
  $isccExe = Find-InnoSetupCompiler
  if (-not $isccExe) {
    throw "No se encontro ISCC.exe. Instala Inno Setup 6 o define INNO_SETUP_COMPILER."
  }

  $appVersion = Get-ProjectVersion
  New-Item -ItemType Directory -Force -Path $InnoOutputDir | Out-Null
  Get-ChildItem -Path $InnoOutputDir -Filter "ZFSMgr-Setup-*.exe" -File -ErrorAction SilentlyContinue |
    Remove-Item -Force
  $installerPayloadDir = Join-Path $BuildDir "package-runtime"
  New-InstallerPayload -sourceDir $exeDir -payloadDir $installerPayloadDir

  $issPath = $InnoScriptPath
  if ([string]::IsNullOrWhiteSpace($issPath)) {
    $issPath = Join-Path $BuildDir "zfsmgr-installer.iss"
    New-DefaultInnoScript -path $issPath -version $appVersion -sourceDir $installerPayloadDir -outputDir $InnoOutputDir
  } elseif (-not (Test-Path $issPath)) {
    throw "No se encontro el script de Inno Setup: $issPath"
  }

  Write-Host "Generando instalador con Inno Setup: $issPath"
  & $isccExe "/O$InnoOutputDir" $issPath
  if ($LASTEXITCODE -ne 0) {
    throw "ISCC falló (exit $LASTEXITCODE)"
  }
  Write-Host "Instalador generado en: $InnoOutputDir"
  $installerExe = Get-ChildItem -Path $InnoOutputDir -Filter "ZFSMgr-Setup-$appVersion*.exe" -File -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
  if (-not $installerExe) {
    throw "No se encontró el instalador .exe generado por Inno Setup para la versión $appVersion."
  }
  if ($UploadSftp) {
    Upload-ArtifactSftp $installerExe.FullName
    Upload-DownloadsDaemons
  }
} elseif ($UploadSftp) {
  throw "--sftpfc16 solo puede usarse junto con --inno cuando se sube un instalador."
}

Write-Host "Build completado: $exePath"
