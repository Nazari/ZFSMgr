#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

preferred_downloads_dir() {
  if [[ -d "${HOME}/Descargas" ]]; then
    printf '%s\n' "${HOME}/Descargas"
    return 0
  fi
  if [[ -d "${HOME}/Downloads" ]]; then
    printf '%s\n' "${HOME}/Downloads"
    return 0
  fi
  printf '%s\n' "${HOME}"
}

OUTPUT_DIR="${OUTPUT_DIR:-$(preferred_downloads_dir)/z}"
DOWNLOADS_DIR="${DOWNLOADS_DIR:-${HOME}/Downloads/z}"
DAEMON_DOWNLOADS_DIR="${DOWNLOADS_DIR}/daemons"

LINUX_REMOTE="${LINUX_REMOTE:-linarese@fc16}"
WINDOWS_REMOTE="${WINDOWS_REMOTE:-eladi@surface}"
MAC_REMOTE="${MAC_REMOTE:-linarese@mmela.local}"
MAC_DAEMON_REMOTE="${MAC_DAEMON_REMOTE:-linarese@mbp}"
BUILD_GIT_REMOTE="${BUILD_GIT_REMOTE:-github}"
BUILD_GIT_REF="${BUILD_GIT_REF:-}"
BUILDALL_LOG_DIR="${BUILDALL_LOG_DIR:-}"
BUILD_PLATFORMS="${BUILD_PLATFORMS:-mac,linux,windows}"
DAEMON_OUTPUT_DIR="${PROJECT_ROOT}/build-macos/daemons"

usage() {
  cat <<'EOF'
Uso:
  buildall.sh

Compila y recopila artefactos de macOS, Linux y Windows.

No acepta argumentos posicionales. Se configura por variables de entorno:
  OUTPUT_DIR         Directorio local de salida
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

find_local_artifact() {
  local pattern="$1"
  local type="$2"
  find "${PROJECT_ROOT}" -maxdepth 2 -type "${type}" -name "${pattern}" -print0 \
    | xargs -0 -r ls -td 2>/dev/null \
    | head -n1
}

copy_local_artifact() {
  local source="$1"
  local dest
  dest="${OUTPUT_DIR}/$(basename "${source}")"
  if [[ -e "${dest}" ]]; then
    rm -rf "${dest}"
  fi
  if [[ -d "${source}" ]]; then
    cp -R "${source}" "${dest}"
  else
    cp -f "${source}" "${dest}"
  fi
}

create_macos_dmg() {
  local app_path="$1"
  local app_name dmg_name zip_name staging_dir
  app_name="$(basename "${app_path}")"
  dmg_name="${app_name%.app}.dmg"
  zip_name="${app_name}.zip"
  staging_dir="$(mktemp -d "${TMPDIR:-/tmp}/zfsmgr-dmg.XXXXXX")"
  (
    cp -R "${app_path}" "${staging_dir}/${app_name}"
    ln -s /Applications "${staging_dir}/Applications"
    rm -f "${OUTPUT_DIR}/${dmg_name}" "${OUTPUT_DIR}/${zip_name}"
    hdiutil create \
      -quiet \
      -volname "${app_name%.app}" \
      -srcfolder "${staging_dir}" \
      -format UDZO \
      "${OUTPUT_DIR}/${dmg_name}"
  )
  rm -rf "${staging_dir}"
}

finalize_macos_artifact() {
  local app_path="$1"
  local arch="${2:-}"
  local app_name dmg_name
  app_name="$(basename "${app_path}")"
  dmg_name="${app_name%.app}.dmg"
  create_macos_dmg "${OUTPUT_DIR}/${app_name}"
  rm -rf "${OUTPUT_DIR}/${app_name}"
  local final_dmg="${OUTPUT_DIR}/${dmg_name}"
  if [[ -n "${arch}" ]]; then
    local target="${OUTPUT_DIR}/${app_name%.app}_${arch}.dmg"
    mv "${final_dmg}" "${target}"
    final_dmg="${target}"
  fi
  mkdir -p "${DOWNLOADS_DIR}"
  cp -f "${final_dmg}" "${DOWNLOADS_DIR}/"
  printf '%s\n' "${final_dmg}"
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

copy_linux_remote_artifact() {
  local remote_path="$1"
  local dest="${OUTPUT_DIR}/$(basename "${remote_path}")"
  local quoted_remote
  quoted_remote="$(printf '%q' "${remote_path}")"

  log "Ruta remota Linux resuelta: ${remote_path}"
  if scp -p "${LINUX_REMOTE}:${remote_path}" "${OUTPUT_DIR}/"; then
    return 0
  fi
  log "scp directo falló; reintentando con ruta remota entrecomillada"
  if scp -p "${LINUX_REMOTE}:\"${remote_path}\"" "${OUTPUT_DIR}/"; then
    return 0
  fi
  log "scp volvió a fallar; usando fallback por ssh+cat"
  rm -f "${dest}"
  ssh_linux "cat ${quoted_remote}" > "${dest}"
  chmod +x "${dest}" || true
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

extract_marked_artifact() {
  local marker="$1"
  MARKER="${marker}" perl -ne '
    BEGIN { binmode STDIN; binmode STDOUT; $marker = $ENV{"MARKER"} // ""; }
    if (index($_, $marker) == 0) {
      $value = substr($_, length($marker));
      $value =~ s/\r?\n\z//;
    }
    END { print $value if defined $value && length $value; }
  '
}

windows_to_scp_path() {
  local win_path="$1"
  local drive rest
  if [[ "${win_path}" =~ ^([A-Za-z]):[\\/](.*)$ ]]; then
    drive="${BASH_REMATCH[1]}"
    rest="${BASH_REMATCH[2]}"
    rest="${rest//\\//}"
    printf '/%s:/%s\n' "${drive}" "${rest}"
    return 0
  fi
  fail "Ruta Windows no válida para scp: ${win_path}"
}

copy_mac_remote_artifact() {
  local remote_path="$1"
  local dest="${OUTPUT_DIR}/$(basename "${remote_path}")"
  local quoted_remote
  quoted_remote="$(printf '%q' "${remote_path}")"

  log "Ruta remota macOS resuelta: ${remote_path}"
  if scp -p "${MAC_REMOTE}:${remote_path}" "${OUTPUT_DIR}/"; then
    return 0
  fi
  log "scp directo falló; reintentando con ruta remota entrecomillada"
  if scp -p "${MAC_REMOTE}:\"${remote_path}\"" "${OUTPUT_DIR}/"; then
    return 0
  fi
  log "scp volvió a fallar; usando fallback por tar vía ssh"
  rm -rf "${dest}"
  mkdir -p "${dest}"
  ssh_mac "tar -C $(printf '%q' "$(dirname "${remote_path}")") -cf - $(printf '%q' "$(basename "${remote_path}")")" \
    | tar -C "${OUTPUT_DIR}" -xf -
}

copy_daemon_artifact() {
  local source="$1"
  local tag="$2"
  local dest_dir="${DAEMON_OUTPUT_DIR}/${tag}"
  mkdir -p "${dest_dir}"
  cp -f "${source}" "${dest_dir}/"
  mkdir -p "${DAEMON_DOWNLOADS_DIR}/${tag}"
  cp -f "${source}" "${DAEMON_DOWNLOADS_DIR}/${tag}/"
}

copy_daemon_remote_artifact() {
  local remote_host="$1"
  local remote_path="$2"
  local tag="$3"
  local dest_dir="${DAEMON_OUTPUT_DIR}/${tag}"
  mkdir -p "${dest_dir}"
  scp -p "${remote_host}:${remote_path}" "${dest_dir}/"
  mkdir -p "${DAEMON_DOWNLOADS_DIR}/${tag}"
  scp -p "${remote_host}:${remote_path}" "${DAEMON_DOWNLOADS_DIR}/${tag}/"
}

mkdir -p "${OUTPUT_DIR}"
mkdir -p "${DOWNLOADS_DIR}/tools"
cp -f "${PROJECT_ROOT}/scripts/generate-daemon-keys.sh" "${DOWNLOADS_DIR}/tools/"
cp -f "${PROJECT_ROOT}/scripts/deploy-daemon.sh" "${DOWNLOADS_DIR}/tools/"

if [[ $# -gt 0 ]]; then
  case "$1" in
    *)
      fail "Este script no acepta argumentos posicionales. Usa --help para ver las variables soportadas."
      ;;
  esac
fi

require_cmd ssh
require_cmd scp
require_cmd git
require_cmd base64

log "Directorio destino local: ${OUTPUT_DIR}"
if [[ -n "${BUILD_GIT_REF}" ]]; then
  log "Build remoto fijado al ref git: ${BUILD_GIT_REF} (${BUILD_GIT_REMOTE})"
fi
if [[ -n "${BUILDALL_LOG_DIR}" ]]; then
  log "Logs por plataforma en: ${BUILDALL_LOG_DIR}"
fi
log "Plataformas solicitadas: ${BUILD_PLATFORMS}"

if platform_enabled mac && [[ "$(local_os)" == "Darwin" ]]; then
  log "Compilando macOS en local"
  run_platform_logged macos-local "${SCRIPT_DIR}/build-macos.sh" --bundle --sign
  MAC_ARTIFACT="$(find_local_artifact 'ZFSMgr-*.app' d)"
  [[ -n "${MAC_ARTIFACT}" ]] || fail "No se encontró el .app generado en build-macos"
  copy_local_artifact "${MAC_ARTIFACT}"
  local_daemon_prefix="/opt/homebrew"
  if [[ "$(uname -m)" == "x86_64" ]]; then
    local_daemon_prefix="/usr/local"
  fi
  log "Compilando daemon macOS local en ${PROJECT_ROOT}/build-daemon"
  cmake -S "${PROJECT_ROOT}/daemon" -B "${PROJECT_ROOT}/build-daemon" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="${local_daemon_prefix}"
  cmake --build "${PROJECT_ROOT}/build-daemon" -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
  local_daemon_artifact="${PROJECT_ROOT}/build-daemon/zfsmgr_daemon"
  [[ -f "${local_daemon_artifact}" ]] || fail "No se encontró el daemon macOS local generado"
  copy_daemon_artifact "${local_daemon_artifact}" "macos-$(uname -m)"
  MAC_ARCH_LOCAL="$(uname -m)"
  MAC_DMG="$(finalize_macos_artifact "${MAC_ARTIFACT}" "${MAC_ARCH_LOCAL}")"
  log "Artefacto macOS copiado: $(basename "${MAC_DMG}")"
elif platform_enabled mac; then
  log "Compilando macOS remoto en ${MAC_REMOTE}"
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
find "${repo}/build-macos" -maxdepth 1 -type d -name 'ZFSMgr-*.app' -prune -exec rm -rf {} + 2>/dev/null || true
./scripts/build-macos.sh --bundle --sign
artifact="$(find "${repo}/build-macos" -maxdepth 1 -type d -name 'ZFSMgr-*.app' -print0 | xargs -0 -r ls -td 2>/dev/null | head -n1)"
[[ -n "${artifact}" ]] || { echo "No se encontró el .app generado en macOS." >&2; exit 1; }
arch="$(uname -m)"
printf 'ARCH_MAC=%s\n' "${arch}"
printf 'ARTIFACT_MAC=%s\n' "${artifact}"
EOF
  MAC_BUILD_OUTPUT="$(run_platform_logged macos-remote run_mac_b64_script "${MAC_BUILD_SCRIPT}")"
  MAC_ARTIFACT="$(printf '%s\n' "${MAC_BUILD_OUTPUT}" | extract_marked_artifact 'ARTIFACT_MAC=')"
  [[ -n "${MAC_ARTIFACT}" ]] || fail "No se pudo resolver el artefacto macOS"
  MAC_ARCH="$(printf '%s\n' "${MAC_BUILD_OUTPUT}" | extract_marked_artifact 'ARCH_MAC=')"
  if [[ -z "${MAC_ARCH}" ]]; then
    MAC_ARCH="x86_64"
  fi
  copy_mac_remote_artifact "${MAC_ARTIFACT}"
  MAC_DMG="$(finalize_macos_artifact "${MAC_ARTIFACT}" "${MAC_ARCH}")"
  log "Artefacto macOS copiado: $(basename "${MAC_DMG}")"
else
  log "macOS omitido"
fi

if platform_enabled mac && [[ "$(local_os)" == "Darwin" ]] && [[ -n "${MAC_DAEMON_REMOTE}" ]]; then
  log "Compilando daemon macOS remoto en ${MAC_DAEMON_REMOTE}"
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
printf 'ARCH_MAC_DAEMON=%s\n' "${arch}"
printf 'ARTIFACT_MAC_DAEMON=%s\n' "${artifact}"
EOF
  MAC_DAEMON_OUTPUT="$(run_platform_logged macos-daemon-remote run_mac_daemon_b64_script "${MAC_DAEMON_BUILD_SCRIPT}")"
  MAC_DAEMON_ARCH="$(printf '%s\n' "${MAC_DAEMON_OUTPUT}" | extract_marked_artifact 'ARCH_MAC_DAEMON=')"
  MAC_DAEMON_ARTIFACT="$(printf '%s\n' "${MAC_DAEMON_OUTPUT}" | extract_marked_artifact 'ARTIFACT_MAC_DAEMON=')"
  [[ -n "${MAC_DAEMON_ARCH}" ]] || fail "No se pudo resolver la arquitectura del daemon macOS remoto"
  [[ -n "${MAC_DAEMON_ARTIFACT}" ]] || fail "No se pudo resolver el artefacto daemon macOS remoto"
  copy_daemon_remote_artifact "${MAC_DAEMON_REMOTE}" "${MAC_DAEMON_ARTIFACT}" "macos-${MAC_DAEMON_ARCH}"
  log "Daemon macOS remoto copiado: $(basename "${MAC_DAEMON_ARTIFACT}")"
fi

if platform_enabled linux; then
log "Compilando Linux remoto en ${LINUX_REMOTE}"
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
app_version="$(sed -nE 's/^[[:space:]]*set\(ZFSMGR_APP_VERSION_STRING "([^"]+)"\).*/\1/p' "${repo}/resources/CMakeLists.txt" | head -n1)"
if [[ -z "${app_version}" ]]; then
  app_version="$(sed -nE 's/^[[:space:]]*project\(ZFSMgrQt VERSION ([^ )]+).*/\1/p' "${repo}/resources/CMakeLists.txt" | head -n1)"
fi
[[ -n "${app_version}" ]] || { echo "No se pudo resolver la versión de la AppImage." >&2; exit 1; }
arch="$(uname -m)"
find "${repo}/build-linux" -maxdepth 1 -type f -name 'ZFSMgr-*.AppImage' -delete 2>/dev/null || true
find "${repo}/build-linux" -maxdepth 1 -type f -name 'zfsmgr_*.deb' -delete 2>/dev/null || true
./scripts/build-linux.sh --appimage --deb
artifact_appimage="${repo}/build-linux/ZFSMgr-${app_version}-${arch}.AppImage"
artifact_deb="$(find "${repo}/build-linux" -maxdepth 1 -type f -name "zfsmgr_${app_version}_*.deb" -print0 \
  | xargs -0 -r ls -td 2>/dev/null | head -n1)"
[[ -f "${artifact_appimage}" ]] || { echo "No se encontró el .AppImage generado: ${artifact_appimage}" >&2; exit 1; }
[[ -n "${artifact_deb}" ]] || { echo "No se encontró el .deb generado para la versión ${app_version}." >&2; exit 1; }
[[ -f "${artifact_deb}" ]] || { echo "No se encontró el .deb generado: ${artifact_deb}" >&2; exit 1; }
printf 'ARTIFACT_LINUX_APPIMAGE=%s\n' "${artifact_appimage}"
printf 'ARTIFACT_LINUX_DEB=%s\n' "${artifact_deb}"
EOF
LINUX_BUILD_OUTPUT="$(run_platform_logged linux-remote run_linux_b64_script "${LINUX_BUILD_SCRIPT}")"
LINUX_APPIMAGE_ARTIFACT="$(printf '%s\n' "${LINUX_BUILD_OUTPUT}" | extract_marked_artifact 'ARTIFACT_LINUX_APPIMAGE=')"
LINUX_DEB_ARTIFACT="$(printf '%s\n' "${LINUX_BUILD_OUTPUT}" | extract_marked_artifact 'ARTIFACT_LINUX_DEB=')"
[[ -n "${LINUX_APPIMAGE_ARTIFACT}" ]] || fail "No se pudo resolver el artefacto Linux AppImage"
[[ -n "${LINUX_DEB_ARTIFACT}" ]] || fail "No se pudo resolver el artefacto Linux DEB"
copy_linux_remote_artifact "${LINUX_APPIMAGE_ARTIFACT}"
log "Artefacto Linux copiado: $(basename "${LINUX_APPIMAGE_ARTIFACT}")"
copy_linux_remote_artifact "${LINUX_DEB_ARTIFACT}"
log "Artefacto Linux copiado: $(basename "${LINUX_DEB_ARTIFACT}")"

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
printf 'ARCH_LINUX_DAEMON=%s\n' "${arch}"
printf 'ARTIFACT_LINUX_DAEMON=%s\n' "${artifact}"
EOF
LINUX_DAEMON_OUTPUT="$(run_platform_logged linux-daemon run_linux_b64_script "${LINUX_DAEMON_BUILD_SCRIPT}")"
LINUX_DAEMON_ARCH="$(printf '%s\n' "${LINUX_DAEMON_OUTPUT}" | extract_marked_artifact 'ARCH_LINUX_DAEMON=')"
LINUX_DAEMON_ARTIFACT="$(printf '%s\n' "${LINUX_DAEMON_OUTPUT}" | extract_marked_artifact 'ARTIFACT_LINUX_DAEMON=')"
[[ -n "${LINUX_DAEMON_ARTIFACT}" ]] || fail "No se pudo resolver el artefacto Linux daemon"
[[ -n "${LINUX_DAEMON_ARCH}" ]] || fail "No se pudo resolver la arquitectura del daemon Linux"
copy_daemon_remote_artifact "${LINUX_REMOTE}" "${LINUX_DAEMON_ARTIFACT}" "linux-${LINUX_DAEMON_ARCH}"
log "Daemon Linux copiado: $(basename "${LINUX_DAEMON_ARTIFACT}")"
else
  log "Linux omitido"
fi

if platform_enabled windows; then
log "Compilando Windows remoto en ${WINDOWS_REMOTE}"
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
  $appVersion = $null
  $cmakeFile = Join-Path $repo "resources\CMakeLists.txt"
  if (Test-Path $cmakeFile) {
    $content = Get-Content -Raw $cmakeFile
    $m = [regex]::Match($content, 'set\s*\(\s*ZFSMGR_APP_VERSION_STRING\s+"([^"]+)"')
    if ($m.Success) {
      $appVersion = $m.Groups[1].Value
    }
  }
  if (-not $appVersion) {
    throw "No se pudo resolver la versión de Windows."
  }
  & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repo "scripts\build-windows.ps1") --inno
  $artifact = Get-ChildItem -Path (Join-Path $repo "build-windows\installer") -Filter "ZFSMgr-Setup-$appVersion*.exe" -File -ErrorAction Stop |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1 -ExpandProperty FullName
  if (-not $artifact) {
    throw "No se encontró el .exe generado para la versión $appVersion."
  }
  Write-Output "ARTIFACT_WINDOWS=$artifact"
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
EOF
WINDOWS_ARTIFACT="$(run_platform_logged windows-remote run_windows_b64_ps "${WINDOWS_BUILD_SCRIPT}" | extract_marked_artifact 'ARTIFACT_WINDOWS=')"
[[ -n "${WINDOWS_ARTIFACT}" ]] || fail "No se pudo resolver el artefacto Windows"
WINDOWS_SCP_PATH="$(windows_to_scp_path "${WINDOWS_ARTIFACT}")"
scp -p "${WINDOWS_REMOTE}:${WINDOWS_SCP_PATH}" "${OUTPUT_DIR}/"
log "Artefacto Windows copiado: $(basename "${WINDOWS_ARTIFACT}")"

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
Write-Output "ARCH_WINDOWS_DAEMON=$arch"
Write-Output "ARTIFACT_WINDOWS_DAEMON=$artifact"
EOF
WINDOWS_DAEMON_OUTPUT="$(run_platform_logged windows-daemon run_windows_b64_ps "${WINDOWS_DAEMON_BUILD_SCRIPT}")"
WINDOWS_DAEMON_ARCH="$(printf '%s\n' "${WINDOWS_DAEMON_OUTPUT}" | extract_marked_artifact 'ARCH_WINDOWS_DAEMON=')"
WINDOWS_DAEMON_ARTIFACT="$(printf '%s\n' "${WINDOWS_DAEMON_OUTPUT}" | extract_marked_artifact 'ARTIFACT_WINDOWS_DAEMON=')"
[[ -n "${WINDOWS_DAEMON_ARTIFACT}" ]] || fail "No se pudo resolver el artefacto Windows daemon"
[[ -n "${WINDOWS_DAEMON_ARCH}" ]] || fail "No se pudo resolver la arquitectura del daemon Windows"
WINDOWS_DAEMON_SCP_PATH="$(windows_to_scp_path "${WINDOWS_DAEMON_ARTIFACT}")"
mkdir -p "${DAEMON_OUTPUT_DIR}/windows-${WINDOWS_DAEMON_ARCH}"
scp -p "${WINDOWS_REMOTE}:${WINDOWS_DAEMON_SCP_PATH}" "${DAEMON_OUTPUT_DIR}/windows-${WINDOWS_DAEMON_ARCH}/"
log "Daemon Windows copiado: $(basename "${WINDOWS_DAEMON_ARTIFACT}")"
else
  log "Windows omitido"
fi

log "Artefactos disponibles en ${OUTPUT_DIR}"
ls -1 "${OUTPUT_DIR}"
