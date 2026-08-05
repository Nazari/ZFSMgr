#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

DO_WINDOWS=0
SKIP_QT=0
DO_FREEBSD=0
DO_MACOS=0
DRY_RUN=0
FORCE=0

# Fuente única de la versión de Qt que sí elegimos (CI y cross de Windows/Linux).
# La de FreeBSD NO se fija aquí: viene del repositorio de paquetes de FreeBSD y el
# propio script instala después un Qt host que le haga juego.
QT_VERSION="$(cat "${PROJECT_ROOT}/qt-version.txt" 2>/dev/null | tr -d '[:space:]')"
QT_VERSION="${QT_VERSION:-6.8.3}"
QT_ROOT="${HOME}/Qt"
OPENSSL_VERSION="3.3.1"
OPENSSL_PREFIX="${HOME}/opt/openssl-mingw64"
OPENSSL_MACOS_PREFIX="${HOME}/opt/openssl-macos-x86_64"

# 13.5 y 14.2 ya no están en download.freebsd.org (404): usar una release vigente.
FREEBSD_RELEASE="14.3-RELEASE"
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
  --openssl-macos-prefix <d> Prefijo OpenSSL target macOS (default: ${OPENSSL_MACOS_PREFIX})
  --freebsd-release <rel>    Release FreeBSD para sysroot (default: ${FREEBSD_RELEASE})
  --freebsd-arch <arch>      Arquitectura FreeBSD (default: ${FREEBSD_ARCH})
  --freebsd-sysroot-base <d> Base de sysroots (default: ${FREEBSD_SYSROOT_BASE})
  --freebsd-repo-branch <b>  Rama repo pkg FreeBSD: quarterly|latest (default: ${FREEBSD_REPO_BRANCH})
  --osxcross-root <dir>      Ruta de osxcross (default: ${OSXCROSS_ROOT})
  --macos-sdk <file|dir>     SDK macOS (.tar.xz/.tar.zst/.sdk) para compilar osxcross
  --skip-qt                  Con --windows, solo prepara OpenSSL (sin Qt)
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
  QT6_MACOS_PREFIX=<qt-root>/<version>/clang_64
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

ensure_aqt() {
  local venv="${HOME}/.local/venvs/aqtinstall"
  # No basta con que el ejecutable exista: un entorno virtual a medias deja el
  # lanzador de aqt en su sitio pero sin el módulo detrás, y entonces el fallo
  # aparece mucho después, como un traceback de Python en mitad del registro y
  # lejos de su causa. Se comprueba que arranca de verdad; si no, se rehace.
  local aqt_usable=0
  if [[ -x "${venv}/bin/aqt" ]] && "${venv}/bin/aqt" version >/dev/null 2>&1; then
    aqt_usable=1
  fi
  if [[ ${FORCE} -eq 1 || ${aqt_usable} -eq 0 ]]; then
    if [[ -d "${venv}" && ${aqt_usable} -eq 0 ]]; then
      echo "[aqt] el entorno virtual está incompleto; se rehace" >&2
      run_cmd "rm -rf '${venv}'" >&2
    fi
    # A stderr: esta función devuelve la ruta de aqt por stdout y quien la llama la
    # captura con $(...). Sin redirigir, la salida de venv y pip se capturaba junto a
    # la ruta y el resultado era una cadena enorme ("File name too long" al ejecutar).
    # Solo se notaba al aprovisionar desde cero, porque en una máquina ya preparada
    # este bloque no se ejecuta.
    run_cmd "python3 -m venv '${venv}'" >&2
    run_cmd "'${venv}/bin/pip' install -U pip aqtinstall" >&2
  fi
  echo "${venv}/bin/aqt"
}

# Extraída para poder pedirla sola con --skip-qt. "no-shared" es intencionado: el
# agente de Windows viaja solo a la máquina remota y no puede depender de DLL de
# OpenSSL que allí no existen.
install_openssl_mingw() {
  if [[ ${FORCE} -eq 1 || ! -f "${OPENSSL_PREFIX}/lib/libcrypto.a" ]]; then
    local work="/tmp/openssl-mingw-build"
    run_cmd "rm -rf '${work}'"
    run_cmd "mkdir -p '${work}' '${HOME}/opt'"
    run_cmd "curl -fL --retry 5 --retry-delay 3 --retry-all-errors -C - -o '${work}/openssl.tar.gz' 'https://www.openssl.org/source/openssl-${OPENSSL_VERSION}.tar.gz'"
    run_cmd "tar -xf '${work}/openssl.tar.gz' -C '${work}'"
    run_cmd "cd '${work}/openssl-${OPENSSL_VERSION}' && perl ./Configure mingw64 --cross-compile-prefix=x86_64-w64-mingw32- --prefix='${OPENSSL_PREFIX}' --libdir=lib no-tests no-shared"
    run_cmd "cd '${work}/openssl-${OPENSSL_VERSION}' && make -j'$(nproc 2>/dev/null || echo 4)'"
    run_cmd "cd '${work}/openssl-${OPENSSL_VERSION}' && make install_sw"
  fi
}

install_windows_targets() {
  need_cmd python3
  need_cmd perl
  need_cmd make
  need_cmd curl
  need_cmd x86_64-w64-mingw32-gcc
  need_cmd x86_64-w64-mingw32-g++
  need_cmd x86_64-w64-mingw32-windres

  # Con --skip-qt solo se prepara OpenSSL para MinGW. Lo usa la integración continua
  # para cruzar el agente, que no usa Qt: instalarlo allí serían varios minutos y
  # cientos de megas para nada.
  if [[ ${SKIP_QT} -eq 1 ]]; then
    install_openssl_mingw
    return 0
  fi

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

  install_openssl_mingw

  echo
  echo "[windows] listo:"
  echo "  export QT6_WINDOWS_PREFIX='${qt_win}'"
  echo "  export QT_HOST_PATH='${qt_host}'"
  echo "  export QT_HOST_PATH_CMAKE_DIR='${qt_host}/lib/cmake/Qt6'"
  echo "  export OPENSSL_ROOT_DIR='${OPENSSL_PREFIX}'"
}

freebsd_packagesite_yaml() {
  local osmajor="${FREEBSD_RELEASE%%.*}"
  local repo_root="https://pkg.freebsd.org/FreeBSD:${osmajor}:${FREEBSD_ARCH}/${FREEBSD_REPO_BRANCH}"
  local work="/tmp/zfsmgr-freebsd-pkgsite"
  local pkg="${work}/packagesite.pkg"
  local tarf="${work}/packagesite.tar"
  local yaml="${work}/packagesite.yaml"

  # Mismo cuidado que en ensure_aqt: esta función devuelve una ruta por stdout, así
  # que la salida de las órdenes va a stderr para no contaminarla.
  run_cmd "mkdir -p '${work}'" >&2
  if [[ ${FORCE} -eq 1 || ! -s "${yaml}" ]]; then
    run_cmd "curl -fsSL --retry 5 --retry-delay 3 --retry-all-errors -o '${pkg}' '${repo_root}/packagesite.pkg'" >&2
    run_cmd "unzstd -f '${pkg}' -o '${tarf}'" >&2
    run_cmd "tar -xf '${tarf}' -C '${work}' packagesite.yaml" >&2
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
  local osmajor="${FREEBSD_RELEASE%%.*}"
  local repo_root="https://pkg.freebsd.org/FreeBSD:${osmajor}:${FREEBSD_ARCH}/${FREEBSD_REPO_BRANCH}"
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
    run_cmd "curl -fsSL --retry 5 --retry-delay 3 --retry-all-errors -o '${local_pkg}' '${repo_root}/${relpath}'"
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
    run_cmd "curl -fL --retry 5 --retry-delay 3 --retry-all-errors -C - -o '${txz}' '${url}'"
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

  local aqt
  aqt="$(ensure_aqt)"
  local qt_macos="${QT_ROOT}/${QT_VERSION}/clang_64"
  if [[ ${FORCE} -eq 1 || ! -f "${qt_macos}/lib/cmake/Qt6/Qt6Config.cmake" ]]; then
    run_cmd "mkdir -p '${QT_ROOT}'"
    run_cmd "'${aqt}' install-qt -O '${QT_ROOT}' mac desktop '${QT_VERSION}' clang_64"
  fi

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

  # Y otra vez, COMPARTIDO, en un prefijo aparte. Son dos cosas distintas:
  #  - el estático de arriba lo enlazan la GUI y el agente; el agente se despliega solo
  #    en máquinas ajenas, así que tiene que seguir siendo autosuficiente;
  #  - estas .dylib no las enlaza nadie: las carga en tiempo de ejecución el plugin
  #    libqopensslbackend.dylib de Qt, y sin ellas el .app se queda con SecureTransport,
  #    que no presenta el certificado de cliente. Resultado: ningún daemon conecta
  #    (`SSLHandshake failed: -9831`) y la aplicación dice que el agente no está en
  #    marcha aunque lo esté. Pasó con los .app publicados de 0.90.6 y 0.90.7.
  # Las DOS arquitecturas: se publica un .app por cada una y las dos necesitan su
  # OpenSSL. (El bloque estático de arriba solo cubre x86_64; eso viene de antes.)
  local ossl_arch
  for ossl_arch in x86_64 arm64; do
    local shared_prefix="${HOME}/opt/openssl-macos-${ossl_arch}-shared"
    [[ ${FORCE} -eq 0 && -f "${shared_prefix}/lib/libssl.3.dylib" ]] && continue
    local conf_target="darwin64-${ossl_arch}-cc"
    local xtriple="\$(ls -1 '${OSXCROSS_ROOT}'/target/bin/*-apple-darwin*-clang | sed -E 's|.*/([^/]+)-clang|\\1|' | rg '^${ossl_arch}-apple-darwin' | sort -V | tail -n1)"
    local swork="/tmp/openssl-macos-shared-${ossl_arch}"
    run_cmd "rm -rf '${swork}'"
    run_cmd "mkdir -p '${swork}' '$(dirname "${shared_prefix}")'"
    run_cmd "curl -fL --retry 5 --retry-delay 3 --retry-all-errors -C - -o '${swork}/openssl.tar.gz' 'https://www.openssl.org/source/openssl-${OPENSSL_VERSION}.tar.gz'"
    run_cmd "tar -xf '${swork}/openssl.tar.gz' -C '${swork}'"
    run_cmd "cd '${swork}/openssl-${OPENSSL_VERSION}' && PATH='${OSXCROSS_ROOT}/target/bin':\$PATH SDKROOT='${sdk_guess}' CFLAGS='-isysroot ${sdk_guess} -mmacosx-version-min=10.15' LDFLAGS='-isysroot ${sdk_guess} -mmacosx-version-min=10.15' ./Configure ${conf_target} --cross-compile-prefix='${xtriple}-' --prefix='${shared_prefix}' --libdir=lib no-tests shared"
    run_cmd "cd '${swork}/openssl-${OPENSSL_VERSION}' && make -j'$(nproc 2>/dev/null || echo 4)'"
    run_cmd "cd '${swork}/openssl-${OPENSSL_VERSION}' && make install_sw"
  done

  echo
  echo "[macos] listo (si build.sh completó):"
  echo "  export PATH='${OSXCROSS_ROOT}/target/bin:\$PATH'"
  echo "  export OSXCROSS_TARGET='\$(ls -1 ${OSXCROSS_ROOT}/target/bin/*-apple-darwin*-clang | sed -E \"s|.*/([^/]+)-clang|\\1|\" | rg \"^x86_64-apple-darwin\" | sort -V | tail -n1)'"
  echo "  export OSX_SYSROOT='\$(ls -d ${OSXCROSS_ROOT}/target/SDK/MacOSX*.sdk | sort -V | tail -n1)'"
  echo "  export QT6_MACOS_PREFIX='${qt_macos}'"
  echo "  export OPENSSL_ROOT_DIR='${OPENSSL_MACOS_PREFIX}'"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --windows) DO_WINDOWS=1; shift ;;
    --skip-qt) SKIP_QT=1; shift ;;
    --freebsd) DO_FREEBSD=1; shift ;;
    --macos) DO_MACOS=1; shift ;;
    --all) DO_WINDOWS=1; DO_FREEBSD=1; DO_MACOS=1; shift ;;
    --qt-version) shift; QT_VERSION="${1:-}"; shift ;;
    --qt-root) shift; QT_ROOT="${1:-}"; shift ;;
    --openssl-version) shift; OPENSSL_VERSION="${1:-}"; shift ;;
    --openssl-prefix) shift; OPENSSL_PREFIX="${1:-}"; shift ;;
    --openssl-macos-prefix) shift; OPENSSL_MACOS_PREFIX="${1:-}"; shift ;;
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
