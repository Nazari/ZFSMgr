#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build-macos"
BUNDLE_APP=0
EXTRA_CMAKE_ARGS=()

for arg in "$@"; do
  if [[ "${arg}" == "--bundle" ]]; then
    BUNDLE_APP=1
  else
    EXTRA_CMAKE_ARGS+=("${arg}")
  fi
done

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

cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release "${EXTRA_CMAKE_ARGS[@]}"
cmake --build "${BUILD_DIR}" -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

echo "Build completado: ${BUILD_DIR}/zfsmgr_qt"
if [[ "${BUNDLE_APP}" -eq 1 ]]; then
  APP_BUNDLE="${BUILD_DIR}/zfsmgr_qt.app"
  if [[ ! -d "${APP_BUNDLE}" ]]; then
    echo "Error: no se ha generado ${APP_BUNDLE}" >&2
    exit 1
  fi

  # Empaqueta frameworks/plugins de Qt dentro del .app (sin firma).
  if command -v macdeployqt >/dev/null 2>&1; then
    macdeployqt "${APP_BUNDLE}" -always-overwrite
  else
    echo "Aviso: macdeployqt no encontrado; el .app puede no ser portable fuera de este equipo."
  fi

  # Garantiza que la app quede sin firma.
  /usr/bin/codesign --remove-signature "${APP_BUNDLE}" >/dev/null 2>&1 || true
  echo "App macOS creada (sin firmar): ${APP_BUNDLE}"
else
  echo "Empaquetado .app omitido (usa --bundle para generarlo)."
fi
