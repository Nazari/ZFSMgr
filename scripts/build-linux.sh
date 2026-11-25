#!/usr/bin/env bash
# Builds the Linux CLI and agent (and the tests) in builds/linux. The packages are made by
# scripts/empaqueta.sh; the interface is the Java one in gui-java/.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
# shellcheck source=entorno.sh
. "${SCRIPT_DIR}/entorno.sh"
BUILD_DIR="${ZFSMGR_BUILD_DIR:-${PROJECT_ROOT}/builds/linux}"
SOURCE_DIR="${PROJECT_ROOT}/resources"

usage() {
  cat <<'EOF'
Uso:
  build-linux.sh [-- <args extra de CMake>]

Compila zfsmgr-cli, zfsmgr_agent y las pruebas en builds/linux (o ZFSMGR_BUILD_DIR).
EOF
}

EXTRA_ARGS=()
for arg in "$@"; do
  case "${arg}" in
    -h|--help) usage; exit 0 ;;
    --) ;;
    *) EXTRA_ARGS+=("${arg}") ;;
  esac
done

# A tree configured from another checkout keeps that checkout's paths: start it again.
if [[ -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
  cached_source="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "${BUILD_DIR}/CMakeCache.txt" | head -n1)"
  current_source="$(cd "${SOURCE_DIR}" && pwd -P)"
  if [[ -n "${cached_source}" && "${cached_source}" != "${current_source}" ]]; then
    echo "Detectado build cache con fuente distinta (${cached_source}); regenerando ${BUILD_DIR}..."
    rm -rf "${BUILD_DIR}"
  fi
fi

cmake -S "${SOURCE_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release "${EXTRA_ARGS[@]}"
cmake --build "${BUILD_DIR}" -j"$(nproc 2>/dev/null || echo 4)"
echo "Build completado: ${BUILD_DIR}/zfsmgr-cli, ${BUILD_DIR}/zfsmgr_agent"
