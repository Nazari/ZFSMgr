#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
# shellcheck source=entorno.sh
. "${SCRIPT_DIR}/entorno.sh"
SOURCE_DIR="${PROJECT_ROOT}/resources"
TS="$(date '+%Y%m%d-%H%M%S')"
OUTPUT_DIR="${OUTPUT_DIR:-${PROJECT_ROOT}/builds/artifacts/${TS}}"
LOG_DIR="${BUILDALL_LOG_DIR:-${PROJECT_ROOT}/builds/logs/buildall-cross-${TS}}"
PLATFORMS="${BUILD_PLATFORMS:-linux,windows,freebsd,macos}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
MACOS_ARCHES="${MACOS_ARCHES:-amd64,arm64}"

usage() {
  cat <<'EOF'
Uso:
  buildall-cross.sh [--platforms linux,windows,freebsd,macos] [--macos-arches amd64,arm64] [--output-dir <dir>] [--log-dir <dir>] [--jobs <n>]

Descripción:
  Compila las cinco plataformas en este equipo y deja en OUTPUT_DIR SOLO los paquetes de
  la versión (scripts/empaqueta.sh), cada uno con su .sha256:
  - zfsmgr-<ver>.zip   CLI y agentes de todas las plataformas, la interfaz Java e instaladores
  Los agentes quedan también en builds/agents/<plataforma>/, de donde los toma install-daemon.

Variables opcionales:
  OUTPUT_DIR      Directorio de los paquetes (default: builds/artifacts/<timestamp>)
  BUILDALL_LOG_DIR Directorio de logs por plataforma
  BUILD_PLATFORMS Lista separada por comas
  MACOS_ARCHES    Lista separada por comas (default: amd64,arm64)
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

cache_agent_binary() {
  local src="$1"
  local platform_arch="$2"
  local ext="$3"
  [[ -f "${src}" ]] || return 1
  local cache_dir="${PROJECT_ROOT}/builds/agents/${platform_arch}"
  mkdir -p "${cache_dir}"
  local dst="${cache_dir}/zfsmgr_agent${ext}"
  if [[ -e "${dst}" ]]; then
    local src_real dst_real
    src_real="$(readlink -f "${src}" 2>/dev/null || printf '%s' "${src}")"
    dst_real="$(readlink -f "${dst}" 2>/dev/null || printf '%s' "${dst}")"
    if [[ "${src_real}" == "${dst_real}" ]]; then
      return 0
    fi
  fi
  cp -f "${src}" "${dst}"
  case "${platform_arch}" in
    macos-*) firma_agente_macos "${dst}" ;;
  esac
}

# El agente de macOS, recortado y FIRMADO con un certificado estable.
#
# Firmado «ad hoc» —lo que hace el enlazador—, macOS ata el «Acceso total al disco» a la
# huella exacta del binario, y cada actualización lo pierde: el agente deja de ver los pools
# por importar hasta que alguien vuelve a concederlo a mano en cada Mac (mmela, 2026-10-10).
# Con un certificado propio el requisito es «identifier org.zfsmgr.agent y certificado raíz
# = <huella del certificado>», que no cambia de una versión a otra.
#
# El certificado es AUTOFIRMADO (no hay Developer ID de pago, decisión del 2026-08-02) y
# vive fuera del repositorio, en ${ZFSMGR_FIRMA_DIR:-~/.config/zfsmgr-firma}: certificado.pem
# y clave.pem. Si se pierde la clave, todos los Mac piden el permiso una vez más.
#
# Se recorta ANTES de firmar: recortar después invalida la firma. Por eso el empaquetador ya
# no recorta estos agentes.
firma_agente_macos() {
  local bin="$1"
  local dir="${ZFSMGR_FIRMA_DIR:-${HOME}/.config/zfsmgr-firma}"
  llvm-strip "${bin}" 2>/dev/null || true
  local firmador="${RCODESIGN:-}"
  [[ -n "${firmador}" ]] || firmador="$(command -v rcodesign || true)"
  if [[ -z "${firmador}" || ! -f "${dir}/certificado.pem" || ! -f "${dir}/clave.pem" ]]; then
    echo "AVISO: ${bin} queda firmado ad hoc (falta rcodesign o el certificado en ${dir})." >&2
    echo "       macOS pedirá otra vez el «Acceso total al disco» tras cada actualización." >&2
    return 0
  fi
  "${firmador}" sign --pem-source "${dir}/certificado.pem" --pem-source "${dir}/clave.pem" \
    --binary-identifier org.zfsmgr.agent --timestamp-url none "${bin}" >/dev/null
  echo "Firmado ${bin} con el certificado de ${dir}"
}

has_platform() {
  local needle="$1"
  [[ ",${PLATFORMS}," == *",${needle},"* ]]
}

autodetect_path_glob() {
  local glob_pattern="$1"
  local found=""
  shopt -s nullglob
  local candidates=(${glob_pattern})
  shopt -u nullglob
  if [[ ${#candidates[@]} -gt 0 ]]; then
    found="$(printf '%s\n' "${candidates[@]}" | sort -V | tail -n1)"
  fi
  echo "${found}"
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
      openssl_prefix="${ZFSMGR_ENTORNO}/opt/openssl-macos-x86_64"
      openssl_target="darwin64-x86_64-cc"
      ;;
    arm64)
      openssl_prefix="${ZFSMGR_ENTORNO}/opt/openssl-macos-arm64"
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

  compila_openssl_macos "${openssl_prefix}" "${openssl_target}" "${osxcross_target}" "${sdk}" \
                        "no-shared" "${arch}"
  printf '%s\n' "${openssl_prefix}"
}

# Builds OpenSSL for macOS. Todo lo que imprime va a stderr: quien llama devuelve el prefijo por
# stdout y una línea suelta de make lo convertiría en una ruta imposible.
compila_openssl_macos() {
  local prefix="$1" openssl_target="$2" osxcross_target="$3" sdk="$4" modo="$5" etiqueta="$6"
  local work="/tmp/openssl-macos-${etiqueta}-build"
  mkdir -p "${work}" "$(dirname "${prefix}")"
  rm -rf "${work:?}"/*
  curl -fL -o "${work}/openssl.tar.gz" "https://www.openssl.org/source/openssl-3.3.1.tar.gz" >&2
  tar -xf "${work}/openssl.tar.gz" -C "${work}" >&2
  (
    cd "${work}/openssl-3.3.1"
    export PATH="/opt/osxcross/target/bin:${PATH}"
    export SDKROOT="${sdk}"
    export CFLAGS="-isysroot ${sdk} -mmacosx-version-min=10.15"
    export LDFLAGS="-isysroot ${sdk} -mmacosx-version-min=10.15"
    ./Configure "${openssl_target}" --cross-compile-prefix="${osxcross_target}-" --prefix="${prefix}" --libdir=lib no-tests "${modo}" >&2
    make -j"${JOBS}" >&2
    make install_sw >&2
  ) >&2
}

declare -A AGENT_BIN_PATHS=()

# Registra el agente recién compilado Y LO CACHEA EN EL ACTO.
#
# Cachear aquí y no al empaquetar es el arreglo de un fallo real: la caché se rellenaba al
# final, así que un cruce que abortara a mitad
# —macOS sin poder enlazar, por ejemplo— dejaba en `builds/agents/` los binarios de la
# compilación ANTERIOR. Desde el CLI se instalaban esos, y el daemon recién puesto salía
# marcado como desactualizado sin que se entendiera por qué.
set_agent_path_if_exists() {
  local key="$1"
  local path="$2"
  local ext=""
  [[ "${key}" == windows-* ]] && ext=".exe"
  if [[ -f "${path}" ]]; then
    AGENT_BIN_PATHS["${key}"]="${path}"
    cache_agent_binary "${path}" "${key}" "${ext}"
  fi
}

resolve_agent_source() {
  local key="$1"
  local ext="$2"
  local found="${AGENT_BIN_PATHS[${key}]:-}"
  if [[ -n "${found}" && -f "${found}" ]]; then
    printf '%s\n' "${found}"
    return 0
  fi
  local cache="${PROJECT_ROOT}/builds/agents/${key}/zfsmgr_agent${ext}"
  if [[ -f "${cache}" ]]; then
    # Se REUTILIZA uno de una compilación anterior. Se avisa en voz alta: empaquetar un
    # agente viejo junto a un cliente nuevo da una máquina que parece atendida y responde
    # con otra versión de protocolo, y en silencio eso no se descubre hasta usarla.
    echo "AVISO: ${key} no se compiló en esta pasada; se reutiliza ${cache}" >&2
    printf '%s\n' "${cache}"
    return 0
  fi
  return 1
}

# Every platform's agent of this pass is in builds/agents/<platform>: say it when one is
# reused from an earlier pass, and stop when one is missing.
check_agents() {
  local key src ext
  for key in linux-x86_64 windows-x86_64 freebsd-x86_64 macos-amd64 macos-arm64; do
    ext=""
    [[ "${key}" == windows-* ]] && ext=".exe"
    src="$(resolve_agent_source "${key}" "${ext}" || true)"
    [[ -n "${src}" ]] || { echo "No se pudo resolver agente ${key}" >&2; return 1; }
    cache_agent_binary "${src}" "${key}" "${ext}"
  done
}

# OPENSSL_ROOT_DIR también vale para un objetivo: la imagen de toolchain la exporta
# apuntando al OpenSSL de MinGW, que es lo que necesita el cruce a Windows, y la
# compilación nativa de Linux lo heredaba e intentaba enlazar libssl.a de Windows.
_linux_unset=()
if [[ "${OPENSSL_ROOT_DIR:-}" == *mingw* || "${OPENSSL_ROOT_DIR:-}" == *macos* ]]; then
  _linux_unset+=(-u OPENSSL_ROOT_DIR)
fi

# Every target: its CLI and its agent.
if has_platform "linux"; then
  run_phase linux-local env "${_linux_unset[@]}" "${SCRIPT_DIR}/build-linux.sh"
  set_agent_path_if_exists "linux-x86_64" "${PROJECT_ROOT}/builds/linux/zfsmgr_agent"
fi

if has_platform "windows"; then
  run_phase windows-local "${SCRIPT_DIR}/build-cross.sh" --target windows --jobs "${JOBS}"
  set_agent_path_if_exists "windows-x86_64" "${PROJECT_ROOT}/builds/cross-windows/zfsmgr_agent.exe"
fi

if has_platform "freebsd"; then
  run_phase freebsd-local "${SCRIPT_DIR}/build-cross.sh" --target freebsd --jobs "${JOBS}"
  set_agent_path_if_exists "freebsd-x86_64" "${PROJECT_ROOT}/builds/cross-freebsd/zfsmgr_agent"
fi

compila_macos() {
  local arch="$1"
  local target openssl_prefix build_dir
  target="$(find_osxcross_target "${arch}" || true)"
  [[ -n "${target}" ]] || { echo "No se encontró OSXCROSS_TARGET para macOS/${arch}" >&2; exit 1; }
  openssl_prefix="$(ensure_macos_openssl "${arch}" "${target}")"
  build_dir="${PROJECT_ROOT}/builds/cross-macos-${arch}"
  local args=(--target macos --jobs "${JOBS}" --build-dir "${build_dir}")
  # OSXCROSS_TARGET explícito: sin él, el árbol de arm64 llegó a configurarse con el
  # OpenSSL de x86_64 y fallaba al enlazar.
  run_phase "macos-${arch}" env \
    OSXCROSS_TARGET="${target}" \
    OPENSSL_ROOT_DIR="${openssl_prefix}" \
    "${SCRIPT_DIR}/build-cross.sh" "${args[@]}"
  set_agent_path_if_exists "macos-${arch}" "${build_dir}/zfsmgr_agent"
  return 0
}

if has_platform "macos"; then
  IFS=',' read -r -a _mac_arches <<< "${MACOS_ARCHES}"
  for _arch in "${_mac_arches[@]}"; do
    _arch="$(echo "${_arch}" | xargs)"
    [[ -n "${_arch}" ]] || continue
    compila_macos "${_arch}"
  done
fi
check_agents

# Los dos zip de la versión. El empaquetador comprueba la huella de cada pieza y se niega
# si alguna no es de esta compilación.
run_phase empaqueta "${SCRIPT_DIR}/empaqueta.sh" --salida "${OUTPUT_DIR}"
log "Artefactos en ${OUTPUT_DIR}:"
find "${OUTPUT_DIR}" -maxdepth 1 -type f -printf '  %f\n' | sort

# Only the last three builds are kept, and their logs: each one is about 480 MB, and they
# piled up. Only after a build that FINISHED, so a failed one never pushes a good one out;
# and only in the default places, never in a directory given with --output-dir / --log-dir.
# The names are timestamps, so their order is their age.
conserva_ultimas() {
  local dir="$1" patron="$2" viejo
  [[ -d "${dir}" ]] || return 0
  while IFS= read -r viejo; do
    [[ -n "${viejo}" ]] || continue
    log "Se borra una compilación antigua: ${viejo#"${PROJECT_ROOT}/"}"
    rm -rf -- "${viejo}"
  done < <(find "${dir}" -mindepth 1 -maxdepth 1 -type d -name "${patron}" | sort | head -n -3)
}
if [[ "$(dirname "${OUTPUT_DIR}")" == "${PROJECT_ROOT}/builds/artifacts" ]]; then
  conserva_ultimas "${PROJECT_ROOT}/builds/artifacts" '[0-9]*-[0-9]*'
fi
if [[ "$(dirname "${LOG_DIR}")" == "${PROJECT_ROOT}/builds/logs" ]]; then
  conserva_ultimas "${PROJECT_ROOT}/builds/logs" 'buildall-cross-*'
fi
