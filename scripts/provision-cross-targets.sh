#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
# shellcheck source=entorno.sh
. "${SCRIPT_DIR}/entorno.sh"

DO_WINDOWS=0
DO_FREEBSD=0
DO_MACOS=0
DO_MACOS_SIGNER=0
DRY_RUN=0
FORCE=0

OPENSSL_VERSION="3.3.1"
OPENSSL_PREFIX="${ZFSMGR_ENTORNO}/opt/openssl-mingw64"
OPENSSL_MACOS_PREFIX="${ZFSMGR_ENTORNO}/opt/openssl-macos-x86_64"

# 13.5 y 14.2 ya no están en download.freebsd.org (404): usar una release vigente.
FREEBSD_RELEASE="14.3-RELEASE"
FREEBSD_ARCH="amd64"
FREEBSD_SYSROOT_BASE="${ZFSMGR_ENTORNO}/sysroots"

OSXCROSS_ROOT="/opt/osxcross"
MACOS_SDK_PATH=""

usage() {
  cat <<USAGE
Uso:
  provision-cross-targets.sh [opciones]

Opciones:
  --windows                  Instala prerequisitos target Windows (OpenSSL MinGW)
  --freebsd                  Descarga/actualiza sysroot base de FreeBSD
  --macos                    Prepara osxcross (requiere --macos-sdk)
  --macos-signer             Solo el firmador (rcodesign) de los agentes de macOS
  --all                      Equivale a --windows --freebsd --macos
  --openssl-version <v>      Versión OpenSSL MinGW (default: ${OPENSSL_VERSION})
  --openssl-prefix <dir>     Prefijo OpenSSL MinGW (default: ${OPENSSL_PREFIX})
  --openssl-macos-prefix <d> Prefijo OpenSSL target macOS (default: ${OPENSSL_MACOS_PREFIX})
  --freebsd-release <rel>    Release FreeBSD para sysroot (default: ${FREEBSD_RELEASE})
  --freebsd-arch <arch>      Arquitectura FreeBSD (default: ${FREEBSD_ARCH})
  --freebsd-sysroot-base <d> Base de sysroots (default: ${FREEBSD_SYSROOT_BASE})
  --osxcross-root <dir>      Ruta de osxcross (default: ${OSXCROSS_ROOT})
  --macos-sdk <file|dir>     SDK macOS (.tar.xz/.tar.zst/.sdk) para compilar osxcross
  --force                    Reinstala/reconstruye aunque exista
  --dry-run                  Solo imprime acciones
  -h, --help                 Muestra esta ayuda y sale

Salida esperada:
- Windows:
  OPENSSL_ROOT_DIR=<openssl-prefix>
- FreeBSD:
  FREEBSD_SYSROOT=<freebsd-sysroot-base>/freebsd<version>-<arch>
- macOS:
  PATH=<osxcross-root>/target/bin:\$PATH
  OSX_SYSROOT=<osxcross-root>/target/SDK/MacOSX*.sdk
  OPENSSL_ROOT_DIR=<openssl-macos-prefix>
USAGE
}

run_cmd() {
  if [[ ${DRY_RUN} -eq 1 ]]; then
    echo "[dry-run] $*"
  else
    eval "$*"
  fi
}

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "Falta comando requerido: $1" >&2
    exit 2
  }
}

# "no-shared" es intencionado: el
# agente de Windows viaja solo a la máquina remota y no puede depender de DLL de
# OpenSSL que allí no existen.
install_openssl_mingw() {
  if [[ ${FORCE} -eq 1 || ! -f "${OPENSSL_PREFIX}/lib/libcrypto.a" ]]; then
    local work="/tmp/openssl-mingw-build"
    run_cmd "rm -rf '${work}'"
    run_cmd "mkdir -p '${work}' '${ZFSMGR_ENTORNO}/opt'"
    run_cmd "curl -fL --retry 5 --retry-delay 3 --retry-all-errors -C - -o '${work}/openssl.tar.gz' 'https://www.openssl.org/source/openssl-${OPENSSL_VERSION}.tar.gz'"
    run_cmd "tar -xf '${work}/openssl.tar.gz' -C '${work}'"
    run_cmd "cd '${work}/openssl-${OPENSSL_VERSION}' && perl ./Configure mingw64 --cross-compile-prefix=x86_64-w64-mingw32- --prefix='${OPENSSL_PREFIX}' --libdir=lib no-tests no-shared"
    run_cmd "cd '${work}/openssl-${OPENSSL_VERSION}' && make -j'$(nproc 2>/dev/null || echo 4)'"
    run_cmd "cd '${work}/openssl-${OPENSSL_VERSION}' && make install_sw"
  fi
}

install_windows_targets() {
  need_cmd perl
  need_cmd make
  need_cmd curl
  need_cmd x86_64-w64-mingw32-gcc
  need_cmd x86_64-w64-mingw32-g++
  need_cmd x86_64-w64-mingw32-windres

  install_openssl_mingw

  echo
  echo "[windows] listo:"
  echo "  export OPENSSL_ROOT_DIR='${OPENSSL_PREFIX}'"
}

install_freebsd_sysroot() {
  need_cmd curl
  need_cmd tar

  local rel_no_suffix="${FREEBSD_RELEASE%-RELEASE}"
  local sysroot="${FREEBSD_SYSROOT_BASE}/freebsd${rel_no_suffix}-${FREEBSD_ARCH}"
  local txz="/tmp/freebsd-base-${FREEBSD_RELEASE}-${FREEBSD_ARCH}.txz"
  local url="https://download.freebsd.org/releases/${FREEBSD_ARCH}/${FREEBSD_ARCH}/${FREEBSD_RELEASE}/base.txz"

  if [[ ${FORCE} -eq 1 || ! -d "${sysroot}/usr/include" ]]; then
    run_cmd "mkdir -p '${FREEBSD_SYSROOT_BASE}' '${sysroot}'"
    run_cmd "curl -fL --retry 5 --retry-delay 3 --retry-all-errors -C - -o '${txz}' '${url}'"
    run_cmd "tar -xJf '${txz}' -C '${sysroot}'"
  fi

  echo
  echo "[freebsd] listo:"
  echo "  export FREEBSD_SYSROOT='${sysroot}'"
}

# The signer of the macOS agents (rcodesign): it signs from Linux, with the stable
# certificate that keeps «Full Disk Access» across versions (see buildall-cross.sh).
install_macos_signer() {
  need_cmd curl
  need_cmd tar
  local destino="${HOME}/.local/bin/rcodesign"
  if [[ ${FORCE} -eq 0 && -x "${destino}" ]]; then
    return 0
  fi
  local ver="0.29.0"
  local base="https://github.com/indygreg/apple-platform-rs/releases/download/apple-codesign/${ver}"
  local tarro="apple-codesign-${ver}-x86_64-unknown-linux-musl.tar.gz"
  local work="/tmp/rcodesign-dl"
  run_cmd "rm -rf '${work}' && mkdir -p '${work}' '$(dirname "${destino}")'"
  run_cmd "curl -fL --retry 5 --retry-delay 3 -o '${work}/${tarro}' '${base}/${tarro}'"
  # Con su suma publicada: es un binario que va a firmar lo que se distribuye.
  run_cmd "curl -fL --retry 5 --retry-delay 3 -o '${work}/${tarro}.sha256' '${base}/${tarro}.sha256'"
  run_cmd "cd '${work}' && echo \"\$(cat '${tarro}.sha256')  ${tarro}\" | sha256sum -c -"
  run_cmd "tar -xzf '${work}/${tarro}' -C '${work}'"
  run_cmd "cp '${work}'/apple-codesign-*/rcodesign '${destino}'"
  run_cmd "chmod 0755 '${destino}'"
}

setup_osxcross() {
  need_cmd git
  need_cmd clang
  need_cmd cmake
  need_cmd python3
  need_cmd curl
  need_cmd make
  need_cmd perl

  if [[ -z "${MACOS_SDK_PATH}" ]]; then
    echo "[macos] falta --macos-sdk <file|dir>; omitiendo osxcross." >&2
    return 0
  fi

  if [[ ${FORCE} -eq 1 || ! -d "${OSXCROSS_ROOT}/.git" ]]; then
    run_cmd "rm -rf '${OSXCROSS_ROOT}'"
    run_cmd "git clone https://github.com/tpoechtrager/osxcross.git '${OSXCROSS_ROOT}'"
  fi

  run_cmd "mkdir -p '${OSXCROSS_ROOT}/tarballs'"
  if [[ ${DRY_RUN} -eq 1 ]]; then
    if [[ "${MACOS_SDK_PATH}" == *.sdk ]]; then
      run_cmd "tar -C '$(dirname "${MACOS_SDK_PATH}")' -cJf '${OSXCROSS_ROOT}/tarballs/$(basename "${MACOS_SDK_PATH}").tar.xz' '$(basename "${MACOS_SDK_PATH}")'"
    else
      run_cmd "cp -f '${MACOS_SDK_PATH}' '${OSXCROSS_ROOT}/tarballs/'"
    fi
  elif [[ -f "${MACOS_SDK_PATH}" ]]; then
    run_cmd "cp -f '${MACOS_SDK_PATH}' '${OSXCROSS_ROOT}/tarballs/'"
  elif [[ -d "${MACOS_SDK_PATH}" ]]; then
    run_cmd "tar -C '$(dirname "${MACOS_SDK_PATH}")' -cJf '${OSXCROSS_ROOT}/tarballs/$(basename "${MACOS_SDK_PATH}").tar.xz' '$(basename "${MACOS_SDK_PATH}")'"
  else
    echo "[macos] ruta SDK inválida: ${MACOS_SDK_PATH}" >&2
    exit 2
  fi

  run_cmd "cd '${OSXCROSS_ROOT}' && UNATTENDED=1 ./build.sh"

  install_macos_signer

  local sdk_guess="\$(ls -d '${OSXCROSS_ROOT}'/target/SDK/MacOSX*.sdk | sort -V | tail -n1)"
  local osxcross_target="\$(ls -1 '${OSXCROSS_ROOT}'/target/bin/*-apple-darwin*-clang | sed -E 's|.*/([^/]+)-clang|\\1|' | rg '^x86_64-apple-darwin' | sort -V | tail -n1)"
  if [[ ${FORCE} -eq 1 || ! -f "${OPENSSL_MACOS_PREFIX}/lib/libcrypto.a" ]]; then
    local work="/tmp/openssl-macos-build"
    run_cmd "rm -rf '${work}'"
    run_cmd "mkdir -p '${work}' '$(dirname "${OPENSSL_MACOS_PREFIX}")'"
    run_cmd "curl -fL --retry 5 --retry-delay 3 --retry-all-errors -C - -o '${work}/openssl.tar.gz' 'https://www.openssl.org/source/openssl-${OPENSSL_VERSION}.tar.gz'"
    run_cmd "tar -xf '${work}/openssl.tar.gz' -C '${work}'"
    run_cmd "cd '${work}/openssl-${OPENSSL_VERSION}' && PATH='${OSXCROSS_ROOT}/target/bin':\$PATH SDKROOT='${sdk_guess}' CFLAGS='-isysroot ${sdk_guess} -mmacosx-version-min=10.15' LDFLAGS='-isysroot ${sdk_guess} -mmacosx-version-min=10.15' ./Configure darwin64-x86_64-cc --cross-compile-prefix='${osxcross_target}-' --prefix='${OPENSSL_MACOS_PREFIX}' --libdir=lib no-tests no-shared"
    run_cmd "cd '${work}/openssl-${OPENSSL_VERSION}' && make -j'$(nproc 2>/dev/null || echo 4)'"
    run_cmd "cd '${work}/openssl-${OPENSSL_VERSION}' && make install_sw"
  fi

  echo
  echo "[macos] listo (si build.sh completó):"
  echo "  export PATH='${OSXCROSS_ROOT}/target/bin:\$PATH'"
  echo "  export OSXCROSS_TARGET='\$(ls -1 ${OSXCROSS_ROOT}/target/bin/*-apple-darwin*-clang | sed -E \"s|.*/([^/]+)-clang|\\1|\" | rg \"^x86_64-apple-darwin\" | sort -V | tail -n1)'"
  echo "  export OSX_SYSROOT='\$(ls -d ${OSXCROSS_ROOT}/target/SDK/MacOSX*.sdk | sort -V | tail -n1)'"
  echo "  export OPENSSL_ROOT_DIR='${OPENSSL_MACOS_PREFIX}'"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --windows) DO_WINDOWS=1; shift ;;
    --freebsd) DO_FREEBSD=1; shift ;;
    --macos) DO_MACOS=1; shift ;;
    --macos-signer) DO_MACOS_SIGNER=1; shift ;;
    --all) DO_WINDOWS=1; DO_FREEBSD=1; DO_MACOS=1; shift ;;
    --openssl-version) shift; OPENSSL_VERSION="${1:-}"; shift ;;
    --openssl-prefix) shift; OPENSSL_PREFIX="${1:-}"; shift ;;
    --openssl-macos-prefix) shift; OPENSSL_MACOS_PREFIX="${1:-}"; shift ;;
    --freebsd-release) shift; FREEBSD_RELEASE="${1:-}"; shift ;;
    --freebsd-arch) shift; FREEBSD_ARCH="${1:-}"; shift ;;
    --freebsd-sysroot-base) shift; FREEBSD_SYSROOT_BASE="${1:-}"; shift ;;
    --osxcross-root) shift; OSXCROSS_ROOT="${1:-}"; shift ;;
    --macos-sdk) shift; MACOS_SDK_PATH="${1:-}"; shift ;;
    --force) FORCE=1; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Opción desconocida: $1" >&2; usage >&2; exit 1 ;;
  esac
done

if [[ ${DO_WINDOWS} -eq 0 && ${DO_FREEBSD} -eq 0 && ${DO_MACOS} -eq 0 && ${DO_MACOS_SIGNER} -eq 0 ]]; then
  usage
  exit 1
fi

if [[ ${DO_WINDOWS} -eq 1 ]]; then
  install_windows_targets
fi
if [[ ${DO_FREEBSD} -eq 1 ]]; then
  install_freebsd_sysroot
fi
if [[ ${DO_MACOS_SIGNER} -eq 1 && ${DO_MACOS} -eq 0 ]]; then
  install_macos_signer
fi
if [[ ${DO_MACOS} -eq 1 ]]; then
  setup_osxcross
fi

echo
echo "Provisioning completado."
