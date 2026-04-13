#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SOURCE_DIR="${PROJECT_ROOT}/resources"
TS="$(date '+%Y%m%d-%H%M%S')"
OUTPUT_DIR="${OUTPUT_DIR:-${PROJECT_ROOT}/builds/artifacts/${TS}}"
LOG_DIR="${BUILDALL_LOG_DIR:-${PROJECT_ROOT}/builds/logs/buildall-cross-${TS}}"
PLATFORMS="${BUILD_PLATFORMS:-linux,windows,freebsd,macos}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
MACOS_ARCHES="${MACOS_ARCHES:-amd64,arm64}"
WINDOWS_INSTALLER="${WINDOWS_INSTALLER:-1}"

usage() {
  cat <<'EOF'
Uso:
  buildall-cross.sh [--platforms linux,windows,freebsd,macos] [--macos-arches amd64,arm64] [--windows-installer 0|1] [--output-dir <dir>] [--log-dir <dir>] [--jobs <n>]

Descripción:
  Ejecuta builds locales en fc16 y deja artefactos en OUTPUT_DIR.
  - Linux nativo: .AppImage + .deb
  - Windows cross: zfsmgr_qt.exe (+ instalador Inno opcional en Linux)
  - FreeBSD cross: paquete .tar.gz con zfsmgr_qt
  - macOS cross: .app.zip (una por arquitectura en MACOS_ARCHES)

Variables opcionales:
  OUTPUT_DIR      Directorio destino de artefactos (default: builds/artifacts/<timestamp>)
  BUILDALL_LOG_DIR Directorio de logs por plataforma
  BUILD_PLATFORMS Lista separada por comas
  MACOS_ARCHES    Lista separada por comas (default: amd64,arm64)
  WINDOWS_INSTALLER  1=genera setup Inno por Wine, 0=solo exe (default: 1)
  JOBS            Paralelismo para build-cross
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --platforms)
      shift
      [[ $# -gt 0 ]] || { echo "Falta valor para --platforms" >&2; exit 1; }
      PLATFORMS="$1"
      shift
      ;;
    --output-dir)
      shift
      [[ $# -gt 0 ]] || { echo "Falta valor para --output-dir" >&2; exit 1; }
      OUTPUT_DIR="$1"
      shift
      ;;
    --macos-arches)
      shift
      [[ $# -gt 0 ]] || { echo "Falta valor para --macos-arches" >&2; exit 1; }
      MACOS_ARCHES="$1"
      shift
      ;;
    --log-dir)
      shift
      [[ $# -gt 0 ]] || { echo "Falta valor para --log-dir" >&2; exit 1; }
      LOG_DIR="$1"
      shift
      ;;
    --jobs)
      shift
      [[ $# -gt 0 ]] || { echo "Falta valor para --jobs" >&2; exit 1; }
      JOBS="$1"
      shift
      ;;
    --windows-installer)
      shift
      [[ $# -gt 0 ]] || { echo "Falta valor para --windows-installer" >&2; exit 1; }
      WINDOWS_INSTALLER="$1"
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Opción desconocida: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

mkdir -p "${OUTPUT_DIR}" "${LOG_DIR}"
OUTPUT_DIR="$(cd "${OUTPUT_DIR}" && pwd)"
LOG_DIR="$(cd "${LOG_DIR}" && pwd)"

version_from_cmake() {
  sed -nE 's/^[[:space:]]*set\([[:space:]]*ZFSMGR_APP_VERSION_STRING[[:space:]]*"([^"]+)".*/\1/p' "${SOURCE_DIR}/CMakeLists.txt" | head -n1
}

APP_VERSION="$(version_from_cmake)"
[[ -n "${APP_VERSION}" ]] || APP_VERSION="0.10.0rc1"

log() {
  printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*"
}

run_phase() {
  local name="$1"
  shift
  local logfile="${LOG_DIR}/${name}.log"
  log "Iniciando ${name} (log: ${logfile})"
  "$@" >"${logfile}" 2>&1
  log "Completado ${name}"
}

has_platform() {
  local needle="$1"
  [[ ",${PLATFORMS}," == *",${needle},"* ]]
}

find_osxcross_target() {
  local arch="$1"
  local pattern=""
  local candidates
  case "${arch}" in
    amd64) pattern='^x86_64-apple-darwin' ;;
    arm64) pattern='^(arm64|aarch64)-apple-darwin' ;;
    *) return 1 ;;
  esac
  candidates="$(ls -1 /opt/osxcross/target/bin/*-apple-darwin*-clang 2>/dev/null | sed -E 's|.*/([^/]+)-clang$|\1|' | grep -E "${pattern}" | sort -V || true)"
  [[ -n "${candidates}" ]] || return 1
  printf '%s\n' "${candidates}" | tail -n1
}

ensure_macos_openssl() {
  local arch="$1"
  local osxcross_target="$2"
  local sdk
  sdk="$(ls -d /opt/osxcross/target/SDK/MacOSX*.sdk 2>/dev/null | sort -V | tail -n1 || true)"
  [[ -n "${sdk}" ]] || { echo "No se encontró SDK de macOS en /opt/osxcross/target/SDK" >&2; return 1; }
  local openssl_prefix=""
  local openssl_target=""
  case "${arch}" in
    amd64)
      openssl_prefix="${HOME}/opt/openssl-macos-x86_64"
      openssl_target="darwin64-x86_64-cc"
      ;;
    arm64)
      openssl_prefix="${HOME}/opt/openssl-macos-arm64"
      openssl_target="darwin64-arm64-cc"
      ;;
    *)
      return 1
      ;;
  esac

  if [[ -f "${openssl_prefix}/include/openssl/evp.h" && ( -f "${openssl_prefix}/lib/libcrypto.a" || -f "${openssl_prefix}/lib/libcrypto.dylib" ) ]]; then
    printf '%s\n' "${openssl_prefix}"
    return 0
  fi

  local work="/tmp/openssl-macos-${arch}-build"
  mkdir -p "${work}" "$(dirname "${openssl_prefix}")"
  rm -rf "${work:?}"/*
  curl -fL -o "${work}/openssl.tar.gz" "https://www.openssl.org/source/openssl-3.3.1.tar.gz" >&2
  tar -xf "${work}/openssl.tar.gz" -C "${work}" >&2
  (
    cd "${work}/openssl-3.3.1"
    export PATH="/opt/osxcross/target/bin:${PATH}"
    export SDKROOT="${sdk}"
    export CFLAGS="-isysroot ${sdk} -mmacosx-version-min=10.15"
    export LDFLAGS="-isysroot ${sdk} -mmacosx-version-min=10.15"
    ./Configure "${openssl_target}" --cross-compile-prefix="${osxcross_target}-" --prefix="${openssl_prefix}" --libdir=lib no-tests no-shared >&2
    make -j"${JOBS}" >&2
    make install_sw >&2
  ) >&2
  printf '%s\n' "${openssl_prefix}"
}

if has_platform "linux"; then
  run_phase linux-local "${SCRIPT_DIR}/build-linux.sh" --appimage --deb
  linux_appimage="$(find "${PROJECT_ROOT}/builds/linux" -maxdepth 1 -type f -name "ZFSMgr-${APP_VERSION}-*.AppImage" | sort -V | tail -n1 || true)"
  linux_deb="$(find "${PROJECT_ROOT}/builds/linux" -maxdepth 1 -type f -name "zfsmgr_${APP_VERSION}_*.deb" | sort -V | tail -n1 || true)"
  [[ -n "${linux_appimage}" ]] || { echo "No se encontró AppImage Linux" >&2; exit 1; }
  [[ -n "${linux_deb}" ]] || { echo "No se encontró .deb Linux" >&2; exit 1; }
  cp -f "${linux_appimage}" "${OUTPUT_DIR}/"
  cp -f "${linux_deb}" "${OUTPUT_DIR}/"
fi

if has_platform "windows"; then
  # Autodetect QT6_WINDOWS_PREFIX if not set, so it can be passed to the Inno step
  if [[ -z "${QT6_WINDOWS_PREFIX:-}" ]]; then
    _qt_win_guess="$(autodetect_path_glob "${HOME}/Qt/*/mingw_64")"
    [[ -n "${_qt_win_guess}" ]] && QT6_WINDOWS_PREFIX="${_qt_win_guess}" && export QT6_WINDOWS_PREFIX
  fi

  run_phase windows-local "${SCRIPT_DIR}/build-cross.sh" --target windows --jobs "${JOBS}"
  win_exe="${PROJECT_ROOT}/builds/cross-windows/zfsmgr_qt.exe"
  [[ -f "${win_exe}" ]] || { echo "No se encontró zfsmgr_qt.exe de Windows cross" >&2; exit 1; }
  cp -f "${win_exe}" "${OUTPUT_DIR}/ZFSMgr-${APP_VERSION}-windows.exe"
  if [[ "${WINDOWS_INSTALLER}" == "1" ]]; then
    _inno_extra_args=()
    [[ -n "${QT6_WINDOWS_PREFIX:-}" ]] && _inno_extra_args+=(--qt-prefix "${QT6_WINDOWS_PREFIX}")
    run_phase windows-installer-local "${SCRIPT_DIR}/build-windows-inno-linux.sh" \
      --input-dir "${PROJECT_ROOT}/builds/cross-windows" \
      --output-dir "${PROJECT_ROOT}/builds/windows-installer" \
      --version "${APP_VERSION}" \
      --exe "zfsmgr_qt.exe" \
      "${_inno_extra_args[@]}"
    win_setup="$(find "${PROJECT_ROOT}/builds/windows-installer" -maxdepth 1 -type f -name "ZFSMgr-Setup-${APP_VERSION}*.exe" | sort -V | tail -n1 || true)"
    [[ -n "${win_setup}" ]] || { echo "No se encontró instalador Inno generado" >&2; exit 1; }
    cp -f "${win_setup}" "${OUTPUT_DIR}/"
  fi
fi

if has_platform "freebsd"; then
  run_phase freebsd-local "${SCRIPT_DIR}/build-cross.sh" --target freebsd --jobs "${JOBS}"
  fbsd_exe="${PROJECT_ROOT}/builds/cross-freebsd/zfsmgr_qt"
  [[ -f "${fbsd_exe}" ]] || { echo "No se encontró zfsmgr_qt de FreeBSD cross" >&2; exit 1; }
  fbsd_pkg="${OUTPUT_DIR}/ZFSMgr-${APP_VERSION}-FreeBSD.tar.gz"
  tar -C "${PROJECT_ROOT}/builds/cross-freebsd" -czf "${fbsd_pkg}" zfsmgr_qt
fi

if has_platform "macos"; then
  IFS=',' read -r -a _mac_arches <<< "${MACOS_ARCHES}"
  for _arch in "${_mac_arches[@]}"; do
    _arch="$(echo "${_arch}" | xargs)"
    [[ -n "${_arch}" ]] || continue
    _target="$(find_osxcross_target "${_arch}" || true)"
    [[ -n "${_target}" ]] || { echo "No se encontró OSXCROSS_TARGET para macOS/${_arch}" >&2; exit 1; }
    _openssl_prefix="$(ensure_macos_openssl "${_arch}" "${_target}")"
    _build_dir="${PROJECT_ROOT}/builds/cross-macos-${_arch}"
    run_phase "macos-local-${_arch}" env \
      OSXCROSS_TARGET="${_target}" \
      OPENSSL_ROOT_DIR="${_openssl_prefix}" \
      "${SCRIPT_DIR}/build-cross.sh" --target macos --build-dir "${_build_dir}" --jobs "${JOBS}"
    mac_app="$(find "${_build_dir}" -maxdepth 1 -type d -name "ZFSMgr-*.app" | sort -V | tail -n1 || true)"
    [[ -n "${mac_app}" ]] || { echo "No se encontró .app de macOS cross (${_arch})" >&2; exit 1; }
    mac_zip="${OUTPUT_DIR}/ZFSMgr-${APP_VERSION}-macos-${_arch}.app.zip"
    (
      cd "${_build_dir}"
      rm -f "${mac_zip}"
      zip -qry "${mac_zip}" "$(basename "${mac_app}")"
    )
  done
fi

log "Artefactos en ${OUTPUT_DIR}:"
find "${OUTPUT_DIR}" -maxdepth 1 -type f -printf '  %f\n' | sort
