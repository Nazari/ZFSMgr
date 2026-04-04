#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build-freebsd"
SOURCE_DIR="${PROJECT_ROOT}/resources"
SFTP_TARGET="${ZFSMGR_SFTP_TARGET:-sftp://linarese@fc16:Descargas/z}"
ARCH="$(uname -m)"
BUILD_PKG=0
UPLOAD_SFTP=0
EXTRA_ARGS=()

usage() {
  cat <<'EOF'
Uso:
  build-freebsd.sh [opciones] [-- <args extra de CMake>]

Opciones:
  --pkg        Genera también el paquete .tar.gz mediante CPack
  --sftpfc16   Sube los artefactos finales al destino SFTP configurado
  -h, --help   Muestra esta ayuda

Variables opcionales:
  ZFSMGR_SFTP_TARGET  Destino SFTP para --sftpfc16

Dependencias en FreeBSD (instalar con pkg):
  pkg install cmake qt6 openssl

Ejemplos:
  ./scripts/build-freebsd.sh
  ./scripts/build-freebsd.sh --pkg
  ./scripts/build-freebsd.sh --pkg --sftpfc16
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

resolve_app_version() {
  local version=""
  if [[ -f "${SOURCE_DIR}/CMakeLists.txt" ]]; then
    version="$(sed -nE 's/^[[:space:]]*set\([[:space:]]*ZFSMGR_APP_VERSION_STRING[[:space:]]*"([^"]+)".*/\1/p' "${SOURCE_DIR}/CMakeLists.txt" | head -n1)"
  fi
  if [[ -z "${version}" && -f "${SOURCE_DIR}/CMakeLists.txt" ]]; then
    version="$(sed -nE 's/^[[:space:]]*project\([[:space:]]*ZFSMgrQt[[:space:]]+VERSION[[:space:]]+([^[:space:])]+).*/\1/p' "${SOURCE_DIR}/CMakeLists.txt" | head -n1)"
  fi
  if [[ -z "${version}" && -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    version="$(sed -n 's/^ZFSMGR_APP_VERSION_STRING:UNINITIALIZED=//p' "${BUILD_DIR}/CMakeCache.txt" | head -n1)"
  fi
  if [[ -z "${version}" ]]; then
    version="0.10.0rc1"
  fi
  printf '%s\n' "${version}"
}

APP_VERSION="$(resolve_app_version)"

for arg in "$@"; do
  case "${arg}" in
    --pkg)
      BUILD_PKG=1
      ;;
    --sftpfc16)
      UPLOAD_SFTP=1
      ;;
    *)
      EXTRA_ARGS+=("${arg}")
      ;;
  esac
done

if [[ "${UPLOAD_SFTP}" -eq 1 && "${BUILD_PKG}" -eq 0 ]]; then
  echo "Error: --sftpfc16 requiere --pkg." >&2
  exit 1
fi

parse_sftp_target() {
  local target="$1"
  local authority path user host host_and_base base_path
  if [[ "${target}" =~ ^sftp:// || "${target}" =~ ^sft:// ]]; then
    target="${target#sftp://}"
    target="${target#sft://}"
    authority="${target%%/*}"
    path="/${target#*/}"
    if [[ "${authority}" == *"@"* ]]; then
      user="${authority%@*}"
      host_and_base="${authority#*@}"
      if [[ "${host_and_base}" == *":"* ]]; then
        host="${host_and_base%%:*}"
        base_path="${host_and_base#*:}"
        if [[ -n "${base_path}" ]]; then
          if [[ "${path}" == "/" ]]; then
            path=""
          fi
          if [[ "${base_path}" == /* ]]; then
            path="${base_path}${path}"
          else
            path="${base_path}${path}"
          fi
        fi
      else
        host="${host_and_base}"
      fi
    elif [[ "${authority}" == *":"* ]]; then
      user="${authority%%:*}"
      host="${authority#*:}"
    else
      user="${USER:-linarese}"
      host="${authority}"
    fi
  elif [[ "${target}" == *":"* ]]; then
    user="${target%%@*}"
    if [[ "${target}" != *"@"* ]]; then
      user="${USER:-linarese}"
    fi
    host="${target#*@}"
    host="${host%%:*}"
    path="/${target#*:}"
  else
    echo "Error: destino SFTP inválido: ${target}" >&2
    return 1
  fi
  echo "${user}@${host}|${path}"
}

upload_to_sftp() {
  local artifact="$1"
  local parsed remote path
  parsed="$(parse_sftp_target "${SFTP_TARGET}")"
  remote="${parsed%%|*}"
  path="${parsed#*|}"
  echo "Subiendo artefacto a ${remote}:${path}"
  if [[ "${path}" == /* ]]; then
    ssh -o BatchMode=yes "${remote}" "mkdir -p '${path}'" </dev/null
    scp "${artifact}" "${remote}:${path}/" </dev/null
  else
    ssh -o BatchMode=yes "${remote}" "mkdir -p \"\$HOME/${path}\"" </dev/null
    scp "${artifact}" "${remote}:~/${path}/" </dev/null
  fi
}

upload_pkg_artifacts() {
  local pkg_file
  local found=0
  while IFS= read -r -d '' pkg_file; do
    upload_to_sftp "${pkg_file}"
    found=1
  done < <(find "${BUILD_DIR}" -maxdepth 1 -type f \( -name '*.tar.gz' -o -name '*.tar.bz2' \) -print0)
  if [[ ${found} -eq 0 ]]; then
    echo "Error: no se encontró ningún paquete para subir." >&2
    exit 1
  fi
}

ensure_build_dir_source_match() {
  if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    return 0
  fi
  local cached_source current_source
  cached_source="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "${BUILD_DIR}/CMakeCache.txt" | head -n1)"
  current_source="$(cd "${SOURCE_DIR}" && pwd -P)"
  if [[ -z "${cached_source}" ]]; then
    return 0
  fi
  if [[ "${cached_source}" != "${current_source}" ]]; then
    echo "Detectado build cache con fuente distinta:"
    echo "  cache:   ${cached_source}"
    echo "  actual:  ${current_source}"
    echo "Regenerando ${BUILD_DIR}..."
    rm -rf "${BUILD_DIR}"
  fi
}

# nproc no existe en FreeBSD; sysctl -n hw.ncpu es el equivalente
NCPU="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

if [[ "${BUILD_PKG}" -eq 0 ]]; then
  ensure_build_dir_source_match
  cmake -S "${SOURCE_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release "${EXTRA_ARGS[@]}"
  cmake --build "${BUILD_DIR}" -j"${NCPU}"
  echo "Build completado: ${BUILD_DIR}/zfsmgr_qt"
  exit 0
fi

echo "Configuring and building Release binary..."
ensure_build_dir_source_match
cmake -S "${SOURCE_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release "${EXTRA_ARGS[@]}"
cmake --build "${BUILD_DIR}" -j"${NCPU}"

echo "Building package (TGZ)..."
# Sobreescribir el generador DEB (definido en CMakeLists para UNIX) con TGZ
cpack --config "${BUILD_DIR}/CPackConfig.cmake" -G TGZ -B "${BUILD_DIR}"
echo "Paquete generado:"
ls -lh "${BUILD_DIR}"/*.tar.gz 2>/dev/null || ls -lh "${BUILD_DIR}"/*.tar.bz2 2>/dev/null || true

if [[ "${UPLOAD_SFTP}" -eq 1 ]]; then
  upload_pkg_artifacts
fi
