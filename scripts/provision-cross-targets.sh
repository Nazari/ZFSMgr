#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

DO_WINDOWS=0
DO_FREEBSD=0
DO_MACOS=0
DRY_RUN=0
FORCE=0

QT_VERSION="6.8.3"
QT_ROOT="${HOME}/Qt"
OPENSSL_VERSION="3.3.1"
OPENSSL_PREFIX="${HOME}/opt/openssl-mingw64"

FREEBSD_RELEASE="13.5-RELEASE"
FREEBSD_ARCH="amd64"
FREEBSD_SYSROOT_BASE="${HOME}/sysroots"
FREEBSD_REPO_BRANCH="quarterly"

OSXCROSS_ROOT="/opt/osxcross"
MACOS_SDK_PATH=""

usage() {
  cat <<USAGE
Uso:
  provision-cross-targets.sh [opciones]

Opciones:
  --windows                  Instala prerequisitos target Windows (Qt target+host y OpenSSL MinGW)
  --freebsd                  Descarga/actualiza sysroot base de FreeBSD
  --macos                    Prepara osxcross (requiere --macos-sdk)
  --all                      Equivale a --windows --freebsd --macos
  --qt-version <v>           Versión Qt para aqt (default: ${QT_VERSION})
  --qt-root <dir>            Prefijo instalación Qt (default: ${QT_ROOT})
  --openssl-version <v>      Versión OpenSSL MinGW (default: ${OPENSSL_VERSION})
  --openssl-prefix <dir>     Prefijo OpenSSL MinGW (default: ${OPENSSL_PREFIX})
  --freebsd-release <rel>    Release FreeBSD para sysroot (default: ${FREEBSD_RELEASE})
  --freebsd-arch <arch>      Arquitectura FreeBSD (default: ${FREEBSD_ARCH})
  --freebsd-sysroot-base <d> Base de sysroots (default: ${FREEBSD_SYSROOT_BASE})
  --freebsd-repo-branch <b>  Rama repo pkg FreeBSD: quarterly|latest (default: ${FREEBSD_REPO_BRANCH})
  --osxcross-root <dir>      Ruta de osxcross (default: ${OSXCROSS_ROOT})
  --macos-sdk <file|dir>     SDK macOS (.tar.xz/.tar.zst/.sdk) para compilar osxcross
  --force                    Reinstala/reconstruye aunque exista
  --dry-run                  Solo imprime acciones
  -h, --help                 Muestra esta ayuda y sale

Salida esperada:
- Windows:
  QT6_WINDOWS_PREFIX=<qt-root>/<version>/mingw_64
  QT_HOST_PATH=<qt-root>/<version>/gcc_64
  QT_HOST_PATH_CMAKE_DIR=<qt-root>/<version>/gcc_64/lib/cmake/Qt6
  OPENSSL_ROOT_DIR=<openssl-prefix>
- FreeBSD:
  FREEBSD_SYSROOT=<freebsd-sysroot-base>/freebsd<version>-<arch>
- macOS:
  PATH=<osxcross-root>/target/bin:\$PATH
  OSX_SYSROOT=<osxcross-root>/target/SDK/MacOSX*.sdk
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

ensure_aqt() {
  local venv="${HOME}/.local/venvs/aqtinstall"
  if [[ ${FORCE} -eq 1 || ! -x "${venv}/bin/aqt" ]]; then
    run_cmd "python3 -m venv '${venv}'"
    run_cmd "'${venv}/bin/pip' install -U pip aqtinstall"
  fi
  echo "${venv}/bin/aqt"
}

install_windows_targets() {
  need_cmd python3
  need_cmd perl
  need_cmd make
  need_cmd curl
  need_cmd x86_64-w64-mingw32-gcc
  need_cmd x86_64-w64-mingw32-g++
  need_cmd x86_64-w64-mingw32-windres

  local aqt
  aqt="$(ensure_aqt)"

  local qt_win="${QT_ROOT}/${QT_VERSION}/mingw_64"
  local qt_host="${QT_ROOT}/${QT_VERSION}/gcc_64"

  if [[ ${FORCE} -eq 1 || ! -f "${qt_win}/lib/cmake/Qt6/Qt6Config.cmake" ]]; then
    run_cmd "mkdir -p '${QT_ROOT}'"
    run_cmd "'${aqt}' install-qt -O '${QT_ROOT}' windows desktop '${QT_VERSION}' win64_mingw"
  fi

  if [[ ${FORCE} -eq 1 || ! -f "${qt_host}/lib/cmake/Qt6/Qt6Config.cmake" ]]; then
    run_cmd "mkdir -p '${QT_ROOT}'"
    run_cmd "'${aqt}' install-qt -O '${QT_ROOT}' linux desktop '${QT_VERSION}' linux_gcc_64"
  fi

  if [[ ${FORCE} -eq 1 || ! -f "${OPENSSL_PREFIX}/lib/libcrypto.a" ]]; then
    local work="/tmp/openssl-mingw-build"
    run_cmd "rm -rf '${work}'"
    run_cmd "mkdir -p '${work}' '${HOME}/opt'"
    run_cmd "curl -fL -o '${work}/openssl.tar.gz' 'https://www.openssl.org/source/openssl-${OPENSSL_VERSION}.tar.gz'"
    run_cmd "tar -xf '${work}/openssl.tar.gz' -C '${work}'"
    run_cmd "cd '${work}/openssl-${OPENSSL_VERSION}' && perl ./Configure mingw64 --cross-compile-prefix=x86_64-w64-mingw32- --prefix='${OPENSSL_PREFIX}' --libdir=lib no-tests no-shared"
    run_cmd "cd '${work}/openssl-${OPENSSL_VERSION}' && make -j'$(nproc 2>/dev/null || echo 4)'"
    run_cmd "cd '${work}/openssl-${OPENSSL_VERSION}' && make install_sw"
  fi

  echo
  echo "[windows] listo:"
  echo "  export QT6_WINDOWS_PREFIX='${qt_win}'"
  echo "  export QT_HOST_PATH='${qt_host}'"
  echo "  export QT_HOST_PATH_CMAKE_DIR='${qt_host}/lib/cmake/Qt6'"
  echo "  export OPENSSL_ROOT_DIR='${OPENSSL_PREFIX}'"
}

freebsd_packagesite_yaml() {
  local repo_root="https://pkg.freebsd.org/FreeBSD:13:${FREEBSD_ARCH}/${FREEBSD_REPO_BRANCH}"
  local work="/tmp/zfsmgr-freebsd-pkgsite"
  local pkg="${work}/packagesite.pkg"
  local tarf="${work}/packagesite.tar"
  local yaml="${work}/packagesite.yaml"

  run_cmd "mkdir -p '${work}'"
  if [[ ${FORCE} -eq 1 || ! -s "${yaml}" ]]; then
    run_cmd "curl -fsSL -o '${pkg}' '${repo_root}/packagesite.pkg'"
    run_cmd "unzstd -f '${pkg}' -o '${tarf}'"
    run_cmd "tar -xf '${tarf}' -C '${work}' packagesite.yaml"
  fi
  echo "${yaml}"
}

freebsd_resolve_pkg_paths() {
  local yaml="$1"
  python3 - "$yaml" <<'PY'
import json, sys
yaml = sys.argv[1]
idx = {}
with open(yaml, 'r', encoding='utf-8', errors='ignore') as f:
    for line in f:
        o = json.loads(line)
        idx[o['name']] = {
            'path': o.get('path') or o.get('repopath') or '',
            'deps': list((o.get('deps') or {}).keys())
        }

target = 'qt6-base'
seen = set()
stack = [target]
while stack:
    name = stack.pop()
    if name in seen:
        continue
    seen.add(name)
    meta = idx.get(name)
    if not meta:
        continue
    stack.extend(meta['deps'])

for name in sorted(seen):
    meta = idx.get(name)
    if meta and meta['path']:
        print(meta['path'])
PY
}

freebsd_qt_base_version() {
  local yaml="$1"
  python3 - "$yaml" <<'PY'
import json, sys, re
yaml = sys.argv[1]
for line in open(yaml, 'r', encoding='utf-8', errors='ignore'):
    if '"name":"qt6-base"' in line:
        o = json.loads(line)
        ver = o.get('version', '')
        m = re.match(r'([0-9]+\\.[0-9]+\\.[0-9]+)', ver)
        print(m.group(1) if m else ver)
        break
PY
}

install_freebsd_qt_packages() {
  local sysroot="$1"
  local repo_root="https://pkg.freebsd.org/FreeBSD:13:${FREEBSD_ARCH}/${FREEBSD_REPO_BRANCH}"
  local yaml
  yaml="$(freebsd_packagesite_yaml)"

  local pkg_paths
  pkg_paths="$(freebsd_resolve_pkg_paths "${yaml}")"
  if [[ -z "${pkg_paths}" ]]; then
    echo "[freebsd] no se pudieron resolver paquetes para qt6-base" >&2
    exit 2
  fi

  local cache_dir="/tmp/zfsmgr-freebsd-pkgs"
  run_cmd "mkdir -p '${cache_dir}'"
  while IFS= read -r relpath; do
    [[ -n "${relpath}" ]] || continue
    local bn
    bn="$(basename "${relpath}")"
    local local_pkg="${cache_dir}/${bn}"
    local local_tar="${cache_dir}/${bn%.pkg}.tar"
    run_cmd "curl -fsSL -o '${local_pkg}' '${repo_root}/${relpath}'"
    run_cmd "unzstd -f '${local_pkg}' -o '${local_tar}'"
    run_cmd "tar -xf '${local_tar}' -C '${sysroot}'"
  done <<< "${pkg_paths}"

  local qt_base_ver
  qt_base_ver="$(freebsd_qt_base_version "${yaml}")"
  if [[ -n "${qt_base_ver}" ]]; then
    local aqt
    aqt="$(ensure_aqt)"
    local qt_host="${QT_ROOT}/${qt_base_ver}/gcc_64"
    if [[ ${FORCE} -eq 1 || ! -f "${qt_host}/lib/cmake/Qt6/Qt6Config.cmake" ]]; then
      run_cmd "mkdir -p '${QT_ROOT}'"
      run_cmd "'${aqt}' install-qt -O '${QT_ROOT}' linux desktop '${qt_base_ver}' linux_gcc_64"
    fi
  fi
}

install_freebsd_sysroot() {
  need_cmd curl
  need_cmd tar
  need_cmd unzstd
  need_cmd python3

  local rel_no_suffix="${FREEBSD_RELEASE%-RELEASE}"
  local sysroot="${FREEBSD_SYSROOT_BASE}/freebsd${rel_no_suffix}-${FREEBSD_ARCH}"
  local txz="/tmp/freebsd-base-${FREEBSD_RELEASE}-${FREEBSD_ARCH}.txz"
  local url="https://download.freebsd.org/releases/${FREEBSD_ARCH}/${FREEBSD_ARCH}/${FREEBSD_RELEASE}/base.txz"

  if [[ ${FORCE} -eq 1 || ! -d "${sysroot}/usr/include" ]]; then
    run_cmd "mkdir -p '${FREEBSD_SYSROOT_BASE}' '${sysroot}'"
    run_cmd "curl -fL -o '${txz}' '${url}'"
    run_cmd "tar -xJf '${txz}' -C '${sysroot}'"
  fi

  if [[ ${FORCE} -eq 1 || ! -f "${sysroot}/usr/local/lib/cmake/Qt6/Qt6Config.cmake" ]]; then
    install_freebsd_qt_packages "${sysroot}"
  fi

  echo
  echo "[freebsd] listo:"
  echo "  export FREEBSD_SYSROOT='${sysroot}'"
  echo "  export QT6_FREEBSD_PREFIX='${sysroot}/usr/local'"
  echo "  # recomendado para tools host:"
  echo "  export QT_HOST_PATH='$(ls -d "${QT_ROOT}"/*/gcc_64 2>/dev/null | sort -V | tail -n1)'"
  echo "  export QT_HOST_PATH_CMAKE_DIR='\${QT_HOST_PATH}/lib/cmake/Qt6'"
}

setup_osxcross() {
  need_cmd git
  need_cmd clang
  need_cmd cmake

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

  echo
  echo "[macos] listo (si build.sh completó):"
  echo "  export PATH='${OSXCROSS_ROOT}/target/bin:\$PATH'"
  echo "  export OSXCROSS_TARGET='x86_64-apple-darwin23'"
  echo "  export OSX_SYSROOT='\$(ls -d ${OSXCROSS_ROOT}/target/SDK/MacOSX*.sdk | sort -V | tail -n1)'"
  echo "  # pendiente: export QT6_MACOS_PREFIX=<qt6-target-macos>"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --windows) DO_WINDOWS=1; shift ;;
    --freebsd) DO_FREEBSD=1; shift ;;
    --macos) DO_MACOS=1; shift ;;
    --all) DO_WINDOWS=1; DO_FREEBSD=1; DO_MACOS=1; shift ;;
    --qt-version) shift; QT_VERSION="${1:-}"; shift ;;
    --qt-root) shift; QT_ROOT="${1:-}"; shift ;;
    --openssl-version) shift; OPENSSL_VERSION="${1:-}"; shift ;;
    --openssl-prefix) shift; OPENSSL_PREFIX="${1:-}"; shift ;;
    --freebsd-release) shift; FREEBSD_RELEASE="${1:-}"; shift ;;
    --freebsd-arch) shift; FREEBSD_ARCH="${1:-}"; shift ;;
    --freebsd-sysroot-base) shift; FREEBSD_SYSROOT_BASE="${1:-}"; shift ;;
    --freebsd-repo-branch) shift; FREEBSD_REPO_BRANCH="${1:-}"; shift ;;
    --osxcross-root) shift; OSXCROSS_ROOT="${1:-}"; shift ;;
    --macos-sdk) shift; MACOS_SDK_PATH="${1:-}"; shift ;;
    --force) FORCE=1; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Opción desconocida: $1" >&2; usage >&2; exit 1 ;;
  esac
done

if [[ ${DO_WINDOWS} -eq 0 && ${DO_FREEBSD} -eq 0 && ${DO_MACOS} -eq 0 ]]; then
  usage
  exit 1
fi

if [[ ${DO_WINDOWS} -eq 1 ]]; then
  install_windows_targets
fi
if [[ ${DO_FREEBSD} -eq 1 ]]; then
  install_freebsd_sysroot
fi
if [[ ${DO_MACOS} -eq 1 ]]; then
  setup_osxcross
fi

echo
echo "Provisioning completado."
