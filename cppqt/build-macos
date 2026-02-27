#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build-macos"

# Soporte Homebrew Apple Silicon e Intel.
if [[ -d "/opt/homebrew/opt/qt@6" ]]; then
  QT_PREFIX="/opt/homebrew/opt/qt@6"
elif [[ -d "/usr/local/opt/qt@6" ]]; then
  QT_PREFIX="/usr/local/opt/qt@6"
else
  QT_PREFIX=""
fi

if [[ -n "${QT_PREFIX}" ]]; then
  export PATH="${QT_PREFIX}/bin:${PATH}"
  export CMAKE_PREFIX_PATH="${QT_PREFIX}:${CMAKE_PREFIX_PATH:-}"
fi

if [[ -d "/opt/homebrew/opt/openssl@3" ]]; then
  export CMAKE_PREFIX_PATH="/opt/homebrew/opt/openssl@3:${CMAKE_PREFIX_PATH:-}"
elif [[ -d "/usr/local/opt/openssl@3" ]]; then
  export CMAKE_PREFIX_PATH="/usr/local/opt/openssl@3:${CMAKE_PREFIX_PATH:-}"
fi

cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build "${BUILD_DIR}" -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

echo "Build completado: ${BUILD_DIR}/zfsmgr_qt"
