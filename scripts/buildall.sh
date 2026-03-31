#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

LINUX_REMOTE="${LINUX_REMOTE:-linarese@fc16}"
WINDOWS_REMOTE="${WINDOWS_REMOTE:-eladi@surface}"
MAC_REMOTE="${MAC_REMOTE:-linarese@mmela.local}"
MAC_DAEMON_REMOTE="${MAC_DAEMON_REMOTE:-linarese@mbp}"
BUILD_GIT_REMOTE="${BUILD_GIT_REMOTE:-github}"
BUILD_GIT_REF="${BUILD_GIT_REF:-}"
BUILDALL_LOG_DIR="${BUILDALL_LOG_DIR:-}"
BUILD_PLATFORMS="${BUILD_PLATFORMS:-mac,linux,windows}"

usage() {
  cat <<'EOF'
Uso:
  buildall.sh

Compila y recopila artefactos de macOS, Linux y Windows.

No acepta argumentos posicionales. Se configura por variables de entorno:
  BUILD_PLATFORMS    Lista separada por comas: mac,linux,windows
  BUILD_GIT_REMOTE   Remoto git desde el que fijar el ref en builders remotos
  BUILD_GIT_REF      Commit/ref exacto a construir en builders remotos
  BUILDALL_LOG_DIR   Directorio de logs por plataforma
  MAC_REMOTE         Host remoto macOS
  MAC_DAEMON_REMOTE  Host remoto macOS x86_64 para el daemon
  LINUX_REMOTE       Host remoto Linux
  WINDOWS_REMOTE     Host remoto Windows

Ejemplos:
  ./scripts/buildall.sh
  BUILD_PLATFORMS=linux,windows ./scripts/buildall.sh
  BUILD_GIT_REF=$(git rev-parse HEAD) ./scripts/buildall.sh
EOF
}

for arg in "$@"; do
  case "${arg}" in
    -h|--help)
      usage
      exit 0
      ;;
  esac
done

log() {
  printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*"
}

platform_enabled() {
  local needle="$1"
  local token
  IFS=',' read -r -a _platform_tokens <<< "${BUILD_PLATFORMS}"
  for token in "${_platform_tokens[@]}"; do
    token="${token//[[:space:]]/}"
    [[ -n "${token}" ]] || continue
    if [[ "${token}" == "${needle}" ]]; then
      return 0
    fi
  done
  return 1
}

run_platform_logged() {
  local phase="$1"
  shift
  if [[ -z "${BUILDALL_LOG_DIR}" ]]; then
    "$@"
    return
  fi
  mkdir -p "${BUILDALL_LOG_DIR}"
  log "Log de ${phase}: ${BUILDALL_LOG_DIR}/${phase}.log"
  "$@" 2>&1 | tee "${BUILDALL_LOG_DIR}/${phase}.log"
}

fail() {
  echo "Error: $*" >&2
  exit 1
}

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || fail "No se encontró el comando requerido: $1"
}

local_os() {
  uname -s
}

ssh_linux() {
  ssh -o BatchMode=yes -o ConnectTimeout=10 "${LINUX_REMOTE}" "$@"
}

ssh_windows() {
  ssh -o BatchMode=yes -o ConnectTimeout=10 "${WINDOWS_REMOTE}" "$@"
}

ssh_mac() {
  ssh -o BatchMode=yes -o ConnectTimeout=10 "${MAC_REMOTE}" "$@"
}

ssh_mac_daemon() {
  ssh -o BatchMode=yes -o ConnectTimeout=10 "${MAC_DAEMON_REMOTE}" "$@"
}


run_linux_b64_script() {
  local script="$1"
  local encoded
  encoded="$(printf '%s' "${script}" | base64 | tr -d '\n')"
  ssh_linux "env BUILD_GIT_REMOTE=$(printf '%q' "${BUILD_GIT_REMOTE}") BUILD_GIT_REF=$(printf '%q' "${BUILD_GIT_REF}") bash -lc \"\$(printf '%s' '${encoded}' | base64 -d)\""
}

run_windows_b64_ps() {
  local script="$1"
  local encoded
  encoded="$(printf '%s' "${script}" | base64 | tr -d '\n')"
  ssh_windows "set \"BUILD_GIT_REMOTE=${BUILD_GIT_REMOTE}\" && set \"BUILD_GIT_REF=${BUILD_GIT_REF}\" && powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"\$s=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('${encoded}')); Invoke-Expression \$s\""
}

run_mac_b64_script() {
  local script="$1"
  local encoded
  encoded="$(printf '%s' "${script}" | base64 | tr -d '\n')"
  ssh_mac "env BUILD_GIT_REMOTE=$(printf '%q' "${BUILD_GIT_REMOTE}") BUILD_GIT_REF=$(printf '%q' "${BUILD_GIT_REF}") bash -lc \"\$(printf '%s' '${encoded}' | base64 -d)\""
}

run_mac_daemon_b64_script() {
  local script="$1"
  local encoded
  encoded="$(printf '%s' "${script}" | base64 | tr -d '\n')"
  ssh_mac_daemon "env BUILD_GIT_REMOTE=$(printf '%q' "${BUILD_GIT_REMOTE}") BUILD_GIT_REF=$(printf '%q' "${BUILD_GIT_REF}") bash -lc \"\$(printf '%s' '${encoded}' | base64 -d)\""
}

if [[ $# -gt 0 ]]; then
  case "$1" in
    *)
      fail "Este script no acepta argumentos posicionales. Usa --help para ver las variables soportadas."
      ;;
  esac
fi

require_cmd ssh
require_cmd git
require_cmd base64

if [[ -n "${BUILD_GIT_REF}" ]]; then
  log "Build remoto fijado al ref git: ${BUILD_GIT_REF} (${BUILD_GIT_REMOTE})"
fi
if [[ -n "${BUILDALL_LOG_DIR}" ]]; then
  log "Logs por plataforma en: ${BUILDALL_LOG_DIR}"
fi
log "Plataformas solicitadas: ${BUILD_PLATFORMS}"

if platform_enabled mac && [[ "$(local_os)" == "Darwin" ]]; then
  log "Compilando macOS en local; el artefacto se subirá a fc16 (si se ejecuta con --sftpfc16)."
  run_platform_logged macos-local "${SCRIPT_DIR}/build-macos.sh" --bundle --sign --sftpfc16
  log "Build macOS local completado y publicado en fc16."
elif platform_enabled mac; then
  log "Compilando macOS remoto en ${MAC_REMOTE}..."
  read -r -d '' MAC_BUILD_SCRIPT <<'EOF' || true
set -euo pipefail
repo=""
for candidate in \
  "$HOME/work/ZFSMgr" \
  "$HOME/Work/ZFSMgr" \
  "/Users/linarese/work/ZFSMgr" \
  "/Users/linarese/Work/ZFSMgr"
do
  if [[ -d "${candidate}/.git" ]]; then
    repo="${candidate}"
    break
  fi
done
[[ -n "${repo}" ]] || { echo "No se encontró el repo ZFSMgr en macOS." >&2; exit 1; }
cd "${repo}"
if [[ -n "${BUILD_GIT_REF:-}" ]]; then
  original_ref="$(git symbolic-ref -q --short HEAD || git rev-parse --short HEAD)"
  original_detached=0
  git symbolic-ref -q HEAD >/dev/null 2>&1 || original_detached=1
  restore_ref() {
    if [[ "${original_detached}" -eq 1 ]]; then
      git checkout --detach "${original_ref}" >/dev/null 2>&1 || true
    else
      git checkout "${original_ref}" >/dev/null 2>&1 || true
    fi
  }
  trap restore_ref EXIT
  git fetch "${BUILD_GIT_REMOTE}" "${BUILD_GIT_REF}"
  git checkout --detach FETCH_HEAD
else
  git pull --ff-only
fi
./scripts/build-macos.sh --bundle --sign --sftpfc16
EOF
  run_platform_logged macos-remote run_mac_b64_script "${MAC_BUILD_SCRIPT}"
  log "Build macOS remoto completado en ${MAC_REMOTE}; el artefacto se subió a fc16."
else
  log "macOS omitido"
fi

if platform_enabled mac && [[ "$(local_os)" == "Darwin" ]] && [[ -n "${MAC_DAEMON_REMOTE}" ]]; then
  log "Compilando daemon macOS remoto en ${MAC_DAEMON_REMOTE}..."
  read -r -d '' MAC_DAEMON_BUILD_SCRIPT <<'EOF' || true
set -euo pipefail
repo=""
for candidate in \
  "$HOME/work/ZFSMgr" \
  "$HOME/Work/ZFSMgr" \
  "/Users/linarese/work/ZFSMgr" \
  "/Users/linarese/Work/ZFSMgr"
do
  if [[ -d "${candidate}/.git" ]]; then
    repo="${candidate}"
    break
  fi
done
[[ -n "${repo}" ]] || { echo "No se encontró el repo ZFSMgr en macOS." >&2; exit 1; }
cd "${repo}"
if [[ -n "${BUILD_GIT_REF:-}" ]]; then
  original_ref="$(git symbolic-ref -q --short HEAD || git rev-parse --short HEAD)"
  original_detached=0
  git symbolic-ref -q HEAD >/dev/null 2>&1 || original_detached=1
  restore_ref() {
    if [[ "${original_detached}" -eq 1 ]]; then
      git checkout --detach "${original_ref}" >/dev/null 2>&1 || true
    else
      git checkout "${original_ref}" >/dev/null 2>&1 || true
    fi
  }
  trap restore_ref EXIT
  git fetch "${BUILD_GIT_REMOTE}" "${BUILD_GIT_REF}"
  git checkout --detach FETCH_HEAD
else
  git pull --ff-only
fi
arch="$(uname -m)"
daemon_build_dir="${repo}/build-daemon"
rm -rf "${daemon_build_dir}"
cmake_args=(-DCMAKE_BUILD_TYPE=Release)
qt_dir=""
brew_bin=""
for candidate in \
  "$(command -v brew 2>/dev/null || true)" \
  "/usr/local/bin/brew" \
  "/opt/homebrew/bin/brew"
do
  if [[ -n "${candidate}" && -x "${candidate}" ]]; then
    brew_bin="${candidate}"
    break
  fi
done
for candidate in \
  "/Users/linarese/Qt/6.10.2/macos/lib/cmake/Qt6" \
  "$("${brew_bin}" --prefix qt 2>/dev/null || true)/lib/cmake/Qt6" \
  "$("${brew_bin}" --prefix qt@6 2>/dev/null || true)/lib/cmake/Qt6"
do
  if [[ -n "${candidate}" && -z "${qt_dir}" && -f "${candidate}/Qt6Config.cmake" ]]; then
    qt_dir="${candidate}"
  fi
done
libssh_prefix=""
if [[ -n "${brew_bin}" ]]; then
  libssh_prefix="$("${brew_bin}" --prefix libssh 2>/dev/null || true)"
fi
if [[ -n "${qt_dir}" ]]; then
  cmake_args+=(-DQt6_DIR="${qt_dir}")
fi
if [[ -n "${libssh_prefix}" ]]; then
  cmake_args+=(-DCMAKE_PREFIX_PATH="${libssh_prefix}")
fi
if [[ ${#cmake_args[@]} -eq 1 ]]; then
  brew_prefix=""
  if [[ -n "${brew_bin}" ]]; then
    brew_prefix="$("${brew_bin}" --prefix 2>/dev/null || true)"
  fi
  if [[ -n "${brew_prefix}" ]]; then
    cmake_args+=(-DCMAKE_PREFIX_PATH="${brew_prefix}")
  fi
fi
cmake -S "${repo}/daemon" -B "${daemon_build_dir}" "${cmake_args[@]}"
cmake --build "${daemon_build_dir}" -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
artifact="${daemon_build_dir}/zfsmgr_daemon"
[[ -f "${artifact}" ]] || { echo "No se encontró el daemon generado en macOS." >&2; exit 1; }
echo "Daemon macOS compilado en ${arch}"
EOF
  run_platform_logged macos-daemon-remote run_mac_daemon_b64_script "${MAC_DAEMON_BUILD_SCRIPT}"
  log "Daemon macOS remoto compilado en ${MAC_DAEMON_REMOTE}."
fi

if platform_enabled linux; then
  log "Compilando Linux remoto en ${LINUX_REMOTE}..."
  read -r -d '' LINUX_BUILD_SCRIPT <<'EOF' || true
set -euo pipefail
repo=""
for candidate in \
  "$HOME/work/ZFSMgr" \
  "$HOME/Work/ZFSMgr" \
  "/home/linarese/work/ZFSMgr" \
  "/home/linarese/Work/ZFSMgr"
do
  if [[ -d "${candidate}/.git" ]]; then
    repo="${candidate}"
    break
  fi
done
[[ -n "${repo}" ]] || { echo "No se encontró el repo ZFSMgr en Linux." >&2; exit 1; }
cd "${repo}"
if [[ -n "${BUILD_GIT_REF:-}" ]]; then
  original_ref="$(git symbolic-ref -q --short HEAD || git rev-parse --short HEAD)"
  original_detached=0
  git symbolic-ref -q HEAD >/dev/null 2>&1 || original_detached=1
  restore_ref() {
    if [[ "${original_detached}" -eq 1 ]]; then
      git checkout --detach "${original_ref}" >/dev/null 2>&1 || true
    else
      git checkout "${original_ref}" >/dev/null 2>&1 || true
    fi
  }
  trap restore_ref EXIT
  git fetch "${BUILD_GIT_REMOTE}" "${BUILD_GIT_REF}"
  git checkout --detach FETCH_HEAD
else
  git pull --ff-only
fi
./scripts/build-linux.sh --appimage --deb --sftpfc16
EOF
  run_platform_logged linux-remote run_linux_b64_script "${LINUX_BUILD_SCRIPT}"
  log "Build Linux remoto completado en ${LINUX_REMOTE}; los artefactos subieron a fc16."

  read -r -d '' LINUX_DAEMON_BUILD_SCRIPT <<'EOF' || true
set -euo pipefail
repo=""
for candidate in \
  "$HOME/work/ZFSMgr" \
  "$HOME/Work/ZFSMgr" \
  "/home/linarese/work/ZFSMgr" \
  "/home/linarese/Work/ZFSMgr"
do
  if [[ -d "${candidate}/.git" ]]; then
    repo="${candidate}"
    break
  fi
done
[[ -n "${repo}" ]] || { echo "No se encontró el repo ZFSMgr en Linux." >&2; exit 1; }
cd "${repo}"
if [[ -n "${BUILD_GIT_REF:-}" ]]; then
  original_ref="$(git symbolic-ref -q --short HEAD || git rev-parse --short HEAD)"
  original_detached=0
  git symbolic-ref -q HEAD >/dev/null 2>&1 || original_detached=1
  restore_ref() {
    if [[ "${original_detached}" -eq 1 ]]; then
      git checkout --detach "${original_ref}" >/dev/null 2>&1 || true
    else
      git checkout "${original_ref}" >/dev/null 2>&1 || true
    fi
  }
  trap restore_ref EXIT
  git fetch "${BUILD_GIT_REMOTE}" "${BUILD_GIT_REF}"
  git checkout --detach FETCH_HEAD
else
  git pull --ff-only
fi
daemon_build_dir="${repo}/build-daemon"
arch="$(uname -m)"
cmake -S "${repo}/daemon" -B "${daemon_build_dir}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${daemon_build_dir}" -j"$(nproc 2>/dev/null || echo 4)"
artifact="${daemon_build_dir}/zfsmgr_daemon"
[[ -f "${artifact}" ]] || { echo "No se encontró el daemon generado en Linux." >&2; exit 1; }
echo "Daemon Linux compilado en ${arch}"
EOF
  run_platform_logged linux-daemon run_linux_b64_script "${LINUX_DAEMON_BUILD_SCRIPT}"
  log "Daemon Linux remoto compilado en ${LINUX_REMOTE}."
else
  log "Linux omitido"
fi

if platform_enabled windows; then
  log "Compilando Windows remoto en ${WINDOWS_REMOTE}..."
  read -r -d '' WINDOWS_BUILD_SCRIPT <<'EOF' || true
$ErrorActionPreference = "Stop"
$candidates = @(
  "$HOME\Work\ZFSMgr",
  "$HOME\work\ZFSMgr",
  "C:\Users\eladi\Work\ZFSMgr",
  "C:\Users\eladi\work\ZFSMgr"
)
$repo = $null
foreach ($candidate in $candidates) {
  if (Test-Path (Join-Path $candidate ".git")) {
    $repo = $candidate
    break
  }
}
if (-not $repo) {
  throw "No se encontró el repo ZFSMgr en Windows."
}
Set-Location $repo
$restoreNeeded = $false
if ($env:BUILD_GIT_REF) {
  $originalDetached = $false
  $originalRef = (git symbolic-ref -q --short HEAD) 2>$null
  if (-not $originalRef) {
    $originalDetached = $true
    $originalRef = (git rev-parse --short HEAD).Trim()
  }
  git fetch $env:BUILD_GIT_REMOTE $env:BUILD_GIT_REF
  git checkout --detach FETCH_HEAD
  if ($LASTEXITCODE -ne 0) { throw "No se pudo hacer checkout de FETCH_HEAD" }
  $restoreNeeded = $true
} else {
  git pull --ff-only
}
try {
  & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repo "scripts\build-windows.ps1") --inno --sftpfc16
} finally {
  if ($restoreNeeded) {
    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    if ($originalDetached) {
      git -c advice.detachedHead=false checkout --detach $originalRef *> $null
    } else {
      git checkout $originalRef *> $null
    }
    $ErrorActionPreference = $previousErrorActionPreference
  }
}
Write-Output "Windows build remoto completado."
EOF
  run_platform_logged windows-remote run_windows_b64_ps "${WINDOWS_BUILD_SCRIPT}"
  log "Windows remoto compilado en ${WINDOWS_REMOTE}; el instalador se subió a fc16."

  read -r -d '' WINDOWS_DAEMON_BUILD_SCRIPT <<'EOF' || true
$ErrorActionPreference = "Stop"
$candidates = @(
  "$HOME\Work\ZFSMgr",
  "$HOME\work\ZFSMgr",
  "C:\Users\eladi\Work\ZFSMgr",
  "C:\Users\eladi\work\ZFSMgr"
)
$repo = $null
foreach ($candidate in $candidates) {
  if (Test-Path (Join-Path $candidate ".git")) {
    $repo = $candidate
    break
  }
}
if (-not $repo) {
  throw "No se encontró el repo ZFSMgr en Windows."
}
Set-Location $repo
if ($env:BUILD_GIT_REF) {
  $originalDetached = $false
  $originalRef = (git symbolic-ref -q --short HEAD) 2>$null
  if (-not $originalRef) {
    $originalDetached = $true
    $originalRef = (git rev-parse --short HEAD).Trim()
  }
  git fetch $env:BUILD_GIT_REMOTE $env:BUILD_GIT_REF
  git checkout --detach FETCH_HEAD
  if ($LASTEXITCODE -ne 0) { throw "No se pudo hacer checkout de FETCH_HEAD" }
} else {
  git pull --ff-only
}
$arch = $env:PROCESSOR_ARCHITECTURE
$daemonBuildDir = Join-Path $repo "build-daemon"
cmake -S (Join-Path $repo "daemon") -B $daemonBuildDir
if ($LASTEXITCODE -ne 0) { throw "No se pudo configurar el daemon." }
cmake --build $daemonBuildDir --config Release
if ($LASTEXITCODE -ne 0) { throw "No se pudo compilar el daemon." }
$artifact = Get-ChildItem -Path $daemonBuildDir -Recurse -Filter "zfsmgr_daemon*.exe" -File -ErrorAction Stop |
  Sort-Object LastWriteTime -Descending |
  Select-Object -First 1 -ExpandProperty FullName
if (-not $artifact) {
  throw "No se encontró el daemon generado para Windows."
}
Write-Output "Daemon Windows compilado."
EOF
  run_platform_logged windows-daemon run_windows_b64_ps "${WINDOWS_DAEMON_BUILD_SCRIPT}"
  log "Daemon Windows remoto compilado en ${WINDOWS_REMOTE}."
else
  log "Windows omitido"
fi
